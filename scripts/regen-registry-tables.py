#!/usr/bin/env python3
"""Regenerate Markdown tables in handbook chapters from substrate-registry
manifests.

The tool is intentionally narrow: it reads a JSON manifest, projects a
configurable set of fields to a Markdown table, and rewrites the section
of a target file delimited by a pair of HTML-style markers:

    <!-- BEGIN registry-table: <id> -->
    | ... |
    <!-- END registry-table: <id> -->

Anything between BEGIN and END is replaced. Anything outside the markers
is left untouched. If the target file already contains the same content
between the markers, the file is not rewritten — Q-Reg-3's contract is
"idempotent regeneration", so CI / release workflows can call this on
every push without dirtying git.

Usage:

    regen-registry-tables.py \\
        --manifest scripts/aqm-eval/scheduler-manifest.json \\
        --array-key schedulers \\
        --target <CHAPTER>.md \\
        --marker scheduler-catalogue \\
        --columns fileTag,displayName,family,parameterShape,description \\
        --headers "Tag,Name,Family,Parameter shape,Description"

Idempotent: exits 0 with no change if the section is already current.
Exits 0 with a write if the section needed updating. Exits 2 if the
markers are missing from the target.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List

sys.path.insert(0, str(Path(__file__).resolve().parent / "common"))
import manifest_loader  # noqa: E402


def render_markdown_table(rows: List[List], headers: List[str]) -> str:
    """Render a GitHub-flavoured Markdown table with left-aligned cells.

    Caller-provided headers are required; the manifest is data, not
    presentation, so the column-display labels live in the call site.
    """
    out: List[str] = []
    out.append("| " + " | ".join(headers) + " |")
    out.append("|" + "|".join(["---"] * len(headers)) + "|")
    for r in rows:
        cells = [str(c) for c in r]
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def replace_marker_block(text: str, marker_id: str, new_block: str) -> str:
    """Replace the content between BEGIN/END marker pair with `new_block`.

    Markers stay in place; only their interior is rewritten. Newlines
    surrounding the block are preserved as a single blank line on each
    side so successive regenerations don't drift the file's whitespace.

    Raises KeyError if either marker is missing.
    """
    begin = f"<!-- BEGIN registry-table: {marker_id} -->"
    end = f"<!-- END registry-table: {marker_id} -->"
    b = text.find(begin)
    e = text.find(end)
    if b < 0 or e < 0 or e <= b:
        raise KeyError(
            f"markers for '{marker_id}' not found or out of order in target file"
        )
    head = text[: b + len(begin)]
    tail = text[e:]
    return head + "\n" + new_block + "\n" + tail


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path, required=True)
    p.add_argument("--array-key", required=True)
    p.add_argument("--target", type=Path, required=True)
    p.add_argument("--marker", required=True, help="marker id (e.g. scheduler-catalogue)")
    p.add_argument(
        "--columns",
        required=True,
        help="comma-separated manifest field names projected as table columns",
    )
    p.add_argument(
        "--headers",
        required=True,
        help="comma-separated table header labels; must match column count",
    )
    args = p.parse_args(argv)

    cols = [c.strip() for c in args.columns.split(",")]
    headers = [h.strip() for h in args.headers.split(",")]
    if len(cols) != len(headers):
        print(
            f"--columns ({len(cols)}) and --headers ({len(headers)}) "
            f"must have the same length",
            file=sys.stderr,
        )
        return 2

    entries = manifest_loader.load_entries(args.manifest, args.array_key)
    rows = manifest_loader.project(entries, cols)
    table = render_markdown_table(rows, headers)

    text = args.target.read_text()
    try:
        new_text = replace_marker_block(text, args.marker, table)
    except KeyError as ex:
        print(f"error: {ex}", file=sys.stderr)
        return 2

    if new_text == text:
        print(f"[regen-registry-tables] {args.target}:{args.marker} already current")
        return 0

    args.target.write_text(new_text)
    print(f"[regen-registry-tables] rewrote {args.target}:{args.marker} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
