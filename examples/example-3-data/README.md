# RealAudio empirical CDFs

Four empirical cumulative distributions driving the Gold/RealAudio traffic
in `diffserv-example-3` (under `--scale=full`). Sourced from ns-2.29's pristine
`tcl/ex/realaudio/` (aggregated from Broadcast.com RealAudio traces,
ca. 2000) and copied verbatim from
`ns2/diffserv4ns/examples/example-3/`:

| File             | Rows | Driven quantity                                 |
|------------------|------|-------------------------------------------------|
| `userintercdf1`  | 177  | Inter-arrival time between user flow starts (s) |
| `sflowcdf`       | 4    | Number of sequential flows per user (count)     |
| `flowdurcdf`     | 254  | Flow duration (minutes; scaled ×60 to seconds)  |
| `fratecdf`       | 8    | Per-flow emission rate (kbps)                   |

Each row is `value count cum_prob`. The middle `count` column is the raw
histogram frequency and is ignored by the loader (matching ns-2's
`RandomVariable/Empirical::loadCDF` which scans via `%lf %*f %lf`).

## Where they're consumed

`ns3::stratum::LoadEmpiricalCdfFromFile` in
`src/ns-3/model/empirical-cdf-loader.{h,cc}` reads these files and
returns an `ns3::EmpiricalRandomVariable`.
