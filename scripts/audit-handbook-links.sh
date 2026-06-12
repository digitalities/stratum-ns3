#!/usr/bin/env bash
# audit-handbook-links.sh — single-tree link audit for the merged handbook.
#   1. Every relative markdown link/image target resolves to an existing file.
#   2. Legacy reference tokens (guide/, "handbook §N", old chapter filenames)
#      are forbidden.
# Usage: audit-handbook-links.sh [HANDBOOK_DIR]   (default: handbook)
# Exit codes: 0 = clean, 1 = problems found (count printed), also 1 if HANDBOOK_DIR missing.
set -euo pipefail
readonly HB="${1:-handbook}"
[ -d "$HB" ] || { echo "ERROR: $HB not found" >&2; exit 1; }
fail=0

# -- Check 1: relative link/image targets resolve ----------------------------
# Fenced code blocks are excluded: lambda captures like [](DataRate r) would
# false-positive as markdown links. Lines inside fences are blanked (not
# removed) so reported line numbers stay true.
#
# Layout note: the handbook ships with the module at the repo root, so links
# like ../test/x.h are correct in the PUBLIC layout. In the dev monorepo those
# siblings live under src/ns-3/ (or at the repo root for top-level files), so
# a dev-fallback resolution is tried before flagging. The staged-tree run
# (mirror gate) is authoritative: there the literal path must resolve.
while IFS=: read -r file line target; do
  tgt="${target%%#*}"                      # strip anchor
  [ -z "$tgt" ] && continue
  case "$tgt" in
    http://*|https://*|mailto:*) continue ;;
  esac
  if [ -e "$HB/$tgt" ] || [ -e "$(dirname "$file")/$tgt" ]; then
    continue
  fi
  # dev-monorepo fallback for public-layout ../ siblings
  case "$tgt" in
    ../*)
      rest="${tgt#../}"
      if [ -e "src/ns-3/$rest" ] || [ -e "$rest" ]; then
        continue
      fi
      ;;
  esac
  echo "BROKEN LINK: $file:$line -> $target"
  fail=$((fail+1))
done < <(
  find "$HB" -name '*.md' | while IFS= read -r f; do
    # `|| true` keeps zero-link files from aborting the subshell under set -e
    # (which silently truncated coverage at the first link-free file).
    awk '/^```/{fence=!fence; print ""; next} fence{print ""; next} {print}' "$f" \
      | grep -noE '\]\([^)]+\)' \
      | sed -E "s|^([0-9]+):\]\(([^)]+)\)\$|${f}:\1:\2|" || true
  done
)

# -- Check 2: legacy cross-tree tokens forbidden ------------------------------
while IFS= read -r hit; do
  echo "LEGACY REF: $hit"
  fail=$((fail+1))
done < <(grep -RnE 'guide/|handbook §|handbook chapter [0-9]' "$HB" --include='*.md' || true)

# -- Check 3: old (pre-merge) chapter filenames must not be referenced --------
# Guard (^|[^-]) prevents false positives: new names embed some old basenames
# (e.g. II-03-traffic-management.md contains 01-traffic-management.md, always
# preceded by '-').
for old in 01-traffic-management 02-diffserv-model 03-architecture \
           04-ns229-module 05-ns235-port 06-ns3-port 07-scenarios \
           08-results-three-way 09-conclusions 10-l4s-extension \
           11-cake-implementation 11A-cake-flent-figure-pack \
           12-wireless-extension 13-aqm-eval-suite 00-index \
           validation-longform aqm-eval-interop appendix-B-provenance; do
  while IFS= read -r hit; do
    echo "STALE FILENAME REF: $hit"
    fail=$((fail+1))
  done < <(grep -RnE "(^|[^-])${old}\.md" "$HB" --include='*.md' || true)
done

if [ "$fail" -gt 0 ]; then
  echo "FAIL: $fail problem(s)"
  exit 1
fi
echo "PASS: handbook links clean"
