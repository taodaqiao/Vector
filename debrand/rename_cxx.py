"""Rename the C++ identity surface that survives into the artifact.

Typeinfo and local symbol names are built from namespace and class names, so
`vector::native::module::VectorModule` puts the strings `vector` and `Vector`
into `.rodata` of `libzygisk.so` no matter how the string literals are handled:

    N6vector6native6module12VectorModuleE

This script renames the identifiers in place.  It is idempotent: running it on
an already-renamed tree reports zero edits.

Usage:
    python debrand/rename_cxx.py [--dry-run]
"""
from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIRS = ("native", "zygisk", "dex2oat")
SUFFIXES = {".cpp", ".cc", ".h", ".hpp"}
SKIP_PARTS = {"build", "external", ".git"}

# Order matters: longest first, so `vector::native::module` is not left as
# `sys::rt::module` half-renamed by an earlier, shorter rule.
RENAMES = (
    ("vector::native", "sys::rt"),
    ("namespace vector {", "namespace sys {"),
    ("// namespace vector::native", "// namespace sys::rt"),
    ("VectorModule", "LoaderModule"),
    ("ConfigBridge", "SettingsSource"),
    ("ConfigImpl", "SettingsImpl"),
)


def iter_sources():
    for name in SCAN_DIRS:
        base = ROOT / name
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            if SKIP_PARTS & set(path.parts):
                continue
            yield path


def main() -> int:
    dry_run = "--dry-run" in sys.argv
    edited = already = 0
    touched = []
    for path in iter_sources():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        original = text
        for old, new in RENAMES:
            text = text.replace(old, new)
        if text == original:
            if any(old in original for old, new in RENAMES if old == new):
                already += 1
            continue
        edited += 1
        touched.append(path.relative_to(ROOT).as_posix())
        if not dry_run:
            path.write_text(text, encoding="utf-8", newline="\n",
                            errors="surrogateescape")
    print(f"{'would edit' if dry_run else 'edited'} {edited} file(s); unchanged {already}")
    for name in touched[:40]:
        print(f"  {name}")
    if len(touched) > 40:
        print(f"  ... and {len(touched) - 40} more")
    # Any survivors would keep the fingerprint alive.
    leftovers = []
    for path in iter_sources():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        for old, _new in RENAMES:
            if old in text:
                leftovers.append(f"{path.relative_to(ROOT).as_posix()}: {old}")
    if leftovers:
        print(f"leftover: {len(leftovers)}")
        for item in leftovers[:20]:
            print(f"  {item}")
        return 1
    print("leftover: 0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
