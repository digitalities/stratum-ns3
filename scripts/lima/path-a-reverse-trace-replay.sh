#!/usr/bin/env bash
# Path A reverse trace-replay: feed a Stratum-captured cake_enqueue pcap
# into Linux sch_cake at the matching anchor, capture the post-cake
# egress stream, and compute share_A by source-IP.
#
# Counterpart to scripts/lima/trace-replay-capture-harness.sh — that
# script captures Linux cake_enqueue arrivals for ns-3 replay (forward
# direction). This one is the reverse: ns-3 cake_enqueue arrivals fed
# into Linux sch_cake.
#
# Topology (identical to the forward harness, 6 netns inside Lima):
#
#   src-a (10.1.1.1) -va0|va1-> router-a -rab1|rab2-> router-b -vsa0|vsa1-> sink-a (10.3.1.1)
#   src-b (10.1.2.1) -vb0|vb1-/                       \-vsb0|vsb1-> sink-b (10.3.2.1)
#
# Bottleneck: router-a's rab1 carries `netem delay 20ms` (root handle 1:)
# parent of `cake bandwidth 100mbit triple-isolate` (handle 2:). Matches
# both the ns-3 cake-host-fairness-sweep bottleneck (20ms channel delay,
# 100Mbps, triple-isolate) and the forward-replay reference harness.
#
# The captured Stratum pcap has source IPs in 10.1.1.0/24 (host A) and
# 10.1.2.0/24 (host B). Both subnets exactly match the Linux topology,
# so no source-IP rewriting is needed — we split the pcap by source-IP
# and replay each half from the matching netns.
#
# Usage:
#   bash path-a-reverse-trace-replay.sh <input-pcap-host> <out-dir-host>
#
# Outputs to <out-dir-host>:
#   linux-egress.pcap           — post-cake rab1 egress capture
#   share-a.txt                 — computed share_A + classifier verdict
#   tc-snapshot-before.json     — cake state at start
#   tc-snapshot-after.json      — cake state at end
#   run.log                     — VM-side execution log
set -euo pipefail

IN_PCAP="${1:-}"
OUT_DIR="${2:-/tmp/path-a-out}"
if [ -z "$IN_PCAP" ] || [ ! -f "$IN_PCAP" ]; then
    echo "Usage: $0 <input-pcap-host> <out-dir-host>" >&2
    echo "  input-pcap-host  Path on the macOS host to the Stratum cake_enqueue pcap." >&2
    echo "  out-dir-host     Path on the macOS host where outputs will land." >&2
    exit 1
fi
mkdir -p "$OUT_DIR"

VM_NAME="${VM_NAME:-cake-host-fairness}"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
HOST_A_NET="${HOST_A_NET:-10.1.1.0/24}"
HOST_B_NET="${HOST_B_NET:-10.1.2.0/24}"
DRAIN_S="${DRAIN_S:-5}"

VM_IN="/tmp/path-a-in.pcap"
VM_OUT_DIR="/tmp/path-a-out"

"${LIMACTL}" copy "${IN_PCAP}" "${VM_NAME}:${VM_IN}"

"${LIMACTL}" shell "${VM_NAME}" -- sudo bash -c "
set -euo pipefail
rm -rf ${VM_OUT_DIR}
mkdir -p ${VM_OUT_DIR}

# Split the input pcap by source-IP.
cd ${VM_OUT_DIR}
tcpdump -r ${VM_IN} -w host-a.pcap 'src net ${HOST_A_NET}' 2>/dev/null
tcpdump -r ${VM_IN} -w host-b.pcap 'src net ${HOST_B_NET}' 2>/dev/null
echo 'pcap split:'
echo '  host-a:' \$(tcpdump -r host-a.pcap 2>/dev/null | wc -l) 'packets'
echo '  host-b:' \$(tcpdump -r host-b.pcap 2>/dev/null | wc -l) 'packets'

# Tear down + recreate the 6-netns topology.
ip -all netns delete 2>/dev/null || true
for ns in src-a src-b router-a router-b sink-a sink-b; do
    ip netns add \$ns
done
ip link add va0 type veth peer name va1
ip link add vb0 type veth peer name vb1
ip link add rab1 type veth peer name rab2
ip link add vsa0 type veth peer name vsa1
ip link add vsb0 type veth peer name vsb1
ip link set va0 netns src-a;     ip link set va1 netns router-a
ip link set vb0 netns src-b;     ip link set vb1 netns router-a
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

ip -n src-a    route add default via 10.1.1.2
ip -n src-b    route add default via 10.1.2.2
ip -n sink-a   route add default via 10.3.1.2
ip -n sink-b   route add default via 10.3.2.2
ip -n router-a route add 10.3.0.0/16 via 10.0.0.2
ip -n router-b route add 10.1.0.0/16 via 10.0.0.1
ip netns exec router-a sysctl -qw net.ipv4.ip_forward=1
ip netns exec router-b sysctl -qw net.ipv4.ip_forward=1

