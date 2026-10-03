# skywalker_engine: the platform-independent core plus (on Apple) the Metal backend.

set(SKY_ENGINE_SOURCES
    src/core/Json.cpp
    src/core/Log.cpp
    src/core/Strings.cpp
    src/ecs/Reflection.cpp
    src/ecs/Components.cpp
    src/scene/Scene.cpp
    src/scene/History.cpp
    src/wander/Compiler.cpp
    src/wander/Runtime.cpp
    src/render/Png.cpp
    src/render/MeshData.cpp
    src/render/FrameBuilder.cpp
    src/render/NullRenderer.cpp
    src/render/Annotate.cpp
    src/render/Gltf.cpp
    src/render/Hdr.cpp
    src/fx/Particles.cpp
    src/fx/Ocean.cpp
    src/render/MeshFormats.cpp
    src/assets/Fetch.cpp
    src/render/TextureGen.cpp
    src/assets/AssetDatabase.cpp
    src/assets/Material.cpp
    src/assets/Prefab.cpp
    src/agent/ToolRegistry.cpp
    src/agent/EngineTools.cpp
    src/agent/AssetTools.cpp
    src/agent/WorldTools.cpp
    src/agent/NetworkTools.cpp
    src/agent/FxTools.cpp
    src/agent/McpServer.cpp
    src/agent/SocketServer.cpp
    src/engine/Engine.cpp
    src/engine/Gizmo.cpp
    # 2D, text, UI and dialogue
    src/third_party/StbImpl.cpp
    src/ecs/Components2D.cpp
    src/text/Font.cpp
    src/text/TextLayout.cpp
    src/render2d/ImageIO.cpp
    src/render2d/Atlas.cpp
    src/render2d/Tilemap.cpp
    src/render2d/Sprites.cpp
    src/render2d/Raster2D.cpp
    src/ui/UiStyle.cpp
    src/ui/UiLayout.cpp
    src/ui/UiSystem.cpp
    src/ui/Dialogue.cpp
    src/ui/World2D.cpp
    src/agent/Tools2D.cpp
    src/agent/UiTools.cpp
    src/agent/DialogueTools.cpp
)

add_library(skywalker_engine STATIC ${SKY_ENGINE_SOURCES})
add_library(sky::engine ALIAS skywalker_engine)

# stb (public domain): stb_truetype (SDF fonts), stb_image (PNG/JPEG decoding), stb_rect_pack (atlases).
include(FetchContent)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515)
FetchContent_MakeAvailable(stb)
target_include_directories(skywalker_engine SYSTEM PRIVATE ${stb_SOURCE_DIR})
# Built-in assets (fonts) are found here when running from a build tree; SKYWALKER_ASSETS overrides.
target_compile_definitions(skywalker_engine PRIVATE SKY_BUILTIN_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
    SKY_DEFAULT_FONT_FILE="${PROJECT_SOURCE_DIR}/assets/fonts/Inter.ttf")  # embedded fallback (#embed)

find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

target_include_directories(skywalker_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(skywalker_engine PUBLIC sky::options PRIVATE ZLIB::ZLIB CURL::libcurl Threads::Threads)
# Definitions are C-family only (Swift consumers of the C API must not see them).
target_compile_definitions(skywalker_engine PUBLIC
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:SKY_VERSION_STRING="${PROJECT_VERSION}">)

if(SKY_HAS_METAL)
    # Shaders are compiled at runtime from embedded source (enables live shader editing
    # by agents and does not require the offline Metal toolchain).
    set(_shader_src ${CMAKE_CURRENT_SOURCE_DIR}/platform/metal/shaders/Standard.metal)
    set(_shader_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/ShaderSource.inc)
    add_custom_command(
        OUTPUT ${_shader_inc}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_shader_src} -DOUTPUT=${_shader_inc}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        DEPENDS ${_shader_src} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        COMMENT "Embedding Metal shader source")

    # Additional shader libraries: Name.metal -> generated/NameSource.inc
    set(_extra_shader_incs)
    foreach(_name Sprite2D UI)
        set(_src ${CMAKE_CURRENT_SOURCE_DIR}/platform/metal/shaders/${_name}.metal)
        set(_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/${_name}Source.inc)
        add_custom_command(
            OUTPUT ${_inc}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${_src} -DOUTPUT=${_inc} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
            DEPENDS ${_src} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
            COMMENT "Embedding ${_name}.metal")
        list(APPEND _extra_shader_incs ${_inc})
    endforeach()

    target_sources(skywalker_engine PRIVATE platform/metal/MetalRenderer.mm platform/metal/MetalRenderer2D.mm
                   ${_shader_inc} ${_extra_shader_incs})
    target_include_directories(skywalker_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(skywalker_engine PUBLIC $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:SKY_HAS_METAL=1>)
    target_link_libraries(skywalker_engine PUBLIC
        "-framework Foundation" "-framework Metal" "-framework MetalKit" "-framework QuartzCore")
else()
    target_compile_definitions(skywalker_engine PUBLIC $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:SKY_HAS_METAL=0>)
endif()

# Stable C API (used by the Swift editor and future language bindings).
add_library(skywalker_capi STATIC capi/sky_api.cpp)
add_library(sky::capi ALIAS skywalker_capi)
target_include_directories(skywalker_capi PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/capi/include>)
target_link_libraries(skywalker_capi PUBLIC sky::engine)
