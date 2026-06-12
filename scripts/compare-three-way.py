#!/usr/bin/env python3
"""Three-way (and four-way) comparison plotter for DiffServ4NS.

Produces overlay figures across simulator variants:
  - ns-2.29 (2001 original, src/ns-2.29/)
  - ns-2.35 (2026 port,    src/ns-2.35/)
  - ns-3    (2026 port,    src/ns-3/)

For Scenario 2 the ns-2.35 port carries two sweeps (WebTraf and bulk-TCP
HTTP models); S2 caPL/boPL plots overlay all four series so the
traffic-model dimension is visible and attributable.

Output figures land under handbook/figures/08-three-way/ and feed
the three-way comparison handbook section.

Driven by a `(scenario, metric) -> plot_fn` registry:
    python3 scripts/compare-three-way.py --all
    python3 scripts/compare-three-way.py --scenario s1 --metric owd
    python3 scripts/compare-three-way.py --list-metrics
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    sys.stderr.write("matplotlib + numpy required: pip install matplotlib numpy\n")
    sys.exit(2)


# ---------------------------------------------------------------------------
# Simulator palette + label conventions (single source of truth)
# ---------------------------------------------------------------------------

PALETTE = {
    "ns-2.29":          "#666666",
    "ns-2.35":          "#cc6600",
    "ns-2.35-webtraf":  "#cc6600",
    "ns-2.35-bulktcp":  "#e8a36b",
    "ns-3":             "#1f77b4",
}

LABELS = {
    "ns-2.29":         "ns-2.29 (2001 original)",
    "ns-2.35":         "ns-2.35 (2026 port)",
    "ns-2.35-webtraf": "ns-2.35 WebTraf",
    "ns-2.35-bulktcp": "ns-2.35 bulk-TCP",
    "ns-3":            "ns-3 (2026 port)",
}

# Real TF-TANT hardware reference for PQ 512 B EF (Andreozzi 2001, Fig A.5/A.6)
TF_TANT_PQ_512_OWD_MS = 17.0


# ---------------------------------------------------------------------------
# Trace loaders (single source of truth — mirror ns229-vs-ns235-compare.py
# and ns235-vs-ns3-compare.py formats).
# ---------------------------------------------------------------------------

def load_xy(path):
    """Two-column trace: (time, value)."""
    t, v = [], []
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) == 2:
                try:
                    t.append(float(parts[0]))
                    v.append(float(parts[1]))
                except ValueError:
                    pass
    return t, v


def load_ncol(path, ncols):
    """time-series + (ncols-1) value columns."""
    t = []
    cols = [[] for _ in range(ncols - 1)]
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) == ncols:
                try:
                    t.append(float(parts[0]))
                    for i in range(ncols - 1):
                        cols[i].append(float(parts[i + 1]))
                except ValueError:
                    pass
    return t, cols


def stats_of(values, window=None):
    if window is not None:
        lo, hi = window
    else:
        lo, hi = None, None

    if lo is not None and isinstance(values, tuple):
        t, v = values
        vals = [vv for tt, vv in zip(t, v) if lo <= tt <= hi]
    else:
        vals = values if not isinstance(values, tuple) else values[1]

    n = len(vals)
    if n == 0:
        return dict(mean=0.0, std=0.0, p50=0.0, p95=0.0, n=0)
    s = sorted(vals)

    def pct(p):
        idx = min(int(p / 100.0 * n), n - 1)
        return s[idx]

    return dict(
        mean=statistics.mean(vals),
        std=statistics.pstdev(vals) if n > 1 else 0.0,
        p50=pct(50),
        p95=pct(95),
        n=n,
    )


# ---------------------------------------------------------------------------
# run.log parser — shared with scripts/scenario2-table44.py
# ---------------------------------------------------------------------------

STATS_LINE_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+([\d.]+)%\s+([\d.]+)%\s+([\d.]+)%")
DSCP_TO_DP = {10: 0, 12: 1, 14: 2}


def parse_run_log_dp(path):
    """Return {dp: {'caPL': %, 'boPL': %}} from end-of-sim Packets Statistics.

    run.log holds multiple blocks; the final match wins (end-of-simulation).
    """
    latest = {}
    with open(path) as f:
        for line in f:
            m = STATS_LINE_RE.match(line)
            if m:
                latest[int(m.group(1))] = {
                    "tx":     float(m.group(3)),
                    "ldrops": float(m.group(4)),
                    "edrops": float(m.group(5)),
                }
    dp_metrics = {}
    for dscp, dp in DSCP_TO_DP.items():
        if dscp in latest:
            dp_metrics[dp] = {
                "caPL": latest[dscp]["edrops"],
                "boPL": latest[dscp]["ldrops"],
            }
    return dp_metrics


# ---------------------------------------------------------------------------
# Path registry — per scenario, per simulator, what file to read.
# ---------------------------------------------------------------------------

# ns3_root is set by main() when --ns3-root is provided.  All other keys
# resolve against repo_root; the "ns-3" key resolves against ns3_root.
_ns3_root: "Path | None" = None


def _sim_root(sim: str, repo_root: Path) -> Path:
    """Return the root directory for *sim*'s output tree."""
    if sim == "ns-3" and _ns3_root is not None:
        return _ns3_root
    return repo_root


