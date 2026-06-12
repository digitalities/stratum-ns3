"""Stratum plot-recipe module.

Loads per-recipe configuration from scripts/plot-recipe-config.yaml,
dispatches to a plot-type renderer, emits SVG (default) or PNG.

Plot types live in PLOT_TYPES dict; calibration provenance is declared
per-recipe in the YAML config.
"""
from __future__ import annotations

import argparse
import math
import re
import sys
import textwrap
from pathlib import Path

import numpy as np
import pandas as pd
import yaml

CONFIG_PATH = Path(__file__).resolve().parent / "plot-recipe-config.yaml"

KNOWN_PLOT_TYPES = {
    "owd-time-series",
    "per-flow-bar",
    "steady-state-bar",
    "throughput-stacked",
    "ecdf",
    "fairness-jain",
    "meter-colour-bar",
    "tin-rate-heatmap",
    "aqm-envelope",
}

KNOWN_ARMS_MODES = {"overlay-all", "facet-grid", "filter-required"}

CALIBRATION_SOURCE_PREFIXES = ("handbook-§", "rfc-", "paper", "hardcoded", "measured")


# ---- Plot-type dispatch -------------------------------------------------

# Plot-type renderers register here at import time.
PLOT_TYPES: dict[str, callable] = {}


# ---- CLI entry ----------------------------------------------------------

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="plot-recipe",
        description="Render Stratum user-guide recipe figures.",
    )
    parser.add_argument("recipe", nargs="?", help="Recipe name (see --list).")
    parser.add_argument("--format", choices=["svg", "png"], default="svg",
                        help="Output format (default: svg).")
    parser.add_argument("--out", type=Path, default=None,
                        help="Output directory (default: handbook/figures/<recipe>/).")
    parser.add_argument("--arm", default=None,
                        help="Filter to a specific arm under output_glob.")
    parser.add_argument("--list", action="store_true",
                        help="Enumerate configured recipes.")
    parser.add_argument("--validate", action="store_true",
                        help="Schema-check the YAML config.")
    parser.add_argument("--list-arms", metavar="RECIPE", default=None,
                        help="Enumerate arms on disk for RECIPE.")

    args = parser.parse_args(argv)

    # Load config (always needed)
    try:
        config = load_config(CONFIG_PATH)
    except FileNotFoundError:
        print(f"plot-recipe: config not found: {CONFIG_PATH}", file=sys.stderr)
        return 4
    except yaml.YAMLError as e:
        print(f"plot-recipe: malformed YAML: {e}", file=sys.stderr)
        return 4

    if args.list:
        return cmd_list(config)
    if args.validate:
        return cmd_validate(config)
    if args.list_arms:
        return cmd_list_arms(config, args.list_arms)

    if not args.recipe:
        parser.print_help()
        return 1

    return cmd_render(config, args)


# ---- Subcommands --------------------------------------------------------

def load_config(path: Path) -> dict:
    with path.open() as f:
        return yaml.safe_load(f)


def cmd_list(config: dict) -> int:
    """Enumerate configured recipes."""
    for name in sorted(config.get("recipes", {}).keys()):
        print(name)
    return 0


