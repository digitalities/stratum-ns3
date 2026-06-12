#!/usr/bin/env bash
# ============================================================
# CAKE paper Fig-5 DiffServ-tin isolation — real-Linux netns measurement.
#
# Replicates Q-15.13 / CAKE paper Fig. 5 on a real kernel sch_cake:
#   a 2 Mbit/s EF-marked (DSCP 46) fixed-rate UDP flow competes with 32
#   bulk TCP flows over a 10 Mbit/s bottleneck, under three qdiscs:
#     cake-diffserv  -> cake bandwidth 10mbit diffserv4   (EF -> Voice tin)
#     cake-besteffort-> cake bandwidth 10mbit besteffort  (per-flow FQ only)
#     fq-codel       -> htb 10mbit + fq_codel leaf        (per-flow FQ only)
#   Per arm: EF goodput (Mbit/s), EF loss (%), and EF-marked ping latency
#   under load (induced = rtt - min). -> output/ns3/cake-fig5/linux-goodput-loss.csv
#
# Companion to the pure-ns-3 fixture CakeFig5SparseFlowLatencyTest; the two
# arms feed scripts/cake-fig5-plot.py (ns-3 vs Linux served-vs-starved).
#
# Requires a Lima VM with sch_cake + iperf3 + jq + bc + iproute2.
# MUST run foreground + unsandboxed (limactl is unreachable from a
# backgrounded/sandboxed process).
#
# Run:  bash scripts/cake-fig5-lima-harness.sh
# ============================================================
set -euo pipefail

VM_NAME="${VM_NAME:-cake-host-fairness}"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
[ -x "$LIMACTL" ] || LIMACTL="$(command -v limactl)"
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/output/ns3/cake-fig5}"
BANDWIDTH="${BANDWIDTH:-10}"    # Mbit/s bottleneck
DURATION="${DURATION:-30}"      # s per arm
EF_RATE="${EF_RATE:-2M}"        # fixed-rate flow offered load
NBULK="${NBULK:-32}"            # competing bulk TCP flows

mkdir -p "${OUT_DIR}"
CSV="${OUT_DIR}/linux-goodput-loss.csv"

ARM_LABELS=(cake-diffserv cake-besteffort fq-codel)
# Canonical arm -> tc qdisc setup keyword passed into the VM body.
arm_kw() {
  case "$1" in
    cake-diffserv)   echo "cake-diffserv" ;;
    cake-besteffort) echo "cake-besteffort" ;;
    fq-codel)        echo "fq-codel" ;;
    *) echo "FATAL: unknown arm '$1'" >&2; exit 1 ;;
  esac
}

if ! ${LIMACTL} list -q 2>/dev/null | grep -qx "${VM_NAME}"; then
  echo "FATAL: Lima VM '${VM_NAME}' not found." >&2
  echo "Run: ${LIMACTL} start --name=${VM_NAME} --tty=false template://ubuntu" >&2
  exit 1
fi
if ! ${LIMACTL} list 2>/dev/null | awk -v n="${VM_NAME}" '$1==n{print $2}' | grep -qx Running; then
  echo "[lima] starting VM ${VM_NAME} ..."
  ${LIMACTL} start "${VM_NAME}" --tty=false
fi
${LIMACTL} shell "${VM_NAME}" sudo modprobe sch_cake >/dev/null 2>&1 || {
  echo "FATAL: sch_cake unavailable in VM kernel." >&2; exit 1; }

# One Fig-5 arm inside the VM. Args: <arm_kw>.
# Prints: "EF_GOODPUT_MBPS=.. EF_LOSS_PCT=.. BULK_GOODPUT_MBPS=.. PING_P50_MS=.. PING_P99_MS=.."
ONE_RUN_BODY='
set -eu
ARM=$1
BW='"${BANDWIDTH}"'; DUR='"${DURATION}"'; EFRATE='"${EF_RATE}"'; NBULK='"${NBULK}"'

ip -all netns delete 2>/dev/null || true
ip netns add src; ip netns add router; ip netns add sink

ip link add s0 type veth peer name s1
ip link add r0 type veth peer name r1
ip link set s0 netns src;    ip link set s1 netns router
ip link set r0 netns router; ip link set r1 netns sink

ip -n src    addr add 10.1.1.1/24 dev s0; ip -n src    link set s0 up
ip -n router addr add 10.1.1.2/24 dev s1; ip -n router link set s1 up
ip -n router addr add 10.2.1.2/24 dev r0; ip -n router link set r0 up
ip -n sink   addr add 10.2.1.1/24 dev r1; ip -n sink   link set r1 up
ip -n src  link set lo up; ip -n sink link set lo up
ip -n src  route add default via 10.1.1.2
ip -n sink route add default via 10.2.1.2
ip netns exec router sysctl -qw net.ipv4.ip_forward=1

