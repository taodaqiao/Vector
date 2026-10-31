#include "obfuscation.h"

#include <android/sharedmem.h>
#include <android/sharedmem_jni.h>
#include <fcntl.h>
#include <jni.h>
#include <slicer/dex_utf8.h>
#include <slicer/reader.h>
#include <slicer/writer.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <utils/jni_helper.hpp>

namespace {

std::once_flag init_flag;

std::map<std::string, std::string> signatures = {
    {"Lde/robv/android/xposed/", ""},
    {"Landroid/app/AndroidApp", ""},
    {"Landroid/content/res/XRes", ""},
    {"Landroid/content/res/XModule", ""},
    {"Lio/github/libxposed/api/Xposed", ""},
    {"Lorg/matrix/vector/core/", ""},
    {"Lorg/matrix/vector/impl/", ""},
    {"Lorg/matrix/vector/nativebridge/", ""},
    {"Lorg/matrix/vector/service/", ""},
};

// Namespaces whose *class names* are replaced as well, not only their package prefix.
//
// The prefix rewrite alone leaves every descriptor looking like L<random>/XposedBridge;, and a
// simple name is the part a scanner actually matches: XposedBridge, XposedHelpers, XC_MethodHook
// and the rest are the words that a detector hunting for hooking frameworks looks for, and they sit
// in the string pool of every loaded dex exactly as the compiler wrote them. Replacing the whole
// descriptor is also the only rewrite that cannot damage an unrelated string, since the key is the
// entire string rather than a substring of it.
//
// The list is deliberately short. `org.matrix.vector.core`, `nativebridge` and `service` are
// excluded because native code composes their class names itself -- `obfs_map.At(prefix) + "Main"`,
// `+ "ResourcesHook"`, `+ "BridgeService"` -- so renaming the simple name there would leave native
// asking for a class that no longer exists. Those three packages are also the ones that carry no
// recognisable word: "Main", "NativeApiBridge", "FrameworkService" say nothing about what this is.
constexpr std::string_view simple_name_scopes[] = {
    "Lde/robv/android/xposed/",
    "Lorg/matrix/vector/impl/",
};

// Full descriptor -> replacement descriptor, learned from the framework dex the first time each
// descriptor is seen and reused verbatim for every module dex afterwards. Learning only happens for
// [simple_name_scopes], which no module class belongs to, so a module dex that is somehow processed
// before the framework dex cannot teach the table a name the framework will not agree with.
std::map<std::string, std::string> class_names;
std::mutex class_names_mutex;

// Member (method / field) names worth rewriting on their own. This is a separate table from
// [signatures] for one concrete reason: a dex string pool is a set of *unique* strings and a method
// id points into it by index, so a single in-place edit renames the definition and every call site
// at once -- including the string literal of a reflective lookup, which is the same entry. There is
// no second place that has to be kept in agreement.
//
// The prefix table cannot do this job, because it replaces with strstr: "callback" also occurs
// inside "callbackSnapshot". These names are therefore matched whole (memcmp on equal length),
// never as substrings, and the same words are resolved through the map by the native side:
//   native/src/jni/hook_bridge.cpp     the "callback" lookup and the JNI registration table
//   native/src/jni/resources_hook.cpp  translateResId / translateAttrId
//
// Only names that exist solely in this framework belong here. A module that hooks a *target* method
// by name keeps that name as a string literal in its own dex, which shares the pool entry with the
// framework's member of the same spelling -- renaming it would quietly turn the module's hook into a
// lookup of a method that no longer exists. That is why "callback" and "invokeOriginal" are absent:
// both are ordinary names in third-party code (okhttp-style callbacks, hand-written helpers), and a
// module hooking one of those would break with no error of ours to point at.
constexpr std::string_view member_names[] = {
    "findAndHookMethod",     "hookAllMethods",       "beforeHookedMethod", "afterHookedMethod",
    "invokeOriginalMethod",  "replaceCallback",      "unhookMethod",       "deoptimizeMethod",
    "callbackSnapshot",      "findStaticInitializer", "legacyApiPrefixes",
    "translateResId",        "translateAttrId",
};

// Filled only when the marker file is present. The decision is made by the Kotlin side, which owns
// the data directory constant, and arrives as an argument to getSignatures() -- duplicating that
// path here would be a second place to keep in sync.
std::map<std::string, std::string> member_name_map;

// Remembered so a later call with a different flag can say so instead of silently disagreeing with
// the table that was already handed out.
bool members_decided = false;
bool members_enabled = false;

jclass class_file_descriptor = nullptr;
jmethodID method_file_descriptor_ctor = nullptr;

jclass class_shared_memory = nullptr;
jmethodID method_shared_memory_ctor = nullptr;

}  // anonymous namespace

