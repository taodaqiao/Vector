#include <core/sanitizer.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <string>
#include <string_view>
#include <vector>


#include <core/native_api.h>

#include "common/logging.h"
#include "common/obscure.h"

namespace sys::rt::sanitizer {
namespace {

// --- Re-entry guard without thread-local storage --------------------------
// These two guards used to be `thread_local`, which on this target means emulated TLS: the first
// access on a thread calls __emutls_get_address, which allocates the control block through
// malloc.  The filter runs inside open/openat, that is, on the allocator's own slow path, so that
// allocation could re-enter the filter on the very thread still setting its TLS up.  Tombstones
// from com.google.android.gms showed exactly that chain:
//   SyscallImpl -> __emutls_get_address -> malloc -> je_malloc -> je_tsd_init_finish -> fault
// Native TLS is not available either: the NDK stubs do not export __tls_get_addr at this API
// level, so the link fails outright.  The state therefore lives in a small fixed table keyed by
// the thread pointer, which is read through pthread_self() and never allocates.
constexpr std::size_t kGuardSlots = 1024;
constexpr std::size_t kGuardProbes = 16;

struct GuardSlot {
    std::atomic<unsigned long> owner{0};
    int depth = 0;
    int loader_depth = 0;
};

GuardSlot g_guard_slots[kGuardSlots];

/// The slot this thread owns, acquiring one on first use.  Null only if the table is full, in
/// which case the caller runs unguarded rather than failing.
GuardSlot *FindGuardSlot() {
    unsigned long key = static_cast<unsigned long>(static_cast<unsigned long long>(pthread_self())) >> 4;
    if (key == 0) key = 1;  // 0 marks a free slot.
    for (std::size_t probe = 0; probe < kGuardProbes; ++probe) {
        GuardSlot &slot = g_guard_slots[(key + probe * 97) % kGuardSlots];
        if (slot.owner.load(std::memory_order_acquire) == key) return &slot;
        unsigned long expected = 0;
        if (slot.owner.compare_exchange_strong(expected, key, std::memory_order_acq_rel)) {
            return &slot;
        }
    }
    return nullptr;
}

void ReleaseGuardSlot(GuardSlot *slot) {
    if (slot != nullptr && slot->depth == 0 && slot->loader_depth == 0) {
        slot->owner.store(0, std::memory_order_release);
    }
}

/// Non-zero while a filter hook of ours is on the stack for this thread.
int GuardDepth() {
    const GuardSlot *slot = FindGuardSlot();
    return slot != nullptr ? slot->depth : 0;
}

void GuardEnter() {
    if (GuardSlot *slot = FindGuardSlot()) ++slot->depth;
}

void GuardLeave() {
    GuardSlot *slot = FindGuardSlot();
    if (slot == nullptr) return;
    if (slot->depth > 0) --slot->depth;
    ReleaseGuardSlot(slot);
}

/// Same, for the two hooks the dynamic linker itself may reach.
int LoaderDepth() {
    const GuardSlot *slot = FindGuardSlot();
    return slot != nullptr ? slot->loader_depth : 0;
}

void LoaderEnter() {
    if (GuardSlot *slot = FindGuardSlot()) ++slot->loader_depth;
}

void LoaderLeave() {
    GuardSlot *slot = FindGuardSlot();
    if (slot == nullptr) return;
    if (slot->loader_depth > 0) --slot->loader_depth;
    ReleaseGuardSlot(slot);
}

std::atomic<bool> g_installed{false};

/// Lowercase fragments; any line containing one of them is not handed back.
std::vector<std::string> g_tokens;

// --- Original functions ---------------------------------------------------
using open_fn = int (*)(const char *, int, ...);
using openat_fn = int (*)(int, const char *, int, ...);
using fopen_fn = FILE *(*)(const char *, const char *);
using readlink_fn = ssize_t (*)(const char *, char *, size_t);
using readlinkat_fn = ssize_t (*)(int, const char *, char *, size_t);
using execve_fn = int (*)(const char *, char *const[], char *const[]);
using syscall_fn = long (*)(long, ...);

syscall_fn real_syscall = nullptr;

open_fn real_open = nullptr;
open_fn real_open_2 = nullptr;
openat_fn real_openat = nullptr;
openat_fn real_openat_2 = nullptr;
fopen_fn real_fopen = nullptr;
readlink_fn real_readlink = nullptr;
readlinkat_fn real_readlinkat = nullptr;
execve_fn real_execve = nullptr;

/// Upper bound on a single procfs snapshot we are willing to copy.
constexpr size_t kMaxSnapshot = 8u << 20;

/// Longest path we agree to interpret; anything longer is passed through.
constexpr size_t kMaxPath = 512;

/// The name shown in /proc/self/maps for snapshots we create.  ART uses the
/// same one for its JIT cache, so a stray mapping is unremarkable.
constexpr char kSnapshotName[] = "jit-cache";

void AppendToken(std::string_view token) {
    std::string lowered(token);
    for (auto &c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto &existing : g_tokens) {
        if (existing == lowered) return;
    }
    g_tokens.push_back(std::move(lowered));
}

void BuildTokenList() {
    if (!g_tokens.empty()) return;
    // Ciphertext only: a plaintext token table would itself be the string a
    // scanner greps for, so the words below never appear in the artifact.
    // Anything living under the module tree: the module directory itself, the
    // framework's private data directory, and every sibling module's entries.
    AppendToken(SEALED(kTokenDataAdb));
    // Named components.  These survive independently of the install path, e.g.
    // in a copied binary or a second-stage mapping.
    AppendToken(SEALED(kTokenLsposed));
    AppendToken(SEALED(kTokenLspd));
    AppendToken(SEALED(kTokenVector));
    AppendToken(SEALED(kTokenZygisk));
    AppendToken(SEALED(kTokenLsplant));
    AppendToken(SEALED(kTokenDobby));
    AppendToken(SEALED(kTokenXposed));
    AppendToken(SEALED(kTokenRobv));
    AppendToken(SEALED(kTokenLibXposed));
    AppendToken(SEALED(kTokenMagisk));
    AppendToken(SEALED(kTokenKernelSu));
    AppendToken(SEALED(kTokenApatch));
    AppendToken(SEALED(kTokenRiru));
    AppendToken(SEALED(kTokenShamiko));
}

/**
 * @brief Detects a mapping line that describes anonymous executable memory.
 *
 * Hook engines allocate their trampolines with `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)`
 * and never give the region a name, so it shows up as an executable line with an
 * empty pathname column.  Ordinary processes have almost none of those - the JIT
 * cache, the boot image and every library mapping are named - which makes "exec but
 * nameless" a cheap signature of instrumentation even after every token-bearing
 * line has been removed.
 *
 * Only `maps`-shaped lines are considered, and `smaps` field lines such as
 * `VmFlags:` do not have an address range, so they are rejected by the leading
 * hex-dash-hex check.
 */
bool IsAnonymousExecutable(std::string_view line) {
    const std::size_t range_end = line.find(' ');
    if (range_end == std::string_view::npos) return false;
    const std::string_view range = line.substr(0, range_end);
    if (range.find('-') == std::string_view::npos) return false;

    const std::size_t perms_end = line.find(' ', range_end + 1);
    if (perms_end == std::string_view::npos) return false;
    const std::string_view perms = line.substr(range_end + 1, perms_end - range_end - 1);
    if (perms.size() != 4 || perms[2] != 'x') return false;

    // Skip offset, device and inode; whatever follows the fifth field is the pathname.
    std::size_t pos = perms_end;
    for (int field = 0; field < 3; ++field) {
        pos = line.find(' ', pos + 1);
        if (pos == std::string_view::npos) return false;
    }
    std::string_view pathname = line.substr(pos + 1);
    while (!pathname.empty() && pathname.front() == ' ') pathname.remove_prefix(1);
    return pathname.empty();
}

/**
 * The other half of the same question: "is any named page writable and executable at
 * once?".  A normal process never has one -- the loader maps libraries read-only plus
 * executable, or writable without execute for relocations.  An inline-hook engine has
 * to, because writing a jump into a function means writing into code that lives in a
 * named library.  Dobby does exactly that: `code-patch-tool-posix.cc` mprotects the
 * patched range to PROT_READ | PROT_WRITE | PROT_EXEC and leaves it that way for the
 * life of the process, so `libart.so` keeps showing up as `rwxp`.
 *
 * Leaving the page writable is not something this layer can change -- that is the hook
 * engine's design.  Not reporting it is: the line keeps its address, offset, device,
 * inode and pathname, and only the permission column drops the `w`.  Anything that
 * parses maps for addresses and names is unaffected; the single question that matters
 * here stops having an answer.
 */
bool MaskWritableExecutable(std::string_view line, std::string *out) {
    const std::size_t range_end = line.find(' ');
    if (range_end == std::string_view::npos) return false;
    if (line.substr(0, range_end).find('-') == std::string_view::npos) return false;

    const std::size_t perms_end = line.find(' ', range_end + 1);
    if (perms_end == std::string_view::npos) return false;
    const std::string_view perms = line.substr(range_end + 1, perms_end - range_end - 1);
    if (perms.size() != 4 || perms[0] != 'r' || perms[1] != 'w' || perms[2] != 'x') return false;

    out->assign(line);
    (*out)[range_end + 2] = '-';
    return true;
}

bool ContainsToken(std::string_view line) {
    // No allocation on the common path: a maps line is short, and this runs for every line of a
    // file an app may read in a loop. The stack buffer covers the overwhelming majority; the
    // std::string branch exists only for the rare long path.
    char buffer[512];
    const size_t length = line.size();
    std::string fallback;
    char *lowered = buffer;
    if (length >= sizeof(buffer)) {
        fallback.assign(line);
        lowered = fallback.data();
    } else {
        std::memcpy(buffer, line.data(), length);
        buffer[length] = '\0';
    }
    for (size_t i = 0; i < length; ++i) {
        lowered[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowered[i])));
    }
    const std::string_view view(lowered, length);
    for (const auto &token : g_tokens) {
        if (view.find(token) != std::string_view::npos) return true;
    }
    return false;
}

