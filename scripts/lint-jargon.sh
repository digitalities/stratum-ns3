#!/usr/bin/env bash
# scripts/lint-jargon.sh
#
# Quality-check linter for internal-jargon tokens in any source
# file, Doxygen block, or script that ships in the public release
# repository. The release is read by external audiences who do
# not have project-internal context (artefact-evaluation reviewers,
# ns-3 community members, future reusers, students), so internal
# tokens read as opaque jargon and dilute the technical content.
#
# The rule is documented in `docs/ns3-doxygen-style.md` section 10.1
# and section 11 of the same guide.
#
# Exit code:
#   0 — no jargon detected
#   1 — one or more tokens found; offending file:line listed
#
# Scope (SCAN_PATHS_CODE — .cc/.h/.py/.sh only):
#   src/ns-3/, src/ns-2.35/, scripts/, handbook/
# Scope (SCAN_PATHS_MD — .md only):
#   specs/, handbook/
# Scope (SCAN_PATHS_CONFIG — .yml/.yaml/.md):
#   .github/  (issue + PR templates, label definitions)
# Scope (SCAN_PATHS_SCRIPTS_MD — .md, bare ADR-NNNN citations sanctioned):
#   scripts/  (nested READMEs that ship alongside the scripts)
#
# Structural rules (after the token patterns; see the bottom of the script):
#   stale-chapter-ref          — part-numbered handbook chapter references
#                                must name a file that exists under handbook/
#   adr-public-surface-format  — ADR metadata line must be spelled
#                                `**Public surface:**` (dev tree only)
#
# Note: the `postponed-dir-ref` pattern flags Markdown cross-references to
# `handbook/` and `guide/`, which are deferred from the v1.0 release. Remove
# `guide/` (and, when the handbook ships, `handbook/`) from that pattern once
# those trees are published.
# Out of scope:
#   src/ns-2.29/                  (frozen 2001 original; read-only)
#   docs/adr/                     (decision records, private to the project)
#   docs/superpowers/, paper/     (dev-only; do not ship in release)
#   src/ns-3/CHANGELOG.md         (project-history artefact whose value
#                                  depends on referencing phasing)
#   scripts/lint-ns3-idioms.sh    (author-tooling: encodes rules sourced
#                                  from author-private memory; the
#                                  references to that source layer are
#                                  intentional self-documentation)
#
# Allowlisted token patterns (stripped from the candidate line before the
# banned regex is re-tested — an allowlisted token therefore never masks a
# banned token sharing its line):
#   I-N, S-N.M, Q-N.M             — public spec identifiers indexed in
#                                   specs/01-intent.md, specs/02-structural.md,
#                                   and specs/03-quality.md
#   F-A..F-D                      — public empirical-findings catalogue
#   N2-N, D2-N, N3-N              — public bug-area-prefix identifiers
#                                   indexed in docs/HISTORICAL_BUGS.md
#                                   (note: D3-N is NOT allowlisted — that
#                                   bucket is the private DS4-for-ns-3
#                                   evolutionary log)
#   DS4-PN                        — public NS2_PATCHES patch tokens

set -euo pipefail

readonly REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# handbook/ ships in the public stratum mirror (merged Part I/II/III book,
# 2026-06-06), so it is release-bound and in lint scope. guide/ no longer
# exists (merged into handbook/ as Part I); any guide/ reference is stale
# and stays forbidden via the postponed-dir-ref pattern.
# Layout-aware: the dev monorepo keeps the module at src/ns-3/; the
# public stratum repo has it at the repo root (model/, helper/, ...).
if [ -d "src/ns-3" ]; then
  readonly SCAN_PATHS_CODE=(
    "src/ns-3"
    "src/ns-2.35"
    "scripts"
    "patches/ns3"
  )
else
  readonly SCAN_PATHS_CODE=(
    "model"
    "helper"
    "test"
    "examples"
    "doc"
    "scripts"
    "patches/ns3"
  )
fi

readonly SCAN_PATHS_MD=(
  "specs"
  "handbook"
)

# provenance/ ships wholesale in the public release (frozen reference excerpts
# plus the LINEAGE docs), so it is release-bound and must be jargon-clean. It is
# scanned like SCAN_PATHS_MD but with one carve-out applied in the loop below:
# the decision-record-number pattern is skipped, because bare ADR-NNNN is the
# sanctioned citation form in shipped docs and the LINEAGE files cite ADRs by id.
readonly SCAN_PATHS_PROVENANCE=(
  "provenance"
)

