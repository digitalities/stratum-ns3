#!/usr/bin/env bash
# reproduce-paper-stratum.sh — Stratum ns-3 paper reproduction orchestrator.
#
# Runs every ns-3-side figure and table entry point in sequence, records a
# PASS / FAIL / SKIP verdict for each, and writes a summary table to
# output/paper/reproduction-status.md.  Each step is an independent entry
# point that also works standalone; this script strings them together.
#
# The optional three-way cross-simulator comparison (ns-2 vs ns-3) requires
# ns-2 baseline traces produced by the companion diffserv4ns repository.
# See --ns2-baselines / --fetch-ns2-baselines below.
#
# Prerequisite: a built ns-3 tree with the Stratum module. From a clean
# checkout:
#
#   ./scripts/fetch-ns3.sh
#   cd ns3/ns-3-dev && ./ns3 configure --enable-tests --enable-examples
#   ./ns3 build stratum && cd -
#
# Usage:
#   ./scripts/reproduce-paper.sh                             # full ns-3-side reproduction
#   ./scripts/reproduce-paper.sh --smoke                     # fast: test suites only
#   ./scripts/reproduce-paper-stratum.sh --list                      # list steps and exit
#   ./scripts/reproduce-paper-stratum.sh --ns2-baselines <path>      # include three-way comparison
#   ./scripts/reproduce-paper-stratum.sh --fetch-ns2-baselines       # (see note below)
#
# --ns2-baselines <path>
#   Path to a directory that mirrors the diffserv4ns output/ tree layout:
#     <path>/output/ns2-29/example-1/
#     <path>/output/ns2-35/example-1/
#     <path>/output/ns2/example-2-fullscale/
#     ... (see scripts/compare-three-way.py --help for the full list)
#   Produced by scripts/reproduce-ns2-baselines.sh in the diffserv4ns
#   repository (https://github.com/digitalities/diffserv4ns).
#   When this flag is given the three-way comparison step is enabled.
#
# --fetch-ns2-baselines
#   Reserved for a future pinned-tag auto-fetch from the diffserv4ns
#   repository.  Currently exits with an error — use --ns2-baselines
#   with locally produced traces instead.
#
# Environment:
#   NS3_DEV   Path to the built ns-3-dev tree.  Default: ns3/ns-3-dev
# ----------------------------------------------------------------------

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Prefer the lifted layout (examples at repo root) then fall back to the
# dev-monorepo layout (examples under src/ns-3/).
if [ -d "$REPO_ROOT/examples" ]; then
    _EXAMPLES_BASE="$REPO_ROOT/examples"
else
    _EXAMPLES_BASE="$REPO_ROOT/src/ns-3/examples"
fi

# Locate the built ns-3 tree: explicit NS3_DEV wins; otherwise probe the
# known layouts (monorepo, version-named sibling, generic sibling, parent
# tree when this module is installed as contrib/stratum).
if [ -z "${NS3_DEV:-}" ]; then
    if   [ -f "$REPO_ROOT/ns3/ns-3-dev/test.py" ]; then NS3_DEV="$REPO_ROOT/ns3/ns-3-dev"
    elif [ -f "$REPO_ROOT/../ns-3/test.py" ];      then NS3_DEV="$REPO_ROOT/../ns-3"
    elif [ -f "$REPO_ROOT/../ns-3.48/test.py" ];   then NS3_DEV="$REPO_ROOT/../ns-3.48"  # legacy name
    elif [ -f "$REPO_ROOT/../ns-3-dev/test.py" ];  then NS3_DEV="$REPO_ROOT/../ns-3-dev"
    elif [ -f "$REPO_ROOT/../../test.py" ];        then NS3_DEV="$REPO_ROOT/../.."
    else NS3_DEV="$REPO_ROOT/ns3/ns-3-dev"
    fi
fi
# The scenario runner ships under a different name per layout: the dev
# monorepo carries run-scenario-stratum.sh; the public repo renames it
# to run-scenario.sh at mirror time. Probe for the dev name first.
RUN_SCENARIO="$REPO_ROOT/scripts/run-scenario-stratum.sh"
[ -f "$RUN_SCENARIO" ] || RUN_SCENARIO="$REPO_ROOT/scripts/run-scenario.sh"

STATUS_DIR="$REPO_ROOT/output/paper"
STATUS_MD="$STATUS_DIR/reproduction-status.md"

MODE="full"
NS2_BASELINES=""
FETCH_NS2_BASELINES=false

