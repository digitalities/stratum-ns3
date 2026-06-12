#!/usr/bin/env bash
# D1 risk mitigation (spec §9 row D1):
# Verify EmuFdNetDevice + Lima kernel can pass a single ICMP packet through
# the ns-3 bridge with no shaping, BEFORE layering in CAKE + iperf3.
# If this fails, fall back to TapBridge before any qdisc work.

set -euo pipefail

REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
NS3_DEV=${NS3_DEV:-$REPO/ns3/ns-3-dev}
EXAMPLE="$NS3_DEV/build/contrib/stratum/examples/ns3.47-cake-stratum-bridge-router-default"

if [ ! -x "$EXAMPLE" ]; then
    # Search for the binary under build/ (suffix varies with ns-3 version).
    EXAMPLE=$(find "$NS3_DEV/build" -maxdepth 5 -name '*cake-stratum-bridge-router*' -type f -perm -u+x 2>/dev/null | head -1)
    if [ -z "$EXAMPLE" ]; then
        echo "FAIL: cake-stratum-bridge-router binary not found under $NS3_DEV/build"
        echo "Build first: cd $NS3_DEV && ./ns3 build cake-stratum-bridge-router"
        exit 1
    fi
fi
echo "Using example binary: $EXAMPLE"

bash "$REPO/scripts/stratum-bridge/closed-loop/setup-netns.sh"

ip netns exec ns3-rt env \
    NS3_COMMIT="$(git -C "$NS3_DEV" rev-parse HEAD 2>/dev/null || echo unknown)" \
    DIFFSERV_SHA="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)" \
    "$EXAMPLE" \
        --bandwidth=10Gbps --simTime=20 --RngRun=1 \
        > /tmp/closed-loop-smoke-ns3.stdout 2> /tmp/closed-loop-smoke-ns3.stderr &
NS3_PID=$!
trap 'kill -TERM "$NS3_PID" 2>/dev/null || true' EXIT

echo "ns-3 PID $NS3_PID; sleeping 5s for raw-socket establishment..."
sleep 5

echo "Ping src-a -> sink-a (one packet, 5s timeout):"
if ip netns exec src-a ping -c 1 -W 5 10.3.1.10; then
    echo ""
    echo "===================================="
    echo "SMOKE PASS: bridge passes ICMP."
    echo "===================================="
    rc=0
else
    echo ""
    echo "===================================="
    echo "SMOKE FAIL: ICMP did not traverse the bridge."
    echo "  ns-3 stderr (tail):"
    tail -20 /tmp/closed-loop-smoke-ns3.stderr || true
    echo "  netns state:"
    ip -n ns3-rt addr show
    ip -n router-a route show
    ip -n router-b route show
    echo "===================================="
    rc=1
fi

kill -TERM "$NS3_PID" 2>/dev/null || true
sleep 2
kill -KILL "$NS3_PID" 2>/dev/null || true
wait "$NS3_PID" 2>/dev/null || true
trap - EXIT
exit "$rc"
