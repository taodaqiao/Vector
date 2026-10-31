cmake_minimum_required(VERSION 3.10)
project(daemon)

set(CMAKE_CXX_STANDARD 23)

add_subdirectory(${VECTOR_ROOT}/external external)

set(SOURCES
        dex2oat.cpp
        logcat.cpp
        obfuscation.cpp
        )

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Everything the Java side looks up is a JNI export, and JNIEXPORT already pins those to default
# visibility.  Hiding the rest keeps C++ mangled names -- which spell out the project's namespace
# and class names -- out of the dynamic symbol table of a library that ships inside the APK.
target_compile_options(${PROJECT_NAME} PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden -fno-ident)
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--exclude-libs,ALL)

# Randomised per build by build.gradle.kts; see the comment there. Quoted here so the macro
# reaches the compiler as a string literal rather than a bare token.
target_compile_definitions(${PROJECT_NAME} PRIVATE DEV_SOCK_NAME="${DEV_SOCK_NAME}")

target_link_libraries(${PROJECT_NAME} PRIVATE lsplant_static dex_builder_static android log)

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
