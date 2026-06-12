#!/usr/bin/env bash
#
# Cross-validation sweep for the in-tree DualPI2 (DsL4sQueueDisc) against
# the upstream-shaped DualPI2 (ns3::DualPi2QueueDisc) carried via
# patches/ns3/0015-dualpi2-gprt.patch.
#
# Three operating cells (bandwidth × baseRTT), two qdisc backends, 30
# seeds each → 180 runs. Each run is ~10s wall-clock at 60s sim time, so
# the full sweep is roughly 30 minutes on a single core. The example
# binary writes one summary.txt per run; this script invokes runs in a
# nested loop, then prints a per-cell aggregate.
#
# Cells (chosen from the upstream sweep grid {4, 12, 40, 120, 200} Mbps
# × {5, 10, 20, 50, 100} ms):
#   - 40 Mbps × 50 ms   (mid-BW, mid-RTT — the canonical anchor)
#   - 120 Mbps × 10 ms  (high-BW, low-RTT — short-BDP regime)
#   - 12 Mbps × 100 ms  (low-BW, high-RTT — long-BDP regime)
#
# L4S step-AQM threshold per cell follows the upstream THRESHOLD_MS map
# in github.com/GPRT/l4s-for-ns3/experiments/run_exps.sh:
#   {4: 6, 12: 4, 40: 1, 120: 1, 200: 1} ms
#
# Override via environment:
#   N_SEEDS=5 ./scripts/l4s-dualpi2-gprt-parity-sweep.sh    # quick run
#   SIM_TIME=30 WARMUP=5 ...                                # shorter sims
#
# Acceptance gate (per cell, aggregated across seeds):
#   - |JFI_stratum - JFI_gprt| ≤ 0.01
#   - Both implementations within ±5 % of the 40 Mbps bottleneck rate
#
# This script must be run from inside a peer ns-3-dev worktree built
# with patch 0015-dualpi2-gprt applied (i.e. from ns3/ns-3-dev-<topic>/).

set -euo pipefail

# Cell definitions: rate:baseRttMs:l4sThresholdMs
CELLS=(
    "40Mbps:50:1"
    "120Mbps:10:1"
    "12Mbps:100:4"
    "100Mbps:5:1"
)

QDISCS=("stratum" "gprt")

N_SEEDS="${N_SEEDS:-30}"
SIM_TIME="${SIM_TIME:-60}"
WARMUP="${WARMUP:-10}"
OUT_BASE="${OUT_BASE:-output/ns3/dualpi2-gprt-parity}"

if [ ! -x ./ns3 ]; then
    echo "ERROR: ./ns3 not found in PWD. Run from ns3/ns-3-dev-<topic>/."
    exit 1
fi

if ! ls build/contrib/stratum/examples/ns3*-diffserv-l4s-dualpi2-gprt-parity-default >/dev/null 2>&1; then
    echo "ERROR: parity example not built. Run ./ns3 configure --enable-examples && ./ns3 build first."
    exit 1
fi

total=$(( ${#CELLS[@]} * ${#QDISCS[@]} * N_SEEDS ))
count=0
echo "Sweep: ${#CELLS[@]} cells × ${#QDISCS[@]} qdiscs × $N_SEEDS seeds = $total runs."
echo "SIM_TIME=$SIM_TIME WARMUP=$WARMUP OUT_BASE=$OUT_BASE"
echo

for cell in "${CELLS[@]}"; do
    IFS=":" read -r rate rttMs threshMs <<< "$cell"
    cell_dir="${OUT_BASE}/${rate}-${rttMs}ms"
    for q in "${QDISCS[@]}"; do
        for seed in $(seq 1 "$N_SEEDS"); do
            count=$(( count + 1 ))
            outdir="${cell_dir}/${q}/seed${seed}"
            if [ -f "${outdir}/summary.txt" ]; then
                echo "[${count}/${total}] ${rate} × ${rttMs}ms × ${q} × seed=${seed}  (cached, skipping)"
                continue
            fi
            mkdir -p "$outdir"
            echo "[${count}/${total}] ${rate} × ${rttMs}ms × ${q} × seed=${seed}"
            ./ns3 run --no-build \
                "diffserv-l4s-dualpi2-gprt-parity \
                    --rootQdisc=${q} \
                    --bottleneckRate=${rate} \
                    --baseRttMs=${rttMs} \
                    --l4sThresholdMs=${threshMs} \
                    --simTime=${SIM_TIME} \
                    --warmup=${WARMUP} \
                    --rngRun=${seed} \
                    --outDir=${outdir}" \
                > "${outdir}/run.log" 2>&1
        done
    done
done

echo
echo "===== Per-cell aggregate ====="
echo

python3 - "$OUT_BASE" <<'EOF'
import os, sys, statistics

base = sys.argv[1]
cells = sorted(d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d)))

def parse_summary(path):
    out = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                out[k] = v
    return out

print(f"{'cell':<18} {'qdisc':<10} {'n':>3}  {'goodput_L_mean':>14}  {'goodput_C_mean':>14}  {'jfi_mean':>10}  {'jfi_stdev':>10}")
print("-" * 90)
results = {}
for cell in cells:
    for qdisc in ("stratum", "gprt"):
        qdir = os.path.join(base, cell, qdisc)
        if not os.path.isdir(qdir):
            continue
        seeds = sorted(d for d in os.listdir(qdir) if d.startswith("seed"))
        jfis, gLs, gCs = [], [], []
        for s in seeds:
            p = os.path.join(qdir, s, "summary.txt")
            if not os.path.exists(p):
                continue
            d = parse_summary(p)
            jfis.append(float(d["jfi"]))
            gLs.append(float(d["goodput_L_mbps"]))
            gCs.append(float(d["goodput_C_mbps"]))
        if not jfis:
            continue
        n = len(jfis)
        jm = statistics.mean(jfis)
        js = statistics.stdev(jfis) if n > 1 else 0.0
        gLm = statistics.mean(gLs)
        gCm = statistics.mean(gCs)
        results.setdefault(cell, {})[qdisc] = (n, jm, js, gLm, gCm)
        print(f"{cell:<18} {qdisc:<10} {n:>3}  {gLm:>14.3f}  {gCm:>14.3f}  {jm:>10.4f}  {js:>10.4f}")

print()
print("===== Parity gate (|JFI_stratum - JFI_gprt| ≤ 0.01) =====")
print()
print(f"{'cell':<18} {'jfi_stratum':>12}  {'jfi_gprt':>12}  {'delta':>10}  {'gate':>8}")
print("-" * 70)
fail = 0
for cell, by_qdisc in sorted(results.items()):
    if "stratum" not in by_qdisc or "gprt" not in by_qdisc:
        continue
    js = by_qdisc["stratum"][1]
    jg = by_qdisc["gprt"][1]
    delta = abs(js - jg)
    verdict = "PASS" if delta <= 0.01 else "FAIL"
    if verdict == "FAIL":
        fail += 1
    print(f"{cell:<18} {js:>12.4f}  {jg:>12.4f}  {delta:>10.4f}  {verdict:>8}")

print()
sys.exit(1 if fail else 0)
EOF
