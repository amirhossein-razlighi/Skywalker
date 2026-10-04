# skywalker-player: the standalone game runtime (Cocoa window + CAMetalLayer, no SwiftUI).
# `skywalker build` copies it into Name.app/Contents/MacOS; it also runs a project folder directly.
add_executable(skywalker-player main.cpp PlayerCore.cpp Player.mm)
target_link_libraries(skywalker-player PRIVATE sky::engine
    "-framework AppKit" "-framework GameController" "-framework QuartzCore" "-framework CoreGraphics")
target_compile_options(skywalker-player PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
set_target_properties(skywalker-player PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
