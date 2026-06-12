#!/usr/bin/env bash
# Sweep runner for the CAKE Flent figure pack.
#
# Produces a (rate × RTT) matrix of .flent.gz bundles per figure under:
#   <out_root>/fig4-tcp-4up-squarewave/<rate>-<rtt>/
#   <out_root>/fig5-rrul-diffserv/<rate>-<rtt>/
#   <out_root>/fig6-host-isolation/<isolation>/<rate>-<rtt>/
#
# Usage:
#   bash scripts/flent-export/run-figure-pack.sh [<out_root>]
# Default <out_root> = output/cake-flent-figure-pack
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NS3_DIR="${REPO_ROOT}/ns3/ns-3-dev"
EXPORT_DIR="${REPO_ROOT}/scripts/flent-export"

OUT_ROOT="${1:-${REPO_ROOT}/output/cake-flent-figure-pack}"
mkdir -p "${OUT_ROOT}"

RATES=(10Mbps 100Mbps 1000Mbps)
RTTS=(10ms 40ms 100ms)

run_and_convert() {
    local example="$1" test="$2" outdir="$3" extra_args="${4:-}"
    pushd "${NS3_DIR}" >/dev/null
    ./ns3 run "${example} --output=${outdir}/ ${extra_args}" >"${outdir}.log" 2>&1
    popd >/dev/null
    pushd "${EXPORT_DIR}" >/dev/null
    # shellcheck disable=SC1091
    source .venv/bin/activate
    ns3-csv-to-flent --test "${test}" --indir "${outdir}" --output "${outdir}.flent.gz"
    deactivate
    popd >/dev/null
}

# Fig 4: TCP 4-up square wave
for rate in "${RATES[@]}"; do
    for rtt in "${RTTS[@]}"; do
        outdir="${OUT_ROOT}/fig4-tcp-4up-squarewave/${rate}-${rtt}"
        mkdir -p "${outdir}"
        run_and_convert "cake-tcp-4up-squarewave" "tcp_4up_squarewave" \
            "${outdir}" "--bw=${rate} --rtt=${rtt} --length=60"
    done
done

# Fig 5: RRUL with DiffServ marking
for rate in "${RATES[@]}"; do
    for rtt in "${RTTS[@]}"; do
        outdir="${OUT_ROOT}/fig5-rrul-diffserv/${rate}-${rtt}"
        mkdir -p "${outdir}"
        run_and_convert "cake-rrul-diffserv" "rrul" \
            "${outdir}" "--bw=${rate} --rtt=${rtt} --length=60"
    done
done

# Fig 6: host isolation, both modes
for isolation in triple flowblind; do
    for rate in "${RATES[@]}"; do
        for rtt in "${RTTS[@]}"; do
            outdir="${OUT_ROOT}/fig6-host-isolation/${isolation}/${rate}-${rtt}"
            mkdir -p "${outdir}"
            run_and_convert "cake-host-isolation" "host_isolation" \
                "${outdir}" "--isolation=${isolation} --bw=${rate} --rtt=${rtt} --length=30"
        done
    done
done

echo "Figure pack written to: ${OUT_ROOT}"
echo "Total cells: $((${#RATES[@]} * ${#RTTS[@]} * 4))   (3 fig + 1 isolation toggle)"
echo "Run flent --plot=... -i <bundle>.flent.gz to render."
