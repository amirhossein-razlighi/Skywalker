#pragma once
// Front end of Wander: lexing, parsing and semantic checks.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/wander/Ast.h"

namespace sky::wander {

struct CompileResult {
    std::shared_ptr<const Program> program;  // null if there were errors
    std::vector<Diagnostic> diagnostics;

    bool ok() const { return program != nullptr; }
    size_t errorCount() const;
    Json toJson() const;  // agent-friendly diagnostics report
};

/// Compiles Wander source. `knownComponents` lets the checker validate
/// `self.<component>.<field>` paths (pass the scene's component names).
CompileResult compile(std::string_view source, const std::vector<std::string>& knownComponents = {});

/// Built-in function names (for docs, hints and autocomplete).
const std::vector<std::string>& builtinFunctions();

/// Short language reference returned to agents by the `wander_reference` tool.
const char* referenceText();

Json diagnosticToJson(const Diagnostic& d);

}  // namespace sky::wander
