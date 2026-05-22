# Standalone test runner for the CSD model — doesn't depend on axmol runtime,
# only on pugixml + fmt + doctest (header-only). Runs in a plain CMake build.
#
# Build:
#   cmake -S app/UILayoutEditor/tests -B build.tests
#   cmake --build build.tests
#   ./build.tests/uilayouteditor_tests

cmake_minimum_required(VERSION 3.22)
project(uilayouteditor_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AXMOL_ROOT "/Users/Vlad/Work/GameDev/axmol"
    CACHE PATH "Path to axmol root (for bundled pugixml/fmt/doctest)")

# --- pugixml (vendored from axmol/3rdparty) ---------------------------------

add_library(opencs_pugixml STATIC
    ${AXMOL_ROOT}/3rdparty/pugixml/pugixml.cpp)
target_include_directories(opencs_pugixml PUBLIC
    ${AXMOL_ROOT}/3rdparty/pugixml)

# --- fmt (header-only in modern versions, but axmol ships compiled) ---------

find_path(FMT_INCLUDE_DIR fmt/format.h
    PATHS ${AXMOL_ROOT}/3rdparty/fmt/include
    NO_DEFAULT_PATH)
if(NOT FMT_INCLUDE_DIR)
    message(FATAL_ERROR "Couldn't find fmt at ${AXMOL_ROOT}/3rdparty/fmt/include")
endif()

# We use fmt header-only to keep the test build simple.
add_library(opencs_fmt INTERFACE)
target_include_directories(opencs_fmt INTERFACE ${FMT_INCLUDE_DIR})
target_compile_definitions(opencs_fmt INTERFACE FMT_HEADER_ONLY=1)

# --- CSD model library under test --------------------------------------------

add_library(uilayouteditor_csdmodel STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/UILayoutEditor/CsdModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/UILayoutEditor/Timeline.cpp)
target_include_directories(uilayouteditor_csdmodel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(uilayouteditor_csdmodel PUBLIC
    opencs_pugixml
    opencs_fmt)

# --- test executable ---------------------------------------------------------

# axmol ships doctest as split header + cpp. Compile doctest_main.cpp (our
# thin wrapper) to get the implementation and main().
add_executable(${PROJECT_NAME}
    test_csd_roundtrip.cpp
    test_editor_logic.cpp
    test_timeline.cpp
    doctest_main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE
    ${AXMOL_ROOT}/3rdparty/doctest)
target_link_libraries(${PROJECT_NAME} PRIVATE
    uilayouteditor_csdmodel)