# Scenario 1 — relative to repo root
S1_PATHS = {
    "ns-2.29": "output/ns2-29/example-1",
    "ns-2.35": "output/ns2-35/example-1",
    "ns-3":    "output/ns3/example-1/PQ-0512",
}

# Scenario 2 — sweep dirs (per set)
S2_PATHS = {
    "ns-2.29":         "output/ns2/example-2-fullscale",
    "ns-2.35-webtraf": "output/ns2-35/example-2-fullscale",
    "ns-2.35-bulktcp": "output/ns2-35/example-2-fullscale-bulktcp",
    "ns-3":            "output/ns3/example-2-fullscale-n400",
}

# Scenario 3 — fullscale directories
S3_PATHS = {
    "ns-2.29": "output/ns2-29/example-3-fullscale",
    "ns-2.35": "output/ns2-35/example-3-fullscale",
    "ns-3":    "output/ns3/example-3-fullscale",
}

# S3 steady-state window
S3_T_START = 1000.0
S3_T_END = 5000.0


# ---------------------------------------------------------------------------
# Figure helpers — unified save + summary print
# ---------------------------------------------------------------------------

def _save(fig, out_dir, stem):
    out_dir.mkdir(parents=True, exist_ok=True)
    svg = out_dir / f"{stem}.svg"
    pdf = out_dir / f"{stem}.pdf"
    fig.tight_layout()
    fig.savefig(svg)
    fig.savefig(pdf)
    plt.close(fig)
    print(f"  wrote {svg}")
    print(f"  wrote {pdf}")


def _print_summary(name, summary, unit=""):
    if not summary:
        return
    for sim, s in summary.items():
        print(f"  {sim:18s} mean={s['mean']:.3f}{unit}  "
              f"std={s['std']:.3f}  p50={s['p50']:.3f}  "
              f"p95={s['p95']:.3f}  n={s['n']}")


# ---------------------------------------------------------------------------
# Scenario 1 plots
# ---------------------------------------------------------------------------

def fig_s1_owd(repo_root: Path, out_dir: Path) -> dict:
    """S1 EF OWD at PQ scheduler (three-way overlay + TF-TANT hardware line)."""
    series = {}
    for sim, base in S1_PATHS.items():
        p = _sim_root(sim, repo_root) / base / "OWD.tr"
        if p.exists():
            series[sim] = load_xy(str(p))
    if not series:
        return {}

    fig, ax = plt.subplots(figsize=(8.0, 4.5))
    summary = {}
    for sim, (t, v) in series.items():
        ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim], linewidth=1.4, alpha=0.85)
        summary[sim] = stats_of((t, v))
    ax.axhline(TF_TANT_PQ_512_OWD_MS, color="#cc0000", linestyle="--", linewidth=1.1,
               alpha=0.7, label=f"real TF-TANT hardware (~{TF_TANT_PQ_512_OWD_MS:.0f} ms)")
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel("EF one-way delay (ms)")
    ax.set_title("Scenario 1 — EF OWD at PQ scheduler, 512 B packets")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right", fontsize=9)
    _save(fig, out_dir, "s1-owd-pq")
    return summary