// --- Path classification --------------------------------------------------

enum class ProcFile {
    kNone,
    kMaps,    ///< maps, smaps, smaps_rollup, numa_maps
    kMounts,  ///< mountinfo, mounts
    kUnix,    ///< net/unix
    kFdLink,  ///< fd/N, map_files/* -- readlink only
};

/**
 * @brief Decides whether a path names a procfs view worth redacting.
 *
 * Accepts the three spellings a caller may use to reach its own process:
 * `/proc/self/...`, `/proc/thread-self/...` and `/proc/<pid>/...`.  Relative
 * paths are not interpreted: that would need the dirfd resolved, and no
 * mainstream maps reader does it.
 */
ProcFile Classify(std::string_view path) {
    if (path.size() < 7 || path.size() > kMaxPath) return ProcFile::kNone;
    if (path.compare(0, 6, "/proc/") != 0) return ProcFile::kNone;

    std::string_view rest = path.substr(6);
    size_t slash = rest.find('/');
    if (slash == std::string_view::npos) return ProcFile::kNone;
    std::string_view selector = rest.substr(0, slash);
    if (selector.empty()) return ProcFile::kNone;
    if (selector != "self" && selector != "thread-self") {
        for (char c : selector) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return ProcFile::kNone;
        }
    }

    std::string_view tail = rest.substr(slash + 1);
    if (tail.empty()) return ProcFile::kNone;

    if (tail == "maps" || tail == "smaps" || tail == "smaps_rollup" || tail == "numa_maps") {
        return ProcFile::kMaps;
    }
    if (tail == "mountinfo" || tail == "mounts") return ProcFile::kMounts;
    if (tail == "net/unix") return ProcFile::kUnix;
    // /proc/<pid>/task/<tid>/(maps|smaps|mounts|...)
    if (tail.compare(0, 5, "task/") == 0) {
        std::string_view inner = tail.substr(5);
        size_t inner_slash = inner.find('/');
        if (inner_slash == std::string_view::npos) return ProcFile::kNone;
        return Classify(std::string("/proc/self/") + std::string(inner.substr(inner_slash + 1)));
    }
    // /proc/<pid>/map_files/<range> -- readlink leaks the backing file.
    if (tail.compare(0, 10, "map_files/") == 0) return ProcFile::kFdLink;
    // /proc/<pid>/fd/<n> -- readlink leaks the backing file.
    if (tail.compare(0, 3, "fd/") == 0) {
        std::string_view inner = tail.substr(3);
        if (inner.empty()) return ProcFile::kNone;
        for (char c : inner) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return ProcFile::kNone;
        }
        return ProcFile::kFdLink;
    }
    return ProcFile::kNone;
}

bool IsWatched(std::string_view path) { return Classify(path) != ProcFile::kNone; }

// --- Snapshot construction ------------------------------------------------

/**
 * @brief Builds an inheritable descriptor holding @p path with watched lines removed.
 *
 * @param path           The procfs view to copy.
 * @param close_on_exec  Whether the returned descriptor should carry FD_CLOEXEC.
 * @return A descriptor positioned at the start of the snapshot, or -1.
 */
