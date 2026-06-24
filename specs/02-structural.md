# Structural Specs — diffserv-ns3

**Tier:** Structural (testable assertions against the implementation)
**Audience:** Implementer and CI
**Format:** Each spec describes one observable property of one component, expressed as a test that should pass. Specs link back to Intent specs (I-N) where applicable.

## S-1: Token bucket arithmetic

**S-1.1** [↪ I-2.3] Given a `TokenBucketMeter` with CIR=500 kbps, CBS=10000 bytes, fed a CBR stream at 500 kbps with 1000-byte packets, in steady state every packet shall be marked GREEN.

**S-1.2** [↪ I-2.3] Given a `TokenBucketMeter` with CIR=500 kbps, CBS=10000 bytes, fed a CBR stream at 1 Mbps with 1000-byte packets, the steady-state GREEN packet ratio shall equal 0.5 ± 0.01.

**S-1.3** [↪ I-2.3] After an idle period of `t` seconds, the bucket level shall increase by `min(CIR * t, CBS - cBucket)` bytes on the next packet's `ApplyMeter` call.

**S-1.4** [↪ I-2.3] The bucket level shall never exceed CBS.

**S-1.5** [↪ I-2.3] The bucket level shall never go negative.

## S-2: srTCM (RFC 2697)

**S-2.1** [↪ I-2.1] Given an `SrTcmMeter` with CIR=1 Mbps, CBS=10000, EBS=20000 starting empty, after 100 ms idle the cBucket shall hold 12500 bytes (overflowing into eBucket which shall hold 0 bytes — overflow is to eBucket, but cBucket caps at CBS=10000 and the remaining 2500 goes to eBucket).

**S-2.2** [↪ I-2.1] Given the conditions above, a 5000-byte packet arriving immediately after shall be marked GREEN, leaving cBucket=5000, eBucket=2500.

**S-2.3** [↪ I-2.1] Given an `SrTcmMeter` with CIR=1 Mbps, CBS=2000, EBS=4000 fed CBR traffic at 1.5 Mbps with 1000-byte packets, the long-run GREEN ratio shall equal CIR/arrival_rate = 0.667 ± 0.02.

**S-2.4** [↪ I-2.1] Given the conditions in S-2.3, the long-run YELLOW ratio shall equal (excess up to PIR-CIR equivalent for srTCM) such that GREEN+YELLOW = (CIR+EBS_refill)/arrival_rate, and the RED ratio shall match the remainder.

**S-2.5** [↪ I-2.1] The colour decision sequence shall match the reference implementation in `dsPolicy.cc:697` line for line for any deterministic input sequence.

## S-3: trTCM (RFC 2698)

**S-3.1** [↪ I-2.2] Given a `TrTcmMeter` with CIR=500 kbps, PIR=1 Mbps, CBS=2000, PBS=4000, fed CBR traffic at 750 kbps with 1000-byte packets, the long-run GREEN ratio shall equal CIR/arrival_rate = 0.667 ± 0.02.

**S-3.2** [↪ I-2.2] Given the conditions above, the long-run YELLOW ratio shall equal (PIR-CIR)/arrival_rate = 0.667 - long-run-RED, and the long-run RED ratio shall be approximately 0 (since arrival rate < PIR).

**S-3.3** [↪ I-2.2] Given a `TrTcmMeter` with CIR=500 kbps, PIR=1 Mbps, fed CBR at 1.5 Mbps, the GREEN ratio shall equal 0.333 ± 0.02 and the RED ratio shall equal 0.333 ± 0.02.

**S-3.4** [↪ I-2.2] When `pBucket < packet_size`, the packet shall be marked RED regardless of `cBucket`.

**S-3.5** [↪ I-2.2] When `cBucket < packet_size <= pBucket`, the packet shall be marked YELLOW and pBucket shall be decremented by packet_size while cBucket shall be unchanged.

**S-3.6** [↪ I-2.2] When `cBucket >= packet_size`, both buckets shall be decremented by packet_size and the packet marked GREEN.

## S-4: TSW2CM and TSW3CM

**S-4.1** [↪ I-2.4] After feeding CBR traffic at rate `R` for at least `10 * winLen`, the EWMA `avgRate` shall be within 5% of `R`.

**S-4.2** [↪ I-2.4] Given a `Tsw2cmMeter` with CIR=1 Mbps and `winLen=1s`, fed CBR at 500 kbps, the long-run GREEN ratio shall equal 1.0.

**S-4.3** [↪ I-2.4] Given a `Tsw2cmMeter` with CIR=1 Mbps, fed CBR at 2 Mbps, the long-run GREEN ratio shall equal CIR/avgRate = 0.5 ± 0.05.

## S-5: Round Robin family

**S-5.1** [↪ I-3.1] `RoundRobinScheduler` with N backlogged equal-priority queues shall deliver each queue exactly 1/N of the link capacity in steady state.

**S-5.2** [↪ I-3.2] `WeightedRoundRobinScheduler` with weights `w_1, ..., w_N` shall deliver queue `i` a share of `w_i / sum(w)` of link capacity ± 1 max packet size per round.

**S-5.3** [↪ I-3.3] `WeightedInterleavedRoundRobinScheduler` with weights `w_1, ..., w_N` shall deliver the same long-run share as WRR but with bounded burstiness — no flow shall be served more than `ceil(w_i / gcd(w))` consecutive packets.

## S-6: Priority Queue

**S-6.1** [↪ I-3.4] `PriorityScheduler` shall always serve the highest-priority non-empty queue.

**S-6.2** [↪ I-3.4] When a higher-priority queue is rate-capped at `R`, lower-priority queues shall receive the remaining capacity `C - R` in steady state.

**S-6.3** [↪ I-3.4] An EF queue (priority 0) under offered load `0.3 * C` with cap `0.5 * C` shall deliver all 0.3C and leave 0.7C for lower queues.

## S-7: Weighted Fair Queueing (WFQ)

**S-7.1** [↪ I-3.5] Given N backlogged flows with weights `w_1, ..., w_N` over a link of capacity `C`, each flow `i` shall receive throughput `w_i / sum(w) * C` within ± `MTU/T` over any window of duration `T >= N * MTU * 8 / C`.

**S-7.2** [↪ I-3.5] When a flow becomes idle and then resumes, its first packet shall be served within one max-packet-time of resumption (no penalty for idleness).

**S-7.3** [↪ I-3.5] The virtual time `v_time` shall be monotonically non-decreasing.

**S-7.4** [↪ I-3.5] The scheduler shall not serve packets out of order within a single flow.

## S-8: WF2Q+

**S-8.1** [↪ I-3.6] All assertions S-7.1–S-7.4 apply to `Wf2qPlusScheduler`.

**S-8.2** [↪ I-3.6] **Eligibility property**: at any service instant, the scheduler shall only consider packets whose virtual start time `S` satisfies `S <= V` (the current virtual time).

**S-8.3** [↪ I-3.6] The worst-case service delay for a flow with weight `w_i` and rate `r_i = w_i/sum(w) * C` shall be bounded by `(L_max,i / r_i) + (L_max / C)` where `L_max,i` is the maximum packet size of flow `i` and `L_max` is the maximum across all flows.

## S-9: SCFQ

**S-9.1** [↪ I-3.7] `ScfqScheduler` shall use the finish time of the packet currently in service as virtual time, not a separate GPS computation.

**S-9.2** [↪ I-3.7] Long-run throughput shares shall match WFQ within ± `MTU/T` for any time window `T`.

**S-9.3** [↪ I-3.7] Packet labels shall be monotonically non-decreasing within a single flow.

## S-10: SFQ (Start-time Fair Queueing)

**S-10.1** [↪ I-3.8] Each packet shall receive a start tag and a finish tag based on the previous packet on its flow.

**S-10.2** [↪ I-3.8] Packets shall be served in increasing order of start tag.

**S-10.3** [↪ I-3.8] Long-run throughput shares shall match WFQ within ± `MTU/T`.

## S-11: LLQ

**S-11.1** [↪ I-3.9] `LlqScheduler` configured with one priority queue and a WFQ component shall serve the priority queue strictly first when non-empty.

**S-11.2** [↪ I-3.9] When the priority queue is idle, the WFQ component shall manage the remaining traffic according to S-7.1.

**S-11.3** [↪ I-3.9] The priority queue's rate cap shall be respected — if cap=R is set and offered load > R, only R bandwidth shall be granted to the priority queue.

## S-12: DS-RED queue with virtual queues

**S-12.1** [↪ I-4.3] `RedSubQueue` with two drop precedence levels shall drop higher-precedence packets at a higher RED probability than lower-precedence packets at the same average queue length.

**S-12.2** [↪ I-4.3] When the physical buffer is full, all incoming packets shall be dropped regardless of drop precedence (tail drop on overflow).

**S-12.3** [↪ I-4.3] The RED average queue length shall use the same EWMA formula as standard ns-3 RED, with `MEAN_PKT_SIZE = 1000` for the initial implementation.

## S-13: Edge router

**S-13.1** [↪ I-1.5] After a packet has been enqueued and dequeued by the edge router, its IPv4 DSCP field shall match the result of `PolicyClassifier::ApplyPolicy()`. Internally, the DSCP is stored in a `DscpTag` at enqueue time and applied to the IPv4 header at dequeue time.

**S-13.2** [↪ I-1.6] A packet not matching any mark rule shall enter the queue with its original DSCP unchanged.

**S-13.3** [↪ I-1.1, I-1.2] Mark rules with specific source or destination addresses shall match only packets with those exact addresses; `ANY_HOST` shall match all.

**S-13.18** [↪ I-17.1, I-17.2] **(IPv6 twin of S-13.1.)** After a packet has been enqueued and dequeued by the edge router, its DS-field (DSCP+ECN octet) shall be read via `item->GetUint8Value(QueueItem::IP_DSFIELD, dsField)` (family-agnostic; RFC 2474 §2) and the DSCP rewrite shall be applied via `stratum::SetDscpPreservingEcn(item, dscp)`. For an `Ipv6QueueDiscItem` the read returns `m_header.GetTrafficClass()` and the write uses `Ipv6Header::SetDscp()` (ECN-preserving by `Ipv6Header` contract); the resulting DSCP on dequeue shall match `PolicyClassifier::ApplyPolicy()`, identical to the IPv4 contract. A non-IP `QueueDiscItem` (neither v4 nor v6) shall have its DS-field read return `false` and its DSCP rewrite skipped (no-op), with the packet forwarded unmodified. Verified by: `EdgeIpv6ClassifyMarkTest` in `diffserv-test-suite.cc`.

