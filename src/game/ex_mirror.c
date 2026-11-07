/*
 * Mirroring for characters converted from Street Fighter EX2 / EX2 Plus (docs/SFEX2.md).
 *
 * Fighting Layer draws a fighter facing the other way by mirroring the *pose*: func_80216FF0 swaps the arm
 * and leg tracks and rewrites the angles with per-bone rules. That is an exact mirror only for a left-right
 * symmetric model, which all of Fighting Layer's characters are. EX2 characters are not (Pullum's sash end
 * hangs on one side and swings to the front), and EX2 mirrors the whole figure instead. So for a converted
 * character (EX2 mirrors the figure facing right, see below):
 *   - build the authored (unmirrored) pose: func_80216FF0 runs with the mirror flag cleared;
 *   - reflect it in the player's local X: every node's rotation R becomes Mx R Mx = R(rx, -ry, -rz) and its
 *     translation x is negated, and every vertex and normal of the model (parts and skin) has x negated;
 *   - reflection turns polygons round, so the GTE's NCLIP result is negated while her parts and skin are
 *     drawn (func_802069CC, func_802B6088, func_802B639C); they are culled by NCLIP > 0.
 * World = base * Mx * (pose chain) * Mx * v: the mirror image of the authored figure, like EX2 draws it.
 *
 * Converted models are registered by src/game/file_override.c when it loads one with a skeleton (extra_dir),
 * so none of this runs in a normal game. The hand-variant swap Fighting Layer does when mirrored is skipped for
 * converted models (f_80216EE4 below).
 */
#include "recomp.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

void f_80216FF0_orig(void);
void f_802069CC_orig(void);
void f_802B6088_orig(void);
void f_802B639C_orig(void);
extern int g_gte_nclip_invert;

#define NODES 19
#define NODE(pl, i) ((pl) + 0x1F4u + (uint32_t)(i) * 0xE8u)

/* lo/hi: where the model sits in RAM; head_lo/head_hi/head: a clean copy of its body parts (E0..E(N-3)).
 * The Fighting Layer character's move code animates its own meshes by rewriting fixed records (Kato's
 * overlay, 0x800A66xx-0x800A6Axx, writes into the head and a few other parts), which pulls an EX2 model out of
 * shape, so the parts are put back before each draw. */
typedef struct { uint32_t lo, hi, head_lo, head_hi; uint8_t *head; } Model;
static Model g_models[4];
static int g_nmodels;

typedef struct {
    uint32_t player, model;
    int mirrored;                  /* state the model's vertices and bone offsets are in */
} State;
static State g_state[2];

void ex_mirror_register(uint32_t dest, uint32_t size) {
    Model *m = NULL;
    for (int i = 0; i < g_nmodels; i++) if (g_models[i].lo == dest) m = &g_models[i];
    if (!m && g_nmodels < 4) m = &g_models[g_nmodels++];
    if (!m) return;
    m->lo = dest;
    m->hi = dest + size;
    uint32_t n = MEM_R32(dest);                        /* N entries: E0..E(N-3) parts, then the skin */
    m->head_lo = dest + 4u * MEM_R32(dest);            /* E0 */
    m->head_hi = dest + 4u * MEM_R32(dest + 4u * (n - 2));   /* end of the last part */
    free(m->head);
    m->head = malloc(m->head_hi - m->head_lo);
    if (m->head)
        for (uint32_t a = m->head_lo; a < m->head_hi; a++) m->head[a - m->head_lo] = MEM_R8(a);
    /* a fresh load: the data is unmirrored again */
    for (int i = 0; i < 2; i++)
        if (g_state[i].model == dest) { g_state[i].model = 0; g_state[i].player = 0; }
    rt_log("ex mirror: converted model at %08X (%u bytes)", dest, size);
}

/* Another file was loaded over [dest, dest+size): a converted model there is gone (the next fight, an attract
 * demo...), so forget it; otherwise another character's model loaded at that address would be treated as hers. */
void ex_ponytail_forget(uint32_t model);
void ex_mirror_forget(uint32_t dest, uint32_t size) {
    for (int i = 0; i < g_nmodels; i++) {
        Model *m = &g_models[i];
        if (dest >= m->hi || dest + size <= m->lo) continue;
        rt_log("ex mirror: model at %08X overwritten, forgotten", m->lo);
        ex_ponytail_forget(m->lo);
        for (int j = 0; j < 2; j++)
            if (g_state[j].model == m->lo) { g_state[j].model = 0; g_state[j].player = 0; }
        free(m->head);
        *m = g_models[--g_nmodels];
        g_models[g_nmodels].head = NULL;
        i--;
    }
}

