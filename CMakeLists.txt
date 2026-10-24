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

add_library(me src/engine.cpp)
target_include_directories(me PUBLIC include)
target_compile_options(me PRIVATE -Wall -Wextra -Wpedantic -Werror)
if(ME_SANITIZE)
  target_compile_options(me PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(me PUBLIC -fsanitize=address,undefined)
endif()

enable_testing()
add_executable(tests tests/test_smoke.cpp tests/test_types.cpp)
target_include_directories(tests PRIVATE tests)
target_link_libraries(tests PRIVATE me GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(tests)

add_executable(bench bench/bench_smoke.cpp)
target_link_libraries(bench PRIVATE me benchmark::benchmark_main)
