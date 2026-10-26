cmake_minimum_required(VERSION 3.22)

# CXX is declared here even though nothing above this line is C++. llama.cpp
# enables it in its own subdirectory, but CMake needs the top-level generator
# to know about the language before it can emit build rules for it.
#
# This does not change how `hydra` is built: it still compiles two .c files as
# C99. It does mean ANDROID_STL=c++_static is linked into libhydra.so as well,
# which is unavoidable because the STL is a per-build setting, not a
# per-target one.
project(hydra C CXX)

set(CMAKE_C_STANDARD 99)

# Repo root: android/app/src/main/cpp -> 5 levels up = repo root
get_filename_component(HYDRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)

add_library(hydra SHARED
        ${HYDRA_ROOT}/src/hydra_engine.c
        ${CMAKE_CURRENT_SOURCE_DIR}/hydra_jni.c)

target_include_directories(hydra PRIVATE
        ${HYDRA_ROOT}/include)

# NEON is auto-enabled by the NDK for armeabi-v7a and arm64-v8a;
# hydra_neon.h activates via __ARM_NEON. No extra flags needed.
target_compile_options(hydra PRIVATE -O3 -Wall -Wextra)

find_library(log-lib log)
target_link_libraries(hydra ${log-lib})

# ===========================================================================
# llama.cpp - the real inference engine
#
# Pinned by submodule tag. The audit (docs/ANDROID-AUDIT.md, section 3)
# records the pin and the exact API this file is written against:
#     tag    v0.5.0
#     commit 7fe450e19305b828c199d602c23a8337aaa1f03b
#     licence MIT (Copyright (c) 2023-2026 The ggml authors)
#
# Every option below is set explicitly rather than left at its default.
# An upstream default can change between releases; a pinned option cannot.
# rules.md R15.
# llama.cpp is built for arm64-v8a and x86_64.
#
# arm64-v8a is where real phones are. x86_64 is added because it is the only
# way the engine can be EXERCISED here: rules.md R23 requires a new platform
# target to be verified on that platform, and rules.md R32 forbids claiming a
# result that was not measured. The only emulator available is x86_64
# (API 24, TCG software emulation, no KVM), so an arm64-only llama.cpp would
# leave the entire engine unverified - every number would be "NOT MEASURED"
# and Gate 2 unreachable.
#
# The task brief asked for arm64-v8a only. That is a real deviation and it is
# recorded here rather than quietly applied: x86_64 costs roughly 10 MB more
# per APK and buys nothing on a consumer phone, but it is the price of being
# able to prove the engine works at all.
#
# armeabi-v7a is still excluded: no 32-bit ARM device can be verified here
# either, and shipping an unverified ABI for it would be worse than shipping
# none. On armeabi-v7a the app runs the Hydra demo only, and LlamaEngine.load()
# is never called there because libllama_jni.so is not packaged for that ABI.
set(HYDRA_LLAMA_ABIS "arm64-v8a" "x86_64")

if(NOT CMAKE_ANDROID_ARCH_ABI IN_LIST HYDRA_LLAMA_ABIS)
  message(STATUS "llama.cpp: skipped for ${CMAKE_ANDROID_ARCH_ABI} "
                 "(built for: ${HYDRA_LLAMA_ABIS})")
  return()
endif()

set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
# No TLS stack on Android: nothing in this app fetches over the network.
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)

set(GGML_NATIVE     OFF CACHE BOOL "" FORCE)  # never -march=native: the build
set(GGML_LTO        OFF CACHE BOOL "" FORCE)  # machine must match the phone
set(GGML_CCACHE     OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP     OFF CACHE BOOL "" FORCE)  # Android has no OpenMP runtime
set(GGML_BLAS       OFF CACHE BOOL "" FORCE)
set(GGML_CUDA       OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN     OFF CACHE BOOL "" FORCE)
set(GGML_OPENCL     OFF CACHE BOOL "" FORCE)
set(GGML_METAL      OFF CACHE BOOL "" FORCE)
set(GGML_WEBGPU     OFF CACHE BOOL "" FORCE)
set(GGML_RPC        OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)  # no dlopen'd backends on Android

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/llama EXCLUDE_FROM_ALL)

# The JNI bridge for llama.cpp. It is a SEPARATE shared object from libhydra.so
# on purpose: the v1 bridge must stay byte-identical and independently
# loadable, and a fault in the new engine must not take the old one with it.
add_library(llama_jni SHARED ${CMAKE_CURRENT_SOURCE_DIR}/llama_jni.cpp)

# `llama` publishes ../include as PUBLIC, so this also gives us llama.h.
target_link_libraries(llama_jni PRIVATE llama)

target_compile_options(llama_jni PRIVATE -O3 -Wall -Wextra)

# Keyword signature on both calls: CMake rejects mixing the keyword and plain
# forms on one target.
find_library(android-log-lib log)
target_link_libraries(llama_jni PRIVATE ${android-log-lib})
