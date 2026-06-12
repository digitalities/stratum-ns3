#!/usr/bin/env bash
# Remove the closed-loop bridging topology.
set -euo pipefail
for ns in src-a src-b router-a ns3-rt router-b sink-a sink-b; do
    ip netns del "$ns" 2>/dev/null || true
done
echo "OK: netns torn down."
