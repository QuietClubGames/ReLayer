#!/usr/bin/env python3
"""
Make the ReLayer (Fighting Layer PC port) Setup download (developer machine only):

    python tools/install/make_package.py            -> dist/ReLayer-Setup/ and dist/ReLayer-Setup.zip
    python tools/install/make_package.py --public   -> dist/public/...: Fighting Layer only (no Street Fighter EX2
                                                       Plus option, no Pullum tools), for public release

The package holds Setup.exe, a trimmed Python with the disassembler packages, a trimmed 32-bit clang, and
this project's source (tracked files only). It holds NO ROM data and nothing made from the ROM: Setup builds
the game from the player's own fgtlayer.zip. Needs the developer toolchain on PATH (i686-w64-mingw32-clang),
Python with splat64 / spimdisasm / rabbitizer installed, and Pillow (for the icon).

menu_art.png in the repo root, if present, becomes the menu art, the Setup banner and the icon.
"""
import importlib.metadata as md
import os
import shutil
import re
import subprocess
import sys
import zipfile

PUBLIC = "--public" in sys.argv[1:]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "tools", "install")
DIST = os.path.join(ROOT, "dist", "public") if PUBLIC else os.path.join(ROOT, "dist")
OUT = os.path.join(DIST, "ReLayer-Setup")
FILES = os.path.join(OUT, "files")

# what the build needs from this repo (tracked or new files, never ignored ones)
SOURCE_PREFIXES = ("CMakeLists.txt", "config/", "port/", "src/", "tools/extract_rom.py", "tools/gen_ovl_yaml.py",
                   "tools/recomp/", "tools/install/build_from_rom.py", "tools/install/game.rc",
                   "tools/sfex/", "tools/portraits.py")   # tools/sfex: Pullum from the player's sfex2p.zip
SOURCE_SKIP = ("config/ovl.yaml", "config/undefined_")
if PUBLIC:                                                    # the extra fighter's tools stay out
    SOURCE_PREFIXES = tuple(x for x in SOURCE_PREFIXES if x not in ("tools/sfex/", "tools/portraits.py"))
PY_PACKAGES = ["splat64", "spimdisasm", "rabbitizer", "PyYAML", "pylibyaml", "tqdm", "colorama", "intervaltree",
               "sortedcontainers", "n64img", "pypng", "pygfxd", "crunch64",   # splat imports all of these
               "numpy", "pillow"]                                         # tools/sfex (Pullum)
if PUBLIC:
    PY_PACKAGES = PY_PACKAGES[:-2]
PY_LIB_SKIP = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "site-packages", "__pycache__", "lib2to3",
               "pydoc_data", "venv", "unittest"}
PY_DLL_SKIP = ("_tkinter", "tcl", "tk", "_test", "_ctypes_test", "xxlimited")
INCLUDE_SKIP = {"c++", "ddk", "GL", "KHR", "gdiplus", "mach-o", "wrl"}


def say(*a):
    print(*a, flush=True)


def toolchain_root():
    clang = shutil.which("i686-w64-mingw32-clang")
    if not clang:
        sys.exit("i686-w64-mingw32-clang is not on PATH")
    return os.path.dirname(os.path.dirname(os.path.realpath(clang)))


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, skip_dirs=(), skip_file=lambda n: False):
    for d, dirs, files in os.walk(src):
        dirs[:] = [x for x in dirs if x not in skip_dirs]
        for f in files:
            if not skip_file(f):
                copy(os.path.join(d, f), os.path.join(dst, os.path.relpath(os.path.join(d, f), src)))


def make_icon():
    art = os.path.join(ROOT, "menu_art.png")
    ico = os.path.join(HERE, "fgtlayer.ico")
    if not os.path.isfile(art):
        return None
    from PIL import Image, ImageDraw
    im = Image.open(art).convert("RGBA")
    w, h = im.size
    # a square around the face (the art is 2000x1635 with the head left of centre, a quarter of the way down)
    side = int(min(w, h) * 0.27)
    cx, cy = int(w * 0.50), int(h * 0.257)
    face = im.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)).resize((256, 256), Image.LANCZOS)
    mask = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 255, 255), radius=44, fill=255)
    face.putalpha(mask)
    face.save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    return ico


