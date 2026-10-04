cmake_minimum_required(VERSION 3.26)

# Swift compilation mode is a target property (wholemodule for the editor) — CMake >= 3.29.
if(POLICY CMP0157)
    cmake_policy(SET CMP0157 NEW)
endif()

# Must be set before project() to take effect.
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS version")

project(Skywalker
    VERSION 0.1.0
    DESCRIPTION "A game engine built from the ground up for AI agents"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SKY_BUILD_TESTS "Build unit tests" ON)
option(SKY_BUILD_CLI "Build the headless `skywalker` CLI / MCP server" ON)
option(SKY_BUILD_EDITOR "Build the SwiftUI editor (macOS, requires Ninja)" ${APPLE})
option(SKY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(SKY_SANITIZE "" CACHE STRING "Sanitizers to enable: address;undefined | thread")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    enable_language(OBJCXX)
    set(SKY_HAS_METAL ON)
else()
    set(SKY_HAS_METAL OFF)
endif()

if(SKY_BUILD_EDITOR)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        message(WARNING "The Swift editor requires the Ninja generator; disabling SKY_BUILD_EDITOR")
        set(SKY_BUILD_EDITOR OFF)
    else()
        enable_language(Swift)
    endif()
endif()

include(cmake/SkyCompilerOptions.cmake)

add_subdirectory(engine)

if(SKY_BUILD_CLI)
    add_subdirectory(tools/cli)
    add_subdirectory(tools/wander_bench)
    if(APPLE)
        add_subdirectory(tools/player)  # skywalker-player: the standalone game runtime
    endif()
endif()

if(SKY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SKY_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