def cmd_validate(config: dict) -> int:
    """Schema-check the YAML config. Returns 0 on success, 4 on failure."""
    if "recipes" not in config:
        print("plot-recipe: missing top-level 'recipes' key", file=sys.stderr)
        return 4

    errors: list[str] = []

    for name, entry in config["recipes"].items():
        # Required top-level fields
        for required_field in ("example_cmd", "output_glob", "arms", "plots"):
            if required_field not in entry:
                errors.append(f"{name}: missing '{required_field}'")

        if "arms" in entry and entry["arms"] not in KNOWN_ARMS_MODES:
            errors.append(
                f"{name}: arms='{entry['arms']}' not in {sorted(KNOWN_ARMS_MODES)}"
            )

        for i, plot in enumerate(entry.get("plots", [])):
            ctx = f"{name}.plots[{i}]"

            if "type" not in plot:
                errors.append(f"{ctx}: missing 'type'")
            elif plot["type"] not in KNOWN_PLOT_TYPES:
                errors.append(
                    f"{ctx}: type='{plot['type']}' not in {sorted(KNOWN_PLOT_TYPES)}"
                )

            for required_field in ("csv_glob", "x_col", "title", "xlabel", "ylabel", "calibration"):
                if required_field not in plot:
                    errors.append(f"{ctx}: missing '{required_field}'")

            calibration = plot.get("calibration", {})
            if "source" not in calibration:
                errors.append(f"{ctx}.calibration: missing 'source'")
            elif not any(calibration["source"].startswith(p) for p in CALIBRATION_SOURCE_PREFIXES):
                errors.append(
                    f"{ctx}.calibration.source='{calibration['source']}' "
                    f"does not start with one of {CALIBRATION_SOURCE_PREFIXES}"
                )
            if "notes" not in calibration:
                errors.append(f"{ctx}.calibration: missing 'notes'")

    if errors:
        for err in errors:
            print(f"plot-recipe: {err}", file=sys.stderr)
        return 4

    n = len(config["recipes"])
    print(f"plot-recipe: {n} recipes validated OK")
    return 0


def resolve_arms(output_glob: str, arm_filter: str | None = None) -> list[Path]:
    """Enumerate arm directories matching output_glob.

    output_glob is relative to the worktree root. Returns sorted list.
    """
    matches = sorted(Path(".").glob(output_glob))
    # The glob may match files; filter to directories.
    arms = [m for m in matches if m.is_dir()]
    if arm_filter:
        arms = [a for a in arms if a.name == arm_filter or str(a).endswith(arm_filter)]
    return arms


