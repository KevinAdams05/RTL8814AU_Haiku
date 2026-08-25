#!/usr/bin/env python3
"""Decode EAPOL-Key frames out of an over-the-air 802.11 capture.

Built to compare our driver's M2 against the vendor driver's M2 for the same
access point, at the level the access point actually sees. Everything compared
so far has been the USB submission to the chip; a frame can still leave the
radio different from how it was handed over, and the access point rejects what
reaches it, not what we submitted.

Prints the 802.11 header fields as well as the key descriptor, because a
header-level difference -- protected bit, QoS, sequence reuse, retry -- would
matter just as much as a descriptor one.

Usage: air-eapol.py <capture.pcap> [--from MAC] [--hex]
"""
import struct
import sys

SNAP_EAPOL = b"\xaa\xaa\x03\x00\x00\x00\x88\x8e"


def records(path):
    with open(path, "rb") as handle:
        magic = handle.read(4)
        if magic not in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"):
            sys.exit("not a pcap file: %s" % path)
        endian = "<" if magic == b"\xd4\xc3\xb2\xa1" else ">"
        handle.read(20)
        while True:
            header = handle.read(16)
            if len(header) < 16:
                return
            sec, usec, incl, _ = struct.unpack(endian + "IIII", header)
            data = handle.read(incl)
            if len(data) < incl:
                return
            yield sec + usec / 1e6, data


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def label(key_info, key_data_len):
    pairwise = (key_info >> 3) & 1
    install = (key_info >> 6) & 1
    ack = (key_info >> 7) & 1
    mic = (key_info >> 8) & 1
    secure = (key_info >> 9) & 1
    if not pairwise:
        return "group/rekey"
    if ack and not mic:
        return "M1"
    if not ack and mic and not secure:
        return "M2"
    if ack and mic and install and secure:
        return "M3"
    if not ack and mic and secure and key_data_len == 0:
        return "M4"
    return "pairwise/?"


def decode(body):
    """Decode an EAPOL-Key body (starting at the EAPOL version byte)."""
    if len(body) < 95:
        return None
    version, packet_type, length = struct.unpack(">BBH", body[:4])
    descriptor = body[4]
    key_info, key_length = struct.unpack(">HH", body[5:9])
    replay = body[9:17]
    nonce = body[17:49]
    iv = body[49:65]
    rsc = body[65:73]
    reserved = body[73:81]
    mic = body[81:97]
    key_data_len = struct.unpack(">H", body[97:99])[0] if len(body) >= 99 else 0
    key_data = body[99:99 + key_data_len]
    return {
        "version": version, "packet_type": packet_type, "length": length,
        "descriptor": descriptor, "key_info": key_info,
        "key_length": key_length, "replay": replay, "nonce": nonce,
        "iv": iv, "rsc": rsc, "reserved": reserved, "mic": mic,
        "key_data_len": key_data_len, "key_data": key_data,
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    path = args[0]
    want_from = None
    if "--from" in sys.argv:
        want_from = sys.argv[sys.argv.index("--from") + 1].lower()
    show_hex = "--hex" in sys.argv

    found = 0
    for index, (stamp, packet) in enumerate(records(path), 1):
        if len(packet) < 4:
            continue
        radiotap_len = struct.unpack("<H", packet[2:4])[0]
        if radiotap_len < 8 or radiotap_len > len(packet):
            continue
        frame = packet[radiotap_len:]
        if len(frame) < 24:
            continue
        fc = struct.unpack("<H", frame[:2])[0]
        ftype = (fc >> 2) & 3
        subtype = (fc >> 4) & 0xF
        if ftype != 2:                      # data frames only
            continue
        protected = (fc >> 14) & 1
        retry = (fc >> 11) & 1
        duration = struct.unpack("<H", frame[2:4])[0]
        addr1, addr2, addr3 = frame[4:10], frame[10:16], frame[16:22]
        seq = struct.unpack("<H", frame[22:24])[0]
        offset = 24
        qos = None
        if subtype & 0x8:                   # QoS data carries a 2-byte header
            if len(frame) < offset + 2:
                continue
            qos = struct.unpack("<H", frame[offset:offset + 2])[0]
            offset += 2
        if want_from and mac(addr2) != want_from:
            continue
        body = frame[offset:]
        at = body.find(SNAP_EAPOL)
        if at < 0 or at > 8:
            continue
        eapol = body[at + 8:]
        fields = decode(eapol)
        if fields is None:
            continue
        found += 1
        which = label(fields["key_info"], fields["key_data_len"])
        print("--- frame %d  t=%.3fs  %s ---" % (index, stamp, which))
        print("  802.11: %s -> %s (bssid %s)"
              % (mac(addr2), mac(addr1), mac(addr3)))
        print("  subtype=%d protected=%d retry=%d duration=%d seq=%d frag=%d%s"
              % (subtype, protected, retry, duration, seq >> 4, seq & 0xF,
                 "" if qos is None else " qos=0x%04x" % qos))
        print("  eapol:  version=%d type=%d length=%d descriptor=%d"
              % (fields["version"], fields["packet_type"], fields["length"],
                 fields["descriptor"]))
        print("  key_info=0x%04x (%s) key_length=%d key_data_len=%d"
              % (fields["key_info"], describe(fields["key_info"]),
                 fields["key_length"], fields["key_data_len"]))
        print("  replay=%s" % fields["replay"].hex())
        print("  nonce=%s" % fields["nonce"].hex())
        print("  mic=%s" % fields["mic"].hex())
        nonzero = lambda b: "zero" if not any(b) else b.hex()
        print("  iv=%s rsc=%s reserved=%s"
              % (nonzero(fields["iv"]), nonzero(fields["rsc"]),
                 nonzero(fields["reserved"])))
        if fields["key_data_len"]:
            print("  key_data=%s" % fields["key_data"].hex())
        if show_hex:
            print("  full 802.11 frame (%d bytes):" % len(frame))
            for line in range(0, min(len(frame), 160), 16):
                print("    %04x  %s" % (line, frame[line:line + 16].hex(" ")))
    if found == 0:
        print("no EAPOL-Key frames matched"
              + (" from %s" % want_from if want_from else ""))


def describe(key_info):
    bits = []
    names = [(3, "pairwise"), (6, "install"), (7, "ack"), (8, "mic"),
             (9, "secure"), (10, "error"), (11, "request"), (12, "encrypted")]
    for bit, name in names:
        if (key_info >> bit) & 1:
            bits.append(name)
    bits.append("descver=%d" % (key_info & 7))
    return ",".join(bits)


if __name__ == "__main__":
    main()