## S-14: PHB structure

**S-14.1** [↪ I-4.4] An EF configuration (DSCP 46, priority queue, rate cap) shall enforce zero queueing delay for in-profile EF traffic up to the cap.

**S-14.2** [↪ I-4.5] An AF configuration with three drop precedence levels shall apply progressively higher drop probabilities to AFx2 and AFx3 than to AFx1 at any given queue length.

**S-14.3** [↪ I-4.6] A best-effort configuration (DSCP 0) shall use the lowest-priority queue.

## S-15: Statistics collection

**S-15.1** [↪ I-5.6] After running a scenario for time `T`, the average queue length reported per queue shall match a manual integration of trace-source samples within ± 1 packet.

**S-15.2** [↪ I-5.11] After a scenario, `received + dropped = transmitted_by_sender` per DSCP for any closed system.

**S-15.3** [↪ I-5.12] Drop counts attributed to RIO and to buffer overflow shall sum to total drops per DSCP.

## S-16: Helper class

**S-16.1** [↪ I-6.1] A scenario built using only `stratum::Helper` and standard ns-3 helpers shall reproduce example-1 results within the tolerance defined in Q-1.

**S-16.2** [↪ I-6.2] All meter parameters (CIR, CBS, EBS, PIR, PBS) shall be settable and gettable via `Config::Set` and `Config::Get` paths.

## S-17: CAKE substrate (slot dispatcher, tin shaping, hybrid LLQ, host isolation)

The CAKE substrate introduces a pluggable across-slot dispatcher abstraction (`SlotDispatcher`) with three concrete strategies (strict-priority, tin-shaping DRR, hybrid LLQ-across-tins), a per-tin token-bucket gate for hard rate caps, and native per-host isolation on the per-tin `FqCobaltQueueDisc`. The S-17 assertions pin the substrate's behavioural contracts; Q-15 (in `03-quality.md`) pins the end-to-end paper-replication gates.

**Host isolation.** The S-17.x host-isolation assertions below are authored against mainline `FqCobaltQueueDisc` configured with `EnableHostIsolation` and `HostIsolationMode` (the attribute surface from `patches/ns3/0006`, with the per-host hashing hooks from `patches/ns3/0016`). Per-host fairness is an emergent property of per-flow DRR under host-load quantum modulation: each flow's quantum is scaled by the reciprocal divide `quantum·(65535/host_load)>>16`, where `host_load = max(srchost_bulk_flow_count, dsthost_bulk_flow_count)`. The CAKE composition wires `HostIsolationMode=Triple` via `cake::Helper::SetAsCakeDiffserv4(…, enableHostIsolation=true)`. The mainline enum exposes five modes — `Flowblind`, `Flows`, `DualSrcHost`, `DualDstHost`, `Triple` — whose round-trip and per-host behaviour are covered by the `fq-cobalt-queue-disc` test suite.

### S-17.1–4: SlotDispatcher contract and tin-shaping DRR

**S-17.1** [↪ I-7.1] With the strict-priority dispatcher, `SlotDispatcher::SelectDequeueSlot` shall return the same slot index, in the same byte sequence, as the legacy strict-priority `EdgeQueueDisc::DoDequeue` path it replaced (byte-identity gate).

**S-17.2** [↪ I-7.1] `TinShaperDispatcher` with equal quanta and exact-multiple head sizes shall serve at most one consecutive packet from the same slot (DRR fairness invariant).

**S-17.3** [↪ I-7.1] `TinShaperDispatcher::SelectDequeueSlot` shall skip empty slots without consuming deficit; subsequent dispatch sequence shall be byte-identical to the same fixture run with the empty slot omitted.

**S-17.4** [↪ I-7.1] Under mixed-load with three or more saturating slots and equal quanta, the served-byte ratio across slots shall remain within ±2 % of the configured-quantum ratio over a 100-packet window.

### S-17.5–8: cake::Helper composition

**S-17.5** [↪ I-7.2] `cake::Helper::SetAsCakeDiffserv4` shall produce the Linux `sch_cake` `diffserv4` DSCP-to-tin map verbatim (Bulk/BE/Video/Voice with the documented DSCP set per tin). Reference: `sch_cake.c @ 67dc6c56b871` (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c).

**S-17.6** [↪ I-7.2] `cake::Helper::SetAsCakeDiffserv3` shall produce the Linux `sch_cake` `diffserv3` map verbatim. Reference: `sch_cake.c @ 67dc6c56b871` (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c).

**S-17.7** [↪ I-7.2] `cake::Helper::SetAsCakeDiffserv8` shall produce the Linux `sch_cake` `diffserv8` map verbatim. Reference: `sch_cake.c @ 67dc6c56b871` (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c).

**S-17.8** [↪ I-7.2] A single-tin CAKE composition shall pass packets end-to-end through the helper-built `EdgeQueueDisc` + tin-shaper + inner mainline `FqCobaltQueueDisc` chain with byte counts conserved (enqueued = dequeued + dropped).

### S-17.9: ACK-filter API + functional contract

**S-17.9** [↪ I-7.2] `FqCobaltQueueDisc::EnableAckFilter` and `EnableAckFilterAggressive` shall be settable and readable via `Config::Set` / `Config::Get` and the typed accessors. The filter is functional: with `EnableAckFilter=true`, redundant monotonic plain-TCP ACKs within a 5-tuple collapse to the newest survivor (drop reason `ACK_FILTER_DROP`). Functional behaviour is asserted by `AckFilterFunctionalContractTest` in `diffserv-test-suite.cc`. Reference: `sch_cake.c @ 67dc6c56b871` (provenance/linux-sch-cake-67dc6c56b871/sch_cake.c).

### S-17.11–14: Hybrid LLQ-across-tins dispatcher

**S-17.11** [↪ I-7.1] When both an SP-designated slot and one or more DRR slots are non-empty, `HybridLlqDispatcher::SelectDequeueSlot` shall return the SP slot until it drains.

**S-17.12** [↪ I-7.1] When the SP-designated slot is empty, the dispatcher's selection sequence shall be byte-identical to the pure `TinShaperDispatcher` over the same fixture.

**S-17.13** [↪ I-7.1] `HybridLlqDispatcher::PeekSlot` shall be side-effect-free across both the SP fast path and the DRR fall-through (two consecutive peeks return the same slot/packet; subsequent dequeue returns that same packet).

**S-17.14** [↪ I-7.1] `HybridLlqDispatcher::OnDequeue` shall account deficit only for DRR slots; SP-slot dequeues shall not advance any DRR cursor.

### S-17.15–18, 20: Per-tin TBF rate-cap dispatcher

**S-17.15** [↪ I-7.1] `TinTokenBucket` unit math: `HasTokensFor` returns true unconditionally when `rateBps == 0`; `Configure` resets the bucket to a full state (`tokensBytes == burstBytes`); after `Charge(N)`, `HasTokensFor(N)` is false until `N / rateBps` time has elapsed; `Refill` caps at `burstBytes` (no over-fill on long idle).

**S-17.16** [↪ I-7.1] `TinShaperDispatcher` with a per-slot rate cap configured shall serve no more than `cap × duration ± 5 %` bytes from that slot over a saturating-input window.

**S-17.17** [↪ I-7.1] When one slot is capped and another is idle, the capped slot's served bytes shall remain within its own cap regardless of idle-slot capacity (no-save-up / no-borrow-from-idle invariant — distinguishes CAKE's `diffserv4` from plain DRR).

**S-17.18** [↪ I-7.1] `TinShaperDispatcher::PeekSlot` under an active rate cap shall be side-effect-free: two consecutive peeks return the same packet; subsequent dequeue returns that same packet; no tokens consumed and no DRR cursor advanced.

**S-17.20** [↪ I-7.1] LLQ + tin-shaping composition (Cisco MQC LLQ pattern: SP fast path with hard-cap on the priority slot) shall (a) hold the EF slot to its configured cap under saturating EF input, and (b) deliver the EF remainder to BE without DRR starvation.

*Note:* S-17.19 is reserved (the ACK-filter functional gate it once held is now `AckFilterFunctionalContractTest` in `diffserv-test-suite.cc`).

### S-17.21–23: Host isolation

**S-17.21** [↪ I-7.1] Mainline `FqCobaltQueueDisc` with `EnableHostIsolation=false` shall produce a byte-identical dequeue sequence to the same disc with host isolation never enabled, over the same input fixture — the host-isolation path is inert when disabled.

**S-17.22** [↪ I-7.1] With `EnableHostIsolation=true` and `HostIsolationMode=Triple`, distinct source / destination hosts shall be tracked under distinct per-host bulk-flow counters (separate `srchost` / `dsthost` slot-table entries), so that per-host quantum modulation distinguishes them; observable through the per-host slot-table state.

**S-17.23** *(retired)* `MaxHostPairs` LRU bucket recycling was internal to the `DsHostIsolatedFqCobalt` wrapper — see *Retired host-isolation assertions* at the end of §S-17.

### S-17.28: Egress DSCP wash

**S-17.28** [↪ I-7.2] `EdgeQueueDisc::Wash` is a boolean attribute (default `false`) that, when `true`, zeros the DSCP bits (the high six bits of the IPv4 TOS byte) on every dequeued `Ipv4QueueDiscItem` while preserving the low two ECN bits. With `Wash=false`, the high six bits of TOS on a dequeued item shall match the per-packet DSCP tag set during enqueue (the existing rewrite contract). Mirrors Linux `tc-cake wash` semantics: classification stays in effect inside the qdisc; the egress packet leaves with the IP-precedence/DSCP byte cleared so downstream forwarders see CS0/Default.

**S-17.28v6** [↪ I-17.2] **(IPv6 twin of S-17.28 — address-agnostic wash.)** The DSCP-zero wash shall apply identically to `Ipv6QueueDiscItem`: when `Wash=true`, the top six bits of the IPv6 Traffic Class byte shall be zeroed on dequeue while the low two ECN bits are preserved, using `stratum::SetDscpPreservingEcn(item, 0)`. A non-IP item shall pass through the wash path without any header modification (the `SetDscpPreservingEcn` no-op contract from S-13.18 applies). The wash assertion is thus address-family agnostic: the same code path and attribute gate apply to v4, v6, and non-IP items, with only the header-write method dispatched by family. Verified by: `EgressDscpWashIpv6Test` in `diffserv-test-suite.cc`.

