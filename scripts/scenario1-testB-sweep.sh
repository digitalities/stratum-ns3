#!/bin/bash
# Scenario 1 Test B: PQ — impact of BE packet size on EF OWD and IPDV
# (thesis §4.1, Figures A.3/A.4)
#
# Sweeps EF_pkt_size × BE_pkt_size under PQ scheduling.
# Usage: cd ns3/ns-3-dev && bash ../../scripts/scenario1-testB-sweep.sh

set -e

OUTDIR="../../output/ns3/scenario1-testB"
EF_SIZES="64 128 256 512 1024 1518"
BE_SIZES="100 200 400 600 800 1000 1200 1450"
SIMTIME=50

mkdir -p "$OUTDIR"

echo "=== Scenario 1 Test B: PQ — BE packet size impact ==="
echo "EF sizes: $EF_SIZES"
echo "BE sizes: $BE_SIZES"
echo ""

# OWD results table
{
printf "%-8s" "EF\\BE"
for be in $BE_SIZES; do printf "%8s" "${be}B"; done
echo ""
} > "$OUTDIR/owd-results.txt"

# IPDV results table
{
printf "%-8s" "EF\\BE"
for be in $BE_SIZES; do printf "%8s" "${be}B"; done
echo ""
} > "$OUTDIR/ipdv-results.txt"

for ef in $EF_SIZES; do
    printf "%-8s" "${ef}" >> "$OUTDIR/owd-results.txt"
    printf "%-8s" "${ef}" >> "$OUTDIR/ipdv-results.txt"
    for be in $BE_SIZES; do
        echo -n "  EF=$ef BE=$be ... "
        ./ns3 run "diffserv-example-1 --scheduler=PQ --packetSize=$ef --bePktSize=$be --simTime=$SIMTIME --outputDir=$OUTDIR" > /dev/null 2>&1

        rundir="$OUTDIR/PQ-$(printf '%04d' $ef)"
        if [ -f "$rundir/OWD.tr" ] && [ -s "$rundir/OWD.tr" ]; then
            owd=$(awk 'NR>20 {sum+=$2; n++} END {if(n>0) printf "%.1f", sum/n; else printf "N/A"}' "$rundir/OWD.tr")
            ipdv=$(awk 'NR>20 {sum+=$2; n++} END {if(n>0) printf "%.2f", sum/n; else printf "N/A"}' "$rundir/IPDV.tr")
        else
            owd="N/A"; ipdv="N/A"
        fi
        printf "%8s" "$owd" >> "$OUTDIR/owd-results.txt"
        printf "%8s" "$ipdv" >> "$OUTDIR/ipdv-results.txt"
        echo "OWD=${owd}ms IPDV=${ipdv}ms"

        # Rename output dir to include BE size (avoid overwrite)
        if [ -d "$rundir" ]; then
            mv "$rundir" "$OUTDIR/PQ-EF$(printf '%04d' $ef)-BE$(printf '%04d' $be)"
        fi
    done
    echo "" >> "$OUTDIR/owd-results.txt"
    echo "" >> "$OUTDIR/ipdv-results.txt"
done

echo ""
echo "=== OWD Results (ms) ==="
cat "$OUTDIR/owd-results.txt"
echo ""
echo "=== IPDV Results (ms) ==="
cat "$OUTDIR/ipdv-results.txt"