int CreateSnapshotFd(std::string_view path, bool close_on_exec) {
    if (!real_open) {
        errno = ENOSYS;
        return -1;
    }
    std::string owned(path);
    int raw = real_open(owned.c_str(), O_RDONLY | O_CLOEXEC);
    if (raw < 0) return -1;

    std::string data;
    char buffer[8192];
    ssize_t got = 0;
    while ((got = read(raw, buffer, sizeof(buffer))) > 0) {
        data.append(buffer, static_cast<size_t>(got));
        if (data.size() >= kMaxSnapshot) break;
    }
    int read_errno = errno;
    close(raw);
    if (got < 0) {
        errno = read_errno;
        return -1;
    }

    std::string filtered;
    const bool maps_like = Classify(path) == ProcFile::kMaps;
    filtered.reserve(data.size());
    size_t start = 0;
    while (start <= data.size()) {
        size_t end = data.find('\n', start);
        size_t length = (end == std::string::npos ? data.size() : end) - start;
        std::string_view line(data.data() + start, length);
        const bool hidden = ContainsToken(line) || (maps_like && IsAnonymousExecutable(line));
        if (!hidden) {
            // An `rwx` column on a named page is what an inline hook leaves behind;
            // see MaskWritableExecutable for why it is rewritten rather than removed.
            std::string masked;
            if (maps_like && MaskWritableExecutable(line, &masked)) {
                filtered.append(masked);
            } else {
                filtered.append(line);
            }
            filtered.push_back('\n');
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    // Once `syscall` itself is hooked, calling it here would detour through our
    // own wrapper for a call it does not act on.  Harmless, but the wrapper has
    // enough on its plate without re-entering on every snapshot we build.
    long memfd = real_syscall != nullptr ? real_syscall(__NR_memfd_create, kSnapshotName, 0)
                                         : syscall(__NR_memfd_create, kSnapshotName, 0);
    int fd = static_cast<int>(memfd);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < filtered.size()) {
        ssize_t n = write(fd, filtered.data() + written, filtered.size() - written);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        written += static_cast<size_t>(n);
    }
    lseek(fd, 0, SEEK_SET);
    fcntl(fd, F_SETFD, close_on_exec ? FD_CLOEXEC : 0);
    return fd;
}

// --- Hook implementations -------------------------------------------------

mode_t ExtractMode(int flags, va_list args) {
#ifdef O_TMPFILE
    if (flags & (O_CREAT | O_TMPFILE)) return static_cast<mode_t>(va_arg(args, int));
#else
    if (flags & O_CREAT) return static_cast<mode_t>(va_arg(args, int));
#endif
    return 0;
}

/**
 * @brief Marks the current thread as "inside a handler" for the length of a scope.
 *
 * Every handler below allocates memory (std::string, std::vector) and calls into
 * the C library, and the C library can call back into the very functions being
 * intercepted - the allocator initialises thread state, and its slow paths touch
 * files.  Without this marker such a callback re-enters the handler, allocates
 * again, and recurses until the thread runs off the end of its stack.
 *
 * The fix is the one every syscall filter uses: while inside a handler, forward
 * straight to the saved original.  The filter does not need to hide its own
 * reads from itself.
 */
struct HookDepth {
    HookDepth() { GuardEnter(); }
    ~HookDepth() { GuardLeave(); }
    HookDepth(const HookDepth &) = delete;
    HookDepth &operator=(const HookDepth &) = delete;
};

int OpenImpl(const char *path, int flags, va_list args) {
    mode_t mode = ExtractMode(flags, args);
    if (!real_open) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() == 0 && path && IsWatched(path)) {
        HookDepth depth_guard;
        int fd = CreateSnapshotFd(path, (flags & O_CLOEXEC) != 0);
        if (fd >= 0) return fd;
    }
    return real_open(path, flags, mode);
}

int OpenVariadic(const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    int result = OpenImpl(path, flags, args);
    va_end(args);
    return result;
}

int OpenAtImpl(int dirfd, const char *path, int flags, va_list args) {
    mode_t mode = ExtractMode(flags, args);
    if (!real_openat) {
        errno = ENOSYS;
        return -1;
    }
    // Only absolute paths are interpreted; a relative one would need dirfd.
    if (GuardDepth() == 0 && path && path[0] == '/' && IsWatched(path)) {
        HookDepth depth_guard;
        int fd = CreateSnapshotFd(path, (flags & O_CLOEXEC) != 0);
        if (fd >= 0) return fd;
    }
    return real_openat(dirfd, path, flags, mode);
}

int OpenAtVariadic(int dirfd, const char *path, int flags, ...) {
    va_list args;
    va_start(args, flags);
    int result = OpenAtImpl(dirfd, path, flags, args);
    va_end(args);
    return result;
}

/**
 * @brief Handler for the FORTIFY variant `__open_2`.
 *
 * It takes no mode argument -- that is the point of the variant -- so it must
 * not walk a varargs list: reading an argument the caller never pushed is a
 * stack read out of bounds, and doing that on the busiest syscall wrapper in a
 * starting process is how a phone stops booting.
 */
[[maybe_unused]] int Open2Impl(const char *path, int flags) {
    if (!real_open) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() == 0 && path && IsWatched(path)) {
        HookDepth depth_guard;
        int fd = CreateSnapshotFd(path, (flags & O_CLOEXEC) != 0);
        if (fd >= 0) return fd;
    }
    return real_open(path, flags, 0);
}

[[maybe_unused]] int OpenAt2Impl(int dirfd, const char *path, int flags) {
    if (!real_openat) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() == 0 && path && path[0] == '/' && IsWatched(path)) {
        HookDepth depth_guard;
        int fd = CreateSnapshotFd(path, (flags & O_CLOEXEC) != 0);
        if (fd >= 0) return fd;
    }
    return real_openat(dirfd, path, flags, 0);
}

FILE *FopenImpl(const char *path, const char *mode) {
    if (!real_fopen) {
        errno = ENOSYS;
        return nullptr;
    }
    if (GuardDepth() != 0 || !path || !IsWatched(path)) return real_fopen(path, mode);
    HookDepth depth_guard;
    int fd = CreateSnapshotFd(path, true);
    if (fd < 0) return real_fopen(path, mode);
    FILE *file = fdopen(fd, mode);
    if (!file) {
        close(fd);
        return real_fopen(path, mode);
    }
    return file;
}

ssize_t OverwriteWithDecoy(char *buffer, size_t size) {
    // The link points at something we do not admit to having mapped.  An ashmem
    // name is what a runtime mapping looks like, and denies the reader nothing
    // it could prove against.
    static constexpr char kDecoy[] = "/dev/ashmem";
    size_t length = std::min(sizeof(kDecoy) - 1, size);
    if (length == 0) return -1;
    memcpy(buffer, kDecoy, length);
    return static_cast<ssize_t>(length);
}

