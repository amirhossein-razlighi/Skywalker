# Usage: cmake -DSPECS="prefix=dir=glob|prefix=dir=glob" -DEXCLUDE=<regex> -DKEEP=<regex> -DOUTPUT=<file> -P EmbedTree.cmake
# Embeds every file below each directory matching the glob as {"prefix/relative/path", R"(content)"},
# ready to be #included inside an array initializer. Dot directories (.claude-plugin, .agents, ...) are
# included. A file whose "prefix/relative/path" matches EXCLUDE is skipped unless it also matches KEEP.
# Used to ship the engine docs and the agent integrations (skills, subagents, commands) in the binary,
# so `skywalker setup` and the MCP resources/prompts work without the source tree.
string(REPLACE "|" ";" _specs "${SPECS}")
set(_out "")
foreach(_spec IN LISTS _specs)
    string(REPLACE "=" ";" _parts "${_spec}")
    list(GET _parts 0 _prefix)
    list(GET _parts 1 _dir)
    list(GET _parts 2 _glob)
    file(GLOB_RECURSE _files RELATIVE "${_dir}" "${_dir}/${_glob}")
    list(SORT _files)
    foreach(_f IN LISTS _files)
        set(_key "${_prefix}/${_f}")
        if(EXCLUDE AND _key MATCHES "${EXCLUDE}" AND NOT (KEEP AND _key MATCHES "${KEEP}"))
            continue()
        endif()
        if(_f MATCHES "(^|/)(__pycache__|\\.DS_Store)")
            continue()
        endif()
        file(READ "${_dir}/${_f}" _content)
        string(FIND "${_content}" ")SKYEMBED\"" _clash)
        if(NOT _clash EQUAL -1)
            message(FATAL_ERROR "${_dir}/${_f} contains the embed delimiter")
        endif()
        string(APPEND _out "    {\"${_key}\", R\"SKYEMBED(${_content})SKYEMBED\"},\n")
    endforeach()
endforeach()
file(WRITE "${OUTPUT}.tmp" "${_out}")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