def fig_s1_ipdv(repo_root: Path, out_dir: Path) -> dict:
    """S1 EF IPDV at PQ scheduler (three-way overlay)."""
    series = {}
    for sim, base in S1_PATHS.items():
        p = _sim_root(sim, repo_root) / base / "IPDV.tr"
        if p.exists():
            series[sim] = load_xy(str(p))
    if not series:
        return {}

    fig, ax = plt.subplots(figsize=(8.0, 4.5))
    summary = {}
    for sim, (t, v) in series.items():
        ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim], linewidth=1.4, alpha=0.85)
        summary[sim] = stats_of((t, v))
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel("EF inter-packet-delay variation (ms)")
    ax.set_title("Scenario 1 — EF IPDV at PQ scheduler, 512 B packets")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    _save(fig, out_dir, "s1-ipdv-pq")
    return summary


def _fig_s1_queue(repo_root, out_dir, which):
    """EF or BE queue-length overlay."""
    fname = {"ef": "EFQueueLen.tr", "be": "BEQueueLen.tr"}[which]
    title = {"ef": "EF", "be": "BE"}[which]
    series = {}
    for sim, base in S1_PATHS.items():
        p = _sim_root(sim, repo_root) / base / fname
        if p.exists():
            series[sim] = load_xy(str(p))
    if not series:
        return {}

    fig, ax = plt.subplots(figsize=(8.0, 4.5))
    summary = {}
    for sim, (t, v) in series.items():
        ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim], linewidth=1.0, alpha=0.7)
        summary[sim] = stats_of((t, v))
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel(f"{title} queue length (packets)")
    ax.set_title(f"Scenario 1 — {title} queue length, 512 B packets")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    _save(fig, out_dir, f"s1-{which}-queuelen")
    return summary


def fig_s1_ef_queuelen(repo_root, out_dir):
    return _fig_s1_queue(repo_root, out_dir, "ef")


def fig_s1_be_queuelen(repo_root, out_dir):
    return _fig_s1_queue(repo_root, out_dir, "be")


def _fig_s1_servicerate(repo_root, out_dir, klass):
    """EF or BE service rate overlay."""
    col = {"ef": 0, "be": 1}[klass]
    series = {}
    for sim, base in S1_PATHS.items():
        p = _sim_root(sim, repo_root) / base / "ServiceRate.tr"
        if p.exists():
            t, cols = load_ncol(str(p), 3)
            if col < len(cols):
                series[sim] = (t, cols[col])
    if not series:
        return {}

    fig, ax = plt.subplots(figsize=(8.0, 4.5))
    summary = {}
    for sim, (t, v) in series.items():
        ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim], linewidth=1.2, alpha=0.85)
        summary[sim] = stats_of((t, v))
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel(f"{klass.upper()} service rate (kbps)")
    ax.set_title(f"Scenario 1 — {klass.upper()} service rate, 512 B packets")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    _save(fig, out_dir, f"s1-servicerate-{klass}")
    return summary


def fig_s1_servicerate_ef(repo_root, out_dir):
    return _fig_s1_servicerate(repo_root, out_dir, "ef")


def fig_s1_servicerate_be(repo_root, out_dir):
    return _fig_s1_servicerate(repo_root, out_dir, "be")


# ---------------------------------------------------------------------------
# Scenario 2 plots
# ---------------------------------------------------------------------------

def _collect_s2_dp(repo_root, metric):
    """Gather {sim: {set: {dp: value%}}} across 4 sims × 6 sets × 3 DPs."""
    out = {}
    for sim, base in S2_PATHS.items():
        sim_data = {}
        for s in range(1, 7):
            log_path = _sim_root(sim, repo_root) / base / f"set-{s}" / "run.log"
            if not log_path.exists():
                continue
            dp = parse_run_log_dp(str(log_path))
            if dp:
                sim_data[s] = {d: v[metric] for d, v in dp.items()}
        if sim_data:
            out[sim] = sim_data
    return out


