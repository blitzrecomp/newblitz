#!/bin/bash
# gpr32 differential test (docs/PSP_PORT.md section 3.5): every construct class N64Recomp emits, in its generated
# form, under the stock recomp.h (64-bit gpr), RECOMP_GPR32 and RECOMP_GPR32 + RECOMP_RDRAM_MASK, on the Mac.
# Needs the patched runtime recomp.h (make patches). Usage: tools/psp/difftest/run.sh [iterations, default 2000000]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/build/psp/difftest"; mkdir -p "$OUT"
H="$ROOT/lib/N64ModernRuntime/N64Recomp/include"
python3 "$ROOT/tools/psp/difftest/gen.py" "$OUT/ops.c"
F="-O2 -fwrapv -fno-strict-aliasing -w"
for v in "s:" "g:-DRECOMP_GPR32=1" "m:-DRECOMP_GPR32=1 -DRECOMP_RDRAM_MASK"; do
    t=${v%%:*}; d=${v#*:}
    clang $F -I"$H" $d -DRUN=RUN_$t -DRUNW=RUNW_$t -DNOPSF=NOPSF_$t -DOPNAMEF=OPNAMEF_$t -c "$OUT/ops.c" -o "$OUT/$t.o"
done
clang -O2 "$ROOT/tools/psp/difftest/main.c" "$OUT/s.o" "$OUT/g.o" "$OUT/m.o" -o "$OUT/difftest"
"$OUT/difftest" "${1:-2000000}"
