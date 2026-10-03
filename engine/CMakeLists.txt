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
    src/world/Terrain.cpp
    src/world/Foliage.cpp
    src/world/WorldRuntime.cpp
    src/render/Vegetation.cpp
    src/render/ColorGrading.cpp
    src/render/LightClusters.cpp
    src/render/MeshLod.cpp
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
    src/agent/WorldBuildTools.cpp
    src/agent/DccTools.cpp
    src/dcc/Process.cpp
    src/dcc/Apps.cpp
    src/dcc/Manager.cpp
    src/dcc/Embedded.cpp
    src/agent/McpServer.cpp
    src/agent/SocketServer.cpp
    src/engine/Engine.cpp
    src/engine/Gizmo.cpp
    # audio + input (workstream S)
    src/audio/AudioComponents.cpp
    src/audio/Wav.cpp
    src/audio/Synth.cpp
    src/audio/SynthJson.cpp
    src/audio/SfxPresets.cpp
    src/audio/MusicGen.cpp
    src/audio/AudioSystem.cpp
    src/audio/Miniaudio.cpp
    src/input/ActionMap.cpp
    src/agent/AudioTools.cpp
    src/agent/InputTools.cpp
    # Studio (multi-agent roster, board, feedback, loops, playtests, runner)
    src/studio/Catalog.cpp
    src/studio/Studio.cpp
    src/studio/Playtest.cpp
    src/studio/Llm.cpp
    src/studio/Runner.cpp
    src/agent/StudioTools.cpp
    # Physics (Jolt) + navigation (Recast/Detour)
    src/ecs/PhysicsComponents.cpp
    src/physics/JoltCommon.cpp
    src/physics/Shapes.cpp
    src/physics/PhysicsWorld.cpp
    src/physics/PhysicsSystem.cpp
    src/nav/NavMesh.cpp
    src/nav/NavSystem.cpp
    src/physics/DebugDraw.cpp
    src/agent/PhysicsTools.cpp
    src/agent/NavTools.cpp
    # Animation (skinning, animator state machines, sequencer)
    src/anim/Animation.cpp
    src/anim/Controller.cpp
    src/anim/Sequence.cpp
    src/anim/AnimationSystem.cpp
    src/ecs/AnimationComponents.cpp
    src/agent/AnimationTools.cpp
    src/agent/SequenceTools.cpp
)

# DCC bridge: the Python helper library and the Blender add-on are embedded so the engine can
# run and install them without the source tree.
file(GLOB_RECURSE _dcc_py CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/integrations/*.py)
set(_dcc_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/DccEmbedded.inc)
add_custom_command(
    OUTPUT ${_dcc_inc}
    COMMAND ${CMAKE_COMMAND}
            "-DSPECS=lib=${PROJECT_SOURCE_DIR}/integrations/dcc|addon=${PROJECT_SOURCE_DIR}/integrations/blender"
            -DOUTPUT=${_dcc_inc} -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    DEPENDS ${_dcc_py} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    COMMENT "Embedding the DCC helper library and Blender add-on"
    VERBATIM)
set_source_files_properties(src/dcc/Embedded.cpp PROPERTIES
    OBJECT_DEPENDS ${_dcc_inc} COMPILE_OPTIONS -Wno-overlength-strings)

add_library(skywalker_engine STATIC ${SKY_ENGINE_SOURCES})
add_library(sky::engine ALIAS skywalker_engine)

# miniaudio (public domain / MIT-0): single-header audio engine (mixing, spatialization, decoding).
include(FetchContent)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.22
    GIT_SHALLOW TRUE)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # we only want the sources, not miniaudio's own CMake project
endif()
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)
endif()

find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

target_include_directories(skywalker_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_include_directories(skywalker_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(skywalker_engine PUBLIC sky::options PRIVATE ZLIB::ZLIB CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS})
# Third-party header: included as a system path and compiled without our warning set.
target_include_directories(skywalker_engine SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
# (miniaudio does unsigned pointer-offset arithmetic that UBSan flags; ASan still checks it.)
set_source_files_properties(src/audio/Miniaudio.cpp PROPERTIES COMPILE_OPTIONS "-w;-fno-sanitize=undefined")

# Physics (Jolt) and navigation (Recast/Detour), see cmake/PhysicsDeps.cmake.
include(${PROJECT_SOURCE_DIR}/cmake/PhysicsDeps.cmake)
target_link_libraries(skywalker_engine PRIVATE Jolt Recast Detour DetourCrowd)

# meshoptimizer: automatic LODs, see cmake/MeshDeps.cmake.
include(${PROJECT_SOURCE_DIR}/cmake/MeshDeps.cmake)
target_link_libraries(skywalker_engine PRIVATE meshoptimizer)
# Definitions are C-family only (Swift consumers of the C API must not see them).
target_compile_definitions(skywalker_engine PUBLIC
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:SKY_VERSION_STRING="${PROJECT_VERSION}">)

if(SKY_HAS_METAL)
    # Shaders are compiled at runtime from embedded source (enables live shader editing
    # by agents and does not require the offline Metal toolchain).
    # The standard shader library is several modules concatenated in dependency order.
    set(_shader_dir ${CMAKE_CURRENT_SOURCE_DIR}/platform/metal/shaders)
    set(_shader_modules Common.metal Clouds.metal Sky.metal Surface.metal Terrain.metal Lighting.metal Effects.metal Post.metal)
    list(TRANSFORM _shader_modules PREPEND ${_shader_dir}/ OUTPUT_VARIABLE _shader_src)
    string(REPLACE ";" "," _shader_list "${_shader_modules}")
    set(_shader_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/ShaderSource.inc)
    add_custom_command(
        OUTPUT ${_shader_inc}
        COMMAND ${CMAKE_COMMAND} -DDIR=${_shader_dir} -DFILES=${_shader_list} -DOUTPUT=${_shader_inc}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        DEPENDS ${_shader_src} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        COMMENT "Embedding Metal shader source")

    # Animation: GPU skinning compute kernel (separate source, embedded the same way).
    set(_skin_src ${CMAKE_CURRENT_SOURCE_DIR}/platform/metal/shaders/Skinning.metal)
    set(_skin_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/SkinningShaderSource.inc)
    add_custom_command(
        OUTPUT ${_skin_inc}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_skin_src} -DOUTPUT=${_skin_inc}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        DEPENDS ${_skin_src} ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        COMMENT "Embedding Metal skinning shader source")

    target_sources(skywalker_engine PRIVATE platform/metal/MetalRenderer.mm ${_shader_inc} ${_skin_inc})
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
