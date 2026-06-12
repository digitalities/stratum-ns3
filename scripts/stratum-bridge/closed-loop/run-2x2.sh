#!/usr/bin/env bash
# CAKE+L4S 2x2 at (nA=16, nB=1): plain ns-3 vs bridge × {CUBIC, DCTCP}.
# All cells run with host-isolation OFF and DualPI2 L4S inner.
# Matches the Lima 2x2 anchor layout (sim_time=40, n=5).

set -euo pipefail

REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
NS3_DEV=${NS3_DEV:-$REPO/ns3/ns-3-dev}
OUTBASE=${OUTBASE:-$REPO/output/cake-l4s-composition-16-1-hetzner-ccx33}
REPLICAS=${REPLICAS:-5}
SIM_TIME=${SIM_TIME:-40}
# Bridge cells need longer sim_time than plain cells so ns-3 outlives iperf3 (otherwise iperf3 hangs on dead TCP control channel).
BRIDGE_SIM_TIME=${BRIDGE_SIM_TIME:-60}
BRIDGE_TEST_DUR=${BRIDGE_TEST_DUR:-35}
BANDWIDTH=${BANDWIDTH:-100Mbps}
SKIP_PLAIN=${SKIP_PLAIN:-0}

PLAIN=$(find "$NS3_DEV/build" -maxdepth 5 -name '*cake-l4s-composition-optimized*' -type f -perm -u+x 2>/dev/null | head -1)
BRIDGE=$(find "$NS3_DEV/build" -maxdepth 5 -name '*cake-stratum-bridge-router-optimized*' -type f -perm -u+x 2>/dev/null | head -1)
[ -z "$PLAIN" ]  && { echo "FATAL: cake-l4s-composition binary not found"; exit 1; }
[ -z "$BRIDGE" ] && { echo "FATAL: cake-stratum-bridge-router binary not found"; exit 1; }

modprobe tcp_dctcp 2>/dev/null || true
grep -q dctcp /proc/sys/net/ipv4/tcp_available_congestion_control \
  || { echo "FATAL: tcp_dctcp not loadable"; exit 1; }

mkdir -p "$OUTBASE"

run_plain_cell() {
    local VARIANT="$1"
    local CELL="plain-$VARIANT"
    local CDIR="$OUTBASE/$CELL"
    mkdir -p "$CDIR"
    echo ""
    echo "================================================================"
    echo "Cell $CELL starting at $(date -u +%H:%M:%S)"
    echo "================================================================"
    for ((N=1; N<=REPLICAS; N++)); do
        local RDIR="$CDIR/r$N"
        mkdir -p "$RDIR"
        echo "[$CELL r$N] launching cake-l4s-composition (sim=${SIM_TIME}s, RngRun=$N)"
        "$PLAIN" \
            --bandwidth=$BANDWIDTH \
            --simTime=$SIM_TIME \
            --RngRun=$N \
            --nA=16 --nB=1 \
            --variantA=$VARIANT --variantB=$VARIANT \
            --jsonOut="$RDIR/composition.json" \
            > "$RDIR/ns3.stdout" 2> "$RDIR/ns3.stderr"
    done
    echo "[$CELL] all $REPLICAS replicas done at $(date -u +%H:%M:%S)"
}

