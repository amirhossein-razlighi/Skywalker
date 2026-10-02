cmake_minimum_required(VERSION 3.26)

project(Skywalker
    VERSION 0.0.1
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
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
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
endif()

if(SKY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SKY_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
