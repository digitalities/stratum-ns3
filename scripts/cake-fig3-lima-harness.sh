#!/usr/bin/env bash
# ============================================================
# CAKE paper Fig-3 host-isolation — real-Linux netns measurement.
#
# Replicates Q-15.12 / CAKE paper Fig. 3 on a real kernel sch_cake:
#   2 source hosts -> 4 destination hosts, 6 saturating TCP flows
#   (A->destA, A->destB, A->destC, A->destC, B->destC, B->destD),
#   100 Mbit/s bottleneck, ~20 ms one-way, under each of the four
#   flow-isolation modes. Per-flow goodput -> output/ns3/cake-fig3/linux-perflow.csv.
#
# Companion to the pure-ns-3 fixture CakeFig3HostIsolationTest; the
# two arms feed scripts/plot-cake-fig3-three-way.py.
#
# Requires a Lima VM "cake-host-fairness" with sch_cake + iperf3 + jq + bc.
# One-time setup: see scripts/cake-host-fairness-lima-harness.sh.
#
# Run:  bash scripts/cake-fig3-lima-harness.sh
# ============================================================
set -euo pipefail

VM_NAME="${VM_NAME:-cake-host-fairness}"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/output/ns3/cake-fig3}"
BANDWIDTH="${BANDWIDTH:-100}"   # Mbit/s
DELAY_MS="${DELAY_MS:-20}"      # one-way, on the bottleneck
DURATION="${DURATION:-30}"      # s per flow
# shellcheck disable=SC2206  # intentional word-split of the replica list
REPLICAS=(${REPLICAS:-1 2 3})

mkdir -p "${OUT_DIR}"
PERFLOW_CSV="${OUT_DIR}/linux-perflow.csv"

# Canonical mode label  ->  iproute2 cake keyword (the names differ!).
# Plain function + indexed array (no `declare -A`) for bash 3.2 (macOS) portability.
MODE_LABELS=(no-iso source dest triple)
mode_kw() {
  case "$1" in
    no-iso) echo "flows" ;;
    source) echo "dual-srchost" ;;
    dest)   echo "dual-dsthost" ;;
    triple) echo "triple-isolate" ;;
    *)      echo "FATAL: unknown mode '$1'" >&2; exit 1 ;;
  esac
}

# Fig-3 flow mapping: index -> (source label, dest label). destC carries 3 flows.
declare -a FLOW_SRCLBL=(A A A A B B)
declare -a FLOW_DSTLBL=(destA destB destC destC destC destD)

if ! ${LIMACTL} list -q 2>/dev/null | grep -qx "${VM_NAME}"; then
  echo "FATAL: Lima VM '${VM_NAME}' not found." >&2
  echo "Run: ${LIMACTL} start --name=${VM_NAME} --tty=false template://ubuntu" >&2
  exit 1
fi
${LIMACTL} shell "${VM_NAME}" sudo modprobe sch_cake >/dev/null 2>&1 || {
  echo "FATAL: sch_cake unavailable in VM kernel." >&2; exit 1; }

# One Fig-3 run inside the VM. Args: <cake_keyword> <port_base>.
# Prints lines: "<flow_idx> <goodput_mbps>".
ONE_RUN_BODY='
set -eu
CAKE_KW=$1; PORT_BASE=$2
BW='"${BANDWIDTH}"'; DLY='"${DELAY_MS}"'; DUR='"${DURATION}"'

ip -all netns delete 2>/dev/null || true
for ns in src-a src-b router-a router-b dest-a dest-b dest-c dest-d; do
  ip netns add "$ns"
done

# veth wiring: sources -> router-a -> router-b -> dests
ip link add va0  type veth peer name va1
ip link add vb0  type veth peer name vb1
ip link add rab1 type veth peer name rab2
ip link add da0  type veth peer name da1
ip link add db0  type veth peer name db1
ip link add dc0  type veth peer name dc1
ip link add dd0  type veth peer name dd1

ip link set va0 netns src-a;    ip link set va1 netns router-a
ip link set vb0 netns src-b;    ip link set vb1 netns router-a
ip link set rab1 netns router-a; ip link set rab2 netns router-b
ip link set da0 netns router-b; ip link set da1 netns dest-a
ip link set db0 netns router-b; ip link set db1 netns dest-b
ip link set dc0 netns router-b; ip link set dc1 netns dest-c
ip link set dd0 netns router-b; ip link set dd1 netns dest-d

