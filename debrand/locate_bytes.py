"""Locate identity tokens inside an ELF and report the owning section.

Usage: python locate_bytes.py <elf> [token ...]

Prints, for every token, each hit offset plus the section that owns it, so a
hit in .debug_str (DWARF leftovers) is distinguishable from a hit in .rodata
(a real string literal the loader keeps).
"""
from __future__ import annotations

import struct
import sys

EM_ARM, EM_AARCH64, EM_386, EM_X86_64 = 40, 183, 3, 62


def read_sections(blob: bytes):
    if blob[:4] != b"\x7fELF":
        raise SystemExit("not an ELF file")
    is64 = blob[4] == 2
    if is64:
        e_shoff, = struct.unpack_from("<Q", blob, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x3A)
    else:
        e_shoff, = struct.unpack_from("<I", blob, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    raw = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if is64:
            name, stype, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", blob, off)
        else:
            name, stype, flags, addr, offset, size = struct.unpack_from("<IIIIII", blob, off)
        raw.append((name, stype, flags, addr, offset, size))
    strtab_off = raw[e_shstrndx][4]
    out = []
    for name, stype, flags, addr, offset, size in raw:
        end = blob.index(b"\x00", strtab_off + name)
        out.append((blob[strtab_off + name:end].decode("latin-1"), offset, size))
    return out


def main() -> int:
    path = sys.argv[1]
    tokens = sys.argv[2:] or ["vector", "lsposed", "lsplant", "Dobby", "dobby"]
    blob = open(path, "rb").read()
    sections = read_sections(blob)
    print(f"{path}  {len(blob):,} B  sections={len(sections)}")
    for token in tokens:
        raw = token.encode()
        start = 0
        while True:
            idx = blob.find(raw, start)
            if idx < 0:
                break
            owner = "?"
            for name, off, size in sections:
                if off <= idx < off + size:
                    owner = name
                    break
            print(f"  {token:<10} 0x{idx:08x}  {owner}")
            start = idx + 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
