#!/usr/bin/env bash
# scripts/regen-test-manifest.sh
#
# Captures the ns-3 stratum test surface to src/ns-3/test/test-manifest.txt.
# Run on every release tag (or whenever a test is added or renamed).
#
# Output format: per-suite list of test classes registered via
# AddTestCase(new XxxTest()). Loop-unrolled registrations show up as
# a single entry — the runtime case count is therefore an upper bound
# on the lines per suite. Diffing the manifest at release-tag time
# detects: suite added/removed, test class added/removed, test class
# renamed.
#
# Source-extracted (no build required) so the manifest stays
# regeneratable from a fresh clone.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Layout detection: the monorepo keeps the module at src/ns-3/; the
# public module repo has it at the repo root.
if [ -d "src/ns-3/test" ]; then
    readonly TEST_DIR="src/ns-3/test"
else
    readonly TEST_DIR="test"
fi
readonly OUT="$TEST_DIR/test-manifest.txt"

# Suites are DISCOVERED from the sources, not listed by hand: every
# .cc under $TEST_DIR that constructs a TestSuite("name", ...) is a
# suite source. A hand-kept list silently under-reports the surface
# as suites are added.
discover_suites() {
  local f names
  for f in "$TEST_DIR"/*.cc; do
    # `|| true` is load-bearing: under pipefail, grep's exit 1 on a
    # suite-less file (e.g. an #include'd test-case companion) would
    # otherwise fail the assignment and set -e would end discovery
    # mid-loop, silently truncating the manifest.
    names="$({ grep -hoE 'TestSuite\("[a-z0-9-]+"' "$f" 2>/dev/null || true; } \
               | sed -E 's/TestSuite\("([a-z0-9-]+)"/\1/' | sort -u \
               | paste -sd ',' - | sed 's/,/, /g')"
    if [ -n "$names" ]; then
      printf '%s|%s\n' "$names" "$(basename "$f")"
    fi
  done
}

date_iso="$(date -u +%Y-%m-%d)"
# The only authoritative pin copy is NS3_PIN in scripts/fetch-ns3.sh.
ns3_pin="unknown"
if [ -x "scripts/fetch-ns3.sh" ]; then
  ns3_pin="$(scripts/fetch-ns3.sh --print-pin 2>/dev/null || echo unknown)"
fi
if [ "$ns3_pin" = "unknown" ] && [ -d "ns3/ns-3-dev/.git" ]; then
  ns3_pin="$(cd ns3/ns-3-dev && git rev-parse --short=10 HEAD 2>/dev/null || echo unknown)"
fi

{
  printf '# stratum ns-3 test manifest\n'
  printf '#\n'
  printf '# Snapshot of the test surface registered via AddTestCase() across\n'
  printf '# %s. A diff against this file at release-tag time flags any\n' "$TEST_DIR"
  printf '# suite-rename, test-class rename, or test-class add/remove that\n'
  printf '# changed the surface.\n'
  printf '#\n'
  printf '# Regenerate via: bash scripts/regen-test-manifest.sh\n'
  printf '#\n'
  printf '# Loop-unrolled registrations (e.g. AddTestCase(new XxxTestCase(i))\n'
  printf '# inside a for-loop) appear as a single line; the actual runtime\n'
  printf '# case count is therefore >= the lines listed per suite. To audit\n'
  printf '# runtime case counts, run\n'
  printf '#   "$(ls -t ./build/utils/ns3*-test-runner-default | head -1)" --suite=<suite> --verbose --fullness=EXTENSIVE\n'
  printf '# from inside ns3/ns-3-dev/.\n'
  printf '#\n'
  printf '# Suites are discovered from TestSuite("name", ...) constructor\n'
  printf '# calls in the sources. A discovered suite may be absent from the\n'
  printf '# built test runner when its source is standalone by design (e.g.\n'
  printf '# the RFC vector runner, driven by its own harness rather than\n'
  printf '# compiled into the module test library).\n'
  printf '#\n'
  printf '# Generated: %s\n' "$date_iso"
  printf '# ns-3-dev pin: %s\n' "$ns3_pin"
  printf '\n'
} > "$OUT"

total_invocations=0
total_suites=0

while IFS= read -r entry; do
  suite="${entry%%|*}"
  file="${entry#*|}"
  full_path="$TEST_DIR/$file"
  total_suites=$((total_suites + 1))

  printf '## %s (%s)\n\n' "$suite" "$file" >> "$OUT"

  # AddTestCase(new XxxTestCase()  — extract the class name token.
  # Sed is preferred over awk for portable POSIX BRE matching.
  classes=$(
    grep -E 'AddTestCase\(new [A-Za-z][A-Za-z0-9_]*' "$full_path" \
      | sed -E 's|.*AddTestCase\(new ([A-Za-z][A-Za-z0-9_]*).*|\1|'
  )

  if [ -z "$classes" ]; then
    printf '(no AddTestCase invocations found)\n\n' >> "$OUT"
    continue
  fi

  printf '%s\n\n' "$classes" >> "$OUT"

  count=$(printf '%s\n' "$classes" | wc -l | tr -d ' ')
  total_invocations=$((total_invocations + count))
done < <(discover_suites)

{
  printf '## Summary\n\n'
  printf 'Suite sources:           %d\n' "$total_suites"
  printf 'AddTestCase invocations: %d\n' "$total_invocations"
  printf '(loop-unrolled registrations counted once)\n'
} >> "$OUT"

printf 'wrote %s — %d AddTestCase invocations across %d suite sources\n' \
       "$OUT" "$total_invocations" "$total_suites"
