# Troubleshooting

This page is a v1 stub. It grows as the community files issues — every confirmed gotcha gets added here as a Q-and-A entry.

## Known gotchas (so far)

### Build fails with "diffserv: No such module"

The `contrib/stratum` symlink wasn't created. Depending on your install path:

- **Option A (contrib-clone):** verify `contrib/stratum` exists inside your ns-3 tree and points to the Stratum repo root.
- **Option B (sibling fetch):** re-run `./scripts/fetch-ns3.sh` from the Stratum repo root — it creates `<ns-3>/contrib/stratum` → `<stratum-ns3 root>` and re-applies the local patches.

### `./ns3 build` succeeds but `./ns3 run "diffserv-example-1"` says "example not found"

`./ns3 configure` was run without `--enable-examples`. Reconfigure with:

```bash
./ns3 configure --enable-tests --enable-examples
./ns3 build stratum
```

### Tests pass locally but fail on a fresh clone of mine

Verify your ns-3 tree is at the pinned commit — the pin lives only in `scripts/fetch-ns3.sh` (`--print-pin`). Navigate into the ns-3 tree (`ns-3/` for Option A users, `../ns-3/` from `stratum-ns3/` for Option B users) and reset:

```bash
git checkout "$(contrib/stratum/scripts/fetch-ns3.sh --print-pin)"   # Option A
# or, for Option B: git checkout "$(../stratum-ns3/scripts/fetch-ns3.sh --print-pin)"
```

The pin is intentional — ns-3 mainline tightens compiler flags continuously, and a moved pin can regress the build against `-Werror` even with identical source.

### `--scheduler=wfq` (or other lowercase scheduler names) silently picks the default

The `diffserv-example-1` scheduler CLI flag is **case-sensitive**. Accepted values are exactly `PQ`, `WFQ`, `SCFQ`, `SFQ`, `WF2Qp`, `LLQ`. Lowercase (`wfq`, `pq`) does not raise an error — it just falls through to the default. If your "Try changing" output looks identical to the previous run, this is a likely cause.

### CAKE example says `FqCobalt: m_classes append-only`

Known behaviour of mainline `FqCobaltQueueDisc`: `GetNQueueDiscClasses()` returns flows-ever-seen, not currently-live (no per-flow erase happens in append-only mode). This is not a Stratum bug and does not affect substrate correctness. It only affects diagnostic counters that read the live bulk-flow count via that accessor.

### Wi-Fi recipe doesn't show DiffServ marking effects

Set `QosSupported=false` on the `WifiMac`. When QoS is true at the WiFi MAC, EDCA at L2 dominates over qdisc-level differentiation; for pure qdisc-comparison demos the wireless recipes set this explicitly. See [recipe 1 in wireless.md](I-07-wireless.md) for the gotcha discussion in context.

### `diffserv-cake --tin-mode=diffserv4` fails with "Cannot find argument"

`diffserv-cake.cc` does not accept a `--tin-mode` flag — `SetAsCakeDiffserv4` is hardcoded as the default. Available CLI flags: `--outDir`, `--simTime`, `--totalRateBps`, `--flowRateBps`. For other tin modes (besteffort / precedence / diffserv3 / diffserv8), use the helper directly in a custom scenario.

## Found a gotcha not listed here?

[File a bug](https://github.com/digitalities/stratum-ns3/issues/new?template=bug.yml). If it's reproducible across a clean clone, we'll add it here.
