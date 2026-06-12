#!/usr/bin/env bash
# Lima-VM-based Linux sweep: 12 cells x 3 replicas of iperf3 through
# tc-cake-shaped netns links inside the cake-host-fairness Lima VM.
# Per-flow goodput written to sweep-perflow-linux.csv.
#
# Each bottleneck egress carries a chained qdisc: netem delay 20ms ->
# cake bandwidth 100mbit triple-isolate. The netem stage matches the
# Stratum probe's one-way bottleneck delay so the comparison is
# apples-to-apples.

set -euo pipefail

VM_NAME="cake-host-fairness"
LIMACTL="/opt/homebrew/bin/limactl"
: "${REPO_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
: "${OUT_DIR:=${REPO_ROOT}/output/ns3/cake-host-fairness}"
: "${OUT_CSV:=${OUT_DIR}/sweep-perflow-linux.csv}"
PROVENANCE="${OUT_DIR}/provenance-linux.txt"

mkdir -p "${OUT_DIR}"

if [[ ! -f "${PROVENANCE}" ]]; then
  echo "Missing ${PROVENANCE}. Run cake-host-fairness-lima-harness.sh first."
  exit 1
fi

# Protocol axis. Defaults to cubic (canonical-sweep behavior preserved).
: "${PROTOCOL:=cubic}"
case "${PROTOCOL}" in
  cubic|newreno|bbr|udp) ;;
  *) echo "FATAL: PROTOCOL must be one of {cubic, newreno, bbr, udp}; got '${PROTOCOL}'" >&2; exit 1;;
esac
echo "PROTOCOL=${PROTOCOL}"

# Cell-list override for narrow sweeps. Default is the 12-cell canonical-sweep
# matrix; callers can pass LIMA_CELLS="N M|N M|..." to run a subset.
: "${LIMA_CELLS:=2 1|4 1|8 1|16 1|1 2|1 4|1 8|1 16|1 1|2 2|4 4|8 8}"
IFS='|' read -ra cells <<< "${LIMA_CELLS}"

KERNEL_RELEASE="$(grep '^kernel_release=' "${PROVENANCE}" | cut -d= -f2-)"
IPROUTE2_VERSION="$(grep '^iproute2_version=' "${PROVENANCE}" | cut -d= -f2-)"
IMAGE_SHA="$(grep '^image_sha=' "${PROVENANCE}" | cut -d= -f2-)"

# CSV header.
echo "implementation,strategy,mode,tcp_variant,N,M,bandwidth_mbps,duration_s,rng_run,host,flow_idx,goodput_mbps,flow_drops,flow_marks,git_sha,kernel_release,iproute2_version,image_sha,timestamp_iso" \
  > "${OUT_CSV}"

replicas=(1 2 3)

# Inside-VM script: build 6-netns topology with a SINGLE shared
# bottleneck (router-a -> router-b link carrying netem+cake), so host-A
# and host-B contend on one queue. This mirrors the Stratum probe's
# single-bottleneck topology where both hosts cross one shaped 100Mbps
# link before being distributed to per-sink links. Without the shared
# bottleneck, each host gets its own pipe and the share metric is
# degenerate.
#
# Topology:
#   src-a -- va0/va1 --> router-a
#   src-b -- vb0/vb1 --> router-a
#   router-a -- rab1/rab2 --> router-b   (netem 20ms + cake 100mbit triple-isolate on rab1)
#   router-b -- vsa0/vsa1 --> sink-a
#   router-b -- vsb0/vsb1 --> sink-b
ONE_RUN='
set -eu
N=$1; M=$2; PROTOCOL=$3

# Build iperf3 protocol-specific flags. UDP shares the 100 Mbps offered
# load equally across all flows; TCP variants set the congestion control
# via -C (cubic is the kernel default and needs no flag).
TOTAL=$(( N + M ))
PER_FLOW_MBPS=$(( 100 / TOTAL ))
if [[ "${PROTOCOL}" == "udp" ]]; then
  IPERF_EXTRA="-u -b ${PER_FLOW_MBPS}m"
elif [[ "${PROTOCOL}" == "cubic" ]]; then
  IPERF_EXTRA=""
elif [[ "${PROTOCOL}" == "newreno" ]]; then
  IPERF_EXTRA="-C reno"
else
  IPERF_EXTRA="-C ${PROTOCOL}"
fi

ip -all netns delete 2>/dev/null || true
for nsname in src-a src-b router-a router-b sink-a sink-b; do
  ip netns add "$nsname"
done

ip link add va0 type veth peer name va1
ip link add vb0 type veth peer name vb1
ip link add rab1 type veth peer name rab2
ip link add vsa0 type veth peer name vsa1
ip link add vsb0 type veth peer name vsb1

ip link set va0 netns src-a;    ip link set va1 netns router-a
ip link set vb0 netns src-b;    ip link set vb1 netns router-a
ip link set rab1 netns router-a; ip link set rab2 netns router-b
ip link set vsa0 netns router-b; ip link set vsa1 netns sink-a
ip link set vsb0 netns router-b; ip link set vsb1 netns sink-b

