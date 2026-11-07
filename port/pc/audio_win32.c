/* Windows audio out (waveOut, 16-bit stereo) plus an optional WAV recorder. */
#include "audio.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#define NBUF 8
#define BUF_FRAMES 800          /* one 60 Hz frame at 44.1 kHz is 735; rate control varies it a little */

static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static int16_t g_buf[NBUF][BUF_FRAMES * 2];
static int g_next, g_rate;

int audio_init(int rate) {
    WAVEFORMATEX f = {0};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = (DWORD)rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = (DWORD)rate * 4;
    g_rate = rate;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_wo = NULL; return 0; }
    for (int i = 0; i < NBUF; i++) {
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = BUF_FRAMES * 4;
        g_hdr[i].dwFlags = WHDR_DONE;
    }
    return 1;
}

/* Queue one frame's worth of samples (up to BUF_FRAMES). If every buffer is still playing, it is dropped. */
void audio_push(const int16_t *stereo, int frames) {
    if (!g_wo) return;
    WAVEHDR *h = &g_hdr[g_next];
    if (!(h->dwFlags & WHDR_DONE)) return;
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, sizeof *h);
    if (frames > BUF_FRAMES) frames = BUF_FRAMES;
    memcpy(g_buf[g_next], stereo, (size_t)frames * 4);
    h->dwBufferLength = (DWORD)frames * 4;
    h->dwFlags = 0;
    waveOutPrepareHeader(g_wo, h, sizeof *h);
    waveOutWrite(g_wo, h, sizeof *h);
    g_next = (g_next + 1) % NBUF;
}

/* number of buffers queued and not yet played (-1 when audio is off) */
int audio_queued(void) {
    if (!g_wo) return -1;
    int n = 0;
    for (int i = 0; i < NBUF; i++) if (!(g_hdr[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

/* ---------------------------------------------------------------- WAV recorder */
static FILE *g_wav;
static uint32_t g_wav_bytes;

int wav_open(const char *path, int rate) {
    g_wav = fopen(path, "wb");
    if (!g_wav) return 0;
    uint8_t hdr[44] = {0};
    fwrite(hdr, 1, 44, g_wav);           /* patched in wav_close */
    g_wav_bytes = 0;
    g_rate = rate;
    return 1;
}

void wav_write(const int16_t *stereo, int frames) {
    if (!g_wav) return;
    fwrite(stereo, 4, (size_t)frames, g_wav);
    g_wav_bytes += (uint32_t)frames * 4;
}

void wav_close(void) {
    if (!g_wav) return;
    uint32_t riff = 36 + g_wav_bytes, fmt = 16, rate = (uint32_t)g_rate, bps = rate * 4;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(g_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
    fwrite(&fmt, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
    fwrite(&rate, 4, 1, g_wav); fwrite(&bps, 4, 1, g_wav); fwrite(&align, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 1, 4, g_wav); fwrite(&g_wav_bytes, 4, 1, g_wav);
    fclose(g_wav);
    g_wav = NULL;
}
