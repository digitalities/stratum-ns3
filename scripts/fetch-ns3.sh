#!/bin/bash
# Clone ns-3 (pinned to the ns-3.48 release) as a sibling directory and
# wire up the contrib symlink so the Stratum module is visible to ns-3's
# build system.
#
# Layout:
#   <parent>/
#     stratum-ns3/      <- this repo (the module IS the repo root)
#     ns-3/             <- cloned here by this script (sibling, not nested;
#                          version-neutral name; the pinned revision is
#                          recorded once below as NS3_PIN)
#
# The symlink created inside the ns-3 tree is:
#   <ns-3 tree>/contrib/stratum -> <repo root>
#
# This avoids a directory cycle that would arise if the ns-3 tree were
# cloned inside the repo: contrib/stratum would point to the repo root,
# which would contain the ns-3 tree, which would contain contrib/stratum,
# and so on.
#
# Override the clone destination with the NS3_DIR environment variable
# (the script ships as scripts/fetch-ns3.sh in the stratum-ns3 repo):
#   NS3_DIR=/path/to/my/ns-3 ./scripts/fetch-ns3.sh
#   ./scripts/fetch-ns3.sh --print-pin           # emit the pinned revision
#
# --source-only: defines functions but skips all side-effects. Used by
# test helpers to unit-test patch logic without cloning ns-3-dev.

set -euo pipefail

# REPO_ROOT resolution: env var override wins; otherwise resolves to the
# parent of this script's directory (i.e. the repo root).
if [ -z "${REPO_ROOT:-}" ]; then
    REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fi
cd "$REPO_ROOT"

# Arg parsing: --source-only only.
SOURCE_ONLY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --source-only)
            SOURCE_ONLY=1
            shift
            ;;
        --print-pin)
            PRINT_PIN=1
            shift
            ;;
        --worktree*)
            echo "ERROR: --worktree is not available in this distribution" >&2
            exit 1
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            echo "Usage: $0 [--source-only]" >&2
            exit 1
            ;;
    esac
done

# Default clone destination is a sibling of the repo root with a
# version-neutral name (ns-3/), so docs and muscle memory survive pin
# advances. A version-named sibling from an earlier layout (e.g.
# ns-3.48/) is reused if present and no generic sibling exists.
# Resolve to an absolute path so user-facing messages are unambiguous.
if [ -z "${NS3_DIR:-}" ] && [ ! -d "$REPO_ROOT/../ns-3" ]; then
    for _legacy in "$REPO_ROOT"/../ns-3.[0-9]*; do
        if [ -d "$_legacy" ]; then
            NS3_DIR="$_legacy"
            echo "Reusing legacy version-named sibling: $_legacy" >&2
            break
        fi
    done
fi
NS3_DIR="${NS3_DIR:-$REPO_ROOT/../ns-3}"
if _d="$(cd "$(dirname "$NS3_DIR")" 2>/dev/null && pwd)"; then
    NS3_DIR="$_d/$(basename "$NS3_DIR")"
else
    NS3_DIR="$(dirname "$REPO_ROOT")/ns-3"
fi
TARGET="$NS3_DIR"

# Pinned ns-3-dev revision: the Stratum module is built and tested
# against this specific commit.
NS3_PIN="d2add90b452d600cfb4859baed8e9ea633519447"   # = ns-3.48 release tag (2026-06-02)

# --print-pin: emit the pinned revision and exit. This is the ONLY
# authoritative copy of the pin in this repo — docs and scripts query it
# instead of repeating the version, so a pin advance edits exactly one line.
if [ "${PRINT_PIN:-0}" = 1 ]; then
    echo "$NS3_PIN"
    exit 0
fi

if [ "$SOURCE_ONLY" -eq 0 ]; then
    # Primary mode: clone if absent.
    if [ -d "$TARGET" ]; then
        echo "ns-3-dev already present at $TARGET"
    else
        echo "Cloning ns-3-dev from GitLab..."
        mkdir -p "$(dirname "$TARGET")"
        git clone https://gitlab.com/nsnam/ns-3-dev.git "$TARGET"
        (cd "$TARGET" && git checkout "$NS3_PIN")
    fi
