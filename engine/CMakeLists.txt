cmake_minimum_required(VERSION 3.24)
project(lob LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_NATIVE "Tune for the build machine (-march=native or -mcpu=native)" ON)
option(LOB_LTO "Link-time optimisation" ON)
set(LOB_PGO "OFF" CACHE STRING "Profile-guided optimisation stage: OFF, GENERATE or USE")
set_property(CACHE LOB_PGO PROPERTY STRINGS OFF GENERATE USE)
set(LOB_PGO_DIR "${CMAKE_SOURCE_DIR}/pgo-data" CACHE PATH "Where PGO profiles are written and read")

# ---------------------------------------------------------------- dependencies
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_BUILD_TESTING OFF)
set(ABSL_ENABLE_INSTALL OFF)
FetchContent_Declare(abseil
  URL https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL SYSTEM)

set(INSTALL_GTEST OFF)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL SYSTEM)

set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_ENABLE_WERROR OFF)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL SYSTEM)

# Header-only; skip its own CMakeLists (it predates CMake 4's minimum).
FetchContent_Declare(spscqueue
  URL https://github.com/rigtorp/SPSCQueue/archive/refs/tags/v1.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR do-not-add)

FetchContent_MakeAvailable(abseil googletest benchmark spscqueue)

add_library(rigtorp_spscqueue INTERFACE)
target_include_directories(rigtorp_spscqueue SYSTEM INTERFACE "${spscqueue_SOURCE_DIR}/include")

# ---------------------------------------------------------------- flags
include(CheckCXXCompilerFlag)

# Optimisation and warnings for everything we build.
add_library(lob_flags INTERFACE)
target_compile_options(lob_flags INTERFACE -Wall -Wextra -Wpedantic $<$<CONFIG:Release,RelWithDebInfo>:-O3>)
if(LOB_NATIVE)
  check_cxx_compiler_flag(-march=native LOB_HAS_MARCH_NATIVE)
  check_cxx_compiler_flag(-mcpu=native LOB_HAS_MCPU_NATIVE)
  if(LOB_HAS_MARCH_NATIVE)
    target_compile_options(lob_flags INTERFACE -march=native)
  elseif(LOB_HAS_MCPU_NATIVE)
    target_compile_options(lob_flags INTERFACE -mcpu=native)
  endif()
endif()
if(LOB_LTO)
  target_compile_options(lob_flags INTERFACE $<$<CONFIG:Release,RelWithDebInfo>:-flto>)
  target_link_options(lob_flags INTERFACE $<$<CONFIG:Release,RelWithDebInfo>:-flto>)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(LOB_PGO STREQUAL "GENERATE")
    target_compile_options(lob_flags INTERFACE -fprofile-instr-generate)
    target_link_options(lob_flags INTERFACE -fprofile-instr-generate)
  elseif(LOB_PGO STREQUAL "USE")
    target_compile_options(lob_flags INTERFACE "-fprofile-instr-use=${LOB_PGO_DIR}/lob.profdata"
                                               -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
  endif()
else()
  if(LOB_PGO STREQUAL "GENERATE")
    target_compile_options(lob_flags INTERFACE "-fprofile-generate=${LOB_PGO_DIR}")
    target_link_options(lob_flags INTERFACE "-fprofile-generate=${LOB_PGO_DIR}")
  elseif(LOB_PGO STREQUAL "USE")
    target_compile_options(lob_flags INTERFACE "-fprofile-use=${LOB_PGO_DIR}" -fprofile-partial-training
                                               -Wno-missing-profile)
  endif()
endif()

# Hot-path rules: no exceptions, no RTTI. Applied to the core library, the
# benchmarks and the latency harness (not to the GoogleTest binary).
add_library(lob_hot_path INTERFACE)
target_compile_options(lob_hot_path INTERFACE -fno-exceptions -fno-rtti)

# ---------------------------------------------------------------- targets
add_library(lob_core STATIC src/types.cpp src/cycle_clock.cpp)
target_include_directories(lob_core PUBLIC include)
target_link_libraries(lob_core
  PUBLIC absl::flat_hash_map absl::hash rigtorp_spscqueue
  PRIVATE lob_flags lob_hot_path)

add_executable(latency_harness tools/latency_harness.cpp)
target_include_directories(latency_harness PRIVATE tools)
target_link_libraries(latency_harness PRIVATE lob_core lob_flags lob_hot_path)
find_package(Threads REQUIRED)
target_link_libraries(latency_harness PRIVATE Threads::Threads)

add_executable(lob_bench bench/book_bench.cpp)
target_include_directories(lob_bench PRIVATE tools)
target_link_libraries(lob_bench PRIVATE lob_core benchmark::benchmark lob_flags lob_hot_path)

enable_testing()
include(GoogleTest)
add_executable(lob_tests
  tests/book_test.cpp tests/bitmap_test.cpp tests/differential_test.cpp tests/competition_test.cpp)
target_include_directories(lob_tests PRIVATE tests "${CMAKE_SOURCE_DIR}/../solutions/cpp")
target_link_libraries(lob_tests PRIVATE lob_core GTest::gtest_main lob_flags)
gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)

# ---------------------------------------------------------------- PGO training
# With -DLOB_PGO=GENERATE, `cmake --build . --target pgo-train` runs the
# instrumented harness and benchmarks and merges the profile for the USE stage.
if(LOB_PGO STREQUAL "GENERATE")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(LLVM_PROFDATA NAMES llvm-profdata)
    if(NOT LLVM_PROFDATA AND APPLE)
      execute_process(COMMAND xcrun -f llvm-profdata OUTPUT_VARIABLE LLVM_PROFDATA OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(NOT LLVM_PROFDATA)
      message(FATAL_ERROR "llvm-profdata not found; it is needed to merge clang PGO profiles")
    endif()
    set(merge_step COMMAND "${LLVM_PROFDATA}" merge -o "${LOB_PGO_DIR}/lob.profdata" "${LOB_PGO_DIR}/raw")
    set(profile_env "LLVM_PROFILE_FILE=${LOB_PGO_DIR}/raw/lob-%p.profraw")
  else()
    set(merge_step "")
    set(profile_env "")
  endif()
  add_custom_target(pgo-train
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${LOB_PGO_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LOB_PGO_DIR}/raw"
    COMMAND "${CMAKE_COMMAND}" -E env ${profile_env} $<TARGET_FILE:latency_harness> --count 1000000 --mode both
    COMMAND "${CMAKE_COMMAND}" -E env ${profile_env} $<TARGET_FILE:lob_bench> --benchmark_min_time=0.05s
    ${merge_step}
    DEPENDS latency_harness lob_bench
    COMMENT "Training PGO profile into ${LOB_PGO_DIR}"
    VERBATIM)
endif()
