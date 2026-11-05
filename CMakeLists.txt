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
option(FH_WITH_BOOST "Build the boost::lockfree::spsc_queue baseline if Boost is found" ON)
option(FH_INJECT_ORDERING_BUG "Deliberate bug: publish the SPSC head with a relaxed store" OFF)
option(FH_ENABLE_PUBLISH "Build the ZeroMQ + Protobuf snapshot publisher (bench --publish)" OFF)

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
target_include_directories(fh_core SYSTEM PUBLIC third_party)  # rigtorp/SPSCQueue.h
if(FH_WITH_BOOST)
  find_package(Boost 1.70 QUIET CONFIG)
  if(Boost_FOUND)
    message(STATUS "Boost ${Boost_VERSION}: boost::lockfree::spsc_queue baseline enabled")
    target_link_libraries(fh_core PUBLIC Boost::headers)
    target_compile_definitions(fh_core PUBLIC FH_HAVE_BOOST)
  else()
    message(STATUS "Boost not found: boost queue baseline disabled")
  endif()
endif()
if(FH_INJECT_ORDERING_BUG)
  message(WARNING "FH_INJECT_ORDERING_BUG: SpscQueue publishes with a relaxed store (experiment build)")
  target_compile_definitions(fh_core PUBLIC FH_INJECT_ORDERING_BUG)
endif()
target_compile_options(fh_core PUBLIC ${FH_FLAGS})
target_compile_definitions(fh_core PRIVATE
  FH_CXX_FLAGS="${FH_FLAGS_STRING}"
  FH_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
if(FH_USE_CHRONO)
  target_compile_definitions(fh_core PUBLIC FH_USE_CHRONO)
endif()
target_link_libraries(fh_core PUBLIC Threads::Threads)

# Replaces global operator new/delete with counting versions (Version 2's
# zero-allocation proof). Linked only into tests and tools that check it.
add_library(fh_alloc_counter STATIC src/alloc_counter.cpp)
target_link_libraries(fh_alloc_counter PUBLIC fh_core)

add_executable(golden_hashes tools/golden_hashes.cpp)
target_link_libraries(golden_hashes PRIVATE fh_core)
add_executable(fifo_check tools/fifo_check.cpp)
target_link_libraries(fifo_check PRIVATE fh_alloc_counter)

# Version 1.1 snapshot publisher. Off by default, so the sanitizer jobs and
# the benchmarks build exactly as before and need neither library.
if(FH_ENABLE_PUBLISH)
  # Protobuf 22+ (Homebrew) needs its own CMake package, which also links
  # abseil; older releases (Ubuntu 24.04 ships 3.21) only have FindProtobuf.
  find_package(Protobuf CONFIG QUIET)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()
  find_package(cppzmq CONFIG REQUIRED)
  message(STATUS "Snapshot publisher: Protobuf ${Protobuf_VERSION}, cppzmq ${cppzmq_VERSION}")

  # protoc called directly: protobuf_generate() places its output differently
  # in protobuf's own package and in CMake's FindProtobuf.
  set(FH_PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/proto)
  file(MAKE_DIRECTORY ${FH_PROTO_OUT})
  add_custom_command(
    OUTPUT ${FH_PROTO_OUT}/book_snapshot.pb.cc ${FH_PROTO_OUT}/book_snapshot.pb.h
    COMMAND protobuf::protoc -I ${CMAKE_CURRENT_SOURCE_DIR}/proto --cpp_out=${FH_PROTO_OUT}
            ${CMAKE_CURRENT_SOURCE_DIR}/proto/book_snapshot.proto
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/book_snapshot.proto protobuf::protoc
    COMMENT "protoc book_snapshot.proto")
  add_library(fh_proto STATIC ${FH_PROTO_OUT}/book_snapshot.pb.cc)
  target_include_directories(fh_proto SYSTEM PUBLIC ${FH_PROTO_OUT})
  target_link_libraries(fh_proto PUBLIC protobuf::libprotobuf)

  add_library(fh_publish STATIC src/snapshot_publisher.cpp)
  target_link_libraries(fh_publish PUBLIC fh_core PRIVATE fh_proto cppzmq)
  target_compile_definitions(fh_publish PUBLIC FH_ENABLE_PUBLISH)
endif()

add_executable(feed_handler src/main.cpp)
target_link_libraries(feed_handler PRIVATE fh_core)
if(FH_ENABLE_PUBLISH)
  target_link_libraries(feed_handler PRIVATE fh_publish)
endif()

add_executable(micro_bench bench/micro_bench.cpp)
target_link_libraries(micro_bench PRIVATE fh_core)

add_executable(queue_stress bench/queue_stress.cpp)
target_link_libraries(queue_stress PRIVATE fh_core)

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
  foreach(t parser spsc_queue order_map book golden_compare histogram seqlock queue_stress fifo_book)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE fh_core GTest::gtest_main)
    gtest_discover_tests(test_${t} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endforeach()
  # The allocation proof replaces global new/delete, which the sanitizers'
  # own allocators also do, so it only runs in non-sanitizer builds.
  if(NOT FH_SANITIZE)
    add_executable(test_fifo_alloc tests/test_fifo_alloc.cpp)
    target_link_libraries(test_fifo_alloc PRIVATE fh_alloc_counter GTest::gtest_main)
    gtest_discover_tests(test_fifo_alloc DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endif()
  if(FH_ENABLE_PUBLISH)
    add_executable(test_snapshot_publisher tests/test_snapshot_publisher.cpp)
    target_link_libraries(test_snapshot_publisher PRIVATE fh_publish cppzmq GTest::gtest_main)
    gtest_discover_tests(test_snapshot_publisher DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)
  endif()
endif()
