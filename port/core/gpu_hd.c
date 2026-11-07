/* Drawing-command recorder for high-resolution renderers (see gpu_hd.h). */
#include "gpu_hd.h"
#include "gpu_raster.h"
#include "gte_precise.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "gpu.h"
#include "log.h"

#define MAX_REGIONS 4
#define MAX_PRIMS 32768

typedef struct {
    int used, x, y, w, h;
    int valid;          /* 0 after something we can't replay (upload/copy into it) until the next full clear */
    int cleared;        /* seen a full clear since creation */
    int count;
    HdPrim *prims;
    HdFrame frame;
    /* Textures are read from a snapshot of VRAM taken when the frame went on screen: the game may load new
     * textures while a finished picture stays up (e.g. the STAGE / VS screen before a fight). */
    uint16_t *snap;
    int snap_dirty;     /* new drawing since the snapshot (e.g. the game draws straight into the shown buffer) */
    /* VRAM generations (see gpu_hd.h): snapshots taken before textures in use were overwritten */
    int gen;
    uint16_t *gen_snap[HD_MAX_GEN];
    uint8_t tex_used[64 * 64 / 8];   /* VRAM read as texture/CLUT by this generation, in 16x16 blocks */
    int any_used;
} Region;

static void mark_used(Region *r, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x >> 4, y0 = y >> 4, x1 = (x + w - 1) >> 4, y1 = (y + h - 1) >> 4;
    for (int by = y0; by <= y1; by++)
        for (int bx = x0; bx <= x1; bx++) {
            int b = ((by & 63) << 6) | (bx & 63);
            r->tex_used[b >> 3] |= (uint8_t)(1 << (b & 7));
        }
    r->any_used = 1;
}

static int used_in(const Region *r, int x, int y, int w, int h) {
    if (!r->any_used || w <= 0 || h <= 0) return 0;
    int x0 = x >> 4, y0 = y >> 4, x1 = (x + w - 1) >> 4, y1 = (y + h - 1) >> 4;
    if (x1 > 63) x1 = 63;
    if (y1 > 63) y1 = 63;
    for (int by = y0; by <= y1; by++)
        for (int bx = x0; bx <= x1; bx++) {
            int b = (by << 6) | bx;
            if (r->tex_used[b >> 3] & (1 << (b & 7))) return 1;
        }
    return 0;
}

static void new_frame(Region *r) {
    r->gen = 0;
    r->any_used = 0;
    memset(r->tex_used, 0, sizeof r->tex_used);
}

/* The VRAM a textured primitive reads: the part of its texture page its UVs cover, and its CLUT. */
static void mark_texture(Region *r, const HdPrim *p, int nv) {
    if (!(p->flags & HD_TEX)) return;
    float u0 = 255, u1 = 0, v0 = 255, v1 = 0;
    for (int k = 0; k < nv; k++) {
        u0 = fminf(u0, p->v[k].u); u1 = fmaxf(u1, p->v[k].u);
        v0 = fminf(v0, p->v[k].v); v1 = fmaxf(v1, p->v[k].v);
    }
    int iu0 = (int)u0, iu1 = (int)u1 + 1, iv0 = (int)v0, iv1 = (int)v1 + 1;
    if (p->tw_mask_x || p->tw_mask_y || iu1 > 256 || iv1 > 256) { iu0 = 0; iu1 = 256; iv0 = 0; iv1 = 256; }
    int shift = p->depth == 0 ? 2 : p->depth == 1 ? 1 : 0;           /* texels per VRAM halfword: 4, 2, 1 */
    mark_used(r, p->tx + (iu0 >> shift), p->ty + iv0, ((iu1 - 1) >> shift) - (iu0 >> shift) + 1, iv1 - iv0);
    if (p->depth < 2) mark_used(r, p->cx, p->cy, p->depth == 0 ? 16 : 256, 1);
}

static Region regions[MAX_REGIONS];
static int hd_max_gen_seen;
static int disp_x, disp_y, disp_w = 512, disp_h = 240;

static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

static Region *region_at(int x, int y) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (r->used && x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return r;
    }
    return NULL;
}

void hd_set_display(int x, int y, int w, int h) {
    int switched = x != disp_x || y != disp_y;
    disp_x = x; disp_y = y; disp_w = w; disp_h = h;
    for (int i = 0; i < MAX_REGIONS; i++)
        if (regions[i].used && regions[i].x == x && regions[i].y == y) {
            regions[i].w = w; regions[i].h = h;
            if (switched) regions[i].snap_dirty = 1;     /* a finished frame just went on screen */
            return;
        }
    for (int i = 0; i < MAX_REGIONS; i++)
        if (!regions[i].used) {
            Region *r = &regions[i];
            if (!r->prims) r->prims = malloc(sizeof(HdPrim) * MAX_PRIMS);
            if (!r->snap) r->snap = malloc(1024 * 1024 * sizeof(uint16_t));
            r->used = 1; r->x = x; r->y = y; r->w = w; r->h = h;
            r->valid = 1; r->cleared = 0; r->count = 0; r->snap_dirty = 1;
            new_frame(r);
            return;
        }
}

