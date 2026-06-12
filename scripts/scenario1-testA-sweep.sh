#!/bin/bash
# Scenario 1 Test A: STAR parameter sweep (thesis §4.1 Test A)
#
# Sweeps STAR × packet_size under WFQ scheduling, collecting mean OWD.
# Reproduces thesis Figures A.1/A.2.
#
# Usage: cd ns3/ns-3-dev && bash ../../scripts/scenario1-testA-sweep.sh

set -e

OUTDIR="../../output/ns3/scenario1-testA"
STARS="1.0 1.5 2.0 3.0 4.0 5.0 6.0"
PKTSIZES="64 128 256 512 1024 1518"
SIMTIME=50

mkdir -p "$OUTDIR"

echo "=== Scenario 1 Test A: STAR sweep ==="
echo "STAR values: $STARS"
echo "Packet sizes: $PKTSIZES"
echo "SimTime: ${SIMTIME}s"
echo ""

# Header for results table
printf "%-6s" "STAR" > "$OUTDIR/results.txt"
for ps in $PKTSIZES; do
    printf "%8s" "${ps}B" >> "$OUTDIR/results.txt"
done
echo "" >> "$OUTDIR/results.txt"

for star in $STARS; do
    printf "%-6s" "$star" >> "$OUTDIR/results.txt"
    for ps in $PKTSIZES; do
        echo -n "  STAR=$star pktSize=$ps ... "
        ./ns3 run "diffserv-example-1 --scheduler=WFQ --star=$star --packetSize=$ps --simTime=$SIMTIME --outputDir=$OUTDIR" > /dev/null 2>&1

        # Extract mean OWD (skip first 20 samples for warmup)
        owdfile="$OUTDIR/STAR-${star}-WFQ-$(printf '%04d' $ps)/OWD.tr"
        if [ -f "$owdfile" ] && [ -s "$owdfile" ]; then
            mean_owd=$(awk 'NR>20 {sum+=$2; n++} END {if(n>0) printf "%.1f", sum/n; else printf "N/A"}' "$owdfile")
        else
            mean_owd="N/A"
        fi
        printf "%8s" "$mean_owd" >> "$OUTDIR/results.txt"
        echo "$mean_owd ms"
    done
    echo "" >> "$OUTDIR/results.txt"
done

echo ""
echo "=== Results (mean OWD in ms) ==="
cat "$OUTDIR/results.txt"
echo ""
echo "Results saved to $OUTDIR/results.txt"
