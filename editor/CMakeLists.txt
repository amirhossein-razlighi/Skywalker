# Skywalker Editor — SwiftUI app on top of the C API.
#
# Why Swift? Native macOS UI (SwiftUI/AppKit), first-class Metal layers, Keychain,
# URLSession async networking for agent providers, and structured concurrency. The
# engine stays in C++ behind a small C ABI (engine/capi), which Swift imports directly.

file(GLOB_RECURSE SKY_EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Sources/*.swift)

set(SKY_ICON ${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.icns)
set(SKY_TEMPLATE_DIR ${PROJECT_SOURCE_DIR}/examples/hello_sky)

add_executable(SkywalkerEditor MACOSX_BUNDLE ${SKY_EDITOR_SOURCES})
set_target_properties(SkywalkerEditor PROPERTIES
    OUTPUT_NAME "Skywalker"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME "Skywalker"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.skywalker.editor"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
    Swift_LANGUAGE_VERSION 6
    # Whole-module builds avoid flaky incremental state with bundle outputs and optimize better.
    Swift_COMPILATION_MODE wholemodule
    Swift_MODULE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modules)

target_compile_options(SkywalkerEditor PRIVATE
    $<$<COMPILE_LANGUAGE:Swift>:-parse-as-library>
    $<$<AND:$<COMPILE_LANGUAGE:Swift>,$<CONFIG:Debug>>:-Onone>)
target_link_libraries(SkywalkerEditor PRIVATE sky::capi
    "-framework AppKit" "-framework SwiftUI" "-framework Security" "-framework GameController")

if(EXISTS ${SKY_ICON})
    target_sources(SkywalkerEditor PRIVATE ${SKY_ICON})
    set_source_files_properties(${SKY_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

# swiftc compiles and links in one step and needs the bundle directory to exist up front.
add_custom_command(TARGET SkywalkerEditor PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/bin/Skywalker.app/Contents/MacOS)

# Bundle the headless CLI (so "connect Claude Code" works out of the box) and the starter project.
add_dependencies(SkywalkerEditor skywalker)
# The standalone player rides along so the editor's game_build / "Build game" can package apps anywhere.
if(TARGET skywalker-player)
    add_dependencies(SkywalkerEditor skywalker-player)
    add_custom_command(TARGET SkywalkerEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:skywalker-player> $<TARGET_BUNDLE_CONTENT_DIR:SkywalkerEditor>/MacOS/skywalker-player
        COMMENT "Bundling the player into Skywalker.app")
endif()
add_custom_command(TARGET SkywalkerEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:skywalker> $<TARGET_BUNDLE_CONTENT_DIR:SkywalkerEditor>/MacOS/skywalker-cli
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:SkywalkerEditor>/Resources/Templates
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SKY_TEMPLATE_DIR} $<TARGET_BUNDLE_CONTENT_DIR:SkywalkerEditor>/Resources/Templates/hello_sky
    COMMENT "Bundling CLI and templates into Skywalker.app")