static HdPrim *new_prim(Region *r) {
    if (r->count >= MAX_PRIMS) { r->valid = 0; return NULL; }
    if (r->x == disp_x && r->y == disp_y) r->snap_dirty = 1;   /* drawing into the buffer on screen */
    HdPrim *p = &r->prims[r->count++];
    memset(p, 0, sizeof *p);
    p->gen = (uint8_t)r->gen;
    RasterState *rs = raster_state();
    p->clip_x1 = (int16_t)rs->clip_x1; p->clip_y1 = (int16_t)rs->clip_y1;
    p->clip_x2 = (int16_t)rs->clip_x2; p->clip_y2 = (int16_t)rs->clip_y2;
    p->tw_mask_x = (uint8_t)rs->tw_mask_x; p->tw_mask_y = (uint8_t)rs->tw_mask_y;
    p->tw_off_x = (uint8_t)rs->tw_off_x; p->tw_off_y = (uint8_t)rs->tw_off_y;
    return p;
}

static void set_texture(HdPrim *p, uint32_t page, uint32_t clut) {
    p->tx = (uint16_t)((page & 0xF) * 64);
    p->ty = (uint16_t)(((page >> 4) & 1) * 256 + ((page >> 11) & 1) * 512);
    p->depth = (uint8_t)((page >> 7) & 3);
    p->semi_mode = (uint8_t)((page >> 5) & 3);
    p->cx = (uint16_t)((clut & 0x3F) * 16);
    p->cy = (uint16_t)((clut >> 6) & 0x3FF);
}

static int st_found, st_none;      /* how many vertices had an unrounded position (FGT_HD_STATS) */

static void vertex(HdVtx *v, uint32_t xy, uint32_t col, int ox, int oy) {
    float fx, fy, fz;
    int ix = sx11(xy), iy = sx11(xy >> 16);
    /* Looked up by the rounded value; two vertices on the same pixel with different depths are reported as
     * unknown by gte.c (see "Two different vertices"). Tracking the packet's memory address instead was
     * tried: exact for the ~16% of vertices the game stores straight from the GTE, but the copies it makes
     * of them then disagreed with the originals and opened seams between polygons. */
    int found = gte_precise_lookup(xy, &fx, &fy, &fz);
    if (found) st_found++; else st_none++;
    if (found && fabsf(fx - (float)ix) < 1.0f && fabsf(fy - (float)iy) < 1.0f) {
        v->x = fx + (float)ox; v->y = fy + (float)oy; v->w = fz;
    } else {
        v->x = (float)(ix + ox); v->y = (float)(iy + oy); v->w = 0;     /* 0 = no depth known */
    }
    v->r = (uint8_t)col; v->g = (uint8_t)(col >> 8); v->b = (uint8_t)(col >> 16); v->a = 255;
}

static void record_poly(Region *r, const uint32_t *w) {
    RasterState *rs = raster_state();
    uint32_t cmd = w[0] >> 24;
    int quad = (cmd & 8) != 0, tex = (cmd & 4) != 0, gour = (cmd & 0x10) != 0;
    int n = quad ? 4 : 3, i = 1;
    HdVtx v[4];
    uint32_t clut = 0, page = rs->texpage;
    for (int k = 0; k < n; k++) {
        uint32_t col = (k == 0) ? w[0] : (gour ? w[i++] : w[0]);
        vertex(&v[k], w[i++], col, rs->off_x, rs->off_y);
        if (tex) {
            uint32_t uv = w[i++];
            v[k].u = (float)(uv & 0xFF); v[k].v = (float)((uv >> 8) & 0xFF);
            if (k == 0) clut = uv >> 16;
            if (k == 1) page = uv >> 16;
        }
    }
    /* same size rule as the hardware (and the software renderer): drop huge polygons */
    for (int t = 0; t < (quad ? 2 : 1); t++) {
        HdVtx *a = &v[t], *b = &v[t + 1], *c = &v[t + 2];
        float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
        float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
        if (maxx - minx > 1023 || maxy - miny > 511) continue;
        HdPrim *p = new_prim(r);
        if (!p) return;
        p->kind = HD_TRI;
        p->flags = (uint16_t)((tex ? HD_TEX : 0) | ((tex && (cmd & 1)) ? HD_RAW : 0) | ((cmd & 2) ? HD_SEMI : 0) |
                              (gour ? HD_GOURAUD : 0));
        set_texture(p, tex ? page : rs->texpage, clut);
        p->v[0] = *a; p->v[1] = *b; p->v[2] = *c;
        mark_texture(r, p, 3);
        /* perspective only when all three corners came from the GTE */
        if (!(a->w > 0 && b->w > 0 && c->w > 0)) p->v[0].w = p->v[1].w = p->v[2].w = 1;
    }
}

