"""Scrub identity words from the ELF files that ship inside the module zip.

Everything else has already been handled at the source level: literals are
sealed (debrand/seal_strings.py), the C++ namespace is renamed
(debrand/rename_cxx.py), and only `zygisk_module_entry` is exported.  What is
left is residue the compiler owns and the source cannot reach:

  * typeinfo / RTTI names, e.g. `NSt6__ndk16vectorI...`,
  * local symbol names that survive in `.symtab`'s string table,
  * the odd pooled literal emitted by a dependency (lsplant, dobby).

Substitution is strictly equal length, so no ELF offset, section size, or code
immediate moves.  Only ELF files are touched - patching a signed APK would
break its digest, and the APKs are handled at build time instead.

**Rewrites are confined to string-bearing sections.**  The first version of this
script replaced bytes anywhere in the file, which is how a six-byte word inside
`.text` becomes a silently corrupted instruction - the kind of damage that shows
up as a module that hangs the boot instead of one that prints an error.  Every
candidate offset is now checked against the section table, and hits that fall in
executable or plain data sections are reported and left alone.

Usage:
    python debrand/patch_so.py <file> [<file> ...]   # patch in place
    python debrand/patch_so.py --check <file> ...    # report only
"""
from __future__ import annotations

import struct
import sys

ELF_MAGIC = b"\x7fELF"

# Equal-length substitutions.  Order matters only in that a later rule must not
# match text an earlier rule produced.
SUBSTITUTIONS = (
    (b"lsposed", b"lsp0sed"),
    (b"LSPosed", b"LSP0sed"),
    (b"lsplant", b"lspl4nt"),
    (b"LSPlant", b"LSPl4nt"),
    (b"JingMatrix", b"J1ngMatr1x"),
    (b"Dobby", b"D0bby"),
    (b"dobby", b"d0bby"),
    (b"Vector", b"V3ctor"),
    (b"vector", b"v3ctor"),
    (b"lspd", b"l5pd"),
    (b"robv", b"r0bv"),
    # lsplant's header default for the generated hook-class name ends up in `.rodata`
    # even though this framework overrides it.  That exact spelling is on the published
    # blacklist of at least one shipping detector, and it is unused at runtime, so
    # renaming it costs nothing.
    (b"LSPHooker_", b"L5PHooker_"),
)

# Sections whose contents are text: symbol/string tables, literal pools, debug
# metadata, notes.  Anything else - above all `.text` - is off limits.
# Only sections that no loader and no runtime name lookup ever reads.
#
# `.dynstr` / `.strtab` / `.data.rel.ro` are deliberately absent: a mangled symbol name is an
# identity that the dynamic linker resolves and that a JNI lookup matches byte for byte. Rewriting
# `vector` inside `_ZN5lsplt2v2...NSt6__ndk16vector...` changes a definition's name without changing
# anything that calls it — the library then loads, and the symbol nobody can see is gone. It buys a
# byte in a section only root can read and risks a break nobody notices until a feature quietly
# stops. `.rodata`, `.comment`, `.debug*`, `.note*` and `.ARM.attributes` carry human-readable text
# and nothing else that has to stay consistent.
PATCHABLE_PREFIXES = (
    ".rodata",
    ".comment",
    ".debug",
    ".note",
    ".gnu_debuglink",
    ".ARM.attributes",
)


class ElfError(Exception):
    pass


def read_sections(blob: bytes) -> list[tuple[str, int, int]]:
    """Return (name, offset, size) for every section, or [] if unparsable."""
    if len(blob) < 64 or not blob.startswith(ELF_MAGIC):
        return []
    elf_class = blob[4]              # 1 = 32-bit, 2 = 64-bit
    endian = "<" if blob[5] == 1 else ">"
    try:
        if elf_class == 2:
            (e_shoff,) = struct.unpack_from(endian + "Q", blob, 0x28)
            e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHH", blob, 0x3A)
        elif elf_class == 1:
            (e_shoff,) = struct.unpack_from(endian + "I", blob, 0x20)
            e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHH", blob, 0x2E)
        else:
            return []
    except struct.error:
        return []
    if e_shoff == 0 or e_shnum == 0 or e_shentsize == 0:
        return []
    if e_shoff + e_shnum * e_shentsize > len(blob):
        return []

    def header(index: int) -> tuple[int, int, int, int]:
        base = e_shoff + index * e_shentsize
        if elf_class == 2:
            name, _type, _flags, _addr, offset, size = struct.unpack_from(endian + "IIQQQQ", blob, base)
        else:
            name, _type, _flags, _addr, offset, size = struct.unpack_from(endian + "IIIIII", blob, base)
        return name, offset, size, _type

    shstr_name, shstr_off, shstr_size, _ = header(e_shstrndx)
    if shstr_off + shstr_size > len(blob):
        return []
    shstr = blob[shstr_off:shstr_off + shstr_size]

    sections = []
    for index in range(e_shnum):
        name_off, offset, size, _ = header(index)
        if name_off >= len(shstr):
            continue
        end = shstr.find(b"\0", name_off)
        name = shstr[name_off:end if end != -1 else len(shstr)].decode("ascii", "replace")
        sections.append((name, offset, size))
    return sections


def patch(path: str, check_only: bool) -> int:
    with open(path, "rb") as handle:
        blob = bytearray(handle.read())
    if not blob.startswith(ELF_MAGIC):
        print(f"skip (not ELF): {path}")
        return 0

    sections = read_sections(bytes(blob))
    if not sections:
        print(f"skip (no section table, refusing to guess): {path}")
        return 0

    writable = [
        (name, offset, size)
        for name, offset, size in sections
        if name.startswith(PATCHABLE_PREFIXES) and size and offset + size <= len(blob)
    ]

    total = 0
    outside = 0
    for start, end in ((off, off + size) for _n, off, size in writable):
        chunk = bytes(blob[start:end])
        for old, new in SUBSTITUTIONS:
            if len(old) != len(new):
                raise SystemExit(f"substitution width mismatch: {old!r} -> {new!r}")
            hits = chunk.count(old)
            if hits:
                chunk = chunk.replace(old, new)
                total += hits
        blob[start:end] = chunk

    # Anything left is in a section we must not touch; say so instead of hiding it.
    remaining = bytes(blob)
    for old, _new in SUBSTITUTIONS:
        outside += remaining.count(old)

    if check_only:
        print(f"{path}: {total} rewritable, {outside} outside string sections, {len(writable)} section(s)")
        return total + outside

    if not total:
        print(f"{path}: clean ({outside} outside string sections)")
        return 0

    with open(path, "wb") as handle:
        handle.write(blob)
    print(f"{path}: rewrote {total} occurrence(s) in "
          f"{', '.join(sorted({n for n, _o, _s in writable})) or 'nothing'}")
    if outside:
        print(f"{path}: WARNING {outside} occurrence(s) live in code/data sections and were left alone")
    return total


def main() -> int:
    args = [arg for arg in sys.argv[1:] if arg != "--check"]
    check_only = "--check" in sys.argv
    if not args:
        print(__doc__)
        return 2
    total = 0
    for path in args:
        total += patch(path, check_only)
    # A check run fails when residue is found, so it can gate a build.
    return 1 if (check_only and total) else 0


if __name__ == "__main__":
    raise SystemExit(main())
