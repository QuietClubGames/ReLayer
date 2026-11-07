#!/usr/bin/env python3
"""
Static recompiler: MIPS R3000A (PS1 / Namco System 12) machine code -> C.

Each MIPS function becomes one C function that operates on the global CPU state (`cpu.r[]`, hi/lo)
and the emulated 4 MB RAM through the inline memory helpers in port/core/recomp.h.

Inputs (all generated from your ROM, see README):
  baserom/main.bin, baserom/ovl/*.ovl         code images
  asm/main/**.s, asm/ovl/**.s                  splat/spimdisasm output (function sizes, jump tables)
  config/recomp_extra_funcs.txt                extra entry points found at runtime (optional)

Output:  build/recomp/*.c, build/recomp/recomp_funcs.h

Usage:  python tools/recomp/recomp.py
"""
import glob
import os
import re
import struct
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "recomp")

MAIN_VRAM = 0x801F0000
OVL_VRAM = (0x8009E000, 0x800AE000)     # slot 1, slot 2
OVL_SLOT_SIZE = 0x10000
FUNCS_PER_FILE = 120

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


# --------------------------------------------------------------------------------------------
# asm listing parsing
# --------------------------------------------------------------------------------------------
RE_FUNC = re.compile(r"^nonmatching (func_\w+|\w+), (0x[0-9A-Fa-f]+)\s*$")
RE_INSN = re.compile(r"^\s*/\* ([0-9A-F]+) ([0-9A-F]{8}) ([0-9A-F]{8}) \*/")
RE_JTBL = re.compile(r"^dlabel (jtbl_[0-9A-F]{8})")
RE_ALABEL = re.compile(r"^\s*alabel \w+_([0-9A-F]{8})")
ALABELS = set()      # addresses spimdisasm saw referenced from code (candidate function starts)
RE_WORD = re.compile(r"^\s*/\* [0-9A-F]+ [0-9A-F]{8} [0-9A-F]{8} \*/ \.word (0x[0-9A-F]{8}|\.L[0-9A-F]{8}|\w+)")


def parse_asm(paths):
    """Return (functions, jumptables).
    functions: list of (name, rom_offset, vram, size)   rom_offset is into the segment's binary
    jumptables: list of (rom_offset, [target words])"""
    funcs, jtbls = [], []
    for p in paths:
        lines = open(p, encoding="utf-8").read().splitlines()
        i = 0
        while i < len(lines):
            ma = RE_ALABEL.match(lines[i])
            if ma:
                ALABELS.add(int(ma.group(1), 16))
            m = RE_FUNC.match(lines[i])
            if m and any(l.startswith("glabel") for l in lines[i + 1:i + 3]):
                name, size = m.group(1), int(m.group(2), 16)
                j = i + 1
                while not RE_INSN.match(lines[j]):
                    j += 1
                mi = RE_INSN.match(lines[j])
                funcs.append((name, int(mi.group(1), 16), int(mi.group(2), 16), size))
                i = j
                continue
            m = RE_JTBL.match(lines[i])
            if m:
                words = []
                mo = RE_INSN.match(lines[i + 1]) or RE_INSN.match(lines[i + 2])
                jt_ofs = int(mo.group(1), 16) if mo else -1
                j = i + 1
                while j < len(lines) and not lines[j].startswith("enddlabel"):
                    w = RE_WORD.match(lines[j])
                    if w:
                        v = w.group(1)
                        if v.startswith("0x"):
                            words.append(int(v, 16))
                        elif v.startswith(".L"):
                            words.append(int(v[2:], 16))
                    j += 1
                jtbls.append((jt_ofs, words))
                i = j
                continue
            i += 1
    return funcs, jtbls


