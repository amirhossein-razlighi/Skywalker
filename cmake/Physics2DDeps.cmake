# Third-party 2D physics library, fetched at configure time.
#
#   * Box2D v3 (MIT) — 2D rigid bodies, shapes, chains, joints, sensors, queries and the capsule
#     mover used by character2d.
#
# Built as a static library with its samples, benchmarks and tests disabled (they are only offered
# when Box2D is the top-level project). Its include directory is SYSTEM so Skywalker's strict
# warnings never fire on its headers. Box2D compiles its own sources with -ffp-contract=off, which
# keeps the simulation deterministic across machines.

include(FetchContent)

set(BOX2D_DISABLE_SIMD OFF CACHE BOOL "" FORCE)
set(BOX2D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
set(BOX2D_AVX2 OFF CACHE BOOL "" FORCE)  # SSE2 / NEON only: the same results on every x86-64 and arm64 machine

FetchContent_Declare(box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG v3.1.1
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(box2d)

if(TARGET box2d)
    set_target_properties(box2d PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
