#!/usr/bin/env python3
"""Python-side tests for plot_recipe.py.

Runs under either pytest or as a bare script: `python3 scripts/test_plot_recipe.py`.
"""

import re
import sys
from pathlib import Path


WT = Path(__file__).resolve().parent.parent


def test_aqm_envelope_axis_in_mbps():
    """The rendered aqm-eval-runner SVG must show y-axis in Mbps, not ECDF [0,1]."""
    svg = WT / "guide" / "figures" / "aqm-eval-runner" / "aqm-envelope.svg"
    assert svg.exists(), f"SVG not rendered yet: {svg}"
    text = svg.read_text()
    # matplotlib renders tick labels as <text ...>VALUE</text> elements.
    numbers = [float(m) for m in re.findall(r">(\d+(?:\.\d+)?)</text>", text)]
    big = [n for n in numbers if 5 <= n <= 100]
    assert big, (
        f"Expected at least one y-tick label in [5, 100] Mbps range; "
        f"found numbers: {sorted(set(numbers))[:10]} (likely ECDF [0,1] regression)"
    )


def test_l4s_s1_advantage_three_series():
    """Verify l4s-s1-advantage recipe yields three legend entries
    (L4S/FqCoDel/FIFO) in the probe-owd-ecdf SVG.

    The recipe uses arms=overlay-all with three mode arms (l4s, fqcodel, fifo)
    and a series_label_map that maps each arm name to a human-readable label.
    This test asserts that all three mapped labels appear in the rendered SVG,
    confirming that the overlay-all arm grouping + series_label_map pipeline
    produces distinct series for the 26x AQM-vs-no-AQM gap narrative.
    """
    svg = WT / "guide" / "figures" / "l4s-s1-advantage" / "probe-owd-ecdf.svg"
    assert svg.exists(), f"SVG not rendered yet: {svg}"
    text = svg.read_text()
    # The recipe's series_label_map maps arm names to these display labels:
    #   "l4s"     -> "L4S (DualPI2)"
    #   "fqcodel" -> "FqCoDel"
    #   "fifo"    -> "FIFO (no AQM)"
    # matplotlib renders legend text as <text ...>LABEL</text> elements.
    expected_labels = ["L4S", "FqCoDel", "FIFO"]
    missing = [label for label in expected_labels if label not in text]
    assert not missing, (
        f"SVG legend is missing series labels: {missing}. "
        f"Expected all three mode series (l4s/fqcodel/fifo) to appear via "
        f"series_label_map in {svg}"
    )


def test_aggregate_cv_basic():
    """aggregate: cv computes coefficient of variation (sd/mean) per series."""
    import pandas as pd
    from plot_recipe import _compute_aggregate

    df = pd.DataFrame({"flow_id": [0, 1, 2, 3], "goodput_mbps": [2.0, 2.0, 2.0, 2.0]})
    # zero-variance series -> cv = 0
    assert _compute_aggregate(df, "goodput_mbps", "cv") == 0.0

    df2 = pd.DataFrame({"flow_id": [0, 1, 2, 3], "goodput_mbps": [1.0, 2.0, 3.0, 4.0]})
    # mean=2.5; sd(ddof=0)=sqrt(1.25)~=1.118; cv~=0.447
    cv = _compute_aggregate(df2, "goodput_mbps", "cv")
    assert 0.44 < cv < 0.46, f"cv={cv}"


def test_aggregate_cv_zero_mean_returns_nan():
    """cv of zero-mean series is undefined (would divide by zero) -- return NaN."""
    import math
    import pandas as pd
    from plot_recipe import _compute_aggregate

    df = pd.DataFrame({"flow_id": [0, 1], "x": [0.0, 0.0]})
    result = _compute_aggregate(df, "x", "cv")
    assert math.isnan(result), f"expected NaN, got {result}"


def main():
    test_aqm_envelope_axis_in_mbps()
    print("✅ test_aqm_envelope_axis_in_mbps PASSED")
    test_l4s_s1_advantage_three_series()
    print("✅ test_l4s_s1_advantage_three_series PASSED")
    test_aggregate_cv_basic()
    print("✅ test_aggregate_cv_basic PASSED")
    test_aggregate_cv_zero_mean_returns_nan()
    print("✅ test_aggregate_cv_zero_mean_returns_nan PASSED")


if __name__ == "__main__":
    main()