ip -n src-a    addr add 10.1.1.1/24 dev va0;  ip -n src-a    link set va0 up
ip -n router-a addr add 10.1.1.2/24 dev va1;  ip -n router-a link set va1 up
ip -n src-b    addr add 10.1.2.1/24 dev vb0;  ip -n src-b    link set vb0 up
ip -n router-a addr add 10.1.2.2/24 dev vb1;  ip -n router-a link set vb1 up
ip -n router-a addr add 10.0.0.1/24 dev rab1; ip -n router-a link set rab1 up
ip -n router-b addr add 10.0.0.2/24 dev rab2; ip -n router-b link set rab2 up
ip -n router-b addr add 10.3.1.2/24 dev da0;  ip -n router-b link set da0 up
ip -n dest-a   addr add 10.3.1.1/24 dev da1;  ip -n dest-a   link set da1 up
ip -n router-b addr add 10.3.2.2/24 dev db0;  ip -n router-b link set db0 up
ip -n dest-b   addr add 10.3.2.1/24 dev db1;  ip -n dest-b   link set db1 up
ip -n router-b addr add 10.3.3.2/24 dev dc0;  ip -n router-b link set dc0 up
ip -n dest-c   addr add 10.3.3.1/24 dev dc1;  ip -n dest-c   link set dc1 up
ip -n router-b addr add 10.3.4.2/24 dev dd0;  ip -n router-b link set dd0 up
ip -n dest-d   addr add 10.3.4.1/24 dev dd1;  ip -n dest-d   link set dd1 up

ip -n src-a route add default via 10.1.1.2
ip -n src-b route add default via 10.1.2.2
ip -n dest-a route add default via 10.3.1.2
ip -n dest-b route add default via 10.3.2.2
ip -n dest-c route add default via 10.3.3.2
ip -n dest-d route add default via 10.3.4.2
ip -n router-a route add 10.3.0.0/16 via 10.0.0.2
ip -n router-b route add 10.1.0.0/16 via 10.0.0.1
ip netns exec router-a sysctl -qw net.ipv4.ip_forward=1
ip netns exec router-b sysctl -qw net.ipv4.ip_forward=1

# Bottleneck on router-a egress toward router-b: netem delay + cake.
ip netns exec router-a tc qdisc add dev rab1 root handle 1: netem delay ${DLY}ms
ip netns exec router-a tc qdisc add dev rab1 parent 1: handle 2: cake bandwidth ${BW}mbit ${CAKE_KW}

# Sinks: one iperf3 server per flow (distinct ports).
PORTS=( $((PORT_BASE+0)) $((PORT_BASE+1)) $((PORT_BASE+2)) $((PORT_BASE+3)) $((PORT_BASE+4)) $((PORT_BASE+5)) )
DST_NS=( dest-a dest-b dest-c dest-c dest-c dest-d )
SRC_NS=( src-a src-a src-a src-a src-b src-b )
DST_IP=( 10.3.1.1 10.3.2.1 10.3.3.1 10.3.3.1 10.3.3.1 10.3.4.1 )
for i in 0 1 2 3 4 5; do
  ip netns exec "${DST_NS[$i]}" iperf3 -s -p "${PORTS[$i]}" -1 -D 2>/dev/null || true
done
sleep 1

TMP=$(mktemp -d)
for i in 0 1 2 3 4 5; do
  ip netns exec "${SRC_NS[$i]}" iperf3 -c "${DST_IP[$i]}" -p "${PORTS[$i]}" \
    -t ${DUR} -J -A 0 > "${TMP}/f-${i}.json" 2>/dev/null &
done
wait

for i in 0 1 2 3 4 5; do
  GP_BPS=$(jq -r ".end.streams[0].sender.bits_per_second // 0" "${TMP}/f-${i}.json" 2>/dev/null || echo 0)
  printf "%d %.6f\n" "$i" "$(echo "${GP_BPS} / 1000000" | bc -l)"
done

rm -rf "${TMP}"
ip -all netns delete
'

echo "mode,rng_run,flow_idx,src,dst,goodput_mbps" > "${PERFLOW_CSV}"
for label in "${MODE_LABELS[@]}"; do
  kw="$(mode_kw "$label")"
  for rng in "${REPLICAS[@]}"; do
    echo "[mode=${label} (${kw}) rng=${rng}] running Fig-3 ..."
    ${LIMACTL} shell "${VM_NAME}" sudo bash -c "${ONE_RUN_BODY}" -- "${kw}" 9300 \
      > "${OUT_DIR}/run-${label}-rng${rng}.txt" 2>&1 || {
        echo "FATAL: run mode=${label} rng=${rng} failed; see ${OUT_DIR}/run-${label}-rng${rng}.txt" >&2
        exit 1; }
    while read -r flow_idx gp_mbps; do
      case "${flow_idx}" in 0|1|2|3|4|5) ;; *) continue ;; esac
      printf "%s,%s,%s,%s,%s,%s\n" \
        "${label}" "${rng}" "${flow_idx}" \
        "${FLOW_SRCLBL[$flow_idx]}" "${FLOW_DSTLBL[$flow_idx]}" "${gp_mbps}" \
        >> "${PERFLOW_CSV}"
    done < "${OUT_DIR}/run-${label}-rng${rng}.txt"
  done
done

echo
echo "Wrote ${PERFLOW_CSV}"
echo "Next: python3 scripts/plot-cake-fig3-three-way.py"