while [ $# -gt 0 ]; do
    case "$1" in
        --smoke)               MODE="smoke"; shift ;;
        --full)                MODE="full";  shift ;;
        --list)                MODE="list";  shift ;;
        --ns2-baselines)
            if [ $# -lt 2 ]; then
                echo "ERROR: --ns2-baselines requires a path argument." >&2; exit 1
            fi
            NS2_BASELINES="$2"; shift 2 ;;
        --fetch-ns2-baselines) FETCH_NS2_BASELINES=true; shift ;;
        --help|-h) grep '^# ' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

# --fetch-ns2-baselines is reserved for a future pinned-tag fetch.
# The companion script does not yet ship there; instruct the user.
if [ "$FETCH_NS2_BASELINES" = "true" ]; then
    echo "ERROR: --fetch-ns2-baselines is not yet available." >&2
    echo "" >&2
    echo "The ns-2 baseline producer has not yet been published to the" >&2
    echo "diffserv4ns repository (https://github.com/digitalities/diffserv4ns)." >&2
    echo "" >&2
    echo "To include the three-way comparison, produce ns-2 baselines locally:" >&2
    echo "  1. Clone https://github.com/digitalities/diffserv4ns" >&2
    echo "  2. Follow its REPRODUCIBILITY.md to build ns-2.29 / ns-2.35 via Docker." >&2
    echo "  3. Run:  ./scripts/reproduce-ns2-baselines.sh" >&2
    echo "  4. Pass the repo root as:  --ns2-baselines /path/to/diffserv4ns" >&2
    exit 1
fi

# -- Step registry -------------------------------------------------------
# Each step is "tier|label|command".  tier=smoke runs in both modes;
# tier=full runs only in full mode.
STEPS=(
    "smoke|Core suites + RFC 2697/2698/2859 meter conformance vectors|cd \"\$NS3_DEV\" && python3 test.py -s stratum && python3 test.py -s stratum-per-flow-classifier"
    "smoke|L4S DualPI2 coupled marking + RFC 9331/9332 vectors|cd \"\$NS3_DEV\" && python3 test.py -s stratum-l4s"
    "smoke|CAKE calibration suite|cd \"\$NS3_DEV\" && python3 test.py -s stratum-cake-q15"
    "full|Scenario 1/2/3 ns-3 simulation|bash \"\$RUN_SCENARIO\" example-1 && bash \"\$RUN_SCENARIO\" example-2 && bash \"\$RUN_SCENARIO\" example-3 --sim-time 60"
    "full|Scheduler GPS-convergence (Chang) sweep|bash \"\$REPO_ROOT/scripts/run-q16-chang-sweep.sh\""
    "full|Parekh-Gallager Theorem 1 latency-bound gate|bash \"\$REPO_ROOT/scripts/run-q17-parekh-gate.sh\""
    "full|AQM characterisation envelope (13 AQMs x 9 scenarios)|bash \"\$REPO_ROOT/scripts/aqm-eval/aqm-eval\" matrix && bash \"\$REPO_ROOT/scripts/aqm-eval/aqm-eval\" plot"
    "full|CAKE + L4S composition fairness|bash \"\$REPO_ROOT/scripts/l4s-cake-composition-fairness-sweep.sh\""
)

# Append the three-way comparison step only when baselines are available.
if [ -n "$NS2_BASELINES" ]; then
    STEPS+=(
        "full|Three-way cross-simulator comparison (ns-2 vs ns-3)|python3 \"\$REPO_ROOT/scripts/compare-three-way.py\" --all --repo-root \"\$NS2_BASELINES\" --ns3-root \"\$REPO_ROOT\" --out-dir \"\$REPO_ROOT/output/three-way-figures\""
    )
fi

# Summary of what is NOT reproduced.
print_coverage_note() {
    echo ""
    echo "Not reproduced by this script (heavier prerequisites):"
    if [ -z "$NS2_BASELINES" ]; then
        echo "  - Three-way ns-2 <-> ns-3 cross-simulator comparison: pass"
        echo "    --ns2-baselines <path> (produced by scripts/reproduce-ns2-baselines.sh"
        echo "    in https://github.com/digitalities/diffserv4ns) to enable this step."
    fi
    echo "  - Stratum vs Linux CAKE cross-validation (host-fairness anchor,"
    echo "    trace-replay, stratum-bridge): needs a Lima VM with sch_cake."
    echo "    See REPRODUCIBILITY.md for instructions."
}

