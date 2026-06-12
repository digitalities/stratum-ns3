#!/usr/bin/env python3
"""
Extract per-packet feature CSV from a cake-arrival pcap.

Auto-detects link-layer type (DLT_RAW = 101 = Stratum side; DLT_EN10MB = 1 =
Linux tcpdump capture). Skips the Ethernet header on DLT_EN10MB. Emits one
row per IPv4 TCP packet with columns:

    ts_ns, src_ip, src_port, dst_ip, dst_port, length, is_ack

Where:
    ts_ns     packet timestamp in nanoseconds since pcap epoch
    length    IPv4 total length (header + payload), in bytes
    is_ack    1 if TCP flags ACK is set and TCP payload length == 0

Non-TCP / non-IPv4 packets are skipped.
"""
import argparse
import csv
import sys

import dpkt


# ns-3 writes libpcap's LINKTYPE_RAW (101) on disk, but dpkt's
# DLT_RAW constant is BSD's old value (12). Accept either.
LINK_TYPE_RAW_DPKT = dpkt.pcap.DLT_RAW          # 12 — BSD
LINK_TYPE_RAW_LIBPCAP = 101                     # libpcap LINKTYPE_RAW (used by ns-3, tcpdump on Linux)
LINK_TYPE_EN10MB = dpkt.pcap.DLT_EN10MB         # 1 — both conventions agree


def parse_packet(linktype, buf):
    if linktype == LINK_TYPE_EN10MB:
        try:
            eth = dpkt.ethernet.Ethernet(buf)
        except Exception:
            return None
        ip = eth.data
    elif linktype in (LINK_TYPE_RAW_DPKT, LINK_TYPE_RAW_LIBPCAP):
        try:
            ip = dpkt.ip.IP(buf)
        except Exception:
            return None
    else:
        return None

    if not isinstance(ip, dpkt.ip.IP):
        return None
    if ip.p != dpkt.ip.IP_PROTO_TCP:
        return None
    tcp = ip.data
    if not isinstance(tcp, dpkt.tcp.TCP):
        return None

    src_ip = ".".join(str(b) for b in ip.src)
    dst_ip = ".".join(str(b) for b in ip.dst)
    payload_len = ip.len - (ip.hl * 4) - (tcp.off * 4)
    is_ack = 1 if (tcp.flags & dpkt.tcp.TH_ACK and payload_len <= 0) else 0

    return (src_ip, tcp.sport, dst_ip, tcp.dport, ip.len, is_ack)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.inp, "rb") as fh:
        reader = dpkt.pcap.Reader(fh)
        linktype = reader.datalink()
        if linktype not in (LINK_TYPE_RAW_DPKT, LINK_TYPE_RAW_LIBPCAP, LINK_TYPE_EN10MB):
            print(f"unsupported linktype {linktype}", file=sys.stderr)
            sys.exit(2)

        with open(args.out, "w", newline="") as out:
            w = csv.writer(out)
            w.writerow(["ts_ns", "src_ip", "src_port", "dst_ip", "dst_port", "length", "is_ack"])

            n = 0
            for ts, buf in reader:
                rec = parse_packet(linktype, buf)
                if rec is None:
                    continue
                ts_ns = int(round(ts * 1_000_000_000))
                w.writerow([ts_ns, *rec])
                n += 1

    print(f"Wrote {n} rows to {args.out}")


if __name__ == "__main__":
    main()