def cmd_render(config: dict, args: argparse.Namespace) -> int:
    """Render a recipe's plots."""
    recipe = args.recipe
    if recipe not in config.get("recipes", {}):
        print(f"plot-recipe: unknown recipe '{recipe}' (see --list)", file=sys.stderr)
        return 1

    entry = config["recipes"][recipe]
    plots = entry.get("plots", [])
    if not plots:
        print(f"plot-recipe: recipe '{recipe}' has no plots configured yet", file=sys.stderr)
        return 1

    arms = resolve_arms(entry["output_glob"], args.arm)
    if not arms:
        example_cmd = entry["example_cmd"]
        print(
            f"plot-recipe: no output arms found at {entry['output_glob']}\n"
            f"  Run the recipe first: ./ns3 run \"{example_cmd}\"",
            file=sys.stderr,
        )
        return 2

    # Output directory: --out or handbook/figures/<recipe>/
    out_dir = args.out if args.out else Path("handbook/figures") / recipe
    out_dir.mkdir(parents=True, exist_ok=True)

    import pandas as pd
    for plot_cfg in plots:
        plot_type = plot_cfg["type"]
        if plot_type not in PLOT_TYPES:
            print(f"plot-recipe: plot type '{plot_type}' not implemented", file=sys.stderr)
            return 3
        renderer = PLOT_TYPES[plot_type]

        # Load CSVs from all arms; concatenate with an 'arm' column for series grouping.
        # csv_opts: optional dict forwarded to pd.read_csv (e.g. sep, names, header).
        csv_opts: dict = plot_cfg.get("csv_opts", {})
        dfs = []
        for arm_path in arms:
            csvs = sorted(arm_path.glob(plot_cfg["csv_glob"]))
            for csv_path in csvs:
                df = pd.read_csv(csv_path, **csv_opts)
                df["__arm__"] = arm_path.name
                # __file__ carries the CSV stem (filename without extension) so that
                # recipes matching multiple files per arm (e.g. OWD-ef.tr + OWD-be.tr)
                # can use series_col: __file__ to get one series per file.
                df["__file__"] = csv_path.stem
                dfs.append(df)
        if not dfs:
            print(
                f"plot-recipe: csv_glob '{plot_cfg['csv_glob']}' matched no files "
                f"under {[str(a) for a in arms]}",
                file=sys.stderr,
            )
            return 3
        df = pd.concat(dfs, ignore_index=True)

        # Series column resolution. Three cases:
        #   - multi-arm with YAML series_col → composite "arm/series" via synthetic column
        #   - multi-arm without YAML series_col → group by arm
        #   - single arm → honour YAML series_col (or default __arm__)
        yaml_series = plot_cfg.get("series_col")
        # Regex that strips the rate/latency parameter encoding that fetch scripts
        # embed in arm-directory names (e.g. "-100mbps-40ms").  This produces
        # compact legend labels such as "triple/A" instead of "triple-100mbps-40ms/A".
        _ARM_SUFFIX_RE = re.compile(r"-\d+[mk]?bps-\d+ms$", re.IGNORECASE)
        if len(arms) > 1:
            if yaml_series and yaml_series != "__arm__":
                arm_labels = df["__arm__"].astype(str).str.replace(
                    _ARM_SUFFIX_RE, "", regex=True
                )
                df["__series__"] = arm_labels + "/" + df[yaml_series].astype(str)
                series_col = "__series__"
            else:
                series_col = "__arm__"
        else:
            series_col = yaml_series or "__arm__"

        ext = "svg" if args.format == "svg" else "png"
        # Multi-plot recipes (>1 entry in plots:) need distinct filenames when
        # plot entries share a type. Optional `output_name:` overrides; default
        # is plot_type. Single-plot recipes ignore output_name (back-compat).
        output = out_dir / f"{plot_cfg.get('output_name', plot_type)}.{ext}"

        # Apply time-window filter if recipe declares it (used by steady-state-bar):
        time_window = plot_cfg.get("time_window_last_n_seconds")
        if time_window is not None and "t" in df.columns:
            t_max = df["t"].max()
            df = df[df["t"] >= (t_max - time_window)]

        # Pre-aggregate per (x_col, series_col) when multiple source files contribute
        # to the same series (e.g. 4 per-flow CSVs tagged with the same host label).
        # Summing collapses per-flow values into per-series totals before plotting,
        # so the renderer sees one y-value per (t, series) rather than scattered
        # per-flow points.  Applied only when the recipe requests an explicit
        # series_col and does NOT ask for the default per-arm grouping.
        x_col_for_agg = plot_cfg.get("x_col")
        y_col_for_agg = plot_cfg.get("y_col")
        if (
            plot_cfg.get("series_col")
            and series_col not in ("__arm__", "__file__")
            and x_col_for_agg
            and y_col_for_agg
            # Skip pre-aggregation when the x/y columns are absent from the
            # raw CSV: some plot types (e.g. meter-colour-pie) accept a
            # wide-format frame and synthesise x_col/y_col internally via
            # melt.  Pre-aggregating here would KeyError on the melted names.
            and x_col_for_agg in df.columns
            and y_col_for_agg in df.columns
            and time_window is None
        ):
            df = (
                df.groupby([x_col_for_agg, series_col], as_index=False)[y_col_for_agg]
                .sum()
            )

        # Apply optional uniform downsampling per series before plotting.
        # downsample_n: N keeps at most N evenly-spaced rows per series, preserving
        # first and last points.  Reduces SVG path complexity for dense time-series
        # without distorting the visual shape of the curve.
        downsample_n = plot_cfg.get("downsample_n")
        if downsample_n and plot_cfg.get("x_col"):
            sampled = []
            for sid, grp in df.groupby(series_col, sort=False):
                grp = grp.sort_values(plot_cfg["x_col"])
                n = len(grp)
                if n > downsample_n:
                    idx = np.linspace(0, n - 1, downsample_n, dtype=int)
                    grp = grp.iloc[idx]
                sampled.append(grp)
            df = pd.concat(sampled, ignore_index=True)

        # Apply optional unit conversion to y_col before plotting.
        y_unit_convert = plot_cfg.get("y_unit_convert")
        if y_unit_convert:
            df = _apply_y_unit_convert(df, plot_cfg["y_col"], y_unit_convert)

        # Apply optional per-series y aggregation before plotting.
        # Collapses multiple per-flow rows into one aggregate point per series.
        # Supported modes: sum, mean (pandas native), cv (_compute_aggregate).
        y_aggregate = plot_cfg.get("y_aggregate")
        if y_aggregate and plot_cfg.get("y_col"):
            x_c = plot_cfg["x_col"]
            y_c = plot_cfg["y_col"]
            if y_aggregate == "sum":
                df = df.groupby(series_col, as_index=False).agg(
                    **{x_c: (x_c, "mean"), y_c: (y_c, "sum")}
                )
            elif y_aggregate == "mean":
                df = df.groupby(series_col, as_index=False).agg(
                    **{x_c: (x_c, "mean"), y_c: (y_c, "mean")}
                )
            elif y_aggregate == "cv":
                rows = []
                for sid, grp in df.groupby(series_col, sort=False):
                    cv_val = _compute_aggregate(grp, y_c, "cv")
                    x_val = float(grp[x_c].mean()) if x_c in grp.columns else float("nan")
                    rows.append({series_col: sid, x_c: x_val, y_c: cv_val})
                df = pd.DataFrame(rows)

        # Apply optional series label remapping (display names).
        series_label_map = plot_cfg.get("series_label_map", {})
        if series_label_map:
            df = df.copy()
            df[series_col] = df[series_col].astype(str).map(
                lambda s: series_label_map.get(s, s)
            )

        extra_kwargs: dict = {}
        if plot_type == "per-flow-bar":
            extra_kwargs["ylim_max_multiplier"] = plot_cfg.get("ylim_max_multiplier", 1.0)

        renderer(
            df=df,
            output=output,
            title=plot_cfg["title"],
            xlabel=plot_cfg["xlabel"],
            ylabel=plot_cfg["ylabel"],
            x_col=plot_cfg["x_col"],
            y_col=plot_cfg.get("y_col"),
            series_col=series_col,
            calibration=plot_cfg["calibration"],
            format=args.format,
            **extra_kwargs,
        )
        if args.format == "svg":
            _shrink_svg(output)
        print(f"plot-recipe: wrote {output}")

    return 0