if [ "$MODE" = "list" ]; then
    echo "Steps (tier shown in brackets):"
    for s in "${STEPS[@]}"; do
        IFS='|' read -r tier label _ <<< "$s"
        echo "  [$tier] $label"
    done
    echo ""
    echo "smoke = run with --smoke; full mode (default) runs both tiers."
    print_coverage_note
    exit 0
fi

# If no baselines path was given, print a note before starting so the user
# knows the three-way step is omitted.
if [ -z "$NS2_BASELINES" ]; then
    echo "NOTE: --ns2-baselines not provided; the three-way ns-2 <-> ns-3"
    echo "      comparison step will be skipped.  Pass --ns2-baselines <path>"
    echo "      (produced by scripts/reproduce-ns2-baselines.sh in the"
    echo "      diffserv4ns repository) to enable it."
    echo ""
fi

# -- Pre-flight ----------------------------------------------------------
if [ ! -f "$NS3_DEV/test.py" ]; then
    echo "ERROR: no built ns-3 tree at $NS3_DEV" >&2
    echo "Run ./scripts/fetch-ns3.sh and build the Stratum module first" >&2
    echo "(see the header of this script, or REPRODUCIBILITY.md)." >&2
    exit 1
fi

fq_cobalt="$NS3_DEV/src/traffic-control/model/fq-cobalt-queue-disc.h"
if [ ! -f "$fq_cobalt" ] || ! grep -q 'HostIsolationMode' "$fq_cobalt"; then
    echo "ERROR: the ns-3 tree at $NS3_DEV is missing the required local patches" >&2
    echo "(FqCobaltQueueDisc::HostIsolationMode not found); the Stratum module will" >&2
    echo "not compile. Run ./scripts/fetch-ns3.sh to fetch ns-3-dev at the pinned" >&2
    echo "revision and apply the patches, then rebuild before re-running." >&2
    exit 1
fi
mkdir -p "$STATUS_DIR"

# Validate ns2-baselines path when provided.
if [ -n "$NS2_BASELINES" ]; then
    if [ ! -d "$NS2_BASELINES" ]; then
        echo "ERROR: --ns2-baselines path does not exist: $NS2_BASELINES" >&2
        exit 1
    fi
    # Spot-check at least one expected subdirectory.
    if [ ! -d "$NS2_BASELINES/output/ns2-29" ] && [ ! -d "$NS2_BASELINES/output/ns2-35" ]; then
        echo "ERROR: --ns2-baselines path does not look like a baselines root." >&2
        echo "Expected subdirectories: output/ns2-29/, output/ns2-35/" >&2
        echo "Got: $NS2_BASELINES" >&2
        exit 1
    fi
fi

# -- Run -----------------------------------------------------------------
declare -a RESULT_LABEL RESULT_VERDICT
fail_count=0

run_step() {
    local label="$1" cmd="$2" verdict
    echo ""
    echo "=== $label ==="
    if eval "$cmd"; then
        verdict="PASS"
    else
        verdict="FAIL"
        fail_count=$((fail_count + 1))
    fi
    echo "--- $label: $verdict ---"
    RESULT_LABEL+=("$label")
    RESULT_VERDICT+=("$verdict")
}

for s in "${STEPS[@]}"; do
    IFS='|' read -r tier label cmd <<< "$s"
    if [ "$MODE" = "smoke" ] && [ "$tier" != "smoke" ]; then
        RESULT_LABEL+=("$label"); RESULT_VERDICT+=("SKIP")
        continue
    fi
    run_step "$label" "$cmd"
done

# -- Summary -------------------------------------------------------------
{
    echo "# Reproduction status"
    echo ""
    echo "Mode: \`$MODE\` — generated by \`scripts/reproduce-paper-stratum.sh\`."
    echo ""
    echo "| Step | Verdict |"
    echo "|------|---------|"
    for i in "${!RESULT_LABEL[@]}"; do
        echo "| ${RESULT_LABEL[$i]} | ${RESULT_VERDICT[$i]} |"
    done
} > "$STATUS_MD"

echo ""
echo "============================================================"
echo "  Reproduction summary  (mode: $MODE)"
echo "============================================================"
for i in "${!RESULT_LABEL[@]}"; do
    printf "  %-6s %s\n" "${RESULT_VERDICT[$i]}" "${RESULT_LABEL[$i]}"
done
echo "------------------------------------------------------------"
echo "  Summary written to ${STATUS_MD#"$REPO_ROOT"/}"
echo "============================================================"
print_coverage_note

[ "$fail_count" -eq 0 ] || { echo "$fail_count step(s) FAILED." >&2; exit 1; }
echo "All executed steps PASSED."