### S-17.29: CAKE per-tin `MemLimit` API surface

**S-17.29** [↪ I-7.2] Mainline `FqCobaltQueueDisc::MemLimit` is a uinteger attribute (default `0`) that round-trips through `Config::Set` / `Config::Get` and through the typed `SetMemLimit` / `GetMemLimit` accessors. Mirrors the Linux `tc-cake memlimit BYTES` API surface. With the sentinel default value `0` the gate is disabled and the queue disc is byte-identical to a fresh mainline `FqCobaltQueueDisc`. The packet-count `MaxSize` attribute (default `10240p`) is independent and unaffected by `MemLimit`. The functional byte-counted enqueue gate is asserted by the upstream `FqCobaltQueueDiscMemLimit` test case in the `fq-cobalt-queue-disc` suite — that gate now lives in mainline `FqCobaltQueueDisc` per `patches/ns3/0006-fq-cobalt-ack-filter-memlimit.patch` (filed upstream; pin advances and patch retires once merged).

### S-17.30: CAKE link-layer overhead — statistical rate adjustment (v1)

**S-17.30** [↪ I-2] `cake::Helper::ConfigureLinkLayerOverhead(edge, overhead, atm, mpu)` invoked after a `SetAsCake*` profile composed with `useInnerTbfShaping=true` shall downscale every per-tin inner `TbfQueueDisc::Rate` by `gamma = E[wire_bytes(s)] / E[s]` over the bimodal Internet mix `{(64B, 0.5), (1500B, 0.5)}`, where `wire_bytes(s) = max(s + overhead, mpu)` for `atm=false` and `wire_bytes(s) = max(ceil((s + overhead) / 48) * 53, mpu)` for `atm=true`. This mirrors the per-packet adjustment performed by Linux `cake_overhead()` in `sch_cake.c` at commit `16d7fed7` (the CAKE-paper reference deposit; Hoiland-Jorgensen et al. 2018, Zenodo `10.5281/zenodo.1226887`). The post-adjustment per-tin rate must equal `share × totalRate / gamma` within ±0.5 % relative error. The v1 contract is statistical-mode — accurate in steady state for the bimodal mix; per-packet wire-byte adjustment at TBF dequeue is v1.1 follow-up.

### S-17.31: CAKE raw mode — suppress overhead correction

**S-17.31** [↪ I-2] `cake::Helper::ConfigureLinkLayerOverhead(edge, overhead, atm, mpu, raw=true)` shall short-circuit before any rate-adjustment pass and leave every per-tin inner `TbfQueueDisc::Rate` exactly equal to its post-`SetAsCake*` value, regardless of the `overhead`, `atm`, or `mpu` arguments. Mirrors the Linux `tc-cake raw` flag, which interprets the configured `bandwidth` as the raw IP-layer rate and disables PHY-framing correction. The `raw` argument has a default value of `false`, preserving call-site source compatibility for the four-argument form introduced by S-17.30.

### S-17.32: CAKE per-tin diagnostics — `GetTinStats(slot)` (v1)

**S-17.32** [↪ I-7] `EdgeQueueDisc::GetTinStats(slot)` shall return a `TinStats` snapshot of per-tin counters: `bytesEnqueued`, `bytesDequeued`, `drops`, `marks`. Wire-byte fields are dispatcher-tracked (`TinShaperDispatcher` / `HybridLlqDispatcher` increment in `OnEnqueue` / `OnDequeue` using `QueueDiscItem::GetSize()`); drops / marks are read-side from the inner `QueueDisc::Stats` at call time (`nTotalDroppedPackets` / `nTotalMarkedPackets`). The default `StrictPriorityDispatcher` returns a zeroed snapshot — non-CAKE compositions are unaffected. Out-of-range slot indices yield a zeroed snapshot rather than aborting. v1 contract surfaces the four counters above; sparse-flow count (Linux `flows_used`) is not surfaced — mainline `FqCobaltQueueDisc` exposes only an append-only class list, not a live sparse-flow counter. Mirrors the Linux `tc -s qdisc show` per-tin reporting that operators rely on for CAKE health checks.

### S-17.33: Set-associative flow hash — structural-equivalence audit (mainline ns-3)

**S-17.33** [↪ I-7.2] Mainline ns-3 `FqCobaltQueueDisc::EnableSetAssociativeHash` (with `SetWays = 8`) shall realise the 8-way set-associative flow-bucket lookup specified by Hoiland-Joergensen et al. 2018 §3 and implemented in Linux `sch_cake.c` at the CAKE-paper deposit commit `16d7fed7`. Structural equivalence of the lookup logic (modulo-flows decomposition into outer / inner indices; linear probe within each set; tag-match-or-reuse acceptance) is maintained. The underlying 5-tuple → 32-bit hash function diverges at the bit level (Linux `jhash_3words` vs ns-3 `Ipv4QueueDiscItem::Hash` over the platform `Hasher`, default Murmur3); the divergence is design-neutral with respect to set-associativity and is accepted permanently. The fixture `SetAssociativeHashStructuralPropertiesTest` shall verify (a) the `EnableSetAssociativeHash` and `SetWays` attributes are settable and readable; (b) same-flow → same-bucket determinism (tag-preservation invariant: a second arrival of an already-seen 5-tuple lands in the same queue index it landed in on the first arrival) under both modes; and (c) under default sizing (`m_flows = 1024`, `SetWays = 8`) and a small-cardinality distinct-flow input, distinct flows are spread across distinct queue indices in both modes. Byte-exact equivalence to Linux is **not** asserted (and is not achievable under the divergent hash function). The substrate claim is satisfied at the algorithmic-design level.

**S-17.33v6** [↪ I-17.3] **(IPv6 twin of S-17.33 — family-agnostic flow hash for CAKE bulk counter.)** The CAKE per-host bulk-flow counter (`FlowHashFromItem` in `stratum-cake-live-bulk-counter.cc`) shall use `item->Hash(perturbation)` (the `QueueDiscItem` base-class virtual) rather than a hand-packed `uint64_t` from bare `Ipv4Address` fields. `Ipv4QueueDiscItem::Hash()` returns a Murmur3 hash over the IPv4 5-tuple; `Ipv6QueueDiscItem::Hash()` returns the same Murmur3 hash over the IPv6 5-tuple. Both yield a 32-bit bucket index that is family-agnostic at the call site. A non-IP item (neither `Ipv4QueueDiscItem` nor `Ipv6QueueDiscItem`) shall collapse to bucket 0, preserving the existing non-IP behaviour. The within-tin intra-flow and intra-host hashing inherited from patched `FqCobaltQueueDisc` is already family-agnostic and is not touched by this assertion. Verified by: `TestS17v6FlowHashBulkCounter` in `diffserv-cake-q15-test-suite.cc`, which enqueues five distinct IPv6 flows and confirms each is assigned a distinct bucket (live count = 5). IPv4 flow distinctness through the same code path is covered by `TestCake_LiveBulkCounterTracksConcurrentFlows`, and the non-IP collapse to bucket 0 is enforced by the `GetUint8Value(IP_DSFIELD)` guard.

### S-17.35–17.38: Host-isolation mode assertions *(retired)*

**S-17.35–17.38** Retired with the `DsHostIsolatedFqCobalt` wrapper — see *Retired host-isolation assertions* at the end of §S-17.

### S-17.47: Per-flow counter accessor on the tin-shaping dispatcher

**S-17.47** [↪ I-7.2] After enqueuing M packets across N distinct flows (5-tuple) into a single tin slot s, `TinShaperDispatcher::GetPerFlowStats(s, f, edge)` for every active flow `f` (where `f` is the flow's index in the inner queue disc's `QueueDiscClass` list) shall return:

- `bytesEnqueued` matching the cumulative wire-byte count admitted to that flow's per-flow queue.
- `pktsEnqueued` matching the count of packets enqueued to that flow.
- `bytesRemaining` matching the live backlog (in bytes) of that flow's per-flow queue at call time.
- `pktsDropped` and `pktsMarked` reflecting the inner per-flow `QueueDisc::Stats::nTotalDroppedPackets` / `nTotalMarkedPackets`.

Aggregated across all `f` in `[0, inner->GetNQueueDiscClasses())`, the totals shall match the sum of inner per-flow `nTotalEnqueuedBytes`. Out-of-range slot, slot whose inner is non-FQ (no `QueueDiscClass` list), or out-of-range `f` shall return a zero-initialized `PerFlowStats` rather than aborting.

### S-17.48: Per-host counter accessor *(retired)*

**S-17.48** Retired with the `DsHostIsolatedFqCobalt` wrapper — see *Retired host-isolation assertions* at the end of §S-17.

### S-17.45: Q-15.10 RRUL latency fixture — substrate-replicated cake-rrul scenario

**S-17.45** [↪ I-7.2] An EXTENSIVE-tier ns-3 test fixture in `src/ns-3/test/diffserv-cake-q15-test-suite.cc` shall replicate the cake-rrul scenario (50 Mbit/s bottleneck, 80 ms base RTT, 4 saturating TCP up + 4 saturating TCP down + 3 EF UDP probes at 200 ms cadence, 60 s duration, cake::Helper RateBased shaper, bottleneck device TX queue capped at one packet so the queue disc owns the bottleneck queue) in-process and assert, over the [10 s, 60 s] measurement window, both Q-15.10 gates: the induced EF-probe latency (p99 OWD − min OWD) below `kQ15_10_RrulFig9P99LatencyCeilingMs`, and the floor-sanity bound on min OWD that guards bottleneck ownership. The former empirical-band escape clause is retired alongside Q-15.2's; both gates are enforced.

### S-17.46: Q-15.11 UDP cross-traffic isolation fixture — Voice-vs-BE isolation ratio

**S-17.46** [↪ I-7.2] An EXTENSIVE-tier ns-3 test fixture in `src/ns-3/test/diffserv-cake-q15-test-suite.cc` shall offer a saturating UDP CBR flow on the Best-Effort tin (DSCP 0) alongside a saturating TCP flow plus three EF UDP probes on the Voice tin (DSCP 46) over a single 50 Mbit/s / 80 ms bottleneck for 60 s, and assert the isolation ratio (UDP-tin achieved throughput in Mbit/s, divided by EF-probe OWD jitter in milliseconds, jitter = `p99(OWD) − min(OWD)`) strictly greater than `kQ15_11_IsolationRatioMbpsPerMs` (= 5.0). Sanity preconditions: at least 100 EF probe samples in the measurement window; UDP achieved throughput at least 5 Mbit/s.

