# Usage: cmake -DINPUT=<file> -DOUTPUT=<file> -P EmbedText.cmake
# Wraps a text file in a C++ raw string literal so it can be #included.
file(READ "${INPUT}" content)
file(WRITE "${OUTPUT}.tmp" "R\"SKYEMBED(${content})SKYEMBED\"\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
