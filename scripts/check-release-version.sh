#!/usr/bin/env bash
#
# check-release-version.sh — release-version consistency gate.
#
# CITATION.cff's `version` field is the source of truth. The CHANGELOG's top
# release entry and the Bake module revision in bakeconf.xml must agree with it.
#
# Two modes (the validation logic is identical; only the three file paths differ):
#
#   (default)  validate a MIRROR checkout root — CITATION.cff / CHANGELOG.md /
#              bakeconf.xml. This is what the stratum mirror's Step 6b runs.
#   --stratum  validate the DEV monorepo's stratum stamps before mirroring —
#              CITATION-stratum.cff / CHANGELOG-stratum.md / src/ns-3/bakeconf.xml.
#              The mirror renames the first two to the default names, so this
#              catches a stale stamp without staging the whole mirror first.
#
#     scripts/check-release-version.sh [--stratum] [REPO_ROOT]
#
# Exits non-zero on any mismatch, naming each disagreement. Wired into the
# mirror as an abort-gate so a release cannot ship with a stale version stamp
# in any of these files (the failure mode that left bakeconf.xml pinning an old
# revision across two releases).
#
set -euo pipefail

stratum=0
ROOT="."
for arg in "$@"; do
    case "$arg" in
        --stratum) stratum=1 ;;
        -*) echo "check-release-version: unknown option '$arg'" >&2; exit 2 ;;
        *) ROOT="$arg" ;;
    esac
done

if [ "$stratum" -eq 1 ]; then
    cff="$ROOT/CITATION-stratum.cff"
    chg="$ROOT/CHANGELOG-stratum.md"
    bake="$ROOT/src/ns-3/bakeconf.xml"
else
    cff="$ROOT/CITATION.cff"
    chg="$ROOT/CHANGELOG.md"
    bake="$ROOT/bakeconf.xml"
fi

for f in "$cff" "$chg" "$bake"; do
    [ -f "$f" ] || { echo "check-release-version: missing $f" >&2; exit 1; }
done

# Source of truth: CITATION.cff  ->  version: "v1.2"
V=$(grep -E '^version:' "$cff" | head -1 | grep -oE 'v[0-9][0-9.]*')
[ -n "$V" ] || { echo "check-release-version: cannot read version from $cff" >&2; exit 1; }

fail=0

# CHANGELOG top release heading:  ## v1.2 — 2026-06-16
chg_v=$(grep -m1 -oE '^## v[0-9][0-9.]*' "$chg" | grep -oE 'v[0-9][0-9.]*' || true)
if [ "$chg_v" != "$V" ]; then
    echo "check-release-version: CHANGELOG top entry is '${chg_v:-none}', expected '$V'" >&2
    fail=1
fi

# Bake module revision (the v-prefixed one; the ns-3 module uses ns-3.NN)
bake_v=$(grep -oE 'name="revision" value="v[0-9][0-9.]*"' "$bake" | grep -oE 'v[0-9][0-9.]*' | head -1 || true)
if [ "$bake_v" != "$V" ]; then
    echo "check-release-version: bakeconf.xml revision is '${bake_v:-none}', expected '$V'" >&2
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "check-release-version: OK — CITATION / CHANGELOG / bakeconf all at $V"
fi
exit "$fail"
