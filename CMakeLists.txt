cmake_minimum_required(VERSION 3.20)
project(itchbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ITCHBOOK_WERROR   "Treat warnings as errors" ON)
option(ITCHBOOK_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(ITCHBOOK_NATIVE   "Build with -march=native" OFF)
option(ITCHBOOK_BENCH    "Build benchmarks" ON)

add_library(itchbook INTERFACE)
target_include_directories(itchbook INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(itchbook INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(ITCHBOOK_WERROR)
  target_compile_options(itchbook INTERFACE -Werror)
endif()
if(ITCHBOOK_SANITIZE)
  target_compile_options(itchbook INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(itchbook INTERFACE -fsanitize=address,undefined)
endif()
if(ITCHBOOK_NATIVE)
  target_compile_options(itchbook INTERFACE -march=native)
endif()

# Tools
add_executable(replay tools/replay.cpp)
target_link_libraries(replay PRIVATE itchbook)

add_executable(match_replay tools/match_replay.cpp)
target_link_libraries(match_replay PRIVATE itchbook)

# Unit tests
add_executable(unit_tests
  tests/main.cpp
  tests/test_itch.cpp
  tests/test_hash_map.cpp
  tests/test_levels.cpp
  tests/test_book.cpp
  tests/test_matching.cpp
  tests/test_feed.cpp)
target_compile_definitions(unit_tests PRIVATE ITCHBOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(unit_tests SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(unit_tests PRIVATE itchbook)

# Benchmarks
if(ITCHBOOK_BENCH)
  add_executable(bench_itch bench/bench_itch.cpp)
  target_link_libraries(bench_itch PRIVATE itchbook)
  add_executable(bench_matching bench/bench_matching.cpp)
  target_link_libraries(bench_matching PRIVATE itchbook)
  # The benchmarks replace operator new/delete with malloc/free to count
  # allocations; GCC's -Wmismatched-new-delete misreads that as a mismatch.
  add_executable(bench_memlat bench/bench_memlat.cpp)
  target_link_libraries(bench_memlat PRIVATE itchbook)
  foreach(t bench_itch bench_matching bench_memlat)
    target_compile_options(${t} PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
  endforeach()
endif()

# Tests: unit tests plus the differential tests against the Python references.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
add_test(NAME unit_tests COMMAND unit_tests)
if(Python3_FOUND)
  set(DIFF_DIR ${CMAKE_CURRENT_BINARY_DIR}/difftest)
  add_test(NAME diff_matching
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/reference/run_match_diff.py
            --match-replay $<TARGET_FILE:match_replay> --events 200000 --seed 7 --workdir ${DIFF_DIR})
  add_test(NAME diff_matching_thin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/reference/run_match_diff.py
            --match-replay $<TARGET_FILE:match_replay> --events 200000 --seed 8 --profile thin
            --workdir ${DIFF_DIR}/thin)
  add_test(NAME diff_itch_synthetic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/reference/run_itch_diff.py
            --replay $<TARGET_FILE:replay> --input ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic.itch
            --symbols ALL --checkpoint-every 20000 --workdir ${DIFF_DIR})
  add_test(NAME replay_synthetic_invariants
    COMMAND replay --audit-every 50000 --symbols SYNA,SYNB ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/synthetic.itch)
endif()
