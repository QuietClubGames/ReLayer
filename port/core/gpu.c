/*
 * PS1 GPU command decoder: VRAM transfers, display state, and drawing (via gpu_raster.c).
 */
#include "gpu.h"
#include "gpu_raster.h"
#include "gpu_hd.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

uint16_t g_vram[1024 * 1024];   /* System 12: 2 MB VRAM, 1024 lines */

static uint32_t fifo[16 + 256];
static int fifo_n, fifo_need;
static uint32_t stat = 0x14802000u;
static GpuDisplay disp = {0, 0, 320, 240, 0, 0};
static int disp_x1 = 0x260, disp_x2 = 0x260 + 320 * 8, disp_y1 = 16, disp_y2 = 256;
static uint32_t gpuread_latch;

/* CPU->VRAM / VRAM->CPU transfer state */
static int xfer_mode;            /* 0 none, 1 write, 2 read */
static int xx, xy, xw, xh, xi;

static uint64_t prims_frame, prims_total;

static int cmd_words(uint32_t c) {
    uint32_t op = c >> 24;
    if (op >= 0x20 && op < 0x40) {
        int verts = (op & 8) ? 4 : 3, tex = (op & 4) ? 1 : 0, g = (op & 0x10) ? 1 : 0;
        return 1 + verts * (1 + tex) + (g ? verts - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) {
        if (op & 8) return -1;                                       /* polyline */
        return (op & 0x10) ? 4 : 3;
    }
    if (op >= 0x60 && op < 0x80) return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    if (op >= 0x80 && op < 0xA0) return 4;
    if (op >= 0xA0 && op < 0xC0) return 3;
    if (op >= 0xC0 && op < 0xE0) return 3;
    if (op == 0x02) return 3;
    return 1;
}

static void vram_put(int x, int y, uint16_t v) { g_vram[((y & 1023) << 10) | (x & 1023)] = v; }

/* debug: FGT_GPU_DUMP_FRAME=<n> logs every drawing command of that frame */
#include <stdlib.h>
#include "hw.h"
static void dump_cmd(void) {
    static long want = -2;
    if (want == -2) { const char *e = getenv("FGT_GPU_DUMP_FRAME"); want = e ? atol(e) : -1; }
    if (want < 0 || (long)g_frame != want) return;
    char b[400]; int o = 0;
    for (int i = 0; i < fifo_n && o < 360; i++) o += snprintf(b + o, sizeof b - (size_t)o, " %08X", fifo[i]);
    rt_log("gp0:%s", b);
}

static void exec_cmd(void) {
    uint32_t c = fifo[0], op = c >> 24;
    dump_cmd();
    if (op == 0x02) {                                                /* fill rectangle */
        int x = fifo[1] & 0x3F0, y = (fifo[1] >> 16) & 0x3FF;
        int w = ((fifo[2] & 0x3FF) + 15) & ~15, h = (fifo[2] >> 16) & 0x1FF;
        uint16_t col = (uint16_t)(((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10));
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) vram_put(x + i, y + j, col);
        hd_fill(x, y, w, h, c & 0xFFFFFF);
        return;
    }
    if (op >= 0x20 && op < 0x40) { prims_frame++; raster_poly(fifo); hd_record(fifo, fifo_n); return; }
    if (op >= 0x40 && op < 0x60) { prims_frame++; raster_line(fifo, fifo_n); hd_record(fifo, fifo_n); return; }
    if (op >= 0x60 && op < 0x80) { prims_frame++; raster_rect(fifo); hd_record(fifo, fifo_n); return; }
    if (op >= 0x80 && op < 0xA0) {                                   /* VRAM -> VRAM */
        int sx = fifo[1] & 0x3FF, sy = (fifo[1] >> 16) & 0x3FF, dx = fifo[2] & 0x3FF, dy = (fifo[2] >> 16) & 0x3FF;
        int w = fifo[3] & 0xFFFF, h = fifo[3] >> 16;
        if (!w) w = 1024;
        if (!h) h = 512;
        if (sx != dx || sy != dy) hd_vram_written(dx, dy, w, h);   /* before it happens; onto itself changes nothing */
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++)
            vram_put(dx + i, dy + j, g_vram[(((sy + j) & 1023) << 10) | ((sx + i) & 1023)]);
        return;
    }
    if (op >= 0xA0 && op < 0xE0) {                                   /* start CPU<->VRAM transfer */
        xx = fifo[1] & 0x3FF; xy = (fifo[1] >> 16) & 0x3FF;
        xw = fifo[2] & 0xFFFF; xh = fifo[2] >> 16;
        if (!xw) xw = 1024;
        if (!xh) xh = 512;
        xi = 0;
        xfer_mode = op < 0xC0 ? 1 : 2;
        if (xfer_mode == 1) hd_vram_written(xx, xy, xw, xh);
        return;
    }
    if (op >= 0xE1 && op <= 0xE6) {
        if (op == 0xE1) stat = (stat & ~0x87FFu) | (c & 0x7FFu) | ((c & 0x800u) << 4);
        raster_env(c);
        return;
    }
}

