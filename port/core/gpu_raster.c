/*
 * Software rasterizer for PS1 GPU drawing commands (polygons, rectangles, lines) into g_vram.
 * Written for clarity first; accuracy details (dithering, mask bit, exact edge rules) can be refined.
 */
#include "gpu.h"
#include "gpu_raster.h"
#include <stdlib.h>

#define VRAM_W 1024
#define VRAM_H 1024

static RasterState rs = {0, 0, 1023, 1023, 0, 0, 0, 0, 0, 0, 0, 0, 0};

RasterState *raster_state(void) { return &rs; }

static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

/* ------------------------------------------------------------------ texels and pixels */
typedef struct { int tx, ty, depth, cx, cy, semi; } TexCtx;

static inline uint16_t tex_fetch(const TexCtx *t, int u, int v) {
    u &= 0xFF; v &= 0xFF;
    u = (u & ~(rs.tw_mask_x * 8)) | ((rs.tw_off_x & rs.tw_mask_x) * 8);
    v = (v & ~(rs.tw_mask_y * 8)) | ((rs.tw_off_y & rs.tw_mask_y) * 8);
    int row = ((t->ty + v) & (VRAM_H - 1)) * VRAM_W;
    switch (t->depth) {
    case 0: {
        uint16_t w = g_vram[row + ((t->tx + (u >> 2)) & (VRAM_W - 1))];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return g_vram[(t->cy & (VRAM_H - 1)) * VRAM_W + ((t->cx + idx) & (VRAM_W - 1))];
    }
    case 1: {
        uint16_t w = g_vram[row + ((t->tx + (u >> 1)) & (VRAM_W - 1))];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return g_vram[(t->cy & (VRAM_H - 1)) * VRAM_W + ((t->cx + idx) & (VRAM_W - 1))];
    }
    default:
        return g_vram[row + ((t->tx + u) & (VRAM_W - 1))];
    }
}

static inline uint16_t blend(uint16_t b, uint16_t f, int mode) {
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int fr = f & 31, fg = (f >> 5) & 31, fb = (f >> 10) & 31;
    int r, g, bl;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; bl = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bg + fg; bl = bb + fb; break;
    case 2: r = br - fr; g = bg - fg; bl = bb - fb; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); bl = bb + (fb >> 2); break;
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    bl = bl < 0 ? 0 : bl > 31 ? 31 : bl;
    return (uint16_t)(r | (g << 5) | (bl << 10) | (f & 0x8000));
}

/* Shade one pixel. cmd bits: 0x04 textured, 0x02 semi-transparent, 0x01 raw texture (no modulation). */
static inline void plot(int x, int y, int r, int g, int b, int u, int v, uint32_t cmd, const TexCtx *t) {
    if (x < rs.clip_x1 || x > rs.clip_x2 || y < rs.clip_y1 || y > rs.clip_y2) return;
    uint16_t *dst = &g_vram[(y & (VRAM_H - 1)) * VRAM_W + (x & (VRAM_W - 1))];
    if (rs.mask_check && (*dst & 0x8000)) return;
    uint16_t px;
    int semi = (cmd & 2) != 0;
    if (cmd & 4) {
        uint16_t tx = tex_fetch(t, u, v);
        if (tx == 0) return;
        if (!(tx & 0x8000)) semi = 0;
        if (cmd & 1) px = tx;
        else {
            int cr = ((tx & 31) * r) >> 7, cg = (((tx >> 5) & 31) * g) >> 7, cb = (((tx >> 10) & 31) * b) >> 7;
            px = (uint16_t)((cr > 31 ? 31 : cr) | ((cg > 31 ? 31 : cg) << 5) | ((cb > 31 ? 31 : cb) << 10) | (tx & 0x8000));
        }
    } else {
        px = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }
    if (semi) px = blend(*dst, px, t->semi);
    if (rs.mask_set) px |= 0x8000;
    *dst = px;
}

