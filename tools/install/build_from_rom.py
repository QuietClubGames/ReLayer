#!/usr/bin/env python3
"""
Build a playable Fighting Layer from the player's own ROM: the step Setup runs on their PC.

    python tools/install/build_from_rom.py --rom fgtlayer.zip --dest "D:/Games/Fighting Layer"

1. check and unpack the ROM set              (tools/extract_rom.py)
   with --extra-rom sfex2p.zip: check and unpack Street Fighter EX2 Plus too (tools/sfex/extract_sfex2.py) and
   make Pullum Purna's files from it (tools/sfex/make_swap.py), for <dest>/extra (the extra fighter,
   src/game/roster.c); --portrait <png> is her select-screen picture (256 x 240, tools/portraits.py)
2. disassemble the game's code               (splat)
3. translate it to C                         (tools/recomp/recomp.py)
4. compile and link                          (32-bit clang, in parallel)
5. copy the game and its ROM data to --dest  (settings, saves and controls already there are kept)

Nothing made from the ROM is ever shipped: this script makes it on the player's machine. Lines starting
with "@@" are for Setup: "@@progress <0-100> <text>", "@@error <text>", "@@done <path to exe>".
"""
import argparse
import concurrent.futures
import ctypes
import os
import re
import shutil
import subprocess
import sys
import time

SRC = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE_NAME = "ReLayer.exe"
ROM_FILES = ["prg.bin", "banked.bin", "main.bin", "sound.bin", "wave.bin"]   # what the game reads at run time
LIBS = ["gdi32", "user32", "winmm", "d3d11", "dxgi", "ole32", "windowscodecs"]
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)   # no console windows flashing up under Setup
SKIP = {".git", "baserom", "assets", "assets_split", "asm", "build", "dist", "work", "screenshots", "tests",
        "__pycache__", ".splache", ".claude", "_build"}
SKIP_EXT = (".zip", ".log", ".bmp", ".wav", ".ini", ".bin")


def say(*parts):
    print(*parts, flush=True)


def progress(pct, text):
    say(f"@@progress {int(pct)} {text}")


def fail(text):
    say(f"@@error {text}")
    sys.exit(1)


def run(cmd, cwd, what):
    """Run a step, echoing its output into the log; stop with a readable error if it fails."""
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
    if p.stdout:
        say(p.stdout.rstrip())
    if p.returncode != 0:
        rom_error = [l for l in (p.stdout or "").splitlines() if l.startswith("ROM error:")]
        fail(rom_error[-1][len("ROM error: "):] if rom_error else f"{what} failed (see setup.log).")


def total_ram_gb():
    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("pf_total", ctypes.c_ulonglong), ("pf_avail", ctypes.c_ulonglong),
                    ("v_total", ctypes.c_ulonglong), ("v_avail", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
    try:
        m = MS()
        m.len = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.total / 2**30
    except Exception:
        return 8.0


def find_clang(clang_dir):
    name = "i686-w64-mingw32-clang.exe"
    if clang_dir:
        path = os.path.join(clang_dir, name)
        if not os.path.isfile(path):
            path = os.path.join(clang_dir, "bin", name)
        return path if os.path.isfile(path) else None
    return shutil.which("i686-w64-mingw32-clang")


def runtime_sources():
    """The game's own C files, read from CMakeLists.txt so this list never drifts from the developer build."""
    text = open(os.path.join(SRC, "CMakeLists.txt"), encoding="utf-8").read()
    m = re.search(r"add_executable\(fgtlayer\s+(.*?)\)", text, re.S)
    return m.group(1).split()


def copy_source(work):
    def ignore(d, names):
        return [n for n in names if n in SKIP or n.lower().endswith(SKIP_EXT) or n.startswith("menu_art.")]
    if os.path.isdir(work):
        shutil.rmtree(work)
    shutil.copytree(SRC, work, ignore=ignore)


def compile_all(work, clang, jobs):
    rdir = os.path.join(work, "build", "recomp")
    odir = os.path.join(work, "build", "obj")
    os.makedirs(odir, exist_ok=True)
    tasks = []
    for f in sorted(os.listdir(rdir)):
        if f.endswith(".c"):
            tasks.append((os.path.join(rdir, f), ["-O1", "-w", "-I", os.path.join(work, "port", "core"), "-I", rdir]))
    inc = []
    for d in ("port/core", "src/game", "build/recomp", "port/ui"):
        inc += ["-I", os.path.join(work, d)]
    for f in runtime_sources():
        tasks.append((os.path.join(work, f), ["-O2", "-w"] + inc))
    # biggest first so the slowest files don't finish last
    tasks.sort(key=lambda t: -os.path.getsize(t[0]))

    def one(task):
        src, flags = task
        obj = os.path.join(odir, os.path.relpath(src, work).replace(os.sep, "_").replace("/", "_") + ".o")
        cmd = [clang, "-std=gnu99", "-DNDEBUG", "-fno-strict-aliasing"] + flags + ["-c", src, "-o", obj]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                           creationflags=NO_WINDOW)
        return src, obj, p.returncode, p.stdout

    objs, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for src, obj, rc, out in _as_done(ex, one, tasks):
            done += 1
            if out.strip():
                say(out.rstrip())
            if rc != 0:
                fail(f"Compiling {os.path.basename(src)} failed (see setup.log).")
            objs.append(obj)
            progress(40 + 52 * done / len(tasks), f"Compiling the game ({done} of {len(tasks)})")
    return objs


def _as_done(ex, fn, items):
    futs = [ex.submit(fn, i) for i in items]
    for f in concurrent.futures.as_completed(futs):
        yield f.result()


