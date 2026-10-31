cmake_minimum_required(VERSION 3.10)

project(native)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(${VECTOR_ROOT}/external external)

file(GLOB_RECURSE NATIVE_SOURCES "src/*.cpp")
add_library(${PROJECT_NAME} STATIC ${NATIVE_SOURCES})

set(IGNORED_WARNINGS
    -Wno-c99-extensions
    -Wno-extra-semi
    -Wno-gnu-flexible-array-initializer
    -Wno-gnu-string-literal-operator-template
    -Wno-gnu-zero-variadic-macro-arguments
    -Wno-variadic-macros
    -Wno-zero-length-array
)

# --- Include Directories ---
# Add the 'include' directory as a PUBLIC include path. This means any
# target that links against vector_native will also inherit this include path.
target_include_directories(${PROJECT_NAME} PUBLIC include)

# Version macros come from CMake cache variables (see the root build script). Declaring them
# here, rather than as compiler flags, lets CMake escape the quotes that make the version
# name a C string.
target_compile_definitions(${PROJECT_NAME} PUBLIC
    VERSION_CODE=${VERSION_CODE}
    VERSION_NAME="${VERSION_NAME}"
)

# Add the 'src' directory and external XZ library includes as PRIVATE.
# These are only needed to build vector_native itself.
target_include_directories(${PROJECT_NAME} PRIVATE src ${VECTOR_ROOT}/external/xz-embedded/linux/include)

# --- Compiler Options ---
# Apply the standard pedantic warnings and the ignored warnings list.
target_compile_options(${PROJECT_NAME} PRIVATE -Wpedantic ${IGNORED_WARNINGS})

# --- Symbol and path hygiene ---
# Everything here is compiled into the one shared object the app process loads, so a default
# visibility would export the C++ mangled names of every internal helper (which spell out the
# project namespace) and of the statically linked inline-hook and ART-hook libraries. Hidden by
# default leaves only the Zygisk entry points, which are declared with explicit default visibility.
target_compile_options(${PROJECT_NAME} PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-ident
)
# Source paths would otherwise be embedded verbatim by diagnostics and assertion machinery, and
# both the checkout path and the external dependency paths are identity strings.
target_compile_options(${PROJECT_NAME} PRIVATE
    "-ffile-prefix-map=${VECTOR_ROOT}=/src"
    "-fmacro-prefix-map=${VECTOR_ROOT}=/src"
)

# --- No thread-local state in the filter path ---
# sanitizer.cpp deliberately keeps no thread_local at all. Emulated TLS (what this API level
# compiles to) allocates the control block through malloc on first touch, and the filter runs
# inside open/openat -- the allocator's own slow path. Tombstones from com.google.android.gms
# showed the chain SyscallImpl -> __emutls_get_address -> malloc -> je_malloc ->
# je_tsd_init_finish -> fault on a thread that had never used our TLS. The guards now live in a
# fixed table keyed by the thread pointer; -ftls-model cannot be used because the NDK stubs do not
# export __tls_get_addr here, so a global-dynamic access would not even link.

# --- Library Linking ---
# Link against required external and system libraries.
# PUBLIC libraries are propagated to targets that link against this one.
# PRIVATE libraries are only used for the compilation of this target.
target_link_libraries(${PROJECT_NAME} PUBLIC
    dobby_static
    lsplant_static
    xz_static
    log # Android logging library
    fmt-header-only
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    dex_builder_static
)