# Bottleneck on router egress toward sink (r0), 10 Mbit/s. The download
# path (src->sink) is the bottleneck; ACKs return unshaped.
case "$ARM" in
  cake-diffserv)
    ip netns exec router tc qdisc add dev r0 root handle 1: cake bandwidth ${BW}mbit diffserv4 ;;
  cake-besteffort)
    ip netns exec router tc qdisc add dev r0 root handle 1: cake bandwidth ${BW}mbit besteffort ;;
  fq-codel)
    ip netns exec router tc qdisc add dev r0 root handle 1: htb default 10
    ip netns exec router tc class add dev r0 parent 1: classid 1:10 htb rate ${BW}mbit
    ip netns exec router tc qdisc add dev r0 parent 1:10 handle 20: fq_codel ;;
  *) echo "FATAL: unknown ARM $ARM" >&2; exit 1 ;;
esac

# Sinks: bulk TCP server (-P fans out) + EF UDP server.
ip netns exec sink iperf3 -s -p 5201 -1 -D 2>/dev/null || true
ip netns exec sink iperf3 -s -p 5202 -1 -D 2>/dev/null || true
sleep 1

TMP=$(mktemp -d)
# 32 bulk TCP download streams (distinct 5-tuples -> distinct FQ flows).
ip netns exec src iperf3 -c 10.2.1.1 -p 5201 -P ${NBULK} -t ${DUR} -J -A 0 \
  > "${TMP}/bulk.json" 2>/dev/null &
# EF-marked fixed-rate UDP flow (DSCP 46 -> TOS 184).
ip netns exec src iperf3 -c 10.2.1.1 -p 5202 -u -b ${EFRATE} -l 1400 -S 184 -t ${DUR} -J \
  > "${TMP}/ef.json" 2>/dev/null &
# EF-marked latency probe under load.
ip netns exec src ping -Q 0xb8 -i 0.2 -w ${DUR} 10.2.1.1 > "${TMP}/ping.txt" 2>/dev/null &
wait

EF_LOSS=$(jq -r ".end.sum.lost_percent // 0" "${TMP}/ef.json" 2>/dev/null || echo 0)
EF_SENT_BPS=$(jq -r ".end.sum.bits_per_second // 0" "${TMP}/ef.json" 2>/dev/null || echo 0)
EF_GP=$(echo "${EF_SENT_BPS} / 1000000 * (1 - ${EF_LOSS}/100)" | bc -l)
BULK_BPS=$(jq -r ".end.sum_received.bits_per_second // 0" "${TMP}/bulk.json" 2>/dev/null || echo 0)
BULK_GP=$(echo "${BULK_BPS} / 1000000" | bc -l)

# Ping: induced = rtt - min(rtt); report p50/p99 of the induced series.
PING_STATS=$(grep -oE "time=[0-9.]+" "${TMP}/ping.txt" | sed "s/time=//" | sort -n | awk "
  { v[NR]=\$1 } END {
    if (NR==0) { print \"0 0\"; exit }
    mn=v[1];
    n=0; for(i=1;i<=NR;i++){ ind[n++]=v[i]-mn }
    p50=ind[int(0.50*(n-1)+0.5)]; p99=ind[int(0.99*(n-1)+0.5)];
    printf \"%.3f %.3f\", p50, p99
  }")
PING_P50=$(echo "$PING_STATS" | awk "{print \$1}")
PING_P99=$(echo "$PING_STATS" | awk "{print \$2}")

printf "EF_GOODPUT_MBPS=%.5f EF_LOSS_PCT=%.4f BULK_GOODPUT_MBPS=%.5f PING_P50_MS=%.3f PING_P99_MS=%.3f\n" \
  "${EF_GP}" "${EF_LOSS}" "${BULK_GP}" "${PING_P50}" "${PING_P99}"

rm -rf "${TMP}"
ip -all netns delete
'

echo "arm,ef_goodput_mbps,ef_loss_pct,bulk_goodput_mbps,ping_induced_p50_ms,ping_induced_p99_ms" > "${CSV}"
for arm in "${ARM_LABELS[@]}"; do
  kw="$(arm_kw "$arm")"
  echo "[arm=${arm}] running Fig-5 on Linux sch_cake (${DURATION}s) ..."
  out=$(${LIMACTL} shell "${VM_NAME}" sudo bash -c "${ONE_RUN_BODY}" -- "${kw}" 2>&1) || {
    echo "FATAL: arm=${arm} failed:" >&2; echo "${out}" >&2; exit 1; }
  echo "${out}" | tee "${OUT_DIR}/linux-run-${arm}.txt"
  efgp=$(echo "${out}" | grep -oE "EF_GOODPUT_MBPS=[0-9.]+" | tail -1 | cut -d= -f2)
  efloss=$(echo "${out}" | grep -oE "EF_LOSS_PCT=[0-9.]+" | tail -1 | cut -d= -f2)
  bulkgp=$(echo "${out}" | grep -oE "BULK_GOODPUT_MBPS=[0-9.]+" | tail -1 | cut -d= -f2)
  pp50=$(echo "${out}" | grep -oE "PING_P50_MS=[0-9.]+" | tail -1 | cut -d= -f2)
  pp99=$(echo "${out}" | grep -oE "PING_P99_MS=[0-9.]+" | tail -1 | cut -d= -f2)
  printf "%s,%s,%s,%s,%s,%s\n" "${arm}" "${efgp:-0}" "${efloss:-0}" "${bulkgp:-0}" "${pp50:-0}" "${pp99:-0}" >> "${CSV}"
done

echo
echo "Wrote ${CSV}"
echo "Next: python3 scripts/cake-fig5-plot.py"