static void record_rect(Region *r, const uint32_t *w) {
    RasterState *rs = raster_state();
    uint32_t cmd = w[0] >> 24;
    int i = 1, tex = (cmd & 4) != 0;
    uint32_t xy = w[i++];
    int x0 = sx11(xy) + rs->off_x, y0 = sx11(xy >> 16) + rs->off_y;
    int u0 = 0, v0 = 0;
    uint32_t clut = 0;
    if (tex) { uint32_t uv = w[i++]; u0 = (int)(uv & 0xFF); v0 = (int)((uv >> 8) & 0xFF); clut = uv >> 16; }
    int ww, hh;
    switch ((cmd >> 3) & 3) {
    case 0: ww = (int)(w[i] & 0x3FF); hh = (int)((w[i] >> 16) & 0x1FF); break;
    case 1: ww = hh = 1; break;
    case 2: ww = hh = 8; break;
    default: ww = hh = 16; break;
    }
    HdPrim *p = new_prim(r);
    if (!p) return;
    p->kind = HD_RECT;
    p->flags = (uint16_t)((tex ? HD_TEX : 0) | ((tex && (cmd & 1)) ? HD_RAW : 0) | ((cmd & 2) ? HD_SEMI : 0));
    set_texture(p, rs->texpage, clut);
    for (int k = 0; k < 2; k++) {
        HdVtx *v = &p->v[k];
        v->x = (float)(x0 + (k ? ww : 0)); v->y = (float)(y0 + (k ? hh : 0)); v->w = 1;
        v->u = (float)(u0 + (k ? ww : 0)); v->v = (float)(v0 + (k ? hh : 0));
        v->r = (uint8_t)w[0]; v->g = (uint8_t)(w[0] >> 8); v->b = (uint8_t)(w[0] >> 16); v->a = 255;
    }
    if (tex) {      /* the corner UVs are exclusive: mark up to the last texel actually used */
        p->v[1].u -= 1; p->v[1].v -= 1;
        mark_texture(r, p, 2);
        p->v[1].u += 1; p->v[1].v += 1;
    }
}

static void record_line(Region *r, const uint32_t *w, int nwords) {
    RasterState *rs = raster_state();
    uint32_t cmd = w[0] >> 24;
    int gour = (cmd & 0x10) != 0, poly = (cmd & 8) != 0, i = 1;
    uint32_t c0 = w[0], xy0 = w[i++];
    for (;;) {
        uint32_t c1 = c0;
        if (gour) { if (i >= nwords) break; c1 = w[i++]; }
        if (i >= nwords) break;
        uint32_t xy1 = w[i++];
        if (poly && (xy1 & 0xF000F000u) == 0x50005000u) break;
        HdPrim *p = new_prim(r);
        if (!p) return;
        p->kind = HD_LINE;
        p->flags = (uint16_t)(((cmd & 2) ? HD_SEMI : 0) | (gour ? HD_GOURAUD : 0));
        set_texture(p, rs->texpage, 0);
        vertex(&p->v[0], xy0, c0, rs->off_x, rs->off_y);
        vertex(&p->v[1], xy1, c1, rs->off_x, rs->off_y);
        p->v[0].w = p->v[1].w = 1;
        if (!poly) break;
        xy0 = xy1; c0 = c1;
        if (i < nwords && (w[i] & 0xF000F000u) == 0x50005000u) break;
    }
}

void hd_record(const uint32_t *w, int nwords) {
    RasterState *rs = raster_state();
    Region *r = region_at(rs->clip_x1, rs->clip_y1);
    if (!r || !r->valid) return;
    uint32_t op = w[0] >> 24;
    if (op >= 0x20 && op < 0x40) record_poly(r, w);
    else if (op >= 0x40 && op < 0x60) record_line(r, w, nwords);
    else if (op >= 0x60 && op < 0x80) record_rect(r, w);
}

void hd_fill(int x, int y, int w, int h, uint32_t rgb) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (!r->used) continue;
        if (x <= r->x && y <= r->y && x + w >= r->x + r->w && y + h >= r->y + r->h) {
            r->count = 0;           /* a full clear starts a new frame in this buffer */
            new_frame(r);
            r->valid = 1;
            r->cleared = 1;
        }
    }
    Region *r = region_at(x, y);
    if (!r || !r->valid) return;
    HdPrim *p = new_prim(r);
    if (!p) return;
    p->kind = HD_FILL;
    p->v[0].x = (float)x; p->v[0].y = (float)y;
    p->v[1].x = (float)(x + w); p->v[1].y = (float)(y + h);
    p->v[0].r = (uint8_t)rgb; p->v[0].g = (uint8_t)(rgb >> 8); p->v[0].b = (uint8_t)(rgb >> 16); p->v[0].a = 255;
}

