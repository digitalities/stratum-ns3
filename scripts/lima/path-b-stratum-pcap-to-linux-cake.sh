#!/usr/bin/env bash
# Reverse trace-replay: Stratum-generated cake_enqueue pcap fed into a
# Linux cake netns, share_A measured at the cake output. Complements
# the forward-direction harness (Linux pcap -> Stratum qdisc) recorded
# in the project's design-record ledger.
#
# Hypothesis: if Stratum's deterministic arrival pattern produces
# Stratum-like share_A (~0.77) when fed to Linux cake, the input
# pattern alone drives the gap regardless of qdisc implementation.
# If it produces Linux-like share_A (~0.51), the qdisc implementation
# differs in a way that matters when given a particular kind of input.
#
# Usage:
#   STRATUM_PCAP=/path/to/cake-enqueue.pcap \
#       bash scripts/lima/path-b-stratum-pcap-to-linux-cake.sh
#
# Outputs:
#   /tmp/path-b-out/share_a.txt   per-host bytes + share_A
#   /tmp/path-b-out/recv.pcap     captured cake output

set -euo pipefail

VM_NAME="cake-host-fairness"
LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
STRATUM_PCAP="${STRATUM_PCAP:?STRATUM_PCAP must be set to a Stratum-generated cake_enqueue pcap}"
OUT_DIR="${OUT_DIR:-/tmp/path-b-out}"
HOST_A_NET="${HOST_A_NET:-10.1.1.0/24}"  # cake-host-fairness-sweep host A
HOST_B_NET="${HOST_B_NET:-10.1.2.0/24}"  # cake-host-fairness-sweep host B

mkdir -p "${OUT_DIR}"

if [[ ! -f "${STRATUM_PCAP}" ]]; then
  echo "FATAL: STRATUM_PCAP not found: ${STRATUM_PCAP}" >&2
  exit 1
fi

# Copy pcap into the VM
echo "[1/5] Uploading Stratum pcap into Lima VM..."
${LIMACTL} copy "${STRATUM_PCAP}" "${VM_NAME}:/tmp/stratum-input.pcap"

# Build the netns testbed inside the VM and replay
echo "[2/5] Building netns + cake; replaying pcap..."
${LIMACTL} shell "${VM_NAME}" sudo bash -s <<'INNER'
set -eu

# Clean any prior run state
ip -all netns delete 2>/dev/null || true

# Two netns: 'send' (replays pcap) and 'recv' (captures cake output)
ip netns add send
ip netns add recv

# Veth pair: vs in send, vr in recv. CAKE on vs egress.
ip link add vs type veth peer name vr
ip link set vs netns send
ip link set vr netns recv

ip -n send addr add 10.0.0.1/24 dev vs
ip -n send link set vs up
ip -n recv addr add 10.0.0.2/24 dev vr
ip -n recv link set vr up

# Bring up loopback so tcpreplay/tcpdump work normally
ip -n send link set lo up
ip -n recv link set lo up

# Add cake on vs egress: 100 mbit triple-isolate (matches the Stratum
# probe's bottleneck configuration).
ip netns exec send tc qdisc add dev vs root cake bandwidth 100mbit triple-isolate

# Find the destination MAC of vr (so tcprewrite can target it)
VR_MAC=$(ip -n recv link show vr | awk '/ether/ {print $2}')
VS_MAC=$(ip -n send link show vs | awk '/ether/ {print $2}')
echo "vs MAC=${VS_MAC}  vr MAC=${VR_MAC}"

# Stratum pcap is LINKTYPE_RAW (DLT_RAW=12 BSD or 101 libpcap); rewrite to
# Ethernet with vs->vr MACs, fix checksums.
tcprewrite \
  --dlt=enet \
  --enet-smac="${VS_MAC}" \
  --enet-dmac="${VR_MAC}" \
  --infile=/tmp/stratum-input.pcap \
  --outfile=/tmp/stratum-replay.pcap \
  --fixcsum

# Start tcpdump in recv netns BEFORE replay
ip netns exec recv tcpdump -i vr -w /tmp/recv.pcap -B 8192 -s 0 -n \
  ip and not arp >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1

# Replay from send netns. tcpreplay -t = topspeed, -K = preload pcap, but
# we want timing preserved -> use default replay-at-pcap-timestamps mode.
# --pps-multi if pcap timestamps are very dense; for now, replay at the
# original pace.
ip netns exec send tcpreplay -i vs --preload-pcap /tmp/stratum-replay.pcap

# Let any in-flight buffered packets drain through cake
sleep 2

kill ${TCPDUMP_PID} || true
wait ${TCPDUMP_PID} 2>/dev/null || true

# Read cake stats
echo "=== tc -s qdisc show dev vs (in send netns) ==="
ip netns exec send tc -s qdisc show dev vs

# Move outputs to /tmp/path-b-vm-out for retrieval
mkdir -p /tmp/path-b-vm-out
cp /tmp/recv.pcap /tmp/path-b-vm-out/recv.pcap
INNER

echo "[3/5] Downloading recv.pcap..."
${LIMACTL} copy "${VM_NAME}:/tmp/path-b-vm-out/recv.pcap" "${OUT_DIR}/recv.pcap"

echo "[4/5] Computing share_A by source-IP byte count..."
# Use Python + dpkt-style counting via tshark if available, else fallback.
if command -v tshark >/dev/null 2>&1; then
  python3 - "${OUT_DIR}/recv.pcap" "${HOST_A_NET}" "${HOST_B_NET}" "${OUT_DIR}/share_a.txt" <<'PY'
import subprocess, sys, ipaddress
pcap, net_a, net_b, out_path = sys.argv[1:]
net_a = ipaddress.ip_network(net_a)
net_b = ipaddress.ip_network(net_b)
proc = subprocess.run(
    ["tshark", "-r", pcap, "-T", "fields", "-e", "ip.src", "-e", "ip.len"],
    capture_output=True, text=True, check=False,
)
bytes_a = bytes_b = bytes_other = 0
for line in proc.stdout.splitlines():
    parts = line.split("\t")
    if len(parts) < 2 or not parts[0] or not parts[1]:
        continue
    try:
        src = ipaddress.ip_address(parts[0])
        n = int(parts[1])
    except (ValueError, ipaddress.AddressValueError):
        continue
    if src in net_a:
        bytes_a += n
    elif src in net_b:
        bytes_b += n
    else:
        bytes_other += n
total = bytes_a + bytes_b
share_a = bytes_a / total if total else 0.0
with open(out_path, "w") as fh:
    fh.write(f"bytes_a = {bytes_a}\n")
    fh.write(f"bytes_b = {bytes_b}\n")
    fh.write(f"bytes_other = {bytes_other}\n")
    fh.write(f"share_a = {share_a:.4f}\n")
print(open(out_path).read())
PY
else
  echo "WARN: tshark not found locally; rerun inside Lima or install tshark."
  echo "Copying recv.pcap is sufficient; analyze manually."
fi

echo "[5/5] Done. Outputs in ${OUT_DIR}/"
