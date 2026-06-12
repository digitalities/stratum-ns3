# Flent CSV bundle schema

The CSV bundle is the contract between the ns-3 emitter (`DsFlentCsvSink`)
and the Python converter (`scripts/flent-export/`). One bundle per
simulation run; one directory per bundle.

The emitter writes plain ASCII CSV files plus a small JSON metadata
sidecar; the converter reads the bundle and produces a Flent v1 JSON file
that `flent` itself can plot. All values are written from the simulator's
perspective: time is measured from `Simulator::Now() == 0` and bytes are
counted at the application layer (i.e., the byte count exposed by the
ns-3 `PacketSink`).

Time fields use seconds with six decimal places. Numeric CSV fields are
plain decimal; metadata.json values are quoted JSON strings (the
converter coerces types per-field).

## Per-test files

### rrul

Files in `<output_dir>/`:

- `metadata.json` - keys: `test_name` (str = "rrul"), `length_s` (float),
  `step_size_s` (float), `bandwidth_bps` (int), `rtt_ms` (float),
  `topology_class` (str), `ns3_build_sha` (str), `aqm` (str), `dscp_map` (object).

- `x_values.csv` - single column header `t`, time in seconds.

- `tcp_down_flow{0..3}.csv` - header `t,bytes_delta,goodput_mbps`.
  One row per `step_size` interval. `goodput_mbps` is delta-bytes * 8 / step_size_s / 1e6.

- `tcp_up_flow{0..3}.csv` - same schema as tcp_down.

- `ping_icmp.csv` - header `t,seq,rtt_ms`. Event-based: one row per
  Ping reply received. `seq` is the ICMP echo sequence number.

- `udp_probe_flow{0..3}.csv` - header `t,seq,rtt_ms`. Event-based:
  one row per UDP echo reply received.

### tcp_download

Single TCP flow; drops the per-flow indexing.

- `metadata.json` - `test_name = "tcp_download"`.
- `x_values.csv` - same as rrul.
- `tcp_down.csv` - header `t,bytes_delta,goodput_mbps`.
- `ping_icmp.csv` - same as rrul.

### tcp_upload

Mirror of tcp_download with `tcp_up.csv` instead.
