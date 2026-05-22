# UILayoutEditor — editor-only static library for Walshard Studio.
# Provides the in-engine ImGui editor for .csd/.csb scene files.
#
# Lives under app/ because only the editor binary ever links it; game
# projects use extensions/OCSExtension/ instead. Follows the axmol
# extension layout so it could still be dropped into axmol/extensions/
# if someone wanted to embed it.
#
#   * STATIC library named the extension's target_name
#   * Public headers under src/UILayoutEditor/
#   * Hooks into axmol's build via setup_ax_extension_config()

set(target_name UILayoutEditor)

file(GLOB_RECURSE UILAYOUTEDITOR_SOURCES
    src/*.h src/*.cpp src/*.hpp)

add_library(${target_name} STATIC ${UILAYOUTEDITOR_SOURCES})

target_include_directories(
    ${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Use axmol's extension setup if available (when this dir is under axmol/extensions).
if(COMMAND setup_ax_extension_config)
    setup_ax_extension_config(${target_name})
endif()

# Depends on axmol ImGui + cocostudio extensions. cocostudio provides
# FlatBuffersSerialize::serializeFlatBuffersWithXMLFile which powers the
# Export → .csb pipeline. axmol's top-level extension CMakeLists calls
# target_link_libraries on us with its plain signature, so we match the
# plain style here (no PUBLIC / PRIVATE keyword).
target_link_libraries(${target_name} ImGui cocostudio OCSExtension)

# tinyxml2 + fmt come with axmol core, no extra linkage needed.
