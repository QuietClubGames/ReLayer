/*
 * GTE (Geometry Transformation Engine, PS1 coprocessor 2) in plain C.
 * Follows the psx-spx documentation: 16.16/4.12 fixed point, saturation flags, UNR division.
 */
#include "recomp.h"
#include "log.h"

typedef struct { int16_t x, y, z; } V3;

static int16_t VX[3], VY[3], VZ[3];
static uint32_t RGBC, OTZ, RES1, LZCS;
static int32_t IR0, IR1, IR2, IR3;
static int32_t SX[3], SY[3];
static uint32_t SZ[4];
static uint32_t RGBF[3];
static int32_t MAC0, MAC1, MAC2, MAC3;

static int16_t RT[3][3], LL[3][3], LC[3][3];
static int32_t TR[3], BK[3], FC[3];
static int32_t OFX, OFY, DQB;
static uint16_t H;
static int16_t DQA, ZSF3, ZSF4;
static uint32_t FLAG;

/* ------------------------------------------------------------------ helpers */
static inline int32_t s16(uint32_t v) { return (int16_t)(v & 0xFFFF); }

static void flag_finish(void) {
    FLAG &= 0x7FFFF000u;
    if (FLAG & 0x7F87E000u) FLAG |= 0x80000000u;
}

/* MAC1-3 are 44-bit; check overflow and return the (shifted) value */
static int64_t mac_chk(int i, int64_t v) {
    static const uint32_t pos[3] = {1u << 30, 1u << 29, 1u << 28}, neg[3] = {1u << 27, 1u << 26, 1u << 25};
    if (v > 0x7FFFFFFFFFFLL) FLAG |= pos[i];
    else if (v < -0x80000000000LL) FLAG |= neg[i];
    return v;
}

static int32_t sat_ir(int i, int64_t v, int lm) {
    static const uint32_t f[3] = {1u << 24, 1u << 23, 1u << 22};
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { FLAG |= f[i]; return lo; }
    if (v > 0x7FFF) { FLAG |= f[i]; return 0x7FFF; }
    return (int32_t)v;
}

static void set_mac_ir(int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    int sh = sf ? 12 : 0;
    MAC1 = (int32_t)(mac_chk(0, m1) >> sh);
    MAC2 = (int32_t)(mac_chk(1, m2) >> sh);
    MAC3 = (int32_t)(mac_chk(2, m3) >> sh);
    IR1 = sat_ir(0, MAC1, lm);
    IR2 = sat_ir(1, MAC2, lm);
    IR3 = sat_ir(2, MAC3, lm);
}

static int32_t mac0_chk(int64_t v) {
    if (v > 0x7FFFFFFFLL) FLAG |= 1u << 16;
    else if (v < -0x80000000LL) FLAG |= 1u << 15;
    return (int32_t)v;
}

static uint8_t sat_col(int i, int32_t v) {
    static const uint32_t f[3] = {1u << 21, 1u << 20, 1u << 19};
    if (v < 0) { FLAG |= f[i]; return 0; }
    if (v > 255) { FLAG |= f[i]; return 255; }
    return (uint8_t)v;
}

static void push_color(void) {
    RGBF[0] = RGBF[1];
    RGBF[1] = RGBF[2];
    RGBF[2] = (uint32_t)sat_col(0, MAC1 >> 4) | ((uint32_t)sat_col(1, MAC2 >> 4) << 8) |
              ((uint32_t)sat_col(2, MAC3 >> 4) << 16) | (RGBC & 0xFF000000u);
}

static void push_sz(int64_t v) {
    SZ[0] = SZ[1]; SZ[1] = SZ[2]; SZ[2] = SZ[3];
    if (v < 0) { FLAG |= 1u << 18; v = 0; }
    else if (v > 0xFFFF) { FLAG |= 1u << 18; v = 0xFFFF; }
    SZ[3] = (uint32_t)v;
}

