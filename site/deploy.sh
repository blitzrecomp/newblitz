#!/usr/bin/env bash
# Deploy the website to Cloudflare Pages, project "newblitz-site" (newblitz.com), on the public account.
#
#   site/deploy.sh            # bake (tools/build.py), stage the public files, deploy
#   site/deploy.sh --dry-run  # bake and stage only; prints the staging folder
#
# Uses the public account's own wrangler login (XDG_CONFIG_HOME=build/wrangler_public), never the private one.
# Only the published files go up: the tools, this script, README.md and site.json stay behind.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SITE="$ROOT/site"

python3 "$SITE/tools/build.py"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/newblitz-site.XXXXXX")"
trap '[ "${KEEP_STAGE:-}" = 1 ] || rm -rf "$STAGE"' EXIT
rsync -a --exclude '.DS_Store' --exclude 'tools/' --exclude 'deploy.sh' --exclude 'README.md' --exclude 'site.json' \
    "$SITE/" "$STAGE/"

echo "staged $(find "$STAGE" -type f | wc -l | tr -d ' ') files, $(du -sh "$STAGE" | cut -f1) in $STAGE"
if [ "${1:-}" = "--dry-run" ]; then
    KEEP_STAGE=1
    echo "dry run: not deployed (the staging folder is kept)"
    exit 0
fi

# Cloudflare serves Pages sites as Workers with static assets now (wrangler 4.16x): the staged files are the Worker
# "newblitz-site"'s assets, on newblitz.com (and www) and newblitz-site.blitzrecomp.workers.dev. _headers and
# _redirects work as they did on Pages. The config lives beside the staged files, never in the repo.
PUB="$STAGE/public"; mkdir "$PUB"
find "$STAGE" -mindepth 1 -maxdepth 1 ! -name public -exec mv {} "$PUB/" \;
cat > "$STAGE/wrangler.jsonc" <<'JSON'
{
  "name": "newblitz-site",
  "compatibility_date": "2026-11-13",
  "workers_dev": true,
  "assets": { "directory": "./public", "not_found_handling": "404-page" },
  "routes": [
    { "pattern": "newblitz.com", "custom_domain": true },
    { "pattern": "www.newblitz.com", "custom_domain": true }
  ]
}
JSON
cd "$STAGE"
XDG_CONFIG_HOME="$ROOT/build/wrangler_public" npx wrangler@4 deploy