ssize_t ReadlinkImpl(const char *path, char *buffer, size_t size) {
    if (!real_readlink) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() != 0 || !path) return real_readlink(path, buffer, size);
    HookDepth depth_guard;
    ssize_t result = real_readlink(path, buffer, size);
    if (result <= 0 || !IsWatched(path)) return result;
    std::string_view target(buffer, static_cast<size_t>(result));
    if (!ContainsToken(target)) return result;
    ssize_t decoy = OverwriteWithDecoy(buffer, size);
    return decoy < 0 ? result : decoy;
}

ssize_t ReadlinkatImpl(int dirfd, const char *path, char *buffer, size_t size) {
    if (!real_readlinkat) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() != 0 || !path || path[0] != '/') {
        return real_readlinkat(dirfd, path, buffer, size);
    }
    HookDepth depth_guard;
    ssize_t result = real_readlinkat(dirfd, path, buffer, size);
    if (result <= 0 || !IsWatched(path)) {
        return result;
    }
    std::string_view target(buffer, static_cast<size_t>(result));
    if (!ContainsToken(target)) return result;
    ssize_t decoy = OverwriteWithDecoy(buffer, size);
    return decoy < 0 ? result : decoy;
}

/**
 * @brief Replaces every watched procfs path inside a string with a snapshot path.
 *
 * One code path covers both `cat /proc/self/maps`, where the path is an
 * argument of its own, and `sh -c "cat /proc/self/maps"`, where it is embedded
 * in a script string.
 */
bool RewriteString(const char *input, std::string *output) {
    output->assign(input);
    bool changed = false;
    size_t search = 0;
    while ((search = output->find("/proc/", search)) != std::string::npos) {
        size_t end = search;
        while (end < output->size()) {
            char c = (*output)[end];
            if (c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\'' || c == ';' ||
                c == '|' || c == '&' || c == ')' || c == '>') {
                break;
            }
            ++end;
        }
        std::string_view candidate(output->data() + search, end - search);
        if (!IsWatched(candidate)) {
            search = end + 1;
            continue;
        }
        int fd = CreateSnapshotFd(candidate, false);
        if (fd < 0) {
            search = end + 1;
            continue;
        }
        std::string replacement = "/proc/self/fd/" + std::to_string(fd);
        output->replace(search, candidate.size(), replacement);
        search += replacement.size();
        changed = true;
    }
    return changed;
}

/**
 * @brief Rewrites procfs paths in the argument vector before handing it to exec.
 *
 * An exec'd helper keeps none of our hooks but does inherit descriptors, so the
 * argument is redirected to a snapshot descriptor that survives the exec.
 */
int ExecveImpl(const char *path, char *const argv[], char *const envp[]) {
    if (!real_execve) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() != 0 || !argv) return real_execve(path, argv, envp);

    HookDepth depth_guard;
    std::vector<std::string> rewritten;
    std::vector<size_t> rewritten_slots;
    bool changed = false;
    for (size_t i = 0; argv[i] != nullptr; ++i) {
        std::string candidate;
        if (RewriteString(argv[i], &candidate)) {
            rewritten.push_back(std::move(candidate));
            rewritten_slots.push_back(i);
            changed = true;
        }
    }
    if (!changed) return real_execve(path, argv, envp);

    size_t count = rewritten_slots.size();
    std::vector<char *> final_argv;
    for (size_t i = 0; argv[i] != nullptr; ++i) final_argv.push_back(argv[i]);
    for (size_t i = 0; i < count; ++i) {
        final_argv[rewritten_slots[i]] = rewritten[i].data();
    }
    final_argv.push_back(nullptr);

    int result = real_execve(path, final_argv.data(), envp);
    return result;
}

/**
 * @brief Hides our images from the loader's own module list.
 *
 * `/proc/self/maps` is not the only way to enumerate what is loaded in a
 * process: `dl_iterate_phdr()` walks the linker's list in memory and reports
 * each object's original `dlpi_name`, which still says
 * `<module dir>/zygisk/<abi>.so`.  The filter therefore has to run there too,
 * by wrapping the caller's callback and dropping the entries we filter.
 */
int (*real_dl_iterate_phdr)(int (*)(dl_phdr_info *, size_t, void *), void *);

/// Guards the two hooks that the dynamic linker itself may reach, so a callback
/// that resolves a symbol cannot come back through the same hook.
struct LoaderHookGuard {
    LoaderHookGuard() { LoaderEnter(); }
    ~LoaderHookGuard() { LoaderLeave(); }
};

struct IterateContext {
    int (*callback)(dl_phdr_info *, size_t, void *);
    void *data;
};

int IterateTrampoline(dl_phdr_info *info, size_t size, void *data) {
    auto *context = static_cast<IterateContext *>(data);
    if (info != nullptr && info->dlpi_name != nullptr && ContainsToken(info->dlpi_name)) {
        return 0;  // Report "keep going" exactly as an accepted entry would.
    }
    return context->callback(info, size, context->data);
}

int DlIteratePhdrImpl(int (*callback)(dl_phdr_info *, size_t, void *), void *data) {
    if (!real_dl_iterate_phdr) {
        errno = ENOSYS;
        return -1;
    }
    if (GuardDepth() != 0 || callback == nullptr) return real_dl_iterate_phdr(callback, data);
    if (LoaderDepth() != 0) return real_dl_iterate_phdr(callback, data);
    LoaderHookGuard guard;
    IterateContext context{callback, data};
    return real_dl_iterate_phdr(IterateTrampoline, &context);
}

/**
 * @brief Scrubs the object name `dladdr()` reports.
 *
 * Same leak as above through a different door: a detector can resolve any
 * address to its owning image and read the path back out of `Dl_info`.
 */
int (*real_dladdr)(const void *, Dl_info *);

int DladdrImpl(const void *address, Dl_info *info) {
    if (!real_dladdr) {
        errno = ENOSYS;
        return 0;
    }
    if (LoaderDepth() != 0) return real_dladdr(address, info);
    LoaderHookGuard guard;
    int result = real_dladdr(address, info);
    if (result != 0 && GuardDepth() == 0 && info != nullptr && info->dli_fname != nullptr &&
        ContainsToken(info->dli_fname)) {
        info->dli_fname = "";
    }
    return result;
}

/**
 * @brief Everything the loader exports for this module, hidden behind `dlsym`.
 *
 * `zygisk_module_entry` stays exported because that is how the Zygisk loader finds the module, and
 * the dynamic symbol table is read-only once the image is mapped. It is also the shortest detection
 * there is: `dlsym(RTLD_DEFAULT, "zygisk_module_entry")` returning non-null proves a Zygisk module
 * is loaded in this very process, with no procfs involved. So the name is filtered at the call:
 * a caller inside an injected image reaches this hook through its own GOT, our own calls do not
 * (our image is skipped when the slots are rewritten), and the loader has already done its lookup
 * by the time the hooks are installed.
 */
void *(*real_dlsym)(void *, const char *);

