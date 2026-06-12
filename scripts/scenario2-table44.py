#!/usr/bin/env python3
"""Reproduce thesis Table 4.4 from a sweep output directory.

Usage:
    python3 scripts/scenario2-table44.py [SWEEP_DIR]

If SWEEP_DIR is omitted, defaults to output/ns2/example-2-fullscale.
Reads the end-of-simulation 'Packets Statistics' block from each
set-N/run.log under SWEEP_DIR and emits the reproduced Table 4.4 with
deviation against the thesis target values.

Metric definitions:

* caPL (class-aware packet loss): edrops% per drop precedence at the AF
  queue. Directly comparable to thesis caPL (same measurement plane).
* boPL (buffer-overflow packet loss): tail-drop% per drop precedence.
  Directly comparable to thesis boPL.
* delivery_ratio: TxPkts/TotPkts per drop precedence, i.e. fraction of
  packets of that DSCP that cleared the queue. Reported as a reference
  metric — NOT directly comparable to thesis "goodput".
* measured_goodput (when available): TCP-layer retransmission-bytes
  ratio TCPbGoTX(x) / (TCPbGoTX(x) + TCPbReTX(x)) per DSCP, computed
  from the new "Retx Statistics" block emitted by ns-3 (Layer 2.E,
  patches/ns3/0002 + DiffServStatistics consumer) or by ns-2.x with
  the goodput plan's Tcl retx scaffold. Directly comparable to the
  thesis goodput target.

Tolerance check covers caPL, boPL, and (when present) measured_goodput
against THESIS values. delivery_ratio is reported but not checked.
"""
import re
import sys
from pathlib import Path

DSCP_TO_DP = {10: 0, 12: 1, 14: 2}  # AF only — BE (DSCP 0, 50) excluded from Table 4.4

# Thesis Table 4.4 target values (for deviation calculation).
# thesis_goodput is the TCP-layer metric; emitted for reference only.
THESIS = {
    1: {'caPL': {0: 0.00, 1: 0.01, 2: 25.12},
        'boPL': {0: 0.00, 1: 0.00, 2: 0.00},
        'thesis_goodput': {0: 0.87, 1: 0.91, 2: 0.78}},
    2: {'caPL': {0: 0.00, 1: 0.00, 2: 23.61},
        'boPL': {0: 0.00, 1: 0.00, 2: 0.00},
        'thesis_goodput': {0: 0.87, 1: 0.91, 2: 0.79}},
    3: {'caPL': {0: 0.00, 1: 0.54, 2: 27.45},
        'boPL': {0: 0.00, 1: 0.00, 2: 0.00},
        'thesis_goodput': {0: 0.88, 1: 0.91, 2: 0.78}},
    4: {'caPL': {0: 0.03, 1: 1.18, 2: 23.02},
        'boPL': {0: 0.00, 1: 0.00, 2: 0.00},
        'thesis_goodput': {0: 0.89, 1: 0.92, 2: 0.81}},
    5: {'caPL': {0: 2.91, 1: 4.84, 2: 20.50},
        'boPL': {0: 0.00, 1: 0.00, 2: 0.02},
        'thesis_goodput': {0: 0.89, 1: 0.91, 2: 0.83}},
    6: {'caPL': {0: 2.39, 1: 4.40, 2: 18.95},
        'boPL': {0: 0.03, 1: 0.01, 2: 0.09},
        'thesis_goodput': {0: 0.89, 1: 0.91, 2: 0.83}},
}

STATS_LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+([\d.]+)%\s+([\d.]+)%\s+([\d.]+)%')

# Retx Statistics block emitted by DiffServStatistics::PrintStats (ns-3,
# patches/ns3/0002 + Layer 2.E) or by `getStat TCPbReTX` (ns-2.x).
# Format on ns-3 (whitespace-aligned, fixed-width):
#   CP   origBytes   retxBytes  goodput
#   --   ---------   ---------  -------
#    0     3239460           0    1.000
RETX_LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s*$')