static void push_sxy(int64_t x, int64_t y) {
    SX[0] = SX[1]; SY[0] = SY[1]; SX[1] = SX[2]; SY[1] = SY[2];
    if (x < -0x400) { FLAG |= 1u << 14; x = -0x400; } else if (x > 0x3FF) { FLAG |= 1u << 14; x = 0x3FF; }
    if (y < -0x400) { FLAG |= 1u << 13; y = -0x400; } else if (y > 0x3FF) { FLAG |= 1u << 13; y = 0x3FF; }
    SX[2] = (int32_t)x; SY[2] = (int32_t)y;
}

/* UNR (unsigned Newton-Raphson) division used by RTPS/RTPT */
static uint8_t unr_table[0x101];
static int unr_ready;
static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (!unr_ready) {
        for (int i = 0; i <= 0x100; i++) {
            int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
            unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
        }
        unr_ready = 1;
    }
    if (h >= sz3 * 2u) { FLAG |= 1u << 17; return 0x1FFFF; }
    int z = 0;
    while (z < 16 && !(sz3 & (0x8000u >> z))) z++;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = sz3 << z;
    uint32_t u = (uint32_t)unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (uint32_t)((0x2000080 - (int64_t)d * u) >> 8);
    d = (uint32_t)((0x0000080 + (int64_t)d * u) >> 8);
    uint64_t r = (n * d + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}

/* ------------------------------------------------------------------ precise screen positions
 * The GTE rounds projected vertices to whole pixels, which is what makes PS1 polygons wobble. For the
 * high-resolution renderer we remember the unrounded result, keyed by the rounded value the game writes
 * into its drawing packets, so the GPU recorder can look it up. */
#include "gte_precise.h"
#include <math.h>
#include "hw.h"
#define PRECISE_BUCKETS 16384    /* 4 entries each: 64K vertices, far more than a frame projects */
#define PRECISE_WAYS 4
#define PRECISE_AGE 2           /* a lookup may use a vertex projected up to this many frames earlier */
typedef struct { uint32_t key, frame, amb_frame; float x, y, z; } Precise;
static Precise g_precise[PRECISE_BUCKETS][PRECISE_WAYS];
static uint32_t g_bucket_evicted[PRECISE_BUCKETS];   /* frame a still-usable entry was pushed out */

/* Two different vertices often round to the same pixel (a fighter's hand in front of a background corner).
 * Their depths differ, and handing one's depth to the other's polygon bends its texture: the flickering
 * patches seen around the fighters on the ring stage. The game draws a frame a little after projecting it,
 * so a lookup may reach back a frame, and the clash can be between this frame's vertex and the previous
 * one's. So: a pixel that two clearly different positions/depths projected to within the lookup window is
 * ambiguous for that window, and polygons using it fall back to the rounded position. For that to be
 * reliable an entry must not be pushed out while it can still be looked up (a third vertex evicting the
 * first one would hide the clash), hence 4-way buckets, and a bucket that had to evict a live entry answers
 * "don't know" for the lookup window. */
static int precise_differs(const Precise *e, float x, float y, float z) {
    float dz = fabsf(e->z - z), tol = fmaxf(8.0f, 0.02f * fmaxf(e->z, z));
    return dz > tol || fabsf(e->x - x) > 0.25f || fabsf(e->y - y) > 0.25f;
}

static inline uint32_t precise_bucket(uint32_t key) { return (key * 2654435761u) >> 18; }   /* 14 bits */

static void precise_store(uint32_t key, float x, float y, float z) {
    uint32_t bi = precise_bucket(key), now = (uint32_t)g_frame + 1;   /* +1 so 0 means empty */
    Precise *bk = g_precise[bi], *e = NULL;
    for (int w = 0; w < PRECISE_WAYS; w++)
        if (bk[w].frame && bk[w].key == key && now - bk[w].frame <= PRECISE_AGE) { e = &bk[w]; break; }
    if (e) {
        if (precise_differs(e, x, y, z)) e->amb_frame = now;
        if (e->frame == now) return;            /* same vertex projected again (shared corners) */
    } else {
        /* a free or expired way, else the oldest (and remember that something usable was lost) */
        int best = 0;
        for (int w = 0; w < PRECISE_WAYS; w++) {
            if (!bk[w].frame || now - bk[w].frame > PRECISE_AGE) { best = w; goto take; }
            if (bk[w].frame < bk[best].frame) best = w;
        }
        g_bucket_evicted[bi] = now;
    take:
        e = &bk[best];
        e->key = key;
        e->amb_frame = 0;
    }
    e->frame = now;
    e->x = x; e->y = y; e->z = z;
}