def icon_object(work, clang):
    """The game's icon (tools/install/fgtlayer.ico, made from the menu art by make_package.py), if there is one."""
    ico_dir = os.path.join(work, "tools", "install")
    windres = os.path.join(os.path.dirname(clang), "llvm-windres.exe")
    if not os.path.isfile(os.path.join(ico_dir, "fgtlayer.ico")) or not os.path.isfile(windres):
        return []
    obj = os.path.join(work, "build", "icon.o")
    p = subprocess.run([windres, "--no-preprocess", "--target=pe-i386", "game.rc", "-O", "coff", "-o", obj],
                       cwd=ico_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       creationflags=NO_WINDOW)
    if p.returncode != 0:
        say("icon skipped: " + p.stdout.strip())
        return []
    return [obj]


def link(work, clang, objs, exe):
    objs = objs + icon_object(work, clang)
    rsp = os.path.join(work, "build", "link.rsp")
    with open(rsp, "w") as f:
        for o in objs:
            f.write('"' + o.replace("\\", "/") + '"\n')
    cmd = [clang, "-O2", "-mwindows", "@" + rsp, "-o", exe] + ["-l" + l for l in LIBS]
    run(cmd, work, "Linking the game")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True, help="the MAME fgtlayer.zip")
    ap.add_argument("--dest", required=True, help="folder to install the game into")
    ap.add_argument("--clang", help="folder with i686-w64-mingw32-clang.exe (default: PATH)")
    ap.add_argument("--art", help="picture to use as menu_art.png if the folder has none yet")
    ap.add_argument("--extra-rom", help="the MAME sfex2p.zip (Street Fighter EX2 Plus): adds Pullum Purna")
    ap.add_argument("--portrait", help="her select-screen picture (PNG), instead of one made from EX2 Plus's")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compiles (default: by CPU count and memory)")
    ap.add_argument("--keep-work", action="store_true", help="keep the intermediate files in <dest>/_build")
    a = ap.parse_args()

    t0 = time.time()
    rom = os.path.abspath(a.rom)
    dest = os.path.abspath(a.dest)
    clang = find_clang(a.clang)
    if not clang:
        fail("The compiler is missing from this Setup folder. Download Setup again.")
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, int(total_ram_gb() * 2)))   # ~100 MB per compile at most
    work = os.path.join(dest, "_build")
    os.makedirs(dest, exist_ok=True)
    say(f"Fighting Layer build: rom={rom} dest={dest} clang={clang} jobs={jobs} python={sys.executable}")

    progress(1, "Preparing")
    copy_source(work)
    py = [sys.executable]

    progress(3, "Checking your ROM")
    run(py + [os.path.join(work, "tools", "extract_rom.py"), rom], work, "Reading the ROM")

    extra = None
    if a.extra_rom:
        progress(6, "Checking your Street Fighter EX2 Plus ROM")
        run(py + [os.path.join(work, "tools", "sfex", "extract_sfex2.py"), "--set", "sfex2p",
                  os.path.abspath(a.extra_rom)], work, "Reading the Street Fighter EX2 Plus ROM")
        progress(8, "Making Pullum Purna")
        if a.portrait and os.path.isfile(a.portrait):
            os.makedirs(os.path.join(work, "art", "select"), exist_ok=True)
            shutil.copyfile(a.portrait, os.path.join(work, "art", "select", "PULLUM_portrait.png"))
        extra = os.path.join(work, "extra")
        run(py + [os.path.join(work, "tools", "sfex", "make_swap.py"), "--game", "ex2p", "--ex2", "04", "--fl", "00",
                  "--own-stance", "--out", extra], work, "Making Pullum Purna")
        shutil.rmtree(os.path.join(work, "ex2prom"), ignore_errors=True)   # about 100 MB of unpacked ROM, done with

    progress(10, "Reading the game's code")
    run(py + [os.path.join(work, "tools", "gen_ovl_yaml.py")], work, "Finding the character code")
    run(py + ["-m", "splat", "split", "config/main.yaml"], work, "Disassembling the game")
    progress(20, "Reading the character code")
    run(py + ["-m", "splat", "split", "config/ovl.yaml"], work, "Disassembling the characters")

    progress(30, "Translating the game's code to C")
    run(py + [os.path.join(work, "tools", "recomp", "recomp.py")], work, "Translating the game")

    progress(40, "Compiling the game")
    objs = compile_all(work, clang, jobs)

    progress(93, "Linking")
    exe_tmp = os.path.join(work, "build", EXE_NAME)
    link(work, clang, objs, exe_tmp)

    progress(96, "Installing")
    rom_dir = os.path.join(dest, "rom")
    os.makedirs(rom_dir, exist_ok=True)
    for f in ROM_FILES:
        shutil.copyfile(os.path.join(work, "baserom", f), os.path.join(rom_dir, f))
    exe = os.path.join(dest, EXE_NAME)
    shutil.copyfile(exe_tmp, exe)
    if extra:                                        # the extra fighter, made fresh: replaces an older one
        shutil.rmtree(os.path.join(dest, "extra"), ignore_errors=True)
        shutil.copytree(extra, os.path.join(dest, "extra"))
    if a.art and os.path.isfile(a.art):
        ext = os.path.splitext(a.art)[1].lower()
        have = any(os.path.isfile(os.path.join(dest, "menu_art" + e)) for e in (".png", ".jpg", ".jpeg"))
        if not have:
            shutil.copyfile(a.art, os.path.join(dest, "menu_art" + ext))
    if not a.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    say(f"built in {time.time() - t0:.0f} s")
    progress(100, "Done")
    say(f"@@done {exe}")


if __name__ == "__main__":
    main()
