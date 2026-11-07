#pragma once
#include <stdint.h>

/*
 * Drawing-command recorder for high-resolution renderers.
 *
 * The software renderer (gpu_raster.c) stays authoritative for VRAM. Alongside it, every drawing command
 * aimed at a screen buffer is recorded with full-precision vertex positions, so a GPU backend can redraw
 * the displayed buffer at any resolution. Fighting Layer double-buffers two 512x240 screens, clears each
 * one at the start of a frame, and never copies or uploads pictures into them, so a frame is exactly
 * "everything drawn since the last full clear".
 *
 * Textures: the replay reads VRAM after the fact, but some stages load new textures into VRAM while a frame
 * is being drawn (the ring stage does, around the fighters). So VRAM is snapshotted in "generations": just
 * before an upload or copy overwrites texels or CLUTs that recorded primitives use, the current VRAM is saved
 * and those primitives keep reading from that copy.
 */
#define HD_MAX_GEN 8

enum { HD_TRI, HD_RECT, HD_LINE, HD_FILL };
enum {
    HD_TEX = 1,      /* textured */
    HD_RAW = 2,      /* texture not modulated by vertex colour */
    HD_SEMI = 4,     /* semi-transparent */
    HD_GOURAUD = 8,
};

typedef struct {
    float x, y;      /* VRAM pixel coordinates (draw offset applied, sub-pixel precision when known) */
    float w;         /* perspective divisor (GTE depth), 1 when unknown */
    float u, v;      /* texel coordinates */
    uint8_t r, g, b, a;
} HdVtx;

typedef struct {
    uint8_t kind, semi_mode, depth, gen;   /* depth: 0 = 4-bit CLUT, 1 = 8-bit CLUT, 2 = 15-bit direct;
                                              gen: which VRAM snapshot the texture comes from (HdFrame.vram) */
    uint16_t flags;
    uint16_t tx, ty, cx, cy;               /* texture page origin, CLUT origin (VRAM pixels) */
    uint8_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int16_t clip_x1, clip_y1, clip_x2, clip_y2;
    HdVtx v[3];   /* TRI: 3 corners; RECT/FILL: v[0] top-left, v[1] bottom-right (exclusive); LINE: v[0], v[1] */
} HdPrim;

typedef struct {
    int x, y, w, h;                        /* the displayed buffer in VRAM */
    int count;
    const HdPrim *prims;
    int ngen;                              /* VRAM snapshots in vram[] (1 unless textures changed mid-frame) */
    const uint16_t *vram[HD_MAX_GEN];      /* VRAM (1024x1024) for primitives of each generation; the last one
                                              is VRAM as it was when this frame went on screen */
} HdFrame;

/* called by gpu.c */
void hd_record(const uint32_t *words, int nwords);
void hd_fill(int x, int y, int w, int h, uint32_t rgb);
void hd_vram_written(int x, int y, int w, int h);       /* CPU upload or VRAM copy */
void hd_set_display(int x, int y, int w, int h);

/* the recorded frame for the buffer on screen right now, or NULL if it can't be redrawn exactly */
const HdFrame *gpu_hd_frame(void);