int gte_precise_lookup(uint32_t key, float *x, float *y, float *z) {
    uint32_t bi = precise_bucket(key), now = (uint32_t)g_frame + 1;
    if (g_bucket_evicted[bi] && now - g_bucket_evicted[bi] <= PRECISE_AGE) return 0;
    for (int w = 0; w < PRECISE_WAYS; w++) {
        const Precise *e = &g_precise[bi][w];
        if (!e->frame || e->key != key) continue;
        if (now - e->frame > PRECISE_AGE) return 0;                         /* stale */
        if (e->amb_frame && now - e->amb_frame <= PRECISE_AGE) return 0;    /* ambiguous */
        *x = e->x; *y = e->y; *z = e->z;
        return 1;
    }
    return 0;
}

int g_gte_nclip_invert;                 /* see NCLIP */

/* ------------------------------------------------------------------ operations */
static void rtp(int v, int sf, int lm, int last) {
    int64_t x = VX[v], y = VY[v], z = VZ[v];
    int64_t m1 = ((int64_t)TR[0] << 12) + RT[0][0] * x + RT[0][1] * y + RT[0][2] * z;
    int64_t m2 = ((int64_t)TR[1] << 12) + RT[1][0] * x + RT[1][1] * y + RT[1][2] * z;
    int64_t m3 = ((int64_t)TR[2] << 12) + RT[2][0] * x + RT[2][1] * y + RT[2][2] * z;
    int sh = sf ? 12 : 0;
    MAC1 = (int32_t)(mac_chk(0, m1) >> sh);
    MAC2 = (int32_t)(mac_chk(1, m2) >> sh);
    MAC3 = (int32_t)(mac_chk(2, m3) >> sh);
    IR1 = sat_ir(0, MAC1, lm);
    IR2 = sat_ir(1, MAC2, lm);
    /* IR3 quirk: the flag tests MAC3>>12 when sf=0, but IR3 saturates from MAC3 */
    {
        int64_t t = sf ? MAC3 : (m3 >> 12);
        if (t < -0x8000 || t > 0x7FFF) FLAG |= 1u << 22;
        int32_t lo = lm ? 0 : -0x8000;
        IR3 = MAC3 < lo ? lo : MAC3 > 0x7FFF ? 0x7FFF : MAC3;
    }
    push_sz(m3 >> 12);
    uint32_t div = gte_divide(H, SZ[3]);
    int64_t sx = mac0_chk((int64_t)div * IR1 + OFX);
    int64_t sy = mac0_chk((int64_t)div * IR2 + OFY);
    push_sxy(sx >> 16, sy >> 16);
    if (sx >> 16 == SX[2] && sy >> 16 == SY[2] && SZ[3])          /* not clamped */
        precise_store(((uint32_t)SX[2] & 0xFFFF) | ((uint32_t)SY[2] << 16),
                      (float)((double)sx / 65536.0), (float)((double)sy / 65536.0), (float)SZ[3]);
    if (last) {
        int64_t m0 = mac0_chk((int64_t)div * DQA + DQB);
        MAC0 = (int32_t)m0;
        int64_t i0 = m0 >> 12;
        IR0 = i0 < 0 ? (FLAG |= 1u << 12, 0) : i0 > 0x1000 ? (FLAG |= 1u << 12, 0x1000) : (int32_t)i0;
    }
}

