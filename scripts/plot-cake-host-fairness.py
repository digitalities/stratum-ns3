#!/usr/bin/env python3
"""Produce the 5 host-fairness sweep figures from sweep-cells.csv.

Figures:
  1. cake-host-fairness-headline-line.{svg,pdf} - share_A vs N for N-vs-1
  2. cake-host-fairness-headline-heatmap.{svg,pdf} - 3-panel sparse heatmap
  3. cake-host-fairness-symmetric.{svg,pdf} - bar chart at (N, N) cells
  4. cake-host-fairness-mirror.{svg,pdf} - bar chart of (N, 1) vs (1, N)
  5. cake-host-fairness-f-vs-n.{svg,pdf} - f normalised vs log(N)
"""

import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

REPO_ROOT = Path(os.environ.get(
    "REPO_ROOT", Path(__file__).resolve().parent.parent))
OUT_DIR = Path(os.environ.get(
    "OUT_DIR", REPO_ROOT / "output" / "ns3" / "cake-host-fairness"))

IMPLS = [
    ("linux", "none", "Linux tc-cake", "tab:green"),
    ("stratum", "nested", "Stratum (Nested DRR)", "tab:blue"),
    ("stratum", "flat", "Stratum (Flat DRR + ilog2)", "tab:orange"),
]


def _save(fig: plt.Figure, name: str) -> None:
    for ext in ("svg", "pdf"):
        path = OUT_DIR / f"{name}.{ext}"
        fig.savefig(path, bbox_inches="tight")
    plt.close(fig)


def _filter_impl(cells: pd.DataFrame, impl: str, strat: str) -> pd.DataFrame:
    return cells[(cells["implementation"] == impl) & (cells["strategy"] == strat)]


def plot_headline_line(cells: pd.DataFrame) -> None:
    fig, ax = plt.subplots(figsize=(8, 5))
    for impl, strat, label, color in IMPLS:
        sub = _filter_impl(cells, impl, strat)
        sub = sub[(sub["M"] == 1) & (sub["N"] >= 2)].sort_values("N")
        ax.errorbar(sub["N"], sub["share_A_mean"], yerr=sub["share_A_stddev"],
                    marker="o", label=label, color=color, capsize=3)
        for _, row in sub.iterrows():
            ax.annotate(f"{row['share_A_mean']:.3f}",
                        (row["N"], row["share_A_mean"]),
                        textcoords="offset points", xytext=(5, 5),
                        fontsize=7, color=color)
    ax.axhline(0.5, color="grey", linestyle="--", linewidth=0.7,
               label="host-fair (0.5)")
    ax.set_xscale("log", base=2)
    ax.set_xlabel("N (host-A flow count, M=1)")
    ax.set_ylabel("share_A (mean over k=3 replicas)")
    ax.set_title("Host-share share_A for N-vs-1 (CUBIC, 100Mbps, 30s)")
    ax.legend(loc="best")
    ax.grid(True, alpha=0.3)
    _save(fig, "cake-host-fairness-headline-line")


def plot_headline_heatmap(cells: pd.DataFrame) -> None:
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))
    Ns = [1, 2, 4, 8, 16]
    Ms = [1, 2, 4, 8, 16]

    for ax, (impl, strat, label, _color) in zip(axes, IMPLS):
        sub = _filter_impl(cells, impl, strat)
        grid_f = np.full((len(Ns), len(Ms)), np.nan)
        grid_share = np.full((len(Ns), len(Ms)), np.nan)
        for _, row in sub.iterrows():
            try:
                i = Ns.index(int(row["N"]))
                j = Ms.index(int(row["M"]))
            except ValueError:
                continue
            grid_f[i, j] = row["f_mean"]
            grid_share[i, j] = row["share_A_mean"]
        cmap = matplotlib.colormaps["RdBu"].copy()
        cmap.set_bad("lightgrey")
        im = ax.imshow(grid_f, cmap=cmap, vmin=-0.2, vmax=1.2,
                       origin="lower", aspect="auto")
        ax.set_xticks(range(len(Ms)))
        ax.set_xticklabels([str(m) for m in Ms])
        ax.set_yticks(range(len(Ns)))
        ax.set_yticklabels([str(n) for n in Ns])
        ax.set_xlabel("M (host-B flow count)")
        ax.set_ylabel("N (host-A flow count)")
        ax.set_title(label)
        for i in range(len(Ns)):
            for j in range(len(Ms)):
                if not np.isnan(grid_share[i, j]):
                    ax.text(j, i, f"{grid_share[i, j]:.2f}",
                            ha="center", va="center", fontsize=7,
                            color="black")
        fig.colorbar(im, ax=ax, label="f (1.0 = host-fair, 0 = per-flow)")
    fig.suptitle("Host-share f(N, M) — Stratum vs Linux (NaN at N=M diagonal)")
    fig.tight_layout()
    _save(fig, "cake-host-fairness-headline-heatmap")


