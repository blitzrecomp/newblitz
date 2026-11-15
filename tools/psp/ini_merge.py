#!/usr/bin/env python3
"""Merges platform/psp/fat.ini's keys into PPSSPP's ppsspp.ini in place (run_ppsspp.sh backs the file up first and puts it back
afterwards). --appendconfig applies [SystemParam] but this PPSSPP build ignores an appended [Log] section, so the keys go in here.
Usage: ini_merge.py <ppsspp.ini> <overrides.ini>"""
import sys


def sections(path):
    out, cur = {}, None
    for raw in open(path, encoding="utf-8-sig"):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = out.setdefault(line[1:-1], {})
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[k.strip()] = v.strip()
    return out


def main():
    target, overrides = sys.argv[1], sections(sys.argv[2])
    lines = open(target, encoding="utf-8-sig").read().splitlines()
    cur, done = None, set()
    for i, raw in enumerate(lines):
        s = raw.strip()
        if s.startswith("[") and s.endswith("]"):
            cur = s[1:-1]
        elif "=" in s and cur in overrides:
            k = s.split("=", 1)[0].strip()
            if k in overrides[cur]:
                lines[i] = f"{k} = {overrides[cur][k]}"
                done.add((cur, k))
    for sec, kv in overrides.items():
        missing = [(k, v) for k, v in kv.items() if (sec, k) not in done]
        if not missing:
            continue
        at = next((i for i, l in enumerate(lines) if l.strip() == f"[{sec}]"), None)
        if at is None:
            lines += [f"[{sec}]"] + [f"{k} = {v}" for k, v in missing]
        else:
            lines[at + 1:at + 1] = [f"{k} = {v}" for k, v in missing]
    open(target, "w", encoding="utf-8").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
