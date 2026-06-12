#!/usr/bin/env python3
"""Stratum-bridge prototype: scenario-emission half.

Reads a Stratum scenario IR (YAML, schema stratum-bridge/scenario/v1)
and writes a self-contained bash script to stdout that, when run on a
host with the named Lima VM available, builds the equivalent Linux
netns testbed and reports per-flow goodput + share_A.

The emitted script is intentionally self-contained: it does not depend
on this Python tool or other repository scripts at run time, only on
the existence of the named Lima VM with sch_cake / iperf3 / jq / bc
available (see scripts/cake-host-fairness-lima-harness.sh for one-time
provenance setup).

Usage:
    python3 scripts/stratum-bridge/emit-netns.py \\
        scripts/stratum-bridge/scenarios/cake-16-1-cubic.yaml \\
        > /tmp/cake-16-1-cubic.sh
    bash /tmp/cake-16-1-cubic.sh
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from datetime import datetime, timezone
from pathlib import Path
from textwrap import dedent

import yaml

SCHEMA_V1 = "stratum-bridge/scenario/v1"
ALLOWED_PROTOCOLS = {"cubic", "newreno", "bbr", "udp"}


def _fail(msg: str) -> None:
    print(f"emit-netns: error: {msg}", file=sys.stderr)
    sys.exit(2)


def _validate(scenario: dict, path: Path) -> None:
    schema = scenario.get("schema")
    if schema != SCHEMA_V1:
        _fail(f"{path}: schema must be {SCHEMA_V1!r} (got {schema!r})")
    if scenario.get("topology", {}).get("kind") != "shared-bottleneck":
        _fail(f"{path}: only topology.kind = shared-bottleneck is supported in v1")
    if scenario.get("qdisc", {}).get("kind") != "cake":
        _fail(f"{path}: only qdisc.kind = cake is supported in v1")
    if scenario.get("qdisc", {}).get("mode") != "triple-isolate":
        _fail(f"{path}: only qdisc.mode = triple-isolate is supported in v1")
    if scenario.get("backend", {}).get("kind") != "linux-netns":
        _fail(f"{path}: only backend.kind = linux-netns is supported in v1")
    protocol = scenario.get("traffic", {}).get("protocol")
    if protocol not in ALLOWED_PROTOCOLS:
        _fail(f"{path}: traffic.protocol must be one of {sorted(ALLOWED_PROTOCOLS)} (got {protocol!r})")


def emit(scenario: dict, source_path: Path) -> str:
    name = scenario["name"]
    bandwidth = int(scenario["bottleneck"]["bandwidth_mbps"])
    delay_ms = int(scenario["bottleneck"]["one_way_delay_ms"])
    n_a = int(scenario["hosts"]["a"]["flow_count"])
    n_b = int(scenario["hosts"]["b"]["flow_count"])
    protocol = scenario["traffic"]["protocol"]
    duration_s = int(scenario["traffic"]["duration_s"])
    replicas = list(scenario["traffic"]["replicas"])
    vm_name = scenario["backend"]["vm"]

    source_text = source_path.read_text()
    ir_digest = hashlib.sha256(source_text.encode("utf-8")).hexdigest()[:12]
    emitted_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    if protocol == "udp":
        iperf_extra = f"-u -b $(( {bandwidth} / ({n_a} + {n_b}) ))m"
    elif protocol == "cubic":
        iperf_extra = ""
    elif protocol == "newreno":
        iperf_extra = "-C reno"
    else:
        iperf_extra = f"-C {protocol}"

    replicas_bash = " ".join(str(r) for r in replicas)

    one_run = dedent(r"""
        ONE_RUN_BODY='
        set -eu
        N=$1; M=$2

        ip -all netns delete 2>/dev/null || true
        for nsname in src-a src-b router-a router-b sink-a sink-b; do
          ip netns add "$nsname"
        done

        ip link add va0 type veth peer name va1
        ip link add vb0 type veth peer name vb1
        ip link add rab1 type veth peer name rab2
        ip link add vsa0 type veth peer name vsa1
        ip link add vsb0 type veth peer name vsb1

        ip link set va0 netns src-a;    ip link set va1 netns router-a
        ip link set vb0 netns src-b;    ip link set vb1 netns router-a
        ip link set rab1 netns router-a; ip link set rab2 netns router-b
        ip link set vsa0 netns router-b; ip link set vsa1 netns sink-a
        ip link set vsb0 netns router-b; ip link set vsb1 netns sink-b

        ip -n src-a    addr add 10.1.1.1/24 dev va0; ip -n src-a    link set va0 up
        ip -n router-a addr add 10.1.1.2/24 dev va1; ip -n router-a link set va1 up
        ip -n src-b    addr add 10.1.2.1/24 dev vb0; ip -n src-b    link set vb0 up
        ip -n router-a addr add 10.1.2.2/24 dev vb1; ip -n router-a link set vb1 up
        ip -n router-a addr add 10.0.0.1/24 dev rab1; ip -n router-a link set rab1 up
        ip -n router-b addr add 10.0.0.2/24 dev rab2; ip -n router-b link set rab2 up
        ip -n router-b addr add 10.3.1.2/24 dev vsa0; ip -n router-b link set vsa0 up
        ip -n sink-a   addr add 10.3.1.1/24 dev vsa1; ip -n sink-a   link set vsa1 up
        ip -n router-b addr add 10.3.2.2/24 dev vsb0; ip -n router-b link set vsb0 up
        ip -n sink-b   addr add 10.3.2.1/24 dev vsb1; ip -n sink-b   link set vsb1 up

        ip -n src-a route add default via 10.1.1.2
        ip -n src-b route add default via 10.1.2.2
        ip -n sink-a route add default via 10.3.1.2
        ip -n sink-b route add default via 10.3.2.2
        ip -n router-a route add 10.3.0.0/16 via 10.0.0.2
        ip -n router-b route add 10.1.0.0/16 via 10.0.0.1
        ip netns exec router-a sysctl -qw net.ipv4.ip_forward=1
        ip netns exec router-b sysctl -qw net.ipv4.ip_forward=1

        ip netns exec router-a tc qdisc add dev rab1 root handle 1: netem delay %DELAY_MS%ms
        ip netns exec router-a tc qdisc add dev rab1 parent 1: handle 2: cake bandwidth %BANDWIDTH%mbit triple-isolate

        PORT_A=9000; PORT_B=10000
        for ((i=0;i<N;i++)); do
          ip netns exec sink-a iperf3 -s -p $((PORT_A+i)) -1 -D 2>/dev/null || true
        done
        for ((j=0;j<M;j++)); do
          ip netns exec sink-b iperf3 -s -p $((PORT_B+j)) -1 -D 2>/dev/null || true
        done
        sleep 1

        TMP=$(mktemp -d)
        for ((i=0;i<N;i++)); do
          ip netns exec src-a iperf3 -c 10.3.1.1 -p $((PORT_A+i)) -t %DURATION% -J -A 0 %IPERF_EXTRA% \
            > "${TMP}/a-${i}.json" 2>/dev/null &
        done
        for ((j=0;j<M;j++)); do
          ip netns exec src-b iperf3 -c 10.3.2.1 -p $((PORT_B+j)) -t %DURATION% -J -A 0 %IPERF_EXTRA% \
            > "${TMP}/b-${j}.json" 2>/dev/null &
        done
        wait

        JQ_GP="%JQ_GP%"
        for ((i=0;i<N;i++)); do
          GP_BPS=$(jq -r "${JQ_GP}" "${TMP}/a-${i}.json" 2>/dev/null || echo 0)
          printf "A %d %.6f\n" "$i" "$(echo "${GP_BPS} / 1000000" | bc -l)"
        done
        for ((j=0;j<M;j++)); do
          GP_BPS=$(jq -r "${JQ_GP}" "${TMP}/b-${j}.json" 2>/dev/null || echo 0)
          printf "B %d %.6f\n" "$j" "$(echo "${GP_BPS} / 1000000" | bc -l)"
        done

        rm -rf "${TMP}"
        ip -all netns delete
        '
    """).strip()

    jq_gp = ".end.sum.bits_per_second // 0" if protocol == "udp" else ".end.streams[0].sender.bits_per_second // 0"
    one_run = (
        one_run.replace("%DELAY_MS%", str(delay_ms))
        .replace("%BANDWIDTH%", str(bandwidth))
        .replace("%DURATION%", str(duration_s))
        .replace("%IPERF_EXTRA%", iperf_extra)
        .replace("%JQ_GP%", jq_gp)
    )

    template = dedent(r"""
        #!/usr/bin/env bash
        # ============================================================
        # Stratum-bridge emitted scenario: %NAME%
        # Source IR:    %SOURCE%
        # IR digest:    %IR_DIGEST%
        # Emitted at:   %EMITTED_AT% (UTC)
        # Backend:      linux-netns via Lima VM %VM%
        #
        # This script is the scenario-emission output of the
        # stratum-bridge prototype. Running it in an environment with
        # the named Lima VM available reproduces the Linux band for
        # the cell encoded in the source IR. See
        # scripts/stratum-bridge/README.md for context.
        # ============================================================
        set -euo pipefail

        VM_NAME="%VM%"
        LIMACTL="${LIMACTL:-/opt/homebrew/bin/limactl}"
        N=%N_A%
        M=%N_B%
        PROTOCOL="%PROTOCOL%"
        REPLICAS=( %REPLICAS_BASH% )

        OUT_DIR="${OUT_DIR:-./stratum-bridge-output/%NAME%}"
        mkdir -p "${OUT_DIR}"
        PERFLOW_CSV="${OUT_DIR}/perflow.csv"
        SUMMARY="${OUT_DIR}/summary.txt"

        if ! ${LIMACTL} list --format json 2>/dev/null \
            | grep -q "\"name\":\"${VM_NAME}\""; then
          echo "FATAL: Lima VM '${VM_NAME}' not found." >&2
          echo "Run: ${LIMACTL} start --name=${VM_NAME} --tty=false template://ubuntu" >&2
          exit 1
        fi
        if ! ${LIMACTL} list --format json 2>/dev/null \
            | grep -q "\"status\":\"Running\""; then
          ${LIMACTL} start "${VM_NAME}" >/dev/null
        fi
        ${LIMACTL} shell "${VM_NAME}" sudo modprobe sch_cake >/dev/null 2>&1
        if [[ "${PROTOCOL}" == "bbr" ]]; then
          ${LIMACTL} shell "${VM_NAME}" sudo modprobe tcp_bbr >/dev/null 2>&1 || true
        fi

        %ONE_RUN%

        echo "implementation,strategy,mode,tcp_variant,N,M,bandwidth_mbps,duration_s,rng_run,host,flow_idx,goodput_mbps" \
          > "${PERFLOW_CSV}"

        for rng in "${REPLICAS[@]}"; do
          echo "[rng=${rng}] running N=${N} M=${M} protocol=${PROTOCOL} ..."
          ${LIMACTL} shell "${VM_NAME}" sudo bash -c "${ONE_RUN_BODY}" -- "$N" "$M" \
            > "${OUT_DIR}/run-rng${rng}.txt" 2>&1 || {
            echo "FATAL: run rng=${rng} failed; see ${OUT_DIR}/run-rng${rng}.txt" >&2
            exit 1
          }
          while read -r host flow_idx gp_mbps; do
            case "${host}" in A|B) ;; *) continue ;; esac
            printf "stratum-bridge,emit-netns,triple,%s,%d,%d,%d,%d,%d,%s,%s,%s\n" \
              "${PROTOCOL}" "$N" "$M" "%BANDWIDTH%" "%DURATION%" "$rng" \
              "$host" "$flow_idx" "$gp_mbps" \
              >> "${PERFLOW_CSV}"
          done < "${OUT_DIR}/run-rng${rng}.txt"
        done

        # Aggregate share_A = host-A goodput / (host-A + host-B) per replica,
        # then mean and pstdev across replicas.
        python3 - "${PERFLOW_CSV}" > "${SUMMARY}" <<'PY'
        import csv, statistics, sys
        from collections import defaultdict
        path = sys.argv[1]
        runs = defaultdict(lambda: {"A": 0.0, "B": 0.0})
        with open(path) as fh:
            r = csv.DictReader(fh)
            for row in r:
                runs[row["rng_run"]][row["host"]] += float(row["goodput_mbps"])
        shares = []
        for rng in sorted(runs, key=lambda k: int(k)):
            a, b = runs[rng]["A"], runs[rng]["B"]
            if a + b > 0:
                shares.append(a / (a + b))
        if not shares:
            print("share_A: no valid replicas"); sys.exit(0)
        m = statistics.mean(shares)
        s = statistics.pstdev(shares) if len(shares) > 1 else 0.0
        print(f"replicas: {len(shares)}")
        for i, x in enumerate(shares, 1):
            print(f"  share_A[{i}] = {x:.4f}")
        print(f"share_A_mean = {m:.4f}")
        print(f"share_A_pstd = {s:.4f}")
        PY

        echo
        echo "=== %NAME% ==="
        cat "${SUMMARY}"
        echo
        echo "Per-flow CSV: ${PERFLOW_CSV}"
    """).strip()

    return (
        template.replace("%NAME%", name)
        .replace("%SOURCE%", str(source_path))
        .replace("%IR_DIGEST%", ir_digest)
        .replace("%EMITTED_AT%", emitted_at)
        .replace("%VM%", vm_name)
        .replace("%N_A%", str(n_a))
        .replace("%N_B%", str(n_b))
        .replace("%PROTOCOL%", protocol)
        .replace("%REPLICAS_BASH%", replicas_bash)
        .replace("%BANDWIDTH%", str(bandwidth))
        .replace("%DURATION%", str(duration_s))
        .replace("%ONE_RUN%", one_run)
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Emit a Linux netns testbed bash script from a Stratum scenario IR.")
    parser.add_argument("scenario", type=Path, help="Path to scenario YAML (schema stratum-bridge/scenario/v1)")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Write to file instead of stdout")
    args = parser.parse_args()

    if not args.scenario.exists():
        _fail(f"scenario not found: {args.scenario}")
    with args.scenario.open() as fh:
        scenario = yaml.safe_load(fh)
    _validate(scenario, args.scenario)

    script = emit(scenario, args.scenario)
    if args.output:
        args.output.write_text(script + "\n")
        args.output.chmod(0o755)
    else:
        sys.stdout.write(script + "\n")


if __name__ == "__main__":
    main()
