#!/usr/bin/env bash
# Q-16 chang2015 GPS-convergence sweep.
#
# Runs the chang-comparison example across:
#   schedulers: WFQ WRR WF2Q+ SCFQ SFQ           (5)
#   data rates: 0.5 1 10 50 Mbps                 (4)
#   weight ratios: 1 2 7 10                      (4)
# = 80 total runs, ~5-15 minutes wall-clock depending on host.
#
# Output: output/chang-comparison/<sched>-T<T*1000>-R<R>/summary.txt
#         per Chang's measurement convention (steady-state mean over
#         the second half of a 300 s simulation).
#
# After completion, run:
#   python3 scripts/plot-q16-chang.py
# to verify Q-16.1 / Q-16.2 envelopes and produce the convergence
# panels for the paper appendix.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NS3_DIR="$REPO_ROOT/ns3/ns-3-dev"
OUT_DIR="$REPO_ROOT/output/chang-comparison"

if [[ ! -d "$NS3_DIR" ]]; then
    echo "error: ns-3 tree not found at $NS3_DIR" >&2
    echo "       run scripts/fetch-ns3.sh first" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"

cd "$NS3_DIR"
./ns3 build chang-comparison >/dev/null

SCHEDULERS=(WFQ WRR "WF2Q+" SCFQ SFQ)
DATA_RATES=(0.5 1 10 50)
WEIGHT_RATIOS=(1 2 7 10)

total=$((${#SCHEDULERS[@]} * ${#DATA_RATES[@]} * ${#WEIGHT_RATIOS[@]}))
i=0
fail=0

for S in "${SCHEDULERS[@]}"; do
    for T in "${DATA_RATES[@]}"; do
        for R in "${WEIGHT_RATIOS[@]}"; do
            i=$((i + 1))
            label="$S-T${T}-R${R}"
            printf "[%3d/%d] %-22s ... " "$i" "$total" "$label"
            if ./ns3 run "chang-comparison --scheduler=$S --dataRate=$T --weightRatio=$R --outputDir=$OUT_DIR" >/dev/null 2>&1; then
                err=$(awk '/^error_pct/ {print $2}' "$OUT_DIR/$S-T$(awk -v t="$T" 'BEGIN{printf "%d",t*1000}')-R$R/summary.txt" 2>/dev/null || echo "n/a")
                printf "ok  (err=%s%%)\n" "$err"
            else
                fail=$((fail + 1))
                printf "FAIL\n"
            fi
        done
    done
done

echo
echo "Q-16 sweep complete: $((total - fail))/$total runs ok"
echo "Artefacts: $OUT_DIR"
echo "Next: python3 scripts/plot-q16-chang.py"
[[ $fail -eq 0 ]] || exit 1