### S-17.49: CAKE conservative preset — defensive overhead defaults

**S-17.49** [↪ I-2] `cake::Helper::SetAsCakeConservative(edge)` invoked after a `SetAsCake*` profile composed with `useInnerTbfShaping=true` shall apply the Linux `tc-cake(8)` `conservative` preset: per-packet overhead 48 bytes, minimum-packet-unit floor 64 bytes, ATM cell framing disabled (`atm=false`). The call shall be observably equivalent to `ConfigureLinkLayerOverhead(edge, 48, false, 64, false)` and shall trigger the same `gamma`-downscale of every per-tin TBF rate as S-17.30, with the bimodal-mix `gamma = E[wire_bytes(s)]/E[s]` evaluated at `(overhead=48, atm=false, mpu=64)` over `{(64B, 0.5), (1500B, 0.5)}`. The post-call relative error of every per-tin `TbfQueueDisc::Rate` against the closed-form expected rate `share × totalRate / gamma` shall be within ±0.5 %.

### S-17.50: Host-isolation Flowblind mode *(retired)*

**S-17.50** Retired with the `DsHostIsolatedFqCobalt` wrapper — see *Retired host-isolation assertions* at the end of §S-17.

### S-17.51: CAKE diagnostic text dump — `tc -s qdisc show cake` mirror

**S-17.51** [↪ I-7] `cake::Helper::PrintTcStats(os, edge)` (delegating to `cake::StatsFormatter::Print`) shall write to @p os a human-readable diagnostic dump for a CAKE-composed `EdgeQueueDisc` whose section ordering and section-key vocabulary mirrors iproute2 `q_cake.c @ 87c66f79d8b0::cake_print_xstats` (line 617), frozen under `provenance/linux-iproute2-87c66f79d8b0/`. The output shall contain, at a minimum: a header line beginning with `qdisc cake` and including the `tins N` token; an aggregate `Sent` line with `bytes`, `pkt`, `dropped`, `requeues` keys; an aggregate `backlog` line; one per-tin block per populated inner slot containing the keys `tin <i> kind=`, `thresh`, `bytes_enqueued`, `bytes_dequeued`, `drops`, `marks`, `ever_seen`, and `backlog`. The `ever_seen` field deliberately differs from the Linux name `bulk_flow_count`: stock ns-3 `FqCobaltQueueDisc` exposes only an append-only class list (`GetNQueueDiscClasses()`), so the substrate cannot honestly report the live `bulk_flow_count` Linux's `sch_cake` tracks. The discrepancy is documented in the per-tin output line itself and in the formatter's class Doxygen. The output is structural, not byte-exact: future iproute2 cosmetic changes (whitespace, decimal precision, label re-ordering within a section) shall not regress the fixture. A null edge pointer shall produce a single-line diagnostic of the form `qdisc cake (null)` and return without aborting.

### S-17.52: Path-α/β/γ shaper comparison panel

**S-17.52** [↪ I-7] An EXTENSIVE-tier ns-3 fixture in `src/ns-3/test/diffserv-cake-q15-test-suite.cc` shall drive a single 4-tin TCP saturation scenario (the existing `Q15Scenario6Run` helper, 1 Gbps P2P, 100 Mbit/s aggregate cap, 4 long-lived BulkSend flows over 30 s) through all three `cake::Helper::ShaperMode` paths — α (`TokenBucket`, default in-dispatcher token-bucket gate; the helper composes this with `enableTinShaping=false` so neither per-tin TBF caps nor per-tin token-bucket gates are wired in), β (`RateBased`, virtual-clock per-tin shaper plus global clock), γ (`TbfInner`, mainline `TbfQueueDisc` as per-tin inner via `patches/ns3/0004`) — and characterise the path-choice landscape via aggregate-throughput ratios. The fixture shall assert: (a) `|β/γ − 1| ≤ 0.02` (β tracks γ within ±2 % under the symmetric regime — restated for completeness, identical to S-17.41); (b) `α/γ > 1.5` AND `α/β > 1.5` (α materially diverges from β/γ by more than 1.5× under the default helper composition because `enableTinShaping=false` lets traffic through at link rate rather than enforcing the 100 Mbit/s aggregate cap — this is the reviewer-defensive "when does path choice matter?" characterisation, demonstrating that α under the default helper is NOT a drop-in cap-enforcing replacement for β/γ); (c) the S-17.44 bound restated within the panel: under `RateBased` (β) the aggregate egress saturates at the cap (`< 102 Mbit/s`, `> 95 Mbit/s`). Per-tin gating for α (`enableTinShaping=true`, in-dispatcher `TinTokenBucket` caps) requires either a helper-API extension or a fixture that calls `SetAsCakeDiffserv4` directly with `enableTinShaping=true`; both are deferred and documented inline in the fixture's Doxygen block per the calibration discipline established by S-17.45 / Q-15.7.

### S-17.53: CAKE `autorate-ingress` API contract + no-op default

**S-17.53** [↪ I-7] `cake::Helper::SetEnableAutorateIngress(bool)` and the paired `GetEnableAutorateIngress() const` accessor shall implement the Linux `tc-cake(8)` `autorate-ingress` flag at API level, mirroring the flag's authoritative definition in the `tc-cake(8) @ 87c66f79d8b0` man page's OTHER PARAMETERS section (line 600) and the parser entry in iproute2 `q_cake.c @ 87c66f79d8b0::cake_parse_opt` (line 91), both frozen under `provenance/linux-iproute2-87c66f79d8b0/`. The contract is structural and asserted by a QUICK-tier ns-3 fixture in `src/ns-3/test/diffserv-test-suite.cc` named `CakeAutorateIngressApiContractTest`. After invoking `SetEnableAutorateIngress(true)`: (a) `GetEnableAutorateIngress()` returns `true`; (b) `GetAutorateIngressHook()` returns a non-null pointer to a `AutorateIngressHook` whose `ComputeRateDelta(N)` returns `0` for at least three values spanning `{1 000 000, 100 000 000, 1 000 000 000}` bps; (c) under a deterministic 4-tin diffserv4 scenario driving a fixed packet sequence through a CAKE-composed `EdgeQueueDisc`, the per-packet enqueue and dequeue counts captured with the flag enabled (no-op hook installed) shall equal those captured with the flag disabled (the byte-identity contract). After invoking `SetEnableAutorateIngress(false)`: `GetEnableAutorateIngress()` returns `false` and `GetAutorateIngressHook()` returns `nullptr`. The default selector is the no-op `AutorateIngressHook`, so the (a)–(c) contract above characterises the flag with no rate adaptation — what `SetEnableAutorateIngress(true)` installs absent an explicit implementation selector; S-17.53 continues to pin this no-op default and its byte-identity. The closed-loop estimator — the peak-arrival-bandwidth EWMA folded inline in Linux `sch_cake.c::cake_enqueue` (a per-window capacity estimate, not an RTT-trend tracker) — is implemented in `cake::LinuxAutorateHook` and attached via `cake::Helper::SetAutorateImpl(Linux)` on the path-β shaper; its upward convergence, re-adaptation, byte-exactness to `cake_enqueue`, and slow-decay behaviour are asserted by S-17.63–S-17.67, not by S-17.53.

### S-17.54: `cake::Helper::SetAsCakeAlphaTinShaped` enables α tin-shaping

**S-17.54** [↪ I-7] `cake::Helper::SetAsCakeAlphaTinShaped(edge, totalRate, ...)` shall compose path α (in-dispatcher TokenBucket across tins) with per-tin caps enabled (`enableTinShaping=true`) while preserving the TokenBucket dispatcher choice (`useInnerTbfShaping=false`). The contract is asserted by an EXTENSIVE-tier ns-3 fixture in `src/ns-3/test/diffserv-cake-q15-test-suite.cc` that drives the 4-tin TCP saturation scenario from S-17.52 (1 Gbps P2P, 100 Mbit/s aggregate cap, 4 long-lived BulkSend flows over 30 s) under three compositions: α with tin-shaping enabled (this preset), β (`RateBased`), γ (`TbfInner`). The fixture shall assert that aggregate goodput under α-with-tin-shaping falls within ±5 % of γ (`|α/γ − 1| ≤ 0.05`) AND within ±5 % of β (`|α/β − 1| ≤ 0.05`) at the 100 Mbit/s cap. This inverts S-17.52's deliberate divergence assertion: with tin-shaping enabled, α joins β and γ in cap-enforcement; without it (the default helper composition asserted by S-17.52), α diverges by more than 1.5×. The ±5 % tolerance reflects measured steady-state deviations of < 1 % for α/β and α/γ pairs; the bound provides regression headroom without admitting silent loss of the cap-enforcing equivalence.

### S-17.55: cake-host-fairness-sweep probe reproduces 4-vs-1 Triple host-isolation CUBIC baseline

**S-17.55** [↪ I-7.2] An EXTENSIVE-tier ns-3 test fixture in `src/ns-3/test/diffserv-cake-host-fairness-smoke-test.cc` shall replicate the parameterised host-fairness probe configuration emitted by the `cake-host-fairness-sweep` example (4 bulk TCP CUBIC flows from host A; 1 bulk TCP CUBIC flow from host B; 100 Mbit/s bottleneck at 20 ms one-way delay; 30 s duration; TCP socket buffers sized above the path bandwidth-delay product, `SndBufSize = RcvBufSize = 4 MB` ≫ the 100 Mbit/s × 44 ms ≈ 550 KB BDP, so neither host is receive-window-limited; `cake::Helper::SetAsCakeDiffserv4` composed with `enableTinShaping=true` and `enableHostIsolation=true`; per-tin inner mainline `FqCobaltQueueDisc` with `EnableHostIsolation=true` and `HostIsolationMode=Triple`) and assert that `share_A = goodput_A / (goodput_A + goodput_B)` falls within the band `[0.625, 0.665]` over a single `RngRun=1` replica. The band brackets the measured single-replica goodput share of 0.6466 with margin for single-replica RNG and platform-FP jitter. This is the buffer-adequate host-isolation share: it sits between per-flow-fair (0.80) and per-host-fair (0.50), measured apples-to-apples with a transport that autotunes its receive window. At the ns-3 default 128 KB socket buffers the lone host-B flow is receive-window-limited (its window pins ≈ 4× below the BDP) and `share_A` is inflated to ≈ 0.76 — a socket-buffer artefact, not a host-isolation property. This fixture guards against probe-side regressions on the version-controlled `cake-host-fairness-sweep` example.

