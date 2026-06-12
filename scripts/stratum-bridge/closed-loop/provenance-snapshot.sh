#!/usr/bin/env bash
# Capture environment manifest into provenance-env.json (spec §8 R2).
set -euo pipefail

OUT="${1:-output/stratum-bridge-closed-loop/provenance-env.json}"
mkdir -p "$(dirname "$OUT")"

to_json() {
    if command -v jq >/dev/null; then
        jq -Rs . <<<"$1"
    else
        python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))' <<<"$1"
    fi
}

git_sha=$(git rev-parse HEAD 2>/dev/null || echo unknown)
git_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
git_dirty=$(test -z "$(git status --porcelain 2>/dev/null)" && echo clean || echo dirty)
kernel=$(uname -srvmo)
distro="unknown"
if [ -r /etc/os-release ]; then
    distro=$(. /etc/os-release && echo "${PRETTY_NAME:-unknown}")
fi
iperf3_v=$(iperf3 --version 2>/dev/null | head -1 || echo "iperf3 not installed")
tcpdump_v=$(tcpdump --version 2>&1 | head -1 || echo "tcpdump not installed")
python_v=$(python3 --version 2>&1 || echo "python3 not installed")
ip_v=$(ip -V 2>&1 | head -1 || echo "iproute2 not installed")
date_iso=$(date -u +%Y-%m-%dT%H:%M:%SZ)

cat > "$OUT" <<EOF
{
  "captured_at": $(to_json "$date_iso"),
  "git_sha": $(to_json "$git_sha"),
  "git_branch": $(to_json "$git_branch"),
  "git_dirty": $(to_json "$git_dirty"),
  "kernel": $(to_json "$kernel"),
  "distro": $(to_json "$distro"),
  "iperf3_version": $(to_json "$iperf3_v"),
  "tcpdump_version": $(to_json "$tcpdump_v"),
  "python_version": $(to_json "$python_v"),
  "iproute2_version": $(to_json "$ip_v")
}
EOF

echo "OK: provenance written to $OUT"
