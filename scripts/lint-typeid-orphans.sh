#!/usr/bin/env bash
# scripts/lint-typeid-orphans.sh
#
# Flags orphaned instantiable TypeIds: classes registered with
# .AddConstructor<T>() in model/ or helper/ that no other file in the
# module references. A registered constructor keeps a class reachable
# (instantiable by TypeId string) even with zero #include users, so an
# include-graph or dead-symbol check never reports it. A class that
# nothing in model/, helper/, examples/, or test/ names is public API
# surface with no exercised behaviour — almost always residue from a
# finished experiment.
#
# Detection is by class-name token. Classes referenced only through a
# registry cell or an attribute path still match, because those sites
# spell the class name inside the TypeId string. Common names always
# have hits and are never flagged, so the check errs toward silence —
# a flagged name is worth a look.
#
# Exit 0: no orphans. Exit 1: orphans listed on stdout.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# Layout detection: the monorepo keeps the module at src/ns-3/; the
# public module repo has it at the repo root.
if [ -d "src/ns-3/model" ]; then
    readonly MODULE_DIR="src/ns-3"
else
    readonly MODULE_DIR="."
fi

fail=0

while IFS= read -r reg_file; do
    base="$(basename "$reg_file")"
    base="${base%.cc}"

    while IFS= read -r cls; do
        [ -n "$cls" ] || continue
        hits="$(grep -rlE "\b${cls}\b" \
                     "$MODULE_DIR/model" "$MODULE_DIR/helper" \
                     "$MODULE_DIR/examples" "$MODULE_DIR/test" \
                     --include='*.cc' --include='*.h' 2>/dev/null \
                  | grep -vE "/${base}\.(h|cc)$" || true)"
        if [ -z "$hits" ]; then
            printf 'ORPHAN TypeId: %s — registered in %s.{h,cc}, referenced nowhere else\n' \
                   "$cls" "$base"
            fail=1
        fi
    done < <(grep -hoE 'AddConstructor<[A-Za-z0-9_:]+>' "$reg_file" \
               | sed -E 's/AddConstructor<([A-Za-z0-9_:]+)>/\1/' \
               | sed -E 's/.*:://' \
               | sort -u)
done < <(grep -rl 'AddConstructor<' "$MODULE_DIR/model" "$MODULE_DIR/helper" \
           --include='*.cc' 2>/dev/null | sort)

if [ "$fail" -eq 0 ]; then
    echo "PASS: no orphaned TypeIds in model/ or helper/"
fi
exit "$fail"