# Disable rp_filter so router-a accepts packets whose source IPs
# (10.1.1.1 / 10.1.2.1) are routed via different interfaces than what
# rp_filter expects. Without this, packets injected via tcpreplay may
# be silently dropped before reaching cake.
for iface in all default va1 vb1 rab1; do
    ip netns exec router-a sysctl -qw net.ipv4.conf.\$iface.rp_filter=0 || true
done

# Bottleneck qdisc chain: netem 20ms parent of cake 100mbit triple-isolate.
ip netns exec router-a tc qdisc add dev rab1 root handle 1: netem delay 20ms
ip netns exec router-a tc qdisc add dev rab1 parent 1: handle 2: cake bandwidth 100mbit triple-isolate

# Snapshot cake configuration before the run.
ip netns exec router-a tc -s -j qdisc show dev rab1 \
    | python3 -m json.tool > ${VM_OUT_DIR}/tc-snapshot-before.json

# DLT_RAW (pure-IP, no link-layer header) pcaps cannot be replayed directly
# via tcpreplay because veth peers expect Ethernet frames — the IP header
# bytes would be misinterpreted as MAC addresses on receipt. Convert each
# half via tcprewrite into DLT_EN10MB with the matching source/gateway MAC
# pair so the kernel accepts and routes the packets normally.
VA0_MAC=\$(ip -n src-a    link show va0 | awk '/link\\/ether/ {print \$2}')
VA1_MAC=\$(ip -n router-a link show va1 | awk '/link\\/ether/ {print \$2}')
VB0_MAC=\$(ip -n src-b    link show vb0 | awk '/link\\/ether/ {print \$2}')
VB1_MAC=\$(ip -n router-a link show vb1 | awk '/link\\/ether/ {print \$2}')
# --fixcsum: recompute IP and L4 checksums. ns-3 leaves them at 0 in the
# pcap (its internal validation is simulator-only); without this fix the
# Linux IP layer rejects every packet as InHdrErrors.
tcprewrite --fixcsum --dlt=enet --enet-smac=\"\${VA0_MAC}\" --enet-dmac=\"\${VA1_MAC}\" \
    -i host-a.pcap -o host-a.enet.pcap
tcprewrite --fixcsum --dlt=enet --enet-smac=\"\${VB0_MAC}\" --enet-dmac=\"\${VB1_MAC}\" \
    -i host-b.pcap -o host-b.enet.pcap

# Start egress capture on rab1 (post-cake, per Linux egress AF_PACKET
# tap semantics). 'ip' filter discards ARP / non-IP frames.
ip netns exec router-a tcpdump -nn -i rab1 -w ${VM_OUT_DIR}/linux-egress.pcap 'ip' &
TCPDUMP_PID=\$!
sleep 0.5

# Inject. No --topspeed = original inter-arrival timing. Run the two
# replays in parallel so host-A and host-B traffic arrives at rab1
# with the same time-base as the original ns-3 capture.
(ip netns exec src-a tcpreplay --intf1=va0 --quiet ${VM_OUT_DIR}/host-a.enet.pcap || true) &
PID_A=\$!
(ip netns exec src-b tcpreplay --intf1=vb0 --quiet ${VM_OUT_DIR}/host-b.enet.pcap || true) &
PID_B=\$!
wait \$PID_A \$PID_B

# Drain in-flight packets through the 20ms netem + cake pipeline.
sleep ${DRAIN_S}

# Stop egress capture and snapshot final cake state.
kill \$TCPDUMP_PID 2>/dev/null || true
wait \$TCPDUMP_PID 2>/dev/null || true
ip netns exec router-a tc -s -j qdisc show dev rab1 \
    | python3 -m json.tool > ${VM_OUT_DIR}/tc-snapshot-after.json

echo '--- replay done; computing share_A on host side ---'
" 2>&1 | tee "${OUT_DIR}/run.log"

# Pull artefacts back to the host (egress pcap only; share_A is computed
# host-side because the heredoc'd Python in the Lima session is brittle
# under double-quoted bash -c, and a separate analysis script is cleaner).
for f in linux-egress.pcap tc-snapshot-before.json tc-snapshot-after.json; do
    "${LIMACTL}" copy "${VM_NAME}:${VM_OUT_DIR}/${f}" "${OUT_DIR}/${f}"
done

# Compute share_A on the host using the standalone analysis script.
SCRIPT_DIR="$(cd "$(dirname "$0")"/.. && pwd)"
python3 "${SCRIPT_DIR}/analysis/path-a-share-a.py" \
    --out "${OUT_DIR}/share-a.txt" \
    "${OUT_DIR}/linux-egress.pcap"

echo "=== Path A reverse trace-replay complete ==="
echo "  input pcap:   ${IN_PCAP}"
echo "  output dir:   ${OUT_DIR}"
echo "  share_a.txt:"
sed 's/^/    /' "${OUT_DIR}/share-a.txt"