// Converts Dex signatures to Java format.
// Trailing slashes are translated to dots, which correctly aligns with
// Java's string matching expectations for package prefixes.
static std::string to_java(const std::string &signature) {
    std::string java(signature, 1);
    std::replace(java.begin(), java.end(), '/', '.');
    return java;
}

// A replacement for one simple name, character for character the same length so that the string can
// be overwritten in place. Two characters are structure rather than name and are kept: '$' separates
// an outer class from a nested one -- the runtime resolves the link through it -- and '/' separates
// package segments, which a class in a sub-package (de.robv.android.xposed.callbacks.*) still has.
static std::string random_name_like(std::string_view original) {
    static constexpr auto chrs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    thread_local static std::mt19937 rg{std::random_device{}()};
    thread_local static std::uniform_int_distribution<std::string::size_type> pick(
        0, strlen(chrs) - 1);

    std::string out;
    out.reserve(original.size());
    for (char c : original) out += (c == '$' || c == '/') ? c : chrs[pick(rg)];
    return out;
}

// Same-length replacement for a member name. Unlike a class simple name this one has no '$' and no
// '/' in it: a dex identifier is letters, digits, '_' and '$' only. '_' is kept because it is used
// by the platform's own synthetic names and dropping it changes how a name reads in a stack trace
// without changing what it hides.
static std::string random_member_like(std::string_view original) {
    static constexpr auto chrs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

    thread_local static std::mt19937 rg{std::random_device{}()};
    thread_local static std::uniform_int_distribution<std::string::size_type> pick(
        0, strlen(chrs) - 1);

    std::string out;
    out.reserve(original.size());
    for (char c : original) out += (c == '$' || c == '_') ? c : chrs[pick(rg)];
    return out;
}

static void ensureInitialized(JNIEnv *env, bool rename_members) {
    // Thread-safe one-time initialization
    std::call_once(init_flag, [&]() {
        LOGD("ObfuscationManager.init");
        members_decided = true;
        members_enabled = rename_members;

        if (auto file_descriptor = lsplant::JNI_FindClass(env, "java/io/FileDescriptor")) {
            class_file_descriptor =
                static_cast<jclass>(lsplant::JNI_NewGlobalRef(env, file_descriptor));
        } else
            return;

        method_file_descriptor_ctor =
            lsplant::JNI_GetMethodID(env, class_file_descriptor, "<init>", "(I)V");

        if (auto shared_memory = lsplant::JNI_FindClass(env, "android/os/SharedMemory")) {
            class_shared_memory =
                static_cast<jclass>(lsplant::JNI_NewGlobalRef(env, shared_memory));
        } else
            return;

        method_shared_memory_ctor = lsplant::JNI_GetMethodID(env, class_shared_memory, "<init>",
                                                             "(Ljava/io/FileDescriptor;)V");

        auto regen = [](std::string_view original_signature) {
            static constexpr auto chrs = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

            thread_local static std::mt19937 rg{std::random_device{}()};
            thread_local static std::uniform_int_distribution<std::string::size_type> pick(
                0, strlen(chrs) - 1);
            thread_local static std::uniform_int_distribution<std::string::size_type> choose_slash(
                0, 10);

            std::string out;
            size_t length = original_signature.size();
            out.reserve(length);
            out += "L";

            for (size_t i = 1; i < length - 1; i++) {
                if (choose_slash(rg) > 8 &&  // 20% chance for a slash
                    out.back() != '/' &&     // Avoid consecutive slashes
                    i != 1 &&                // No slash immediately after 'L'
                    i != length - 2) {       // No slash right before the end
                    out += "/";
                } else {
                    out += chrs[pick(rg)];
                }
            }

            // Respect the original termination character type to prevent
            if (original_signature.back() == '/') {
                out += "/";
            } else {
                out += chrs[pick(rg)];
            }

            if (out.length() != original_signature.length()) {
                LOGE("Length mismatch! Org: %zu vs New: %zu. '%s' -> '%s'",
                     original_signature.length(), out.length(),
                     std::string(original_signature).c_str(), out.c_str());
            }

            return out;
        };

        for (auto &i : signatures) {
            i.second = regen(i.first);
            LOGV("%s => %s", i.first.c_str(), i.second.c_str());
        }

        if (rename_members) {
            for (auto name : member_names) {
                std::string mapped = random_member_like(name);
                if (mapped.size() != name.size()) {
                    LOGE("Member name length mismatch for '%s'", std::string(name).c_str());
                    continue;
                }
                member_name_map.emplace(std::string(name), std::move(mapped));
            }
            LOGI("obfuscation: member renaming enabled for %zu name(s)", member_name_map.size());
        } else {
            LOGI("obfuscation: member renaming disabled (marker file absent)");
        }

        LOGD("ObfuscationManager init successfully");
    });
}

