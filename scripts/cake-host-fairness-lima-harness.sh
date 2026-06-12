#!/usr/bin/env bash
# Lima VM harness for ground-truth Linux measurement of the CAKE
# host-isolation sweep. Companion fallback to the Docker harness when
# Docker Desktop's linuxkit kernel lacks sch_cake.
#
# Assumes a Lima VM named "cake-host-fairness" is already running with
# kernel sch_cake available, iperf3 + jq + bc installed, and the user
# inside the VM has passwordless sudo. Provisioning command:
#
#   limactl start --name=cake-host-fairness --tty=false template://ubuntu
#   limactl shell cake-host-fairness sudo apt-get install -y iperf3 jq bc
#
# This script records VM provenance (kernel, iproute2 version,
# distribution) to provenance-linux.txt and verifies sch_cake loads.

set -euo pipefail

VM_NAME="cake-host-fairness"
LIMACTL="/opt/homebrew/bin/limactl"
: "${REPO_ROOT:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OUT_DIR="${REPO_ROOT}/output/ns3/cake-host-fairness"
PROVENANCE="${OUT_DIR}/provenance-linux.txt"

mkdir -p "${OUT_DIR}"

# Verify VM exists and is running.
if ! ${LIMACTL} list --format json 2>/dev/null \
    | grep -q "\"name\":\"${VM_NAME}\""; then
  echo "Lima VM '${VM_NAME}' not found. Run:"
  echo "  ${LIMACTL} start --name=${VM_NAME} --tty=false template://ubuntu"
  exit 1
fi

if ! ${LIMACTL} list --format json 2>/dev/null \
    | grep -q "\"status\":\"Running\""; then
  echo "Starting Lima VM '${VM_NAME}'..."
  ${LIMACTL} start "${VM_NAME}"
fi

# Confirm sch_cake module loads inside the VM.
echo "Verifying sch_cake module availability..."
${LIMACTL} shell "${VM_NAME}" sudo modprobe sch_cake 2>&1 || {
  echo "FATAL: sch_cake module unavailable in VM kernel."
  exit 1
}

# BBR availability check (callers needing the bbr congestion control may
# need to fall back if absent). Ubuntu ships tcp_bbr as a kernel module
# that is not auto-loaded; modprobe it before probing.
${LIMACTL} shell "${VM_NAME}" sudo modprobe tcp_bbr 2>/dev/null || true
BBR_AVAILABLE="$(${LIMACTL} shell "${VM_NAME}" sysctl -n net.ipv4.tcp_available_congestion_control 2>/dev/null \
  | tr ' ' '\n' | grep -c '^bbr$' || true)"
if [[ "${BBR_AVAILABLE}" -ge 1 ]]; then
  echo "BBR: available"
  BBR_STATUS="true"
else
  echo "BBR: NOT available — sweep callers must fall back to {cubic, newreno, udp}"
  BBR_STATUS="false"
fi

# Capture provenance.
KERNEL_RELEASE="$(${LIMACTL} shell "${VM_NAME}" uname -r)"
# Extract bare iproute2 version (e.g. "iproute2-6.16.0"). The raw
# `tc -V` output contains commas ("tc utility, iproute2-X.Y.Z,
# libbpf A.B.C") which would corrupt unquoted CSV emission.
IPROUTE2_VERSION="$(${LIMACTL} shell "${VM_NAME}" tc -V 2>&1 \
  | tr -d '\n' \
  | grep -oE 'iproute2-[0-9.]+' \
  | head -1)"
if [[ -z "${IPROUTE2_VERSION}" ]]; then
  IPROUTE2_VERSION="iproute2-unknown"
fi
DISTRO="$(${LIMACTL} shell "${VM_NAME}" bash -c 'cat /etc/os-release | grep PRETTY_NAME | cut -d= -f2 | tr -d \"')"
LIMA_VERSION="$(${LIMACTL} --version 2>&1)"
DATE_ISO="$(date -u +%FT%TZ)"

{
  echo "harness_kind=lima"
  echo "vm_name=${VM_NAME}"
  echo "lima_version=${LIMA_VERSION}"
  echo "distro=${DISTRO}"
  echo "kernel_release=${KERNEL_RELEASE}"
  echo "iproute2_version=${IPROUTE2_VERSION}"
  echo "image_sha="  # not applicable to Lima (no docker image)
  echo "bbr_available=${BBR_STATUS}"
  echo "sweep_start_iso=${DATE_ISO}"
} > "${PROVENANCE}"

echo "Lima harness ready."
echo "Kernel: ${KERNEL_RELEASE}"
echo "iproute2: ${IPROUTE2_VERSION}"
echo "Distro: ${DISTRO}"
echo ""
echo "Next: bash scripts/cake-host-fairness-lima-sweep.sh"