def cmd_list_arms(config: dict, recipe: str) -> int:
    """Enumerate arms on disk for RECIPE."""
    if recipe not in config.get("recipes", {}):
        print(f"plot-recipe: unknown recipe '{recipe}'", file=sys.stderr)
        return 1
    entry = config["recipes"][recipe]
    arms = resolve_arms(entry["output_glob"])
    if not arms:
        print(f"plot-recipe: no arms on disk under {entry['output_glob']}")
        return 0
    for a in arms:
        print(a.name)
    return 0


# ---- Renderer setup -----------------------------------------------------

def _setup_matplotlib():
    """Configure matplotlib for git-diff-friendly SVG output."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams["svg.fonttype"] = "none"   # text-as-text in SVG, not paths
    plt.rcParams["svg.hashsalt"] = "stratum-plot-recipe"  # deterministic IDs
    plt.rcParams["path.simplify"] = True          # enable vector-path compaction
    plt.rcParams["path.simplify_threshold"] = 0.5  # raised from default 0.1111
    return plt


def _annotate_calibration(ax, calibration: dict) -> None:
    """Overlay calibration band + caption.

    Wraps the (source + notes) caption at ~100 chars per line so long
    notes stay inside the figure width. matplotlib's xlabel does not
    word-wrap on its own; without textwrap.fill, long captions render
    as a single line that overflows horizontally beyond the SVG canvas.
    tight_layout() (called after) accommodates the multi-line height.
    """
    notes = calibration.get("notes", "")
    source = calibration.get("source", "hardcoded")
    caption = textwrap.fill(f"Source: {source} — {notes}", width=100)
    ax.set_xlabel(ax.get_xlabel() + f"\n{caption}", fontsize=8)


# ---- Plot-type implementations ------------------------------------------

def plot_owd_time_series(
    df,
    output: Path,
    title: str,
    xlabel: str,
    ylabel: str,
    x_col: str,
    y_col: str,
    series_col: str,
    calibration: dict,
    format: str = "svg",
):
    """Per-series line plot over time. Used for OWD, RTT, queue-depth visualisations."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    for series_id, group in df.groupby(series_col):
        # Use the series identifier directly as the label; avoid prepending an
        # internal column name (names starting with '_' are silently skipped by
        # matplotlib's legend logic).
        ax.plot(group[x_col], group[y_col], label=str(series_id), linewidth=1.2)

    # Calibration band overlay (per-class ranges)
    range_by_class = calibration.get("range_by_class", {})
    for cls_name, cls_range in range_by_class.items():
        ax.axhspan(cls_range["lo"], cls_range["hi"], alpha=0.1, label=f"{cls_name} expected")

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    legend_title = "Series" if series_col == "__series__" else series_col
    ax.legend(title=legend_title, loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)

    _annotate_calibration(ax, calibration)

    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["owd-time-series"] = plot_owd_time_series


