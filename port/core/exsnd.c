/*
 * A small sample player mixed into the game's sound output (see exsnd.h). A converted character's voice is not in
 * Fighting Layer's sound ROM, so her samples are played here and added to the C352's output (s12sound_run), which
 * keeps them in recordings and under the same output rate control.
 *
 * File: "EXSN", u32 count, count x {u32 offset, u32 frames, u32 rate, u8 volume 0-127, u8 kind, u16 0}, then s16
 * mono samples (offset in bytes from their start). Kind 1 = voice: one channel per side, a new voice cuts the
 * previous one (as on the PlayStation SPU voice the game reserves per side); kind 0 = effect: 4 channels per side
 * in turn.
 */
#include "exsnd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_RATE 88200
#define PER_SIDE 5                 /* 1 voice + 4 effects */
#define GAIN 28                    /* of 256: her samples peak near full scale. 56 matched the peaks of Fighting Layer's voices (~9000) but sounded too loud in play; half that */

typedef struct { uint32_t offset, frames, rate; uint8_t volume, kind; } Sample;
typedef struct { const int16_t *pcm; uint32_t frames, pos, step; int left, right; } Channel;   /* pos/step 16.16 */

static Sample *g_samples;
static int g_count;
static int16_t *g_pcm;
static Channel g_ch[2 * PER_SIDE];
static int g_next[2];

int exsnd_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[4];
    uint32_t count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "EXSN", 4) || fread(&count, 4, 1, f) != 1 || count > 4096) {
        fclose(f);
        return 0;
    }
    Sample *s = calloc(count, sizeof *s);
    for (uint32_t i = 0; s && i < count; i++) {
        uint8_t rec[16];
        if (fread(rec, 1, 16, f) != 16) { free(s); s = NULL; break; }
        memcpy(&s[i].offset, rec, 4);
        memcpy(&s[i].frames, rec + 4, 4);
        memcpy(&s[i].rate, rec + 8, 4);
        s[i].volume = rec[12];
        s[i].kind = rec[13];
    }
    long start = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f) - start;
    fseek(f, start, SEEK_SET);
    int16_t *pcm = s && size > 0 ? malloc((size_t)size) : NULL;
    int ok = pcm && fread(pcm, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok) { free(s); free(pcm); return 0; }
    for (uint32_t i = 0; i < count; i++)                 /* drop records that point outside the data */
        if ((uint64_t)s[i].offset + 2ull * s[i].frames > (uint64_t)size) s[i].frames = 0;
    exsnd_stop_all();
    free(g_samples);
    free(g_pcm);
    g_samples = s;
    g_pcm = pcm;
    g_count = (int)count;
    return 1;
}

void exsnd_stop_all(void) { memset(g_ch, 0, sizeof g_ch); }

void exsnd_play(int side, int id, int pan) {
    if (id < 0 || id >= g_count || !g_samples[id].frames || side < 0 || side > 1) return;
    const Sample *s = &g_samples[id];
    int c = side * PER_SIDE;
    if (!s->kind) { c += 1 + g_next[side]; g_next[side] = (g_next[side] + 1) & 3; }
    if (pan < -63) pan = -63;
    if (pan > 63) pan = 63;
    Channel *ch = &g_ch[c];
    ch->pcm = (const int16_t *)((const uint8_t *)g_pcm + s->offset);
    ch->frames = s->frames;
    ch->pos = 0;
    ch->step = (uint32_t)(((uint64_t)s->rate << 16) / OUT_RATE);
    int v = s->volume * GAIN;                            /* 0 .. 127 * 256 */
    ch->left = v * (64 - pan) / 64;
    ch->right = v * (64 + pan) / 64;
}

void exsnd_mix(int16_t *stereo, int frames) {
    for (int c = 0; c < 2 * PER_SIDE; c++) {
        Channel *ch = &g_ch[c];
        if (!ch->frames) continue;
        for (int i = 0; i < frames; i++) {
            uint32_t k = ch->pos >> 16;
            if (k + 1 >= ch->frames) { ch->frames = 0; break; }
            int frac = (int)(ch->pos & 0xFFFF);
            int x = ch->pcm[k] + (((ch->pcm[k + 1] - ch->pcm[k]) * frac) >> 16);
            int l = stereo[2 * i] + ((x * ch->left) >> 15), r = stereo[2 * i + 1] + ((x * ch->right) >> 15);
            stereo[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            stereo[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            ch->pos += ch->step;
        }
    }
}
