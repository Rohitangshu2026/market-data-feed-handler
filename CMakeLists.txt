# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Rohitangshu Bose

cmake_minimum_required(VERSION 3.25)
project(market_data_feed_handler VERSION 1.0.0 DESCRIPTION "A market data feed handler that recovers from loss" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)  # the bench's numbers need it
endif()

option(FEED_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FEED_COVERAGE "Build with gcov instrumentation, for the coverage target" OFF)
option(FEED_WERROR "Treat warnings as errors" OFF)

# Every component, as one library shared by the programs and the tests.
# Includes are written from src/, e.g. #include "handler/feed_handler.hpp".
add_library(market_data_feed
  src/book/order_book.cpp
  src/exchange/exchange.cpp
  src/network/lossy_link.cpp
  src/handler/feed_handler.cpp
  src/lamport/lamport.cpp
  src/cli/options.cpp
  src/stats/stats.cpp
  src/multi/multi_exchange.cpp
  src/multi/multi_feed_handler.cpp
  src/multi/multi_lossy_link.cpp)
target_include_directories(market_data_feed PUBLIC src)
target_compile_options(market_data_feed PUBLIC -Wall -Wextra -Wpedantic $<$<BOOL:${FEED_WERROR}>:-Werror>)
if(FEED_SANITIZE)
  target_compile_options(market_data_feed PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(market_data_feed PUBLIC -fsanitize=address,undefined)
endif()
if(FEED_COVERAGE)
  target_compile_options(market_data_feed PUBLIC --coverage -O0)
  target_link_options(market_data_feed PUBLIC --coverage)
endif()

# The demo: scenarios that show each recovery path; see docs/DEMO.md.
add_executable(feed_demo apps/demo.cpp)
target_link_libraries(feed_demo PRIVATE market_data_feed)

# The bench: speed, latency, lag, correctness, design choices, with confidence
# intervals over seeds; see docs/PERFORMANCE.md.
add_executable(feed_bench apps/bench.cpp)
target_link_libraries(feed_bench PRIVATE market_data_feed)

# GoogleTest, fetched at configure time and pinned by its hash. To build
# offline, point -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST at a local copy.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Unit, structural, functional and property tests; see docs/TESTING.md.
enable_testing()
include(GoogleTest)
add_executable(feed_tests
  tests/order_book_test.cpp
  tests/exchange_test.cpp
  tests/lossy_link_test.cpp
  tests/feed_handler_test.cpp
  tests/property_test.cpp
  tests/lamport_test.cpp
  tests/cli_test.cpp
  tests/stats_test.cpp
  tests/multi_instrument_test.cpp)
target_link_libraries(feed_tests PRIVATE market_data_feed GTest::gtest_main)
gtest_discover_tests(feed_tests PROPERTIES LABELS unit DISCOVERY_TIMEOUT 60)

# End-to-end tests: each runs a program and checks its exit status and output.
# expect(<test> <exit status> <regex the output matches> <regex it must not> <program> <args>...)
function(expect name status pattern absent program)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -DSTATUS=${status} "-DPATTERN=${pattern}" "-DABSENT=${absent}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect.cmake -- $<TARGET_FILE:${program}> ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS e2e)
endfunction()
# answer(<test> <answers file> <regex> <program> <args>...): the same, with the
# program reading its answers from tests/answers/<file>, and exiting 0.
function(answer name file pattern program)
  add_test(NAME ${name}
    COMMAND ${CMAKE_COMMAND} -DSTATUS=0 "-DPATTERN=${pattern}" "-DABSENT=DIFFERS|internal error"
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/answers/${file}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect.cmake -- $<TARGET_FILE:${program}> ${ARGN})
  set_tests_properties(${name} PROPERTIES LABELS e2e)
endfunction()
set(never "DIFFERS|internal error")

expect(e2e.demo.help 0 "usage: feed_demo" "${never}" feed_demo --help)
expect(e2e.demo.all 0 "Lamport time never does" "${never}" feed_demo all --quiet)
expect(e2e.demo.watch 0 "matches the exchange's own book" "${never}" feed_demo watch --events 20000 --rate 1000000)
expect(e2e.demo.burst 0 "seqs 101-150 still missing after 3 more arrivals: retransmitted" "${never}"
       feed_demo burst --gap 101-150 --events 300 --quiet)
expect(e2e.demo.reorder 0 "[(]0 retransmissions needed[)]" "${never}" feed_demo reorder --seed 9)
expect(e2e.demo.tail 0 "seqs 16-20 missing: retransmitted" "${never}" feed_demo tail --lost 5)
expect(e2e.demo.outage 0 "too old to retransmit: snapshot" "${never}" feed_demo outage)
expect(e2e.demo.random 0 "matches the exchange's own book" "${never}"
       feed_demo random --events 2e5 --loss 30% --seed 42 --recovery first --window 0)
expect(e2e.demo.lamport 0 "passes the 10 ms handoff" "${never}" feed_demo lamport --latency-ms 10 --skews-ms 0,10,20)
expect(e2e.demo.multi 0 "All 3 instruments reconstructed correctly with zero cross-contamination" "${never}"
       feed_demo multi --events 300 --loss 15% --seed 123)
expect(e2e.demo.bad_number 2 "--events: .abc. is not a whole number" "${never}" feed_demo random --events abc)
expect(e2e.demo.bad_chance 2 "--loss: .1.5. is not a chance from 0 to 1" "${never}" feed_demo random --loss 1.5)
expect(e2e.demo.out_of_range 2 "--window: 2000000 is out of range" "${never}" feed_demo random --window 2000000)
expect(e2e.demo.unknown_option 2 "unknown option --sed" "${never}" feed_demo random --sed 5)
expect(e2e.demo.missing_value 2 "--seed needs a value" "${never}" feed_demo random --seed)
expect(e2e.demo.given_twice 2 "--seed is given twice" "${never}" feed_demo random --seed 1 --seed=2)
expect(e2e.demo.flag_value 2 "--quiet takes no value" "${never}" feed_demo burst --quiet=yes)
expect(e2e.demo.unknown_scenario 2 "unknown scenario .nonsense." "${never}" feed_demo nonsense)
expect(e2e.demo.extra_word 2 "unexpected word .extra." "${never}" feed_demo random extra)
expect(e2e.demo.bad_range 2 "is not a range of seqs" "${never}" feed_demo burst --gap 15-11)
expect(e2e.demo.gap_past_end 2 "--gap must end before the last event" "${never}" feed_demo burst --gap 20-30)
expect(e2e.demo.tail_too_long 2 "--lost cannot be more than --events" "${never}" feed_demo tail --lost 30)
expect(e2e.demo.bad_choice 2 "is not one of: first, every" "${never}" feed_demo random --recovery all)
expect(e2e.demo.all_checks_first 2 "--gap must end before the last event" "== burst" feed_demo all --gap 9000-9001)
expect(e2e.demo.random_seed 0 "seed [0-9]+, window.*replay: feed_demo random --events 3000 --seed [0-9]+" "${never}"
       feed_demo random --events 3000)
expect(e2e.demo.show 0 "the first 4 and the last 4 of 1,000 events.*lost  *recovered by retransmission" "${never}"
       feed_demo random --events 1000 --seed 5 --show 4 --loss 50%)
expect(e2e.demo.show_all 0 "all 22 events, and what happened to each" "${never}" feed_demo burst --seed 3 --show 50)
expect(e2e.demo.bad_show 2 "--show: .x. is not a whole number" "${never}" feed_demo random --show x)
answer(e2e.ask.random random.txt "the random seed is [0-9]+.*replay: feed_demo random --events 20000 --seed [0-9]+ --loss 30%"
       feed_demo)
answer(e2e.ask.retry retry.txt
       "is not one of the scenarios.*is not a whole number.*--gap must end before the last event.*seqs 10-12 still missing"
       feed_demo)
answer(e2e.ask.quit quit.txt "How many events should the exchange publish" feed_demo)
answer(e2e.ask.lamport lamport.txt
       "Lamport time never does.*replay: feed_demo lamport --events 200 --seed 11 --latency-ms 10 --skews-ms 0,10,20,40"
       feed_demo lamport --ask)
answer(e2e.ask.defaults_from_command_line defaults.txt "How many events should the exchange publish[?] .5,000." feed_demo
       random --ask --events 5000)

expect(e2e.bench.help 0 "usage: feed_bench" "${never}" feed_bench --help)
expect(e2e.bench.correct 0 "10/10" "${never}" feed_bench correct --events 2e4 --losses 0,20%)
expect(e2e.bench.compare 0 "the same in every seed" "${never}"
       feed_bench compare --param window --a 0 --b 8 --events 2e4 --seeds 4 --loss 0 --reorder 0 --dup 0)
expect(e2e.bench.one_seed 2 "--seeds: 1 is out of range" "${never}" feed_bench speed --seeds 1)
expect(e2e.bench.compare_needs_param 2 "compare needs --param, --a and --b" "${never}" feed_bench compare)
expect(e2e.bench.param_elsewhere 2 "belong to the compare section" "${never}" feed_bench speed --param window)
expect(e2e.bench.unknown_section 2 "unknown section .fast." "${never}" feed_bench fast)
expect(e2e.bench.bad_csv 2 "--csv: cannot write" "${never}" feed_bench speed --csv /nonexistent/dir)

# API documentation from the Javadoc-style comments, into docs/api/html.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  add_custom_target(docs COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    COMMENT "Writing the API documentation to docs/api/html")
endif()

# Line and branch coverage of the library by the tests, into build/coverage.
find_program(GCOVR gcovr)
if(FEED_COVERAGE AND GCOVR)
  add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
    COMMAND ${GCOVR} --root ${CMAKE_CURRENT_SOURCE_DIR} --filter ${CMAKE_CURRENT_SOURCE_DIR}/src/
            --exclude-throw-branches --exclude-unreachable-branches --txt --html-details
            ${CMAKE_BINARY_DIR}/coverage/index.html ${CMAKE_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Coverage report: build/coverage/index.html")
endif()
