cmake_minimum_required(VERSION 3.22)

project(plotjuggler_rtt VERSION 0.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PJRTT_BUILD_PLUGIN "Build the PlotJuggler plugin (needs plotjuggler_sdk)" ON)
option(BUILD_TESTING "Build the unit tests" ON)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time
endif()

# Header-only dependencies: use an installed copy when there is one,
# otherwise download the pinned version.
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# Core logic: data map, binary parser, CSV logger. No PlotJuggler or Qt,
# so it is unit tested on its own.
add_library(rtt_core STATIC
  src/rtt_data_map.cpp
  src/binary_parser.cpp
  src/csv_logger.cpp)
target_include_directories(rtt_core PUBLIC src)
target_link_libraries(rtt_core PUBLIC nlohmann_json::nlohmann_json)
set_target_properties(rtt_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PJRTT_BUILD_PLUGIN)
  find_package(plotjuggler_sdk REQUIRED COMPONENTS plugin_sdk)

  find_package(asio CONFIG QUIET)
  if(NOT TARGET asio::asio)
    FetchContent_Declare(asio
      URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-30-2.tar.gz
      URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8)
    FetchContent_MakeAvailable(asio)
    add_library(asio_standalone INTERFACE)
    target_include_directories(asio_standalone SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
    find_package(Threads REQUIRED)
    target_link_libraries(asio_standalone INTERFACE Threads::Threads)
    if(WIN32)
      target_link_libraries(asio_standalone INTERFACE ws2_32 mswsock)
      target_compile_definitions(asio_standalone INTERFACE _WIN32_WINNT=0x0A00)
    endif()
    add_library(asio::asio ALIAS asio_standalone)
  endif()

  add_library(segger_rtt_streamer SHARED src/rtt_source.cpp src/rtt_connection.cpp)
  target_link_libraries(segger_rtt_streamer PRIVATE
    rtt_core plotjuggler_sdk::plugin_sdk asio::asio)
  pj_configure_plugin(segger_rtt_streamer
    FAMILIES        data_source dialog
    MANIFEST_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    MANIFEST_HEADER generated/rtt_manifest.hpp
    MANIFEST_VAR    kRttManifest)
  pj_embed_file(segger_rtt_streamer
    FILE     src/rtt_dialog.ui
    HEADER   generated/rtt_dialog_ui.hpp
    VAR_NAME kRttDialogUi)

  install(TARGETS segger_rtt_streamer LIBRARY DESTINATION . RUNTIME DESTINATION .)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/segger_rtt_streamer.pjmanifest.json DESTINATION .)
endif()

if(BUILD_TESTING)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
    FetchContent_MakeAvailable(googletest)
  endif()

  foreach(test rtt_data_map binary_parser csv_logger run_timeline)
    add_executable(test_${test} tests/test_${test}.cpp)
    target_link_libraries(test_${test} PRIVATE rtt_core GTest::gtest_main)
    add_test(NAME ${test} COMMAND test_${test})
  endforeach()

  # Loads the built plugin through the SDK host loader and streams from a
  # fake J-Link server, without PlotJuggler.
  if(PJRTT_BUILD_PLUGIN)
    find_package(plotjuggler_sdk REQUIRED COMPONENTS plugin_host)
    add_executable(test_plugin_e2e tests/test_plugin_e2e.cpp)
    target_link_libraries(test_plugin_e2e PRIVATE
      plotjuggler_sdk::plugin_host asio::asio nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_definitions(test_plugin_e2e PRIVATE
      PJRTT_PLUGIN_PATH="$<TARGET_FILE:segger_rtt_streamer>"
      PJRTT_BURST_MAP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/examples/burst_map.json")
    add_dependencies(test_plugin_e2e segger_rtt_streamer)
    add_test(NAME plugin_e2e COMMAND test_plugin_e2e)
  endif()
endif()