static void tex_from_page(TexCtx *t, uint32_t page, uint32_t clut) {
    t->tx = (int)(page & 0xF) * 64;
    t->ty = (int)((page >> 4) & 1) * 256 + (int)((page >> 11) & 1) * 512;   /* bit 11: 2 MB VRAM */
    t->semi = (int)((page >> 5) & 3);
    t->depth = (int)((page >> 7) & 3);
    t->cx = (int)(clut & 0x3F) * 16;
    t->cy = (int)((clut >> 6) & 0x3FF);
}

/* ------------------------------------------------------------------ triangles */
typedef struct { int x, y, r, g, b, u, v; } Vtx;

/* Edge function: > 0 when p is on the inner side of edge P->Q for a triangle with positive area. */
static inline int64_t edge(const Vtx *P, const Vtx *Q, int x, int y) {
    return (int64_t)(Q->x - P->x) * (y - P->y) - (int64_t)(Q->y - P->y) * (x - P->x);
}

/* Top-left fill rule (as on the PS1 GPU): pixels exactly on an edge belong to the triangle only if it is a
 * top edge (horizontal, triangle below) or a left edge. Shared edges are then drawn exactly once. */
static inline int owns_edge(const Vtx *P, const Vtx *Q) {
    int dx = Q->x - P->x, dy = Q->y - P->y;
    return dy < 0 || (dy == 0 && dx > 0);
}

/* floor(n / d) for d > 0 */
static inline int floordiv(int64_t n, int64_t d) { return (int)(n >= 0 ? n / d : -((-n + d - 1) / d)); }

/* An attribute interpolated exactly as floor(N / area), where N is a linear function of x.
 * Stepping keeps quotient and remainder separately, so no division is needed inside a row. */
typedef struct { int q; int64_t r, qstep_r, area; int qstep; } Lerp;

static inline void lerp_row(Lerp *L, int64_t n_at_x0, int64_t dn_dx, int64_t area) {
    L->area = area;
    L->q = floordiv(n_at_x0, area);
    L->r = n_at_x0 - (int64_t)L->q * area;
    L->qstep = floordiv(dn_dx, area);
    L->qstep_r = dn_dx - (int64_t)L->qstep * area;    /* 0 <= qstep_r < area */
}
static inline void lerp_step(Lerp *L) {
    L->q += L->qstep;
    L->r += L->qstep_r;
    if (L->r >= L->area) { L->r -= L->area; L->q++; }
}