# --------------------------------------------------------------------------------------------
# instruction decoding
# --------------------------------------------------------------------------------------------
class Insn:
    __slots__ = ("addr", "w", "op", "rs", "rt", "rd", "sa", "fn", "imm", "simm", "target")

    def __init__(self, addr, w):
        self.addr, self.w = addr, w
        self.op = w >> 26
        self.rs = (w >> 21) & 31
        self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31
        self.sa = (w >> 6) & 31
        self.fn = w & 63
        self.imm = w & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)

    # control-flow classification
    def kind(self):
        op, fn, rt = self.op, self.fn, self.rt
        if op == 0 and fn == 8:
            return "jr"
        if op == 0 and fn == 9:
            return "jalr"
        if op == 2:
            return "j"
        if op == 3:
            return "jal"
        if op in (4, 5, 6, 7):
            return "branch"
        if op == 1 and rt in (0, 1, 0x10, 0x11):
            return "branch"
        return None

    def branch_target(self):
        return (self.addr + 4 + (self.simm << 2)) & 0xFFFFFFFF


def hx(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


def r(n):
    return "0u" if n == 0 else f"R({n})"


class FuncGen:
    """Generate C for one function (possibly with several entry points)."""

    def __init__(self, unit, name, start, size, code, code_base):
        self.unit, self.name, self.start, self.size = unit, name, start, size
        self.end = start + size
        self.code, self.code_base = code, code_base
        self.insns = [Insn(a, self.word(a)) for a in range(start, self.end, 4)]
        self.labels = set()
        self.jt_targets = sorted(t for t in unit.jt_targets if start <= t < self.end)
        self.labels.update(self.jt_targets)
        for ins in self.insns:
            k = ins.kind()
            if k == "branch" and start <= ins.branch_target() < self.end:
                self.labels.add(ins.branch_target())
            elif k == "j" and start <= ins.target < self.end:
                self.labels.add(ins.target)
        self.out = []

    def word(self, a):
        o = a - self.code_base
        if 0 <= o <= len(self.code) - 4:
            return struct.unpack_from("<I", self.code, o)[0]
        return 0

    def emit(self, s):
        self.out.append(s)

    # ---- single non-control instruction -> C statement ----
    def simple(self, ins):
        op, rs, rt, rd, sa, fn = ins.op, ins.rs, ins.rt, ins.rd, ins.sa, ins.fn
        imm, simm = ins.imm, ins.simm
        a = ins.addr

        def setr(dst, expr):
            if dst == 0:
                return f"(void)({expr});" if any(x in expr for x in ("MEM_", "gte_", "cop0_")) else "/* nop */"
            return f"R({dst}) = {expr};"

        if ins.w == 0:
            return "/* nop */"
        if op == 0:
            if fn == 0:  return setr(rd, f"{r(rt)} << {sa}")
            if fn == 2:  return setr(rd, f"{r(rt)} >> {sa}")
            if fn == 3:  return setr(rd, f"(uint32_t)((int32_t){r(rt)} >> {sa})")
            if fn == 4:  return setr(rd, f"{r(rt)} << ({r(rs)} & 31)")
            if fn == 6:  return setr(rd, f"{r(rt)} >> ({r(rs)} & 31)")
            if fn == 7:  return setr(rd, f"(uint32_t)((int32_t){r(rt)} >> ({r(rs)} & 31))")
            if fn == 12: return f"cpu_syscall({hx(a)});"
            if fn == 13: return f"cpu_break({hx(a)}, {(ins.w >> 6) & 0xFFFFF});"
            if fn == 16: return setr(rd, "cpu.hi")
            if fn == 17: return f"cpu.hi = {r(rs)};"
            if fn == 18: return setr(rd, "cpu.lo")
            if fn == 19: return f"cpu.lo = {r(rs)};"
            if fn == 24: return f"cpu_mult({r(rs)}, {r(rt)});"
            if fn == 25: return f"cpu_multu({r(rs)}, {r(rt)});"
            if fn == 26: return f"cpu_div({r(rs)}, {r(rt)});"
            if fn == 27: return f"cpu_divu({r(rs)}, {r(rt)});"
            if fn in (32, 33): return setr(rd, f"{r(rs)} + {r(rt)}")
            if fn in (34, 35): return setr(rd, f"{r(rs)} - {r(rt)}")
            if fn == 36: return setr(rd, f"{r(rs)} & {r(rt)}")
            if fn == 37: return setr(rd, f"{r(rs)} | {r(rt)}")
            if fn == 38: return setr(rd, f"{r(rs)} ^ {r(rt)}")
            if fn == 39: return setr(rd, f"~({r(rs)} | {r(rt)})")
            if fn == 42: return setr(rd, f"(uint32_t)((int32_t){r(rs)} < (int32_t){r(rt)})")
            if fn == 43: return setr(rd, f"(uint32_t)({r(rs)} < {r(rt)})")
        if op in (8, 9):  return setr(rt, f"{r(rs)} + {hx(simm)}")
        if op == 10: return setr(rt, f"(uint32_t)((int32_t){r(rs)} < {simm})")
        if op == 11: return setr(rt, f"(uint32_t)({r(rs)} < {hx(simm)})")
        if op == 12: return setr(rt, f"{r(rs)} & 0x{imm:X}u")
        if op == 13: return setr(rt, f"{r(rs)} | 0x{imm:X}u")
        if op == 14: return setr(rt, f"{r(rs)} ^ 0x{imm:X}u")
        if op == 15: return setr(rt, hx(imm << 16))
        ea = f"{r(rs)} + {hx(simm)}"
        if op == 32: return setr(rt, f"(uint32_t)(int32_t)(int8_t)MEM_R8({ea})")
        if op == 33: return setr(rt, f"(uint32_t)(int32_t)(int16_t)MEM_R16({ea})")
        if op == 34: return setr(rt, f"MEM_LWL({ea}, {r(rt)})")
        if op == 35: return setr(rt, f"MEM_R32({ea})")
        if op == 36: return setr(rt, f"(uint32_t)MEM_R8({ea})")
        if op == 37: return setr(rt, f"(uint32_t)MEM_R16({ea})")
        if op == 38: return setr(rt, f"MEM_LWR({ea}, {r(rt)})")
        if op == 40: return f"MEM_W8({ea}, (uint8_t){r(rt)});"
        if op == 41: return f"MEM_W16({ea}, (uint16_t){r(rt)});"
        if op == 42: return f"MEM_SWL({ea}, {r(rt)});"
        if op == 43: return f"MEM_W32({ea}, {r(rt)});"
        if op == 46: return f"MEM_SWR({ea}, {r(rt)});"
        if op == 50: return f"gte_mtc2({rt}, MEM_R32({ea}));"          # lwc2
        if op == 58: return f"MEM_W32({ea}, gte_mfc2({rt}));"          # swc2
        if op == 16:  # cop0
            if rs == 0:  return setr(rt, f"cop0_read({rd})")
            if rs == 4:  return f"cop0_write({rd}, {r(rt)});"
            if rs == 16 and fn == 16: return "cop0_rfe();"
        if op == 18:  # cop2 (GTE)
            if rs == 0:  return setr(rt, f"gte_mfc2({rd})")
            if rs == 2:  return setr(rt, f"gte_cfc2({rd})")
            if rs == 4:  return f"gte_mtc2({rd}, {r(rt)});"
            if rs == 6:  return f"gte_ctc2({rd}, {r(rt)});"
            if rs & 16:  return f"gte_op(0x{ins.w & 0x1FFFFFF:07X}u);"
        return f"cpu_unimpl({hx(a)}, {hx(ins.w)}); return;"

    def call(self, target):
        """C statement(s) calling a fixed address."""
        return self.unit.call_expr(target)

    def gen(self, entry=None):
        name = self.name if entry is None else self.unit.fname(entry)
        self.out = []
        self.emit(f"void {name}(void) {{")
        if entry is not None and entry != self.start:
            self.labels.add(entry)
            self.emit(f"    goto L_{entry:08X};")
        insns = self.insns
        i = 0
        n = len(insns)
        while i < n:
            ins = insns[i]
            if ins.addr in self.labels:
                self.emit(f"L_{ins.addr:08X}:;")
            k = ins.kind()
            if k is None:
                self.emit(f"    {self.simple(ins)}")
                i += 1
                continue
            delay = insns[i + 1] if i + 1 < n else Insn(ins.addr + 4, self.word(ins.addr + 4))
            ds = self.simple(delay) if delay.kind() is None else f"cpu_unimpl({hx(delay.addr)}, {hx(delay.w)}); /* branch in delay slot */"
            nxt = ins.addr + 8
            if k == "branch":
                cond = self.branch_cond(ins)
                link = ins.op == 1 and ins.rt in (0x10, 0x11)
                tgt = ins.branch_target()
                self.emit(f"    {{ int c = {cond};")
                if link:
                    self.emit(f"      R(31) = {hx(nxt)};")
                self.emit(f"      {ds}")
                if link:
                    self.emit(f"      if (c) {{ {self.call(tgt)} }} }}")
                elif self.start <= tgt < self.end:
                    back = f"CHECK_EVENTS(0x{ins.addr:08X}u); " if tgt <= ins.addr else ""
                    self.emit(f"      if (c) {{ {back}goto L_{tgt:08X}; }} }}")
                else:
                    self.emit(f"      if (c) {{ {self.call(tgt)} return; }} }}")
            elif k == "j":
                tgt = ins.target
                self.emit(f"    {ds}")
                if self.start <= tgt < self.end:
                    back = f"CHECK_EVENTS(0x{ins.addr:08X}u); " if tgt <= ins.addr else ""
                    self.emit(f"    {back}goto L_{tgt:08X};")
                else:
                    self.emit(f"    {self.call(tgt)} return;")
            elif k == "jal":
                self.emit(f"    R(31) = {hx(nxt)};")
                self.emit(f"    {ds}")
                self.emit(f"    {self.call(ins.target)}")
            elif k == "jr":
                if ins.rs == 31:
                    self.emit(f"    {ds}")
                    self.emit("    return;")
                else:
                    self.emit(f"    {{ uint32_t t = {r(ins.rs)};")
                    self.emit(f"      {ds}")
                    if self.jt_targets:
                        self.emit("      switch (t) {")
                        for t in self.jt_targets:
                            self.emit(f"        case {hx(t)}: goto L_{t:08X};")
                        self.emit("      }")
                    self.emit("      call_indirect(t); return; }")
            elif k == "jalr":
                self.emit(f"    {{ uint32_t t = {r(ins.rs)};")
                if ins.rd:
                    self.emit(f"      R({ins.rd}) = {hx(nxt)};")
                self.emit(f"      {ds}")
                self.emit("      call_indirect(t); }")
            # the delay slot is consumed; if something branches into it, emit it standalone
            if delay.addr in self.labels and i + 1 < n:
                if nxt < self.end:
                    self.labels.add(nxt)
                    self.emit(f"    goto L_{nxt:08X};")
                else:
                    self.emit(f"    {self.call(self.end)} return;" if k not in ("j", "jr") else "    return;")
                self.emit(f"L_{delay.addr:08X}:;")
                self.emit(f"    {ds}")
            i += 2
        # fell off the end of the function
        last = insns[-2].kind() if n >= 2 else None
        if last not in ("j", "jr"):
            self.emit(f"    {self.call(self.end)} return;")
        if self.end in self.labels:
            self.emit(f"L_{self.end:08X}:; {self.call(self.end)} return;")
        self.emit("}")
        return "\n".join(self.out)

    def branch_cond(self, ins):
        op, rs, rt = ins.op, ins.rs, ins.rt
        if op == 4: return f"{r(rs)} == {r(rt)}"
        if op == 5: return f"{r(rs)} != {r(rt)}"
        if op == 6: return f"(int32_t){r(rs)} <= 0"
        if op == 7: return f"(int32_t){r(rs)} > 0"
        if rt in (0, 0x10): return f"(int32_t){r(rs)} < 0"
        return f"(int32_t){r(rs)} >= 0"


def split_merged(funcs, code, vram):
    """spimdisasm sometimes merges a function that is only reached through a pointer into the one before
    it. Split after `jr ra` + delay slot when the next word opens a stack frame and nothing earlier in
    the function branches past that point."""
    out = []
    W = lambda a: struct.unpack_from("<I", code, a - vram)[0]
    for name, rom, start, size in funcs:
        end = start + size
        cur, far = start, start
        a = start
        while a < end:
            w = W(a)
            ins = Insn(a, w)
            k = ins.kind()
            if k == "branch":
                far = max(far, ins.branch_target())
            elif k == "j" and start <= ins.target < end:
                far = max(far, ins.target)
            if w == 0x03E00008 and a + 8 < end:
                ahead = [W(a + 8 + 4 * k) for k in range(6) if a + 8 + 4 * k < end]
                opens_frame = any((x >> 16) == 0x27BD and x & 0x8000 for x in ahead)
                if (opens_frame or (a + 8) in ALABELS) and far <= a + 4:
                    out.append((name if cur == start else f"func_{cur:08X}", rom + (cur - start), cur, a + 8 - cur))
                    cur = a + 8
                    far = cur
            a += 4
        out.append((name if cur == start else f"func_{cur:08X}", rom + (cur - start), cur, end - cur))
    return out


class Unit:
    """A code image: the main executable or one overlay variant."""

    def __init__(self, tag, code, vram, funcs, jtbls, prefix):
        self.tag, self.code, self.vram, self.prefix = tag, code, vram, prefix
        self.end = vram + len(code)
        n0 = len(funcs)
        funcs = split_merged(funcs, code, vram)
        self.n_split = len(funcs) - n0
        self.funcs = sorted({f[2]: f for f in funcs}.values(), key=lambda f: f[2])
        self.func_starts = {f[2] for f in self.funcs}
        self.jt_targets = {t for _, tbl in jtbls for t in tbl if vram <= t < self.end}
        self.extra_entries = set()   # jal targets that land inside a function
        self.overrides = set()       # function starts replaced by hand-written C
        self.main = None             # the main Unit, for overlay -> main calls

    def fname(self, addr):
        return f"{self.prefix}{addr:08X}"

    def owns(self, addr):
        return self.vram <= addr < self.end

    def call_expr(self, target):
        if self.owns(target) and (target in self.func_starts or target in self.extra_entries):
            return f"{self.fname(target)}();"
        if self.main is not None and self.main.owns(target) and (
                target in self.main.func_starts or target in self.main.extra_entries):
            return f"{self.main.fname(target)}();"
        return f"call_indirect({hx(target)});"

    def containing(self, addr):
        lo, hi = 0, len(self.funcs) - 1
        best = None
        while lo <= hi:
            mid = (lo + hi) // 2
            if self.funcs[mid][2] <= addr:
                best = self.funcs[mid]
                lo = mid + 1
            else:
                hi = mid - 1
        if best and best[2] <= addr < best[2] + best[3]:
            return best
        return None

    def collect_entries(self, extra):
        """Find jal targets (in this unit and others) that are not function starts."""
        for addr in extra:
            if self.owns(addr) and addr not in self.func_starts:
                self.extra_entries.add(addr)

    def generate(self):
        files, protos, table = [], [], []
        chunk, idx = [], 0
        for name, _, start, size in self.funcs:
            # Functions listed in config/recomp_overrides.txt are replaced by hand-written C (src/): the
            # recompiled version is kept as <name>_orig so the replacement can still call it.
            cname = self.fname(start) + ("_orig" if start in self.overrides else "")
            g = FuncGen(self, cname, start, size, self.code, self.vram)
            body = [f"/* {name} {start:08X} size {size:#x} */", g.gen()]
            protos.append(f"void {self.fname(start)}(void);")
            if start in self.overrides:
                protos.append(f"void {cname}(void);   /* recompiled original, replaced in src/ */")
            table.append(start)
            for e in sorted(x for x in self.extra_entries if start < x < start + size):
                g2 = FuncGen(self, self.fname(start), start, size, self.code, self.vram)
                body.append(f"/* secondary entry {e:08X} into {name} */")
                body.append(g2.gen(entry=e))
                protos.append(f"void {self.fname(e)}(void);")
                table.append(e)
            chunk.append("\n".join(body))
            if len(chunk) >= FUNCS_PER_FILE:
                files.append(chunk)
                chunk = []
        if chunk:
            files.append(chunk)
        written = []
        for n, ch in enumerate(files):
            path = os.path.join(OUT, f"{self.tag}_{n:03d}.c")
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write('#include "recomp.h"\n#include "recomp_funcs.h"\n\n')
                f.write("\n\n".join(ch))
                f.write("\n")
            written.append(os.path.basename(path))
        return written, protos, sorted(table)


def all_jal_targets(units):
    """Call targets, plus branch/jump targets that leave their own function (shared tails, tail calls)."""
    t = set()
    for u in units:
        for name, _, start, size in u.funcs:
            for a in range(start, start + size, 4):
                o = a - u.vram
                if o + 4 > len(u.code):
                    break
                w = struct.unpack_from("<I", u.code, o)[0]
                ins = Insn(a, w)
                k = ins.kind()
                if k == "jal":
                    t.add(ins.target)
                elif k == "j" and not (start <= ins.target < start + size):
                    t.add(ins.target)
                elif k == "branch" and not (start <= ins.branch_target() < start + size):
                    t.add(ins.branch_target())
    return t


def load_overrides():
    """config/recomp_overrides.txt: '<unit> <hex address>' per line."""
    p = os.path.join(ROOT, "config", "recomp_overrides.txt")
    out = defaultdict(set)
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#")[0].strip()
            if line:
                unit, addr = line.split()
                out[unit].add(int(addr, 16))
    return out


def load_extra_funcs():
    p = os.path.join(ROOT, "config", "recomp_extra_funcs.txt")
    extra = defaultdict(set)
    if os.path.exists(p):
        for line in open(p):
            line = line.split("#")[0].strip()
            if line:
                unit, addr = line.split()
                extra[unit].add(int(addr, 16))
    return extra


def main():
    os.makedirs(OUT, exist_ok=True)
    for f in glob.glob(os.path.join(OUT, "*.c")):
        os.remove(f)

    # ---- main executable ----
    main_code = open(os.path.join(ROOT, "baserom", "main.bin"), "rb").read()
    mfuncs, mjt = parse_asm(sorted(glob.glob(os.path.join(ROOT, "asm", "main", "**", "*.s"), recursive=True)))
    mainu = Unit("main", main_code, MAIN_VRAM, mfuncs, mjt, "f_")

    # ---- overlays: split slot-1 functions by overlay using the file table ----
    rows = [l.split("\t") for l in open(os.path.join(ROOT, "baserom", "filetable.tsv")).read().splitlines()]
    ovl1 = [(n, int(o, 16) & 0x7FFFFFFF, int(s, 16)) for n, o, s, _ in rows if n.startswith("OVL/")]
    base = ovl1[0][1]
    ofuncs, ojt = parse_asm(sorted(glob.glob(os.path.join(ROOT, "asm", "ovl", "**", "*.s"), recursive=True)))
    units = [mainu]
    ovl_units = []
    for n, ofs, size in ovl1:
        stem = os.path.basename(n).split(".")[0].lower()[:-2]      # "pl00"
        lo, hi = ofs - base, ofs - base + size
        fl = [f for f in ofuncs if lo <= f[1] < hi]
        jt = [t for o, tbl in ojt if lo <= o < hi for t in tbl]    # this overlay's own jump tables
        for slot in (1, 2):
            delta = OVL_VRAM[slot - 1] - OVL_VRAM[0]
            code = open(os.path.join(ROOT, "baserom", "ovl", f"{stem}_{slot}.ovl"), "rb").read()
            funcs = [(nm, o, v + delta, s) for nm, o, v, s in fl]
            u = Unit(f"ovl_{stem}_{slot}", code, OVL_VRAM[slot - 1], funcs, [(0, [t + delta for t in jt])],
                     f"o_{stem}_{slot}_")
            u.main = mainu
            u.stem, u.slot = stem, slot
            units.append(u)
            ovl_units.append(u)

    extra = load_extra_funcs()
    overrides = load_overrides()
    for u in units:
        u.overrides = overrides.get(u.tag if u.tag == "main" else u.tag[4:], set())
    jal = all_jal_targets(units)
    mainu.collect_entries(jal | extra.get("main", set()))
    for u in ovl_units:
        # an overlay's own jal targets only (other overlays share the address range)
        own = all_jal_targets([u])
        u.collect_entries(own | extra.get(u.tag[4:], set()))

    all_files, all_protos = [], []
    tables = {}
    for u in units:
        files, protos, table = u.generate()
        all_files += files
        all_protos += protos
        tables[u.tag] = table

    # ---- function tables for call_indirect ----
    with open(os.path.join(OUT, "recomp_funcs.h"), "w", newline="\n") as f:
        f.write("/* generated by tools/recomp/recomp.py */\n#pragma once\n")
        f.write("\n".join(all_protos) + "\n")
    with open(os.path.join(OUT, "recomp_tables.c"), "w", newline="\n") as f:
        f.write('#include "recomp.h"\n#include "recomp_funcs.h"\n\n')
        f.write("const FuncEntry g_main_funcs[] = {\n")
        f.write("".join(f"    {{0x{a:08X}u, {mainu.fname(a)}}},\n" for a in tables["main"]))
        f.write("};\nconst int g_main_funcs_count = sizeof(g_main_funcs) / sizeof(g_main_funcs[0]);\n\n")
        for u in ovl_units:
            f.write(f"static const FuncEntry tbl_{u.tag}[] = {{\n")
            f.write("".join(f"    {{0x{a:08X}u, {u.fname(a)}}},\n" for a in tables[u.tag]))
            f.write("};\n")
        f.write("\nconst OverlayInfo g_overlays[] = {\n")
        for u in ovl_units:
            fp = ", ".join(f"0x{b:02X}" for b in u.code[:OVL_FINGERPRINT])
            # code probes: a few function entries spread over the overlay. The header (the fingerprint) holds
            # pointers the game may rewrite while a fight runs; the code itself never changes.
            entries = [a for a in tables[u.tag] if a - u.vram >= OVL_FINGERPRINT and a - u.vram + OVL_PROBE <= len(u.code)]
            picks = [entries[(i * len(entries)) // OVL_PROBES] for i in range(OVL_PROBES)] if entries else []
            probes = ", ".join("{0x%08Xu, {%s}}" % (a, ", ".join(f"0x{b:02X}" for b in u.code[a - u.vram:a - u.vram + OVL_PROBE]))
                               for a in picks)
            f.write(f'    {{"{u.stem}", {u.slot}, 0x{u.vram:08X}u, 0x{len(u.code):X}u, tbl_{u.tag}, '
                    f'sizeof(tbl_{u.tag}) / sizeof(FuncEntry), {{{fp}}}, {{{probes}}}, {len(picks)}}},\n')
        f.write("};\nconst int g_overlays_count = sizeof(g_overlays) / sizeof(g_overlays[0]);\n")
    all_files.append("recomp_tables.c")
    with open(os.path.join(OUT, "sources.cmake"), "w", newline="\n") as f:
        f.write("set(RECOMP_SOURCES\n" + "".join(f"    ${{RECOMP_DIR}}/{x}\n" for x in all_files) + ")\n")

    nf = sum(len(t) for t in tables.values())
    print(f"split {sum(u.n_split for u in units)} merged functions")
    print(f"recompiled {len(mainu.funcs)} main functions (+{len(mainu.extra_entries)} secondary entries), "
          f"{len(ovl_units)} overlay variants, {nf} entry points total, {len(all_files)} C files")


OVL_FINGERPRINT = 64
OVL_PROBES, OVL_PROBE = 4, 16          # must match recomp.h

if __name__ == "__main__":
    main()
