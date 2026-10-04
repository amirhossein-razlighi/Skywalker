#pragma once
// Wander 2 syntax tree.
//
// The parser produces this tree; the compiler checks it and emits bytecode; the
// formatter prints it back canonically; the graph view converts it to and from nodes.
// Nothing here is resolved or typed: that is the compiler's job.
//
// Language summary (see docs/WANDER.md for the guide):
//   file      := (use | const | fn | behavior | test | handler)*
//   behavior  := 'behavior' Name (intent | var | param | const | fn | handler | state | test)* 'end'
//   handler   := 'on' trigger block 'end'
//   state     := 'state' Name handler* 'end'            (on enter / on exit / on tick / ...)
//   test      := 'test' "name" block 'end'

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
    std::string file;  // module path when the problem is inside a `use`d module; empty = this source
};

/// A type annotation as written (`number`, `entity?`, `number|string`). Empty = none.
struct TypeRef {
    std::string text;
    SourceLoc loc;
    bool empty() const { return text.empty(); }
};

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using Block = std::vector<StmtPtr>;

struct Expr {
    enum class Kind {
        Number,      // number
        String,      // text (no interpolation)
        Interp,      // "a {x} b": parts = literal segments (n+1), args = expressions (n)
        Bool,        // number != 0
        None,
        Color,       // color
        Vector,      // (x, y, z) / (r, g, b, a): args
        List,        // [a, b]: args
        Map,         // {k: v}: parts = keys, args = values
        Ident,       // text
        Member,      // lhs.text
        Index,       // lhs[rhs]
        Call,        // text(args)        free function: builtin, fn, or a state machine helper
        MethodCall,  // lhs.text(args)    list/map/string methods, module functions (alias.fn)
        Unary,       // text in {"-", "not"}: lhs
        Binary,      // lhs text rhs: + - * / % < <= > >= == != and or in
    };
    Kind kind;
    SourceLoc loc;
    double number = 0;
    std::string text;
    Vec4 color;
    std::vector<ExprPtr> args;
    std::vector<std::string> parts;
    ExprPtr lhs;
    ExprPtr rhs;
    bool parenthesized = false;  // written in (...) — kept so the formatter preserves intent

    Expr(Kind k, SourceLoc l) : kind(k), loc(l) {}
};

enum class WaitKind { Seconds, Frames, Until };

struct Stmt {
    enum class Kind {
        Let,          // let name(: type) = value
        Const,        // const name = value (block-local constant)
        Assign,       // target = value | set target to value
        OpAssign,     // target op= value (name = "+", "-", "*", "/")
        If,           // branches: (cond, body)...; cond == nullptr => else
        While,        // while value ... end
        For,          // for name(, name2) in value ... | for name in value..extra (step extra2)
        Repeat,       // repeat value times ... end
        Every,        // every value seconds ... end
        After,        // after value seconds ... end
        Wait,         // wait value | wait until value | wait frames value
        Break,
        Continue,
        Return,       // return (value)?
        Stop,         // stop: leave the handler
        GoTo,         // go to name
        Move,         // move target by value
        MoveToward,   // move target toward value at extra
        Rotate,       // rotate target by value
        Look,         // look target at value
        Emit,         // emit "name" (with value)? (to extra)?
        Destroy,      // destroy target
        Log,          // log value
        Call,         // value is a call expression used as a statement
        Expect,       // expect value ("message" in extra)?            tests only
        Press,        // press value  (key name)                       tests only
        Hold,         // hold value                                    tests only
        Release,      // release value                                 tests only
        Click,        // click value (entity)                          tests only
    };
    Kind kind;
    SourceLoc loc;
    int nodeId = 0;  // unique per file
    std::string name;
    std::string name2;
    TypeRef type;
    ExprPtr target;
    ExprPtr value;
    ExprPtr extra;
    ExprPtr extra2;
    bool isRange = false;  // For: numeric range form
    bool inclusive = false;  // For: a..=b
    WaitKind waitKind = WaitKind::Seconds;
    std::vector<std::pair<ExprPtr, Block>> branches;
    Block body;
    std::vector<std::string> comments;  // comment lines directly above (kept by formatter/graph)

    Stmt(Kind k, SourceLoc l) : kind(k), loc(l) {}
};

enum class Trigger {
    Start, Tick, Event, Key, Click, Enter, Exit,
    Action,  // `on action "jump"`: an input action was pressed (input.json)
    // physics: `on collide "filter"?`, `on trigger_enter "filter"?`, `on trigger_exit "filter"?`
    // (filter = the other entity's name or one of its tags; `other` names it in the handler)
    Collide, TriggerEnter, TriggerExit,
    // `on frame`: every displayed frame (display rate), cosmetic only: its writes are undone after the
    // frame and it cannot change vars, wait, change state, spawn or emit (docs/WANDER.md "on frame").
    Frame,
};
const char* toString(Trigger t);

struct Handler {
    Trigger trigger = Trigger::Tick;
    std::string argument;  // event/key/action name, contact filter, or the registered trigger word (custom)
    std::string binding;   // `with <name>` for event payloads (default: data)
    bool custom = false;   // registered trigger word like `on contact` (an event underneath)
    SourceLoc loc;
    Block body;
    std::vector<std::string> comments;
};

struct Param {
    std::string name;
    TypeRef type;
    SourceLoc loc;
};

struct FnDecl {
    std::string name;
    std::vector<Param> params;
    TypeRef returns;
    Block body;
    SourceLoc loc;
    std::vector<std::string> comments;
};

struct VarDecl {
    std::string name;
    TypeRef type;
    ExprPtr initial;
    SourceLoc loc;
    bool isParam = false;  // `param`: a tunable exposed in the editor and the spec
    ExprPtr minValue;      // param range (optional)
    ExprPtr maxValue;
    std::string doc;       // param description (optional)
    std::vector<std::string> comments;
};

struct ConstDecl {
    std::string name;
    TypeRef type;
    ExprPtr value;
    SourceLoc loc;
    std::vector<std::string> comments;
};

struct StateDecl {
    std::string name;
    std::vector<Handler> handlers;
    SourceLoc loc;
    std::vector<std::string> comments;
};

struct TestDecl {
    std::string name;
    Block body;
    SourceLoc loc;
    std::vector<std::string> comments;
};

struct BehaviorDef {
    std::string name;
    std::string intent;
    std::vector<VarDecl> vars;  // vars and params in declaration order
    std::vector<ConstDecl> consts;
    std::vector<FnDecl> fns;
    std::vector<Handler> handlers;
    std::vector<StateDecl> states;
    std::vector<TestDecl> tests;
    SourceLoc loc;
    bool implicit = false;  // bare handlers at file level (no `behavior` header)
    std::vector<std::string> comments;
};

struct UseDecl {
    std::string path;   // project-relative, ".wander" optional
    std::string alias;  // defaults to the file stem
    SourceLoc loc;
    std::vector<std::string> comments;
};

/// One parsed source file (a behavior script or a `use`d library module).
struct Module {
    std::vector<UseDecl> uses;
    std::vector<ConstDecl> consts;
    std::vector<FnDecl> fns;
    std::vector<BehaviorDef> behaviors;
    std::vector<TestDecl> tests;  // file-level tests
    std::vector<std::string> trailingComments;
    int nodeCount = 0;
};

}  // namespace sky::wander
