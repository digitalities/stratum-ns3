#!/usr/bin/env bash
# Reproduce the CAKE+L4S composition fairness result reported in the paper
# (section "Composing CAKE and L4S"): one scalable (DCTCP) and one classic
# (Cubic) flow share a diffserv tin on a 40 Mbit/s, 50 ms-RTT bottleneck.
# Compares the CAKE client (per-tin DualPI2 inner) against the bare L4S
# client (standalone DualPI2) and the GPRT reference, reporting Jain's
# Fairness Index per qdisc.
#
# Run from the ns-3-dev peer (the diffserv-l4s-dualpi2-gprt-parity example
# must be built). Same harness for all three qdiscs; only the root differs.
set -euo pipefail

NS3_DEV="${NS3_DEV:-$(cd "$(dirname "$0")/../ns3/ns-3-dev" 2>/dev/null && pwd)}"
OUTDIR="${OUTDIR:-$(cd "$(dirname "$0")/.." && pwd)/output/cake-l4s-fairness}"
SEEDS="${SEEDS:-1 2 3 4 5 6 7 8}"
RATE="${RATE:-40Mbps}"
RTT="${RTT:-50}"
SIMTIME="${SIMTIME:-60}"
WARMUP="${WARMUP:-10}"

BIN=$(find "$NS3_DEV/build" -maxdepth 5 -type f -perm -u+x \
        -name '*diffserv-l4s-dualpi2-gprt-parity*' | head -1)
[ -z "$BIN" ] && { echo "ERROR: parity binary not found; build the example first." >&2; exit 1; }

mkdir -p "$OUTDIR"
for q in l4s cake gprt; do
    for s in $SEEDS; do
        "$BIN" --rootQdisc="$q" --bottleneckRate="$RATE" --baseRttMs="$RTT" \
               --l4sThresholdMs=1 --simTime="$SIMTIME" --warmup="$WARMUP" \
               --rngRun="$s" --outDir="$OUTDIR/$q-$s" >/dev/null 2>&1
    done
done

python3 - "$OUTDIR" "$SEEDS" <<'PY'
import sys, glob, statistics as st
base, seeds = sys.argv[1], sys.argv[2].split()
print(f"qdisc     n  JFI mean  sd      min     L Mbps  C Mbps")
for q in ("l4s", "cake", "gprt"):
    J = L = C = None
    J, L, C = [], [], []
    for d in sorted(glob.glob(f"{base}/{q}-*")):
        s = {}
        for line in open(f"{d}/summary.txt"):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                s[k] = v
        J.append(float(s["jfi"])); L.append(float(s["goodput_L_mbps"])); C.append(float(s["goodput_C_mbps"]))
    if not J:
        continue
    print(f"{q:<8}  {len(J)}  {st.mean(J):.4f}    {st.pstdev(J):.4f}  {min(J):.4f}  "
          f"{st.mean(L):6.1f}  {st.mean(C):6.1f}")
PY
echo "Per-seed summaries under $OUTDIR/<qdisc>-<seed>/summary.txt"
