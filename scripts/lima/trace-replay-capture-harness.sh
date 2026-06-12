#!/usr/bin/env bash
# Stratum trace-replay validation — Linux-side capture harness.
#
# Captures a single (16,1) CUBIC reference run of Linux sch_cake and
# packages everything the ns-3 replay binary needs:
#
#   linux-trace.pcap         — nanosecond-resolution tcpdump on rab1 egress
#                              (one frame per packet that entered cake_enqueue;
#                              pre-qdisc tap point per the Linux egress TC pipeline)
#   linux-flow-quantum.csv   — cake_get_flow_quantum return-value stream
#                              (supplementary diagnostic)
#   share_A.txt              — sanity-check that this run reproduced
#                              Linux's 0.5146 ± 0.01 reference share_A
#   tc-snapshot-before.json  — tc -s -j qdisc show dev rab1 before iperf3
#   tc-snapshot-after.json   — tc -s -j qdisc show dev rab1 after iperf3
#   run.log                  — VM-side execution log
#
# Topology (shared with other Lima harnesses in this directory):
#
#   src-a ─va─ router-a ─rab1─ router-b ─vsa─ sink-a
#   src-b ─vb─/                ─vsb─ sink-b
#
# Bottleneck on router-a's rab1 egress:
#   root:   netem delay 20ms          (handle 1:)
#   child:  cake bandwidth 100mbit triple-isolate   (parent 1: handle 2:)
#
# Load: N=16 host-A CUBIC flows, M=1 host-B CUBIC flow, duration=30s, single replica.
#
# Capture point: tcpdump -i va1 -Q in  AND  tcpdump -i vb1 -Q in
#   in the router-a netns. On Linux the egress AF_PACKET tap fires INSIDE
#   dev_hard_start_xmit (i.e. AFTER qdisc dequeue), so capturing on rab1
#   egress would record cake's OUTPUT order, not its input. The correct
#   pre-cake capture point is the ingress side of the two veth-peer
#   interfaces in router-a — the packets arrive there from src-a / src-b,
#   are routed by IP forwarding (sub-microsecond), and then handed to cake
#   on rab1. Capturing on va1+vb1 ingress yields the cake-input timeline
#   modulo a ~1 us forwarding delay (well below the 20 ms RTT).
#
# Two pcaps are produced and merged by timestamp at replay time.
#
# Usage:
#   bash scripts/lima/trace-replay-capture-harness.sh <out-dir-on-host>

set -euo pipefail

OUT_DIR="${1:-/tmp/trace-replay-capture}"
VM_NAME="${VM_NAME:-cake-host-fairness}"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
N="${N:-16}"
M="${M:-1}"
DURATION_S="${DURATION_S:-30}"
PROBE_TAIL_S="${PROBE_TAIL_S:-5}"

mkdir -p "${OUT_DIR}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BT_SCRIPT_LOCAL="${SCRIPT_DIR}/trace-replay-capture.bt"
BT_SCRIPT_VM="/tmp/trace-replay-capture.bt"

${LIMACTL} shell "${VM_NAME}" -- sudo cp \
  "${BT_SCRIPT_LOCAL}" "${BT_SCRIPT_VM}"

echo "[host] Stratum trace-replay capture (Linux side)"
echo "  N=${N} host-A flows, M=${M} host-B flow"
echo "  duration=${DURATION_S}s (single replica)"
echo "  qdisc stack: netem 20ms -> cake 100mbit triple-isolate on rab1"
echo "  capture: tcpdump -Q in on va1 + vb1 (pre-cake) in router-a netns"
echo "  output: ${OUT_DIR}"
echo