### S-17.56: RateBased single-tin work conservation

**S-17.56** [↪ I-7] Under `cake::Helper::ShaperMode::RateBased` with a 100 Mbit/s global cap and uniform 50 Mbit/s per-tin rates, a single saturating UDP flow classified to one tin shall achieve aggregate egress > 95 Mbit/s and < 102 Mbit/s over a 20 s window (measured 98.11 Mbit/s). Per Linux `cake_dequeue` shaped-mode semantics (`sch_cake.c:2102-2131` at the frozen provenance excerpt), per-tin clocks demote selection priority and never cap throughput; the global clock pair (primary plus the 1.5× failsafe companion, `sch_cake.c:1543-1561`) is the only hard gate. Hard per-tin ceilings remain the contract of the `TokenBucket` (α, S-17.54) and `TbfInner` (γ) shaper paths.

### S-17.57: RateBased schedule-meeting priority selection

**S-17.57** [↪ I-7] Under the same composition with 25 Mbit/s per-tin rates, a Voice-tin UDP flow offered 10 Mbit/s (inside its allowance) concurrent with a Best-Effort-tin flow offered 120 Mbit/s shall be delivered at > 9 and < 12 Mbit/s while the Best-Effort flow takes the work-conserving remainder (> 80 Mbit/s) and the aggregate stays inside (95, 102) Mbit/s (measured: Voice 9.95, Best-Effort 88.16, aggregate 98.11). This realises the highest-priority-tin-meeting-schedule selection rule with the diffserv4 ascending priority Best-Effort < Bulk < Video < Voice (Linux raw tin-index scan order; the dispatcher receives the layout's priority permutation from the helper because the slot order differs from Linux's tin order in the Bulk/Best-Effort pair).

### S-17.58: Integrated shaped single-tin work conservation

**S-17.58** [↪ I-7] Under the integrated shaped composition (`cake::Helper::SetAsCakeDiffserv4` stacked with `cake::Helper::SetBandwidth` at 100 Mbit/s — the aggregate clock pair and schedule-meeting tin selection of `ShapedTinDispatcher` over per-tin mainline `FqCobaltQueueDisc` inners), a single saturating UDP flow classified to one tin shall achieve aggregate egress > 95 Mbit/s and < 102 Mbit/s over a 20 s window (measured 97.45 Mbit/s). These are the S-17.56 shaped-mode properties (`sch_cake.c:2102-2131`, `sch_cake.c:1543-1561` at the frozen provenance excerpt) carried through the edge composer: per-tin clocks demote, the global clock pair is the only hard gate, and in-tin scheduling is the inner disc's per-flow DRR + Cobalt.

### S-17.59: Integrated shaped schedule-meeting priority selection

**S-17.59** [↪ I-7] Under the same integrated composition (diffserv4 tin rates derived from the 100 Mbit/s bandwidth: rate, rate>>4, rate>>1, rate>>2), a Voice-tin UDP flow offered 10 Mbit/s (inside its 25 Mbit/s allowance) concurrent with a Best-Effort-tin flow offered 120 Mbit/s shall be delivered at > 9 and < 12 Mbit/s while the Best-Effort flow takes the work-conserving remainder (> 80 Mbit/s) and the aggregate stays inside (95, 102) Mbit/s (measured: Voice 9.95, Best-Effort 86.53, aggregate 96.48). This is the S-17.57 selection rule through `EdgeQueueDisc` + `ShapedTinDispatcher`, with the helper-provided ascending-priority permutation {1,0,2,3}.

### S-17.60: Integrated shaped diffserv3 work conservation

**S-17.60** [↪ I-7] Under the integrated shaped composition for the diffserv3 layout (`cake::Helper::SetAsCakeDiffserv3` stacked with `cake::Helper::SetBandwidth` at 100 Mbit/s — tin rates rate, rate>>4, rate>>2 for BE / Bulk / Latency-Sensitive per `cake_config_diffserv3`, `sch_cake.c:2553-2583` at the frozen provenance excerpt, dispatched from the tin profile recorded on the edge at compose time), a single saturating UDP flow classified to the Latency-Sensitive tin (allowance rate>>2) shall achieve aggregate egress > 95 Mbit/s and < 102 Mbit/s over a 20 s window (measured 97.45 Mbit/s; 119.4 on the unshaped composition). Ascending tin priority is the raw Linux tin-index order carried into this layout's slot numbering ({2,0,1}: BE < Bulk < Latency-Sensitive — the shaped-mode scan walks tin indices ascending and the last schedule-meeting tin wins, `sch_cake.c:2110-2123`).

### S-17.61: Integrated shaped diffserv8 work conservation

**S-17.61** [↪ I-7] The same property through the diffserv8 layout (`cake::Helper::SetAsCakeDiffserv8` + `SetBandwidth`; geometric tin-rate ladder, each tin at 7/8 of the previous per `cake_config_diffserv8`, `sch_cake.c:2467-2512`; identity priority permutation per `normal_order`): a single saturating UDP flow classified to the Network Control tin (CS6 → tin 7 under the kernel `diffserv8[]` table; allowance (7/8)^7 ≈ 39.3% of the cap) shall achieve aggregate egress > 95 Mbit/s and < 102 Mbit/s over a 20 s window (measured 97.45 Mbit/s; 119.4 unshaped). Per-tin clocks demote; the global clock pair remains the only hard gate.

### S-17.62: diffserv3 unshaped contention follows the kernel quantum ladder

**S-17.62** [↪ I-7] On a `cake::Helper::SetAsCakeDiffserv3`-composed edge under the default work-conserving dispatcher, with the Latency-Sensitive and Best-Effort tins both saturated by equal-size packets whose wire size divides the per-tin DRR quanta exactly, the served-byte ratio Latency-Sensitive : Best-Effort shall equal 1:4 (±2% over the drained window) — the `cake_config_diffserv3` quantum ladder (quanta 1024 / 64 / 256 for BE / Bulk / Latency-Sensitive, `sch_cake.c:2576-2580` at the frozen provenance excerpt; measured 0.25, 0.5 before the share reconciliation). The Bulk tin is excluded from the gate: its DRR quantum is floored at one MTU, a recorded quantum-derivation divergence from the kernel's sub-MTU quanta.

### S-17.63: Autorate-ingress upward convergence

**S-17.63** [↪ I-16] An EXTENSIVE-tier ns-3 fixture (`TestCake_AutorateConvergesAndReadapts` in `src/ns-3/test/diffserv-cake-q15-test-suite.cc`) shall drive a single-packet arrival train into a `RateBasedShaperDispatcher` with a `LinuxAutorateHook` installed and a bootstrap aggregate rate below the offered bottleneck. Packets are spaced so each measurement window carries bytes ÷ duration equal to the target bottleneck R₁, plus a small deterministic timing jitter (0–1884 ns). Over a 2 s phase the aggregate global-clock rate (`RateBasedShaperDispatcher::GetGlobalRateBps()`) shall converge upward to within ±12 % of `(R₁ · 15) >> 4` (15/16 of R₁; measured ≈ 9.50 Mbit/s at R₁ = 10 Mbit/s, ≈ +1.3 % from the IPv4 header counted in the wire length — `AUTORATE_CONVERGE_SUM` harvest). The jitter is load-bearing: a perfectly periodic stream drives the inter-arrival EWMA to lock onto the period exactly, after which the window-close condition stops firing and the estimate freezes (`sch_cake.c:1897` at the frozen provenance excerpt); a few microseconds of jitter emulates the natural burstiness the kernel relies on.

### S-17.64: Autorate-ingress upward re-adaptation

**S-17.64** [↪ I-16] Under the same fixture, a second 2 s phase raising the offered bottleneck to R₂ > R₁ shall re-adapt the aggregate rate upward to within ±12 % of `(R₂ · 15) >> 4` (measured 19.00 Mbit/s at R₂ = 20 Mbit/s — `AUTORATE_CONVERGE_SUM` harvest). Upward adaptation is the kernel's fast direction (the `avg_peak_bandwidth` attack EWMA uses shift 2, α = ¼, when a higher window is observed, `sch_cake.c:1905`); downward adaptation is slow-by-design (decay shift 8, α = 1/256) and is characterised separately, not gated here.

### S-17.65: Autorate-ingress peak-bandwidth estimator is byte-exact to cake_enqueue

**S-17.65** [↪ I-16] The `LinuxAutorateHook` peak-bandwidth estimator shall be byte-exact to `sch_cake.c::cake_enqueue`: the two-term `cake_ewma` rounding form (`sch_cake.c:1373`); the asymmetric attack/decay shifts — shift 2 (α = ¼) when the new sample exceeds the running average, shift 8 (α = 1/256) otherwise (`:1889-1906`); the seed of `avg_peak_bandwidth` from the configured aggregate rate (`:2893`); the 1-second inter-arrival cap (`:1885`); the accumulate-window-bytes-first ordering (`:1871`); and the reconfigure target `(avg_peak_bandwidth · 15) >> 4` (`:1913`). The internal estimate is held in the kernel's native bytes-per-second; the bits-per-second conversion happens only at the seed input and the target output (the ×8 following the `>> 4`, kernel order). A focused fixture (`TestCake_AutorateEwmaMatchesKernel`) shall drive deterministic window sequences directly through `OnEnqueue(bytes, time)` (no queue-disc item, so no IPv4 header inflates the wire length) and assert the reported target equals both an independent in-test re-evaluation of the kernel recurrence and two hand-derived literal anchors (one-window 7 031 248 bps; twelve-window 10 953 040 bps at a 2 Mbit/s seed). The assertion is exact equality, not a band, and covers the `cake_ewma` form, the seed, the 1-second cap, and the byte-accumulation order against `cake_enqueue`.

### S-17.66: per-host bulk-flow count tracks the SPARSE→BULK transition