def _fig_s2_dp_metric(repo_root, out_dir, metric, ylabel, stem):
    """4-line plot across 6 WRED sets, one subplot per DP."""
    data = _collect_s2_dp(repo_root, metric)
    if not data:
        return {}
    fig, axs = plt.subplots(1, 3, figsize=(12.0, 4.0), sharey=True)
    dps = [0, 1, 2]
    for i, dp in enumerate(dps):
        ax = axs[i]
        for sim in ["ns-2.29", "ns-2.35-bulktcp", "ns-2.35-webtraf", "ns-3"]:
            if sim not in data:
                continue
            sets = sorted(data[sim].keys())
            ys = [data[sim][s].get(dp, 0.0) for s in sets]
            ax.plot(sets, ys, marker="o", label=LABELS[sim],
                    color=PALETTE[sim], linewidth=1.4, alpha=0.9)
        ax.set_title(f"DP{dp}")
        ax.set_xlabel("WRED set")
        ax.grid(True, alpha=0.3)
        if i == 0:
            ax.set_ylabel(ylabel)
    axs[-1].legend(loc="best", fontsize=8)
    fig.suptitle(f"Scenario 2 — {ylabel.lower()} per drop-precedence, 4-way comparison")
    _save(fig, out_dir, stem)
    # flatten summary: {sim: {"DP0-setN": value, ...}}
    flat = {sim: {f"DP{dp}-set{s}": data[sim][s][dp]
                  for s in data[sim] for dp in data[sim][s]}
            for sim in data}
    return flat


def fig_s2_capl(repo_root, out_dir):
    return _fig_s2_dp_metric(repo_root, out_dir, "caPL",
                             "caPL (early-drop %)", "s2-capl-per-dp")


def fig_s2_bopl(repo_root, out_dir):
    return _fig_s2_dp_metric(repo_root, out_dir, "boPL",
                             "boPL (tail-drop %)", "s2-bopl-per-dp")


def _fig_s2_queuelen(repo_root, out_dir, qi, set_num=1):
    """Queue-length overlay for q_i across simulators (representative set)."""
    series = {}
    for sim, base in S2_PATHS.items():
        p = _sim_root(sim, repo_root) / base / f"set-{set_num}" / "QueueLen.tr"
        if p.exists():
            t, cols = load_ncol(str(p), 3)
            if qi < len(cols):
                series[sim] = (t, cols[qi])
    if not series:
        return {}
    fig, ax = plt.subplots(figsize=(9.0, 4.2))
    summary = {}
    for sim, (t, v) in series.items():
        ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim],
                linewidth=0.9, alpha=0.55)
        summary[sim] = stats_of((t, v))
    qname = {0: "Q0 (AF)", 1: "Q1 (BE)"}[qi]
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel(f"{qname} queue length (packets)")
    ax.set_title(f"Scenario 2 — {qname} queue length, WRED set {set_num}")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=8)
    _save(fig, out_dir, f"s2-queuelen-q{qi}")
    return summary


def fig_s2_queuelen_q0(repo_root, out_dir):
    return _fig_s2_queuelen(repo_root, out_dir, 0)


def fig_s2_queuelen_q1(repo_root, out_dir):
    return _fig_s2_queuelen(repo_root, out_dir, 1)


