# Third-party physics and navigation libraries (workstream P), fetched at configure time.
#
#   * Jolt Physics (MIT)          — rigid bodies, colliders, character controller, joints, queries.
#   * Recast/Detour (zlib)        — navigation mesh generation, path finding and crowd steering.
#
# Both are built as static libraries with their samples/tests/tools disabled. Their include
# directories are SYSTEM so Skywalker's strict warnings (-Werror) never fire on their headers.

include(FetchContent)

# --- Jolt Physics -------------------------------------------------------------------------
# Cross-platform determinism keeps play sessions replayable (same inputs => same simulation),
# which Skywalker's agent tooling (sim_trace, replays, tests) relies on.
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)  # Skywalker derives from Jolt interfaces with RTTI on
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
# GPU compute back ends (hair/cloth research features) are not used.
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(USE_ASSERTS ON CACHE BOOL "" FORCE)  # catch API misuse in debug / sanitizer builds
else()
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
    SYSTEM)
FetchContent_MakeAvailable(JoltPhysics)
# Jolt exports -pthread as a public compile/link option. The Swift linker (editor) rejects it and
# consumers already link Threads::Threads, so keep it private to Jolt's own compilation.
foreach(_sky_prop INTERFACE_LINK_OPTIONS INTERFACE_COMPILE_OPTIONS)
    get_target_property(_sky_opts Jolt ${_sky_prop})
    if(_sky_opts)
        list(REMOVE_ITEM _sky_opts -pthread)
        set_target_properties(Jolt PROPERTIES ${_sky_prop} "${_sky_opts}")
    endif()
endforeach()

# --- Recast / Detour ------------------------------------------------------------------------
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(recastnavigation)

# Third-party code is built without Skywalker's sanitizer/warning flags (they are interface
# options of sky::options and never reach these targets), but must be position independent
# and C++17 at least.
foreach(_sky_dep Jolt Recast Detour DetourCrowd)
    if(TARGET ${_sky_dep})
        set_target_properties(${_sky_dep} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endforeach()

# Detour zeroes its (non-trivially copyable) tile array with memset; recastnavigation's own code, known safe.
if(TARGET Detour)
    target_compile_options(Detour PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wno-unknown-warning-option -Wno-nontrivial-memcall>)
endif()
