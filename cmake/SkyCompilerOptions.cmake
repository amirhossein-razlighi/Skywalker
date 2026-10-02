# Shared compiler settings for all Skywalker C/C++/ObjC++ targets.

add_library(sky_options INTERFACE)
add_library(sky::options ALIAS sky_options)

target_compile_options(sky_options INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang>:-Wall -Wextra -fobjc-arc -Wno-unused-parameter>
)

if(SKY_WARNINGS_AS_ERRORS)
    target_compile_options(sky_options INTERFACE $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Werror>)
endif()

if(SKY_SANITIZE)
    string(REPLACE ";" "," _sky_san "${SKY_SANITIZE}")
    message(STATUS "Skywalker: sanitizers enabled: ${_sky_san}")
    target_compile_options(sky_options INTERFACE -fsanitize=${_sky_san} -fno-omit-frame-pointer -g)
    target_link_options(sky_options INTERFACE -fsanitize=${_sky_san})
endif()
