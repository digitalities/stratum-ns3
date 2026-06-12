#!/usr/bin/env bash
# Q-17 Parekh-Gallager 1993 Theorem 1 conformance — visual + numerical
# audit runner.
#
# Invokes the stratum-q17-parekh-theorem1 test fixture, captures its
# stderr report, and renders a 3-panel summary chart + CSV. Companion
# to scripts/run-q16-chang-sweep.sh + scripts/plot-q16-chang.py.
#
# Q-17 is wall-clock cheap (~5 s simulated × 15 cases ≈ 1 min total)
# so a fresh sweep + plot is appropriate at every release tag.
#
# Output: output/chang-comparison/q17-parekh-theorem1.png
#         output/chang-comparison/q17-summary.csv

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

# Build the test binary if needed.
./ns3 build stratum-test >/dev/null

REPORT_FILE="$OUT_DIR/q17-report.txt"

echo "Running Q-17 fixture (EXTENSIVE) ..."
"$(ls -t "$NS3_DIR"/build/utils/ns3*-test-runner-default | head -1)" \
    --test-name=stratum-q17-parekh-theorem1 \
    --fullness=EXTENSIVE 2>&1 \
    | grep '\[Q-17\.1' \
    | sort -u \
    > "$REPORT_FILE"

cases="$(wc -l < "$REPORT_FILE" | tr -d ' ')"
echo "Captured $cases (scheduler, regime) cases to $REPORT_FILE"

cd "$REPO_ROOT"
python3 scripts/plot-q17-parekh.py "$REPORT_FILE" --out-dir "$OUT_DIR"

echo
echo "Q-17 audit complete."
echo "  Report:  $REPORT_FILE"
echo "  CSV:     $OUT_DIR/q17-summary.csv"
echo "  Plot:    $OUT_DIR/q17-parekh-theorem1.png"