static void mat_vec(int16_t m[3][3], int64_t vx, int64_t vy, int64_t vz, int64_t t0, int64_t t1, int64_t t2,
                    int sf, int lm) {
    set_mac_ir((t0 << 12) + m[0][0] * vx + m[0][1] * vy + m[0][2] * vz,
               (t1 << 12) + m[1][0] * vx + m[1][1] * vy + m[1][2] * vz,
               (t2 << 12) + m[2][0] * vx + m[2][1] * vy + m[2][2] * vz, sf, lm);
}

/* depth-cue interpolation of MAC towards the far colour by IR0, then shift, push colour */
static void depth_cue_finish(int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    int sh = sf ? 12 : 0;
    int32_t i1 = sat_ir(0, (((int64_t)FC[0] << 12) - m1) >> sh, 0);
    int32_t i2 = sat_ir(1, (((int64_t)FC[1] << 12) - m2) >> sh, 0);
    int32_t i3 = sat_ir(2, (((int64_t)FC[2] << 12) - m3) >> sh, 0);
    set_mac_ir((int64_t)i1 * IR0 + m1, (int64_t)i2 * IR0 + m2, (int64_t)i3 * IR0 + m3, sf, lm);
    push_color();
}

static void color_finish(int64_t m1, int64_t m2, int64_t m3, int sf, int lm) {
    set_mac_ir(m1, m2, m3, sf, lm);
    push_color();
}

/* normal-colour family: 0 = NCS, 1 = NCCS, 2 = NCDS */
static void nc(int v, int kind, int sf, int lm) {
    mat_vec(LL, VX[v], VY[v], VZ[v], 0, 0, 0, sf, lm);
    mat_vec(LC, IR1, IR2, IR3, BK[0], BK[1], BK[2], sf, lm);
    int64_t r = RGBC & 0xFF, g = (RGBC >> 8) & 0xFF, b = (RGBC >> 16) & 0xFF;
    if (kind == 0) color_finish(IR1, IR2, IR3, 0, lm);
    else if (kind == 1) color_finish((r * IR1) << 4, (g * IR2) << 4, (b * IR3) << 4, sf, lm);
    else depth_cue_finish((r * IR1) << 4, (g * IR2) << 4, (b * IR3) << 4, sf, lm);
}

