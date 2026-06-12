#!/usr/bin/env python3
"""Pair-wise Scenario 3 comparison (any two simulators, 771 nodes, 5000 s).

Reads ServiceRate.tr and QueueLen.tr from both directories and produces
numerical tables plus overlay plots named <scenario>__<pair>__<metric>.png.

Usage:
    python3 scripts/scenario3-comparison.py \
      --dir-a output/ns2-29/example-3 --label-a ns-2.29 \
      --dir-b output/ns2-35/example-3 --label-b ns-2.35 \
      --pair ns229-vs-ns235 \
      --out-dir output/comparison/ns229-vs-ns235/scenario-3
"""
import argparse
import os


def load_trace(path, ncols, skip_warmup=1000):
    data = [[] for _ in range(ncols)]
    with open(path) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) != ncols:
                continue
            t = float(parts[0])
            if t < skip_warmup:
                continue
            for i in range(ncols):
                data[i].append(float(parts[i]))
    return data


def mean(v):
    return sum(v) / len(v) if v else 0


def std(v):
    m = mean(v)
    return (sum((x - m) ** 2 for x in v) / len(v)) ** 0.5 if v else 0


def print_rate_comparison(a, b, label_a, label_b, out_file=None):
    services = ["premium", "gold", "silver", "bronze", "be"]
    header = (f"{'Service':<10} {label_a+' mean':>12} {label_b+' mean':>12} "
              f"{'diff%':>8} {label_a+' std':>12} {label_b+' std':>12}")
    sep = "-" * len(header)

    lines = ["=== Service Rate Comparison (t=1000-5000, kbps) ===", header, sep]
    for i, s in enumerate(services):
        ma, mb = mean(a[i + 1]), mean(b[i + 1])
        sa, sb = std(a[i + 1]), std(b[i + 1])
        diff = (mb - ma) / ma * 100 if ma > 0 else 0
        lines.append(f"{s:<10} {ma:>12.1f} {mb:>12.1f} {diff:>7.1f}% {sa:>12.1f} {sb:>12.1f}")

    ta = sum(mean(a[i + 1]) for i in range(5))
    tb = sum(mean(b[i + 1]) for i in range(5))
    lines.append(sep)
    lines.append(f"{'Total':<10} {ta:>12.1f} {tb:>12.1f} {(tb-ta)/ta*100 if ta else 0:>7.1f}%")

    text = "\n".join(lines)
    print(text)
    if out_file:
        with open(out_file, "w") as f:
            f.write(text + "\n")


def print_queue_comparison(a, b, label_a, label_b, out_file=None):
    names = ["Premium(Q0)", "Gold(Q1)", "Silver(Q2)", "Bronze(Q3)", "BE(Q4)"]
    header = (f"{'Queue':<14} {label_a+' mean':>12} {label_b+' mean':>12} "
              f"{label_a+' max':>12} {label_b+' max':>12}")
    sep = "-" * len(header)

    lines = ["\n=== Queue Length Comparison (t=1000-5000) ===", header, sep]
    for i, name in enumerate(names):
        ma, mb = mean(a[i + 1]), mean(b[i + 1])
        xa = max(a[i + 1]) if a[i + 1] else 0
        xb = max(b[i + 1]) if b[i + 1] else 0
        lines.append(f"{name:<14} {ma:>12.1f} {mb:>12.1f} {xa:>12.0f} {xb:>12.0f}")

    text = "\n".join(lines)
    print(text)
    if out_file:
        with open(out_file, "a") as f:
            f.write(text + "\n")


