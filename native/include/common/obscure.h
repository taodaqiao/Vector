/*
 * Sealed identity strings.
 *
 * A hider that spells out what it hides is a fingerprint in its own right: the
 * sanitizer's token table, and the class-name prefixes the bridges resolve, would
 * otherwise sit in .rodata as the very words a scanner greps for.
 *
 * The plaintext lives only in `debrand/seal_strings.py`; the header it generates
 * holds ciphertext byte arrays, and `Open()` recovers the text on the stack when
 * it is needed.  This is obfuscation against string scanning, not secrecy: the
 * mask is in the binary too.
 *
 * A `Sealed{literal}` object was tried first and does not work: the compiler
 * emits the literal that initialises the object unless it can constant-initialise
 * it, and the plaintext came straight back into the artifact.
 *
 * The generated table declares its own short namespace so this header owns no
 * namespace of its own: a project-wide rename of the framework namespace then
 * cannot leave the two halves disagreeing, which is exactly how the first
 * regeneration broke the build.
 */

#pragma once

#include <cstddef>
#include <string>

#include "common/sealed_strings.h"

/// Convenience overloads so call sites cannot pass a mismatched length.
#define SEALED(table) \
    ::rtstr::Open(::rtstr::table, ::rtstr::table##Size)
