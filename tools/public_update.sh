#!/bin/bash
# The next commit of the public repo (github.com/blitzrecomp/newblitz): this checkout's sanitized tree, made by
# tools/make_public_tree.sh with all its checks, laid on top of the public repo's own history (no force push).
#
#   tools/public_update.sh "New Blitz 1.0.0"            # build, commit in build/public/update, show what changed
#   tools/public_update.sh "New Blitz 1.0.0" --push     # ... and push it (as blitzrecomp)
#
# The public account's gh login lives in build/gh_public (GH_CONFIG_DIR), apart from this Mac's own accounts.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
MSG=${1:?give the commit message}
PUSH=${2:-}
OUT=$ROOT/build/public/blitzrecomp
WORK=$ROOT/build/public/update
AUTHOR=$(cat "$ROOT/build/public_author")
export GH_CONFIG_DIR=$ROOT/build/gh_public
GIT_CRED=(-c credential.helper= -c "credential.helper=!gh auth git-credential")

tools/make_public_tree.sh --author "$AUTHOR"
rm -rf "$WORK"
git "${GIT_CRED[@]}" clone -q https://github.com/blitzrecomp/newblitz.git "$WORK"
rsync -a --delete --exclude .git "$OUT/" "$WORK/"
cd "$WORK"
git add -A
# The submodules at this checkout's pins, as gitlinks (the sanitized tree carries no submodule folders).
(cd "$ROOT" && git submodule status) | while read -r sha path _; do
    sha=${sha#[-+U]}
    rm -rf "$path"
    git update-index --add --cacheinfo 160000,"$sha","$path"
done
if git diff --cached --quiet; then echo "the public repo is already up to date"; exit 0; fi
git diff --cached --stat | tail -15
git -c user.name="${AUTHOR% <*}" -c user.email="$(echo "$AUTHOR" | sed -E 's/.*<(.*)>.*/\1/')" commit -q -m "$MSG"
echo "committed in $WORK: $(git log --oneline -1)"
if [ "$PUSH" = --push ]; then
    git "${GIT_CRED[@]}" push -q origin main
    echo "pushed to github.com/blitzrecomp/newblitz"
else
    echo "not pushed: review $WORK, then run again with --push"
fi
