include(FetchContent)

# doctest: tiny, fast-compiling, widely used single-header test framework.
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
# doctest 2.4.11 declares an ancient cmake_minimum_required; allow it under CMake 4.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(doctest)

set(SKY_TEST_SOURCES
    main.cpp
    test_json.cpp
    test_math.cpp
    test_ecs.cpp
    test_scene.cpp
    test_history.cpp
    test_wander_compiler.cpp
    test_wander_runtime.cpp
    test_render.cpp
    test_tools.cpp
    test_mcp.cpp
    test_assets.cpp
    test_texturegen.cpp
    test_import.cpp
    test_fx.cpp
    test_wander_examples.cpp
    test_wander2.cpp
    test_world.cpp
    test_audio.cpp
    test_input.cpp
    test_capi_input.cpp
    test_dcc.cpp
    test_studio.cpp
    test_physics.cpp
    test_nav.cpp
)

add_executable(skywalker_tests ${SKY_TEST_SOURCES})
target_link_libraries(skywalker_tests PRIVATE sky::engine sky::capi doctest::doctest)
target_compile_definitions(skywalker_tests PRIVATE
    SKY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

add_test(NAME skywalker_tests COMMAND skywalker_tests)
