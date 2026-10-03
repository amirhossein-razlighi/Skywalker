#pragma once
// Wander front end: source text -> syntax tree (no name resolution or typing).

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/wander/Ast.h"

namespace sky::wander {

struct ParseResult {
    std::shared_ptr<Module> module;  // always set (possibly partial when there are errors)
    std::vector<Diagnostic> diagnostics;
    bool ok() const;
};

/// Parses a Wander source file. Comments are kept on the statements and declarations they
/// precede so the formatter and the graph view can preserve them.
ParseResult parse(std::string_view source);

/// Parses a single expression ("self.position + (0, 1, 0)"). Null with diagnostics on error.
ExprPtr parseExpression(std::string_view text, std::vector<Diagnostic>* diagnostics = nullptr);

/// Words that cannot be used as names.
const std::vector<std::string>& reservedWords();

/// Canonical source text for a module (2-space indentation, one statement per line).
/// format(parse(format(m))) == format(m). Used by the graph view and `wander_format`.
std::string format(const Module& module);
std::string formatExpr(const Expr& e);

}  // namespace sky::wander
