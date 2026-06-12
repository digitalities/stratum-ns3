#!/usr/bin/env bash
# run-scenario-stratum.sh — ns-3 scenario runner for Stratum.
#
# Usage:
#   scripts/run-scenario-stratum.sh <scenario> [--sim-time <sec>] [--extra-flags "..."]
#
# <scenario>:
#   example-1            — Scenario 1 (PQ EF/BE, CBR traffic)
#   example-2            — Scenario 2 small-scale (PQ/SCFQ/LLQ, Telnet+FTP+CBR)
#   example-2-fullscale  — Scenario 2 full-scale (469 nodes, WRED sweep)
#   example-3            — Scenario 3 (LLQ+SFQ, Premium/Gold/Silver/Bronze/BE)
#
# Options:
#   --sim-time <sec>     Override simulation time (passed to the scenario if supported)
#   --extra-flags "..."  Additional flags forwarded to the ns-3 example
#   --print-outdir       Print the computed output directory and exit (no side effects)
#
# Output:
#   output/ns3/<scenario>/   (created if absent, cleaned if present)
#
# Exit code: 0 on success, non-zero on failure.
#
# ns-3 tree lookup
# ----------------
# The script tries two layouts in order:
#   1. $NS3_DEV when set; otherwise the layouts probed below (sibling
#      ns-3/ (or a legacy version-named ns-3.NN/), sibling ns-3-dev/, or
#      the parent tree under contrib/).
#   2. Stratum standalone layout: ns3-dev adjacent to or specified by NS3_DEV.
# Override by setting NS3_DEV to the absolute path of the built ns-3 tree.

set -euo pipefail
cd "$(dirname "$0")/.."

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
if [ $# -lt 1 ]; then
    echo "Usage: $0 <scenario> [--sim-time <sec>] [--extra-flags \"...\"] [--print-outdir]" >&2
    exit 1
fi

SCENARIO="$1"; shift

SIM_TIME_OVERRIDE=""
EXTRA_FLAGS=""
PRINT_OUTDIR=false

while [ $# -gt 0 ]; do
    case "$1" in
        --sim-time)     SIM_TIME_OVERRIDE="$2"; shift 2 ;;
        --extra-flags)  EXTRA_FLAGS="$2"; shift 2 ;;
        --print-outdir) PRINT_OUTDIR=true; shift ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate inputs
# ---------------------------------------------------------------------------
VALID_SCENARIOS="example-1 example-2 example-2-fullscale example-3"

_valid=false
for _s in $VALID_SCENARIOS; do
    if [ "$_s" = "$SCENARIO" ]; then _valid=true; break; fi
done
if [ "$_valid" = "false" ]; then
    echo "ERROR: Unknown scenario '$SCENARIO'. Valid: $VALID_SCENARIOS" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Output directory
# ---------------------------------------------------------------------------
OUTDIR="output/ns3/${SCENARIO}"

if [ "$PRINT_OUTDIR" = "true" ]; then
    echo "$OUTDIR"
    exit 0
fi

mkdir -p "$OUTDIR"
# Clean previous run contents (not the directory itself).
find "$OUTDIR" -mindepth 1 -maxdepth 1 -delete 2>/dev/null || true

echo "=== run-scenario-stratum.sh: scenario=$SCENARIO ==="
echo "    Output: $OUTDIR"
[ -n "$SIM_TIME_OVERRIDE" ] && echo "    sim-time override: ${SIM_TIME_OVERRIDE}s"
echo ""

# ---------------------------------------------------------------------------
# ns-3 dispatch
# ---------------------------------------------------------------------------

# Locate the built ns-3 tree: explicit NS3_DEV wins; otherwise probe the
# known layouts (monorepo, version-named sibling, generic sibling, parent
# tree when this module is installed as contrib/stratum).
if [ -z "${NS3_DEV:-}" ]; then
    if   [ -f "$(pwd)/ns3/ns-3-dev/test.py" ]; then NS3_DEV="$(pwd)/ns3/ns-3-dev"
    elif [ -f "$(pwd)/../ns-3/test.py" ];      then NS3_DEV="$(pwd)/../ns-3"
    elif [ -f "$(pwd)/../ns-3.48/test.py" ];   then NS3_DEV="$(pwd)/../ns-3.48"  # legacy name
    elif [ -f "$(pwd)/../ns-3-dev/test.py" ];  then NS3_DEV="$(pwd)/../ns-3-dev"
    elif [ -f "$(pwd)/../../test.py" ];        then NS3_DEV="$(pwd)/../.."
    else NS3_DEV="$(pwd)/ns3/ns-3-dev"
    fi
fi

run_ns3() {
    local NS3_EXAMPLE=""
    local NS3_ARGS="--outputDir=$(pwd)/$OUTDIR"

    case "$SCENARIO" in
        example-1)
            NS3_EXAMPLE="diffserv-example-1"
            [ -n "$SIM_TIME_OVERRIDE" ] && NS3_ARGS="$NS3_ARGS --simTime=$SIM_TIME_OVERRIDE"
            ;;
        example-2)
            NS3_EXAMPLE="diffserv-example-2"
            [ -n "$SIM_TIME_OVERRIDE" ] && NS3_ARGS="$NS3_ARGS --simTime=$SIM_TIME_OVERRIDE"
            ;;
        example-2-fullscale)
            NS3_EXAMPLE="diffserv-example-2"
            NS3_ARGS="$NS3_ARGS --scale=full"
            [ -n "$SIM_TIME_OVERRIDE" ] && NS3_ARGS="$NS3_ARGS --simTime=$SIM_TIME_OVERRIDE"
            ;;
        example-3)
            NS3_EXAMPLE="diffserv-example-3"
            [ -n "$SIM_TIME_OVERRIDE" ] && NS3_ARGS="$NS3_ARGS --simTime=$SIM_TIME_OVERRIDE"
            ;;
    esac

    [ -n "$EXTRA_FLAGS" ] && NS3_ARGS="$NS3_ARGS $EXTRA_FLAGS"

    if [ ! -d "$NS3_DEV" ]; then
        echo "ERROR: ns-3 tree not found at $NS3_DEV" >&2
        echo "Run ./scripts/fetch-ns3.sh (or set NS3_DEV to the built ns-3 tree path)." >&2
        exit 1
    fi

    echo "  Running ns-3 example: $NS3_EXAMPLE"
    echo "  Args: $NS3_ARGS"
    echo ""

    local RC=0
    (cd "$NS3_DEV" && ./ns3 run "$NS3_EXAMPLE $NS3_ARGS" 2>&1) \
        | tee "$OUTDIR/ns3-stdout.log" || RC=${PIPESTATUS[0]:-$?}

    local STDOUT_LINES
    STDOUT_LINES=$(wc -l < "$OUTDIR/ns3-stdout.log" 2>/dev/null || echo 0)

    echo ""
    if [ $RC -eq 0 ]; then
        echo "PASS: scenario=$SCENARIO (stdout $STDOUT_LINES lines)"
    else
        echo "FAIL: scenario=$SCENARIO (exit code $RC)"
    fi

    return $RC
}

run_ns3