/* the registered model this player draws (by where its chest part points), or 0 */
static uint32_t model_of(uint32_t player) {
    if (!g_nmodels || (player & 0xFF000000u) != 0x80000000u) return 0;
    uint32_t part = MEM_R32(NODE(player, 2) + 0xD0u);
    for (int i = 0; i < g_nmodels; i++)
        if (part >= g_models[i].lo && part < g_models[i].hi) return g_models[i].lo;
    return 0;
}

static State *state_of(uint32_t player, uint32_t model) {
    for (int i = 0; i < 2; i++)
        if (g_state[i].player == player) {
            if (g_state[i].model != model) { g_state[i].model = model; g_state[i].mirrored = 0; }
            return &g_state[i];
        }
    for (int i = 0; i < 2; i++)
        if (!g_state[i].player) {
            g_state[i].player = player; g_state[i].model = model; g_state[i].mirrored = 0;
            return &g_state[i];
        }
    g_state[0].player = player; g_state[0].model = model; g_state[0].mirrored = 0;
    return &g_state[0];
}

static void neg16(uint32_t a) { MEM_W16(a, (uint16_t)(-(int16_t)MEM_R16(a))); }

/* negate x of every vertex and normal in one part (types: docs/FL_MODEL_FORMAT.md 1.3) */
static void flip_part(uint32_t p, uint32_t end) {
    static const struct { int type, size, voff, nv, noff, nn; } k[] = {
        {0x20, 36, 4, 3, 0x1C, 1}, {0x28, 44, 4, 4, 0x24, 1}, {0x30, 60, 12, 3, 0x24, 3}, {0x38, 80, 16, 4, 0x30, 4},
        {0x24, 44, 12, 3, 0x24, 1}, {0x2C, 52, 12, 4, 0x2C, 1}, {0x34, 60, 12, 3, 0x24, 3}, {0x3C, 76, 12, 4, 0x2C, 4},
        {0x44, 52, 4, 3, 0x1C, 3}, {0x4C, 68, 4, 4, 0x24, 3},
    };
    while (p + 4 <= end) {
        int16_t count = (int16_t)MEM_R16(p), type = (int16_t)MEM_R16(p + 2);
        if (count <= 0) return;                        /* 0xFFFFFFFF end, or (0,0) after metal groups */
        int t = -1;
        for (int i = 0; i < (int)(sizeof k / sizeof k[0]); i++) if (k[i].type == type) t = i;
        if (t < 0) return;
        uint32_t r = p + 4;
        for (int c = 0; c < count; c++, r += (uint32_t)k[t].size) {
            for (int v = 0; v < k[t].nv; v++) neg16(r + (uint32_t)k[t].voff + 8u * (uint32_t)v);
            for (int n = 0; n < k[t].nn; n++) neg16(r + (uint32_t)k[t].noff + 8u * (uint32_t)n);
        }
        p = r;
    }
}

static void flip_model(uint32_t m) {
    uint32_t n = MEM_R32(m);                           /* N entries; the first word is N */
    if (n < 4 || n > 64) return;
    uint32_t end = 0;
    for (int i = 0; i < g_nmodels; i++) if (g_models[i].lo == m) end = g_models[i].hi;
    for (uint32_t e = 0; e + 2 < n; e++)               /* parts E0..E(N-3) */
        flip_part(m + 4u * MEM_R32(m + 4u * e), m + 4u * MEM_R32(m + 4u * (e + 1)));
    uint32_t s = m + 4u * MEM_R32(m + 4u * (n - 1));   /* skin geometry E(N-1) */
    while (s + 4 <= end && MEM_R32(s) != 0xFFFFFFFFu) {
        for (int sec = 0; sec < 2; sec++) {
            uint32_t count = MEM_R16(s + 2);
            for (uint32_t v = 0; v < count; v++) { neg16(s + 4 + 16 * v); neg16(s + 4 + 16 * v + 8); }
            s += 4 + 16 * count;
        }
        s += 4 + 12 * MEM_R32(s);
    }
}