def plot_overlay(a, b, label_a, label_b, out_dir, pair, scenario):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available — skipping plots")
        return

    services = ["Premium (VoIP)", "Gold (RealAudio)", "Silver (Telnet+FTP)",
                "Bronze (HTTP)", "Best Effort"]
    slug = ["premium", "gold", "silver", "bronze", "be"]

    # One combined 2x3 panel
    fig, axes = plt.subplots(2, 3, figsize=(15, 8))
    for i, title in enumerate(services):
        ax = axes[i // 3][i % 3]
        ax.plot(a[0], a[i + 1], "b-", alpha=0.4, linewidth=0.5, label=label_a)
        ax.plot(b[0], b[i + 1], "r-", alpha=0.4, linewidth=0.5, label=label_b)
        ax.set_title(title)
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Rate (kbps)")
        ax.legend(fontsize=8)

    axes[1][2].set_visible(False)
    plt.suptitle(f"{scenario}: {label_a} vs {label_b} (Service Rates, 771 nodes, 5000 s)")
    plt.tight_layout()
    combined_path = os.path.join(out_dir, f"{scenario}__{pair}__service-rate-all.png")
    plt.savefig(combined_path, dpi=150)
    plt.close(fig)
    print(f"Plot saved: {combined_path}")

    # Per-service individual plots for easier inclusion in a paper
    for i, (title, s) in enumerate(zip(services, slug)):
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.plot(a[0], a[i + 1], "b-", alpha=0.6, linewidth=0.8, label=label_a)
        ax.plot(b[0], b[i + 1], "r-", alpha=0.6, linewidth=0.8, label=label_b)
        ax.set_title(f"{scenario}: {title} — {label_a} vs {label_b}")
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Rate (kbps)")
        ax.legend()
        plt.tight_layout()
        p = os.path.join(out_dir, f"{scenario}__{pair}__service-rate-{s}.png")
        plt.savefig(p, dpi=150)
        plt.close(fig)
        print(f"Plot saved: {p}")


def main():
    parser = argparse.ArgumentParser(description="Pair-wise Scenario 3 comparison")
    parser.add_argument("--dir-a", required=True, help="Directory for trace A")
    parser.add_argument("--dir-b", required=True, help="Directory for trace B")
    parser.add_argument("--label-a", default="A", help="Label for A (plot legend, table header)")
    parser.add_argument("--label-b", default="B", help="Label for B")
    parser.add_argument("--pair", required=True,
                        help="Pair slug for filenames, e.g. ns229-vs-ns235")
    parser.add_argument("--scenario", default="scenario-3",
                        help="Scenario slug for filenames")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    summary_path = os.path.join(args.out_dir, "comparison-summary.txt")

    a_sr = load_trace(os.path.join(args.dir_a, "ServiceRate.tr"), 6)
    b_sr = load_trace(os.path.join(args.dir_b, "ServiceRate.tr"), 6)
    print_rate_comparison(a_sr, b_sr, args.label_a, args.label_b, summary_path)

    a_ql = load_trace(os.path.join(args.dir_a, "QueueLen.tr"), 6)
    b_ql = load_trace(os.path.join(args.dir_b, "QueueLen.tr"), 6)
    print_queue_comparison(a_ql, b_ql, args.label_a, args.label_b, summary_path)

    plot_overlay(a_sr, b_sr, args.label_a, args.label_b,
                 args.out_dir, args.pair, args.scenario)

    # OWD / IPDV plots (VoIP flow — single-column traces after time)
    for tr_name, metric_slug, ylabel in [
        ("OWD.tr",  "owd",  "OWD (ms)"),
        ("IPDV.tr", "ipdv", "IPDV (ms)"),
    ]:
        pa = os.path.join(args.dir_a, tr_name)
        pb = os.path.join(args.dir_b, tr_name)
        if not (os.path.exists(pa) and os.path.getsize(pa) > 0
                and os.path.exists(pb) and os.path.getsize(pb) > 0):
            print(f"  [skip] {tr_name}: empty or missing in one of the dirs")
            continue
        a_d = load_trace(pa, 2)
        b_d = load_trace(pb, 2)
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            continue
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.plot(a_d[0], a_d[1], "b-", alpha=0.6, linewidth=0.8, label=args.label_a)
        ax.plot(b_d[0], b_d[1], "r-", alpha=0.6, linewidth=0.8, label=args.label_b)
        ax.set_title(f"{args.scenario}: VoIP {ylabel} — {args.label_a} vs {args.label_b}")
        ax.set_xlabel("Time (s)")
        ax.set_ylabel(ylabel)
        ax.legend()
        plt.tight_layout()
        p = os.path.join(args.out_dir, f"{args.scenario}__{args.pair}__{metric_slug}.png")
        plt.savefig(p, dpi=150)
        plt.close(fig)
        print(f"Plot saved: {p}")


if __name__ == "__main__":
    main()