void gte_op(uint32_t op) {
    int sf = (op >> 19) & 1, lm = (op >> 10) & 1;
    int mx = (op >> 17) & 3, vv = (op >> 15) & 3, cv = (op >> 13) & 3;
    int64_t r = RGBC & 0xFF, g = (RGBC >> 8) & 0xFF, b = (RGBC >> 16) & 0xFF;
    FLAG = 0;
    switch (op & 0x3F) {
    case 0x01: rtp(0, sf, lm, 1); break;                                        /* RTPS */
    case 0x30: rtp(0, sf, lm, 0); rtp(1, sf, lm, 0); rtp(2, sf, lm, 1); break;  /* RTPT */
    case 0x06:                                                                  /* NCLIP */
        MAC0 = mac0_chk((int64_t)SX[0] * SY[1] + (int64_t)SX[1] * SY[2] + (int64_t)SX[2] * SY[0] -
                        (int64_t)SX[0] * SY[2] - (int64_t)SX[1] * SY[0] - (int64_t)SX[2] * SY[1]);
        if (g_gte_nclip_invert) MAC0 = -MAC0;   /* a mirrored model is drawn (src/game/ex_mirror.c) */
        break;
    case 0x0C:                                                                  /* OP */
        set_mac_ir((int64_t)RT[1][1] * IR3 - (int64_t)RT[2][2] * IR2, (int64_t)RT[2][2] * IR1 - (int64_t)RT[0][0] * IR3,
                   (int64_t)RT[0][0] * IR2 - (int64_t)RT[1][1] * IR1, sf, lm);
        break;
    case 0x10: depth_cue_finish(r << 16, g << 16, b << 16, sf, lm); break;      /* DPCS */
    case 0x2A:                                                                  /* DPCT */
        for (int i = 0; i < 3; i++) {
            uint32_t c = RGBF[0];
            depth_cue_finish((int64_t)(c & 0xFF) << 16, (int64_t)((c >> 8) & 0xFF) << 16, (int64_t)((c >> 16) & 0xFF) << 16, sf, lm);
        }
        break;
    case 0x11: depth_cue_finish((int64_t)IR1 << 12, (int64_t)IR2 << 12, (int64_t)IR3 << 12, sf, lm); break; /* INTPL */
    case 0x29: depth_cue_finish((r * IR1) << 4, (g * IR2) << 4, (b * IR3) << 4, sf, lm); break;             /* DCPL */
    case 0x12: {                                                                /* MVMVA */
        int16_t (*m)[3] = mx == 0 ? RT : mx == 1 ? LL : LC;
        int16_t garbage[3][3] = {{(int16_t)-(int32_t)((RGBC & 0xFF) << 4), (int16_t)((RGBC & 0xFF) << 4), (int16_t)IR0},
                                 {RT[0][2], RT[0][2], RT[0][2]}, {RT[1][1], RT[1][1], RT[1][1]}};
        if (mx == 3) m = garbage;
        int64_t x, y, z;
        if (vv < 3) { x = VX[vv]; y = VY[vv]; z = VZ[vv]; } else { x = IR1; y = IR2; z = IR3; }
        int64_t t0 = 0, t1 = 0, t2 = 0;
        if (cv == 0) { t0 = TR[0]; t1 = TR[1]; t2 = TR[2]; }
        else if (cv == 1) { t0 = BK[0]; t1 = BK[1]; t2 = BK[2]; }
        else if (cv == 2) { t0 = FC[0]; t1 = FC[1]; t2 = FC[2]; }   /* hardware bug with FC not modelled */
        mat_vec(m, x, y, z, t0, t1, t2, sf, lm);
        break;
    }
    case 0x13: nc(0, 2, sf, lm); break;                                         /* NCDS */
    case 0x16: nc(0, 2, sf, lm); nc(1, 2, sf, lm); nc(2, 2, sf, lm); break;     /* NCDT */
    case 0x1B: nc(0, 1, sf, lm); break;                                         /* NCCS */
    case 0x3F: nc(0, 1, sf, lm); nc(1, 1, sf, lm); nc(2, 1, sf, lm); break;     /* NCCT */
    case 0x1E: nc(0, 0, sf, lm); break;                                         /* NCS */
    case 0x20: nc(0, 0, sf, lm); nc(1, 0, sf, lm); nc(2, 0, sf, lm); break;     /* NCT */
    case 0x14:                                                                  /* CDP */
        mat_vec(LC, IR1, IR2, IR3, BK[0], BK[1], BK[2], sf, lm);
        depth_cue_finish((r * IR1) << 4, (g * IR2) << 4, (b * IR3) << 4, sf, lm);
        break;
    case 0x1C:                                                                  /* CC */
        mat_vec(LC, IR1, IR2, IR3, BK[0], BK[1], BK[2], sf, lm);
        color_finish((r * IR1) << 4, (g * IR2) << 4, (b * IR3) << 4, sf, lm);
        break;
    case 0x28: set_mac_ir((int64_t)IR1 * IR1, (int64_t)IR2 * IR2, (int64_t)IR3 * IR3, sf, lm); break; /* SQR */
    case 0x2D: {                                                                /* AVSZ3 */
        int64_t m = mac0_chk((int64_t)ZSF3 * (SZ[1] + SZ[2] + SZ[3]));
        MAC0 = (int32_t)m;
        m >>= 12;
        OTZ = m < 0 ? (FLAG |= 1u << 18, 0) : m > 0xFFFF ? (FLAG |= 1u << 18, 0xFFFF) : (uint32_t)m;
        break;
    }
    case 0x2E: {                                                                /* AVSZ4 */
        int64_t m = mac0_chk((int64_t)ZSF4 * (SZ[0] + SZ[1] + SZ[2] + SZ[3]));
        MAC0 = (int32_t)m;
        m >>= 12;
        OTZ = m < 0 ? (FLAG |= 1u << 18, 0) : m > 0xFFFF ? (FLAG |= 1u << 18, 0xFFFF) : (uint32_t)m;
        break;
    }
    case 0x3D: color_finish((int64_t)IR0 * IR1, (int64_t)IR0 * IR2, (int64_t)IR0 * IR3, sf, lm); break;  /* GPF */
    case 0x3E: {                                                                /* GPL */
        int sh = sf ? 12 : 0;
        color_finish(((int64_t)MAC1 << sh) + (int64_t)IR0 * IR1, ((int64_t)MAC2 << sh) + (int64_t)IR0 * IR2,
                     ((int64_t)MAC3 << sh) + (int64_t)IR0 * IR3, sf, lm);
        break;
    }
    default:
        if (rt_once(ONCE_GTE, op & 0x3F)) rt_log("GTE op %02X (cmd %07X) not implemented", op & 0x3F, op);
    }
    flag_finish();
}

