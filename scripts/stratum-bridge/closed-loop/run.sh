#!/usr/bin/env bash
# Top-level orchestration: 3 replicas of the closed-loop bridge experiment.
# All artifacts under output/stratum-bridge-closed-loop/.

set -euo pipefail

REPO=${REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}
NS3_DEV=${NS3_DEV:-$REPO/ns3/ns-3-dev}
OUTDIR=${OUTDIR:-"$REPO/output/stratum-bridge-closed-loop"}
TCP_VARIANT=${TCP_VARIANT:-cubic}  # cubic | reno | bbr (passed to iperf3 -C)

# Prefer the optimized build (strips NS3_ASSERT + NS3_LOG = much less per-packet
# overhead, required to keep up with 17-flow 100Mbps EmuFdNetDevice load on
# 4 CPUs without the RealtimeSimulator falling behind).
EXAMPLE=$(find "$NS3_DEV/build" -maxdepth 5 -name '*cake-stratum-bridge-router-optimized*' -type f -perm -u+x 2>/dev/null | head -1)
if [ -z "$EXAMPLE" ]; then
    EXAMPLE=$(find "$NS3_DEV/build" -maxdepth 5 -name '*cake-stratum-bridge-router*' -type f -perm -u+x 2>/dev/null | head -1)
fi
if [ -z "$EXAMPLE" ]; then
    echo "FATAL: cake-stratum-bridge-router binary not found under $NS3_DEV/build"
    exit 1
fi
echo "Example binary: $EXAMPLE"

mkdir -p "$OUTDIR"

# 0. Environment manifest
"$REPO/scripts/stratum-bridge/closed-loop/provenance-snapshot.sh" \
    "$OUTDIR/provenance-env.json"

# 1. Topology
bash "$REPO/scripts/stratum-bridge/closed-loop/setup-netns.sh"

