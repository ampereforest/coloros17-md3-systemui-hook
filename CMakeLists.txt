cmake_minimum_required(VERSION 3.28)
project(coloros17_md3_systemui_hook LANGUAGES C CXX ASM)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This native hook is deliberately Android ARM64-only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# LSPlant: ART inline hooking. The vendored tree is pinned to the upstream
# commit recorded in THIRD_PARTY_NOTICES.md and already includes the ColorOS 17
# fixes in patches/LSPlant-coloros17-gsi-fixes.patch.
set(LSPLANT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LSPLANT_STANDALONE OFF CACHE BOOL "" FORCE)
set(DEX_BUILDER_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/LSPlant/lsplant/src/main/jni" lsplant)

# Dobby (LSPosed fork): instruction relocation / trampolines. The vendored tree
# already includes patches/DobbyLS-arm64-absolute64-jumps.patch.
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
set(Plugin.Android.BionicLinkerUtil OFF CACHE BOOL "" FORCE)
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/DobbyLS" dobby)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT DEFINED MD3_DEX)
    set(MD3_DEX "${CMAKE_CURRENT_SOURCE_DIR}/build/dex/classes.dex")
endif()
if(NOT EXISTS "${MD3_DEX}")
    message(FATAL_ERROR
        "Missing ${MD3_DEX}. Run ./build.sh (it compiles the Java helper), "
        "or pass -DMD3_DEX=/absolute/path/to/classes.dex")
endif()

set(DEX_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_dex.hpp")
add_custom_command(
    OUTPUT "${DEX_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/native/embed_dex.py"
            "${MD3_DEX}" "${DEX_HEADER}"
    DEPENDS "${MD3_DEX}" "${CMAKE_CURRENT_SOURCE_DIR}/src/native/embed_dex.py"
    VERBATIM)

add_library(md3hook SHARED
    src/native/module.cpp
    src/native/art_elf.cpp
    "${DEX_HEADER}")

target_include_directories(md3hook PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native/third_party/zygisk"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(md3hook PRIVATE lsplant_static dobby log dl)

target_compile_options(md3hook PRIVATE
    -Wall -Wextra -Wno-missing-field-initializers
    -ffunction-sections -fdata-sections)

# Android 15+ devices may use 16 KiB memory pages. Every PT_LOAD segment must
# be aligned accordingly or the loader rejects the library.
target_link_options(md3hook PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384"
    "-Wl,-z,relro" "-Wl,-z,now" "-Wl,--gc-sections" "-Wl,--exclude-libs,ALL")

set_target_properties(md3hook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "arm64-v8a"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/zygisk")
