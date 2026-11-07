#!/usr/bin/env python3
"""
Fighting Layer (Arika/Namco 1998, Namco System 12) ROM extractor.

Reads the MAME merged set (fgtlayer.zip), verifies CRCs, and writes:

  baserom/prg.bin          4 MB main program ROM (ftl3vera.2e/.2j interleaved), mapped at 0x1FC00000
  baserom/main.bin         game executable image, copied to RAM 0x801F0000 at boot
  baserom/ovl/PLxx_n.ovl   per-character code overlays (slot 1 -> 0x80090000, slot 2 -> 0x800A0000)
  baserom/banked.bin       32 MB banked data ROM (rom0, rom1, rom2, then flash fl3/fl4)
  baserom/sound.bin        H8/3002 sound CPU program (ftl1vera.11s, big-endian)
  baserom/wave.bin         C352 sample ROM (wave0 + wave1)
  assets/<DIR>/<FILE>      every file in the game's ROM file table
  baserom/filetable.tsv    name / raw offset / size / source

Usage:  python tools/extract_rom.py [path/to/fgtlayer.zip]
"""
import os
import struct
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (zip member, crc32) for the world/"VerA" set
FILES = {
    "prg_e":  ("ftl3vera.2e",  0x17C23471),
    "prg_j":  ("ftl3vera.2j",  0x1626465D),
    "fl3l":   ("ftl1fl3l.7",   0xE4FB01E1),
    "fl3h":   ("ftl1fl3h.8",   0x67A5C56F),
    "fl4l":   ("ftl1fl4l.5",   0x8039242C),
    "fl4h":   ("ftl1fl4h.6",   0x5AD59726),
    "rom0":   ("ftl1rom0.9",   0xE33CE365),
    "rom1":   ("ftl1rom1.10",  0xA1EC7D08),
    "rom2":   ("ftl1rom2.11",  0x204BE858),
    "sound":  ("ftl1vera.11s", 0xE3F957CD),
    "wave0":  ("ftl1wave0.2",  0xEE009A2B),
    "wave1":  ("ftl1wave1.1",  0xA54A89CD),
}

PRG_GAME_HEADER = 0x24000      # loader descriptor in prg ROM
FILETABLE_OFS = 0x180000       # 32-byte entries: name[24], u32 offset, u32 size
OVL_FLAG = 0x80000000          # file-table offset flag: data lives in prg ROM, not banked ROM
BANK_FLASH = 0x1800000         # banked offsets >= this are in the flash chips


def interleave16(lo: bytes, hi: bytes) -> bytes:
    out = bytearray(len(lo) * 2)
    out[0::2] = lo
    out[1::2] = hi
    return bytes(out)


def read_rom_set(zpath):
    """The twelve ROM chips, found by name or, failing that, by CRC (other ROM managers name files
    differently). Exits with a message a player can act on when the set is wrong or incomplete."""
    try:
        z = zipfile.ZipFile(zpath)
    except FileNotFoundError:
        sys.exit(f"ROM error: {zpath} does not exist.")
    except zipfile.BadZipFile:
        sys.exit(f"ROM error: {os.path.basename(zpath)} is not a zip file (or is damaged).")
    by_name = {os.path.basename(i.filename).lower(): i for i in z.infolist() if not i.is_dir()}
    by_crc = {}
    for i in z.infolist():
        by_crc.setdefault(i.CRC, i)
    raw, missing, wrong = {}, [], []
    for key, (name, crc) in FILES.items():
        info = by_crc.get(crc)
        if info is None:
            if name.lower() in by_name:
                wrong.append(name)
            else:
                missing.append(name)
            continue
        data = z.read(info)
        if zlib.crc32(data) != crc:
            wrong.append(name)
            continue
        raw[key] = data
    if missing or wrong:
        msg = ["ROM error: this is not the Fighting Layer (World, Ver. A) set this port needs "
               "(MAME: fgtlayer)."]
        if missing:
            msg.append("Missing: " + ", ".join(missing) + ".")
        if wrong:
            msg.append("A different version or a bad dump: " + ", ".join(wrong) + ".")
        sys.exit(" ".join(msg))
    return raw


def write(path, data):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)


def main():
    zpath = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "fgtlayer.zip")
    raw = read_rom_set(zpath)
    print("All ROM CRCs OK")

    prg = interleave16(raw["prg_e"], raw["prg_j"])
    flash = interleave16(raw["fl3l"], raw["fl3h"]) + interleave16(raw["fl4l"], raw["fl4h"])
    banked = raw["rom0"] + raw["rom1"] + raw["rom2"] + flash
    write("baserom/prg.bin", prg)
    write("baserom/banked.bin", banked)
    write("baserom/sound.bin", raw["sound"])
    write("baserom/wave.bin", raw["wave0"] + raw["wave1"])

    # Game image descriptor at 0x24000: [?, ?, ?, ?, src_rom_addr, dst_ram, size, entry]
    hdr = struct.unpack_from("<8I", prg, PRG_GAME_HEADER)
    src, dst, size = hdr[4] - 0x1FC00000, hdr[5], hdr[6]
    end = src + size
    while prg[end - 1] == 0xFF:
        end -= 1
    end = (end + 3) & ~3
    main_bin = prg[src:end]
    write("baserom/main.bin", main_bin)
    print(f"main.bin: prg+{src:#x} -> RAM {dst:#010x}, {len(main_bin):#x} bytes (image size {size:#x})")

    # File table
    rows = []
    o = FILETABLE_OFS
    while True:
        name = prg[o:o + 24].split(b"\0")[0]
        if not name or not (0x20 <= name[0] < 0x7F):
            break
        off, sz = struct.unpack_from("<II", prg, o + 24)
        name = name.decode("ascii")
        if off & OVL_FLAG:
            src_name, blob = "prg", prg[off & ~OVL_FLAG:(off & ~OVL_FLAG) + sz]
        else:
            src_name = "flash" if off >= BANK_FLASH else "mask"
            blob = banked[off:off + sz]
        rows.append(f"{name}\t{off:08x}\t{sz:08x}\t{src_name}")
        write(os.path.join("assets", *name.split("/")), blob)
        if name.endswith(".OVL"):
            write(os.path.join("baserom", "ovl", os.path.basename(name).lower()), blob)
        o += 32
    write("baserom/filetable.tsv", ("\n".join(rows) + "\n").encode())
    print(f"Extracted {len(rows)} files from the ROM file table")


if __name__ == "__main__":
    main()
