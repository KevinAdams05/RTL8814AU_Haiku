#!/usr/bin/env python3
"""Check the README's Hardware Compatibility List against the driver's table.

The list of devices the driver claims lives in kSupportedDevices in
src/RTL8814AU.h, and the README documents the same set for people deciding
whether their adapter is supported. Those two drift: an ID gets added to the
code and not the table, or a row keeps a typo in a hex digit that nobody
notices because it looks plausible.

Exits non-zero on any mismatch, so it can go in a pre-release check.

Usage: scripts/check-hcl.py [repo-root]
"""
import os
import re
import sys


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    header = open(os.path.join(root, 'src', 'RTL8814AU.h')).read()
    match = re.search(r'kSupportedDevices\[\]\s*=\s*\{(.*?)\};', header, re.S)
    if match is None:
        sys.exit("could not find kSupportedDevices in src/RTL8814AU.h")

    devices = [(v.lower(), p.lower(), name) for v, p, name in re.findall(
        r'\{\s*0x([0-9a-fA-F]{4})\s*,\s*0x([0-9a-fA-F]{4})\s*,\s*"([^"]*)"',
        match.group(1))]

    readme = open(os.path.join(root, 'README.md')).read()
    rows = [(v.lower(), p.lower()) for v, p in re.findall(
        r'\|\s*([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\s*\|', readme)]

    claimed = {(v, p) for v, p, _ in devices}
    listed = set(rows)

    missing = [(v, p, n) for v, p, n in devices if (v, p) not in listed]
    extra = sorted(listed - claimed)
    duplicated = sorted({r for r in rows if rows.count(r) > 1})

    print("driver claims %d device(s); README lists %d row(s)"
          % (len(devices), len(rows)))

    problems = 0
    for vendor, product, name in missing:
        print("  MISSING from README: %s:%s  %s" % (vendor, product, name))
        problems += 1
    for vendor, product in extra:
        print("  in README but not claimed by the driver: %s:%s"
              % (vendor, product))
        problems += 1
    for vendor, product in duplicated:
        print("  listed twice in README: %s:%s" % (vendor, product))
        problems += 1

    if problems:
        sys.exit("%d problem(s)" % problems)
    print("the two agree")


main()