/* ------------------------------------------------------------------ register access */
static uint32_t pack16(int32_t lo, int32_t hi) { return ((uint32_t)lo & 0xFFFF) | ((uint32_t)hi << 16); }

static uint32_t lzcr(void) {
    uint32_t v = LZCS;
    if (v & 0x80000000u) v = ~v;
    int n = 0;
    while (n < 32 && !(v & (0x80000000u >> n))) n++;
    return (uint32_t)n;
}

static uint32_t orgb(void) {
    int32_t r = IR1 >> 7, g = IR2 >> 7, b = IR3 >> 7;
    r = r < 0 ? 0 : r > 31 ? 31 : r;
    g = g < 0 ? 0 : g > 31 ? 31 : g;
    b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint32_t)r | ((uint32_t)g << 5) | ((uint32_t)b << 10);
}

uint32_t gte_mfc2(int reg) {
    switch (reg & 31) {
    case 0: return pack16(VX[0], VY[0]);  case 1: return (uint32_t)(int32_t)VZ[0];
    case 2: return pack16(VX[1], VY[1]);  case 3: return (uint32_t)(int32_t)VZ[1];
    case 4: return pack16(VX[2], VY[2]);  case 5: return (uint32_t)(int32_t)VZ[2];
    case 6: return RGBC;                  case 7: return OTZ;
    case 8: return (uint32_t)(int32_t)(int16_t)IR0;
    case 9: return (uint32_t)(int32_t)(int16_t)IR1;
    case 10: return (uint32_t)(int32_t)(int16_t)IR2;
    case 11: return (uint32_t)(int32_t)(int16_t)IR3;
    case 12: return pack16(SX[0], SY[0]); case 13: return pack16(SX[1], SY[1]);
    case 14: case 15: return pack16(SX[2], SY[2]);
    case 16: return SZ[0]; case 17: return SZ[1]; case 18: return SZ[2]; case 19: return SZ[3];
    case 20: return RGBF[0]; case 21: return RGBF[1]; case 22: return RGBF[2];
    case 23: return RES1;
    case 24: return (uint32_t)MAC0; case 25: return (uint32_t)MAC1; case 26: return (uint32_t)MAC2; case 27: return (uint32_t)MAC3;
    case 28: case 29: return orgb();
    case 30: return LZCS;
    default: return lzcr();
    }
}