def fig_s2_servicerate_by_dscp(repo_root, out_dir, set_num=1):
    """5-subplot panel: one timeseries per DSCP (10/12/14/0/50), four sims overlaid."""
    dscp_labels = ["DSCP 10 (AF11-DP0)", "DSCP 12 (AF11-DP1)",
                   "DSCP 14 (AF11-DP2)", "DSCP 0 (BE)",
                   "DSCP 50 (BE policed)"]

    series_per_col = {col: {} for col in range(5)}
    for sim, base in S2_PATHS.items():
        p = _sim_root(sim, repo_root) / base / f"set-{set_num}" / "ServiceRate.tr"
        if not p.exists():
            continue
        t, cols = load_ncol(str(p), 6)
        if not t:
            # ns-3 S2 ServiceRate is queue-aggregate (3 cols), not per-DSCP.
            # Skip so the panel shows only the simulators whose trace
            # layout matches the per-DSCP semantics.
            continue
        for col in range(min(5, len(cols))):
            series_per_col[col][sim] = (t, cols[col])

    if not any(series_per_col.values()):
        return {}

    fig, axs = plt.subplots(5, 1, figsize=(9.0, 11.0), sharex=True)
    summary = {}
    for col in range(5):
        ax = axs[col]
        summary[dscp_labels[col]] = {}
        for sim, (t, v) in series_per_col[col].items():
            ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim],
                    linewidth=0.9, alpha=0.8)
            summary[dscp_labels[col]][sim] = stats_of((t, v))
        ax.set_ylabel("kbps")
        ax.set_title(dscp_labels[col], fontsize=10)
        ax.grid(True, alpha=0.3)
        if col == 0:
            ax.legend(loc="best", fontsize=8)
    axs[-1].set_xlabel("Simulation time (s)")
    fig.suptitle(f"Scenario 2 — per-DSCP service rate, WRED set {set_num}")
    _save(fig, out_dir, "s2-servicerate-by-dscp")
    return summary


# ---------------------------------------------------------------------------
# Scenario 3 plots
# ---------------------------------------------------------------------------

def _collect_s3_classrate(repo_root, filename, ncols):
    """Load classX.tr style. ncols = total columns (time + value columns).

    ServiceRate.tr has 6 columns (time + 5 classes: Premium/Gold/Silver/Bronze/BE).
    ClassRate.tr has 7 columns (time + 6 values: Premium + 4 AF tiers split by
    aggregate vs. rate, + BE — the first 5 value columns map to the same class
    ordering, so callers use cols[0:5]).
    """
    out = {}
    for sim, base in S3_PATHS.items():
        p = _sim_root(sim, repo_root) / base / filename
        if p.exists():
            t, cols = load_ncol(str(p), ncols)
            if t:
                out[sim] = (t, cols)
    return out


def fig_s3_servicerate_panel(repo_root, out_dir):
    """Per-class mean service rates over steady-state window (bar chart three-way)."""
    raw = _collect_s3_classrate(repo_root, "ServiceRate.tr", 6)
    if not raw:
        return {}
    classes = ("Premium", "Gold", "Silver", "Bronze", "BE")
    window = (S3_T_START, S3_T_END)

    means = {}
    for sim, (t, cols) in raw.items():
        row = []
        for ci, _ in enumerate(classes):
            vals = [v for tt, v in zip(t, cols[ci]) if window[0] <= tt <= window[1]]
            row.append(statistics.mean(vals) if vals else 0.0)
        means[sim] = row

    fig, ax = plt.subplots(figsize=(9.0, 4.5))
    x = np.arange(len(classes))
    nsims = len(means)
    width = 0.8 / max(nsims, 1)
    order = [s for s in ("ns-2.29", "ns-2.35", "ns-3") if s in means]
    for i, sim in enumerate(order):
        off = (i - (nsims - 1) / 2.0) * width
        ax.bar(x + off, means[sim], width, label=LABELS[sim],
               color=PALETTE[sim], alpha=0.9)
    ax.set_xticks(x)
    ax.set_xticklabels(classes)
    ax.set_ylabel(f"Mean service rate (kbps), t = {window[0]:.0f}–{window[1]:.0f} s")
    ax.set_title("Scenario 3 — per-class service rates (three-way)")
    ax.grid(True, alpha=0.3, axis="y")
    ax.legend(loc="best", fontsize=9)
    _save(fig, out_dir, "s3-service-rates")
    return {sim: dict(zip(classes, row)) for sim, row in means.items()}


