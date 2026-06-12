#!/usr/bin/env python3
"""
Q-1.4 harness: two-sample Kolmogorov–Smirnov test of per-packet IPDV (and OWD)
distributions between the ns-2 DiffServ4NS baseline and the ns-3 port.

Loads:
  - ns-2 frequency distribution files:
        baseline/ns2/example-1/<ALG>-<PKTSIZE>/{owd,ipdv}<PKTSIZE>_FD.tr
    Format per line: "<bin_seconds> <percentage>" (bin width 10 µs).
  - ns-3 per-packet sample files:
        /tmp/adr-0037-baseline/ex1-run1/<ALG>/ or a user-supplied dir.
    Format per line: "<time_s> <metric_ms>" one row per received EF packet.

The ns-2 FD is resampled into a synthetic sample vector by repeating each bin
centre (in milliseconds, to match ns-3 units) proportionally to its percentage
times a fixed multiplier. Both sample vectors are then fed to scipy's
`ks_2samp`. The output is a Markdown-rendered table plus a CDF-overlay figure
per scheduler.

Usage:
    scripts/ipdv-ks-compare.py --ns3-dir <dir> [--schedulers PQ,WFQ,…]
                               [--pktsize 512] [--out out/]

See also: specs/03-quality.md Q-1.4 (IPDV / OWD KS comparison gate).
"""
from __future__ import annotations

import argparse
import pathlib
import sys
from dataclasses import dataclass

import numpy as np

try:
    from scipy import stats
except ImportError:  # pragma: no cover
    sys.exit("scipy is required; install via `pip install scipy numpy matplotlib`.")

try:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt  # noqa: E402
except ImportError:  # pragma: no cover
    plt = None


DEFAULT_SCHEDULERS = ["PQ", "WFQ", "WF2Qp", "SCFQ", "SFQ"]
NS2_BASELINE_ROOT = pathlib.Path("baseline/ns2/example-1")
# Each bin in the ns-2 FD contributes (percentage × MULT) synthetic samples.
# At MULT=1000 and ≥ 80 non-empty bins, this gives ~8e5 synthetic samples —
# KS is stable well below this; higher values just slow the computation.
FD_RESAMPLE_MULT = 1000


@dataclass
class Result:
    scheduler: str
    pkt_size: int
    metric: str  # "OWD" or "IPDV"
    ns2_n: int
    ns3_n: int
    ns2_mean_ms: float
    ns3_mean_ms: float
    ns2_median_ms: float
    ns3_median_ms: float
    ks_d: float
    ks_p: float

    def verdict(self, alpha: float) -> str:
        return "PASS" if self.ks_p >= alpha else "FAIL"


def load_ns2_fd(path: pathlib.Path, *, in_ms: bool = True) -> np.ndarray:
    """Load an ns-2 frequency-distribution file and expand to a sample vector.

    Parameters
    ----------
    path : pathlib.Path
        Path to an ns-2 ``*_FD.tr`` file (``bin_seconds percentage`` format).
    in_ms : bool
        Convert bin centres from seconds to milliseconds (to match ns-3
        per-packet traces, which store values in ms). Default True.
    """
    data = np.loadtxt(path)
    bins = data[:, 0]
    pct = data[:, 1]
    if in_ms:
        bins = bins * 1000.0
    counts = np.rint(pct * FD_RESAMPLE_MULT).astype(int)
    samples = np.repeat(bins, counts)
    return samples


def load_ns3_samples(path: pathlib.Path) -> np.ndarray:
    """Load an ns-3 per-packet sample file (``time_s value_ms``)."""
    data = np.loadtxt(path)
    if data.ndim == 1:  # single row
        return np.array([data[1]])
    return data[:, 1]