def plot_per_flow_bar(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg", ylim_max_multiplier=1.0):
    """Grouped bar chart of a metric per flow (series) across arms/configs."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    grouped = df.groupby([series_col, x_col])[y_col].sum().reset_index() if series_col != x_col else df

    # Pivot for grouped bars per series
    pivot = grouped.pivot(index=x_col, columns=series_col, values=y_col).fillna(0)
    pivot.plot(kind="bar", ax=ax, edgecolor="black", linewidth=0.5, rot=0)

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, axis="y", alpha=0.3)
    if ylim_max_multiplier != 1.0:
        ymin, ymax = ax.get_ylim()
        ax.set_ylim(ymin, ymax * ylim_max_multiplier)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["per-flow-bar"] = plot_per_flow_bar


def plot_steady_state_bar(df, output, title, xlabel, ylabel, x_col, y_col, series_col,
                          calibration, format="svg"):
    """Group by (x_col, series_col), bar-plot the mean of y_col.

    The time-window filter (last N seconds) is applied in cmd_render's
    dispatcher before this function is called.
    """
    plt = _setup_matplotlib()
    grouped = df.groupby([x_col, series_col])[y_col].mean().reset_index()
    pivot = grouped.pivot(index=x_col, columns=series_col, values=y_col).fillna(0)
    # Strip arm-suffix from x-axis tick labels so they read e.g. "flowblind" instead
    # of "flowblind-100mbps-40ms" (matches the legend label stripping applied in
    # cmd_render's composite __series__ construction).
    _arm_suffix_re = re.compile(r"-\d+[mk]?bps-\d+ms$", re.IGNORECASE)
    pivot.index = [_arm_suffix_re.sub("", str(label)) for label in pivot.index]
    fig, ax = plt.subplots(figsize=(9, 5))
    pivot.plot(kind="bar", ax=ax, width=0.7, rot=0)
    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    legend_title = "Series" if series_col == "__series__" else series_col
    # Anchor legend outside the plot area (right) so it doesn't overlap bars.
    ax.legend(title=legend_title, bbox_to_anchor=(1.02, 1.0), loc="upper left")
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format, bbox_inches="tight")
    plt.close(fig)


PLOT_TYPES["steady-state-bar"] = plot_steady_state_bar


def plot_throughput_stacked(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Stacked area chart of per-class throughput over time."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    pivot = df.pivot_table(index=x_col, columns=series_col, values=y_col, aggfunc="sum").fillna(0)
    ax.stackplot(pivot.index, pivot.T.values, labels=pivot.columns, alpha=0.6)

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.legend(loc="upper left", fontsize=8)
    ax.grid(True, alpha=0.3)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["throughput-stacked"] = plot_throughput_stacked


def plot_ecdf(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Empirical CDF of a metric, optionally split by series."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    if series_col and series_col in df.columns:
        # Use bare series_id as the legend label, not f"{series_col}={series_id}".
        # Internal series_col names like "__file__" or "__arm__" would otherwise
        # produce labels starting with "_", which matplotlib auto-hides from legends.
        for series_id, group in df.groupby(series_col):
            values = np.sort(group[x_col].values)
            ecdf = np.arange(1, len(values) + 1) / len(values)
            ax.plot(values, ecdf, label=str(series_id), linewidth=1.5)
    else:
        values = np.sort(df[x_col].values)
        ecdf = np.arange(1, len(values) + 1) / len(values)
        ax.plot(values, ecdf, linewidth=1.5)

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel or "ECDF")
    ax.set_ylim(0, 1.05)
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["ecdf"] = plot_ecdf


def plot_fairness_jain(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Jain's fairness index over time with perfect and starvation threshold references."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    if series_col and series_col in df.columns:
        for series_id, group in df.groupby(series_col):
            ax.plot(group[x_col], group[y_col], label=f"{series_col}={series_id}", linewidth=1.2)
    else:
        ax.plot(df[x_col], df[y_col], linewidth=1.2)

    ax.axhline(1.0, color="gray", linestyle=":", linewidth=0.5, label="perfect (1.0)")
    ax.axhline(0.7, color="red", linestyle=":", linewidth=0.5, label="starvation threshold (0.7)")

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel or "Jain's fairness index")
    ax.set_ylim(0, 1.1)
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["fairness-jain"] = plot_fairness_jain


def plot_meter_colour_bar(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Horizontal bar chart of meter colour distribution (green/yellow/red).

    Length is comparable across categories; wedge area is not — bars
    let readers eyeball ratios (e.g. 95:5) at a glance.  Zero-count
    categories are dropped so structurally-absent colours (e.g. yellow
    under sr-TCM, which has no excess bucket) do not draw a labelled
    empty wedge.
    """
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(6, 2.5))

    # Accept wide per-window format (columns: green, yellow, red) as well as the
    # legacy long format (columns: x_col=colour, y_col=count).  Wide format is
    # emitted by the per-window MeterColour.csv tracer; melt and sum across all
    # windows so each colour gets one aggregate count.
    if {"green", "yellow", "red"}.issubset(df.columns):
        totals = df[["green", "yellow", "red"]].sum()
        df = totals.reset_index()
        df.columns = ["colour", "count"]
        x_col = "colour"
        y_col = "count"

    colour_map = {"green": "#2ca02c", "yellow": "#ffbb33", "red": "#d62728"}
    # Drop zero-count categories: a bar of length zero collapses cleanly,
    # whereas a pie wedge of size zero used to leave a labelled artefact.
    nonzero = df[df[y_col] > 0].reset_index(drop=True)
    labels = nonzero[x_col].tolist()
    sizes = nonzero[y_col].tolist()
    colors = [colour_map.get(str(lab).lower(), "gray") for lab in labels]

    total = float(sum(sizes)) if sizes else 0.0
    y_pos = list(range(len(labels)))
    ax.barh(y_pos, sizes, color=colors, edgecolor="white", linewidth=1)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()  # first row (typically green) sits at the top
    ax.set_xlabel(ylabel or "packet count")

    # Annotate each bar with "pct% (count)" to the right of the bar tip.
    for i, count in enumerate(sizes):
        pct = (100.0 * count / total) if total else 0.0
        ax.text(count, i, f"  {pct:.1f}%  ({int(count):,})", va="center", fontsize=9)

    # Headroom so right-side annotations do not clip the axes.
    if sizes:
        ax.set_xlim(0, max(sizes) * 1.25)

    ax.set_title(title)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["meter-colour-bar"] = plot_meter_colour_bar


