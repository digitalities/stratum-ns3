# ns3-csv-to-flent

Convert ns-3 simulation CSV bundles (emitted by `DsFlentCsvSink`) into
[Flent](https://flent.org/) JSON v1 (`.flent.gz`) files for replay in
`flent --plot`.

## Workflow

Three commands turn an ns-3 simulation into a Flent plot:

```bash
# 1. Run the ns-3 example to emit a CSV bundle.
./ns3 run "cake-rrul --outdir=/tmp/cake-rrul-bundle"

# 2. Convert the CSV bundle to a Flent JSON v1 file.
ns3-csv-to-flent \
    --test rrul \
    --indir /tmp/cake-rrul-bundle \
    --output /tmp/cake-rrul.flent.gz

# 3. Plot in Flent (any plot type Flent supports for the chosen test).
flent --plot=totals -i /tmp/cake-rrul.flent.gz
```

(Step 3 is validated separately; this package only covers steps 1 -> 2.)

## Install

```bash
cd scripts/flent-export
python3 -m venv .venv
source .venv/bin/activate
pip install -e .[dev]
```

## Run tests

```bash
pytest -v
```

## Supported tests

| Test           | Bundle layout                                         |
|----------------|--------------------------------------------------------|
| `rrul`         | 4 down + 4 up TCP, ICMP ping, 4 UDP probes             |
| `tcp_download` | single TCP download flow + ICMP ping                   |
| `tcp_upload`   | single TCP upload flow + ICMP ping                     |

The CSV bundle layout is documented in `SCHEMA.md`.

## Adding a new test

1. Add `ns3_csv_to_flent/schemas/<name>.py` defining `<NAME>_SCHEMA`.
2. Register it in `ns3_csv_to_flent/schemas/__init__.py` `SCHEMAS`.
3. Drop a golden-input fixture in `tests/fixtures/<name>-golden-input/`.
4. Add `tests/test_<name>_roundtrip.py`.

A schema is a dict with `name`, `title`, `series` (list of
`(series_name, csv_filename)` tuples), and `totals` (mapping a sum-name
to a list of member series names).
