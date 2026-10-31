#!/usr/bin/env python3
"""Scan a built module tree (or zip contents already extracted) for identity tokens.

Usage: python scan_tokens.py <directory>

Prints one line per file that still contains a signature that a detector could
use to recognise the framework.  Exit code 1 when anything is found, so it can
be wired into CI.
"""
from __future__ import annotations

import os
import sys

# Tokens that must never survive into a shipped artifact.  Keep them lower-case;
# the scan is case-sensitive on purpose so that legitimate identifiers such as
# "Vector2" in a math header do not produce noise -- add an explicit variant
# when a differently-cased spelling matters.
PATTERNS = [
    b"vector",
    b"lsposed",
    b"lspd",
    b"zygisk_vector",
    b"JingMatrix",
    b"robv",
    b"lsplant",
    b"dobby",
    b"Dobby",
    b"serial_vector",
    b"5291374ceda0aef7c5d86cd2a4f6a3ac",
    b"com.sysutil.compat.core",
    b"org.matrix.vector",
    b"Vector-",
]

SKIP_SUFFIXES = (".sha256",)


def scan(root: str) -> int:
    findings = 0
    for dirpath, _, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(SKIP_SUFFIXES):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace("\\", "/")
            with open(path, "rb") as handle:
                data = handle.read()
            hits = {pat.decode(): data.count(pat) for pat in PATTERNS if data.count(pat)}
            if hits:
                findings += 1
                print(f"{rel:44} {hits}")
    return findings


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    if not os.path.isdir(root):
        print(f"not a directory: {root}")
        return 2
    findings = scan(root)
    print(f"\n{findings} file(s) still carry identity tokens")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