bool IsHiddenSymbol(const char *name) {
    if (name == nullptr) return false;
    // Kept in a static so the sealed string is decoded once instead of on every dlsym call.
    static const std::string token = SEALED(kTokenZygisk);
    return std::strstr(name, token.c_str()) != nullptr;
}

void *DlsymImpl(void *handle, const char *name) {
    if (!real_dlsym) {
        errno = ENOSYS;
        return nullptr;
    }
    if (GuardDepth() == 0 && IsHiddenSymbol(name)) return nullptr;
    return real_dlsym(handle, name);
}

void *(*real_dlvsym)(void *, const char *, const char *);

void *DlvsymImpl(void *handle, const char *name, const char *version) {
    if (!real_dlvsym) {
        errno = ENOSYS;
        return nullptr;
    }
    if (GuardDepth() == 0 && IsHiddenSymbol(name)) return nullptr;
    return real_dlvsym(handle, name, version);
}

// --- GOT hooking -----------------------------------------------------------//
// These are GOT hooks, not inline patches.  An inline hooker came first and took
// SIGSEGV on Android 16 arm64: `opendir+24` is `bl __open_2@plt`, and every
// injected process died at the same unmapped address, so the trampoline -- not
// the callee -- was at fault.  A GOT slot is a data word the loader already
// rewrites for itself: nothing is copied, nothing is executed from new memory,
// and the call still satisfies CFI and the shadow call stack because the
// replacement keeps the callee's prototype.
//
// The vendored PLT helper came second and had to go.  It decided which library a
// mapping belonged to by matching page offsets, and on APEX images with several
// mappings of one file that match can land on the wrong mapping; the handler then
// called a "backup" value that had never been a function pointer, so the faulting
// PC was an address no mapping covered -- 0x2c15747588 in `art::OatFile::Open`,
// 0x2d4e982d88 in `opendir`, different every run.  The slot arithmetic below is
// therefore narrow on purpose: a slot is `load bias + r_offset` from that image's
// own DT_JMPREL, and the callee is `dlsym`, never the slot's previous contents
// (a lazily bound slot still holds the resolver stub, and calling that is the
// same crash in a different disguise).

struct HookSpec {
    const char *symbol;
    void *replacement;
    void **backup;
};

struct GotPatch {
    uintptr_t slot;
    uintptr_t original;
    int perms;
};

std::vector<GotPatch> g_patches;

size_t PageSize() {
    static const size_t cached = [] {
        long size = sysconf(_SC_PAGESIZE);
        return size > 0 ? static_cast<size_t>(size) : static_cast<size_t>(4096);
    }();
    return cached;
}

/**
 * @brief Whether a loaded image is worth patching.
 *
 * A GOT hook only bites in an image that imports the symbol, and the importers
 * we care about are few: the runtime's file API (libjavacore), the runtime, the
 * app_process binary, and the app's own libraries.
 *
 * bionic itself is deliberately absent.  libc reaches its own entry points
 * through self-relocations, and rewriting those takes `opendir()` down with it:
 * `opendir+24` is `bl __open_2@plt`, so a bad slot there faults before any of
 * our code runs.  Nothing is lost by skipping it -- every call we need to see is
 * issued by another image and passes through that image's own GOT.
 */
bool IsHookWorthyImage(std::string_view path) {
    static constexpr std::string_view kInterest[] = {
            "libjavacore.so", "libart.so", "libnativehelper.so",
            "libandroid_runtime.so", "app_process", "/data/app/", "/data/data/",
            "/data/user/",
    };
    for (std::string_view name : kInterest) {
        if (path.find(name) != std::string_view::npos) return true;
    }
    return false;
}

/**
 * @brief The permissions the containing segment was mapped with.
 *
 * Restoring them matters: leaving a GOT page writable is visible in
 * `/proc/self/maps` to anything that scans for it, which is the one thing this
 * file exists to prevent.
 */
int PermsForSlot(const dl_phdr_info *info, uintptr_t addr) {
    const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) continue;
        const uintptr_t start = base + static_cast<uintptr_t>(phdr.p_vaddr);
        if (addr < start || addr >= start + static_cast<uintptr_t>(phdr.p_memsz)) continue;
        int perms = 0;
        if ((phdr.p_flags & PF_R) != 0) perms |= PROT_READ;
        if ((phdr.p_flags & PF_W) != 0) perms |= PROT_WRITE;
        if ((phdr.p_flags & PF_X) != 0) perms |= PROT_EXEC;
        return perms;
    }
    // Zero means "this address does not belong to the image".  Callers must treat that
    // as "do not touch".  An earlier version fell back to PROT_READ and restored the
    // page that way, which turned an address computed from a bogus relocation into a
    // permanently read-only page somewhere else in the process - the exact shape of
    // the ACCERR crashes that followed.
    return 0;
}

bool PatchSlot(uintptr_t slot, uintptr_t value, int perms, uintptr_t *original) {
    if (perms == 0) return false;
    const uintptr_t page_size = PageSize();
    const uintptr_t page = slot & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE) != 0) {
        return false;
    }
    *original = *reinterpret_cast<uintptr_t *>(slot);
    *reinterpret_cast<uintptr_t *>(slot) = value;
    mprotect(reinterpret_cast<void *>(page), page_size, perms);
    return true;
}

/**
 * @brief Patches the matching GOT slots of one image.
 *
 * @return how many slots in this image were rewritten.
 */
