cmake_minimum_required(VERSION 3.10)

project(zygisk)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${VECTOR_ROOT}/native native)

add_library(${PROJECT_NAME} SHARED module.cpp ipc_bridge.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC include)

# Identity macros arrive as CMake cache variables from the build script, so CMake adds the
# escaped quotes that turn them into C strings.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    INJECTED_PACKAGE_NAME="${INJECTED_PACKAGE_NAME}"
    INJECTED_PACKAGE_UID=${INJECTED_PACKAGE_UID}
    MANAGER_PACKAGE_NAME="${MANAGER_PACKAGE_NAME}"
    BRIDGE_SERVICE_NAME="${BRIDGE_SERVICE_NAME}"
    BRIDGE_SERVICE_ALT_NAME="${BRIDGE_SERVICE_ALT_NAME}"
)

target_link_libraries(${PROJECT_NAME} native log)

# --- Symbol and path hygiene ---
# The module is the only place these names could be read from: the process it is loaded into is
# the one that would do the reading. Hide everything except the declared Zygisk entry points, and
# internalise the statically linked archives so the inline-hook and ART-hook symbols stop being
# dynamic exports of the module.
target_compile_options(${PROJECT_NAME} PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-ident
    "-ffile-prefix-map=${VECTOR_ROOT}=/src"
    "-fmacro-prefix-map=${VECTOR_ROOT}=/src"
)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL)

if (DEFINED DEBUG_SYMBOLS_PATH)
    message(STATUS "Debug symbols will be placed at ${DEBUG_SYMBOLS_PATH}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}
            COMMAND ${CMAKE_OBJCOPY} --only-keep-debug $<TARGET_FILE:${PROJECT_NAME}>
            ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}/${PROJECT_NAME}.debug
            COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:${PROJECT_NAME}>
            COMMAND ${CMAKE_OBJCOPY} --add-gnu-debuglink ${DEBUG_SYMBOLS_PATH}/${ANDROID_ABI}/${PROJECT_NAME}.debug
            $<TARGET_FILE:${PROJECT_NAME}>)
endif()