def plot_symmetric(cells: pd.DataFrame) -> None:
    fig, ax = plt.subplots(figsize=(8, 5))
    Ns = [1, 2, 4, 8]
    width = 0.25
    x = np.arange(len(Ns))
    for off, (impl, strat, label, color) in enumerate(IMPLS):
        sub = _filter_impl(cells, impl, strat)
        sub = sub[sub["N"] == sub["M"]].set_index("N")
        vals = [sub.loc[n, "share_A_mean"] if n in sub.index else np.nan
                for n in Ns]
        errs = [sub.loc[n, "share_A_stddev"] if n in sub.index else 0
                for n in Ns]
        ax.bar(x + (off - 1) * width, vals, width=width, yerr=errs,
               label=label, color=color, capsize=3)
    ax.axhline(0.5, color="grey", linestyle="--", linewidth=0.7,
               label="host-fair (0.5)")
    ax.set_xticks(x)
    ax.set_xticklabels([f"({n},{n})" for n in Ns])
    ax.set_xlabel("(N, M) symmetric control")
    ax.set_ylabel("share_A (mean ± stddev over k=3)")
    ax.set_title("Symmetric controls (should all yield share_A ≈ 0.5)")
    ax.legend(loc="best")
    ax.grid(True, alpha=0.3, axis="y")
    ax.set_ylim(0.4, 0.6)
    _save(fig, "cake-host-fairness-symmetric")


def plot_mirror(cells: pd.DataFrame) -> None:
    fig, ax = plt.subplots(figsize=(10, 5))
    Ns = [2, 4, 8, 16]
    width = 0.12
    x = np.arange(len(Ns))
    for off, (impl, strat, label, color) in enumerate(IMPLS):
        sub = _filter_impl(cells, impl, strat).set_index(["N", "M"])
        fwd = [sub.loc[(n, 1), "share_A_mean"]
               if (n, 1) in sub.index else np.nan for n in Ns]
        mir = [1 - sub.loc[(1, n), "share_A_mean"]
               if (1, n) in sub.index else np.nan for n in Ns]
        ax.bar(x + (off * 2 - 2) * width, fwd, width=width,
               label=f"{label}: share_A(N,1)", color=color, hatch="")
        ax.bar(x + (off * 2 - 1) * width, mir, width=width,
               label=f"{label}: 1−share_A(1,N)", color=color, hatch="//")
    ax.axhline(0.5, color="grey", linestyle="--", linewidth=0.7)
    ax.set_xticks(x)
    ax.set_xticklabels([f"N={n}" for n in Ns])
    ax.set_xlabel("Asymmetric pair (N-vs-1 ↔ 1-vs-N)")
    ax.set_ylabel("share_A or 1−share_A_mirror")
    ax.set_title("Mirror-test: share_A(N,1) should equal 1−share_A(1,N)")
    ax.legend(loc="best", fontsize=7)
    ax.grid(True, alpha=0.3, axis="y")
    _save(fig, "cake-host-fairness-mirror")


def plot_f_vs_n(cells: pd.DataFrame) -> None:
    fig, ax = plt.subplots(figsize=(8, 5))
    for impl, strat, label, color in IMPLS:
        sub = _filter_impl(cells, impl, strat)
        sub = sub[(sub["M"] == 1) & (sub["N"] >= 2)].sort_values("N")
        ax.errorbar(sub["N"], sub["f_mean"], yerr=sub["f_stddev"],
                    marker="o", label=label, color=color, capsize=3)
    ax.axhline(1.0, color="grey", linestyle=":", linewidth=0.7,
               label="host-fair (f=1)")
    ax.axhline(0.0, color="grey", linestyle=":", linewidth=0.7,
               label="per-flow (f=0)")
    ax.set_xscale("log", base=2)
    ax.set_xlabel("N (host-A flow count, M=1)")
    ax.set_ylabel("f = (per_flow_share_A − share_A)/(per_flow_share_A − 0.5)")
    ax.set_title("Normalised fairness fraction f vs log(N)")
    ax.legend(loc="best")
    ax.grid(True, alpha=0.3)
    ax.set_ylim(-0.2, 1.2)
    _save(fig, "cake-host-fairness-f-vs-n")


