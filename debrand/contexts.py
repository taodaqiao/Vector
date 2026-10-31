#!/usr/bin/env python3
"""Print byte context around identity tokens inside a binary.

Usage: python contexts.py <file> [token ...]
"""
from __future__ import annotations

import sys

DEFAULT = [
    b"vector",
    b"lsposed",
    b"lspd",
    b"lsplant",
    b"Dobby",
    b"robv",
    b"org.matrix.vector",
]


def main() -> int:
    path = sys.argv[1]
    tokens = [t.encode() for t in sys.argv[2:]] or DEFAULT
    data = open(path, "rb").read()
    for token in tokens:
        positions = []
        start = 0
        while True:
            index = data.find(token, start)
            if index < 0:
                break
            positions.append(index)
            start = index + 1
        print(f"\n### {token.decode()}  x{len(positions)}")
        seen: set[bytes] = set()
        for pos in positions:
            lo = max(0, pos - 60)
            hi = min(len(data), pos + 60)
            chunk = data[lo:hi]
            ascii_only = bytes(c if 32 <= c < 127 else 0x2E for c in chunk).decode()
            if ascii_only in seen:
                continue
            seen.add(ascii_only)
            if len(seen) > 12:
                print("    ...")
                break
            print(f"  0x{pos:08x} {ascii_only}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
