################################################################################
# OCSExtension — runtime library shipped to game projects.
#
# Provides the runtime-side counterpart of every "new" node type the OCS
# editor can author beyond the stock axmol/cocos studio surface
# (VectorMapNode, LottieNode, ...). Game projects link this lib and call
# `ocs::ext::registerAll()` once at startup so CSLoader can instantiate
# those types when it loads a .csb.
#
# The OCS editor process ALSO links this lib so the canvas preview
# renders the same way the runtime would — single source of truth for
# the node implementations.
#
# Game-project integration:
#   add_subdirectory(${OCS_ROOT}/extensions/OCSExtension OCSExtension)
#   target_link_libraries(${APP_NAME} PRIVATE OCSExtension)
#   // in AppDelegate::applicationDidFinishLaunching():
#   #include "OCSExtension/OCSExtension.h"
#   ocs::ext::registerAll();
################################################################################

message(STATUS "OCSExtension: configuring")
set(target_name OCSExtension)

file(GLOB_RECURSE OCSEXT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
message(STATUS "OCSExtension sources: ${OCSEXT_SOURCES}")

add_library(${target_name} STATIC ${OCSEXT_SOURCES})
message(STATUS "OCSExtension target created")

target_include_directories(${target_name}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Depend on axmol core + cocostudio. The exact target names depend on
# the host project — try both common spellings; downstream can override
# OCSEXT_AXMOL_TARGET via -D if neither matches.
if(NOT OCSEXT_AXMOL_TARGET)
    if(TARGET axmol)
        set(OCSEXT_AXMOL_TARGET axmol)
    elseif(TARGET axmol::axmol)
        set(OCSEXT_AXMOL_TARGET axmol::axmol)
    endif()
endif()

if(OCSEXT_AXMOL_TARGET)
    target_link_libraries(${target_name} PUBLIC ${OCSEXT_AXMOL_TARGET})
endif()

if(TARGET cocostudio)
    target_link_libraries(${target_name} PUBLIC cocostudio)
endif()

set_target_properties(${target_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
