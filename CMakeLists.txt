cmake_minimum_required(VERSION 3.16)

project(plotjuggler_rtt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network Xml)

# The installed plotjugglerConfig.cmake lists fmt::fmt and lua::lua as link
# dependencies of plotjuggler_base without finding them, and lua is not
# installed at all. This plugin uses neither, so empty targets are enough
# when the real ones are missing.
find_package(fmt QUIET)
foreach(dep fmt::fmt lua::lua)
  if(NOT TARGET ${dep})
    add_library(${dep} INTERFACE IMPORTED)
  endif()
endforeach()
find_package(plotjuggler REQUIRED)

# Core logic, free of PlotJuggler so it can be unit tested on its own.
add_library(rtt_core STATIC
  src/rtt_data_map.cpp
  src/binary_parser.cpp
  src/csv_logger.cpp)
target_include_directories(rtt_core PUBLIC src)
target_link_libraries(rtt_core PUBLIC Qt5::Core)
set_target_properties(rtt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(DataStreamRTT SHARED src/datastream_rtt.cpp)
target_compile_definitions(DataStreamRTT PRIVATE QT_PLUGIN)
target_link_libraries(DataStreamRTT PRIVATE
  rtt_core
  plotjuggler::plotjuggler_base
  Qt5::Widgets Qt5::Network Qt5::Xml)

install(TARGETS DataStreamRTT LIBRARY DESTINATION bin)

option(BUILD_TESTING "Build the unit tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    FetchContent_MakeAvailable(googletest)
  endif()

  foreach(test rtt_data_map binary_parser csv_logger)
    add_executable(test_${test} tests/test_${test}.cpp)
    target_link_libraries(test_${test} PRIVATE rtt_core GTest::gtest_main)
    add_test(NAME ${test} COMMAND test_${test})
  endforeach()
endif()
