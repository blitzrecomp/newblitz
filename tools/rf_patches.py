#!/usr/bin/env python3
"""Regenerate our RecompFrontend patches from the submodule's working tree, so `make app` (which checks the
whole patch stack, tools/apply_patches.sh) builds what is on disk.

  .venv/bin/python tools/rf_patches.py

Each patch keeps the files it already owns: 0001, 0003 and 0004 are kept as they are; 0002 is the Blitz
settings look (its six files); 0005 is the Blitz '27 look, every other change, written as a diff against
the earlier patches applied in order to a clean checkout. Run after editing lib/RecompFrontend.
"""
import glob
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RF = ROOT / "lib" / "RecompFrontend"
P = ROOT / "platform" / "recompfrontend-patches"
FILES_0002 = ["recompui/src/config/ui_config_option.cpp", "recompui/src/config/ui_config_page_options_menu.cpp",
              "recompui/src/elements/ui_modal.cpp", "recompui/src/elements/ui_radio.cpp",
              "recompui/src/elements/ui_tab_set.cpp", "recompui/src/elements/ui_tab_set.h"]


def run(cmd, cwd):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{' '.join(cmd)}: {r.stderr}")
    return r.stdout


def patch(n):
    return Path(glob.glob(str(P / f"{n}-*.patch"))[0])


def main():
    tmp = tempfile.mkdtemp()
    run(["git", "worktree", "add", "-q", "--detach", tmp, "HEAD"], RF)
    try:
        # 0002: against upstream.
        patch("0002").write_text(run(["git", "diff", "HEAD", "--"] + FILES_0002, RF))
        # 0005: against 0001-0004 applied in order.
        for n in ("0001", "0002", "0003", "0004"):
            run(["git", "apply", str(patch(n))], tmp)
        run(["git", "add", "-A"], tmp)
        run(["git", "-c", "user.email=x@x", "-c", "user.name=x", "commit", "-qm", "base", "--no-verify"], tmp)
        for f in run(["git", "diff", "HEAD", "--name-only"], RF).split():
            shutil.copy(RF / f, Path(tmp) / f)
        out = P / "0005-blitz27-look.patch"
        out.write_text(run(["git", "diff"], tmp))
        print("rewrote", patch("0002").name, "and", out.name)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", tmp], cwd=RF, capture_output=True)
        subprocess.run(["git", "worktree", "prune"], cwd=RF, capture_output=True)


if __name__ == "__main__":
    main()