def _fig_s3_xy(repo_root, out_dir, fname, ylabel, stem):
    """Generic S3 two-column timeseries overlay (OWD, IPDV)."""
    series = {}
    for sim, base in S3_PATHS.items():
        p = _sim_root(sim, repo_root) / base / fname
        if p.exists():
            series[sim] = load_xy(str(p))
    if not series:
        return {}
    fig, ax = plt.subplots(figsize=(9.0, 4.5))
    summary = {}
    for sim, (t, v) in series.items():
        # warmup skip
        keep = [(tt, vv) for tt, vv in zip(t, v) if tt >= 100.0]
        if not keep:
            continue
        tt, vv = zip(*keep)
        ax.plot(tt, vv, label=LABELS[sim], color=PALETTE[sim],
                linewidth=0.8, alpha=0.7)
        summary[sim] = stats_of((list(tt), list(vv)))
    ax.set_xlabel("Simulation time (s)")
    ax.set_ylabel(ylabel)
    ax.set_title(f"Scenario 3 — {ylabel.lower()} (fullscale, warm-up t<100 skipped)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best", fontsize=9)
    _save(fig, out_dir, stem)
    return summary


def fig_s3_owd(repo_root, out_dir):
    return _fig_s3_xy(repo_root, out_dir, "OWD.tr", "One-way delay (ms)", "s3-owd")


def fig_s3_ipdv(repo_root, out_dir):
    return _fig_s3_xy(repo_root, out_dir, "IPDV.tr", "IPDV (ms)", "s3-ipdv")


def fig_s3_classrate(repo_root, out_dir):
    """5-subplot panel: one per thesis class, three-way overlay.

    scenario-3.tcl emits ClassRate.tr with per-application columns
    (voip / stream / telnet / ftp / http / bg), whereas ServiceRate.tr
    emits per-DSCP-class (Premium / Gold / Silver / Bronze / BE).
    The application-to-class mapping per thesis §4.3 Table 4.5:
      Premium = VoIP
      Gold    = RealAudio (stream)
      Silver  = Telnet + FTP (combined)
      Bronze  = HTTP
      BE      = Background CBR
    We fold Telnet + FTP into Silver to keep the five-class panel aligned
    with ServiceRate's semantics.
    """
    raw = _collect_s3_classrate(repo_root, "ClassRate.tr", 7)
    if not raw:
        return {}
    classes = ("Premium (VoIP)", "Gold (Stream)",
               "Silver (Telnet+FTP)", "Bronze (HTTP)", "BE (Background)")
    # (col index into cols, or tuple of indices summed)
    col_spec = (0, 1, (2, 3), 4, 5)

    fig, axs = plt.subplots(5, 1, figsize=(9.0, 11.0), sharex=True)
    summary = {c: {} for c in classes}
    for ci, (cname, spec) in enumerate(zip(classes, col_spec)):
        ax = axs[ci]
        for sim, (t, cols) in raw.items():
            if isinstance(spec, tuple):
                v = [sum(parts) for parts in zip(*[cols[i] for i in spec])]
            else:
                v = cols[spec]
            ax.plot(t, v, label=LABELS[sim], color=PALETTE[sim],
                    linewidth=0.8, alpha=0.8)
            summary[cname][sim] = stats_of((t, v),
                                           window=(S3_T_START, S3_T_END))
        ax.set_ylabel("kbps")
        ax.set_title(cname, fontsize=10)
        ax.grid(True, alpha=0.3)
        if ci == 0:
            ax.legend(loc="best", fontsize=8)
    axs[-1].set_xlabel("Simulation time (s)")
    fig.suptitle("Scenario 3 — per-class source rate timeseries (three-way)")
    _save(fig, out_dir, "s3-classrate-per-class")
    return summary


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

