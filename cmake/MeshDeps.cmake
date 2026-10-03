# meshoptimizer (MIT): mesh simplification for automatic LODs, vertex cache/fetch optimization.
include(FetchContent)
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v0.22
    GIT_SHALLOW TRUE)
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(meshoptimizer)
# Third-party: compiled without Skywalker's strict warnings.
get_target_property(_meshopt_inc meshoptimizer INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(meshoptimizer PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_meshopt_inc}")
