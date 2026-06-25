#!/usr/bin/env bash
#
# scripts/lint-test-names.sh
#
# Release guard: fail if ns-3 rejects any test-case name in the built
# test runner.
#
# ns-3's test runner forbids the characters  " / \ | ?  in a test-case
# name (src/core/model/test.cc, TestCase::AddTestCase): the name is
# reused as a --test-name selector and as a temp-dir path fragment, so
# a '/' would be read as a path separator. A name that violates this is
# NOT dropped -- ns-3 prints "Invalid test name: ..." to stderr and runs
# the case anyway. The suite therefore still passes, a plain "0 failed"
# gate never notices, yet every test run -- including the suite step a
# fresh user runs to verify an install -- prints the warning, and the
# offending case cannot be selected individually by name.
#
# This guard asks ns-3's own runner which names it rejects, so it can
# never drift from the real rule and has no source-parsing blind spot
# (e.g. names split across adjacent string literals). Any rejection is a
# hard failure.
#
# Usage:
#   scripts/lint-test-names.sh [NS3_TREE]
#     NS3_TREE  a configured + built ns-3 tree to inspect
#               (default: current directory). A built test runner is
#               expected under <NS3_TREE>/build/utils/. The audit gates
#               build before calling this, so the runner is already
#               present there.
#
# Scope: --print-test-name-list constructs every registered suite, so the
# check covers the whole runner, not only stratum. In practice mainline
# ns-3 keeps its names valid, so a rejection is a stratum name; a mainline
# rejection after a pin advance is still worth surfacing (fix or report
# upstream).
#
# Exit codes:
#   0  no invalid test-case names
#   1  one or more invalid names (listed on stderr)
#   2  inconclusive -- no built test runner found (build the suite first)
#
set -uo pipefail

tree="${1:-.}"

# Discover the runner by glob so the ns-3 version in the name is not
# hard-coded (it advances with the pin).
runner=$(ls "$tree"/build/utils/ns3.*-test-runner-default 2>/dev/null | head -n1)

if [ -z "$runner" ]; then
    echo "lint-test-names: no built test runner under $tree/build/utils/." >&2
    echo "                 Build the suite first (./ns3 build), then re-run." >&2
    exit 2
fi

# Redirection order matters: 2>&1 points stderr at the pipe, then >/dev/null
# discards the valid-name list on stdout, leaving grep to read stderr only.
invalid=$("$runner" --print-test-name-list 2>&1 >/dev/null \
            | grep '^Invalid test name' || true)

if [ -n "$invalid" ]; then
    echo "lint-test-names: FAIL -- ns-3 rejects the following test-case name(s)." >&2
    echo "  forbidden characters:  \" / \\ | ?   (src/core/model/test.cc)" >&2
    echo "$invalid" | sed 's/^/  /' >&2
    exit 1
fi

echo "lint-test-names: OK -- no invalid test-case names ($runner)."
exit 0
