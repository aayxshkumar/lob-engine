cmake_minimum_required(VERSION 3.24)
project(lob_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCH "Build benchmarks" ON)
# e.g. -DLOB_SANITIZERS=address,undefined (CI on Linux) or =undefined.
# AddressSanitizer currently hangs at startup on macOS 26 with Apple clang 17,
# so run ASan on Linux.
set(LOB_SANITIZERS "" CACHE STRING "Comma-separated -fsanitize= list")

if(LOB_SANITIZERS)
  add_compile_options(-fsanitize=${LOB_SANITIZERS} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${LOB_SANITIZERS})
endif()

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)
target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(FetchContent)

if(LOB_BUILD_TESTS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests tests/order_book_test.cpp)
  target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
  include(GoogleTest)
  # PRE_TEST: sanitizer builds start slowly and can trip the build-time discovery timeout.
  gtest_discover_tests(lob_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
endif()

if(LOB_BUILD_BENCH)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_bench bench/order_book_bench.cpp)
  target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark)

  add_executable(lob_latency bench/latency.cpp)
  target_link_libraries(lob_latency PRIVATE lob)
endif()