/* pose finish: authored pose for converted characters, then reflected if they face the mirrored way */
void f_80216FF0(void) {
    uint32_t player = R(4), model = model_of(player);
    if (!model) { f_80216FF0_orig(); return; }
    uint8_t flags = MEM_R8(player + 0x1C8);
    /* EX2 characters are authored facing the other way from Fighting Layer's: EX2 draws its player 1 (facing
     * right) as the mirror image and player 2 as authored, while Fighting Layer's flag is set for the player
     * facing left. So reflect when the flag is clear, draw as authored when it is set. */
    int mirror = !(flags & 1);
    MEM_W8(player + 0x1C8, flags & ~1u);
    f_80216FF0_orig();
    MEM_W8(player + 0x1C8, flags);
    State *st = state_of(player, model);
    if (mirror != st->mirrored) {                      /* turned round: flip the model and the bones */
        flip_model(model);
        for (int i = 1; i < NODES; i++) MEM_W32(NODE(player, i) + 0xA0u, (uint32_t)-(int32_t)MEM_R32(NODE(player, i) + 0xA0u));
        st->mirrored = mirror;
    }
    if (mirror) {
        MEM_W32(NODE(player, 0) + 0xA0u, (uint32_t)-(int32_t)MEM_R32(NODE(player, 0) + 0xA0u));   /* root x */
        for (int i = 0; i < NODES; i++) { neg16(NODE(player, i) + 0x9Au); neg16(NODE(player, i) + 0x9Cu); }
    }
}

static int mirrored_now(uint32_t player) {
    uint32_t model = model_of(player);
    if (!model) return 0;
    for (int i = 0; i < 2; i++) if (g_state[i].player == player) return g_state[i].mirrored;
    return 0;
}

/* Motion blend start (func_802173C4, from the motion setter): saves the pose on screen, each node's angles
 * (+0x98/+0x9A/+0x9C -> +0xC0/+0xC2/+0xC4) and the root translation (+0xA0.. -> +0x1750..), and the pose
 * finish then blends the new motion from it over the next frames. For a converted character that pose is the
 * reflected one, while the blend runs on the authored pose (func_80216FF0 above), so the saved pose is
 * reflected back here. Without it every blend started while reflected was off, slightly for most motions and
 * badly when she turns round (forward + heavy kick's sidestep tipped her onto her side). */
void f_802173C4_orig(void);
void f_802173C4(void) {
    uint32_t player = R(4);
    f_802173C4_orig();
    if (!mirrored_now(player)) return;
    int count = MEM_R16(0x802DCA74u + 2u * MEM_R8(player + 2));   /* the nodes it saved */
    for (int i = 0; i < count && i < NODES; i++) { neg16(NODE(player, i) + 0xC2u); neg16(NODE(player, i) + 0xC4u); }
    MEM_W32(player + 0x1750u, (uint32_t)-(int32_t)MEM_R32(player + 0x1750u));             /* root x */
}

/* Hit spheres (docs/EX2P_HIT.md): each frame func_8021FBEC builds a player's body hurt spheres (func_8021F364:
 * the list at +0x1788 and the extra ones at +0x178C), attack spheres (func_8021F704, +0x1790) and clash spheres
 * (func_8021F978, +0x1798), each an offset {x, y, z} in a node's space through the node's matrix. Fighting Layer
 * mirrors by the pose, and for a mirrored player (+0x1C8 bit 0) moves each sphere to the other limb's node with z
 * negated. A converted character is mirrored as a whole figure instead: when reflected, every node's matrix is
 * the reflected one (Mx N Mx), so the sphere's own offset has to be reflected too, x negated, or it lands on the
 * far side of its joint (her standing heavy kick's spheres ran back from the knee instead of out to the foot); and
 * when drawn as authored (the flag set) the sphere stays on its own node. So for her the flag is cleared while
 * they are built, and when she is reflected the offsets' x are negated for the build and put back after. */
static void neg_spheres(uint32_t s, int n) { for (int i = 0; i < n; i++) neg16(s + 8u * (uint32_t)i); }
static void neg_body(uint32_t list) {                       /* 18 at most, ended by radius 0 */
    for (int i = 0; list && i < 18 && MEM_R8(list + 8u * (uint32_t)i + 6); i++) neg16(list + 8u * (uint32_t)i);
}
static void neg_timed(uint32_t list, uint32_t head) {       /* {s16 start; u8 keys; u8 n; [attr]; spheres} */
    for (int k = 0; list && k < 64 && (int16_t)MEM_R16(list) >= 0; k++) {
        int n = MEM_R8(list + 3);
        neg_spheres(list + head, n);
        list += head + 8u * (uint32_t)n;
    }
}
static void spheres_flip(uint32_t player, int which) {
    if (which == 0) { neg_body(MEM_R32(player + 0x1788u)); neg_timed(MEM_R32(player + 0x178Cu), 4); }
    else if (which == 1) neg_timed(MEM_R32(player + 0x1790u), 24);
    else neg_timed(MEM_R32(player + 0x1798u), 4);
}
#define SPHERE_WRAPPER(name, which) \
    void f_##name##_orig(void); \
    void f_##name(void) { \
        uint32_t player = R(4); \
        if (!model_of(player)) { f_##name##_orig(); return; } \
        int refl = mirrored_now(player); \
        uint8_t flags = MEM_R8(player + 0x1C8); \
        MEM_W8(player + 0x1C8, flags & ~1u); \
        if (refl) spheres_flip(player, which); \
        f_##name##_orig(); \
        if (refl) spheres_flip(player, which); \
        MEM_W8(player + 0x1C8, flags); \
    }