def plot_protocol_robustness(cells_csv: Path, out_basename: str) -> None:
    """4xK grouped bar chart for the protocol-robustness matrix.

    cells_csv columns expected (subset): implementation, protocol, N, M,
    share_A_mean. Rendered as one subplot per (N, M) cell with 4 protocols
    on the x-axis and two bars per protocol (Stratum vs Linux). The
    host-fair reference line at share_A=0.5 is overlaid; per-protocol gap
    is annotated above the taller of the two bars."""
    df = pd.read_csv(cells_csv, keep_default_na=False)
    cells = sorted({(int(r.N), int(r.M)) for r in df.itertuples()})
    protocols = ["cubic", "newreno", "bbr", "udp"]
    width = 0.35
    x = np.arange(len(protocols))

    fig, axes = plt.subplots(1, len(cells), figsize=(6 * len(cells), 5),
                             sharey=True)
    if len(cells) == 1:
        axes = [axes]

    for ax, (N, M) in zip(axes, cells):
        sub = df[(df["N"] == N) & (df["M"] == M)]
        def _share(impl: str, proto: str) -> float:
            row = sub[(sub["implementation"] == impl)
                      & (sub["protocol"] == proto)]
            return float(row["share_A_mean"].iloc[0]) if len(row) > 0 else float("nan")

        s_vals = [_share("stratum", p) for p in protocols]
        l_vals = [_share("linux", p) for p in protocols]
        ax.bar(x - width / 2, s_vals, width, label="Stratum", color="tab:blue")
        ax.bar(x + width / 2, l_vals, width, label="Linux", color="tab:green")
        for i_p, (s, l) in enumerate(zip(s_vals, l_vals)):
            gap = abs(s - l)
            ax.annotate(f"Δ={gap:.3f}",
                        (x[i_p], max(s, l) + 0.02),
                        ha="center", fontsize=9)
        ax.axhline(0.5, color="gray", linestyle="--", alpha=0.5,
                   label="host-fair target")
        ax.set_xticks(x)
        ax.set_xticklabels(protocols)
        ax.set_ylim(0, 1.0)
        ax.set_xlabel("Protocol")
        ax.set_title(f"Cell (N={N}, M={M})")
        ax.legend(loc="upper left", fontsize=9)

    axes[0].set_ylabel("share_A  (Host A goodput fraction)")
    fig.suptitle("Stratum vs Linux: host-share across TCP variants and UDP",
                 fontsize=12)
    plt.tight_layout()
    _save(fig, out_basename)


def aggregate_protocol_cells(perflow_csv: Path) -> pd.DataFrame:
    """Aggregate protocol-robustness per-flow rows into per-cell mean share_A.

    Groups by (implementation, protocol, N, M, rng_run) → share_A per
    replica → mean across replicas per cell. Output columns:
    implementation, protocol, N, M, share_A_mean, share_A_std, total_mbps_mean."""
    df = pd.read_csv(perflow_csv, keep_default_na=False)
    # Map "linux"'s tcp_variant column to the protocol axis (probe writes
    # tcp_variant as protocol name when --udp is set; Lima sweep writes
    # PROTOCOL directly into tcp_variant). We treat tcp_variant as protocol.
    df["protocol"] = df["tcp_variant"]
    rows = []
    grouped = df.groupby(
        ["implementation", "protocol", "N", "M", "rng_run"], as_index=False)
    for keys, sub in grouped:
        impl, proto, n, m, rng = keys
        tot = float(sub["goodput_mbps"].sum())
        a = float(sub.loc[sub["host"] == "A", "goodput_mbps"].sum())
        share_A = (a / tot) if tot > 0 else float("nan")
        rows.append({"implementation": impl, "protocol": proto,
                     "N": int(n), "M": int(m), "rng_run": rng,
                     "share_A": share_A, "total_mbps": tot})
    per_replica = pd.DataFrame(rows)
    cells = per_replica.groupby(
        ["implementation", "protocol", "N", "M"], as_index=False).agg(
        share_A_mean=("share_A", "mean"),
        share_A_std=("share_A", "std"),
        total_mbps_mean=("total_mbps", "mean"))
    return cells


def main() -> int:
    # Backwards-compatible behaviour: when invoked with no args, regenerate
    # the canonical-sweep (sweep-cells.csv) 5-figure pack.
    import sys
    if len(sys.argv) >= 2 and sys.argv[1] == "--mode=protocols":
        perflow_csv = OUT_DIR / "phase-1-5-perflow.csv"
        cells_csv = OUT_DIR / "phase-1-5-cells.csv"
        cells = aggregate_protocol_cells(perflow_csv)
        cells.to_csv(cells_csv, index=False)
        print(f"Wrote {cells_csv} ({len(cells)} cells).")
        plot_protocol_robustness(cells_csv, "cake-host-fairness-protocols")
        print("Wrote protocol-robustness figure (svg + pdf) to:", OUT_DIR)
        return 0

    cells = pd.read_csv(OUT_DIR / "sweep-cells.csv")
    cells["strategy"] = cells["strategy"].fillna("none")
    plot_headline_line(cells)
    plot_headline_heatmap(cells)
    plot_symmetric(cells)
    plot_mirror(cells)
    plot_f_vs_n(cells)
    print("Wrote 5 figures (svg + pdf) to:", OUT_DIR)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