def compare_one(
    scheduler: str,
    pkt_size: int,
    ns3_dir: pathlib.Path,
    ns2_root: pathlib.Path = NS2_BASELINE_ROOT,
) -> tuple[Result, Result]:
    """Run KS on IPDV and OWD for a single (scheduler, pkt_size) pair."""
    ns2_run = ns2_root / f"{scheduler}-{pkt_size:04d}"
    ns2_owd_fd = ns2_run / f"owd{pkt_size}_FD.tr"
    ns2_ipdv_fd = ns2_run / f"ipdv{pkt_size}_FD.tr"
    ns3_run = ns3_dir / scheduler / f"{scheduler}-{pkt_size:04d}"
    # diffserv-example-1 output can take any of three shapes:
    #   1. <ns3_dir>/<ALG>/<ALG>-<SIZE>/  (nested — multi-scheduler sweep)
    #   2. <ns3_dir>/<ALG>/               (flat, no size suffix)
    #   3. <ns3_dir>/<ALG>-<SIZE>/        (size-suffixed flat — what
    #      run-ns3-multi-scheduler.sh writes today).
    # Try each in order so the caller doesn't need to stage symlinks.
    if not ns3_run.is_dir():
        ns3_run = ns3_dir / scheduler
    if not ns3_run.is_dir():
        ns3_run = ns3_dir / f"{scheduler}-{pkt_size:04d}"
    ns3_owd_samples = ns3_run / "OWD-samples.tr"
    ns3_ipdv_samples = ns3_run / "IPDV-samples.tr"

    for p in (ns2_owd_fd, ns2_ipdv_fd, ns3_owd_samples, ns3_ipdv_samples):
        if not p.is_file():
            raise FileNotFoundError(f"missing trace: {p}")

    results = []
    for metric, ns2_path, ns3_path in (
        ("OWD", ns2_owd_fd, ns3_owd_samples),
        ("IPDV", ns2_ipdv_fd, ns3_ipdv_samples),
    ):
        ns2 = load_ns2_fd(ns2_path)
        ns3 = load_ns3_samples(ns3_path)
        if len(ns2) < 2 or len(ns3) < 2:
            raise ValueError(
                f"{metric}: insufficient samples (ns2={len(ns2)}, ns3={len(ns3)})"
            )
        d, p = stats.ks_2samp(ns2, ns3)
        results.append(
            Result(
                scheduler=scheduler,
                pkt_size=pkt_size,
                metric=metric,
                ns2_n=len(ns2),
                ns3_n=len(ns3),
                ns2_mean_ms=float(np.mean(ns2)),
                ns3_mean_ms=float(np.mean(ns3)),
                ns2_median_ms=float(np.median(ns2)),
                ns3_median_ms=float(np.median(ns3)),
                ks_d=float(d),
                ks_p=float(p),
            )
        )
    return results[0], results[1]


def plot_cdf_overlay(
    scheduler: str,
    pkt_size: int,
    metric: str,
    ns2: np.ndarray,
    ns3: np.ndarray,
    result: Result,
    out_dir: pathlib.Path,
    alpha: float,
) -> None:
    if plt is None:
        return
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for label, samples, style in (
        (f"ns-2 (n={len(ns2):,})", ns2, "-"),
        (f"ns-3 (n={len(ns3):,})", ns3, "--"),
    ):
        sorted_s = np.sort(samples)
        cdf = np.arange(1, len(sorted_s) + 1) / len(sorted_s)
        ax.plot(sorted_s, cdf, style, label=label, linewidth=1.5)
    ax.set_xlabel(f"{metric} (ms)")
    ax.set_ylabel("CDF")
    ax.set_title(
        f"{metric} CDF — {scheduler} pktSize={pkt_size}\n"
        f"KS D={result.ks_d:.4f}  p={result.ks_p:.4g}  "
        f"verdict={result.verdict(alpha)} (α={alpha})"
    )
    ax.legend(loc="lower right")
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    stem = f"ks-{metric.lower()}-{scheduler}-{pkt_size:04d}"
    # Emit PNG for quick browsing + SVG and PDF for handbook / paper use
    # (matplotlib writes all three from the same figure; cheap).
    for ext in ("png", "svg", "pdf"):
        fig.savefig(out_dir / f"{stem}.{ext}", dpi=120)
    plt.close(fig)


