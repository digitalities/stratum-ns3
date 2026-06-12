---
title: Part II introduction
origin: 2026-written
status: filled
last-updated: 2026-06-07
---

# Part II: Architecture & design — introduction

Part II explains the substrate from the top down. The chapters cover the
traffic-management concepts the design rests on, the four-slot composer
that ties the three clients together, and the layout of the ns-3 module.

## Reading path

Start with [Stratum architecture and design](II-02-stratum-architecture.md):
the four strategy slots and the three clients composed from them. If you
are new to QoS, [traffic management background](II-03-traffic-management.md)
and [the DiffServ model](II-04-diffserv-model.md) supply the context the
architecture chapter assumes. The three client chapters —
[the DiffServ client](II-05-diffserv-client.md),
[the L4S client](II-06-l4s-client.md), and
[the CAKE client](II-07-cake-client.md) — are peers: each describes one
client's slot choices in depth. [The 2026 ns-3 port](II-08-ns3-module.md)
closes the part with how the substrate is built, tested, and patched into
ns-3.

## Chapter map

| Chapter | File | Purpose |
|---|---|---|
| II-02 | [Stratum architecture and design](II-02-stratum-architecture.md) | The four strategy slots (classify-and-meter, mark-and-route, per-class slot array, across-slot service policy) and the three clients that populate them. |
| II-03 | [Traffic management background](II-03-traffic-management.md) | Best-effort vs class differentiation, IntServ vs DiffServ, AQM (RED/WRED/CoDel/PIE/FQ-CoDel), and where Stratum fits. |
| II-04 | [The DiffServ model](II-04-diffserv-model.md) | RFC 2474/2475 architecture, the DSCP field, EF/AF/CS/BE PHBs, traffic-conditioning components, and the post-2001 RFC chronology. |
| II-05 | [The DiffServ client](II-05-diffserv-client.md) | How the 2001 three-axis design maps onto the four strategy slots: meter and scheduler families, AF drop-precedence, LLQ composition, and the monitoring surface. |
| II-06 | [The L4S client](II-06-l4s-client.md) | What L4S is, DualPI2 coupling formulas, the RFC 9332 App. A.1 controller, the coupled scheduler, and composition with the substrate. |
| II-07 | [The CAKE client](II-07-cake-client.md) | Tin profiles, shaping modes, the ACK filter, feature scope, and composition with the substrate. |
| II-08 | [The 2026 ns-3 port](II-08-ns3-module.md) | Spec-driven implementation, RFC conformance vectors, the patch workflow for ns-3 mainline changes, and validation summary. |
