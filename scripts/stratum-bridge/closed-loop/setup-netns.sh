#!/usr/bin/env bash
# Build the closed-loop bridging topology: 7 netns + veth pairs.
#
# Topology:
#   src-a (10.1.1.10) ── router-a ── [veth] ── ns3-rt (10.2.0.2)
#                                                  │
#                                          ns-3 process (CAKE on tx)
#                                                  │
#                                   [veth] ── router-b ── sink-a (10.3.1.10)
#   src-b (10.1.2.10) ── router-a                         sink-b (10.3.2.10)
#
# Idempotent: safe to re-run.

set -euo pipefail

mk_netns() {
    local name="$1"
    if ! ip netns list | grep -qw "$name"; then
        ip netns add "$name"
    fi
}

mk_veth_pair() {
    # mk_veth_pair <name-a> <netns-a> <name-b> <netns-b>
    local a="$1" nsa="$2" b="$3" nsb="$4"
    if ! ip -n "$nsa" link show "$a" >/dev/null 2>&1; then
        ip link add "$a" type veth peer name "$b"
        ip link set "$a" netns "$nsa"
        ip link set "$b" netns "$nsb"
    fi
    ip -n "$nsa" link set "$a" up
    ip -n "$nsb" link set "$b" up
    ip -n "$nsa" link set "$a" mtu 1500
    ip -n "$nsb" link set "$b" mtu 1500
}

# --- 1. netns ---------------------------------------------------------------
for ns in src-a src-b router-a ns3-rt router-b sink-a sink-b; do
    mk_netns "$ns"
done

ip netns exec router-a sysctl -wq net.ipv4.ip_forward=1
ip netns exec router-b sysctl -wq net.ipv4.ip_forward=1

# --- 2. veth pairs ----------------------------------------------------------
# src-a ↔ router-a
mk_veth_pair vsrca src-a rai1 router-a
ip -n src-a    addr add 10.1.1.10/24 dev vsrca 2>/dev/null || true
ip -n router-a addr add 10.1.1.1/24  dev rai1  2>/dev/null || true

# src-b ↔ router-a
mk_veth_pair vsrcb src-b rai2 router-a
ip -n src-b    addr add 10.1.2.10/24 dev vsrcb 2>/dev/null || true
ip -n router-a addr add 10.1.2.1/24  dev rai2  2>/dev/null || true

# router-a ↔ ns3-rt
# IPs are configured ONLY on the router side; the ns3-rt side has the veth
# device present but unaddressed at the kernel layer. ns-3's
# EmuFdNetDevice carries the 10.2.0.2 / 10.4.0.1 addresses in its own
# Ipv4 layer, so ns-3 answers ARP for those addresses. If the Linux kernel
# also held those addresses, it would answer ARP first (with the veth's
# hwaddr, not ns-3's), causing frame-MAC mismatch on ingress.
mk_veth_pair ra-ns3 router-a ns3-rx ns3-rt
ip -n router-a addr add 10.2.0.1/24 dev ra-ns3 2>/dev/null || true

# ns3-rt ↔ router-b (same arrangement: address only on the router side)
mk_veth_pair ns3-tx ns3-rt rb-ns3 router-b
ip -n router-b addr add 10.4.0.2/24 dev rb-ns3 2>/dev/null || true

# router-b ↔ sink-a
mk_veth_pair rbi1 router-b vsinka sink-a
ip -n router-b addr add 10.3.1.1/24  dev rbi1   2>/dev/null || true
ip -n sink-a   addr add 10.3.1.10/24 dev vsinka 2>/dev/null || true

# router-b ↔ sink-b
mk_veth_pair rbi2 router-b vsinkb sink-b
ip -n router-b addr add 10.3.2.1/24  dev rbi2   2>/dev/null || true
ip -n sink-b   addr add 10.3.2.10/24 dev vsinkb 2>/dev/null || true

# --- 3. routes --------------------------------------------------------------
ip -n src-a    route add default via 10.1.1.1 2>/dev/null || true
ip -n src-b    route add default via 10.1.2.1 2>/dev/null || true
ip -n router-a route add 10.3.0.0/16 via 10.2.0.2 2>/dev/null || true
ip -n router-b route add 10.1.0.0/16 via 10.4.0.1 2>/dev/null || true
ip -n sink-a   route add default via 10.3.1.1 2>/dev/null || true
ip -n sink-b   route add default via 10.3.2.1 2>/dev/null || true

# ns3-rt: ns-3 owns forwarding between ns3-rx and ns3-tx via EmuFdNetDevice
# raw-socket binding. We intentionally do NOT configure Linux-side routing
# for the netns; the kernel receives the raw frames via the bound socket
# only when ns-3 transmits.
#
# EmuFdNetDevice requires the bound interface to be in promiscuous mode so
# its raw socket sees all frames (the kernel would otherwise drop frames
# whose dest-MAC doesn't match the veth's hwaddr). Also disable Linux's
# kernel IP processing on ns3-rt by sinking incoming packets — the raw
# socket bypasses the kernel IP stack, so we don't need (and don't want)
# the kernel to also try to route them.
ip -n ns3-rt link set ns3-rx promisc on
ip -n ns3-rt link set ns3-tx promisc on
ip netns exec ns3-rt sysctl -wq net.ipv4.ip_forward=0
ip netns exec ns3-rt sysctl -wq net.ipv6.conf.all.disable_ipv6=1
# Reject all IPv4 packets that the kernel might process on ns3-rt
# (everything should flow through the ns-3 raw socket instead).
ip netns exec ns3-rt iptables -P INPUT DROP   2>/dev/null || true
ip netns exec ns3-rt iptables -P FORWARD DROP 2>/dev/null || true

# --- 4. lo up everywhere ----------------------------------------------------
for ns in src-a src-b router-a ns3-rt router-b sink-a sink-b; do
    ip -n "$ns" link set lo up
done

# --- 5. Disable veth tx-checksum offload on all veths --------------------
# Linux veth pairs use CHECKSUM_PARTIAL by default — the kernel assumes
# hardware will fill in the L4 checksum at send time. When packets exit
# the kernel via a raw socket (ns-3's EmuFdNetDevice), the unfilled
# checksum bytes appear on the wire and the destination netns kernel
# rejects them. Disabling tx offload forces the kernel to compute the
# real L4 checksum before sending the frame, so what reaches the raw
# socket is what reaches the destination.
disable_tx_offload() {
    local ns="$1" dev="$2"
    ip netns exec "$ns" ethtool -K "$dev" tx off rx off 2>/dev/null || true
    ip netns exec "$ns" ethtool -K "$dev" tx-checksum-ip-generic off 2>/dev/null || true
    ip netns exec "$ns" ethtool -K "$dev" sg off gso off tso off 2>/dev/null || true
}
disable_tx_offload src-a    vsrca
disable_tx_offload router-a rai1
disable_tx_offload router-a rai2
disable_tx_offload router-a ra-ns3
disable_tx_offload src-b    vsrcb
disable_tx_offload ns3-rt   ns3-rx
disable_tx_offload ns3-rt   ns3-tx
disable_tx_offload router-b rb-ns3
disable_tx_offload router-b rbi1
disable_tx_offload router-b rbi2
disable_tx_offload sink-a   vsinka
disable_tx_offload sink-b   vsinkb

echo "OK: 7-netns topology ready."
echo "Local smoke:  ip netns exec src-a ping -c 1 -W 2 10.1.1.1"
echo "Bridge smoke: launch ns-3 inside ns3-rt, then ping src-a -> sink-a."