ip -n src-a    addr add 10.1.1.1/24 dev va0; ip -n src-a    link set va0 up
ip -n router-a addr add 10.1.1.2/24 dev va1; ip -n router-a link set va1 up
ip -n src-b    addr add 10.1.2.1/24 dev vb0; ip -n src-b    link set vb0 up
ip -n router-a addr add 10.1.2.2/24 dev vb1; ip -n router-a link set vb1 up
ip -n router-a addr add 10.0.0.1/24 dev rab1; ip -n router-a link set rab1 up
ip -n router-b addr add 10.0.0.2/24 dev rab2; ip -n router-b link set rab2 up
ip -n router-b addr add 10.3.1.2/24 dev vsa0; ip -n router-b link set vsa0 up
ip -n sink-a   addr add 10.3.1.1/24 dev vsa1; ip -n sink-a   link set vsa1 up
ip -n router-b addr add 10.3.2.2/24 dev vsb0; ip -n router-b link set vsb0 up
ip -n sink-b   addr add 10.3.2.1/24 dev vsb1; ip -n sink-b   link set vsb1 up

ip -n src-a route add default via 10.1.1.2
ip -n src-b route add default via 10.1.2.2
ip -n sink-a route add default via 10.3.1.2
ip -n sink-b route add default via 10.3.2.2
ip -n router-a route add 10.3.0.0/16 via 10.0.0.2
ip -n router-b route add 10.1.0.0/16 via 10.0.0.1
ip netns exec router-a sysctl -qw net.ipv4.ip_forward=1
ip netns exec router-b sysctl -qw net.ipv4.ip_forward=1

# Single shared bottleneck: netem 20ms + cake 100mbit on router-a
# egress facing router-b. Both A and B traffic queue here.
ip netns exec router-a tc qdisc add dev rab1 root handle 1: netem delay 20ms
ip netns exec router-a tc qdisc add dev rab1 parent 1: handle 2: cake bandwidth 100mbit triple-isolate

PORT_A=9000; PORT_B=10000
for ((i=0;i<N;i++)); do
  ip netns exec sink-a iperf3 -s -p $((PORT_A+i)) -1 -D 2>/dev/null || true
done
for ((j=0;j<M;j++)); do
  ip netns exec sink-b iperf3 -s -p $((PORT_B+j)) -1 -D 2>/dev/null || true
done
sleep 1

TMP=$(mktemp -d)
for ((i=0;i<N;i++)); do
  ip netns exec src-a iperf3 -c 10.3.1.1 -p $((PORT_A+i)) -t 30 -J -A 0 ${IPERF_EXTRA} \
    > "${TMP}/a-${i}.json" 2>/dev/null &
done
for ((j=0;j<M;j++)); do
  ip netns exec src-b iperf3 -c 10.3.2.1 -p $((PORT_B+j)) -t 30 -J -A 0 ${IPERF_EXTRA} \
    > "${TMP}/b-${j}.json" 2>/dev/null &
done
wait

# UDP per-flow goodput lives at .end.sum.bits_per_second; TCP per-flow
# goodput is at .end.streams[0].sender.bits_per_second.
if [[ "${PROTOCOL}" == "udp" ]]; then
  JQ_GP=".end.sum.bits_per_second // 0"
else
  JQ_GP=".end.streams[0].sender.bits_per_second // 0"
fi
for ((i=0;i<N;i++)); do
  GP_BPS=$(jq -r "${JQ_GP}" "${TMP}/a-${i}.json" 2>/dev/null || echo 0)
  printf "A %d %.6f\n" "$i" "$(echo "${GP_BPS} / 1000000" | bc -l)"
done
for ((j=0;j<M;j++)); do
  GP_BPS=$(jq -r "${JQ_GP}" "${TMP}/b-${j}.json" 2>/dev/null || echo 0)
  printf "B %d %.6f\n" "$j" "$(echo "${GP_BPS} / 1000000" | bc -l)"
done

rm -rf "${TMP}"
ip -all netns delete
'

total=$(( ${#cells[@]} * ${#replicas[@]} ))
i=0
for cell in "${cells[@]}"; do
  read -r N M <<< "${cell}"
  for rng in "${replicas[@]}"; do
    i=$(( i + 1 ))
    printf "[%2d/%d] cell=(%s,%s) rng=%s\n" "$i" "$total" "$N" "$M" "$rng"
    TS_ISO="$(date -u +%FT%TZ)"
    ${LIMACTL} shell "${VM_NAME}" sudo bash -c "${ONE_RUN}" -- "$N" "$M" "${PROTOCOL}" \
      > "${OUT_DIR}/linux-run-${i}.txt" 2>&1 || true
    while read -r host flow_idx gp_mbps; do
      case "${host}" in
        A|B) ;;
        *) continue;;
      esac
      printf "linux,n/a,triple,%s,%d,%d,100.000000,30.0,%d,%s,%s,%s,,,,%s,%s,%s,%s\n" \
        "${PROTOCOL}" "$N" "$M" "$rng" "$host" "$flow_idx" "$gp_mbps" \
        "${KERNEL_RELEASE}" "${IPROUTE2_VERSION}" "${IMAGE_SHA}" "${TS_ISO}" \
        >> "${OUT_CSV}"
    done < "${OUT_DIR}/linux-run-${i}.txt"
  done
done

DATE_END_ISO="$(date -u +%FT%TZ)"
{
  echo "sweep_end_iso=${DATE_END_ISO}"
  echo "rows_emitted=$(wc -l < "${OUT_CSV}")"
} >> "${PROVENANCE}"

echo "Done. CSV at ${OUT_CSV}; provenance at ${PROVENANCE}."