static void tri(const Vtx *a, const Vtx *b, const Vtx *c, uint32_t cmd, const TexCtx *t, int gouraud) {
    int64_t area = edge(a, b, c->x, c->y);
    if (area == 0) return;
    if (area < 0) { const Vtx *tmp = b; b = c; c = tmp; area = -area; }   /* make the winding positive */
    /* PS1 drops polygons wider than 1023 or taller than 511 */
    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    if (b->x < minx) minx = b->x; if (b->x > maxx) maxx = b->x;
    if (c->x < minx) minx = c->x; if (c->x > maxx) maxx = c->x;
    if (b->y < miny) miny = b->y; if (b->y > maxy) maxy = b->y;
    if (c->y < miny) miny = c->y; if (c->y > maxy) maxy = c->y;
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < rs.clip_x1) minx = rs.clip_x1;
    if (miny < rs.clip_y1) miny = rs.clip_y1;
    if (maxx > rs.clip_x2) maxx = rs.clip_x2;
    if (maxy > rs.clip_y2) maxy = rs.clip_y2;
    if (minx > maxx || miny > maxy) return;
    /* bias makes "on an edge we don't own" fail the >= 0 test */
    int64_t bias0 = owns_edge(b, c) ? 0 : -1, bias1 = owns_edge(c, a) ? 0 : -1, bias2 = owns_edge(a, b) ? 0 : -1;
    /* per-pixel x steps of the edge functions */
    int64_t s0 = -(int64_t)(c->y - b->y), s1 = -(int64_t)(a->y - c->y), s2 = -(int64_t)(b->y - a->y);
    int tex = (cmd & 4) != 0;
    int64_t du = s0 * a->u + s1 * b->u + s2 * c->u, dv = s0 * a->v + s1 * b->v + s2 * c->v;
    int64_t dr = s0 * a->r + s1 * b->r + s2 * c->r, dg = s0 * a->g + s1 * b->g + s2 * c->g;
    int64_t db = s0 * a->b + s1 * b->b + s2 * c->b;
    for (int y = miny; y <= maxy; y++) {
        int64_t w0 = edge(b, c, minx, y), w1 = edge(c, a, minx, y), w2 = edge(a, b, minx, y);
        /* skip ahead to the first pixel inside this row (cheap: just additions) */
        int x = minx;
        while (x <= maxx && (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0)) {
            w0 += s0; w1 += s1; w2 += s2; x++;
        }
        if (x > maxx) continue;
        Lerp U, V, R, G, B;
        if (tex) {
            lerp_row(&U, w0 * a->u + w1 * b->u + w2 * c->u, du, area);
            lerp_row(&V, w0 * a->v + w1 * b->v + w2 * c->v, dv, area);
        }
        if (gouraud) {
            lerp_row(&R, w0 * a->r + w1 * b->r + w2 * c->r, dr, area);
            lerp_row(&G, w0 * a->g + w1 * b->g + w2 * c->g, dg, area);
            lerp_row(&B, w0 * a->b + w1 * b->b + w2 * c->b, db, area);
        }
        /* the inside of a triangle row is one contiguous span */
        for (; x <= maxx; x++) {
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) break;
            plot(x, y, gouraud ? R.q : a->r, gouraud ? G.q : a->g, gouraud ? B.q : a->b,
                 tex ? U.q : 0, tex ? V.q : 0, cmd, t);
            w0 += s0; w1 += s1; w2 += s2;
            if (tex) { lerp_step(&U); lerp_step(&V); }
            if (gouraud) { lerp_step(&R); lerp_step(&G); lerp_step(&B); }
        }
    }
}

void raster_poly(const uint32_t *w) {
    uint32_t cmd = w[0] >> 24;
    int quad = (cmd & 8) != 0, tex = (cmd & 4) != 0, gour = (cmd & 0x10) != 0;
    int n = quad ? 4 : 3, i = 1;
    Vtx v[4];
    uint32_t clut = 0, page = rs.texpage;
    for (int k = 0; k < n; k++) {
        uint32_t col = (k == 0) ? w[0] : (gour ? w[i++] : w[0]);
        uint32_t xy = w[i++];
        v[k].r = (int)(col & 0xFF); v[k].g = (int)((col >> 8) & 0xFF); v[k].b = (int)((col >> 16) & 0xFF);
        v[k].x = sx11(xy) + rs.off_x;
        v[k].y = sx11(xy >> 16) + rs.off_y;
        v[k].u = v[k].v = 0;
        if (tex) {
            uint32_t uv = w[i++];
            v[k].u = (int)(uv & 0xFF); v[k].v = (int)((uv >> 8) & 0xFF);
            if (k == 0) clut = uv >> 16;
            if (k == 1) page = uv >> 16;
        }
    }
    TexCtx t;
    if (tex) {
        rs.texpage = (rs.texpage & ~0x9FFu) | (page & 0x9FFu);
        tex_from_page(&t, page, clut);
    } else {
        tex_from_page(&t, rs.texpage, 0);
    }
    if (!tex) cmd &= ~1u;
    tri(&v[0], &v[1], &v[2], cmd, &t, gour);
    if (quad) tri(&v[1], &v[2], &v[3], cmd, &t, gour);
}