SPHERE_WRAPPER(8021F364, 0)
SPHERE_WRAPPER(8021F704, 1)
SPHERE_WRAPPER(8021F978, 2)

#define DRAW_WRAPPER(name) \
    void f_##name(void) { \
        int inv = mirrored_now(R(4)); \
        g_gte_nclip_invert ^= inv; \
        f_##name##_orig(); \
        g_gte_nclip_invert ^= inv; \
    }
void ex_ponytail_update(uint32_t player, uint32_t model, int mirrored);
extern int g_ghost_drawing;
void ex_ghosts_draw(uint32_t player, uint32_t a1);

/* draw a fighter's parts: her ghost images first (src/game/ex_pullum.c), then put the converted model's parts
 * back, then move her ponytail */
void f_802069CC(void) {
    uint32_t player = R(4);
    if (!g_ghost_drawing) {
        uint32_t a1 = R(5);
        ex_ghosts_draw(player, a1);
        R(4) = player; R(5) = a1;
    }
    uint32_t model = model_of(player);
    int inv = 0;
    if (model) {
        inv = mirrored_now(player);
        for (int i = 0; i < g_nmodels; i++) {
            Model *m = &g_models[i];
            if (m->lo != model || !m->head) continue;
            for (uint32_t a = m->head_lo; a < m->head_hi; a++) MEM_W8(a, m->head[a - m->head_lo]);
            ex_ponytail_update(player, model, inv);
            if (inv) {
                uint32_t n = MEM_R32(m->lo);
                for (uint32_t e = 0; e + 2 < n; e++)
                    flip_part(m->lo + 4u * MEM_R32(m->lo + 4u * e), m->lo + 4u * MEM_R32(m->lo + 4u * (e + 1)));
            }
        }
    }
    g_gte_nclip_invert ^= inv;
    f_802069CC_orig();
    g_gte_nclip_invert ^= inv;
}
DRAW_WRAPPER(802B6088)
DRAW_WRAPPER(802B639C)

/* Kato's move code draws two cloth effects of his own as separate objects (handlers in his code block: slot
 * entry 10, 0x800A4EC8, and entry 11, 0x800A55E4 with its swing physics at 0x800A63E4; +0x10000 in slot 2;
 * object+0x10 = player). On a converted character they show as strips over her chest, so they are skipped for
 * her. Other Fighting Layer characters' extras would need the same when a converted character replaces them. */
void o_pl00_1_800A4EC8_orig(void);
void o_pl00_2_800B4EC8_orig(void);
void o_pl00_1_800A55E4_orig(void);
void o_pl00_2_800B55E4_orig(void);
static int skip_effect(void) { return model_of(MEM_R32(R(4) + 0x10)) != 0; }
void o_pl00_1_800A4EC8(void) { if (!skip_effect()) o_pl00_1_800A4EC8_orig(); }
void o_pl00_2_800B4EC8(void) { if (!skip_effect()) o_pl00_2_800B4EC8_orig(); }
void o_pl00_1_800A55E4(void) { if (!skip_effect()) o_pl00_1_800A55E4_orig(); }
void o_pl00_2_800B55E4(void) { if (!skip_effect()) o_pl00_2_800B55E4_orig(); }

/* Hand shapes (func_80216EE4(player, left, right)): Fighting Layer swaps the two values when the player is
 * mirrored, because it mirrors the pose. A converted character is mirrored as a whole figure, which already
 * puts each hand on the right side, so the swap is skipped for her. */
void f_80216EE4_orig(void);
void f_80216EE4(void) {
    uint32_t player = R(4);
    if (!model_of(player)) { f_80216EE4_orig(); return; }
    uint8_t flags = MEM_R8(player + 0x1C8);
    MEM_W8(player + 0x1C8, flags & ~1u);
    f_80216EE4_orig();
    MEM_W8(player + 0x1C8, flags);
}
