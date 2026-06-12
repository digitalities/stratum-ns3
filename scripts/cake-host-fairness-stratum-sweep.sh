#!/usr/bin/env bash
# Stratum host-fairness sweep driver.
#
# Two modes:
#
#   MODE=phase-1    (default — canonical-sweep shape)
#     Loops 12 (N, M) cells x 2 strategies x 3 RNG replicas at TCP CUBIC.
#
#   MODE=phase-1-5  (protocol-axis sweep)
#     Loops PHASE_1_5_CELLS x 4 protocols x 3 RNG replicas at strategy
#     pinned to "nested" (the canonical-sweep measured Nested == Flat byte-identical
#     across asymmetric cells; cross-strategy axis collapsed for protocol-robustness).
#
# Common env overrides:
#   REPO_ROOT, OUT_DIR, OUT_CSV, PEER_NS3
#
# protocol-robustness env overrides:
#   PHASE_1_5_CELLS  pipe-separated cell list (default: "4 1|4 4")
#   PROTOCOLS        space-separated protocol list (default: "cubic newreno bbr udp")

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
# Locate the ns-3-dev build tree. Default: the ns3/ns-3-dev created by
# scripts/fetch-ns3.sh inside this checkout. Multi-checkout layouts can set
# MAIN_REPO (the checkout that holds the ns3/ trees) or PEER_NS3 directly.
if [[ -n "${MAIN_REPO:-}" ]]; then
  WORKTREE_NAME="$(basename "${REPO_ROOT}")"
  if [[ "${REPO_ROOT}" == "${MAIN_REPO}" ]]; then
    PEER_NS3_DEFAULT="${MAIN_REPO}/ns3/ns-3-dev"
  else
    PEER_NS3_DEFAULT="${MAIN_REPO}/ns3/ns-3-dev-${WORKTREE_NAME}"
  fi
else
  PEER_NS3_DEFAULT="${REPO_ROOT}/ns3/ns-3-dev"
fi
: "${PEER_NS3:=${PEER_NS3_DEFAULT}}"

: "${MODE:=phase-1}"
case "${MODE}" in
  phase-1|phase-1-5) ;;
  *) echo "FATAL: MODE must be phase-1 or phase-1-5; got '${MODE}'" >&2; exit 1;;
esac

: "${OUT_DIR:=${REPO_ROOT}/output/ns3/cake-host-fairness}"
if [[ "${MODE}" == "phase-1-5" ]]; then
  : "${OUT_CSV:=${OUT_DIR}/phase-1-5-perflow-stratum.csv}"
  : "${PROVENANCE:=${OUT_DIR}/phase-1-5-provenance-stratum.txt}"
else
  : "${OUT_CSV:=${OUT_DIR}/sweep-perflow-stratum.csv}"
  : "${PROVENANCE:=${OUT_DIR}/provenance-stratum.txt}"
fi

mkdir -p "${OUT_DIR}"
rm -f "${OUT_CSV}"

GIT_SHA="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
DATE_ISO="$(date -u +%FT%TZ)"
echo "git_sha=${GIT_SHA}"                 >  "${PROVENANCE}"
echo "mode=${MODE}"                       >> "${PROVENANCE}"
echo "peer_ns3=${PEER_NS3}"               >> "${PROVENANCE}"
echo "sweep_start_iso=${DATE_ISO}"        >> "${PROVENANCE}"

replicas=(1 2 3)

if [[ "${MODE}" == "phase-1" ]]; then
  cells=(
    "2 1" "4 1" "8 1" "16 1"
    "1 2" "1 4" "1 8" "1 16"
    "1 1" "2 2" "4 4" "8 8"
  )
  strategies=(nested flat)
  total=$(( ${#cells[@]} * ${#strategies[@]} * ${#replicas[@]} ))
  i=0
  for cell in "${cells[@]}"; do
    read -r N M <<< "${cell}"
    for strat in "${strategies[@]}"; do
      for rng in "${replicas[@]}"; do
        i=$(( i + 1 ))
        printf "[%3d/%d] cell=(%s,%s) strat=%s rng=%s\n" "$i" "$total" "$N" "$M" "$strat" "$rng"
        ( cd "${PEER_NS3}" \
          && ./ns3 run --no-build "cake-host-fairness-sweep \
              --nFlowsA=${N} --nFlowsB=${M} \
              --strategy=${strat} --mode=triple --tcpVariant=cubic \
              --bandwidth=100Mbps --duration=30 \
              --rngRun=${rng} --output=${OUT_CSV}" \
        ) > "${OUT_DIR}/run-${i}.log" 2>&1
      done
    done
  done
else
  # MODE=phase-1-5: protocols axis, strategy pinned to nested.
  : "${PHASE_1_5_CELLS:=4 1|4 4}"
  : "${PROTOCOLS:=cubic newreno bbr udp}"
  IFS='|' read -ra cells_array <<< "${PHASE_1_5_CELLS}"
  read -ra protocols_array <<< "${PROTOCOLS}"
  total=$(( ${#cells_array[@]} * ${#protocols_array[@]} * ${#replicas[@]} ))
  i=0
  for cell in "${cells_array[@]}"; do
    read -r N M <<< "${cell}"
    for protocol in "${protocols_array[@]}"; do
      for rng in "${replicas[@]}"; do
        i=$(( i + 1 ))
        printf "[%3d/%d] cell=(%s,%s) protocol=%s rng=%s\n" "$i" "$total" "$N" "$M" "$protocol" "$rng"
        if [[ "${protocol}" == "udp" ]]; then
          UDP_FLAG="--udp"
          TCP_FLAG=""
        else
          UDP_FLAG=""
          TCP_FLAG="--tcpVariant=${protocol}"
        fi
        ( cd "${PEER_NS3}" \
          && ./ns3 run --no-build "cake-host-fairness-sweep \
              --nFlowsA=${N} --nFlowsB=${M} \
              --strategy=nested --mode=triple ${TCP_FLAG} ${UDP_FLAG} \
              --bandwidth=100Mbps --duration=30 \
              --rngRun=${rng} --output=${OUT_CSV}" \
        ) > "${OUT_DIR}/run-${i}.log" 2>&1
      done
    done
  done
fi

# Backfill git_sha column (the probe leaves it empty).
awk -v sha="${GIT_SHA}" 'BEGIN{FS=OFS=","} NR==1{print;next} {$15=sha; print}' \
    "${OUT_CSV}" > "${OUT_CSV}.tmp" && mv "${OUT_CSV}.tmp" "${OUT_CSV}"

DATE_END_ISO="$(date -u +%FT%TZ)"
echo "sweep_end_iso=${DATE_END_ISO}"       >> "${PROVENANCE}"
echo "rows_emitted=$(wc -l < "${OUT_CSV}")" >> "${PROVENANCE}"

echo "Done. CSV at ${OUT_CSV}; provenance at ${PROVENANCE}."
