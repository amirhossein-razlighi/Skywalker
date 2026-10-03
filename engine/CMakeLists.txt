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
    src/fx/Groom.cpp
    src/fx/GpuParticles.cpp
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
    src/agent/HairTools.cpp
    src/agent/McpServer.cpp
    src/agent/SocketServer.cpp
    src/engine/Engine.cpp
    src/engine/Gizmo.cpp
)

add_library(skywalker_engine STATIC ${SKY_ENGINE_SOURCES})
add_library(sky::engine ALIAS skywalker_engine)

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
    # The standard shader library is several modules concatenated in dependency order.
    set(_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/platform/metal/shaders)
    set(_shader_modules Common.metal Sky.metal Surface.metal Lighting.metal Effects.metal Post.metal)
    list(TRANSFORM _shader_modules PREPEND ${_shader_dir}/ OUTPUT_VARIABLE _shader_src)
    string(REPLACE ";" "," _shader_list "${_shader_modules}")
    set(_shader_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/ShaderSource.inc)
    add_custom_command(
        OUTPUT ${_shader_inc}
        COMMAND ${CMAKE_COMMAND} -DDIR=${_shader_dir} -DFILES=${_shader_list} -DOUTPUT=${_shader_inc}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        DEPENDS ${_shader_src} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        COMMENT "Embedding Metal shader source")

    target_sources(skywalker_engine PRIVATE platform/metal/MetalRenderer.mm ${_shader_inc})
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
