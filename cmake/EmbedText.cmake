# Usage: cmake -DINPUT=<file> -DOUTPUT=<file> -P EmbedText.cmake
#    or: cmake -DDIR=<dir> -DFILES=a.metal,b.metal -DOUTPUT=<file> -P EmbedText.cmake
# Wraps a text file (or the concatenation of several, in order) in a C++ raw string
# literal so it can be #included.
if(DEFINED FILES)
    string(REPLACE "," ";" _files "${FILES}")
    set(content "")
    foreach(_f IN LISTS _files)
        file(READ "${DIR}/${_f}" _part)
        string(APPEND content "// ===== ${_f} =====\n${_part}\n")
    endforeach()
else()
    file(READ "${INPUT}" content)
endif()
file(WRITE "${OUTPUT}.tmp" "R\"SKYEMBED(${content})SKYEMBED\"\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
