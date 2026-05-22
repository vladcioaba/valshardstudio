# Walshard Studio — axmol host app for the UILayoutEditor library.
# Binary target name `Walshard`; display name "Walshard Studio" carried in
# code strings (window titles, About dialog, logs).
#
# Layout:
#   app/                       — host app + editor-only library
#     app/UILayoutEditor/      — editor library (never shipped to games)
#   extensions/OCSExtension/   — runtime library (games link this to load .csb)
#
# Build:
#   cmake -S . -B build -G Xcode -DAX_ROOT=/Users/Vlad/Work/GameDev/axmol
#   cmake --build build --config Debug

cmake_minimum_required(VERSION 3.22...4.1)

# Cap the macOS deployment target before project() runs so the SDK's
# default doesn't get baked into the generated Xcode project. 13.0 is
# old enough to cover any current macOS install while staying within
# axmol's supported range.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0"
        CACHE STRING "Minimum macOS deployment target" FORCE)
endif()

set(APP_NAME Walshard)
project(${APP_NAME})

if(XCODE)
    set(CMAKE_XCODE_GENERATE_TOP_LEVEL_PROJECT_ONLY TRUE)
endif()

set(_is_axmol_embed FALSE)
set(_AX_USE_PREBUILT FALSE)

# AX_ROOT: where axmol lives. Falls back to the user's known layout.
if(NOT DEFINED AX_ROOT)
    set(AX_ROOT "/Users/Vlad/Work/GameDev/axmol" CACHE PATH "axmol engine root")
endif()
if(NOT EXISTS "${AX_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "axmol not found at ${AX_ROOT}. Set -DAX_ROOT=/path/to/axmol.")
endif()
set(ENV{AX_ROOT} ${AX_ROOT})

set(CMAKE_MODULE_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake
    ${AX_ROOT}/templates/common/cmake/modules
    ${AX_ROOT}/templates/cpp/cmake/modules
    ${AX_ROOT}/cmake/Modules
)

include(AXGameEngineOptions)
include(AXGameEngineSetup)

# --- Our libraries ----------------------------------------------------------

# Editor-only library — runs inside the OCS editor process only.
add_subdirectory(app/UILayoutEditor ${CMAKE_BINARY_DIR}/UILayoutEditor)

# Runtime extension — ALSO linked into the editor so the canvas
# preview uses the same node classes the game runtime will. Game
# projects link this same target via:
#   add_subdirectory(${OCS_ROOT}/extensions/OCSExtension OCSExtension)
add_subdirectory(extensions/OCSExtension ${CMAKE_BINARY_DIR}/OCSExtension)

# --- Host app ---------------------------------------------------------------
# Non-recursive globs: app/UILayoutEditor/ is its own target above and
# must not be re-compiled into the host-app binary.

file(GLOB GAME_HEADER app/*.h)
file(GLOB GAME_SOURCE app/*.cpp)
if(APPLE)
    file(GLOB GAME_MM app/*.mm)
    list(APPEND GAME_SOURCE ${GAME_MM})
endif()

set(GAME_INC_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/app")
set(content_folder "${CMAKE_CURRENT_SOURCE_DIR}/app/content")

if(APPLE)
    ax_mark_multi_resources(common_content_files RES_TO "Resources" FOLDERS ${content_folder})
elseif(WINDOWS)
    ax_mark_multi_resources(common_content_files RES_TO "Content" FOLDERS ${content_folder})
endif()

include(AXGameSourceSetup)

set(APP_SOURCES
    ${GAME_HEADER}
    ${GAME_SOURCE}
)

include(AXGameTargetSetup)

ax_setup_app_config(${APP_NAME})

# Link against our extension + axmol's ImGui extension (axmol builds the
# latter automatically because we pull in the engine setup above). Also
# link OpenGL for the tear-off DragLabel — it paints into its own
# secondary GLFW window using GL 2.1 fixed-function calls. axmol itself
# uses Metal on macOS, so GL isn't already pulled in by the engine.
find_package(OpenGL REQUIRED)
target_link_libraries(${APP_NAME} UILayoutEditor OCSExtension ImGui cocostudio OpenGL::GL)

# Per-platform link libs.
if(WIN32)
    # ws2_32: Winsock (InspectServer TCP listener).
    # shell32: native dialogs / process helpers (when we add them).
    target_link_libraries(${APP_NAME} ws2_32 shell32)
endif()

include(AXGamePlatformSetup)
include(AXGameFinalSetup)
