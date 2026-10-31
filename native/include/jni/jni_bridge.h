#pragma once

#include <deque>
#include <mutex>
#include <string>

#include "common/logging.h"
#include "common/obscure.h"
#include "core/config_bridge.h"
#include "core/context.h"

/**
 * @file jni_bridge.h
 * @brief Provides essential macros and helper functions for creating JNI bridges.
 *
 */

namespace sys::rt::jni {

/**
 * @brief Returns the number of elements in a statically-allocated C-style array.
 *
 * This is a compile-time constant.
 * Attempting to use this on a pointer will result in a compilation error,
 * preventing common mistakes.
 *
 * @tparam T The type of the array elements.
 * @tparam N The size of the array.
 * @param arr A reference to the array.
 * @return The number of elements in the array.
 */
template <typename T, size_t N>
[[nodiscard]] constexpr inline size_t ArraySize(T (&)[N]) {
    return N;
}

/**
 * @brief A helper function to get the obfuscated native bridge class signature prefix.
 *
 * It reads the obfuscation map to find the correct, potentially obfuscated,
 * package name for the native bridge classes.
 *
 * @return The JNI signature prefix (e.g., "org/matrix/vector/nativebridge/").
 */
inline std::string GetNativeBridgeSignature() {
    auto *bridge = SettingsSource::GetInstance();
    if (bridge) {
        // The key must match what the daemon places in the map.  Sealed so that the package
        // name is not readable from the artifact's read-only data, and hashed inside the table
        // so it is not resident as text either.
        if (const std::string *mapped = bridge->obfuscation_map().Find(SEALED(kNativeBridgeKey))) {
            return *mapped;
        }
    }
    // Fallback or default value if not found.
    return SEALED(kNativeBridgeSignature);
}

/**
 * @brief Resolves a member name (method or field) through the obfuscation map.
 *
 * The daemon renames the framework's own member names the same way it renames class names: it
 * rewrites the dex string holding the name before the dex is loaded, which renames the definition
 * and every call site in a single step, because a dex string pool is a set of unique strings and a
 * method id points into it by index. Nothing on the Java side needs a second lookup.
 *
 * Native code is the exception: a `GetMethodID(hooker, "callback", ...)` or a JNINativeMethod table
 * carries the text itself, so it has to ask the map for the same word. The map is the only place
 * where the two agree; when the map is absent (renaming disabled, or a build without obfuscation)
 * the input is returned unchanged.
 *
 * The returned pointer is interned for the life of the process, so it may be stored in a
 * JNINativeMethod entry that outlives this call.
 *
 * @param name The unobfuscated member name, as written in source.
 * @return The name to use at runtime.
 */
inline const char *ResolveMemberName(const char *name) {
    auto *bridge = SettingsSource::GetInstance();
    if (!bridge) return name;

    const std::string *mapped = bridge->obfuscation_map().Find(name);
    if (!mapped) return name;

    // Interned rather than returned as mapped->c_str(): the table is rebuilt when a new map arrives,
    // and a dangling pointer inside gMethods[] would be worse than a leaked string.
    static std::mutex intern_mutex;
    static std::deque<std::string> interned;

    std::lock_guard<std::mutex> guard(intern_mutex);
    interned.emplace_back(*mapped);
    return interned.back().c_str();
}

/**
 * @brief Internal implementation for registering native methods.
 *
 * Finds the target class using the framework's class loader and calls JNI's RegisterNatives.
 */
[[gnu::always_inline]]
inline bool RegisterNativeMethodsInternal(JNIEnv *env, std::string_view class_name,
                                          const JNINativeMethod *methods, jint method_count) {
    auto *context = Context::GetInstance();
    if (!context) {
        LOGF("Cannot register natives for '{}', Context is null.", class_name.data());
        return false;
    }
    auto clazz = context->FindClassFromCurrentLoader(env, class_name);
    if (clazz.get() == nullptr) {
        LOGF("JNI class not found: {}", class_name.data());
        return false;
    }
    // Wrapped: a failed registration throws NoSuchMethodError, and returning false while that
    // exception is still pending would hand the next JNI call undefined behaviour.
    return lsplant::JNI_RegisterNatives(env, clazz, methods, method_count) == JNI_OK;
}

// A helper cast for the native method function pointers.
#define VECTOR_JNI_CAST(to) reinterpret_cast<to>

/**
 * @def VECTOR_NATIVE_METHOD(className, functionName, signature)
 * @brief Defines a JNINativeMethod entry.
 *
 * This macro constructs a JNINativeMethod struct, automatically
 * creating the mangled C-style function name that JNI expects.
 *
 * @param className The simple name of the Java class (e.g., "HookBridge").
 * @param functionName The name of the Java method (e.g., "hookMethod").
 * @param signature The JNI signature of the method (e.g., "(I)V").
 */
#define VECTOR_NATIVE_METHOD(className, functionName, signature)                                   \
    {#functionName, signature,                                                                     \
     VECTOR_JNI_CAST(void *)(Java_org_matrix_vector_nativebridge_##className##_##functionName)}

/**
 * @def JNI_START
 * @brief Defines the standard first two arguments for any JNI native method implementation.
 */
#define JNI_START [[maybe_unused]] JNIEnv *env, [[maybe_unused]] jclass clazz

/**
 * @def VECTOR_DEF_NATIVE_METHOD(ret, className, functionName, ...)
 * @brief Defines the function signature for a JNI native method implementation.
 *
 * This macro creates the full C++ function definition with
 * the correct JNI name-mangling convention.
 */
#define VECTOR_DEF_NATIVE_METHOD(ret, className, functionName, ...)                                \
    extern "C" JNIEXPORT ret JNICALL                                                               \
        Java_org_matrix_vector_nativebridge_##className##_##functionName(JNI_START, ##__VA_ARGS__)

/**
 * @def REGISTER_VECTOR_NATIVE_METHODS(class_name)
 * @brief Registers all methods defined in the `gMethods` array for a given class.
 *
 * This is the final step in linking the C++ implementations to the Java native methods.
 */
#define REGISTER_VECTOR_NATIVE_METHODS(class_name)                                                 \
    RegisterNativeMethodsInternal(env, GetNativeBridgeSignature() + #class_name, gMethods,         \
                                  ArraySize(gMethods))

}  // namespace sys::rt::jni
