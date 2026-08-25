"""Shared pcap/radiotap reader for the air-capture tools."""
import struct
import sys


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