SETUP_VERSIONINFO = """
1 VERSIONINFO
FILEVERSION %(n)s
PRODUCTVERSION %(n)s
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "QuietClubGames"
      VALUE "FileDescription", "ReLayer Setup"
      VALUE "FileVersion", "%(v)s"
      VALUE "InternalName", "Setup"
      VALUE "LegalCopyright", "ReLayer code: MIT license. Fighting Layer is a trademark of its owners."
      VALUE "OriginalFilename", "Setup.exe"
      VALUE "ProductName", "ReLayer"
      VALUE "ProductVersion", "%(v)s"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"""


def release_version():
    """The release number, from the ProductVersion line of game.rc (the one place it is written)."""
    with open(os.path.join(HERE, "game.rc")) as f:
        m = re.search(r'VALUE "ProductVersion", "([0-9.]+)"', f.read())
    if not m:
        sys.exit("game.rc has no ProductVersion")
    return m.group(1)


def build_setup_exe(tc, ico):
    clang = os.path.join(tc, "bin", "i686-w64-mingw32-clang.exe")
    windres = os.path.join(tc, "bin", "llvm-windres.exe")
    tmp = os.path.join(DIST, "obj")
    os.makedirs(tmp, exist_ok=True)
    rc = os.path.join(tmp, "setup.rc")
    with open(rc, "w") as f:
        if ico:
            f.write('1 ICON "%s"\n' % ico.replace("\\", "/"))
        f.write('1 24 "%s"\n' % os.path.join(HERE, "setup.manifest").replace("\\", "/"))
        # version details (publisher, product, version) as Windows shows them under Properties > Details; an
        # exe without them looks like throwaway malware to antivirus heuristics. The version comes from game.rc.
        v = release_version()
        n = ",".join((v.split(".") + ["0", "0", "0"])[:4])
        f.write(SETUP_VERSIONINFO % {"n": n, "v": v})
    res = os.path.join(tmp, "setup_res.o")
    subprocess.check_call([windres, "--no-preprocess", "--target=pe-i386", rc, "-O", "coff", "-o", res])
    subprocess.check_call([clang, "-O2", "-s", "-municode", "-mwindows", "-Wall"] + (["-DNO_EXTRA"] if PUBLIC else []) +
                          [os.path.join(HERE, "setup_win32.c"), res,
                           "-o", os.path.join(OUT, "Setup.exe"), "-lcomctl32", "-lcomdlg32", "-lshell32", "-lole32",
                           "-luuid", "-lwindowscodecs", "-lgdi32", "-luser32", "-ladvapi32"])


def package_source(ico):
    out = subprocess.check_output(["git", "ls-files", "-co", "--exclude-standard"], cwd=ROOT, text=True)
    n = 0
    for f in out.splitlines():
        if f.startswith(SOURCE_PREFIXES) and not f.startswith(SOURCE_SKIP) and os.path.isfile(os.path.join(ROOT, f)):
            copy(os.path.join(ROOT, f), os.path.join(FILES, "source", f))
            n += 1
    if ico:
        copy(ico, os.path.join(FILES, "source", "tools", "install", "fgtlayer.ico"))
    say(f"source: {n} files")


