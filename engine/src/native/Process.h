#pragma once
// Running external tools (the C++ compiler, pkg-config) without a shell: arguments are
// passed as a vector, so paths and flags are never interpreted by /bin/sh.

#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::native {

struct ProcessResult {
    int exitCode = -1;
    std::string output;  // stdout and stderr, interleaved
    bool timedOut = false;
};

/// Runs argv[0] (searched in PATH when it has no '/') and waits for it.
Result<ProcessResult> runProcess(const std::vector<std::string>& argv, const std::string& cwd = {}, int timeoutSeconds = 300);

/// Searches PATH for an executable.
std::string whichExecutable(const std::string& name);

/// A structured compiler diagnostic parsed from clang/gcc output.
struct CompilerDiagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string severity;  // error | warning | note
    std::string message;
    Json toJson() const;
};
std::vector<CompilerDiagnostic> parseCompilerOutput(const std::string& output);

}  // namespace sky::native
