cmake_minimum_required(VERSION 3.22)
project(hydra C)

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
