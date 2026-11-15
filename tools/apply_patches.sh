#!/bin/bash
# Apply our patches to third-party submodules (platform/<submodule>-patches/*.patch). Idempotent:
# a patch that is already applied is skipped; one that no longer applies stops the build.
#   tools/apply_patches.sh --check   changes nothing, and fails if a submodule isn't exactly its patches (the build stamp's +)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# A submodule whose working tree already equals its patches applied in order to a clean checkout is done
# (later patches may edit lines of earlier ones, so a per-patch reverse check can't tell).
stack_applied() {
    local sub="$1" dir="$2" tmp
    tmp="$(mktemp -d)"
    git -C "$ROOT/$sub" worktree add -q --detach "$tmp" HEAD >/dev/null 2>&1 || { rm -rf "$tmp"; return 1; }
    local ok=0
    for p in "$dir"/*.patch; do
        [ -e "$p" ] || continue
        git -C "$tmp" apply --whitespace=nowarn "$p" 2>/dev/null || { ok=1; break; }
    done
    if [ $ok = 0 ]; then
        local added=""  # a file a patch adds, or one not in any patch: only --check, since the fallback below can't place them
        [ -z "${CHECKING:-}" ] || added="$(git -C "$tmp" ls-files --others --exclude-standard) $(git -C "$ROOT/$sub" ls-files --others --exclude-standard)"
        for f in $(git -C "$tmp" diff --ignore-submodules --name-only HEAD) $(git -C "$ROOT/$sub" diff --ignore-submodules --name-only HEAD) $added; do
            cmp -s "$tmp/$f" "$ROOT/$sub/$f" || { ok=1; break; }
        done
    fi
    git -C "$ROOT/$sub" worktree remove --force "$tmp" >/dev/null 2>&1 || rm -rf "$tmp"
    git -C "$ROOT/$sub" worktree prune >/dev/null 2>&1 || true
    return $ok
}
apply_dir() {
    local sub="$1" dir="$2"
    if [ -n "$(git -C "$ROOT/$sub" diff --ignore-submodules --name-only HEAD)" ] && stack_applied "$sub" "$dir"; then
        echo "  already applied: all of $(basename "$dir")"
        return
    fi
    for p in "$dir"/*.patch; do
        [ -e "$p" ] || continue
        if git -C "$ROOT/$sub" apply --reverse --check "$p" 2>/dev/null; then
            echo "  already applied: $(basename "$p")"
        elif git -C "$ROOT/$sub" apply --check "$p" 2>/dev/null; then
            git -C "$ROOT/$sub" apply "$p" && echo "  applied: $(basename "$p")"
        else
            echo "  FAILED: $(basename "$p") does not apply to $sub (submodule updated?)" >&2
            exit 1
        fi
    done
}
check_dir() {
    stack_applied "$1" "$2" || { echo "  differs from its patches: $1"; differs=1; }
}
each_stack() {
    "$1" lib/rt64/src/contrib/plume "$ROOT/platform/plume-patches"   # before rt64: its 0003 uses setFullscreen
    "$1" lib/rt64 "$ROOT/platform/rt64-patches"
    "$1" lib/N64ModernRuntime "$ROOT/platform/n64modernruntime-patches"
    "$1" lib/N64ModernRuntime/N64Recomp "$ROOT/platform/n64recomp-patches"   # the runtime's copy of recomp.h (PSP gpr32; inert elsewhere)
    "$1" lib/N64ModernRuntime/N64Recomp/lib/sljit "$ROOT/platform/sljit-patches"  # the runtime's JIT (iOS build)
    "$1" lib/RecompFrontend "$ROOT/platform/recompfrontend-patches"
}
if [ "${1:-}" = "--check" ]; then
    CHECKING=1 differs=0
    each_stack check_dir
    exit $differs
fi
each_stack apply_dir
