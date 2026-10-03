// The DCC helper library and the Blender add-on, embedded at build time (see
// cmake/EmbedFiles.cmake and engine/CMakeLists.txt).

#include "skywalker/dcc/Manager.h"

namespace sky::dcc {

const std::vector<EmbeddedFile>& embeddedFiles() {
    static const std::vector<EmbeddedFile> files{
#include "DccEmbedded.inc"
    };
    return files;
}

}  // namespace sky::dcc