# Nested scripts/ markdown (e.g. scripts/stratum-bridge/README.md) ships in
# the public release but used to match neither scan group: scripts/ is CODE
# scope (whose globs stop at .cc/.h/.py/.sh/.patch) and the MD group did not
# list scripts/. A dev-only findings path shipped through that gap on
# 2026-06-07. Scanned like provenance/ — the decision-record-number pattern
# is skipped because bare ADR-NNNN is the sanctioned citation form in
# shipped script documentation.
readonly SCAN_PATHS_SCRIPTS_MD=(
  "scripts"
)

# Individual top-level markdown files that ship in the release and form the
# reader's front page. Scanned the same way as SCAN_PATHS_MD but as named
# files (they live at the repo root, not under a scanned directory).
# NOTE: LINEAGE.md / CONTRIBUTING.md are intentionally excluded — they carry
# public catalogue identifiers (e.g. the historical BUG-N list) and a
# reference to the dev style guide that are a separate policy question.
readonly SCAN_FILES_MD=(
  "README.md"
  "README-ns-3.md"
)

readonly FILE_GLOBS_CODE=(
  "--include=*.cc"
  "--include=*.h"
  "--include=*.py"
  "--include=*.sh"
  "--include=*.patch"
  "--exclude=lint-jargon.sh"
  "--exclude=lint-ns3-idioms.sh"
  "--exclude=mirror-ns3-to-release.sh"
  "--exclude=mirror-ns3-to-stratum.sh"
  "--exclude=bug11-shim-regression.sh"
  "--exclude=q6-go-no-go.sh"
  "--exclude-dir=.venv"
  "--exclude-dir=deferred"
  "--exclude-dir=__pycache__"
)

readonly FILE_GLOBS_MD=(
  "--include=*.md"
  "--exclude-dir=.venv"
)

# Repository config that ships in the release: GitHub issue/PR templates and
# label definitions. These are read by external contributors but live outside
# the source/doc trees above, so they are scanned as a separate group covering
# both YAML templates and Markdown.
readonly SCAN_PATHS_CONFIG=(
  ".github"
)

readonly FILE_GLOBS_CONFIG=(
  "--include=*.yml"
  "--include=*.yaml"
  "--include=*.md"
)

# Patterns to detect. Each entry is "label|regex".
# Regex uses POSIX extended; word-boundary tokens use \b.
#
# Note on "shim": this word is intentionally absent from the design-journey
# bucket below. It is an established CS term (in the same category as
# proxy/adapter/wrapper pattern names) usable as a component-type token
# in class names. The design-journey phrases describe a development
# trajectory; "shim" describes a thing, so it does not belong in that
# bucket.
readonly PATTERNS=(
  "decision-record-number|ADR-[0-9]{4}"
  "private-repo-url|digitalities/diffserv4ns-dev"
  "phase-label|Phase [0-9]+|phase[0-9]+\b"
  "pr-label|\\bPR[0-9]+[a-z]?\\b"
  "bug-catalogue|\\bBUG-[0-9]+\\b"
  "deprecated-bug-id|\\b[DN][23]-[0-9]+\\b"
  "spec-id-quality|\\bQ-[0-9]+\\.[0-9]+\\b"
  "spec-id-structural|\\bS-[0-9]+\\.[0-9]+\\b"
  "finding-id|\\bF-[A-D]\\b|Finding F-[A-Z]"
  "postponed-dir-ref|guide/[a-z0-9-]+\\.md|handbook chapter [0-9]"
  "internal-plan-path|docs/(superpowers|methodology|prompts|audit|cpp-review-reports)/"
  "internal-adr-path|docs/adr/[0-9]"
  "internal-style-doc-ref|docs/ns3-doxygen-style\\.md"
  "serena-memory-path|reference_ns3_[a-z0-9_]+\\.md|feedback_[a-z0-9_]+\\.md|project_[a-z0-9_]+\\.md"
  "author-private-system|\\bauto-memory\\b|Serena memor"
  "design-journey-phrase|post-refactor|pre-PR[0-9]|composition over inheritance|asymmetric by design|strategy pattern\\b|supersedes|lazy-create"
  "ds4-shorthand|\\bDS4\\b"
)

