#pragma once

/**
 * @file sanitizer.h
 * @brief Process-local redaction of the traces this framework leaves behind.
 *
 * Two kinds of residue are removed:
 *
 *   1. Text read back out of procfs.  Any process that maps a module under
 *      /data/adb shows it in /proc/self/maps; the same directory appears in
 *      mount tables and, as an abstract socket, in /proc/net/unix.  In-process
 *      readers therefore never see those lines.
 *   2. Child processes spawned to read procfs on the parent's behalf
 *      (`sh -c "cat /proc/self/maps"`).  The argument naming the file is
 *      rewritten to a sealed copy that already had the lines removed, because
 *      an exec'd helper does not carry any of our hooks.
 *
 * What is deliberately *not* done: nothing is hidden at the filesystem or
 * kernel level, and no line is invented.  A command that fails to find us
 * simply sees the file it asked for, minus the lines it was looking for.
 */

namespace sys::rt::sanitizer {

/**
 * @brief Installs the redaction hooks.
 *
 * Intended to run once per injected process, after specialization but before
 * any application code executes.  Calling it from a multi-threaded context
 * would make the inline patches racy, so callers must be single-threaded at
 * that point (a specialization callback is).
 *
 * Safe to call more than once; later calls are no-ops.
 */
void Install();

/// True once the hooks have been installed in this process.
[[nodiscard]] bool IsInstalled();

/**
 * @class ScopedBypass
 * @brief Lets framework code see the unredacted files for the duration of a scope.
 *
 * The dynamic linker is the authority on which libraries are loaded, and the
 * fallback that reconstructs a module base from /proc/self/maps needs the real
 * line, not the redacted one.  Nesting is allowed; the flag is per-thread and
 * is inherited by hooks called from within the scope.
 */
class ScopedBypass {
public:
    ScopedBypass();
    ~ScopedBypass();
    ScopedBypass(const ScopedBypass &) = delete;
    ScopedBypass &operator=(const ScopedBypass &) = delete;
};

}  // namespace sys::rt::sanitizer