/* Called just BEFORE an upload or copy changes VRAM. */
void hd_vram_written(int x, int y, int w, int h) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (!r->used) continue;
        if (x < r->x + r->w && x + w > r->x && y < r->y + r->h && y + h > r->y) { r->valid = 0; continue; }
        /* textures this frame already drew with are about to change: keep the old VRAM for those primitives */
        int hit = 0;
        for (int dy = 0; dy < h && !hit; dy += 1024) for (int dx = 0; dx < w && !hit; dx += 1024)
            hit = used_in(r, (x + dx) & 1023, (y + dy) & 1023, w - dx > 1024 ? 1024 : w - dx, h - dy > 1024 ? 1024 : h - dy);
        if (!hit) {     /* a write that wraps past the right/bottom edge of VRAM */
            if (x + w > 1024) hit = used_in(r, 0, y, x + w - 1024, h);
            if (!hit && y + h > 1024) hit = used_in(r, x, 0, w, y + h - 1024);
        }
        if (!hit || !r->valid) continue;
        if (r->gen >= HD_MAX_GEN - 1) { r->valid = 0; continue; }   /* too many changes: software picture */
        if (!r->gen_snap[r->gen]) r->gen_snap[r->gen] = malloc(1024 * 1024 * sizeof(uint16_t));
        if (!r->gen_snap[r->gen]) { r->valid = 0; continue; }
        memcpy(r->gen_snap[r->gen], g_vram, 1024 * 1024 * sizeof(uint16_t));
        r->gen++;
        if (r->gen > hd_max_gen_seen) {
            hd_max_gen_seen = r->gen;
            rt_log("hd: textures replaced while a frame was drawn (VRAM %d,%d %dx%d); %d snapshot(s) in one frame",
                   x, y, w, h, r->gen);
        }
        r->any_used = 0;
        memset(r->tex_used, 0, sizeof r->tex_used);
        r->snap_dirty = 1;
    }
}

const HdFrame *gpu_hd_frame(void) {
    for (int i = 0; i < MAX_REGIONS; i++) {
        Region *r = &regions[i];
        if (r->used && r->x == disp_x && r->y == disp_y) {
            if (!r->valid || !r->cleared) return NULL;
            r->frame.x = r->x; r->frame.y = r->y; r->frame.w = disp_w; r->frame.h = disp_h;
            r->frame.count = r->count;
            r->frame.prims = r->prims;
            if (r->snap_dirty) { memcpy(r->snap, g_vram, 1024 * 1024 * sizeof(uint16_t)); r->snap_dirty = 0; }
            for (int g = 0; g < r->gen; g++) r->frame.vram[g] = r->gen_snap[g];
            r->frame.vram[r->gen] = r->snap;
            r->frame.ngen = r->gen + 1;
            {   /* FGT_HD_STATS: how vertices were matched to their unrounded positions, every 5 seconds */
                static int on = -1, n;
                if (on < 0) on = getenv("FGT_HD_STATS") != NULL;
                if (on && ++n % 300 == 0) {
                    rt_log("hd: vertices with an unrounded position %d, rounded %d", st_found, st_none);
                    st_found = st_none = 0;
                }
            }
            {   /* DEBUG: FGT_HD_DUMP=<frame> writes the frame's primitives to hd_dump.txt */
                extern uint64_t g_frame;
                static long dumpf = -2;
                if (dumpf == -2) { const char *e = getenv("FGT_HD_DUMP"); dumpf = e ? atol(e) : -1; }
                if (dumpf >= 0 && (long)g_frame == dumpf) {
                    FILE *f = fopen("hd_dump.txt", "w");
                    for (int k = 0; f && k < r->count; k++) {
                        const HdPrim *p = &r->prims[k];
                        fprintf(f, "%d kind=%d flags=%d semi=%d depth=%d gen=%d tx=%d ty=%d cx=%d cy=%d", k, p->kind, p->flags, p->semi_mode, p->depth, p->gen, p->tx, p->ty, p->cx, p->cy);
                        for (int q = 0; q < 3; q++) fprintf(f, " | %.2f %.2f w=%.1f uv=%.0f,%.0f", p->v[q].x - r->x, p->v[q].y - r->y, p->v[q].w, p->v[q].u, p->v[q].v);
                        fputc(10, f);
                    }
                    if (f) fclose(f);
                }
            }
            return &r->frame;
        }
    }
    return NULL;
}