ONE_RUN=$(cat <<'EOF'
set -eu
N=$1
M=$2
DURATION_S=$3
PROBE_TAIL_S=$4
OUT_VM_DIR=$5
BT_SCRIPT_VM=$6

mkdir -p "${OUT_VM_DIR}"
LOG="${OUT_VM_DIR}/run.log"
exec > "${LOG}" 2>&1
echo "[$(date +%T)] === trace-replay capture ==="

# ---- (16,1) CUBIC reference topology setup ----
ip -all netns delete 2>/dev/null || true
for nsname in src-a src-b router-a router-b sink-a sink-b; do
  ip netns add "$nsname"
done

ip link add va0 type veth peer name va1
ip link add vb0 type veth peer name vb1
ip link add rab1 type veth peer name rab2
ip link add vsa0 type veth peer name vsa1
ip link add vsb0 type veth peer name vsb1

ip link set va0  netns src-a;    ip link set va1  netns router-a
ip link set vb0  netns src-b;    ip link set vb1  netns router-a
ip link set rab1 netns router-a; ip link set rab2 netns router-b
ip link set vsa0 netns router-b; ip link set vsa1 netns sink-a
ip link set vsb0 netns router-b; ip link set vsb1 netns sink-b

ip -n src-a    addr add 10.1.1.1/24 dev va0;  ip -n src-a    link set va0 up
ip -n router-a addr add 10.1.1.2/24 dev va1;  ip -n router-a link set va1 up
ip -n src-b    addr add 10.1.2.1/24 dev vb0;  ip -n src-b    link set vb0 up
ip -n router-a addr add 10.1.2.2/24 dev vb1;  ip -n router-a link set vb1 up
ip -n router-a addr add 10.0.0.1/24 dev rab1; ip -n router-a link set rab1 up
ip -n router-b addr add 10.0.0.2/24 dev rab2; ip -n router-b link set rab2 up
ip -n router-b addr add 10.3.1.2/24 dev vsa0; ip -n router-b link set vsa0 up
ip -n sink-a   addr add 10.3.1.1/24 dev vsa1; ip -n sink-a   link set vsa1 up
ip -n router-b addr add 10.3.2.2/24 dev vsb0; ip -n router-b link set vsb0 up
ip -n sink-b   addr add 10.3.2.1/24 dev vsb1; ip -n sink-b   link set vsb1 up

ip -n src-a    route add default     via 10.1.1.2
ip -n src-b    route add default     via 10.1.2.2
ip -n sink-a   route add default     via 10.3.1.2
ip -n sink-b   route add default     via 10.3.2.2
ip -n router-a route add 10.3.0.0/16 via 10.0.0.2
ip -n router-b route add 10.1.0.0/16 via 10.0.0.1
ip netns exec router-a sysctl -qw net.ipv4.ip_forward=1
ip netns exec router-b sysctl -qw net.ipv4.ip_forward=1

ip netns exec router-a tc qdisc add dev rab1 root handle 1: netem delay 20ms
ip netns exec router-a tc qdisc add dev rab1 parent 1: handle 2: \
  cake bandwidth 100mbit triple-isolate

# ---- Snapshot qdisc state before traffic ----
ip netns exec router-a tc -s -j qdisc show dev rab1 \
  | python3 -m json.tool > "${OUT_VM_DIR}/tc-snapshot-before.json"

# ---- Start tcpdump and bpftrace BEFORE iperf3 servers (warmup capture) ----
TOTAL_PROBE_S=$(( DURATION_S + PROBE_TAIL_S ))

# Two parallel tcpdumps: va1 ingress (host-A traffic into router-a) and
# vb1 ingress (host-B traffic). Each captures BEFORE IP forwarding hands
# the packet to cake_enqueue on rab1. Nanosecond timestamps via
# --time-stamp-precision=nano. -s 96 caps snaplen at L2+IPv4+TCP+12B
# options, which is everything trace-replay needs.
ip netns exec router-a timeout "${TOTAL_PROBE_S}" tcpdump \
  -i va1 -Q in \
  --time-stamp-precision=nano \
  -B 16384 \
  -s 96 \
  -w "${OUT_VM_DIR}/linux-trace-va1.pcap" \
  > /dev/null 2> "${OUT_VM_DIR}/tcpdump-va1.log" &
TCPDUMP_VA1_PID=$!

ip netns exec router-a timeout "${TOTAL_PROBE_S}" tcpdump \
  -i vb1 -Q in \
  --time-stamp-precision=nano \
  -B 16384 \
  -s 96 \
  -w "${OUT_VM_DIR}/linux-trace-vb1.pcap" \
  > /dev/null 2> "${OUT_VM_DIR}/tcpdump-vb1.log" &
TCPDUMP_VB1_PID=$!

# bpftrace flow_quantum probe (supplementary, for Risk #5 cross-check)
timeout "${TOTAL_PROBE_S}" bpftrace "${BT_SCRIPT_VM}" \
  > "${OUT_VM_DIR}/linux-flow-quantum.csv" 2> "${OUT_VM_DIR}/bpftrace.log" &
BPF_PID=$!

sleep 1   # let probes attach + warm up

# ---- iperf3 servers on sink netns ----
PORT_A=9000
PORT_B=10000
for i in $(seq 0 $(( N - 1 ))); do
  ip netns exec sink-a iperf3 -s -p $(( PORT_A + i )) -1 -D 2>/dev/null || true
done
for j in $(seq 0 $(( M - 1 ))); do
  ip netns exec sink-b iperf3 -s -p $(( PORT_B + j )) -1 -D 2>/dev/null || true
done
sleep 1

# ---- iperf3 clients (CUBIC, ${DURATION_S}s) ----
TMP="${OUT_VM_DIR}/iperf-tmp"
mkdir -p "${TMP}"
for i in $(seq 0 $(( N - 1 ))); do
  ip netns exec src-a iperf3 -c 10.3.1.1 -p $(( PORT_A + i )) \
    -t ${DURATION_S} -J -A 0 -C cubic \
    > "${TMP}/a-${i}.json" 2>/dev/null &
done
for j in $(seq 0 $(( M - 1 ))); do
  ip netns exec src-b iperf3 -c 10.3.2.1 -p $(( PORT_B + j )) \
    -t ${DURATION_S} -J -A 0 -C cubic \
    > "${TMP}/b-${j}.json" 2>/dev/null &
done
wait   # all iperf3 clients

# Let probes finish their tail.
wait ${TCPDUMP_VA1_PID} 2>/dev/null || true
wait ${TCPDUMP_VB1_PID} 2>/dev/null || true
wait ${BPF_PID}          2>/dev/null || true

# ---- Snapshot qdisc state after traffic ----
ip netns exec router-a tc -s -j qdisc show dev rab1 \
  | python3 -m json.tool > "${OUT_VM_DIR}/tc-snapshot-after.json"

# ---- iperf3 JSON consolidation + share_A ----
for f in "${TMP}"/a-*.json "${TMP}"/b-*.json; do
  [ -f "$f" ] && cp "$f" "${OUT_VM_DIR}/$(basename "$f" | sed 's/^/iperf-/')"
done

python3 - <<PY > "${OUT_VM_DIR}/share_A.txt"
import json, glob
a = 0.0; b = 0.0
for f in sorted(glob.glob("${OUT_VM_DIR}/iperf-a-*.json")):
    try:
        d = json.load(open(f))
        end = d.get("end", {})
        v = end.get("sum_received", end.get("sum", {})).get("bits_per_second", 0)
        a += float(v)
    except Exception:
        pass
for f in sorted(glob.glob("${OUT_VM_DIR}/iperf-b-*.json")):
    try:
        d = json.load(open(f))
        end = d.get("end", {})
        v = end.get("sum_received", end.get("sum", {})).get("bits_per_second", 0)
        b += float(v)
    except Exception:
        pass
total = a + b
share_A = a / total if total > 0 else 0
print(f"share_A={share_A:.4f}")
print(f"A_bps={a:.1f}  B_bps={b:.1f}")
PY

cat "${OUT_VM_DIR}/share_A.txt"

# pcap stats (va1 + vb1)
for tag in va1 vb1; do
  PCAP_BYTES=$(stat -c '%s' "${OUT_VM_DIR}/linux-trace-${tag}.pcap" 2>/dev/null || echo 0)
  PCAP_PKTS=$(tcpdump -r "${OUT_VM_DIR}/linux-trace-${tag}.pcap" 2>/dev/null | wc -l || echo 0)
  echo "pcap_${tag}_bytes=${PCAP_BYTES}"
  echo "pcap_${tag}_packets=${PCAP_PKTS}"
done

# Cleanup workspace.
rm -rf "${TMP}"
ip -all netns delete 2>/dev/null || true
EOF
)