**S-17.66** [↪ I-7] On a mainline `FqCobaltQueueDisc` with `EnableHostIsolation=true`, a flow shall contribute to its host's bulk-flow count only after it stays backlogged across a dequeue rotation — when its DRR deficit is first exhausted with packets still queued (the SPARSE→BULK transition, `cake_dequeue`, `sch_cake.c:2157-2165`; the SPARSE_WAIT→BULK re-arming on a later enqueue, `sch_cake.c:1934-1943`, at the frozen provenance excerpt). A flow that drains within its first quantum stays sparse and never increments any per-host bulk-flow counter, so `host_load` (`max(1, srchost_bulk_flow_count, dsthost_bulk_flow_count)`) is built only from genuinely backlogged flows. Verified white-box: with the per-host hashes snapshotted at activation and the increment deferred to the promotion, a single backlogged flow's per-host bulk count is 0 on activation, 1 at the promotion, and 0 again after full drain; a single sub-quantum packet never reaches 1.

### S-17.67: Autorate-ingress downward adaptation is slow-by-design

**S-17.67** [↪ I-16] An EXTENSIVE-tier fixture (`TestCake_AutorateDownwardAdaptation`) shall seed the peak estimate above the offered rate (20 Mbit/s) and feed a lower single-packet stream (5 Mbit/s) with the natural-burstiness jitter, driven directly through `OnEnqueue(bytes, time)`. Because observed windows fall below the running average, the peak EWMA decays with shift 8 (α = 1/256, `sch_cake.c:1906`) — the deliberate slow direction that keeps a transient dip from collapsing the shaped rate. The reconfigure target shall therefore remain above 8 Mbit/s after a 0.5 s horizon (decay barely begun) and settle into a loose band of 4–6 Mbit/s (around 5 Mbit/s · 15/16 = 4.6875 Mbit/s) only over a 30 s horizon. This is a characterization with a loose band, not a tight gate; the fast upward direction is gated by S-17.63 / S-17.64.

### S-17.68: Bottleneck rate-jitter drives the host-isolation share to a host-fair floor

**S-17.68** [↪ I-7.2] An EXTENSIVE-tier ns-3 fixture in `src/ns-3/test/diffserv-cake-host-iso-jitter-floor-test.cc`, paired with the version-controlled `cake-host-iso-jitter-floor` example, shall drive the (4, 1) split-destination CUBIC host-isolation scenario of S-17.55 (100 Mbit/s bottleneck, 20 ms one-way delay, 4 MB socket buffers above the path bandwidth-delay product, MSS 536, `cake::Helper::SetAsCakeDiffserv4` with `enableTinShaping=true` and `enableHostIsolation=true`) under a shared bottleneck rate-jitter applied to **both** bottleneck directions — the forward (data) and reverse (acknowledgement) devices — redrawing the link rate as `nominal · (1 + U[−pct, +pct])` every `jitterPeriodMs`, floored at `0.05 · nominal` so the link never stalls, mean rate preserved. With `jitterPct = 0` the many-flow-host share shall fall in `[0.625, 0.665]` (the S-17.55 baseline, 0.6466). With `jitterPct = 0.5` at a 0.5 ms period over a single `RngRun=1` replica, `share_A` shall fall in `[0.502, 0.542]` (measured 0.5218), below the deterministic baseline and bracketing the matched-Linux offload-off value (0.533), at ≈ 98 % of the deterministic throughput. Over a seed ensemble the jitter share is ≈ 0.55 ± 0.02; 0.522 is the `RngRun=1` value, the distribution floor. The jitter is a measurement device, not a CAKE behaviour: it stands in for the hardware interrupt and softirq dispatch timing a deterministic event schedule lacks, and the `jitterPct = 0` oracle is byte-identical to S-17.55.

### S-17.69: Per-host backlog occupancy is far less asymmetric than the byte-share

**S-17.69** [↪ I-7.2] Under the same fixture's deterministic run, per-host backlog occupancy — the fraction of time each host has at least one packet queued at the bottleneck, sampled at 1 kHz from the root queue disc's `Enqueue` and `Dequeue` trace sources (segmentation offload off, so enqueue and dequeue are one-to-one) — shall show both hosts backlogged a substantial fraction (each > 0.30) with an occupancy ratio (host A / host B) below 1.30 (measured 1.18). The occupancy ratio is far below the byte-share ratio (0.647 / 0.353 ≈ 1.83), so the deterministic share excess is a within-contention allocation effect rather than a per-host occupancy asymmetry. This is the simulator-side cross-check of the matched-Linux measurement, whose per-host occupancy ratio is ≈ 1.00 (97.5 % / 97.5 %). The conclusion is robust to the occupancy definition: a packets-present metric (this fixture) reads mildly asymmetric (1.18) and an active-set metric reads near-symmetric (1.02), both far below the share ratio.

### Retired host-isolation assertions

The following §S-17 slots asserted behaviour of the `DsHostIsolatedFqCobalt` host-isolation wrapper, since removed. The wrapper's nested per-host outer-DRR over inner FQ buckets is superseded by native host isolation on mainline `FqCobaltQueueDisc` (per-flow DRR + reciprocal-divide host-load quantum modulation; `patches/ns3/0006` + `patches/ns3/0016`). The numbers are reserved, not reassigned.

- **S-17.23** — `MaxHostPairs` LRU bucket recycling. Wrapper-internal; mainline uses fixed per-host slot tables with set-associative tag-overwrite (no count cap, no LRU).
- **S-17.35 / S-17.36** — single-collapse `Srchost` / `Dsthost` / `Hosts` modes. Never ported; the mainline `HostIsolationMode` enum is `{Flowblind, Flows, DualSrcHost, DualDstHost, Triple}` (the `Dual*` modes are functional per-side modulation, not aliases).
- **S-17.37** — `Flowblind` / `Flows` nested-inner-type contract. Both modes survive in mainline, but the per-bucket inner-disc-type and `GetActiveHostPairs()` framing was wrapper-specific; mainline tracks per-host counts without a nested dispatcher.
- **S-17.38** — eight-value enum round-trip with `Dual*` aliasing. Mainline has five modes with functional `Dual*`; the round-trip is covered by the `fq-cobalt-queue-disc` test suite.
- **S-17.48** — `GetPerHostStats` → `DsPerHostStats` accessor. Wrapper-only; retired with the class.
- **S-17.50** — `Flowblind` inner-is-`CobaltQueueDisc` single-bucket contract. Wrapper-specific; mainline `Flowblind` disables host tracking on the single `FqCobaltQueueDisc` (no nested inner).

Mainline mode behaviour and the five-value `HostIsolationMode` round-trip are exercised by the `fq-cobalt-queue-disc` test suite; end-to-end host-fairness is guarded by `diffserv-cake-host-fairness-smoke-test.cc` (S-17.55), `diffserv-cake-host-iso-jitter-floor-test.cc` (S-17.68 / S-17.69), and `diffserv-cake-host-iso-phase-1-test-suite.cc`.

## S-flent-sink-host-column-emitted

> **References:** I-10

After a 2-second smoke run of `cake-host-isolation --isolation=triple
--length=2 --output=<temp_dir>`, every `tcp_up_flow*.csv` in
`<temp_dir>` has a 4-column header `t,bytes_delta,goodput_mbps,host`
and ≥1 data row.

## S-flent-sink-host-attribution-correct

> **References:** I-10

After the same 2-second smoke run, every data row of
`tcp_up_flow{0,1,2,3}.csv` has `host == "A"`, and every data row of
`tcp_up_flow4.csv` has `host == "B"`.

## S-flent-sink-backwards-compat-no-hostid

> **References:** I-10

After a 2-second smoke run of any FlentCsvSink-using binary that
does NOT pass a hostId (e.g., `cake-tcp-4up-squarewave --length=2
--output=<temp_dir>`), the emitted `tcp_up_flow*.csv` files have the
same 4-column header `t,bytes_delta,goodput_mbps,host` with an empty
trailing `host` field per row.

### S-meter-base-trace-registered

**S-meter-base-trace-registered** [↪ I-8] `Meter::GetTypeId()` shall advertise a trace source named `MeterColour`
with signature `(Colour, uint32_t, Time)` discoverable via
`TypeId::LookupTraceSourceByName`. The signature shall match the
declared `TracedCallback` member precisely.

### S-meter-trace-srtcm

**S-meter-trace-srtcm** [↪ I-8] When a controlled input sequence is fed through a `SrTcmMeter` instance
with known expected colour sequence (per RFC 2697 §2.2), the
`MeterColour` trace shall fire exactly once per input packet, in order,
with the expected `(colour, classId)` pair. `classId` matches the
`PolicyEntry.classId` field of the meter's input.

### S-meter-trace-trtcm

**S-meter-trace-trtcm** [↪ I-8] As S-meter-trace-srtcm, with `TrTcmMeter` and RFC 2698 §2.2 expected
colour sequence.

### S-meter-trace-tsw2cm

**S-meter-trace-tsw2cm** [↪ I-8] As S-meter-trace-srtcm, with `Tsw2cmMeter` and the project's TSW2CM
reference vector (sources: `src/ns-2.29/diffserv/dsPolicy.cc` TSW logic).

### S-meter-trace-tsw3cm

**S-meter-trace-tsw3cm** [↪ I-8] As S-meter-trace-srtcm, with `Tsw3cmMeter` and RFC 2859-style expected
colour sequence.

### S-meter-trace-byteacct

**S-meter-trace-byteacct** [↪ I-8] As S-meter-trace-srtcm, with `ByteAcctMeter`. Expected colour sequence
sourced from the existing byte-acct unit tests.

### S-meter-trace-fw

**S-meter-trace-fw** [↪ I-8] As S-meter-trace-srtcm, with `FwMeter`. classId defaults to 0 if the
framework-meter has no per-class state; verify against actual class
field during execution.

### S-meter-trace-tokenbucket

**S-meter-trace-tokenbucket** [↪ I-8] As S-meter-trace-srtcm, with `TokenBucketMeter`. `classId=0`
unconditionally (single-class meter).

### S-meter-trace-dumb

**S-meter-trace-dumb** [↪ I-8] As S-meter-trace-srtcm, with `DumbMeter`. All emissions are
`Colour::GREEN, classId=0`.

### S-example-1-perclass-owd

