#!/usr/bin/env python3
"""Sequence numbers of every frame a station transmits.

802.11 duplicate detection keys on (address 2, sequence, fragment, retry). A
station that never advances its sequence number therefore has its own frames
discarded by the receiver as duplicates of the previous one -- so this is worth
checking directly rather than assuming the hardware fills the field in.

Usage: seq-check.py <capture.pcap> <station-mac> [other-mac]
"""
import struct
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from air_eapol_util import records, mac  # noqa: E402  (shared reader)

TYPE = {0: "mgmt", 1: "ctrl", 2: "data"}
MGMT = {0: "assoc-req", 1: "assoc-resp", 4: "probe-req", 5: "probe-resp",
        8: "beacon", 10: "disassoc", 11: "auth", 12: "deauth", 13: "action"}


def main():
    path, who = sys.argv[1], sys.argv[2].lower()
    seen = []
    counts = Counter()
    for index, (stamp, packet) in enumerate(records(path), 1):
        if len(packet) < 4:
            continue
        rtl = struct.unpack("<H", packet[2:4])[0]
        if rtl < 8 or rtl > len(packet):
            continue
        frame = packet[rtl:]
        if len(frame) < 24:
            continue
        fc = struct.unpack("<H", frame[:2])[0]
        ftype, subtype = (fc >> 2) & 3, (fc >> 4) & 0xF
        if ftype == 1:
            continue                      # control frames carry no addr2/seq
        if mac(frame[10:16]) != who:
            continue
        retry = (fc >> 11) & 1
        seq = struct.unpack("<H", frame[22:24])[0]
        name = MGMT.get(subtype, str(subtype)) if ftype == 0 \
            else "data/%d" % subtype
        seen.append((index, stamp, name, seq >> 4, retry))
        counts[seq >> 4] += 1

    print("frames transmitted by %s: %d" % (who, len(seen)))
    print("distinct sequence numbers: %d" % len(counts))
    print("most common: %s" % counts.most_common(6))
    print()
    print("  %-6s %-12s %-14s %5s %5s" % ("frame", "t(rel)", "kind", "seq", "rtry"))
    base = seen[0][1] if seen else 0
    for index, stamp, name, seq, retry in seen[:40]:
        print("  %-6d %-12.3f %-14s %5d %5d"
              % (index, stamp - base, name, seq, retry))


if __name__ == "__main__":
    main()
