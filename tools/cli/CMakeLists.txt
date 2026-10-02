add_executable(skywalker main.cpp)
target_link_libraries(skywalker PRIVATE sky::engine)
set_target_properties(skywalker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
