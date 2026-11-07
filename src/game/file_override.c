/*
 * Loading files from the game's ROM file table (func_802224F4).
 *
 * func_802224F4(dest = a0, name = a1) looks the name up in the file table (a RAM copy at 0x803355C8, 32-byte
 * entries: name[24], address, size), copies the file from ROM to dest and returns its size, or -1 if the
 * name is unknown. Every character model, motion set, texture pack and code block comes through here.
 *
 * Replacement: normally exactly the original. Two exceptions:
 *   FGT_LOG_FILES=1          (development) log every file the game loads (name, destination, size)
 *   the extra fighter        while the extra fighter is loaded (src/game/roster.c), <folder>/<name> (e.g.
 *                            <folder>/TMD/P00_.LMD) is loaded instead of the ROM's copy when it exists. The folder
 *                            is FGT_FILE_DIR or the game's "extra" folder (extra_dir); without one, nothing
 *                            changes. Used for converted Street Fighter EX2 characters (docs/SFEX2.md).
 */
#include "recomp.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void f_802224F4_orig(void);

static int g_init, g_log;
static const char *g_dir;

static long load_replacement(const char *name, uint32_t dest) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 0x100000) { fclose(f); rt_log("file: %s is %ld bytes, ignored", path, n); return -1; }
    /* the destination may run past the end of RAM in theory; copy byte by byte through the RAM mapping */
    uint8_t *buf = malloc((size_t)n);
    long got = buf ? (long)fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got != n) { free(buf); return -1; }
    for (long i = 0; i < n; i++) MEM_W8(dest + (uint32_t)i, buf[i]);
    free(buf);
    return n;
}

/* With a replacement model TMD/Pxx_.LMD, <dir>/SKEL/Pxx.SKL (19 x s16 x,y,z) holds the extra fighter's bone
 * offsets. Fighting Layer reads them from its skeleton table, D_802E7CEC[char] = { u32 offsets; u32 hierarchy;
 * u8 nodes } (docs/FL_MODEL_FORMAT.md 3.1), when it sets up the fighter's model (func_8029C358). The offset
 * tables are shared by groups of characters, so they are written in only for that call (file_skeleton_begin /
 * _end, from src/game/roster.c) and the donor keeps his own. */
void ex_mirror_register(uint32_t dest, uint32_t size);
void ex_mirror_forget(uint32_t dest, uint32_t size);
void ex_ponytail_load(uint32_t model, const char *path);
int roster_loading_extra(void);
int roster_loading_side(void);
const char *extra_dir(void);

static struct { int valid; unsigned ch; int16_t v[19 * 3], saved[19 * 3]; uint32_t table; } g_skel[2];

void file_skeleton_begin(int side) {
    if (side < 0 || side > 1 || !g_skel[side].valid) return;
    uint32_t desc = 0x802E7CECu + 12u * g_skel[side].ch, offsets = MEM_R32(desc);
    if (MEM_R8(desc + 8) != 19 || (offsets >> 24) != 0x80) return;
    g_skel[side].table = offsets;
    for (int i = 0; i < 19 * 3; i++) {
        g_skel[side].saved[i] = (int16_t)MEM_R16(offsets + 2u * (uint32_t)i);
        MEM_W16(offsets + 2u * (uint32_t)i, (uint16_t)g_skel[side].v[i]);
    }
}

void file_skeleton_end(int side) {
    if (side < 0 || side > 1 || !g_skel[side].table) return;
    for (int i = 0; i < 19 * 3; i++) MEM_W16(g_skel[side].table + 2u * (uint32_t)i, (uint16_t)g_skel[side].saved[i]);
    g_skel[side].table = 0;
}

static void apply_skeleton(const char *name, uint32_t dest, uint32_t size) {
    unsigned ch;
    if (strncmp(name, "TMD/P", 5) || sscanf(name + 5, "%2x_.LMD", &ch) != 1) return;
    char path[512];
    snprintf(path, sizeof path, "%s/SKEL/P%02X.SKL", g_dir, ch);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    int16_t v[19 * 3];
    size_t n = fread(v, sizeof v[0], 19 * 3, f);
    fclose(f);
    if (n != 19 * 3) { rt_log("file: %s is too short, ignored", path); return; }
    int side = roster_loading_side();
    if (side >= 0) {
        g_skel[side].valid = 1;
        g_skel[side].ch = ch;
        memcpy(g_skel[side].v, v, sizeof v);
    }
    /* a converted character: mirror it the EX2 way (src/game/ex_mirror.c), and a ponytail if it has one */
    ex_mirror_register(dest, size);
    snprintf(path, sizeof path, "%s/PONY/P%02X.PNY", g_dir, ch);
    ex_ponytail_load(dest, path);
    rt_log("file: extra fighter's model for side %d at %08X", side, dest);
}

void f_802224F4(void) {
    if (!g_init) {
        g_init = 1;
        g_log = getenv("FGT_LOG_FILES") != NULL;
        g_dir = extra_dir();
        if (g_dir) rt_log("file: replacements from %s", g_dir);
    }
    if (!g_log && !g_dir) { f_802224F4_orig(); return; }

    char name[25];
    for (int i = 0; i < 24; i++) name[i] = (char)MEM_R8(R(5) + (uint32_t)i);
    name[24] = 0;
    uint32_t dest = R(4);
    if (g_dir && roster_loading_extra()) {
        long n = load_replacement(name, dest);
        if (n >= 0) {
            if (g_log) rt_log("file: %s -> %08X (%ld bytes, REPLACED)", name, dest, n);
            ex_mirror_forget(dest, (uint32_t)n);
            apply_skeleton(name, dest, (uint32_t)n);
            R(2) = (uint32_t)n;
            return;
        }
    }
    f_802224F4_orig();
    if (g_log) rt_log("file: %s -> %08X (%d bytes)", name, dest, (int32_t)R(2));
    if (g_dir && (int32_t)R(2) > 0) ex_mirror_forget(dest, R(2));   /* a converted model there is gone */
}