void gte_mtc2(int reg, uint32_t v) {
    switch (reg & 31) {
    case 0: VX[0] = (int16_t)v; VY[0] = (int16_t)(v >> 16); break;  case 1: VZ[0] = (int16_t)v; break;
    case 2: VX[1] = (int16_t)v; VY[1] = (int16_t)(v >> 16); break;  case 3: VZ[1] = (int16_t)v; break;
    case 4: VX[2] = (int16_t)v; VY[2] = (int16_t)(v >> 16); break;  case 5: VZ[2] = (int16_t)v; break;
    case 6: RGBC = v; break;              case 7: OTZ = v & 0xFFFF; break;
    case 8: IR0 = s16(v); break; case 9: IR1 = s16(v); break; case 10: IR2 = s16(v); break; case 11: IR3 = s16(v); break;
    case 12: SX[0] = s16(v); SY[0] = s16(v >> 16); break;
    case 13: SX[1] = s16(v); SY[1] = s16(v >> 16); break;
    case 14: SX[2] = s16(v); SY[2] = s16(v >> 16); break;
    case 15: SX[0] = SX[1]; SY[0] = SY[1]; SX[1] = SX[2]; SY[1] = SY[2]; SX[2] = s16(v); SY[2] = s16(v >> 16); break;
    case 16: SZ[0] = v & 0xFFFF; break; case 17: SZ[1] = v & 0xFFFF; break;
    case 18: SZ[2] = v & 0xFFFF; break; case 19: SZ[3] = v & 0xFFFF; break;
    case 20: RGBF[0] = v; break; case 21: RGBF[1] = v; break; case 22: RGBF[2] = v; break;
    case 23: RES1 = v; break;
    case 24: MAC0 = (int32_t)v; break; case 25: MAC1 = (int32_t)v; break;
    case 26: MAC2 = (int32_t)v; break; case 27: MAC3 = (int32_t)v; break;
    case 28: IR1 = (int32_t)((v & 0x1F) << 7); IR2 = (int32_t)(((v >> 5) & 0x1F) << 7); IR3 = (int32_t)(((v >> 10) & 0x1F) << 7); break;
    case 29: break;
    case 30: LZCS = v; break;
    default: break;
    }
}

static void set_mat(int16_t m[3][3], int idx, uint32_t v) {
    int16_t *f = &m[0][0];
    if (idx < 4) { f[idx * 2] = (int16_t)v; f[idx * 2 + 1] = (int16_t)(v >> 16); }
    else f[8] = (int16_t)v;
}
static uint32_t get_mat(int16_t m[3][3], int idx) {
    int16_t *f = &m[0][0];
    if (idx < 4) return pack16(f[idx * 2], f[idx * 2 + 1]);
    return (uint32_t)(int32_t)f[8];
}

uint32_t gte_cfc2(int reg) {
    reg &= 31;
    if (reg < 5) return get_mat(RT, reg);
    if (reg < 8) return (uint32_t)TR[reg - 5];
    if (reg < 13) return get_mat(LL, reg - 8);
    if (reg < 16) return (uint32_t)BK[reg - 13];
    if (reg < 21) return get_mat(LC, reg - 16);
    if (reg < 24) return (uint32_t)FC[reg - 21];
    switch (reg) {
    case 24: return (uint32_t)OFX;
    case 25: return (uint32_t)OFY;
    case 26: return (uint32_t)(int32_t)(int16_t)H;     /* reads back sign-extended (hardware quirk) */
    case 27: return (uint32_t)(int32_t)DQA;
    case 28: return (uint32_t)DQB;
    case 29: return (uint32_t)(int32_t)ZSF3;
    case 30: return (uint32_t)(int32_t)ZSF4;
    default: return FLAG;
    }
}

void gte_ctc2(int reg, uint32_t v) {
    reg &= 31;
    if (reg < 5) { set_mat(RT, reg, v); return; }
    if (reg < 8) { TR[reg - 5] = (int32_t)v; return; }
    if (reg < 13) { set_mat(LL, reg - 8, v); return; }
    if (reg < 16) { BK[reg - 13] = (int32_t)v; return; }
    if (reg < 21) { set_mat(LC, reg - 16, v); return; }
    if (reg < 24) { FC[reg - 21] = (int32_t)v; return; }
    switch (reg) {
    case 24: OFX = (int32_t)v; break;
    case 25: OFY = (int32_t)v; break;
    case 26: H = (uint16_t)v; break;
    case 27: DQA = (int16_t)v; break;
    case 28: DQB = (int32_t)v; break;
    case 29: ZSF3 = (int16_t)v; break;
    case 30: ZSF4 = (int16_t)v; break;
    default: FLAG = v & 0x7FFFF000u; flag_finish(); break;
    }
}