# Bridge cells: localized copy of run.sh body. Reuses setup-netns.sh + teardown.
run_bridge_cell() {
    local VARIANT="$1"
    local CELL="bridge-$VARIANT"
    local CDIR="$OUTBASE/$CELL"
    mkdir -p "$CDIR"
    echo ""
    echo "================================================================"
    echo "Cell $CELL starting at $(date -u +%H:%M:%S)"
    echo "================================================================"

    bash "$REPO/scripts/stratum-bridge/closed-loop/setup-netns.sh"

    for ((N=1; N<=REPLICAS; N++)); do
        local RDIR="$CDIR/r$N"
        mkdir -p "$RDIR"
        local PORT=$((5300 + N))   # ports 5301..5305 for 2x2 (no collision w/ sweep ports 5201..5203)
        echo "[$CELL r$N] starting"

        chrt -f 50 ip netns exec ns3-rt env \
            NS3_COMMIT="cake-l4s-16-1-$VARIANT" \
            DIFFSERV_SHA="hetzner-ccx33" \
            "$BRIDGE" \
                --RngRun=$N \
                --bandwidth=$BANDWIDTH \
                --simTime=$BRIDGE_SIM_TIME \
                --enableHostIsolation=false \
                --useDualPi2Inner=true \
                --hostLoadSource=BulkFlowCount \
                --provenanceOut="$RDIR/provenance.json" \
            > "$RDIR/ns3.stdout" 2> "$RDIR/ns3.stderr" &
        local NS3_PID=$!
        sleep 5

        ip netns exec sink-a pkill -f 'iperf3 -s' 2>/dev/null || true
        ip netns exec sink-b pkill -f 'iperf3 -s' 2>/dev/null || true
        sleep 0.5
        ip netns exec sink-a iperf3 -s -D -p $PORT -I "$RDIR/iperf3-sink-a.pid" --logfile "$RDIR/iperf3-sink-a.log" 2>/dev/null || true
        ip netns exec sink-b iperf3 -s -D -p $PORT -I "$RDIR/iperf3-sink-b.pid" --logfile "$RDIR/iperf3-sink-b.log" 2>/dev/null || true
        for try in 1 2 3 4 5 6 7 8; do
            ok_a=$(ip netns exec sink-a ss -tln "( sport = :$PORT )" | wc -l)
            ok_b=$(ip netns exec sink-b ss -tln "( sport = :$PORT )" | wc -l)
            [ "$ok_a" -gt 1 ] && [ "$ok_b" -gt 1 ] && break
            sleep 0.5
        done

        # iperf3 test duration is fixed (BRIDGE_TEST_DUR=35); BRIDGE_SIM_TIME (=60) gives
        # ns-3 ~20s of buffer past iperf3 completion so the bridge outlives the test.
        ip netns exec src-a iperf3 -c 10.3.1.10 -p $PORT -P 16 -t $BRIDGE_TEST_DUR -C $VARIANT --connect-timeout 5000 --json \
            > "$RDIR/iperf3-src-a.json" 2> "$RDIR/iperf3-src-a.stderr" &
        local C_A=$!
        ip netns exec src-b iperf3 -c 10.3.2.10 -p $PORT -P 1  -t $BRIDGE_TEST_DUR -C $VARIANT --connect-timeout 5000 --json \
            > "$RDIR/iperf3-src-b.json" 2> "$RDIR/iperf3-src-b.stderr" &
        local C_B=$!

        wait "$C_A" "$C_B"

        pkill -F "$RDIR/iperf3-sink-a.pid" 2>/dev/null || true
        pkill -F "$RDIR/iperf3-sink-b.pid" 2>/dev/null || true
        ip netns exec sink-a pkill -f 'iperf3 -s' 2>/dev/null || true
        ip netns exec sink-b pkill -f 'iperf3 -s' 2>/dev/null || true
        kill -TERM "$NS3_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$NS3_PID" 2>/dev/null || true
        wait "$NS3_PID" 2>/dev/null || true
    done

    bash "$REPO/scripts/stratum-bridge/closed-loop/teardown-netns.sh"
    echo "[$CELL] all $REPLICAS replicas done at $(date -u +%H:%M:%S)"
}

if [ "$SKIP_PLAIN" != "1" ]; then
    run_plain_cell  cubic
    run_plain_cell  dctcp
else
    echo "SKIP_PLAIN=1 — skipping plain-cubic and plain-dctcp cells"
fi
run_bridge_cell cubic
run_bridge_cell dctcp

echo ""
echo "=== ALL 2x2 CELLS DONE at $(date -u +%H:%M:%S) ==="
echo "=== outputs ==="
find "$OUTBASE" -maxdepth 3 -name 'composition.json' -o -name 'iperf3-src-a.json' | sort
