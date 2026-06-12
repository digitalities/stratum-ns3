#!/bin/bash
#
# Regenerate substrate-registry tables in the handbook from manifests.
#
# Idempotent: each call to regen-registry-tables.py exits 0 with no
# write when the section is already current. Invoke from anywhere; the
# script resolves repo-relative paths from its own location.
#
# Add a new auto-generated table by:
#   1. Emitting / committing the manifest snapshot under scripts/.
#   2. Adding a marker pair to the target handbook chapter:
#        <!-- BEGIN registry-table: <id> -->
#        <!-- END registry-table: <id> -->
#   3. Appending a regen call below.
#
# The script does NOT regenerate the manifests themselves — that is a
# separate workflow gated on building and running aqm-eval-runner.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REGEN="${SCRIPT_DIR}/regen-registry-tables.py"

run_regen() {
    python3 "${REGEN}" "$@"
}

HANDBOOK_DIR="${REPO_ROOT}/handbook"

# --- Scheduler catalogue ---
WIRELESS_EXT_CHAPTER="${HANDBOOK_DIR}/III-05-wireless.md"
run_regen \
    --manifest "${REPO_ROOT}/scripts/aqm-eval/scheduler-manifest.json" \
    --array-key schedulers \
    --target "${WIRELESS_EXT_CHAPTER}" \
    --marker scheduler-catalogue \
    --columns fileTag,displayName,family,parameterShape,description \
    --headers "Tag,Name,Family,Parameter shape,Description"