def plot_tin_rate_heatmap(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Heatmap of per-tin rate over time."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(9, 4))

    pivot = df.pivot_table(index=series_col, columns=x_col, values=y_col, aggfunc="mean").fillna(0)
    im = ax.imshow(pivot.values, aspect="auto", cmap="viridis",
                   extent=[pivot.columns.min(), pivot.columns.max(),
                           pivot.index.min() - 0.5, pivot.index.max() + 0.5],
                   origin="lower")

    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label(ylabel or "Rate")

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(series_col)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["tin-rate-heatmap"] = plot_tin_rate_heatmap


def plot_aqm_envelope(df, output, title, xlabel, ylabel, x_col, y_col, series_col, calibration, format="svg"):
    """Throughput-vs-load envelope curves per AQM algorithm."""
    plt = _setup_matplotlib()
    fig, ax = plt.subplots(figsize=(8, 5))

    for series_id, group in df.groupby(series_col):
        group_sorted = group.sort_values(x_col)
        ax.plot(group_sorted[x_col], group_sorted[y_col], marker="o", label=str(series_id), linewidth=1.2)

    ax.set_title(title)
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    ax.legend(loc="best", fontsize=8)
    ax.grid(True, alpha=0.3)
    _annotate_calibration(ax, calibration)
    fig.tight_layout()
    fig.savefig(output, format=format)
    plt.close(fig)


PLOT_TYPES["aqm-envelope"] = plot_aqm_envelope


def _shrink_svg(path: Path) -> None:
    """Reduce float precision in an SVG to 2 decimal places.

    Matplotlib's SVG backend emits coordinates to 6+ decimal places.
    Truncating to 2 dp saves 30-50% of file size for dense path charts
    with no perceptible visual change at typical screen/print resolution.
    Only applied when the output format is SVG.
    """
    text = path.read_text(encoding="utf-8")
    # Replace floats with more than 2 decimal digits in path data, transform
    # attributes, and similar numeric contexts.  The pattern matches a digit
    # sequence followed by a dot and 3+ decimal digits.
    text = re.sub(r"(\d+\.\d{3,})", lambda m: f"{float(m.group(0)):.2f}", text)
    path.write_text(text, encoding="utf-8")


def _compute_aggregate(df, col, agg_name):
    """Compute a scalar aggregate of df[col]. Supports sum, mean, max, p95, cv.

    cv (coefficient of variation) returns sd/mean; sd uses ddof=0 (population
    formula). Returns math.nan if mean is exactly 0 (cv is undefined).
    """
    values = df[col].astype(float).to_numpy()
    if agg_name == "sum":
        return float(np.sum(values))
    if agg_name == "mean":
        return float(np.mean(values))
    if agg_name == "max":
        return float(np.max(values))
    if agg_name == "p95":
        return float(np.percentile(values, 95))
    if agg_name == "cv":
        mean = float(np.mean(values))
        if mean == 0.0:
            return math.nan
        sd = float(np.std(values, ddof=0))
        return sd / mean
    raise ValueError(f"unknown aggregate '{agg_name}'")


def _apply_y_unit_convert(df, y_col, convert):
    """Optionally convert the y_col values for display.

    Supported conversions:
      - "bps_to_mbps": divide by 1e6
      - "bytes_to_kib": divide by 1024
    """
    if not convert:
        return df
    if convert == "bps_to_mbps":
        df = df.copy()
        df[y_col] = df[y_col] / 1e6
        return df
    if convert == "bytes_to_kib":
        df = df.copy()
        df[y_col] = df[y_col] / 1024.0
        return df
    raise ValueError(f"Unknown y_unit_convert: {convert}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