// Through the lsplant wrappers rather than raw JNI: each one clears a pending exception and logs
// the Java stack behind it, which is what a failed lookup here would otherwise cost. Returning a
// null jclass while leaving NoClassDefFoundError pending -- as the raw form did -- hands the next
// JNI call undefined behaviour. They also return scoped references, so the local refs this loop
// used to leak per entry are released on the spot.
static jobject stringMapToJavaHashMap(JNIEnv *env, const std::map<std::string, std::string> &map) {
    auto map_class = lsplant::JNI_FindClass(env, "java/util/HashMap");
    if (!map_class) return nullptr;

    auto init = lsplant::JNI_GetMethodID(env, map_class, "<init>", "()V");
    auto put = lsplant::JNI_GetMethodID(env, map_class, "put",
                                        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!init || !put) return nullptr;

    auto hash_map = lsplant::JNI_NewObject(env, map_class, init);
    if (!hash_map) return nullptr;

    for (const auto &[key, value] : map) {
        auto key_java = lsplant::JNI_NewStringUTF(env, key);
        auto value_java = lsplant::JNI_NewStringUTF(env, value);
        lsplant::JNI_CallObjectMethod(env, hash_map, put, key_java, value_java);
    }

    return lsplant::JNI_NewGlobalRef(env, hash_map);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_matrix_vector_daemon_utils_ObfuscationManager_getSignatures(
    JNIEnv *env, [[maybe_unused]] jclass clazz, jboolean rename_members) {
    ensureInitialized(env, rename_members == JNI_TRUE);
    if (members_decided && (rename_members == JNI_TRUE) != members_enabled) {
        // The table is built once per daemon start; a caller that disagrees with the first one is
        // the sign of a marker file that changed while the daemon was already running.
        LOGW("getSignatures: member renaming was already fixed to %s for this process",
             members_enabled ? "on" : "off");
    }

    static jobject signatures_jni = nullptr;
    static std::once_flag jni_map_flag;

    // Thread-safe, one-time JNI HashMap translation
    std::call_once(jni_map_flag, [&]() {
        std::map<std::string, std::string> signatures_java;
        for (const auto &i : signatures) {
            signatures_java[to_java(i.first)] = to_java(i.second);
        }
        // Member names are not descriptors: the first character is part of the name, so they go in
        // verbatim instead of through to_java(), which would strip it.
        for (const auto &i : member_name_map) signatures_java.emplace(i.first, i.second);
        signatures_jni = stringMapToJavaHashMap(env, signatures_java);
    });

    return signatures_jni;
}

static int obfuscateDexBuffer(const void *dex_data, size_t size) {
    dex::Reader reader{reinterpret_cast<const dex::u1 *>(dex_data), size};
    reader.CreateFullIr();
    auto ir = reader.GetIr();

    // The scopes that actually have a mapped prefix in this build; a scope whose prefix is absent
    // (an upstream table without the entry) simply does not participate.
    std::vector<std::pair<std::string, std::string>> scopes;
    for (auto scope : simple_name_scopes) {
        auto mapped = signatures.find(std::string(scope));
        if (mapped != signatures.end()) scopes.emplace_back(mapped->first, mapped->second);
    }

    // The same scopes in the dotted form the runtime uses when it builds a name by concatenation
    // ("de.robv.android.xposed." + simple). A descriptor rewrite cannot reach those, and one of them
    // is the fallback prefix the module class loader keeps for the case where the native bridge is
    // unavailable -- rewriting it makes that fallback agree with the map instead of disagreeing.
    std::vector<std::pair<std::string, std::string>> dotted;
    for (const auto &[scope, mapped] : scopes) {
        std::string from(scope.substr(1));
        std::string to(mapped.substr(1));
        std::replace(from.begin(), from.end(), '/', '.');
        std::replace(to.begin(), to.end(), '/', '.');
        dotted.emplace_back(std::move(from), std::move(to));
    }

    // Held across the whole string walk: two module dexes may be rewritten concurrently, and both
    // read the table that the first of them may still be extending.
    std::lock_guard<std::mutex> guard(class_names_mutex);
    const auto known_before = class_names.size();
    size_t renamed = 0;
    size_t members_renamed = 0;

    // Bounds of the shortest and longest entry in member_names; anything outside cannot match, so it
    // never reaches the comparison loop.
    constexpr size_t kMemberNameMin = 8;
    constexpr size_t kMemberNameMax = 24;

    LOGD("Mutating strings in-place");
    // Mutate strings in-place.
    for (auto &i : ir->strings) {
        const char *s = i->c_str();
        const std::string_view text(s);

        // Whole-descriptor rewrite, before the prefix pass: once the prefix has been replaced the
        // descriptor key below would no longer match, and a descriptor is the one string form whose
        // replacement cannot collide with anything else in the pool.
        size_t lead = 0;
        while (lead + 1 < text.size() && text[lead] == '[') ++lead;  // [Lfoo/Bar; for arrays
        const std::string_view bare = text.substr(lead);
        if (bare.size() > 2 && bare.front() == 'L' && bare.back() == ';') {
            std::string key(bare);
            auto known = class_names.find(key);
            if (known != class_names.end()) {
                memcpy(const_cast<char *>(s) + lead, known->second.data(), known->second.size());
                ++renamed;
            } else {
                for (const auto &[scope, mapped] : scopes) {
                    if (bare.size() <= scope.size() + 1 ||
                        bare.compare(0, scope.size(), scope) != 0)
                        continue;
                    const std::string_view simple =
                        bare.substr(scope.size(), bare.size() - scope.size() - 1);

                    std::string replaced = mapped + random_name_like(simple) + ";";
                    if (replaced.size() != bare.size()) {
                        LOGE("Class name length mismatch: '%s' -> '%s'", key.c_str(),
                             replaced.c_str());
                        break;
                    }
                    memcpy(const_cast<char *>(s) + lead, replaced.data(), replaced.size());
                    class_names.emplace(std::move(key), std::move(replaced));
                    ++renamed;
                    break;
                }
            }
        }

        for (const auto &signature : signatures) {
            char *p = const_cast<char *>(strstr(s, signature.first.c_str()));
            if (p) memcpy(p, signature.second.c_str(), signature.first.length());
        }

        for (const auto &[from, to] : dotted) {
            char *p = const_cast<char *>(strstr(s, from.c_str()));
            if (p) memcpy(p, to.c_str(), from.length());
        }

        // Member names last, and only by exact match. This pass cannot use strstr the way the two
        // above do: "callback" lives inside "callbackSnapshot", so a substring replace would rename
        // a method nobody asked about. Length is compared first, which makes the common case (a
        // string that is not a member name at all) one integer compare per candidate.
        if (!member_name_map.empty() && text.size() >= kMemberNameMin &&
            text.size() <= kMemberNameMax) {
            for (auto candidate : member_names) {
                if (candidate.size() != text.size() ||
                    memcmp(text.data(), candidate.data(), candidate.size()) != 0)
                    continue;
                auto mapped = member_name_map.find(std::string(candidate));
                if (mapped == member_name_map.end()) break;
                memcpy(const_cast<char *>(s), mapped->second.data(), mapped->second.size());
                ++members_renamed;
                break;
            }
        }
    }

    // Info rather than verbose on purpose: this is the only record that the class-name layer ran at
    // all, and the daemon's own log capture drops verbose lines. It reports counts, never names.
    LOGI("obfuscation: %zu descriptor(s) rewritten, %zu member name(s) rewritten, %zu known name(s) (%zu new)",
         renamed, members_renamed, class_names.size(), class_names.size() - known_before);

    dex::Writer writer(ir);
    size_t new_size;
    DexAllocator allocator;

    // CreateImage calls allocator.Allocate()
    auto *image = writer.CreateImage(&allocator, &new_size);
    LOGD("writer.CreateImage returned: %p", image);

    return allocator.GetFd();
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_matrix_vector_daemon_utils_ObfuscationManager_obfuscateDex(JNIEnv *env,
                                                                    [[maybe_unused]] jclass clazz,
                                                                    jobject memory,
                                                                    jboolean rename_members) {
    // Either entry point may be the first one the daemon reaches -- the preload dex is rewritten
    // before the bridge ever asks for the map -- so both carry the marker decision and whichever
    // arrives first fixes it for the process.
    ensureInitialized(env, rename_members == JNI_TRUE);

    int fd = ASharedMemory_dupFromJava(env, memory);
    if (fd < 0) return nullptr;

    auto size = ASharedMemory_getSize(fd);
    LOGV("obfuscateDex: fd=%d, size=%zu", fd, size);

    // CRITICAL: We MUST use MAP_SHARED here, not MAP_PRIVATE.
    // 1. Android's SharedMemory is backed by ashmem or memfd. Mapping these as
    //    MAP_PRIVATE creates a Copy-On-Write (COW) layer. In many Android kernel
    //    configurations, this COW layer does not correctly fault-in the initial
    //    contents from the shared source, resulting in the JNI side seeing
    //    unpopulated zero-pages. This causes slicer to fail immediately.
    // 2. Using MAP_SHARED ensures we have direct access to the same physical
    //    pages populated by the Java layer.
    // 3. ZERO-COPY MUTATION: Slicer's Intermediate Representation (IR) points
    //    directly into this mapped memory for string data. By mutating the
    //    buffer in-place, we update the IR's state without any additional
    //    heap allocations. This is safe here because the Daemon owns the
    //    lifecycle of this temporary buffer and the Java caller will discard
    //    the un-obfuscated original anyway.
    void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        LOGE("Failed to map input dex");
        close(fd);
        return nullptr;
    }

    bool needs_obfuscation = false;
    for (const auto &sig : signatures) {
        if (memmem(mem, size, sig.first.c_str(), sig.first.length()) != nullptr) {
            needs_obfuscation = true;
            break;
        }
    }
    if (!needs_obfuscation) {
        for (const auto &member : member_name_map) {
            if (memmem(mem, size, member.first.c_str(), member.first.length()) != nullptr) {
                needs_obfuscation = true;
                break;
            }
        }
    }

    if (!needs_obfuscation) {
        LOGV("No target signatures found in fd=%d, skipping slicer.", fd);
        munmap(mem, size);

        // Wrap the duplicated FD into Java objects and return instantly
        auto java_fd =
            lsplant::JNI_NewObject(env, class_file_descriptor, method_file_descriptor_ctor, fd);
        auto java_sm =
            lsplant::JNI_NewObject(env, class_shared_memory, method_shared_memory_ctor, java_fd);
        return java_sm.release();
    }

    // Process the DEX and obtain a new file descriptor for the output
    int new_fd = obfuscateDexBuffer(mem, size);

    // Safely unmap and close the input buffer mapping
    munmap(mem, size);
    close(fd);

    if (new_fd < 0) {
        LOGE("Obfuscation failed to create new dex buffer");
        return nullptr;
    }

    // Construct new SharedMemory object around the new_fd
    auto java_fd =
        lsplant::JNI_NewObject(env, class_file_descriptor, method_file_descriptor_ctor, new_fd);
    auto java_sm =
        lsplant::JNI_NewObject(env, class_shared_memory, method_shared_memory_ctor, java_fd);

    return java_sm.release();
}
