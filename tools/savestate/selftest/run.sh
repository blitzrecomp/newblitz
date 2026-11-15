#!/bin/sh
# Transform the fixture with resumable.py, build it with the harness, and run the
# snapshot-at-every-sleep / resume-on-a-new-thread comparison.
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
T=$R/tools/savestate
O=$R/build/savestate/selftest
rm -rf "$O" && mkdir -p "$O"
python3 "$T/resumable.py" --in "$T/selftest/fixture" --out "$O/out" --rom '' --report "$O/report.json" > "$O/summary.json"
python3 - "$O/report.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
live = r['locals_live_across_instrumented_calls']
assert any(x['func'] == 'live_bad' and 'lo' in x['live'] for x in live), 'liveness positive control not flagged'
assert all(x['func'] == 'live_bad' for x in live), live
print('liveness check: flags live_bad (lo live across a sleeping call), nothing else')
print('S =', r['S_used'], 'functions,', r['sites_total'], 'sites:', r['sites_by_kind'])
EOF
/usr/bin/clang -O2 -std=gnu17 -Wno-unused-variable -I"$O/out" -I"$R/lib/N64ModernRuntime/N64Recomp/include" \
    "$T/selftest/harness.c" "$O/out/funcs_0.c" "$O/out/ss_site_funcs.c" -o "$O/harness" -lpthread
"$O/harness"