def parse_runlog(path):
    """Return dict {dscp: {tot, tx, ldrops, edrops, [origBytes, retxBytes, goodput]}}.

    Parses both the legacy 'Packets Statistics' block (always present) and
    the new 'Retx Statistics' block (present only when the run was produced
    by ns-3 with Layer 2.E consumer wired in, or by ns-2.x with the goodput
    plan's Tcl scaffold). The final blocks overwrite earlier ones, giving
    us end-of-simulation totals.
    """
    out = {}
    in_retx = False
    with open(path) as f:
        for line in f:
            stripped = line.strip()
            # Section detection — Retx block starts with its header line.
            if 'Retx Statistics' in stripped:
                in_retx = True
                continue
            if in_retx and stripped.startswith('---'):
                # End of the Retx block (footer rule); fall back to packet stats.
                in_retx = False
                continue

            if in_retx:
                m = RETX_LINE.match(line)
                if not m:
                    continue
                dscp = int(m.group(1))
                entry = out.setdefault(dscp, {})
                entry['origBytes'] = int(m.group(2))
                entry['retxBytes'] = int(m.group(3))
                entry['goodput'] = float(m.group(4))
                continue

            m = STATS_LINE.match(line)
            if not m:
                continue
            dscp = int(m.group(1))
            entry = out.setdefault(dscp, {})
            entry['tot']    = int(m.group(2))
            entry['tx']     = float(m.group(3))
            entry['ldrops'] = float(m.group(4))
            entry['edrops'] = float(m.group(5))
    return out


def compute_metrics(stats):
    """Map DSCP stats -> DP {caPL, boPL, delivery_ratio, [measured_goodput]}.

    measured_goodput is included only when the run.log carried the new
    Retx Statistics block (ns-3 with Layer 2.E, or ns-2.x with retx Tcl
    scaffold). Without that block, the field is omitted.
    """
    out = {}
    for dscp, dp in DSCP_TO_DP.items():
        if dscp not in stats:
            continue
        s = stats[dscp]
        cell = {
            'caPL':           s['edrops'],
            'boPL':           s['ldrops'],
            'delivery_ratio': s['tx'] / 100.0,
        }
        if 'goodput' in s:
            # Use the parser's pre-computed value if present; otherwise
            # derive from the byte counts.
            cell['measured_goodput'] = s['goodput']
        elif 'origBytes' in s and 'retxBytes' in s:
            denom = s['origBytes'] + s['retxBytes']
            cell['measured_goodput'] = (s['origBytes'] / denom) if denom > 0 else 1.0
        out[dp] = cell
    return out