/* ------------------------------------------------------------------ rectangles (sprites) */
void raster_rect(const uint32_t *w) {
    uint32_t cmd = w[0] >> 24;
    int i = 1, tex = (cmd & 4) != 0;
    uint32_t xy = w[i++];
    int x0 = sx11(xy) + rs.off_x, y0 = sx11(xy >> 16) + rs.off_y;
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
    TexCtx t;
    tex_from_page(&t, rs.texpage, clut);
    int r = (int)(w[0] & 0xFF), g = (int)((w[0] >> 8) & 0xFF), b = (int)((w[0] >> 16) & 0xFF);
    if (!tex) cmd &= ~1u;
    int fx = rs.flip_x ? -1 : 1, fy = rs.flip_y ? -1 : 1;
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < ww; x++)
            plot(x0 + x, y0 + y, r, g, b, u0 + x * fx, v0 + y * fy, cmd, &t);
}

/* ------------------------------------------------------------------ lines */
static void line(int x0, int y0, int x1, int y1, uint32_t c0, uint32_t c1, uint32_t cmd, int gour) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    if (dx > 1023 || dy > 511) return;
    TexCtx t;
    tex_from_page(&t, rs.texpage, 0);
    for (int k = 0; k <= n; k++) {
        int x = n ? x0 + (x1 - x0) * k / n : x0, y = n ? y0 + (y1 - y0) * k / n : y0;
        int r = (int)(c0 & 0xFF), g = (int)((c0 >> 8) & 0xFF), b = (int)((c0 >> 16) & 0xFF);
        if (gour && n) {
            r += ((int)(c1 & 0xFF) - r) * k / n;
            g += ((int)((c1 >> 8) & 0xFF) - g) * k / n;
            b += ((int)((c1 >> 16) & 0xFF) - b) * k / n;
        }
        plot(x, y, r, g, b, 0, 0, cmd & ~5u, &t);
    }
}

void raster_line(const uint32_t *w, int nwords) {
    uint32_t cmd = w[0] >> 24;
    int gour = (cmd & 0x10) != 0, poly = (cmd & 8) != 0;
    int i = 1;
    uint32_t c0 = w[0];
    uint32_t xy0 = w[i++];
    for (;;) {
        uint32_t c1 = c0;
        if (gour) { if (i >= nwords) break; c1 = w[i++]; }
        if (i >= nwords) break;
        uint32_t xy1 = w[i++];
        if (poly && (xy1 & 0xF000F000u) == 0x50005000u) break;
        line(sx11(xy0) + rs.off_x, sx11(xy0 >> 16) + rs.off_y, sx11(xy1) + rs.off_x, sx11(xy1 >> 16) + rs.off_y, c0, c1, cmd, gour);
        if (!poly) break;
        xy0 = xy1; c0 = c1;
        if (i < nwords && (w[i] & 0xF000F000u) == 0x50005000u) break;
    }
}

/* ------------------------------------------------------------------ environment commands E1-E6 */
void raster_env(uint32_t v) {
    switch (v >> 24) {
    case 0xE1: rs.texpage = v & 0x3FFF; rs.flip_x = (v >> 12) & 1; rs.flip_y = (v >> 13) & 1; break;
    case 0xE2: rs.tw_mask_x = v & 31; rs.tw_mask_y = (v >> 5) & 31; rs.tw_off_x = (v >> 10) & 31; rs.tw_off_y = (v >> 15) & 31; break;
    case 0xE3: rs.clip_x1 = (int)(v & 0x3FF); rs.clip_y1 = (int)((v >> 10) & 0x3FF); break;
    case 0xE4: rs.clip_x2 = (int)(v & 0x3FF); rs.clip_y2 = (int)((v >> 10) & 0x3FF); break;
    case 0xE5: rs.off_x = sx11(v); rs.off_y = sx11(v >> 11); break;
    case 0xE6: rs.mask_set = v & 1; rs.mask_check = (v >> 1) & 1; break;
    }
}