fi

# Apply local patches carried in patches/ns3/. These address upstream
# ns-3 defects that block our scenarios. Each patch ships as a
# git-format-patch file; the upstream contribution artifacts live under
# docs/upstream/.
#
# apply_patch_robust handles the failure modes seen in practice:
#   1. Plain `git apply` succeeds — pinned upstream context matches.
#   2. Context drift (upstream rebase moved nearby lines) — fall back
#      to `git apply --3way`, which uses blob ancestry to merge.
#   3. Pre-existing untracked files from a prior partial apply (the
#      patch creates new files that already sit on disk untracked).
#      Detect "already exists in working directory" in the apply
#      output, remove the offending untracked files, and retry.
# Post-apply, run `git apply --check --reverse` to verify the patch
# is now logically present (would-be reverse-apply succeeds). This
# catches partial applies that previously slipped through silently.
apply_patch_robust() {
    local patch="$1"
    local name="$(basename "$patch")"

    # Idempotency probe — reverse-apply check succeeds when the patch
    # is fully applied. (May false-negative for new-file patches, in
    # which case we fall through to the apply attempt below.)
    if (cd "$TARGET" && git apply --check --reverse "$patch") 2>/dev/null; then
        echo "Patch $name already applied, skipping."
        return 0
    fi

    echo "Applying patch $name ..."

    # Capture the set of pre-existing tracked files this patch touches.
    # When a tier fails midway and leaves dirty state behind (notably
    # `--3way` writing conflict markers), we restore these files to HEAD
    # so the next tier or next patch starts from a clean baseline.
    local touched_tracked
    touched_tracked=$(grep -E '^diff --git a/' "$patch" | awk '{print $3}' | sed 's|^a/||' \
        | while read f; do
            (cd "$TARGET" && git ls-files --error-unmatch "$f" >/dev/null 2>&1) && echo "$f"
          done)

    _restore_touched() {
        for f in $touched_tracked; do
            (cd "$TARGET" && git checkout HEAD -- "$f") 2>/dev/null
        done
    }

    # Tier 1: plain apply. Capture stdout+stderr; only display on final
    # failure to avoid misleading per-file "Applied patch to X cleanly"
    # output when the patch ultimately rolls back via --3way.
    local err rc
    err=$(cd "$TARGET" && git apply "$patch" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  applied (plain)"
        _verify_patch_applied "$patch" "$name"
        return $?
    fi

    # Tier 2: 3-way merge — handles context drift when blob ancestry
    # is recoverable (true for patches generated against a recent
    # upstream commit even after subsequent rebases). On failure,
    # 3-way may leave conflict markers in working files; we scan for
    # those and restore so the next tier starts clean. Capture output
    # and suppress on success — only display if every tier ultimately
    # fails, so operators get one coherent error report.
    local tier2_out tier2_rc
    tier2_out=$(cd "$TARGET" && git apply --3way "$patch" 2>&1)
    tier2_rc=$?
    if [ $tier2_rc -eq 0 ]; then
        echo "  applied via 3-way merge"
        _verify_patch_applied "$patch" "$name"
        return $?
    fi
    if _files_have_conflict_markers $touched_tracked; then
        echo "  3-way merge left conflict markers, restoring touched files"
        _restore_touched
    fi

    # Tier 3: detect "already exists in working directory" from prior
    # partial apply, clean up the untracked leftovers, retry --3way.
    if echo "$err" | grep -q "already exists in working directory"; then
        echo "  pre-existing untracked files from prior partial apply detected"
        local stale_files
        stale_files=$(echo "$err" \
            | grep "already exists in working directory" \
            | awk '{print $2}' | sed 's/://')
        for f in $stale_files; do
            if [ -f "$TARGET/$f" ] \
               && ! (cd "$TARGET" && git ls-files --error-unmatch "$f" >/dev/null 2>&1); then
                echo "  rm $f (untracked, will be recreated by patch)"
                rm "$TARGET/$f"
            fi
        done
        if (cd "$TARGET" && git apply --3way "$patch") 2>/dev/null; then
            echo "  applied via 3-way merge after untracked-cleanup"
            _verify_patch_applied "$patch" "$name"
            return $?
        fi
        if _files_have_conflict_markers $touched_tracked; then
            echo "  3-way merge left conflict markers after cleanup, restoring touched files"
            _restore_touched
        fi
    fi

    # All tiers exhausted. Surface BOTH Tier 1 (plain) and Tier 2 (--3way)
    # outputs so operators have full diagnostic context — Tier 2's failure
    # detail is often the most informative (conflict context, ancestor
    # mismatch reason) and was captured-but-suppressed during the success
    # path.
    echo "ERROR: Patch $name could not be applied." >&2
    echo "Tier 1 (plain) output:" >&2
    echo "$err" | sed 's/^/  /' >&2
    if [ -n "${tier2_out:-}" ]; then
        echo "Tier 2 (--3way) output:" >&2
        echo "$tier2_out" | sed 's/^/  /' >&2
    fi
    return 1
}

_files_have_conflict_markers() {
    for f in "$@"; do
        if [ -f "$TARGET/$f" ] && grep -qE '^(<<<<<<<|>>>>>>>)' "$TARGET/$f"; then
            return 0
        fi
    done
    return 1
}

# Verify a patch is logically present by checking that a reverse-apply
# would now succeed. Loud failure here means the apply only partially
# took effect (e.g. some hunks landed but new files were skipped).
_verify_patch_applied() {
    local patch="$1"
    local name="$2"
    if (cd "$TARGET" && git apply --check --reverse "$patch") 2>/dev/null; then
        echo "  verified applied"
        return 0
    fi
    echo "ERROR: Patch $name applied but post-verify failed (partial apply?)" >&2
    return 1
}

# Preflight: validate the patch series cumulatively before the real apply
# loop. Patches may anchor on context introduced by earlier patches in
# the series (e.g. a later patch adds a sibling field below one declared
# by an earlier patch), so each patch must be checked against the state
# left behind by its predecessors, not against the unpatched baseline.
#
# Strategy: actually apply each patch sequentially. On any failure, stop
# and report. At the end, roll the working tree back to its pre-preflight
# state — both tracked file modifications (`git reset --hard`) and any
# untracked new files added by patches (diff pre/post untracked listings
# and remove the new ones). The apply phase that follows starts from a
# clean slate.
#
# Returns 0 if every patch would apply cleanly in order, nonzero otherwise.
_preflight_patches() {
    local dir="$1"
    local fail=0
    local pass=0
    local saved_head
    saved_head=$(cd "$TARGET" && git rev-parse HEAD)
    local pre_untracked
    pre_untracked=$(cd "$TARGET" && git ls-files --others --exclude-standard | sort)

    for patch in "$dir"/*.patch; do
        [ -e "$patch" ] || continue
        local name="$(basename "$patch")"
        # Idempotency: if a patch is already applied in the current state,
        # report and move on without re-applying.
        if (cd "$TARGET" && git apply --check --reverse "$patch") 2>/dev/null; then
            pass=$((pass + 1))
            echo "PREFLIGHT OK:   $name (already applied)"
            continue
        fi
        if (cd "$TARGET" && git apply "$patch") 2>/dev/null; then
            pass=$((pass + 1))
            echo "PREFLIGHT OK:   $name"
        elif (cd "$TARGET" && git apply --3way "$patch") 2>/dev/null; then
            pass=$((pass + 1))
            echo "PREFLIGHT OK:   $name (via 3-way)"
        else
            fail=$((fail + 1))
            echo "PREFLIGHT FAIL: $name (neither plain nor --3way passes apply)"
            # Bail early — subsequent patches likely depend on this one
            # and would produce cascading misleading failure reports.
            break
        fi
    done

    # Roll back tracked modifications and any new untracked files added
    # during preflight, leaving the working tree in its pre-preflight
    # state for the real apply loop.
    (cd "$TARGET" && git reset --hard "$saved_head" >/dev/null 2>&1)
    local post_untracked
    post_untracked=$(cd "$TARGET" && git ls-files --others --exclude-standard | sort)
    comm -13 <(echo "$pre_untracked") <(echo "$post_untracked") | while read -r f; do
        [ -n "$f" ] && rm -f "$TARGET/$f"
    done

    echo "PREFLIGHT SUMMARY: $pass OK, $fail FAIL"
    [ "$fail" -eq 0 ]
}

PATCH_DIR="$REPO_ROOT/patches/ns3"
if [ "$SOURCE_ONLY" -eq 0 ] && [ -d "$PATCH_DIR" ]; then
    # Start from a pristine pinned baseline so patch (re)application is
    # idempotent. Re-running from a clean baseline sidesteps state left
    # by prior partial applies: every patch applies via straight `git apply`.
    # Reset tracked files to the pin and drop untracked patch-created files
    # under src/; build/ and cmake-cache/ are kept.
    echo "Resetting $TARGET to the pinned baseline before applying patches..."
    (cd "$TARGET" && git reset --hard "$NS3_PIN" >/dev/null 2>&1 && git clean -fdq src/) \
        || { echo "ERROR: could not reset $TARGET to $NS3_PIN" >&2; exit 1; }
    # Preflight: structured per-patch --check report BEFORE any disk-touching
    # apply. Catches cross-patch drift before the apply loop emits misleading
    # per-file probing output.
    if ! _preflight_patches "$PATCH_DIR"; then
        echo "" >&2
        echo "FATAL: preflight failed for one or more patches." >&2
        echo "Investigate cross-patch drift before retrying. Common causes:" >&2
        echo "  - Upstream rebase moved context lines beyond --3way recovery" >&2
        echo "  - Earlier patch in the loop shifted lines that a later patch targets" >&2
        echo "  - Untracked leftovers from a prior partial apply" >&2
        exit 1
    fi
    PATCH_FAILURES=0
    for patch in "$PATCH_DIR"/*.patch; do
        [ -e "$patch" ] || continue
        if ! apply_patch_robust "$patch"; then
            PATCH_FAILURES=$((PATCH_FAILURES + 1))
        fi
    done
    if [ "$PATCH_FAILURES" -gt 0 ]; then
        echo "" >&2
        echo "FATAL: $PATCH_FAILURES patch(es) failed to apply despite preflight pass." >&2
        echo "This indicates an apply-tier bug; capture full output and investigate." >&2
        exit 1
    fi
fi

if [ "$SOURCE_ONLY" -eq 0 ]; then
    # Create symlink so ns-3 can find the Stratum module as a contrib entry.
    # The repo root IS the module, so the symlink points directly to it.
    SYMLINK="$TARGET/contrib/stratum"
    # Retire legacy symlinks if a previous run created them; idempotent.
    for LEGACY_NAME in diffserv4ns3 diffserv; do
        LEGACY_SYMLINK="$TARGET/contrib/$LEGACY_NAME"
        if [ -L "$LEGACY_SYMLINK" ]; then
            echo "Removing legacy symlink: contrib/$LEGACY_NAME"
            rm "$LEGACY_SYMLINK"
        fi
    done
    SYM_TARGET="$REPO_ROOT"
    # Re-point if an existing symlink targets a different path. Idempotent
    # across path corrections and NS3_DIR override changes.
    if [ -L "$SYMLINK" ]; then
        CURRENT_TARGET="$(readlink "$SYMLINK")"
        if [ "$CURRENT_TARGET" != "$SYM_TARGET" ]; then
            echo "Re-pointing symlink: $CURRENT_TARGET -> $SYM_TARGET"
            rm "$SYMLINK"
        fi
    fi
    if [ ! -e "$SYMLINK" ]; then
        ln -s "$SYM_TARGET" "$SYMLINK"
        echo "Created symlink: contrib/stratum -> $SYM_TARGET"
    fi

    echo "Done. ns-3-dev source is at $TARGET/ (pinned at $NS3_PIN)."
    echo "The checkout carries local patches from patches/ns3/."
    echo "The stratum module is symlinked into contrib/ for building."
fi
