cmake_minimum_required(VERSION 3.24)
project(matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ME_SANITIZE "ASan + UBSan" OFF)

include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz)
FetchContent_MakeAvailable(googletest benchmark)

add_library(me src/engine.cpp src/order_pool.cpp src/id_map.cpp src/book_side.cpp)
target_include_directories(me PUBLIC include)
target_compile_options(me PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(ME_SANITIZE)
  target_compile_options(me PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(me PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()
add_executable(tests tests/test_smoke.cpp tests/test_types.cpp tests/test_order_pool.cpp
  tests/test_id_map.cpp tests/test_book_side.cpp
  tests/test_engine_basic.cpp tests/test_book.cpp
  tests/test_val.cpp tests/test_match.cpp
  tests/test_mkt.cpp tests/test_cxl.cpp
  tests/test_mod.cpp)
target_include_directories(tests PRIVATE tests)
target_link_libraries(tests PRIVATE me GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(tests)

# Randomized and differential tests: millions of commands with checks after each one. A separate
# executable so all of it, including the header-only engine and test support, is optimized even in
# the sanitized debug build (sanitizers and asserts stay on). Inline functions are shared across
# translation units, so optimizing only some files of one executable would not be reliable.
add_executable(tests_random tests/test_random.cpp tests/test_differential.cpp)
target_include_directories(tests_random PRIVATE tests)
target_compile_options(tests_random PRIVATE -O2)
target_link_libraries(tests_random PRIVATE me GTest::gtest_main)
gtest_discover_tests(tests_random DISCOVERY_TIMEOUT 30)

add_executable(bench bench/bench_smoke.cpp)
target_link_libraries(bench PRIVATE me benchmark::benchmark_main)
