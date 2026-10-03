#pragma once
// Wander — Skywalker's Entity-Component-Prompt language.
//
// Each behaviour pairs a natural-language *intent* (what a person or agent wants)
// with a small deterministic program (what the engine runs). Wander is designed so
// that language models write it correctly on the first try and so that anything
// they write is safe to run:
//   * keyword-led statements that read like English ("move self toward target at 3"),
//   * no unbounded loops (only `repeat N times`, N <= 1000) => every handler terminates,
//   * all randomness is seeded => runs are reproducible,
//   * precise diagnostics with line/column and "did you mean" hints.
// See docs/WANDER.md for the language guide.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/math/Math.h"

namespace sky::wander {

struct SourceLoc {
    int line = 1;
    int column = 1;
};

enum class Severity { Error, Warning };

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceLoc loc;
    std::string code;  // stable id, e.g. "unknown_function"
    std::string message;
    std::string hint;
};

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using Block = std::vector<StmtPtr>;

struct Expr {
    enum class Kind { Number, String, Bool, None, Color, Vector, Ident, Member, Call, Unary, Binary };
    Kind kind;
    SourceLoc loc;
    double number = 0;
    std::string text;  // string literal, identifier, member name, function name, operator
    Vec4 color;
    std::vector<ExprPtr> args;  // vector components / call arguments
    ExprPtr lhs;                // member object, unary operand, binary lhs
    ExprPtr rhs;                // binary rhs

    Expr(Kind k, SourceLoc l) : kind(k), loc(l) {}
};

struct Stmt {
    enum class Kind {
        Let,         // let name = value
        Assign,      // target = value | set target to value
        If,          // if cond then ... (elif ...)* (else ...)? end
        Every,       // every <seconds> ... end
        After,       // after <seconds> ... end
        Repeat,      // repeat <n> times ... end
        Move,        // move <target> by <vec>
        MoveToward,  // move <target> toward <point|entity> at <speed>
        Rotate,      // rotate <target> by <vec>
        Look,        // look <target> at <point|entity>
        Emit,        // emit "name" (to <entity>)?
        Destroy,     // destroy <entity>
        Log,         // log <value>
        Stop,        // stop (leave this handler)
        Call,        // bare function call, e.g. spawn("cube", (0,1,0))
    };
    Kind kind;
    SourceLoc loc;
    int nodeId = 0;  // unique within a program; keys timer state for every/after
    std::string name;  // let name / emit event name
    ExprPtr target;    // assignment target / subject entity
    ExprPtr value;     // primary expression
    ExprPtr extra;     // speed for MoveToward, receiver for Emit
    std::vector<std::pair<ExprPtr, Block>> branches;  // If: (cond, body)...; cond==nullptr => else
    Block body;  // Every / After / Repeat

    Stmt(Kind k, SourceLoc l) : kind(k), loc(l) {}
};

enum class Trigger { Start, Tick, Event, Key, Click, Action };
const char* toString(Trigger t);

struct Handler {
    Trigger trigger = Trigger::Tick;
    std::string argument;  // event name / key name
    SourceLoc loc;
    Block body;
};

struct VarDecl {
    std::string name;
    ExprPtr initial;
    SourceLoc loc;
};

struct BehaviorDef {
    std::string name;
    std::string intent;
    std::vector<VarDecl> vars;
    std::vector<Handler> handlers;
    SourceLoc loc;
};

struct Program {
    std::vector<BehaviorDef> behaviors;
    int nodeCount = 0;
};

}  // namespace sky::wander
