/*
 * A swinging ponytail for converted EX2 Plus characters (Pullum Purna), after her move code in EX2 Plus
 * (docs/EX2P_FORMAT.md 8: func_80049620 / func_80049CDC in OVL/PL04_1.OVL).
 *
 * The converter bakes the ponytail into her head part at rest (30 GT4 quads, each written front and back) and
 * writes PONY/Pxx.PNY with its shape: 7 rings of 6 points; rings 0-3 fixed in head space, rings 4-6 hang from
 * the ring above. Each frame (when her parts are drawn) the hanging rings are simulated in world space (the
 * head node's +0x48 matrix: y down, floor at 0): gravity, part of the head's movement, then put back at the
 * ring's length from the point above and kept above the floor. The result is written into her head part's
 * ponytail quads in head space, before the mirror flip (src/game/ex_mirror.c).
 *
 * File (little-endian): u16 first record (in the head part's 0x3C group), u16 quads (30), s16 fixed[24][3],
 * s16 start[18][3], u8 quad[30][4] (point numbers), s16 length[3], s16 gravity[3] (24.8).
 */
#include "recomp.h"
#include "log.h"
#include "hw.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define FIXED 24
#define MOVING 18
#define QUADS 30
#define NODE(pl, i) ((pl) + 0x1F4u + (uint32_t)(i) * 0xE8u)

typedef struct {
    int loaded;
    uint32_t model;                           /* model base the data belongs to */
    uint16_t first;
    int16_t fixed[FIXED][3], start[MOVING][3];
    uint8_t quad[QUADS][4];
    int16_t length[3], gravity[3];
} Pony;

typedef struct {
    uint32_t player, model;
    uint64_t frame;
    int init;
    float p[MOVING][3], v[MOVING][3], head[3];
    float last[FIXED + MOVING][3];            /* the last points written, in head space (for her ghost images) */
} PonyState;

static Pony g_pony[2];
static PonyState g_ps[2];

/* the model at that address was overwritten (src/game/ex_mirror.c) */
void ex_ponytail_forget(uint32_t model) {
    for (int i = 0; i < 2; i++)
        if (g_pony[i].loaded && g_pony[i].model == model) g_pony[i].loaded = 0;
    for (int i = 0; i < 2; i++)
        if (g_ps[i].model == model) { g_ps[i].model = 0; g_ps[i].player = 0; g_ps[i].init = 0; }
}

void ex_ponytail_load(uint32_t model, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    Pony *p = NULL;
    for (int i = 0; i < 2; i++) if (!g_pony[i].loaded || g_pony[i].model == model) { p = &g_pony[i]; break; }
    if (!p) p = &g_pony[0];
    memset(p, 0, sizeof *p);
    uint16_t hdr[2];
    int ok = fread(hdr, 2, 2, f) == 2 && hdr[1] == QUADS &&
             fread(p->fixed, sizeof p->fixed, 1, f) == 1 && fread(p->start, sizeof p->start, 1, f) == 1 &&
             fread(p->quad, sizeof p->quad, 1, f) == 1 && fread(p->length, sizeof p->length, 1, f) == 1 &&
             fread(p->gravity, sizeof p->gravity, 1, f) == 1;
    fclose(f);
    if (!ok) { rt_log("ponytail: %s is not valid", path); return; }
    p->first = hdr[0];
    p->model = model;
    p->loaded = 1;
    for (int i = 0; i < 2; i++) if (g_ps[i].model == model) g_ps[i].init = 0;
    rt_log("ponytail: %s for the model at %08X", path, model);
}

/* the head node's world matrix (+0x48: s16 m[3][3] in 4.12, s32 t[3] at +0x14) */
static void head_matrix(uint32_t player, float m[3][3], float t[3]) {
    uint32_t a = NODE(player, 3) + 0x48u;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) m[r][c] = (int16_t)MEM_R16(a + 2u * (uint32_t)(3 * r + c)) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)(int32_t)MEM_R32(a + 0x14u + 4u * (uint32_t)i);
}

/* head space -> world; sx = -1 when the figure is mirrored (its vertices have x negated) */
static void to_world(const float m[3][3], const float t[3], float sx, const float v[3], float w[3]) {
    float x = v[0] * sx;
    for (int r = 0; r < 3; r++) w[r] = m[r][0] * x + m[r][1] * v[1] + m[r][2] * v[2] + t[r];
}

static void to_head(const float m[3][3], const float t[3], float sx, const float w[3], float v[3]) {
    float d[3] = {w[0] - t[0], w[1] - t[1], w[2] - t[2]};
    for (int c = 0; c < 3; c++) v[c] = m[0][c] * d[0] + m[1][c] * d[1] + m[2][c] * d[2];
    v[0] *= sx;
}

/* the head part's 0x3C (GT4) group: address of its first record, or 0 */
static uint32_t gt4_group(uint32_t model) {
    static const int order[] = {0x20, 0x28, 0x30, 0x38, 0x24, 0x2C, 0x34, 0x3C};
    static const int size[] = {36, 44, 60, 80, 44, 52, 60, 76};
    uint32_t p = model + 4u * MEM_R32(model + 8), end = model + 4u * MEM_R32(model + 12);   /* E2 */
    while (p + 4 <= end) {
        int16_t count = (int16_t)MEM_R16(p), type = (int16_t)MEM_R16(p + 2);
        if (count <= 0) return 0;
        int k = -1;
        for (int i = 0; i < 8; i++) if (order[i] == type) k = i;
        if (k < 0) return 0;
        if (type == 0x3C) return p + 4;
        p += 4 + (uint32_t)count * (uint32_t)size[k];
    }
    return 0;
}