size_t PatchImage(const dl_phdr_info *info, const std::vector<HookSpec> &specs) {
    const uintptr_t base = static_cast<uintptr_t>(info->dlpi_addr);
    const ElfW(Phdr) *dynamic_phdr = nullptr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) continue;
        dynamic_phdr = &info->dlpi_phdr[i];
        break;
    }
    if (dynamic_phdr == nullptr) return 0;
    const auto *dynamic =
        reinterpret_cast<const ElfW(Dyn) *>(base + static_cast<uintptr_t>(dynamic_phdr->p_vaddr));
    // Bounded by the segment, not by DT_NULL alone: an image is free to ship a dynamic array with
    // no terminator, and walking off the end of a mapped page would take the process down at
    // injection time. The whitelist includes every installed app, so this is reachable input.
    const size_t max_entries = dynamic_phdr->p_memsz / sizeof(ElfW(Dyn));

    const char *strtab = nullptr;
    const ElfW(Sym) *symtab = nullptr;
    const void *jmprel = nullptr;
    size_t pltrelsz = 0;
    ElfW(Sxword) pltrel = DT_RELA;
    for (size_t i = 0; i < max_entries && dynamic[i].d_tag != DT_NULL; ++i) {
        const ElfW(Dyn) &entry = dynamic[i];
        switch (entry.d_tag) {
            case DT_STRTAB:
                strtab = reinterpret_cast<const char *>(base + static_cast<uintptr_t>(entry.d_un.d_ptr));
                break;
            case DT_SYMTAB:
                symtab = reinterpret_cast<const ElfW(Sym) *>(base + static_cast<uintptr_t>(entry.d_un.d_ptr));
                break;
            case DT_JMPREL:
                jmprel = reinterpret_cast<const void *>(base + static_cast<uintptr_t>(entry.d_un.d_ptr));
                break;
            case DT_PLTRELSZ:
                pltrelsz = static_cast<size_t>(entry.d_un.d_val);
                break;
            case DT_PLTREL:
                pltrel = static_cast<ElfW(Sxword)>(entry.d_un.d_val);
                break;
            default:
                break;
        }
    }
    if (strtab == nullptr || symtab == nullptr || jmprel == nullptr || pltrelsz == 0) return 0;

    size_t patched = 0;
    size_t skipped = 0;
    auto handle = [&](uintptr_t slot, ElfW(Xword) reloc_info) {
#if defined(__LP64__)
        const size_t symbol_index = static_cast<size_t>(ELF64_R_SYM(reloc_info));
#else
        // A 32-bit r_info packs the symbol index in the top 24 bits; ELF64_R_SYM would shift a
        // 32-bit value by 32, which is undefined and yields 0 in practice -- every relocation would
        // then be compared against symbol 0 and the image would be left unpatched.
        const size_t symbol_index = static_cast<size_t>(ELF32_R_SYM(reloc_info));
#endif
        const char *name = strtab + symtab[symbol_index].st_name;
        for (const HookSpec &spec : specs) {
            if (std::strcmp(name, spec.symbol) != 0) continue;
            // A handler without its real implementation cannot forward, so do not install one.
            if (spec.backup == nullptr || *spec.backup == nullptr) return;
            // Only addresses that really live inside one of this image's PT_LOAD
            // segments are slots.  Anything else is a relocation this reader
            // misparsed, and writing there is how a filter turns into a crash.
            const int perms = PermsForSlot(info, slot);
            if (perms == 0) {
                ++skipped;
                return;
            }
            const uintptr_t replacement = reinterpret_cast<uintptr_t>(spec.replacement);
            if (*reinterpret_cast<uintptr_t *>(slot) == replacement) return;  // already done
            uintptr_t original = 0;
            if (!PatchSlot(slot, replacement, perms, &original)) {
                LOGW("sanitizer: cannot write the {} slot of {}", name, info->dlpi_name);
                return;
            }
            g_patches.push_back({slot, original, perms});
            ++patched;
            return;
        }
    };

    if (pltrel == DT_RELA) {
        const auto *rela = static_cast<const ElfW(Rela) *>(jmprel);
        const size_t count = pltrelsz / sizeof(ElfW(Rela));
        for (size_t i = 0; i < count; ++i) {
            handle(base + static_cast<uintptr_t>(rela[i].r_offset), rela[i].r_info);
        }
    } else if (pltrel == DT_REL) {
        const auto *rel = static_cast<const ElfW(Rel) *>(jmprel);
        const size_t count = pltrelsz / sizeof(ElfW(Rel));
        for (size_t i = 0; i < count; ++i) {
            handle(base + static_cast<uintptr_t>(rel[i].r_offset), rel[i].r_info);
        }
    }
    if (skipped != 0) {
        LOGW("sanitizer: skipped {} slot(s) that sit outside the load segments of {}", skipped,
             info->dlpi_name);
    }
    return patched;
}

/// Address Dobby patched for `syscall`, kept so a failed install can undo it.
void *g_syscall_target = nullptr;

/**
 * @brief Puts every rewritten slot back the way it was found.
 *
 * The originals come from the slots themselves, captured before the write, so
 * this restores the exact pre-install state rather than a plausible one.
 */
void UnpatchAll() {
    for (const GotPatch &patch : g_patches) {
        uintptr_t ignored = 0;
        PatchSlot(patch.slot, patch.original, patch.perms, &ignored);
    }
    g_patches.clear();
    if (g_syscall_target != nullptr) {
        UnhookInline(g_syscall_target);
        g_syscall_target = nullptr;
        real_syscall = nullptr;
    }
}

// --- The raw-syscall path -------------------------------------------------
//
// Everything above lives in a GOT slot, and a GOT slot is only consulted by code
// that reached the symbol through the dynamic linker.  `syscall()` is the way
// around that: a caller who wants `/proc/self/maps` without touching `open()`
// writes `syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY)` and no
// slot we can reach is involved.  bionic's own wrappers each contain `svc #0`
// directly, so there is no PLT entry for this one either -- it has to be patched
// in place, which is what Dobby (already linked in for lsplant) is for.
//
// The scope is deliberately three syscall numbers.  Everything else is forwarded
// untouched, so being on the busiest variadic entry point in libc costs one
// comparison per call.

/**
 * @brief The filtered open family, as `(path, close_on_exec)`.
 *
 * @return false when this syscall number is not one we act on, or carries no
 *         absolute path we would recognise.  A relative path is passed through:
 *         resolving it would mean interpreting `dirfd`, and a detector that got
 *         this far can pass whatever it likes.
 */
bool WatchedSyscall(long number, unsigned long a1, unsigned long a2, unsigned long a3,
                    const char **path, bool *close_on_exec) {
#if defined(__NR_openat)
    if (number == __NR_openat) {
        const auto *candidate = reinterpret_cast<const char *>(a2);
        if (candidate == nullptr || candidate[0] != '/' || !IsWatched(candidate)) return false;
        *path = candidate;
        *close_on_exec = (static_cast<int>(a3) & O_CLOEXEC) != 0;
        return true;
    }
#endif
#if defined(__NR_openat2)
    if (number == __NR_openat2) {
        const auto *candidate = reinterpret_cast<const char *>(a2);
        if (candidate == nullptr || candidate[0] != '/' || !IsWatched(candidate)) return false;
        *path = candidate;
        // struct open_how starts with the same flag word; a null pointer would be
        // a caller bug and is treated as "no flags" rather than dereferenced.
        const auto *how = reinterpret_cast<const unsigned long *>(a3);
        *close_on_exec = how != nullptr && (how[0] & O_CLOEXEC) != 0;
        return true;
    }
#endif
#if defined(__NR_open)
    if (number == __NR_open) {
        const auto *candidate = reinterpret_cast<const char *>(a1);
        if (candidate == nullptr || candidate[0] != '/' || !IsWatched(candidate)) return false;
        *path = candidate;
        *close_on_exec = (static_cast<int>(a2) & O_CLOEXEC) != 0;
        return true;
    }
#endif
    (void) a1;
    (void) a2;
    (void) a3;
    return false;
}

