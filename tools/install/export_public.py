#!/usr/bin/env python3
"""
Copy this repository's files into the public ReLayer repository (a separate folder with its own history):

    python tools/install/export_public.py D:/ReLayer

Every tracked file is copied except the Street Fighter EX2 work (the extra-fighter tools and research notes,
PRIVATE below), and the Markdown files lose the parts between "<!-- private -->" and "<!-- /private -->". Files in the
target that are no longer exported are deleted; its .git folder is left alone, so the export can then be
committed there as the next public version. The game's own extra-fighter code (src/game/ex_*.c, roster.c)
stays: the game is built with it, and it does nothing without an extra fighter's files.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PRIVATE = ("tools/sfex/", "tools/portraits.py", "docs/SFEX2.md", "docs/EX2P_", "docs/NEW_CHARACTER_SLOT.md")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    dest = os.path.abspath(sys.argv[1])
    if os.path.commonpath([dest, ROOT]) == ROOT:
        sys.exit("the public repository must be outside this one")
    files = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).splitlines()
    keep = [f for f in files if not f.startswith(PRIVATE)]
    os.makedirs(dest, exist_ok=True)
    # remove what is no longer exported (never the target's .git)
    for d, dirs, names in os.walk(dest, topdown=True):
        dirs[:] = [x for x in dirs if not (d == dest and x == ".git")]
        for n in names:
            rel = os.path.relpath(os.path.join(d, n), dest).replace("\\", "/")
            if rel not in keep:
                os.remove(os.path.join(d, n))
    for f in keep:
        src, dst = os.path.join(ROOT, f), os.path.join(dest, f)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if f.endswith(".md"):
            text = open(src, encoding="utf-8").read()
            text = re.sub(r"<!-- private -->.*?<!-- /private -->\n?", "", text, flags=re.S)
            open(dst, "w", encoding="utf-8", newline="").write(text)
        else:
            shutil.copy2(src, dst)
    for d, dirs, names in os.walk(dest, topdown=False):      # empty folders left behind
        if ".git" not in d.replace("\\", "/").split("/") and d != dest and not os.listdir(d):
            os.rmdir(d)
    print(f"exported {len(keep)} of {len(files)} files to {dest}")


if __name__ == "__main__":
    main()