def render_table(results: list[Result], alpha: float) -> str:
    lines = [
        "| Scheduler | pktSize |  Metric | ns-2 n   | ns-3 n   | ns-2 mean | ns-3 mean | ns-2 med  | ns-3 med  |   KS D |      p | α | Verdict |",
        "|-----------|---------|---------|----------|----------|-----------|-----------|-----------|-----------|--------|--------|---|---------|",
    ]
    for r in results:
        lines.append(
            f"| {r.scheduler:9s} | {r.pkt_size:7d} | {r.metric:>7s} "
            f"| {r.ns2_n:8d} | {r.ns3_n:8d} "
            f"| {r.ns2_mean_ms:9.4f} | {r.ns3_mean_ms:9.4f} "
            f"| {r.ns2_median_ms:9.4f} | {r.ns3_median_ms:9.4f} "
            f"| {r.ks_d:6.4f} | {r.ks_p:6.4g} "
            f"| {alpha:.2f} | {r.verdict(alpha):7s} |"
        )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--ns3-dir",
        required=True,
        type=pathlib.Path,
        help="Root of ns-3 per-packet output (e.g. /tmp/adr-0037-baseline/ex1-run1)",
    )
    ap.add_argument(
        "--schedulers",
        default=",".join(DEFAULT_SCHEDULERS),
        help="Comma-separated scheduler names (default: PQ,WFQ,WF2Qp,SCFQ,SFQ)",
    )
    ap.add_argument(
        "--pktsize",
        type=int,
        default=512,
        help="Packet size (bytes); must correspond to an ns-2 baseline subdir.",
    )
    ap.add_argument(
        "--alpha",
        type=float,
        default=0.05,
        help="KS significance level (default 0.05; see notes below)",
    )
    ap.add_argument(
        "--out",
        type=pathlib.Path,
        default=pathlib.Path("output/q14-ks"),
        help="Output directory for CDF plots + Markdown report.",
    )
    args = ap.parse_args()

    schedulers = [s.strip() for s in args.schedulers.split(",") if s.strip()]
    args.out.mkdir(parents=True, exist_ok=True)

    results: list[Result] = []
    raw_for_plots: list[tuple[Result, np.ndarray, np.ndarray]] = []

    for sched in schedulers:
        try:
            owd_res, ipdv_res = compare_one(sched, args.pktsize, args.ns3_dir)
        except FileNotFoundError as e:
            print(f"SKIP {sched}/{args.pktsize}: {e}", file=sys.stderr)
            continue
        results.extend([owd_res, ipdv_res])

        # re-load for plotting; cheaper than reshaping compare_one's API.
        # Mirror the 3-way layout search from compare_one().
        ns2_run = NS2_BASELINE_ROOT / f"{sched}-{args.pktsize:04d}"
        ns3_run = args.ns3_dir / sched / f"{sched}-{args.pktsize:04d}"
        if not ns3_run.is_dir():
            ns3_run = args.ns3_dir / sched
        if not ns3_run.is_dir():
            ns3_run = args.ns3_dir / f"{sched}-{args.pktsize:04d}"
        raw_for_plots.append(
            (owd_res, load_ns2_fd(ns2_run / f"owd{args.pktsize}_FD.tr"), load_ns3_samples(ns3_run / "OWD-samples.tr")),
        )
        raw_for_plots.append(
            (ipdv_res, load_ns2_fd(ns2_run / f"ipdv{args.pktsize}_FD.tr"), load_ns3_samples(ns3_run / "IPDV-samples.tr")),
        )

    if not results:
        print("No comparisons ran; check --ns3-dir and baseline coverage.", file=sys.stderr)
        return 1

    table = render_table(results, args.alpha)
    print(table)

    report = args.out / f"ks-report-pkt{args.pktsize}.md"
    report.write_text(
        "# Q-1.4 Kolmogorov–Smirnov report\n\n"
        f"ns-2 baseline: `{NS2_BASELINE_ROOT}/<SCHED>-{args.pktsize:04d}/`  \n"
        f"ns-3 run:      `{args.ns3_dir}`  \n"
        f"KS α:          {args.alpha}  \n"
        f"FD resample multiplier: {FD_RESAMPLE_MULT}\n\n"
        f"{table}\n\n"
        "**Note on ns-2 sample-size inflation.** The ns-2 side stores only a\n"
        "frequency distribution (bin centres + percentages) with 10 µs bin\n"
        "width. We resample to a synthetic sample vector by repeating each\n"
        "bin centre proportionally to its percentage × "
        f"{FD_RESAMPLE_MULT}. The resulting ns-2 `n` column reflects this\n"
        "synthetic size, not the original packet count.\n"
    )
    print(f"\nReport written: {report}", file=sys.stderr)

    for res, ns2, ns3 in raw_for_plots:
        plot_cdf_overlay(
            res.scheduler, res.pkt_size, res.metric, ns2, ns3, res, args.out, args.alpha
        )

    if plt is not None:
        print(f"CDF overlays written to: {args.out}", file=sys.stderr)
    else:
        print("matplotlib not available; CDF plots skipped.", file=sys.stderr)

    any_fail = any(r.verdict(args.alpha) == "FAIL" for r in results)
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