void gpu_gp0(uint32_t v) {
    if (xfer_mode == 1) {
        for (int k = 0; k < 2; k++) {
            int px = xi % xw, py = xi / xw;
            if (py < xh) vram_put(xx + px, xy + py, (uint16_t)(v >> (16 * k)));
            xi++;
        }
        if (xi >= xw * xh) xfer_mode = 0;
        return;
    }
    if (fifo_n == 0) {
        fifo_need = cmd_words(v);
        if (fifo_need < 0) fifo_need = 256;                          /* polyline: until terminator */
    }
    fifo[fifo_n++] = v;
    if (fifo_need == 256 && fifo_n > 2 && (v & 0xF000F000u) == 0x50005000u) { exec_cmd(); fifo_n = 0; return; }
    if (fifo_n >= fifo_need) { exec_cmd(); fifo_n = 0; }
}

void gpu_gp1(uint32_t v) {
    uint32_t op = (v >> 24) & 0x3F;
    {
        static long want = -2;
        if (want == -2) { const char *e = getenv("FGT_GPU_DUMP_FRAME"); want = e ? atol(e) : -1; }
        if (want >= 0 && (long)g_frame >= want - 2 && (long)g_frame <= want) rt_log("gp1: %08X (frame %ld)", v, (long)g_frame);
    }
    switch (op) {
    case 0x00: stat = 0x14802000u; disp.enabled = 0; fifo_n = 0; xfer_mode = 0; break;
    case 0x01: fifo_n = 0; xfer_mode = 0; break;
    case 0x03: disp.enabled = !(v & 1); stat = (stat & ~0x800000u) | ((v & 1) << 23); break;
    case 0x04: stat = (stat & ~0x60000000u) | ((v & 3) << 29); break;
    case 0x05: disp.x = v & 0x3FE; disp.y = (v >> 10) & 0x3FF; hd_set_display(disp.x, disp.y, disp.w, disp.h); break;
    case 0x06: disp_x1 = v & 0xFFF; disp_x2 = (v >> 12) & 0xFFF; break;
    case 0x07: disp_y1 = v & 0x3FF; disp_y2 = (v >> 10) & 0x3FF; break;
    case 0x08: {
        static const int widths[] = {256, 320, 512, 640};
        disp.w = (v & 0x40) ? 368 : widths[v & 3];
        disp.h = (v & 4) ? 480 : 240;
        disp.depth24 = (v >> 4) & 1;
        stat = (stat & ~0x7F4000u) | ((v & 0x3F) << 17) | ((v & 0x40) << 10) | ((v & 0x80) << 7);
        break;
    }
    case 0x10: gpuread_latch = 2; break;                             /* GPU info: version 2 */
    default:
        if (rt_once(ONCE_GPU, 0x100u | op)) rt_log("GP1 %02X (%08X) not handled", op, v);
    }
    (void)disp_x1; (void)disp_x2; (void)disp_y1; (void)disp_y2;
}

uint32_t gpu_read(void) {
    if (xfer_mode == 2) {
        uint32_t v = 0;
        for (int k = 0; k < 2; k++) {
            int px = xi % xw, py = xi / xw;
            v |= (uint32_t)g_vram[(((xy + py) & 1023) << 10) | ((xx + px) & 1023)] << (16 * k);
            xi++;
        }
        if (xi >= xw * xh) xfer_mode = 0;
        return v;
    }
    return gpuread_latch;
}

uint32_t gpu_stat(void) {
    /* always ready: commands, VRAM->CPU, DMA */
    uint32_t s = stat | 0x1C000000u;
    s ^= 0x80000000u * (uint32_t)(prims_total & 1);
    return s;
}

void gpu_vblank(void) {
    static uint64_t last_log;
    prims_total += prims_frame + 1;
    stat ^= 0x80000000u;
    if (prims_frame && (prims_total - last_log) > 2000) {
        rt_log("gpu: %llu primitives this frame", (unsigned long long)prims_frame);
        last_log = prims_total;
    }
    prims_frame = 0;
}

GpuDisplay gpu_display(void) { return disp; }
