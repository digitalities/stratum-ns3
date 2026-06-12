#!/usr/bin/env python3
"""Smoke test for the Stratum-bridge scenario emitter.

Runs the emitter against the bundled scenarios and checks that:
1. The emitted bash script is syntactically valid (`bash -n`).
2. Required parameter substitutions made it into the output.
3. The IR digest in the emitted header changes if and only if the IR text changes.

No Lima dependency. Intended for CI / pre-merge sanity.

Usage: python3 scripts/stratum-bridge/test_emit_netns.py
"""

from __future__ import annotations

import hashlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
EMITTER = HERE / "emit-netns.py"
SCENARIOS = HERE / "scenarios"


def check(condition: bool, msg: str) -> None:
    if not condition:
        print(f"FAIL: {msg}", file=sys.stderr)
        sys.exit(1)
    print(f"  pass: {msg}")


def emit(scenario_path: Path) -> str:
    result = subprocess.run(
        ["python3", str(EMITTER), str(scenario_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        print(f"FAIL: emitter exited {result.returncode} for {scenario_path}", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        sys.exit(1)
    return result.stdout


def main() -> None:
    scenarios = sorted(SCENARIOS.glob("*.yaml"))
    if not scenarios:
        print(f"FAIL: no scenarios under {SCENARIOS}", file=sys.stderr)
        sys.exit(1)

    digests_seen: dict[str, str] = {}
    for scenario in scenarios:
        print(f"\n== {scenario.name} ==")
        script = emit(scenario)

        # 1) bash -n syntax check
        result = subprocess.run(
            ["bash", "-n"],
            input=script,
            text=True,
            capture_output=True,
            check=False,
        )
        check(result.returncode == 0, f"bash -n on emitted script")

        # 2) Required substitutions
        check(scenario.name.replace(".yaml", "") in script, "name appears in header banner")
        check("ONE_RUN_BODY" in script, "ONE_RUN_BODY definition present")
        check("ip -all netns delete" in script, "netns cleanup present")
        check("cake bandwidth" in script, "cake qdisc setup present")
        check("triple-isolate" in script, "triple-isolate mode present")
        check("iperf3" in script, "iperf3 invocation present")
        check('JQ_GP="' in script, "JQ_GP uses double quotes (regression: prev single-quote-in-single-quote bug)")

        # 3) IR-digest stability: re-emit same IR -> same digest
        script_repeat = emit(scenario)
        # Strip the emitted-at timestamp line which differs between emissions
        def strip_timestamp(s: str) -> str:
            return "\n".join(line for line in s.splitlines() if not line.startswith("# Emitted at:"))
        check(strip_timestamp(script) == strip_timestamp(script_repeat), "re-emission is deterministic (modulo timestamp)")

        # 4) Different IRs -> different digests (collision check)
        digest_line = next((line for line in script.splitlines() if line.startswith("# IR digest:")), None)
        check(digest_line is not None, "IR digest line present in header")
        if digest_line:
            ir_digest = digest_line.split(":", 1)[1].strip()
            for other_scenario, other_digest in digests_seen.items():
                check(other_digest != ir_digest, f"IR digest differs from {other_scenario}")
            digests_seen[scenario.name] = ir_digest

    print("\nAll smoke tests passed.")


if __name__ == "__main__":
    main()