# Tokens that look like jargon under the regex above but actually
# index public artefacts (specs, findings catalogue, bug-area
# prefixes, patch IDs). Every allowlist match is STRIPPED from the
# candidate line and the banned regex is re-tested against the
# residue: a candidate whose only banned-looking content was the
# allowlisted token itself is skipped, while a banned token sharing
# a line with an allowlisted one still reports (the same-line gap
# that previously let such lines through). Context entries (e.g. the
# Ferrari title) consume the words that fed the banned match, so
# they keep their allowlisting effect under stripping.
readonly ALLOWLIST_PATTERNS=(
  '\b[ISQ]-[0-9]+(\.[0-9]+)*\b'   # spec identifiers (I-1, S-2.1, Q-15.6)
  '\bF-[A-D][0-9]?\b'             # empirical-findings catalogue (F-A, F-C)
  '\bA3-StratumCake\b'                 # specific multi-character finding ID
  '\bDS4-P[0-9]+\b'               # NS2_PATCHES patch tokens
  '\bN2-[0-9]+\b'                 # ns-2 core defects (HISTORICAL_BUGS public)
  '\bD2-[0-9]+\b'                 # DS4-for-ns-2 defects (HISTORICAL_BUGS public)
  '\bN3-[0-9]+\b'                 # ns-3 core defects (HISTORICAL_BUGS public)
  'Experiment Report, Phase'      # Ferrari 2000 publication title, not a phase label
  '\bDS4-patched\b'               # historical quotation in provenance archival records
  'shorthand \("DS4"\) is retired' # the preface sentence that retires the shorthand
)

# Joined alternation of every allowlist pattern, built once for the
# stripping substitution below.
ALLOWLIST_STRIP_RE="$(IFS='|'; printf '%s' "${ALLOWLIST_PATTERNS[*]}")"
readonly ALLOWLIST_STRIP_RE

# Prints @p line with every allowlisted token removed. perl(1) performs the
# substitution because the patterns carry \b word boundaries, which BSD
# sed -E does not implement (the script must run on both macOS and Linux).
strip_allowlisted() {
  printf '%s' "$1" | perl -pe "s/(?:${ALLOWLIST_STRIP_RE})//g"
}

# Returns 0 (true) if the banned @p regex no longer matches @p line once
# allowlisted tokens are stripped — i.e. the candidate was only ever the
# allowlisted token itself and should be skipped.
is_allowlisted() {
  local line="$1" regex="$2"
  ! strip_allowlisted "$line" | grep -qE "$regex"
}

violations=0
> /tmp/lint-jargon.out

for path in "${SCAN_PATHS_CODE[@]}"; do
  if [ ! -d "$path" ]; then
    continue
  fi
  for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"
    regex="${entry#*|}"
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      if is_allowlisted "$content" "$regex"; then
        continue
      fi
      printf '%s:%s [%s] %s\n' "$file" "$line" "$label" "$content" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    done < <(grep -rnE "${FILE_GLOBS_CODE[@]}" "$regex" "$path" 2>/dev/null || true)
  done
done

for path in "${SCAN_PATHS_MD[@]}"; do
  if [ ! -d "$path" ]; then
    continue
  fi
  for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"
    regex="${entry#*|}"
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      if is_allowlisted "$content" "$regex"; then
        continue
      fi
      printf '%s:%s [%s] %s\n' "$file" "$line" "$label" "$content" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    done < <(grep -rnE "${FILE_GLOBS_MD[@]}" "$regex" "$path" 2>/dev/null || true)
  done
done

for path in "${SCAN_PATHS_CONFIG[@]}"; do
  if [ ! -d "$path" ]; then
    continue
  fi
  for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"
    regex="${entry#*|}"
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      if is_allowlisted "$content" "$regex"; then
        continue
      fi
      printf '%s:%s [%s] %s\n' "$file" "$line" "$label" "$content" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    done < <(grep -rnE "${FILE_GLOBS_CONFIG[@]}" "$regex" "$path" 2>/dev/null || true)
  done
done

for md_file in "${SCAN_FILES_MD[@]}"; do
  if [ ! -f "$md_file" ]; then
    continue
  fi
  for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"
    regex="${entry#*|}"
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      if is_allowlisted "$content" "$regex"; then
        continue
      fi
      printf '%s:%s [%s] %s\n' "$file" "$line" "$label" "$content" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    done < <(grep -HnE "$regex" "$md_file" 2>/dev/null || true)
  done
done

