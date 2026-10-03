add_executable(wander_bench main.cpp)
target_link_libraries(wander_bench PRIVATE sky::engine)
set_target_properties(wander_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
