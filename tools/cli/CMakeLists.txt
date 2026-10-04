add_executable(skywalker main.cpp StudioCommand.cpp SetupCommand.cpp BuildCommand.cpp MovieCommand.cpp ServeCommand.cpp)
target_link_libraries(skywalker PRIVATE sky::engine)
set_target_properties(skywalker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# `skywalker build` bundles the player: build it first so the packager can find it next to this binary.
if(TARGET skywalker-player)
    add_dependencies(skywalker skywalker-player)
endif()