**S-example-1-perclass-owd** [↪ I-9] After a 6-second smoke run of `diffserv-example-1`, the run directory
shall contain `OWD-ef.tr` and `OWD-be.tr`. Each file shall be
non-empty, parseable as whitespace-separated `time owd_seconds` rows,
with `time` monotone non-decreasing.

### S-example-1-perclass-flowrate

**S-example-1-perclass-flowrate** [↪ I-9] After a 6-second smoke run, `FlowRate.csv` shall exist with the header
row `time,classId,rate_kbps`, contain ≥10 sample rows, expose two
distinct `classId` values (0 = EF, 1 = BE) at each timestamp, and
report `rate_kbps` values within the plausibility band [0, link
capacity in kbps].

### S-example-1-metercolour-aggregate

**S-example-1-metercolour-aggregate** [↪ I-9] After a 6-second smoke run, `MeterColour.csv` shall exist with header
row `time,classId,green,yellow,red`, contain ≥10 sample rows, and
report non-negative integer counts whose per-class running sum across
windows is consistent (within ±1 packet) with the total ingress packet
count observed at the meter.

## S-L4S: L4S client — DualPI2 conformance (intent: I-15)

Structural assertions for the L4S client. S-L4S.1–.12 are backfilled one-line
entries for the pre-existing tests in `l4s-routing-test.cc` (this section was
created 2026-06-10; the tests predate it). S-L4S.13–.15 are the conformance
vectors added by the L4S validation audit.

- **S-L4S.1** ECT(1) and CE packets route to the L4S sub-queue; NotECT and
  ECT(0) route to the classic sub-queue (RFC 9331 §5.1).
- **S-L4S.2** `L4sQueueIdx` setter/getter round-trips.
- **S-L4S.3** With the L4S sub-queue empty, the classic path sees zero coupled
  drop (`p' = 0` ⇒ `p_C = 0`).
