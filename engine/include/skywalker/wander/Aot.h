#pragma once
// Ahead-of-time compilation of Wander programs to native code.
//
// generateCpp() translates a program's bytecode to C++ against the AOT ABI
// (skywalker/native/aot.h, embedded into the file); compileNative() builds it with the
// system clang++ (-O2 -shared -fPIC -std=c++20) into a dylib in the project cache,
// dlopen()s it and returns the function table. Libraries are cached by program hash, so
// unchanged behaviors load instantly. Any failure leaves the program on the VM.

#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/native/aot.h"
#include "skywalker/wander/Bytecode.h"

namespace sky::wander {

struct NativeProgram {
    uint64_t hash = 0;
    std::string library;  // path of the loaded dylib
    void* handle = nullptr;
    std::vector<SkyAotFn> fns;  // per proto (null: interpreted)

    NativeProgram() = default;
    NativeProgram(const NativeProgram&) = delete;
    NativeProgram& operator=(const NativeProgram&) = delete;
    ~NativeProgram();
};

struct AotOptions {
    std::string cacheDir;            // where sources and libraries go (created if missing)
    std::string compiler;            // empty: find clang++ (xcrun, PATH)
    std::vector<std::string> flags;  // extra compiler flags
    bool force = false;              // rebuild even if a cached library exists
};

struct AotResult {
    std::shared_ptr<const NativeProgram> native;
    std::string source;   // generated .cpp path
    std::string library;  // .dylib path
    bool cached = false;  // loaded an existing library
    double compileMs = 0;
    size_t protos = 0;    // compiled protos
    size_t inlined = 0;   // instructions translated inline (the rest call back into the VM)
    size_t delegated = 0;
    std::string compilerOutput;
    Json toJson() const;
};

/// C++ translation of a program (self-contained: the ABI header is embedded).
std::string generateCpp(const Program& prog, size_t* inlined = nullptr, size_t* delegated = nullptr);

/// Builds (or loads from cache) native code for a program.
Result<AotResult> compileNative(const Program& prog, const AotOptions& options);

/// Path of a usable C++ compiler, or "" if none (AOT and native modules unavailable).
std::string findCxxCompiler();

/// The host API table handed to generated code.
const SkyAotApi* aotHostApi();

}  // namespace sky::wander
