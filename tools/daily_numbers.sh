#!/bin/bash
# How New Blitz is doing, in one line a day: tools/daily_numbers.sh [DAYS]   (default: the last 7 days)
#
# Totals only, nothing about anyone (site/privacy.html promises no analytics):
#   - GitHub: downloads of each release's apps (SHA256SUMS left out), stars, forks, open issues, and GitHub's own
#     14-day page views (gh with the public account: GH_CONFIG_DIR=build/gh_public)
#   - the season server's daily counts (tools/rosters_hub_worker, GET /<key>/stats): roster_checks (an installed copy
#     asks at start and every few hours, so roughly "copies in use"), season_catalog, season_downloads, crash_reports
# Each run also appends a dated line to build/numbers.log. YouTube, X and Discord have no free API worth wiring in:
# look at them in their own pages. Our own checks of the server count too (a few a day).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DAYS="${1:-7}"
export GH_CONFIG_DIR="$ROOT/build/gh_public"
REPO=blitzrecomp/newblitz

gh_line=$(gh api "repos/$REPO/releases" --jq '[.[] | {tag: .tag_name, n: ([.assets[] | select(.name != "SHA256SUMS") | .download_count] | add // 0)}]
  | map("\(.tag) \(.n)") | join(", ")' 2>/dev/null || echo "?")
repo_line=$(gh api "repos/$REPO" --jq '"stars \(.stargazers_count), forks \(.forks_count), open issues \(.open_issues_count)"' 2>/dev/null || echo "?")
views_line=$(gh api "repos/$REPO/traffic/views" --jq '"\(.count) views (\(.uniques) people), last 14 days"' 2>/dev/null || echo "?")

stats='[]'
if [ -s "$ROOT/build/pak_server" ] && [ -s "$ROOT/build/pak_hub_key" ]; then
    base=$(sed -E 's|/paks/?$||' "$ROOT/build/pak_server" | tr -d '\n')
    key=$(tr -d '\n' < "$ROOT/build/pak_hub_key")
    stats=$(curl -fsS --max-time 15 "$base/$key/stats" 2>/dev/null || echo '[]')
fi

echo "GitHub downloads (all apps, per release): ${gh_line:-none}"
echo "GitHub: $repo_line; $views_line"
echo
STATS="$stats" DAYS="$DAYS" python3 - <<'EOF'
import json, os, datetime
rows = json.loads(os.environ["STATS"] or "[]")
days = int(os.environ["DAYS"])
kinds = ["roster_checks", "season_catalog", "season_downloads", "crash_reports"]
by = {}
for r in rows:
    by.setdefault(r["day"], {})[r["kind"]] = r["n"]
today = datetime.date.today()
print(f"{'day':<11} {'roster checks':>13} {'Add-Ons/season views':>21} {'season downloads':>17} {'crash reports':>14}")
for i in range(days - 1, -1, -1):
    d = (today - datetime.timedelta(days=i)).isoformat()
    c = by.get(d, {})
    print(f"{d:<11} {c.get('roster_checks', 0):>13} {c.get('season_catalog', 0):>21} {c.get('season_downloads', 0):>17} {c.get('crash_reports', 0):>14}")
total = {k: sum(v.get(k, 0) for v in by.values()) for k in kinds}
print(f"{'all (60 d)':<11} {total['roster_checks']:>13} {total['season_catalog']:>21} {total['season_downloads']:>17} {total['crash_reports']:>14}")
EOF

today=$(date +%F)
c=$(STATS="$stats" python3 -c 'import json,os,datetime; d=datetime.date.today().isoformat(); r={x["kind"]:x["n"] for x in json.loads(os.environ["STATS"] or "[]") if x["day"]==d}; print(" ".join(f"{k}={r.get(k,0)}" for k in ["roster_checks","season_catalog","season_downloads","crash_reports"]))')
echo "$today github[$gh_line] $repo_line; $views_line; server $c" >> "$ROOT/build/numbers.log"
