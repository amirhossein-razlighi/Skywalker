# Usage: cmake -DSPECS="prefix=dir|prefix=dir" -DOUTPUT=<file> -P EmbedFiles.cmake
# Embeds every *.py file below each directory as {"prefix/relative/path", R"(content)"}, ready
# to be #included inside an array initializer. Used to ship the DCC helper library and the
# Blender add-on inside the engine binary.
string(REPLACE "|" ";" _specs "${SPECS}")
set(_out "")
foreach(_spec IN LISTS _specs)
    string(FIND "${_spec}" "=" _eq)
    string(SUBSTRING "${_spec}" 0 ${_eq} _prefix)
    math(EXPR _start "${_eq} + 1")
    string(SUBSTRING "${_spec}" ${_start} -1 _dir)
    file(GLOB_RECURSE _files RELATIVE "${_dir}" "${_dir}/*.py")
    list(SORT _files)
    foreach(_f IN LISTS _files)
        if(_f MATCHES "__pycache__")
            continue()
        endif()
        file(READ "${_dir}/${_f}" _content)
        string(APPEND _out "    {\"${_prefix}/${_f}\", R\"SKYEMBED(${_content})SKYEMBED\"},\n")
    endforeach()
endforeach()
file(WRITE "${OUTPUT}.tmp" "${_out}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