VM_OUT_DIR="/tmp/trace-replay-capture-vm"
${LIMACTL} shell "${VM_NAME}" -- sudo bash -c \
  "rm -rf ${VM_OUT_DIR} && mkdir -p ${VM_OUT_DIR}"

${LIMACTL} shell "${VM_NAME}" -- sudo bash -c "${ONE_RUN}" -- \
  "${N}" "${M}" "${DURATION_S}" "${PROBE_TAIL_S}" \
  "${VM_OUT_DIR}" "${BT_SCRIPT_VM}" \
  2>&1 | tee "${OUT_DIR}/host.log"

echo "[host] pulling artefacts..."
${LIMACTL} shell "${VM_NAME}" -- sudo tar -C "${VM_OUT_DIR}" -cf - . \
  > "${OUT_DIR}/_vm-artifacts.tar"
tar -C "${OUT_DIR}" -xf "${OUT_DIR}/_vm-artifacts.tar"
rm -f "${OUT_DIR}/_vm-artifacts.tar"

echo
echo "[host] capture complete. Artefacts:"
ls -la "${OUT_DIR}/"
echo
echo "[host] share_A:"
cat "${OUT_DIR}/share_A.txt"
echo
echo "[host] sanity: expect share_A in [0.5046, 0.5246]"
