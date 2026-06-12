#!/bin/bash
# Scenario 1 Test C: WFQ vs PQ — OWD and IPDV comparison
# (thesis §4.1, Figures A.5/A.6)
#
# Sweeps EF_pkt_size for both WFQ and PQ with 23 BE flows (64-1472B, 64B step).
# Usage: cd ns3/ns-3-dev && bash ../../scripts/scenario1-testC-sweep.sh

set -e

OUTDIR="../../output/ns3/scenario1-testC"
EF_SIZES="64 128 256 512 1024 1518"
SCHEDULERS="PQ WFQ"
SIMTIME=50

mkdir -p "$OUTDIR"

echo "=== Scenario 1 Test C: WFQ vs PQ comparison ==="
echo "EF sizes: $EF_SIZES"
echo "Schedulers: $SCHEDULERS"
echo "BE: 23 flows, 64-1472B (64B step), 100kbps each"
echo ""

# Results table
{
printf "%-8s" "EF"
for s in $SCHEDULERS; do printf "%10s %10s" "${s}_OWD" "${s}_IPDV"; done
echo ""
} > "$OUTDIR/results.txt"

for ef in $EF_SIZES; do
    printf "%-8s" "${ef}" >> "$OUTDIR/results.txt"
    for sched in $SCHEDULERS; do
        echo -n "  $sched EF=$ef ... "
        ./ns3 run "diffserv-example-1 --scheduler=$sched --packetSize=$ef --beFlows=23 --simTime=$SIMTIME --outputDir=$OUTDIR" > /dev/null 2>&1

        rundir="$OUTDIR/${sched}-$(printf '%04d' $ef)"
        if [ -f "$rundir/OWD.tr" ] && [ -s "$rundir/OWD.tr" ]; then
            owd=$(awk 'NR>20 {sum+=$2; n++} END {if(n>0) printf "%.1f", sum/n; else printf "N/A"}' "$rundir/OWD.tr")
            ipdv=$(awk 'NR>20 {sum+=$2; n++} END {if(n>0) printf "%.2f", sum/n; else printf "N/A"}' "$rundir/IPDV.tr")
        else
            owd="N/A"; ipdv="N/A"
        fi
        printf "%10s %10s" "$owd" "$ipdv" >> "$OUTDIR/results.txt"
        echo "OWD=${owd}ms IPDV=${ipdv}ms"
    done
    echo "" >> "$OUTDIR/results.txt"
done

echo ""
echo "=== Results ==="
cat "$OUTDIR/results.txt"
echo ""
echo "Thesis pattern: PQ always performs better than WFQ on OWD."
echo "For small packets the advantage is minor."