- **S-L4S.4** Coupling-map invariant at pinned `p' = 0.2`, `k = 2`: observed
  classic coupled-drop ratio approaches `p_C = p'² = 0.04` and observed L4S mark
  ratio approaches `p_CL = k·p' = 0.4` (RFC 9332 §2.1 eq. (1); App. A.1 Fig. 6
  lines 4–5), statistically over 4000 draws and exactly via the snapshot
  accessors. *(Erratum 2026-06-10: previously asserted `p_C = (k·p')² = 0.16`,
  calibrated to an implementation misreading of eq. (1) and labelled "RFC 9332
  default". Corrected together with the implementation; see the superseding ADR.)*
- **S-L4S.5** CE-mark idempotence: an already-CE packet is not re-marked and
  does not double-count the mark counter (RFC 9331 §5).
- **S-L4S.6** Immediate-mark step: when the L4S head sojourn reaches the L4S
  target, `p_L` saturates to 1 (step-AQM; the RFC 9332 App. A.1 ramp with
  `range → 0`, threshold at the 1 ms L4S target).
- **S-L4S.7** Controller step response (qualitative): under sustained
  over-target classic sojourn, `p'` starts at 0, becomes positive, grows
  monotonically, and respects the [0, 1] clamp.
- **S-L4S.8** Controller no-drift: with no traffic, `p'` stays pinned at 0
  across ticks (negative error clamps).
- **S-L4S.9** `CoupledOnly` mode bypasses the parent WRED pipeline; coupled
  drop is the sole classic AQM.
- **S-L4S.10** The coupled scheduler honours L4S priority without starving the
  classic queue.
- **S-L4S.11** The coupled scheduler does not deadlock when only the L4S queue
  is occupied.
- **S-L4S.12** Mainline `FqCoDelQueueDisc` works as the inner classic AQM;
  composer-level coupled drops never reach the inner disc.

### S-L4S.13 Golden controller vector (RFC 9332 App. A.1)

Deterministic 10-tick vector for the PI² controller and the coupling maps,
derived from RFC 9332 Appendix A.1 only (never from the implementation):

- Constants (Fig. 2): `target = 15 ms`, `Tupdate = 16 ms`, `alpha = 0.16 Hz`,
  `beta = 3.2 Hz` (RFC-stated defaults for `RTT_max = 100 ms`), `k = 2`.
- Update (Fig. 6 lines 2–3): `p' += alpha·(curq − target) + beta·(curq − prevq)`,
  `curq` = classic head sojourn, clamp to [0, 1].
- Input: controller ticks at `t = 16n ms`; queue empty through the first tick
  (`curq = 0`, `prevq` sampled 0); one classic packet enqueued at `t = 24 ms`
  and never dequeued, so `curq = (16n − 24) ms` thereafter.
- Expected `p'` per tick (hand-derivable; exact doubles in the test):
  n=1: 0; n=2: 0.02448; n=3: 0.07712; n=4: 0.13232; n=5: 0.19008;
  n=6: 0.25040; n=7: 0.31328; n=8: 0.37872; n=9: 0.44672; n=10: 0.51728.
- Map probes (Fig. 6 lines 4–5, exact arithmetic): at forced `p' = 0.2, k = 2`:
  `p_C = 0.04`, `p_L = 0.4`; at `p' = 0.2, k = 4`: `p_C = 0.04`, `p_L = 0.8`
  (the cascade honours the configured `k`); at `p' = 0.6, k = 2`: `p_C = 0.36`,
  `p_L = min(1.2, 1) = 1.0`.
- Tolerance: `1e-9` absolute on `p'`; the simulation is RNG-free on this path
  and ns-3 `Time` is exact in ns, so the only error source is double-precision
  accumulation over ~30 FLOPs (≈1e-15); 1e-9 is six orders of headroom, not a
  statistical band. Map probes assert at `1e-9` likewise.

Verified by: `TestSL4s13GoldenControllerVector` in `l4s-routing-test.cc`.

### S-L4S.14 ECN codepoint-transition vector (RFC 9331 §5.1)

For each (input codepoint, controller state) pair, the dequeued packet's
codepoint and disposition shall be:

| input | lane | forced state | disposition | output ECN |
|---|---|---|---|---|
| ECT(1) | L4S | `p' = 0.6, k = 2` (⇒ `p_L = 1`) | forwarded, marked | CE |
| ECT(1) | L4S | `p' = 0` (⇒ `p_L = 0`) | forwarded, unmarked | ECT(1) — never demoted to ECT(0)/NotECT |
| CE | L4S | `p' = 0.6` | forwarded | CE (idempotent, no re-mark) |
| NotECT | classic | `p' = 1` (⇒ `p_C = 1`) | coupled-dropped — never CE-marked | n/a |
| ECT(0) | classic | `p' = 1` (⇒ `p_C = 1`) | coupled-dropped — never CE-flipped | n/a |
| ECT(0) | classic | `p' = 0` | forwarded, untouched | ECT(0) |

Only `Mark()` on the L4S lane may change a codepoint, and only to CE
(RFC 9331 §5.1; Briscoe draft §7.1).

Verified by: `TestSL4s14EcnCodepointTransitions` in `l4s-routing-test.cc`.

### S-L4S.15 DSCP preservation through both lanes (atomic DualQ)

Enqueue packets covering {EF, AF11, CS5, default} × {ECT(1), NotECT, ECT(0)}
with marking active on the L4S lane (`p'` forced high enough to CE-mark) and
WRED inert; on every dequeued packet the DSCP equals the DSCP it carried at
enqueue (`draft-briscoe-tsvwg-l4s-diffserv` §7.1: a DualQ "never alters the
DSCP"; classification is the outer classifier's job). The assertion
is behavioural (per-packet trace of dequeued headers), not structural.

Verified by: `TestSL4s15DscpPreservation` in `l4s-routing-test.cc`.

## S-l4s-piControl-fires-at-nominal-load (intent: I-12)

After running `diffserv-l4s-s2-equivalence` at its v1.7-tuned defaults for a short
sim (`simTime=3`), the emitted `coupling.csv` shall satisfy:

- `(pPrime > 0).sum() / len(pPrime) > 0.05` — controller fires on a non-trivial sample fraction
- For samples where `pPrime > 0`: the RFC 9332 coupling cascade (§2.1 eq. (1);
  App. A.1 Fig. 6 lines 4–5) holds approximately —
  `abs(pC - pPrime**2) / max(pC, 1e-6) < 0.10` (within 10% of formula prediction)
  AND `abs(pL - min(k*pPrime, 1)) / max(pL, 1e-6) < 0.10`
  where `k=2` (default coupling factor)

*(Erratum 2026-06-10: previously stated as `pC = (k·pPrime)²` / `pL = min(2·pPrime, 1)`
under an "RFC 9332 §4.1" citation — the same eq. (1) misreading the implementation
encoded. Corrected with the implementation; see S-L4S.13.)*

The criterion is **coupling-formula verification**, not throughput-ratio equivalence
(the latter is structurally unobservable with non-responsive UDP CBR flows). At the
defaults' offered ratio (~1.5× bottleneck) the non-responsive load holds the classic
sojourn above target, so `pPrime` legitimately rides high (mean ≈ 0.75, clamp
episodes at 1.0) and the formula is verified across the full range, clamp included.
*(An earlier revision stated `pPrime` operates in `[1e-5, 1e-2]` — a band measured
while the classic lane sat in its constructor trap state; see S-L4S.13's erratum
trail and the 2026-06-10 audit.)*

Verified by: `TestS_l4s_piControl_fires_at_nominal_load` in `l4s-scenario-validation-test.cc`.

## S-l4s-s1-latency-arm-differentiation (intent: I-12)

After running `diffserv-l4s-s1-latency --mode=l4s-on --simTime=3` and
`--mode=l4s-off --simTime=3` at default parameters, the EF probe arms shall
satisfy:

- `mean(owd_ef_l4s_on) < 10 ms` (priority routing functional in L4S mode)
- `mean(owd_ef_l4s_off) < 10 ms` (priority routing functional in classic mode)

An earlier draft criterion (15% mean / 20% P95 reduction in AF OWD) was
structurally unachievable because both modes give the EF probe identical priority
access; AF arm differentiation requires Scalable congestion control (e.g.
`TcpDctcp` with `UseEct0=false` as the L4S sender), not UDP CBR.

The relaxed criterion verifies that the scenario's priority wiring is correct in both
modes — a meaningful but weaker assertion than the original. A future cycle with
responsive flows (deferred to v1.8+) can demonstrate the throughput-equivalence
narrative properly.

Verified by: `TestS_l4s_s1_latency_arm_differentiation` in `l4s-scenario-validation-test.cc`.

## S-aqm-envelope-axis-in-mbps (intent: I-13)

Rendering the `aqm-eval-runner` recipe shall produce an SVG whose y-axis tick
labels parse as Mbps (e.g., '5', '10', '15'), not as ECDF [0, 1] values with a
scientific-notation scale factor. The recipe YAML shall declare
`y_unit_convert: bps_to_mbps` and `ylabel: "Aggregate goodput (Mbps)"`.

Verified by: `test_aqm_envelope_axis_in_mbps` in `scripts/test_plot_recipe.py`.

## S-aqm-runner-goodput-window-full-simtime (intent: I-14)

For every per-flow row emitted by `aqm-eval-runner`, the reported
`rx_rate_bps` shall equal `(fm_rx_bytes − fm_retx_bytes) × 8 / simTime` to
within 1 bps of rounding, where `simTime` is the value passed on the command
line and echoed as `simTime=` in the cell summary. The goodput denominator is
the **full `--simTime` window**: reintroducing a warm-up trim on the
denominator alone (e.g. `simTime − 0.5`, removed in 2026-04 after inflating
reported rates by ~5%) breaks this identity and fails the gate.

Verified by: `TestSAqmRunnerGoodputWindowFullSimtime` in
`test/diffserv-test-suite.cc` (suite `stratum`), which smoke-runs the
deterministic UDP-CBR cell `mild-congestion` × `ns3::FqCoDelQueueDisc` at
`--simTime=10` and recomputes the identity from the emitted per-flow CSV.

## S-aqm-runner-udp-cbr-rate-band (intent: I-14)

On the unsaturated deterministic UDP-CBR cell (`mild-congestion`: two
3.5 Mbps CBR bulk flows plus one 200 kbps probe, 7.2 Mbps offered on the
10 Mbps rate-limited bottleneck), each **bulk** flow's IP-layer delivered
rate over its active window `[startSec, simTime]` shall be within **3%** of
the offered application rate:

    |fm_rx_bytes × 8 / (simTime − startSec) − rateBps| / rateBps ≤ 0.03

The 3% band is the IP+UDP header-overhead band for the 1000-byte CBR payload
(28/1000 = 2.8%) plus sub-0.1% start/stop quantisation. The band is fixed
here and shall not be widened. The 200-byte-payload probe flow is outside
this assertion's scope (its header overhead is 14% by construction). A
violation indicates bottleneck loss where none is expected, a
measurement-plane bug, or an offered-rate regression in `BuildFlowPlan()`.

Verified by: `TestSAqmRunnerUdpCbrRateBand` in `test/diffserv-test-suite.cc`
(suite `stratum`), reading the same smoke-run CSV as
S-aqm-runner-goodput-window-full-simtime.

## S-aqm-stratumred-factory-four-step (intent: I-14)

A naive `CreateObject<stratum::RedQueueDisc>()` followed only by
`Initialize()` shall drop every enqueued packet (empty PHB table →
`NO_PHB_MATCH`). The AQM registry's canonical four-step `StratumRed` factory
— `Initialize()` before any configurator, then `SetMredMode(WRED)`, then
queue limit + thresholds, then `AddPhbEntry` for BE and EF — shall accept
and dequeue the same traffic in full. This pins the four-trap
naive-instantiation checklist documented in `handbook/III-06-aqm-eval.md`:
empty PHB table; RIO_C `thMin=thMax=0` force-drop; the `DROP_TAIL` misnomer;
configurators silently no-op before `Initialize()`.

Verified by: `TestSAqmStratumRedFactoryFourStep` in
`test/diffserv-test-suite.cc` (suite `stratum`), driving both arms through
`Enqueue`/`Dequeue` directly with DSCP-0 (best-effort) packets.

## S-aqm-stratuml4swred-factory-classic-lane-functional (intent: I-14)

The AQM registry's `StratumL4sWred` factory shall produce a queue disc whose
classic lane operates as a functional WRED early-drop AQM after
`Initialize()` — MRED mode `WRED`, thresholds 5/15 packets, `maxP` 0.1 and a
25-packet limit on the classic sub-queues, the same mitigated defaults
`stratum::l4s::QueueDisc::DoInitialize` installs for an untouched `Wred`
picker. Concretely: ten back-to-back Not-ECT DSCP-0 packets enqueued without
intervening dequeues shall all be accepted, with zero drops, and all ten
shall dequeue (a healthy WRED EWMA average stays far below `thMin = 5` at
this depth).

The regression this gates (2026-05-02 to 2026-06-10): the factory's partial
classic-lane configuration (`SetQueueLimit`, `AddPhbEntry`) flips the
`m_classicUserConfigured` gate, which skips the Wred default-mitigation
block in `DoInitialize` and leaves the classic sub-queues at the constructor
trap defaults (`RIO_C`, `thMin = thMax = maxP = 0`, `ptc = 0`). The EWMA
average then latches above the zero threshold at the first observed backlog
and every later Not-ECT packet is force-dropped (`RED_FORCED_DROP`); with no
idle-time decay (`ptc = 0`) the lane never reopens, collapsing the cell to
the initial slow-start burst (0.05–0.45 Mbps measured, against 7.8 Mbps
healthy). Under the regression this gate reads one accepted packet and nine
forced drops.

Verified by: `TestSAqmStratumL4sWredFactoryClassicLaneFunctional` in
`test/diffserv-test-suite.cc` (suite `stratum`), driving the registry-built
disc through `Enqueue`/`Dequeue` directly with DSCP-0 (best-effort) packets.

## S-cake-host-asymmetric-capture-bounded (intent: I-11)

Under asymmetric TCP offered load where one host runs N_A flows and another
runs N_B < N_A flows to a separate destination (identical RTT and TCP variant)
through one besteffort tin on mainline `FqCobaltQueueDisc` with
`EnableHostIsolation=true` and `HostIsolationMode=Triple`, the higher-flow
host's byte-share over the steady-state window converges in pure ns-3 to a
config-dependent value between the per-flow share `N_A/(N_A+N_B)` (0.80 at
4-vs-1) and full per-host fairness (0.50) — partial host equalization. With
socket buffers sized above the path bandwidth-delay product, the measured
4-vs-1 CUBIC value is 0.6466 (MSS 536); S-17.55 gates `[0.625, 0.665]`. At the
ns-3 default 128 KB buffers the lone host's flow is receive-window-limited and
the share inflates to ~0.76 — a socket-buffer measurement artefact, not a
host-isolation property.

The host-isolation **mechanism** is Linux-faithful. Each flow's DRR quantum is
divided by `host_load = max(srchost_bulk_flow_count, dsthost_bulk_flow_count)`
via the reciprocal divide `quantum·(65535/host_load)>>16`, bit-equivalent to
Linux `cake_get_flow_quantum` (`provenance/linux-sch-cake-67dc6c56b871/sch_cake.c:688`).
Two controls confirm the mechanism is sound rather than defective: with host
isolation disabled the same probe recovers per-flow fairness, and at the
shared-sink anchor where `host_load` saturates uniformly across all flows the
substrate matches Linux within ~2 pp (≈0.50).

The residual band is therefore a **fidelity boundary**, not a mechanism defect.
Where `host_load` is non-uniform the host-isolation DRR equalizes per host by
observing per-host activity over time, and that observation is sensitive to the
input-timing distribution; pure deterministic ns-3 settles above native Linux's
fuller equalization (≈0.52 at this probe). The gap is **attributed** — localised
by the companion protocol-robustness analysis, not proven mechanistically — to
the interaction between reactive TCP transport and the simulator's deterministic
event cadence (versus Linux NAPI/softirq jitter); the structural in-simulator
emulation that would close it is deferred. The Stratum-bridge prototype
**cross-validates a workaround** — feeding real Linux TCP timing through the
held-constant qdisc reproduces the Linux band under configuration parity — but
does not itself prove the cause, and no impossibility of in-simulator
remediation is claimed. The sensitivity is confined to the host-aware DRR:
per-flow drop/mark logic is timing-robust (the host-isolation-OFF plain-vs-bridge
gap collapses to ≤1 pp).

**Spec history:** v1.10 framed the band as a Triple "sticky-cursor matching
Linux" contract; v1.15 as a nested-DRR emergent property over
`{Triple, DualSrchost, DualDsthost}`. Both described the retired
`DsHostIsolatedFqCobalt` wrapper and mis-attributed the band. Post-retirement the
substrate is mainline `FqCobaltQueueDisc`; both the retired nested path and the
mainline path produce this band byte-identically, so it is intrinsic to the
host-isolation DRR, not an artefact of the retired dispatcher. It is now
characterised as the pure-ns-3 end of a dispatch-cadence fidelity boundary —
mechanism Linux-faithful, outcome timing-sensitive — consistent with the
technical report's Stratum-bridge cross-validation.

## S-cake-host-isolated-ack-filter-per-bucket-conservative (intent: I-11) *(retired)*

The per-host-pair-bucket ACK-filter composition was specific to the
`DsHostIsolatedFqCobalt` wrapper and its now-deleted fixture
`host-isolated-fq-cobalt-ack-filter-test.cc`. Functional ACK filtering survives
on mainline `FqCobaltQueueDisc` (`EnableAckFilter` / `EnableAckFilterAggressive`,
`patches/ns3/0006`) and is asserted by `AckFilterFunctionalContractTest` in
`diffserv-test-suite.cc`. With host isolation enabled, the filter operates per
5-tuple exactly as without it — 5-tuple identity uniquely determines flow
membership, so there is no separate per-bucket composition to assert.

## S-cake-host-isolated-ack-filter-per-bucket-aggressive (intent: I-11) *(retired)*

Retired with the conservative-mode arm above (same wrapper, same deleted
fixture). Aggressive-mode ACK filtering survives on mainline `FqCobaltQueueDisc`
(`EnableAckFilterAggressive`, `patches/ns3/0006`).

## How to use this file

Each S-N.M assertion translates to a unit or integration test in the ns-3 test framework. Tests are named `BriefDescriptionTest` (one-line `\brief` and `\see specs/02-structural.md S-N.M` in the class Doxygen block) and located in `src/diffserv/test/`. The CI gate is: all S-tests pass, with statistical assertions allowed up to their declared tolerance.

When implementing a class, the developer (or Claude Code) should look up the S-tests that reference the corresponding I-spec and use them as the implementation target.
