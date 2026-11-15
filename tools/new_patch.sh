#!/bin/bash
# Write a submodule's new edits as the next patch of its stack: the working tree minus the existing patches applied
# in order to a clean checkout. Edit the submodule (with its patches applied, as `make patches` leaves it), then:
#   tools/new_patch.sh lib/rt64 platform/rt64-patches 0007-android
# tools/apply_patches.sh then sees the whole stack as applied.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
sub="$1" dir="$ROOT/$2" name="$3"
[ -d "$ROOT/$sub" ] && [ -d "$dir" ] || { echo "usage: $0 <submodule> <patch dir> <NNNN-name>"; exit 2; }
out="$dir/$name.patch"
[ -e "$out" ] && { echo "$out exists"; exit 2; }
tmp="$(mktemp -d)"
trap 'git -C "$ROOT/$sub" worktree remove --force "$tmp" >/dev/null 2>&1 || rm -rf "$tmp"; git -C "$ROOT/$sub" worktree prune' EXIT
git -C "$ROOT/$sub" worktree add -q --detach "$tmp" HEAD
for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    git -C "$tmp" apply --whitespace=nowarn "$p"
done
git -C "$tmp" add -A >/dev/null
git -C "$tmp" -c user.name=stack -c user.email=stack@local commit -q --no-verify -m stack  # the stack, to diff against
# Every file changed or added in the working tree, copied over the stack, then diffed.
( cd "$ROOT/$sub" && { git diff --ignore-submodules --name-only HEAD; git ls-files --others --exclude-standard; } ) | sort -u | while read -r f; do
    if [ -e "$ROOT/$sub/$f" ]; then mkdir -p "$tmp/$(dirname "$f")"; cp "$ROOT/$sub/$f" "$tmp/$f"; else rm -f "$tmp/$f"; fi
done
git -C "$tmp" add -A >/dev/null
git -C "$tmp" diff --cached HEAD > "$out"
if [ ! -s "$out" ]; then rm -f "$out"; echo "no new changes in $sub"; exit 1; fi
echo "$out: $(grep -c '^+++ ' "$out") file(s)"