def main():
    if len(sys.argv) > 1:
        base = Path(sys.argv[1])
    else:
        base = Path('output/ns2/example-2-fullscale')
    measured = {}
    for s in range(1, 7):
        logpath = base / f'set-{s}' / 'run.log'
        if not logpath.exists():
            print(f'# WARNING: {logpath} not found', file=sys.stderr)
            continue
        stats = parse_runlog(logpath)
        measured[s] = compute_metrics(stats)

    # Emit Table 4.4 reproduction
    print(f'# Table 4.4 Reproduction — source: `{base}`')
    print()
    print('Source: end-of-simulation `Packets Statistics` block in each')
    print('`set-N/run.log`, produced by `$qE1C printStats` at `testTime - 0.1`.')
    print()
    print('Columns: `measured (deviation vs. thesis)`. caPL/boPL are')
    print('percentage points (pp); delivery_ratio is a normalised fraction [0,1].')
    print('thesis_goodput is emitted as reference (see header note); it is')
    print('NOT tolerance-checked because its metric definition (TCP-retx ratio')
    print('per DSCP) is not directly comparable to our queue-level delivery_ratio.')
    print()
    print('| Metric          | DP  | Set1             | Set2             | Set3             | Set4             | Set5             | Set6             |')
    print('|-----------------|-----|------------------|------------------|------------------|------------------|------------------|------------------|')
    for metric in ('caPL', 'boPL', 'delivery_ratio', 'thesis_goodput'):
        for dp in (0, 1, 2):
            cells = []
            for s in range(1, 7):
                if s not in measured or dp not in measured[s]:
                    cells.append('—'.ljust(16))
                    continue
                if metric == 'delivery_ratio':
                    m = measured[s][dp]['delivery_ratio']
                    t = THESIS[s]['thesis_goodput'][dp]
                    dev = m - t
                    cell = f'{m:.2f} ({dev:+.2f})'
                elif metric == 'thesis_goodput':
                    # If the run produced retx data (ns-3 Layer 2.E or
                    # ns-2.x with retx scaffold), show measured-vs-reference
                    # deviation; otherwise emit reference only.
                    t = THESIS[s]['thesis_goodput'][dp]
                    if 'measured_goodput' in measured[s][dp]:
                        m = measured[s][dp]['measured_goodput']
                        dev = m - t
                        cell = f'{m:.2f} ({dev:+.2f})'
                    else:
                        cell = f'{t:.2f} (ref.)'
                else:
                    m = measured[s][dp][metric]
                    t = THESIS[s][metric][dp]
                    dev = m - t
                    cell = f'{m:.2f}% ({dev:+.2f}pp)'
                cells.append(cell.ljust(16))
            print(f'| {metric:15s} | DP{dp} | ' + ' | '.join(cells) + ' |')
    print()

    # Emit pass/fail summary. caPL + boPL are queue-level metrics and have
    # always been tolerance-checked. measured_goodput (added in Layer 2.E)
    # IS now directly comparable to thesis_goodput because both are now the
    # TCP retx-bytes ratio per DSCP — include it when present.
    TOLERANCES = {'caPL': 2.0, 'boPL': 0.5}
    GOODPUT_TOL = 0.05  # absolute, in [0, 1] units (5 pp). Initial pass; tighten after calibration.

    has_goodput = any('measured_goodput' in measured[s][dp]
                      for s in measured for dp in measured[s])

    print('# Tolerance check')
    print()
    print(f'Tolerances: caPL <= {TOLERANCES["caPL"]}pp, '
          f'boPL <= {TOLERANCES["boPL"]}pp'
          + (f', goodput <= {GOODPUT_TOL:.2f}' if has_goodput else ''))
    print('delivery_ratio: reported as reference; not tolerance-checked')
    print('(queue-level metric, distinct from TCP-retx goodput).')
    if not has_goodput:
        print('thesis_goodput: reference values only — no Retx Statistics block')
        print('found in this sweep. Run with Layer 2.E consumer (ns-3) or the')
        print('retx Tcl scaffold (ns-2.x) to enable measured comparison.')
    print()
    fails = []
    total_cells = 0
    for s in range(1, 7):
        if s not in measured:
            continue
        for dp in (0, 1, 2):
            if dp not in measured[s]:
                continue
            for metric, tol in TOLERANCES.items():
                total_cells += 1
                m = measured[s][dp][metric]
                t = THESIS[s][metric][dp]
                dev = abs(m - t)
                if dev > tol:
                    fails.append(
                        f'Set{s} DP{dp} {metric}: measured {m:.2f}%, '
                        f'thesis {t:.2f}%, dev {dev:.2f}pp > {tol}pp')
            if 'measured_goodput' in measured[s][dp]:
                total_cells += 1
                m = measured[s][dp]['measured_goodput']
                t = THESIS[s]['thesis_goodput'][dp]
                dev = abs(m - t)
                if dev > GOODPUT_TOL:
                    fails.append(
                        f'Set{s} DP{dp} goodput: measured {m:.3f}, '
                        f'thesis {t:.3f}, dev {dev:.3f} > {GOODPUT_TOL:.2f}')
    if fails:
        print(f'## {len(fails)} of {total_cells} CELLS OUTSIDE TOLERANCE:')
        print()
        for fail in fails:
            print(f'- {fail}')
    else:
        print(f'## ALL {total_cells} CELLS WITHIN TOLERANCE')


if __name__ == '__main__':
    main()