long SyscallImpl(long number, ...) {
    va_list args;
    va_start(args, number);
    unsigned long a1 = va_arg(args, unsigned long);
    unsigned long a2 = va_arg(args, unsigned long);
    unsigned long a3 = va_arg(args, unsigned long);
    unsigned long a4 = va_arg(args, unsigned long);
    unsigned long a5 = va_arg(args, unsigned long);
    unsigned long a6 = va_arg(args, unsigned long);
    va_end(args);

    if (real_syscall == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    const char *path = nullptr;
    bool close_on_exec = false;
    if (GuardDepth() == 0 && WatchedSyscall(number, a1, a2, a3, &path, &close_on_exec)) {
        HookDepth depth_guard;
        int fd = CreateSnapshotFd(path, close_on_exec);
        if (fd >= 0) return fd;
    }
    return real_syscall(number, a1, a2, a3, a4, a5, a6);
}

/**
 * @brief Hooks `syscall` in place.
 *
 * A failure here is reported and tolerated: the raw-syscall path is one channel,
 * and losing it is not a reason to run with no filter at all.
 */
bool InstallSyscallHook() {
    void *target = dlsym(RTLD_DEFAULT, "syscall");
    if (target == nullptr) {
        LOGW("sanitizer: syscall is not resolvable, the raw-syscall path stays open");
        return false;
    }
    void *backup = nullptr;
    if (HookInline(target, reinterpret_cast<void *>(&SyscallImpl), &backup) != 0 ||
        backup == nullptr) {
        LOGW("sanitizer: cannot hook syscall, the raw-syscall path stays open");
        return false;
    }
    real_syscall = reinterpret_cast<syscall_fn>(backup);
    g_syscall_target = target;
    return true;
}

struct PatchContext {
    const std::vector<HookSpec> *specs;
    size_t patched;
};

int PatchOneImage(dl_phdr_info *info, size_t /*size*/, void *data) {
    auto *context = static_cast<PatchContext *>(data);
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        const std::string_view path{info->dlpi_name};
        // Our own image is never patched: its `open`/`fopen` calls are the filter
        // itself, and a hooked one would recurse until the stack ends.
        if (path.find("/data/adb") != std::string_view::npos) return 0;
        if (!IsHookWorthyImage(path)) return 0;
    }
    context->patched += PatchImage(info, *context->specs);
    return 0;
}

/**
 * @brief Resolves the real callees, then rewrites every eligible image's slots.
 *
 * `backup` comes from `dlsym`, not from the slot it replaces.  The slot may still
 * hold a lazy-binding stub, and a handler that calls a stub is the same unmapped
 * PC as before.  Installation happens in postAppSpecialize, before the app's own
 * code is reachable, so no handler can run between the first patch and the last.
 */
/**
 * @brief Stops the loader from unmapping this library out from under the hooks.
 *
 * Every slot we rewrite points into this image, and Zygisk is free to `dlclose()`
 * a module once its entry points have returned.  When that happens the slots are
 * left holding addresses inside an unmapped image, and the next `open()` or
 * `dl_iterate_phdr()` in the process jumps there and dies with SEGV_MAPERR at an
 * address that no mapping covers -- the exact signature every earlier attempt at
 * this filter produced, inline hooks and GOT hooks alike.
 *
 * `RTLD_NOLOAD` re-opens an image that is already in the process, so this works
 * after the app has dropped to its own uid and can no longer read /data/adb;
 * `RTLD_NODELETE` makes the mapping permanent.
 *
 * @return whether the pin succeeded.  Best effort: the caller has already promised
 *         the loader will keep this image (SetAllowUnload(false)), so a failure here
 *         is a weaker second lock, not a reason to run unfiltered.
 */
bool PinLibrary() {
    Dl_info self{};
    if (dladdr(reinterpret_cast<void *>(&PinLibrary), &self) == 0 || self.dli_fname == nullptr) {
        // Zygisk loads modules into its own linker namespace, and dladdr only sees the
        // caller's, so this is expected rather than exceptional.
        LOGV("sanitizer: this library is outside the default namespace, relying on the Zygisk unload flag");
        return false;
    }
    if (dlopen(self.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) != nullptr) {
        LOGI("sanitizer: library pinned");
        return true;
    }
    if (dlopen(self.dli_fname, RTLD_NOW | RTLD_NODELETE) != nullptr) {
        LOGI("sanitizer: library pinned by path");
        return true;
    }
    LOGW("sanitizer: cannot pin {} ({}), relying on the Zygisk unload flag", self.dli_fname, strerror(errno));
    return false;
}

bool InstallGotHooks(const std::vector<HookSpec> &specs) {
    for (const HookSpec &spec : specs) {
        if (spec.backup != nullptr && *spec.backup == nullptr) {
            *spec.backup = dlsym(RTLD_DEFAULT, spec.symbol);
        }
    }
    PatchContext context{&specs, 0};
    dl_iterate_phdr(&PatchOneImage, &context);
    LOGV("sanitizer: {} image slot(s) patched", context.patched);
    return context.patched > 0;
}

/**
 * @brief Reads `/proc/self/maps` through the hook and around it, once.
 *
 * This is the only evidence that matters: whether the process itself can still
 * see the module.  Both numbers are printed so a build that silently stopped
 * filtering is visible in logcat instead of being assumed to work.
 */
void SelfCheck() {
    auto scan = [](FILE *file, size_t *lines, size_t *writable_exec) {
        size_t marked = 0;
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), file) != nullptr) {
            ++*lines;
            if (ContainsToken(buffer)) ++marked;
            std::string masked;
            if (MaskWritableExecutable(buffer, &masked)) ++*writable_exec;
        }
        return marked;
    };
    if (!real_fopen) return;
    size_t filtered_lines = 0;
    size_t filtered_marked = 0;
    size_t filtered_rwx = 0;
    if (FILE *file = FopenImpl("/proc/self/maps", "r")) {
        filtered_marked = scan(file, &filtered_lines, &filtered_rwx);
        fclose(file);
    }
    size_t raw_lines = 0;
    size_t raw_marked = 0;
    size_t raw_rwx = 0;
    if (FILE *file = real_fopen("/proc/self/maps", "r")) {
        raw_marked = scan(file, &raw_lines, &raw_rwx);
        fclose(file);
    }
    LOGI("sanitizer self-check: in-process view {} lines / {} marked / {} rwx, "
         "raw {} lines / {} marked / {} rwx",
         filtered_lines, filtered_marked, filtered_rwx, raw_lines, raw_marked, raw_rwx);

    // The same read again, this time the way a detector would do it if it wanted
    // to skip libc entirely.  If this number ever matches `raw`, the GOT layer is
    // working and the syscall hook is not, which is exactly the state that used to
    // be invisible.
    if (real_syscall != nullptr) {
        int fd = static_cast<int>(syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY));
        if (fd >= 0) {
            std::string content;
            char chunk[4096];
            ssize_t n;
            while ((n = read(fd, chunk, sizeof(chunk))) > 0) content.append(chunk, static_cast<size_t>(n));
            close(fd);
            size_t lines = 0;
            size_t marked = 0;
            size_t writable_exec = 0;
            size_t start = 0;
            while (start < content.size()) {
                size_t end = content.find('\n', start);
                std::string_view line{content.data() + start,
                                      (end == std::string::npos ? content.size() : end) - start};
                ++lines;
                if (ContainsToken(line)) ++marked;
                std::string masked;
                if (MaskWritableExecutable(line, &masked)) ++writable_exec;
                if (end == std::string::npos) break;
                start = end + 1;
            }
            LOGI("sanitizer self-check: raw-syscall view {} lines / {} marked / {} rwx", lines, marked,
                 writable_exec);
        } else {
            LOGW("sanitizer self-check: raw-syscall open failed ({})", strerror(errno));
        }
    }

    // The loader's export table cannot be removed from a mapped image, so the proof is that a lookup
    // through the hook returns nothing while the real one still finds it.
    if (real_dlsym != nullptr) {
        const std::string entry = SEALED(kTokenZygisk) + "_module_entry";
        const bool hidden = DlsymImpl(RTLD_DEFAULT, entry.c_str()) == nullptr;
        const bool present = real_dlsym(RTLD_DEFAULT, entry.c_str()) != nullptr;
        LOGI("sanitizer self-check: module entry symbol hidden={} present={}", hidden, present);
    }
}

}  // namespace