for path in "${SCAN_PATHS_PROVENANCE[@]}" "${SCAN_PATHS_SCRIPTS_MD[@]}"; do
  if [ ! -d "$path" ]; then
    continue
  fi
  for entry in "${PATTERNS[@]}"; do
    label="${entry%%|*}"
    # Bare ADR-NNNN is the sanctioned citation form in shipped docs/provenance,
    # so the decision-record-number pattern is skipped for this tree only.
    if [ "$label" = "decision-record-number" ]; then
      continue
    fi
    regex="${entry#*|}"
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      if is_allowlisted "$content" "$regex"; then
        continue
      fi
      printf '%s:%s [%s] %s\n' "$file" "$line" "$label" "$content" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    done < <(grep -rnE "${FILE_GLOBS_MD[@]}" "$regex" "$path" 2>/dev/null || true)
  done
done

# ---------------------------------------------------------------------------
# Structural rules — checks that need more context than a per-line token
# pattern. Each appends to the same violations report.
# ---------------------------------------------------------------------------

# Stale handbook-chapter reference. Extracts every part-numbered chapter
# filename mentioned in release-bound markdown and requires the file to
# exist under handbook/. The alternation is ordered longest-first
# (III|II|I) and \b-anchored so a reference to III-03-l4s.md is never
# miscounted as I-03-l4s.md — Roman-numeral part prefixes are substrings
# of one another, which has defeated unanchored sweeps before.
readonly CHAPTER_REF_RE='\b(III|II|I)-[0-9]{2}[A-Z]?-[a-z0-9-]+\.md'
if [ -d "handbook" ]; then
  while IFS=: read -r file line token; do
    [ -z "$file" ] && continue
    if [ ! -f "handbook/$token" ]; then
      printf '%s:%s [stale-chapter-ref] %s does not exist under handbook/\n' \
        "$file" "$line" "$token" >> /tmp/lint-jargon.out
      violations=$((violations + 1))
    fi
  done < <(
    { grep -rnoE "${FILE_GLOBS_MD[@]}" "$CHAPTER_REF_RE" \
        "${SCAN_PATHS_MD[@]}" "${SCAN_PATHS_PROVENANCE[@]}" \
        "${SCAN_PATHS_SCRIPTS_MD[@]}" 2>/dev/null || true
      grep -noE "$CHAPTER_REF_RE" "${SCAN_FILES_MD[@]}" 2>/dev/null || true
    }
  )

  # ADR Public-surface lines must also track chapter renames (dev tree
  # only — docs/adr/ does not ship). ADR bodies are immutable history and
  # are deliberately NOT scanned; only the Public-surface metadata line
  # carries a currency obligation.
  if [ -d "docs/adr" ]; then
    while IFS=: read -r file line content; do
      [ -z "$file" ] && continue
      while read -r token; do
        [ -z "$token" ] && continue
        if [ ! -f "handbook/$token" ]; then
          printf '%s:%s [stale-chapter-ref] %s does not exist under handbook/\n' \
            "$file" "$line" "$token" >> /tmp/lint-jargon.out
          violations=$((violations + 1))
        fi
      done < <(printf '%s\n' "$content" | grep -oE "$CHAPTER_REF_RE" || true)
    done < <(grep -rn --include='*.md' 'Public surface' docs/adr 2>/dev/null || true)
  fi
fi

# ADR Public-surface format variance (dev tree only). The metadata line is
# spelled `**Public surface:**` (colon inside the bold). The colon-outside
# variant `**Public surface**:` reads identically but silently escapes
# sed/grep addresses written against the canonical form — three ADRs
# missed a rename sweep that way.
if [ -d "docs/adr" ]; then
  while IFS=: read -r file line content; do
    [ -z "$file" ] && continue
    printf '%s:%s [adr-public-surface-format] use **Public surface:** (colon inside bold)\n' \
      "$file" "$line" >> /tmp/lint-jargon.out
    violations=$((violations + 1))
  done < <(grep -rn --include='*.md' -F '**Public surface**:' docs/adr 2>/dev/null || true)
fi

if [ "$violations" -eq 0 ]; then
  printf 'lint-jargon: clean — no internal-jargon tokens found in release-bound sources.\n'
  exit 0
fi

printf 'lint-jargon: %d violation(s) across release-bound sources:\n\n' "$violations"
sort -u /tmp/lint-jargon.out
printf '\nSee docs/ns3-doxygen-style.md section 10.1 for the rule and acceptable substitutes.\n'
exit 1
