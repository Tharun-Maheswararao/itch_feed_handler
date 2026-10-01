cmake_minimum_required(VERSION 3.20)
project(itch_feed_handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FH_NATIVE "Tune for the build machine (-march/-mcpu=native)" ON)
option(FH_USE_CHRONO "Use std::chrono::steady_clock instead of the CPU counter" OFF)
option(FH_BUILD_TESTS "Build unit tests" ON)
set(FH_SANITIZE "" CACHE STRING "Sanitizers, e.g. address,undefined or thread")

find_package(Threads REQUIRED)

set(FH_FLAGS -Wall -Wextra -Wpedantic -Wno-gnu-anonymous-struct)
if(FH_NATIVE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    list(APPEND FH_FLAGS -mcpu=native)
  else()
    list(APPEND FH_FLAGS -march=native)
  endif()
endif()
if(FH_SANITIZE)
  list(APPEND FH_FLAGS -fsanitize=${FH_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${FH_SANITIZE})
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(REMOVE_ITEM FH_FLAGS -Wno-gnu-anonymous-struct)
endif()

# Recorded in every benchmark CSV so results say how they were built.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
string(REPLACE ";" " " _flags_str "${FH_FLAGS}")
set(FH_FLAGS_STRING "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} ${_flags_str}")
string(STRIP "${FH_FLAGS_STRING}" FH_FLAGS_STRING)

add_library(fh_core
  src/report.cpp
  src/sysinfo.cpp
  src/live_view.cpp)
target_include_directories(fh_core PUBLIC include)
target_compile_options(fh_core PUBLIC ${FH_FLAGS})
target_compile_definitions(fh_core PRIVATE
  FH_CXX_FLAGS="${FH_FLAGS_STRING}"
  FH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
if(FH_USE_CHRONO)
  target_compile_definitions(fh_core PUBLIC FH_USE_CHRONO)
endif()
target_link_libraries(fh_core PUBLIC Threads::Threads)

add_executable(feed_handler src/main.cpp)
target_link_libraries(feed_handler PRIVATE fh_core)

add_executable(micro_bench bench/micro_bench.cpp)
target_link_libraries(micro_bench PRIVATE fh_core)

if(FH_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  include(GoogleTest)
  foreach(t parser spsc_queue order_map book golden_compare histogram seqlock)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE fh_core GTest::gtest_main)
    gtest_discover_tests(test_${t} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endforeach()
endif()