ScopedBypass::ScopedBypass() { GuardEnter(); }
ScopedBypass::~ScopedBypass() { GuardLeave(); }

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

void Install() {
    if (g_installed.exchange(true, std::memory_order_acq_rel)) return;

    // The layer is on by default, because the process being hooked must not be
    // able to read its own maps and find us -- a hook that the target can see is
    // not a hook.  The markers exist because a wrong build that reaches a booting
    // phone is expensive to diagnose remotely:
    //
    //   <data>/sysupd/no_sanitizer  installs nothing at all
    //   <data>/sysupd/loader_only   keeps dl_iterate_phdr/dladdr, drops /proc
    const std::string data_dir = SEALED(kTokenDataAdb) + "/sysupd";
    if (access((data_dir + "/no_sanitizer").c_str(), F_OK) == 0) {
        LOGW("sanitizer: disabled by marker, installing no hooks");
        g_installed.store(false, std::memory_order_release);
        return;
    }

    BuildTokenList();
    PinLibrary();
    const bool with_proc = access((data_dir + "/loader_only").c_str(), F_OK) != 0;

    std::vector<HookSpec> specs;
    if (with_proc) {
        // No `__open_2` / `__openat_2`: those are FORTIFY wrappers that libc calls
        // internally, and redirecting them is what broke `opendir()`; the calls we
        // actually need to see arrive as plain `open`/`openat`/`fopen` from another
        // image's PLT.
        specs.push_back(
                {"open", reinterpret_cast<void *>(&OpenVariadic), reinterpret_cast<void **>(&real_open)});
        specs.push_back(
                {"openat", reinterpret_cast<void *>(&OpenAtVariadic), reinterpret_cast<void **>(&real_openat)});
        specs.push_back(
                {"fopen", reinterpret_cast<void *>(&FopenImpl), reinterpret_cast<void **>(&real_fopen)});
        // bionic exports the 64-bit spellings as separate symbols at the same addresses as the plain
        // ones (`open64` == `open`, `openat64` == `openat`, `fopen64` == `fopen`; verified with
        // llvm-readelf on the device libc). They have their own PLT entries, so patching only the
        // plain names leaves any caller that imports the 64-bit spelling reading the real file.
        specs.push_back(
                {"open64", reinterpret_cast<void *>(&OpenVariadic), reinterpret_cast<void **>(&real_open)});
        specs.push_back({"openat64", reinterpret_cast<void *>(&OpenAtVariadic),
                         reinterpret_cast<void **>(&real_openat)});
        specs.push_back(
                {"fopen64", reinterpret_cast<void *>(&FopenImpl), reinterpret_cast<void **>(&real_fopen)});
        specs.push_back(
                {"readlink", reinterpret_cast<void *>(&ReadlinkImpl), reinterpret_cast<void **>(&real_readlink)});
        specs.push_back({"readlinkat", reinterpret_cast<void *>(&ReadlinkatImpl),
                         reinterpret_cast<void **>(&real_readlinkat)});
        specs.push_back(
                {"execve", reinterpret_cast<void *>(&ExecveImpl), reinterpret_cast<void **>(&real_execve)});
    }
    specs.push_back({"dl_iterate_phdr", reinterpret_cast<void *>(&DlIteratePhdrImpl),
                     reinterpret_cast<void **>(&real_dl_iterate_phdr)});
    specs.push_back({"dladdr", reinterpret_cast<void *>(&DladdrImpl),
                     reinterpret_cast<void **>(&real_dladdr)});
    // The shortest Zygisk detection is a symbol lookup, not a file read: `dlsym` for the module
    // entry point needs no procfs and no permissions.
    specs.push_back(
            {"dlsym", reinterpret_cast<void *>(&DlsymImpl), reinterpret_cast<void **>(&real_dlsym)});
    specs.push_back({"dlvsym", reinterpret_cast<void *>(&DlvsymImpl),
                     reinterpret_cast<void **>(&real_dlvsym)});

    const bool registered = InstallGotHooks(specs);
    const bool syscall_active = with_proc && InstallSyscallHook();
    const bool loader_active = real_dl_iterate_phdr != nullptr || real_dladdr != nullptr;
    const bool proc_active = real_open != nullptr || real_openat != nullptr || real_fopen != nullptr;
    if (!registered || (!loader_active && !proc_active)) {
        // Nothing was patched anywhere.  Undo whatever landed so the process is
        // left exactly as it was found, and leave g_installed clear so a later
        // call may retry once the images the app loads lazily are present.
        UnpatchAll();
        g_installed.store(false, std::memory_order_release);
        LOGE("sanitizer: no hook landed (loader={}, proc={})", loader_active, proc_active);
        return;
    }

    LOGI("sanitizer: live handlers open={} openat={} fopen={} readlink={} exec={} phdr={} dladdr={} syscall={}",
         real_open != nullptr, real_openat != nullptr, real_fopen != nullptr,
         real_readlink != nullptr, real_execve != nullptr, real_dl_iterate_phdr != nullptr,
         real_dladdr != nullptr, syscall_active);
    SelfCheck();
    LOGI("sanitizer: installed ({} tokens, {})", g_tokens.size(),
         proc_active ? (real_execve ? "full" : "full-without-exec") : "loader-only");
}

}  // namespace sys::rt::sanitizer
