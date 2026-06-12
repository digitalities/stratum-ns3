"""CLI entrypoint: python -m ns3_csv_to_flent."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import core
from .schemas import SCHEMAS


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="ns3-csv-to-flent",
        description=(
            "Convert an ns-3 CSV bundle (emitted by DsFlentCsvSink) into a "
            "Flent JSON v1 .flent.gz file."
        ),
    )
    parser.add_argument(
        "--test",
        required=True,
        choices=sorted(SCHEMAS.keys()),
        help="Test schema to apply (must match the bundle layout).",
    )
    parser.add_argument(
        "--indir",
        required=True,
        type=Path,
        help="Path to the CSV bundle directory.",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="Path to the output .flent.gz file.",
    )
    parser.add_argument(
        "--title",
        default=None,
        help="Optional title for the Flent JSON doc (defaults to schema title).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Testable entry point. Returns process exit code."""
    parser = _build_parser()
    args = parser.parse_args(argv)

    indir: Path = args.indir
    if not indir.is_dir():
        print(f"error: --indir not a directory: {indir}", file=sys.stderr)
        return 2

    schema = SCHEMAS[args.test]
    metadata = core.read_metadata(indir)
    doc = core.emit_flent_doc(metadata, schema, indir, title=args.title)
    core.write_flent_gz(doc, args.output)
    return 0


def cli() -> None:
    """Console-script entry point (sets sys.exit)."""
    sys.exit(main())


if __name__ == "__main__":
    cli()
