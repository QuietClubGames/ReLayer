#include "log.h"
#include "recomp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *g_logf;

static FILE *logf_get(void) {
    if (!g_logf) {
        /* keep the previous run's log: after a crash or freeze the player restarts before anyone reads it */
        remove("fgtlayer.old.log");
        rename("fgtlayer.log", "fgtlayer.old.log");
        g_logf = fopen("fgtlayer.log", "w");
        if (!g_logf) g_logf = stderr;
    }
    return g_logf;
}

/* A runaway message (a loop logging every frame or address) must never fill the disk: the log stops at 64 MB. */
#define LOG_LIMIT (64L << 20)
static long g_log_bytes;

void rt_log(const char *fmt, ...) {
    if (g_log_bytes > LOG_LIMIT) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(logf_get(), fmt, ap);
    va_end(ap);
    fputc('\n', logf_get());
    g_log_bytes += n > 0 ? n + 1 : 1;
    if (g_log_bytes > LOG_LIMIT) fputs("log: over 64 MB, nothing more is written\n", logf_get());
    fflush(logf_get());
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", buf);
    rt_log("  ra=%08X sp=%08X a0=%08X a1=%08X a2=%08X a3=%08X v0=%08X v1=%08X",
           R(31), R(29), R(4), R(5), R(6), R(7), R(2), R(3));
    fprintf(stderr, "FATAL: %s (see fgtlayer.log)\n", buf);
    plat_fatal_message(buf);
    exit(1);
}

#define ONCE_SLOTS 4096
static uint32_t g_once[ONCE_CHANNELS][ONCE_SLOTS];
static int g_once_n[ONCE_CHANNELS];

int rt_once(int ch, uint32_t key) {
    for (int i = 0; i < g_once_n[ch]; i++)
        if (g_once[ch][i] == key) return 0;
    if (g_once_n[ch] >= ONCE_SLOTS) return 0;     /* full: a loop over many addresses is not logged forever */
    g_once[ch][g_once_n[ch]++] = key;
    return 1;
}