static void put_vertex(uint32_t a, const float v[3]) {
    for (int i = 0; i < 3; i++) {
        float x = v[i] < -32767 ? -32767 : v[i] > 32767 ? 32767 : v[i];
        MEM_W16(a + 2u * (uint32_t)i, (uint16_t)(int16_t)lrintf(x));
    }
}

/* Called by the draw wrapper after her parts are restored, before the mirror flip. */
void ex_ponytail_update(uint32_t player, uint32_t model, int mirrored) {
    Pony *p = NULL;
    for (int i = 0; i < 2; i++) if (g_pony[i].loaded && g_pony[i].model == model) p = &g_pony[i];
    if (!p) return;
    PonyState *s = NULL;
    for (int i = 0; i < 2; i++) if (g_ps[i].player == player) s = &g_ps[i];
    if (!s) for (int i = 0; i < 2; i++) if (!g_ps[i].player) { s = &g_ps[i]; s->player = player; s->init = 0; break; }
    if (!s) return;
    if (s->model != model) { s->model = model; s->init = 0; }
    extern int g_ghost_drawing;
    float pts[FIXED + MOVING][3];
    if (g_ghost_drawing) {                     /* a ghost image: the last shape, no simulation step */
        if (!s->init) return;
        memcpy(pts, s->last, sizeof pts);
        goto write;
    }

    float m[3][3], t[3], sx = mirrored ? -1.0f : 1.0f;
    head_matrix(player, m, t);
    float fixed[FIXED][3];
    for (int i = 0; i < FIXED; i++) {
        float v[3] = {p->fixed[i][0], p->fixed[i][1], p->fixed[i][2]};
        to_world(m, t, sx, v, fixed[i]);
    }
    float jump = (t[0] - s->head[0]) * (t[0] - s->head[0]) + (t[1] - s->head[1]) * (t[1] - s->head[1]) +
                 (t[2] - s->head[2]) * (t[2] - s->head[2]);
    if (!s->init || jump > 300.0f * 300.0f) {            /* first frame, or a jump cut: start positions */
        for (int i = 0; i < MOVING; i++) {
            float v[3] = {p->start[i][0], p->start[i][1], p->start[i][2]};
            to_world(m, t, sx, v, s->p[i]);
            s->v[i][0] = s->v[i][1] = s->v[i][2] = 0;
        }
        memcpy(s->head, t, sizeof s->head);
        s->init = 1;
        s->frame = g_frame;
    }
    if (s->frame != g_frame) {                         /* one simulation step per game frame */
        s->frame = g_frame;
        float move[3] = {t[0] - s->head[0], t[1] - s->head[1], t[2] - s->head[2]};
        memcpy(s->head, t, sizeof s->head);
        for (int ring = 0; ring < 3; ring++)
            for (int j = 0; j < 6; j++) {
                float *q = s->p[6 * ring + j], *vel = s->v[6 * ring + j];
                const float *up = ring == 0 ? fixed[18 + j] : s->p[6 * (ring - 1) + j];
                for (int i = 0; i < 3; i++) vel[i] = vel[i] * 0.75f + move[i] * 0.5f;
                for (int i = 0; i < 3; i++) q[i] += vel[i];
                q[1] += p->gravity[ring] / 256.0f;
                float d[3] = {q[0] - up[0], q[1] - up[1], q[2] - up[2]};
                float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (len < 1e-3f) { d[0] = 0; d[1] = 1; d[2] = 0; len = 1; }
                for (int i = 0; i < 3; i++) q[i] = up[i] + d[i] * (float)p->length[ring] / len;
                if (q[1] > 0) q[1] = 0;                  /* the floor */
            }
    }

    /* write the quads (front and back records) in head space */
    for (int i = 0; i < FIXED; i++) for (int c = 0; c < 3; c++) pts[i][c] = p->fixed[i][c];
    for (int i = 0; i < MOVING; i++) to_head(m, t, sx, s->p[i], pts[FIXED + i]);
    memcpy(s->last, pts, sizeof pts);
write:;
    uint32_t g = gt4_group(model);
    if (!g) return;
    for (int qd = 0; qd < QUADS; qd++) {
        const float *a = pts[p->quad[qd][0]], *b = pts[p->quad[qd][1]], *c = pts[p->quad[qd][2]],
                    *d = pts[p->quad[qd][3]];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-3f) nl = 1;
        for (int i = 0; i < 3; i++) n[i] = n[i] / nl * 4096.0f;
        float back[3] = {-n[0], -n[1], -n[2]};
        const float *front_order[4] = {a, b, c, d}, *back_order[4] = {a, c, b, d};
        uint32_t rf = g + 76u * (uint32_t)(p->first + 2 * qd), rb = rf + 76u;
        for (int k = 0; k < 4; k++) {
            put_vertex(rf + 12u + 8u * (uint32_t)k, front_order[k]);
            put_vertex(rb + 12u + 8u * (uint32_t)k, back_order[k]);
            put_vertex(rf + 0x2Cu + 8u * (uint32_t)k, n);
            put_vertex(rb + 0x2Cu + 8u * (uint32_t)k, back);
        }
    }
}
