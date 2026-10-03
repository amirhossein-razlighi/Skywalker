#pragma once
// Wander compiler: syntax tree -> checked, typed bytecode (Program).
//
// The compiler resolves names (locals, vars, consts, functions, module members, states),
// infers types (gradual: annotations are optional, errors only when a mismatch is
// certain), checks calls against the builtin registry, and emits register bytecode.
// Diagnostics carry line/column, a stable code and usually a did-you-mean hint.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/wander/Ast.h"
#include "skywalker/wander/Builtins.h"
#include "skywalker/wander/Bytecode.h"

namespace sky::wander {

/// Loads the source of a `use`d module by its normalized project path
/// ("scripts/combat.wander").
using ModuleLoader = std::function<Result<std::string>(const std::string& path)>;

struct CompileOptions {
    /// Component names valid in `e.<component>.<field>` (pass Scene::componentNames()).
    std::vector<std::string> components;
    /// Their reflection (optional): enables field-name and field-type checks.
    std::vector<const TypeInfo*> componentTypes;
    /// Builtins to check against; null = BuiltinRegistry::global().
    const BuiltinRegistry* registry = nullptr;
    /// Resolves `use "path"`; empty = modules unavailable (a diagnostic is reported).
    ModuleLoader loadModule;
};

struct CompileResult {
    std::shared_ptr<const Program> program;  // null if there were errors
    std::shared_ptr<const Module> module;    // the parsed tree (also on errors)
    std::vector<Diagnostic> diagnostics;

    bool ok() const { return program != nullptr; }
    size_t errorCount() const;
    /// Agent-friendly report: ok, diagnostics, and a summary of behaviors (vars, params,
    /// handlers, states, functions, tests).
    Json toJson() const;
};

CompileResult compile(std::string_view source, const CompileOptions& options);
/// Convenience: compile against the global registry with the given component names.
CompileResult compile(std::string_view source, const std::vector<std::string>& knownComponents = {});

/// "scripts/combat" -> "scripts/combat.wander"; rejects absolute paths and "..".
Result<std::string> normalizeModulePath(std::string_view path);

/// Builtin function names (for docs, hints and autocomplete).
std::vector<std::string> builtinFunctions();

/// The compact, LLM-oriented language guide returned by `wander_reference`. The function
/// list is generated from the registry.
std::string referenceText(const BuiltinRegistry& registry = BuiltinRegistry::global());
/// Reference for builtins: every entry or one category / name (wander_reference topic=...).
Json builtinReference(const BuiltinRegistry& registry, std::string_view filter);

Json diagnosticToJson(const Diagnostic& d);

/// Bumped when the bytecode format or code generation changes (cache keys).
constexpr int kCompilerVersion = 2;

}  // namespace sky::wander