def package_python():
    base = sys.base_prefix
    dst = os.path.join(FILES, "python")
    for f in os.listdir(base):
        if f.lower().endswith((".exe", ".dll")) or f == "LICENSE.txt":
            copy(os.path.join(base, f), os.path.join(dst, f))
    copy_tree(os.path.join(base, "DLLs"), os.path.join(dst, "DLLs"),
              skip_file=lambda n: n.lower().startswith(PY_DLL_SKIP))
    copy_tree(os.path.join(base, "Lib"), os.path.join(dst, "Lib"), skip_dirs=PY_LIB_SKIP)
    # unittest is imported by a few stdlib modules splat touches (via typing helpers); keep it after all
    copy_tree(os.path.join(base, "Lib", "unittest"), os.path.join(dst, "Lib", "unittest"), skip_dirs={"test", "__pycache__"})
    site = os.path.join(dst, "Lib", "site-packages")
    for name in PY_PACKAGES:
        d = md.distribution(name)
        for f in d.files or []:
            if "__pycache__" in f.parts:
                continue
            src = d.locate_file(f)
            if os.path.isfile(src):
                copy(str(src), os.path.join(site, *f.parts))
        say(f"python package {name} {d.version}")
    # a ._pth file makes this Python ignore any other Python installed on the player's PC
    ver = "python%d%d" % sys.version_info[:2]
    with open(os.path.join(dst, ver + "._pth"), "w") as f:
        f.write("Lib\nDLLs\nLib\\site-packages\n")


def package_clang(tc):
    dst = os.path.join(FILES, "clang")
    ver = sorted(os.listdir(os.path.join(tc, "lib", "clang")))[-1]
    major = ver.split(".")[0]
    for f in ["clang.exe", f"clang-{major}.exe", "i686-w64-mingw32-clang.exe", "ld.lld.exe", "llvm-windres.exe",
              f"libLLVM-{major}.dll", "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
              "i686-w64-windows-gnu.cfg", "mingw32-common.cfg"]:
        copy(os.path.join(tc, "bin", f), os.path.join(dst, "bin", f))
    copy_tree(os.path.join(tc, "lib", "clang", ver, "include"), os.path.join(dst, "lib", "clang", ver, "include"))
    copy(os.path.join(tc, "lib", "clang", ver, "lib", "windows", "libclang_rt.builtins-i386.a"),
         os.path.join(dst, "lib", "clang", ver, "lib", "windows", "libclang_rt.builtins-i386.a"))
    copy_tree(os.path.join(tc, "i686-w64-mingw32", "lib"), os.path.join(dst, "i686-w64-mingw32", "lib"))
    copy_tree(os.path.join(tc, "include"), os.path.join(dst, "include"), skip_dirs=INCLUDE_SKIP,
              skip_file=lambda n: n.endswith((".idl", ".hpp")))
    copy(os.path.join(tc, "LICENSE.TXT"), os.path.join(OUT, "licenses", "LLVM.txt"))


def licenses():
    lic = os.path.join(OUT, "licenses")
    copy(os.path.join(ROOT, "LICENSE"), os.path.join(lic, "ReLayer.txt"))
    copy(os.path.join(sys.base_prefix, "LICENSE.txt"), os.path.join(lic, "Python.txt"))
    for name in PY_PACKAGES:
        d = md.distribution(name)
        for f in d.files or []:
            if f.name.upper().startswith(("LICENSE", "COPYING")):
                copy(str(d.locate_file(f)), os.path.join(lic, f"{name}-{f.name}"))
                break
    with open(os.path.join(lic, "mingw-w64.txt"), "w") as f:
        f.write("The Windows headers and import libraries are from the mingw-w64 project (https://www.mingw-w64.org/),\n"
                "distributed under the Zope Public License, MIT-style licenses, and public domain dedications.\n"
                "The compiler is llvm-mingw (https://github.com/mstorsjo/llvm-mingw).\n")


