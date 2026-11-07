#!/usr/bin/env python3
"""Minimal H8/300H disassembler for the System 12 sound program (debugging aid).

Usage: python tools/h8dis.py START [COUNT]     (START in hex; reads baserom/sound.bin, word-swapped)
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
raw = open(os.path.join(ROOT, "baserom", "sound.bin"), "rb").read()
rom = bytearray(len(raw))
rom[0::2] = raw[1::2]
rom[1::2] = raw[0::2]

RB = [f"r{i}h" for i in range(8)] + [f"r{i}l" for i in range(8)]
RW = [f"r{i}" for i in range(8)] + [f"e{i}" for i in range(8)]
ER = [f"er{i}" for i in range(8)]
CC = ["ra", "rn", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SH = {0x10: ("shll", "shal"), 0x11: ("shlr", "shar"), 0x12: ("rotxl", "rotl"), 0x13: ("rotxr", "rotr")}
BIT = {0x60: "bset", 0x61: "bnot", 0x62: "bclr", 0x63: "btst", 0x67: "bst", 0x70: "bset", 0x71: "bnot",
       0x72: "bclr", 0x73: "btst", 0x74: "bor", 0x75: "bxor", 0x76: "band", 0x77: "bld"}
IBIT = {0x67: "bist", 0x74: "bior", 0x75: "bixor", 0x76: "biand", 0x77: "bild"}


def w(a):
    return (rom[a] << 8) | rom[a + 1]


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    return v - 0x100 if v & 0x80 else v


def a16(v):
    return v | 0xFF0000 if v & 0x8000 else v


def dis(pc):
    op = w(pc)
    hi, lo = op >> 8, op & 0xFF
    a4, b4 = lo >> 4, lo & 15
    n = 2
    t = f".word {op:04x}"
    if op == 0:
        t = "nop"
    elif hi == 0x01:
        w2 = w(pc + 2); n = 4; h2, l2 = w2 >> 8, w2 & 0xFF
        if lo == 0x80: t = "sleep"; n = 2
        elif lo in (0x00, 0x40):
            sz = ".l" if lo == 0 else ".w"
            r = ER[l2 & 7] if lo == 0 else "ccr"
            st = l2 & 0x80
            p = ER[(l2 >> 4) & 7]
            if h2 == 0x69: m = f"@{p}"
            elif h2 == 0x6B:
                w3 = w(pc + 4); n = 6
                if (l2 & 0x70) == 0x20: m = f"@0x{((w3 & 0xff) << 16) | w(pc + 6):06x}"; n = 8
                else: m = f"@0x{a16(w3):06x}"
                r = ER[l2 & 7] if lo == 0 else "ccr"
            elif h2 == 0x6D: m = f"@-{p}" if st else f"@{p}+"
            elif h2 == 0x6F: m = f"@({s16(w(pc + 4))},{p})"; n = 6
            elif h2 == 0x78:
                w3 = w(pc + 4); st = w3 & 0x80; r = ER[w3 & 7] if lo == 0 else "ccr"
                m = f"@(0x{((w(pc + 6) & 0xff) << 16) | w(pc + 8):x},{p})"; n = 10
            else: m = "?"
            mn = "mov" if lo == 0 else ("stc" if st else "ldc")
            t = f"{mn}{sz} {r},{m}" if st else f"{mn}{sz} {m},{r}"
        elif lo == 0xF0: t = f"{ {0x64: 'or', 0x65: 'xor', 0x66: 'and'}.get(h2, '?')}.l {ER[(l2 >> 4) & 7]},{ER[l2 & 7]}"
        elif lo == 0xC0: t = f"mulxs {w2:04x}"
        elif lo == 0xD0: t = f"divxs {w2:04x}"
    elif hi == 0x02: t = f"stc ccr,{RB[b4]}"
    elif hi == 0x03: t = f"ldc {RB[b4]},ccr"
    elif hi in (4, 5, 6, 7): t = f"{['orc', 'xorc', 'andc', 'ldc'][hi - 4]} #0x{lo:02x},ccr"
    elif hi == 0x08: t = f"add.b {RB[a4]},{RB[b4]}"
    elif hi == 0x09: t = f"add.w {RW[a4]},{RW[b4]}"
    elif hi == 0x0A: t = f"inc.b {RB[b4]}" if a4 == 0 else f"add.l {ER[a4 & 7]},{ER[b4 & 7]}"
    elif hi == 0x0B: t = {0: f"adds #1,{ER[b4 & 7]}", 8: f"adds #2,{ER[b4 & 7]}", 9: f"adds #4,{ER[b4 & 7]}",
                          5: f"inc.w #1,{RW[b4]}", 0xD: f"inc.w #2,{RW[b4]}", 7: f"inc.l #1,{ER[b4 & 7]}",
                          0xF: f"inc.l #2,{ER[b4 & 7]}"}.get(a4, "?")
    elif hi == 0x0C: t = f"mov.b {RB[a4]},{RB[b4]}"
    elif hi == 0x0D: t = f"mov.w {RW[a4]},{RW[b4]}"
    elif hi == 0x0E: t = f"addx {RB[a4]},{RB[b4]}"
    elif hi == 0x0F: t = f"daa {RB[b4]}" if a4 == 0 else f"mov.l {ER[a4 & 7]},{ER[b4 & 7]}"
    elif hi in SH:
        sz = {0: ".b", 1: ".w", 3: ".l"}.get(a4 & 7, "?")
        reg = RB[b4] if (a4 & 7) == 0 else RW[b4] if (a4 & 7) == 1 else ER[b4 & 7]
        t = f"{SH[hi][1 if a4 & 8 else 0]}{sz} {reg}"
    elif hi in (0x14, 0x15, 0x16): t = f"{['or', 'xor', 'and'][hi - 0x14]}.b {RB[a4]},{RB[b4]}"
    elif hi == 0x17: t = {0: f"not.b {RB[b4]}", 1: f"not.w {RW[b4]}", 3: f"not.l {ER[b4 & 7]}", 5: f"extu.w {RW[b4]}",
                          7: f"extu.l {ER[b4 & 7]}", 8: f"neg.b {RB[b4]}", 9: f"neg.w {RW[b4]}", 0xB: f"neg.l {ER[b4 & 7]}",
                          0xD: f"exts.w {RW[b4]}", 0xF: f"exts.l {ER[b4 & 7]}"}.get(a4, "?")
    elif hi == 0x18: t = f"sub.b {RB[a4]},{RB[b4]}"
    elif hi == 0x19: t = f"sub.w {RW[a4]},{RW[b4]}"
    elif hi == 0x1A: t = f"dec.b {RB[b4]}" if a4 == 0 else f"sub.l {ER[a4 & 7]},{ER[b4 & 7]}"
    elif hi == 0x1B: t = {0: f"subs #1,{ER[b4 & 7]}", 8: f"subs #2,{ER[b4 & 7]}", 9: f"subs #4,{ER[b4 & 7]}",
                          5: f"dec.w #1,{RW[b4]}", 0xD: f"dec.w #2,{RW[b4]}", 7: f"dec.l #1,{ER[b4 & 7]}",
                          0xF: f"dec.l #2,{ER[b4 & 7]}"}.get(a4, "?")
    elif hi == 0x1C: t = f"cmp.b {RB[a4]},{RB[b4]}"
    elif hi == 0x1D: t = f"cmp.w {RW[a4]},{RW[b4]}"
    elif hi == 0x1E: t = f"subx {RB[a4]},{RB[b4]}"
    elif hi == 0x1F: t = f"das {RB[b4]}" if a4 == 0 else f"cmp.l {ER[a4 & 7]},{ER[b4 & 7]}"
    elif 0x20 <= hi < 0x30: t = f"mov.b @0xffff{lo:02x},{RB[hi & 15]}"
    elif 0x30 <= hi < 0x40: t = f"mov.b {RB[hi & 15]},@0xffff{lo:02x}"
    elif 0x40 <= hi < 0x50: t = f"b{CC[hi & 15]} 0x{pc + 2 + s8(lo):06x}"
    elif hi == 0x50: t = f"mulxu.b {RB[a4]},{RW[b4]}"
    elif hi == 0x51: t = f"divxu.b {RB[a4]},{RW[b4]}"
    elif hi == 0x52: t = f"mulxu.w {RW[a4]},{ER[b4 & 7]}"
    elif hi == 0x53: t = f"divxu.w {RW[a4]},{ER[b4 & 7]}"
    elif hi == 0x54: t = "rts"
    elif hi == 0x55: t = f"bsr 0x{pc + 2 + s8(lo):06x}"
    elif hi == 0x56: t = "rte"
    elif hi == 0x57: t = f"trapa #{(lo >> 4) & 3}"
    elif hi == 0x58: n = 4; t = f"b{CC[a4]} 0x{(pc + 4 + s16(w(pc + 2))) & 0xffffff:06x}"
    elif hi == 0x59: t = f"jmp @{ER[a4 & 7]}"
    elif hi == 0x5A: n = 4; t = f"jmp 0x{(lo << 16) | w(pc + 2):06x}"
    elif hi == 0x5B: t = f"jmp @@0x{lo:02x}"
    elif hi == 0x5C: n = 4; t = f"bsr 0x{(pc + 4 + s16(w(pc + 2))) & 0xffffff:06x}"
    elif hi == 0x5D: t = f"jsr @{ER[a4 & 7]}"
    elif hi == 0x5E: n = 4; t = f"jsr 0x{(lo << 16) | w(pc + 2):06x}"
    elif hi == 0x5F: t = f"jsr @@0x{lo:02x}"
    elif hi in (0x60, 0x61, 0x62, 0x63): t = f"{BIT[hi]} {RB[a4]},{RB[b4]}"
    elif hi in (0x64, 0x65, 0x66): t = f"{['or', 'xor', 'and'][hi - 0x64]}.w {RW[a4]},{RW[b4]}"
    elif hi == 0x67: t = f"{IBIT[0x67] if a4 & 8 else 'bst'} #{a4 & 7},{RB[b4]}"
    elif 0x68 <= hi <= 0x6F:
        word = hi & 1; R = RW if word else RB; sz = ".w" if word else ".b"; st = a4 & 8; p = ER[a4 & 7]
        if hi in (0x68, 0x69): m = f"@{p}"
        elif hi in (0x6A, 0x6B):
            w2 = w(pc + 2); n = 4
            if (a4 & 7) == 2: m = f"@0x{((w2 & 0xff) << 16) | w(pc + 4):06x}"; n = 6
            else: m = f"@0x{a16(w2):06x}"
        elif hi in (0x6C, 0x6D): m = f"@-{p}" if st else f"@{p}+"
        else: m = f"@({s16(w(pc + 2))},{p})"; n = 4
        t = f"mov{sz} {R[b4]},{m}" if st else f"mov{sz} {m},{R[b4]}"
    elif 0x70 <= hi <= 0x77:
        mn = IBIT[hi] if (a4 & 8) and hi in IBIT else BIT[hi]
        t = f"{mn} #{a4 & 7},{RB[b4]}"
    elif hi == 0x78:
        w2 = w(pc + 2); n = 8; R = RW if (w2 >> 8) == 0x6B else RB; st = w2 & 0x80
        m = f"@(0x{((w(pc + 4) & 0xff) << 16) | w(pc + 6):x},{ER[a4 & 7]})"
        t = f"mov {R[w2 & 15]},{m}" if st else f"mov {m},{R[w2 & 15]}"
    elif hi == 0x79:
        n = 4; t = f"{['mov', 'add', 'cmp', 'sub', 'or', 'xor', 'and', '?'][a4 & 7]}.w #0x{w(pc + 2):04x},{RW[b4]}"
    elif hi == 0x7A:
        n = 6; t = f"{['mov', 'add', 'cmp', 'sub', 'or', 'xor', 'and', '?'][a4 & 7]}.l #0x{(w(pc + 2) << 16) | w(pc + 4):08x},{ER[b4 & 7]}"
    elif hi == 0x7B: n = 4; t = "eepmov.b" if lo == 0x5C else "eepmov.w"
    elif 0x7C <= hi <= 0x7F:
        w2 = w(pc + 2); n = 4; bop = w2 >> 8; bn = (w2 >> 4) & 15
        m = f"@{ER[a4 & 7]}" if hi <= 0x7D else f"@0xffff{lo:02x}"
        mn = IBIT.get(bop) if (bn & 8 and bop in IBIT) else BIT.get(bop, "?")
        src = RB[bn] if 0x60 <= bop <= 0x63 else f"#{bn & 7}"
        t = f"{mn} {src},{m}"
    else:
        names = {8: "add.b", 9: "addx", 0xA: "cmp.b", 0xB: "subx", 0xC: "or.b", 0xD: "xor.b", 0xE: "and.b", 0xF: "mov.b"}
        t = f"{names[hi >> 4]} #0x{lo:02x},{RB[hi & 15]}" if (hi >> 4) in names else f".word {op:04x}"
    return n, t


def main():
    pc = int(sys.argv[1], 16)
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    for _ in range(count):
        n, t = dis(pc)
        raw_bytes = " ".join(f"{rom[pc + i]:02x}" for i in range(n))
        print(f"{pc:06x}: {raw_bytes:<30} {t}")
        pc += n


if __name__ == "__main__":
    main()