run_replica() {
    local N="$1"
    local RDIR="$OUTDIR/r$N"
    mkdir -p "$RDIR"
    echo ""
    echo "================================================================"
    echo "Replica $N starting at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "================================================================"

    # 1a. ns-3 bridge in background inside ns3-rt, under SCHED_FIFO priority 50.
    # The real-time scheduler in ns-3 calls NS_FATAL_ERROR when wall-clock falls
    # behind simulator time (which happens easily under 16-flow CAKE load on
    # 4 CPUs). chrt -f gives ns-3 RT priority over iperf3 + tcpdump so it can
    # keep up.
    chrt -f 50 ip netns exec ns3-rt env \
        NS3_COMMIT="$(git -C "$NS3_DEV" rev-parse HEAD 2>/dev/null || echo unknown)" \
        DIFFSERV_SHA="$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)" \
        "$EXAMPLE" \
            --RngRun="$N" \
            --bandwidth=100Mbps \
            --hostLoadSource=BulkFlowCount \
            --simTime=85 \
            --provenanceOut="$RDIR/provenance.json" \
            ${EXTRA_NS3_FLAGS:-} \
        > "$RDIR/ns3.stdout" 2> "$RDIR/ns3.stderr" &
    local NS3_PID=$!
    # Note: --enqueuePcap / --dequeuePcap intentionally omitted. Under 17-flow
    # 100Mbps load on 4 CPUs, the per-packet trace-callback writes added enough
    # I/O latency that ns-3's RealtimeSimulator could not keep up with the
    # kernel's packet-arrival rate and crashed (NS_ASSERT ts >= m_currentTs).
    # The verdict is computed from iperf3-sender JSON; the tcpdump captures at
    # src-a + sink-a provide cross-validation against pcap. We can still get the
    # ns-3-side captures from a separate diagnostic run if needed for ADR.

    echo "[r$N] ns-3 PID $NS3_PID; waiting 5s for raw-socket establishment..."
    sleep 5

    # 1b. Clean any stale iperf3 servers + start fresh (use per-replica ports
    # to avoid cross-replica port-collision races).
    ip netns exec sink-a pkill -f 'iperf3 -s' 2>/dev/null || true
    ip netns exec sink-b pkill -f 'iperf3 -s' 2>/dev/null || true
    sleep 0.5
    PORT=$((5200 + N))   # r1 -> 5201, r2 -> 5202, r3 -> 5203
    ip netns exec sink-a iperf3 -s -D -p $PORT \
        -I "$RDIR/iperf3-sink-a.pid" \
        --logfile "$RDIR/iperf3-sink-a.log" 2>/dev/null || true
    ip netns exec sink-b iperf3 -s -D -p $PORT \
        -I "$RDIR/iperf3-sink-b.pid" \
        --logfile "$RDIR/iperf3-sink-b.log" 2>/dev/null || true
    # Verify both servers are LISTENING before launching clients
    for try in 1 2 3 4 5 6 7 8; do
        ok_a=$(ip netns exec sink-a ss -tln "( sport = :$PORT )" | wc -l)
        ok_b=$(ip netns exec sink-b ss -tln "( sport = :$PORT )" | wc -l)
        if [ "$ok_a" -gt 1 ] && [ "$ok_b" -gt 1 ]; then break; fi
        sleep 0.5
    done
    if [ "$ok_a" -le 1 ] || [ "$ok_b" -le 1 ]; then
        echo "[r$N] WARN: iperf3 server not LISTENING after retries (a=$ok_a b=$ok_b)"
        cat "$RDIR/iperf3-sink-a.log" 2>/dev/null
        cat "$RDIR/iperf3-sink-b.log" 2>/dev/null
    fi

    # 1c. tcpdump on src-a egress + sink-a ingress (-s 96 = header-only)
    ip netns exec src-a  tcpdump -i vsrca  -w "$RDIR/src-a.pcap"  -s 96 -q \
        > /dev/null 2>&1 &
    local TD_SRCA=$!
    ip netns exec sink-a tcpdump -i vsinka -w "$RDIR/sink-a.pcap" -s 96 -q \
        > /dev/null 2>&1 &
    local TD_SINKA=$!
    sleep 1

    # 1d. iperf3 clients (60s test). --connect-timeout fails fast on stuck
    # TCP SYN instead of letting iperf3's internal retries waste minutes.
    ip netns exec src-a iperf3 \
        -c 10.3.1.10 -p $PORT -P 16 -t 60 -C $TCP_VARIANT \
        --connect-timeout 5000 \
        --json \
        > "$RDIR/iperf3-src-a.json" 2> "$RDIR/iperf3-src-a.stderr" &
    local C_A=$!
    ip netns exec src-b iperf3 \
        -c 10.3.2.10 -p $PORT -P 1 -t 60 -C $TCP_VARIANT \
        --connect-timeout 5000 \
        --json \
        > "$RDIR/iperf3-src-b.json" 2> "$RDIR/iperf3-src-b.stderr" &
    local C_B=$!

    echo "[r$N] iperf3 clients started; waiting ~70s for completion..."
    wait "$C_A" "$C_B"
    echo "[r$N] iperf3 clients finished at $(date -u +%Y-%m-%dT%H:%M:%SZ)."

    # 1e. Stop captures and ns-3
    kill -INT "$TD_SRCA"  2>/dev/null || true
    kill -INT "$TD_SINKA" 2>/dev/null || true
    sleep 1
    # Kill iperf3 servers (started with -D, no auto-exit)
    pkill -F "$RDIR/iperf3-sink-a.pid" 2>/dev/null || true
    pkill -F "$RDIR/iperf3-sink-b.pid" 2>/dev/null || true
    ip netns exec sink-a pkill -f 'iperf3 -s' 2>/dev/null || true
    ip netns exec sink-b pkill -f 'iperf3 -s' 2>/dev/null || true
    kill -TERM "$NS3_PID" 2>/dev/null || true
    sleep 2
    kill -KILL "$NS3_PID" 2>/dev/null || true
    wait "$NS3_PID" 2>/dev/null || true

    echo "[r$N] artifacts under $RDIR/"
}

for N in 1 2 3; do
    run_replica "$N"
done

# 2. Teardown
bash "$REPO/scripts/stratum-bridge/closed-loop/teardown-netns.sh"

# 3. Verdict
python3 "$REPO/scripts/stratum-bridge/closed-loop/verdict.py" \
    --bands "$REPO/scripts/stratum-bridge/closed-loop/bands.yaml" \
    --output-dir "$OUTDIR" \
    --replica-glob "$OUTDIR/r*/"