README = """ReLayer: an unofficial PC port of Fighting Layer (Arika, 1998)
===============================================================

ReLayer does not include the game. You need your own legally obtained copy of the arcade ROM:
the MAME set "fgtlayer.zip" (Fighting Layer, World, Ver. A).
Any MAME version's set works: Setup checks the twelve ROM chips by checksum, whatever they are named.
No game data comes with this download.

1. Unzip this whole folder anywhere.
2. Run Setup.exe. Windows may say "Windows protected your PC", because Setup is not signed:
   choose "More info", then "Run anyway".
3. Choose your fgtlayer.zip and where to install, then press Install.

{extra}Setup checks the ROM, then builds the game from it on your PC (a minute or two) and adds shortcuts.
Everything it needs is in the "files" folder; nothing is downloaded. You can delete this folder afterwards.

THE MENU
--------
Controller: press Back and Start together (on Xbox Series / One pads: View + Menu).
Keyboard: Tab or Esc.
The game pauses while the menu is open.
  Controller: D-pad or stick to move, A to choose, B to go back, LB / RB to switch pages, Start to close.
  Keyboard: arrows to move, Enter to choose, Esc to go back, Q / E to switch pages, Tab to close.
Pages:
  Display   window / borderless / exclusive full screen, window size, monitor, picture shape (4:3,
            stretch, whole-number), enhanced or original graphics, quality, screen filter
            (sharp / smooth / scanlines), frame-rate counter
  Audio     volume, mute
  Controls  remap any button ("press the button you want"), stick dead zone, restore defaults
  Game      unlock all characters (on by default), free play, difficulty, damage, round timer,
            rounds, and the arcade's other settings
  System    resume, the arcade's operator menu, restart, quit
Settings are saved in settings.ini, controller buttons in controls.ini (both in the install folder).

CONTROLLER (Xbox-style; the first controller is player 1, the second player 2)
----------
  Move             D-pad or left stick
  Punch L / M / H  X / Y / RB
  Kick  L / M / H  A / B / RT
  Start            Start (Menu)
  Insert coin      Back (View)
  Menu             Back + Start together

KEYBOARD
--------
                  Player 1           Player 2
  Move            arrow keys         I J K L
  Punch L / M / H Z X C              Q W E
  Kick  L / M / H A S D              R T Y
  Start           1                  2
  Insert coin     5                  6

  Tab / Esc       menu (quit from its System page)
  Alt+Enter, F11  window / full screen
  F9              high-resolution 3D on / off
  F2              arcade test button (operator menu)

To play: insert a coin, then press Start. Free play (in the menu's Game page) makes Start alone enough.

To remove the game: Windows Settings > Apps > ReLayer (Fighting Layer PC port) > Uninstall.

Reporting a problem: please include fgtlayer.log from the install folder (the previous run's is
fgtlayer.old.log).

The licenses of the tools Setup uses are in the "licenses" folder.

ReLayer is an unofficial fan project, not affiliated with or endorsed by Arika or Bandai Namco.
Fighting Layer is a trademark of its owners.
"""

EXTRA_README = """Optional extra fighter: if you also have Street Fighter EX2 Plus (the MAME set "sfex2p.zip"), choose it in
Setup too. Setup makes Pullum Purna from it; she is on the select screen between Clemence and Joe. To give
her your own select-screen picture, put a 256 x 240 PNG named PULLUM_portrait.png next to Setup.exe before
installing.

"""


def main():
    tc = toolchain_root()
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(FILES)
    ico = make_icon()
    say(f"icon: {ico}")
    build_setup_exe(tc, ico)
    package_source(ico)
    package_python()
    package_clang(tc)
    licenses()
    art = os.path.join(ROOT, "menu_art.png")
    if os.path.isfile(art):
        copy(art, os.path.join(FILES, "menu_art.png"))
    with open(os.path.join(OUT, "README.txt"), "w", newline="\r\n") as f:
        f.write(README.format(extra="" if PUBLIC else EXTRA_README))
    shutil.rmtree(os.path.join(DIST, "obj"), ignore_errors=True)

    zpath = OUT + ".zip"
    total = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, _, files in os.walk(OUT):
            for f in files:
                p = os.path.join(d, f)
                total += os.path.getsize(p)
                z.write(p, os.path.join("ReLayer-Setup", os.path.relpath(p, OUT)))
    say(f"package: {total / 2**20:.0f} MB unpacked, {os.path.getsize(zpath) / 2**20:.0f} MB zipped -> {zpath}")


if __name__ == "__main__":
    main()