REGISTRY = {
    ("s1", "owd"):             fig_s1_owd,
    ("s1", "ipdv"):            fig_s1_ipdv,
    ("s1", "ef-queuelen"):     fig_s1_ef_queuelen,
    ("s1", "be-queuelen"):     fig_s1_be_queuelen,
    ("s1", "servicerate-ef"):  fig_s1_servicerate_ef,
    ("s1", "servicerate-be"):  fig_s1_servicerate_be,

    ("s2", "capl"):                fig_s2_capl,
    ("s2", "bopl"):                fig_s2_bopl,
    ("s2", "queuelen-q0"):         fig_s2_queuelen_q0,
    ("s2", "queuelen-q1"):         fig_s2_queuelen_q1,
    ("s2", "servicerate-by-dscp"): fig_s2_servicerate_by_dscp,

    ("s3", "service-rates"):  fig_s3_servicerate_panel,
    ("s3", "owd"):            fig_s3_owd,
    ("s3", "ipdv"):           fig_s3_ipdv,
    ("s3", "classrate"):      fig_s3_classrate,
}


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo-root", default=".", help="Repo root (default: cwd).")
    p.add_argument("--ns3-root", default=None,
                   help="Root for ns-3 output trees.  When provided, paths for "
                        "the \"ns-3\" simulator key resolve against this directory "
                        "instead of --repo-root.  All other keys (ns-2.29, "
                        "ns-2.35, legacy ns2) continue to resolve against "
                        "--repo-root.  Omit to preserve backward-compatible "
                        "behaviour (ns3_root = repo_root).")
    p.add_argument("--out-dir", default="handbook/figures/08-three-way",
                   help="Output directory for figures.")
    p.add_argument("--all", action="store_true",
                   help="Run every (scenario, metric) in the registry.")
    p.add_argument("--scenario", choices=["s1", "s2", "s3"],
                   help="Scenario selector (use with --metric).")
    p.add_argument("--metric", help="Metric selector (e.g. owd, ipdv, capl). "
                                    "Use --list-metrics to see available options.")
    p.add_argument("--list-metrics", action="store_true",
                   help="Print every (scenario, metric) in the registry and exit.")
    args = p.parse_args()

    if args.list_metrics:
        by_scenario = {}
        for (sc, me), _ in REGISTRY.items():
            by_scenario.setdefault(sc, []).append(me)
        for sc in sorted(by_scenario):
            print(f"{sc}:")
            for me in by_scenario[sc]:
                print(f"  --scenario {sc} --metric {me}")
        return

    repo_root = Path(args.repo_root).resolve()
    global _ns3_root
    if args.ns3_root is not None:
        _ns3_root = Path(args.ns3_root).resolve()
    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        # In split-root mode the figures belong with the ns-3 side, not
        # inside the ns-2 baselines tree.
        out_dir = (_ns3_root if _ns3_root is not None else repo_root) / out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Repo root: {repo_root}")
    if _ns3_root is not None and _ns3_root != repo_root:
        print(f"ns3 root:  {_ns3_root}")
    print(f"Output:    {out_dir}\n")

    if args.all:
        keys = list(REGISTRY.keys())
    elif args.scenario and args.metric:
        key = (args.scenario, args.metric)
        if key not in REGISTRY:
            sys.stderr.write(f"Unknown (scenario, metric): {key}\n"
                             f"Use --list-metrics for options.\n")
            sys.exit(2)
        keys = [key]
    else:
        # Default: run every registry entry.
        keys = list(REGISTRY.keys())

    for sc, me in keys:
        fn = REGISTRY[(sc, me)]
        print(f"[{sc}:{me}]  {fn.__name__}")
        summary = fn(repo_root, out_dir)
        if summary:
            # heuristic — flat vs nested summary
            first = next(iter(summary.values()))
            if isinstance(first, dict) and "mean" in first:
                _print_summary(fn.__name__, summary)
            else:
                for k, v in summary.items():
                    print(f"  {k}: {v}")
        print()

    # Legacy deferred note is obsolete now that S2 caPL/boPL ship as 4-way
    # plots. Remove if present.
    legacy = out_dir / "s2-caPL-per-DP.DEFERRED.txt"
    if legacy.exists():
        legacy.unlink()
        print(f"  removed legacy marker {legacy}")


if __name__ == "__main__":
    main()
