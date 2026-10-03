#pragma once
// Wander bytecode: a compact register machine.
//
// Each function body (handler, fn, test, var initializer) compiles to a Proto: a flat
// array of 8-byte instructions over a fixed set of registers (locals and temporaries).
// Operand encoding:
//   a, b, c   register indexes (uint16)
//   RK(x)     if bit 15 is set, constant K[x & 0x7fff], else register R[x]
//   sbx       signed 32-bit jump offset stored in (b | c << 16), relative to the next pc
//
// Termination: only backward jumps (loops) and calls can repeat work. Each backward jump
// charges the loop's length and each call the callee's length against the handler's
// budget, so every handler run is bounded (see Runtime::kBudget). The AOT compiler emits
// the very same charges, so the VM and native code fail at the same point.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/wander/Ast.h"
#include "skywalker/wander/Builtins.h"
#include "skywalker/wander/Value.h"

namespace sky::wander {

enum class Op : uint8_t {
    Nop,
    Move,       // R[a] = R[b]
    LoadK,      // R[a] = K[b]
    LoadNone,   // R[a] = none
    LoadBool,   // R[a] = b != 0
    LoadSelf,   // R[a] = self
    LoadEnv,    // R[a] = environment value b (Env)
    GetVar,     // R[a] = var b of the running behavior
    SetVar,     // var a = RK(b)
    GetMember,  // R[a] = R[b].members[c]
    SetMember,  // R[a].members[b] = RK(c)      (entities write through; other values change in place)
    GetField,   // R[a] = R[b].<fields[c]>      (component field: self.light.intensity)
    SetField,   // R[a].<fields[b]> = RK(c)
    Index,      // R[a] = R[b][RK(c)]
    SetIndex,   // R[a][RK(b)] = RK(c)
    Add,        // R[a] = RK(b) + RK(c)
    Sub,
    Mul,
    Div,
    Mod,
    Neg,        // R[a] = -R[b]
    Not,        // R[a] = not R[b]
    Eq,         // R[a] = RK(b) == RK(c)
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    In,         // R[a] = RK(b) in RK(c)
    Jmp,        // pc += sbx
    JmpIf,      // if truthy(R[a]) pc += sbx
    JmpIfNot,   // if not truthy(R[a]) pc += sbx
    NewList,    // R[a] = [R[b] .. R[b+c-1]]
    NewMap,     // R[a] = {R[b]: R[b+1], ...}  (c pairs; keys are strings)
    MakeVec,    // R[a] = (R[b] .. R[b+c-1])   (c = 2, 3: vector; 4: color)
    Concat,     // R[a] = text(R[b]) .. text(R[b+c-1])
    Call,       // R[a] = builtins[b](R[a] .. R[a+c-1])
    CallM,      // methods[b] on receiver R[a] with args R[a+1 .. a+c]; result -> R[a+c+1]; R[a] keeps the (updated) receiver
    CallF,      // R[a] = functions[b](R[a] .. R[a+c-1])
    Ret,        // return R[a]
    RetNone,    // return none
    Every,      // R[a] = timer c fires now (interval RK(b) seconds)
    After,      // R[a] = one-shot timer c fires now (delay RK(b) seconds)
    ForPrep,    // numeric loop over R[a]=start, R[a+1]=limit, R[a+2]=step; x=1 inclusive. Empty -> pc += sbx
    ForLoop,    // R[a] += step; if in range { R[a+3] = R[a]; pc += sbx (backward) }
    IterPrep,   // R[a] = iterable (list, map, string), R[a+1] = cursor. Empty -> pc += sbx
    IterNext,   // advance; sets R[a+2] (x&1: and R[a+3]); jumps back (sbx) while elements remain
    Wait,       // suspend: x=0 seconds RK(b), x=1 frames RK(b)
    GoTo,       // leave the handler and switch to state b
    Stop,       // leave the handler
    Expect,     // test assertion: cond R[a], text K[b]; x=1: R[c], R[c+1] are the compared operands
    Count
};

const char* opName(Op op);

/// Environment values for LoadEnv.
enum class Env : uint16_t { Dt, Time, Frame, Other, State, StateTime };

struct Ins {
    Op op = Op::Nop;
    uint8_t x = 0;
    uint16_t a = 0;
    uint16_t b = 0;
    uint16_t c = 0;

    int32_t sbx() const { return static_cast<int32_t>(static_cast<uint32_t>(b) | (static_cast<uint32_t>(c) << 16)); }
    void setSbx(int32_t v) {
        auto u = static_cast<uint32_t>(v);
        b = static_cast<uint16_t>(u & 0xffff);
        c = static_cast<uint16_t>(u >> 16);
    }
};
static_assert(sizeof(Ins) == 8);

constexpr uint16_t kConstBit = 0x8000;
inline bool isK(uint16_t x) { return (x & kConstBit) != 0; }

enum class ProtoKind : uint8_t { Handler, Function, Test, VarInit };

struct Proto {
    std::string name;  // "Patrol.on tick", "Patrol.fn chase", "test \"...\""
    ProtoKind kind = ProtoKind::Handler;
    int behavior = -1;  // owning behavior (-1: file/module level)
    int state = -1;     // owning state (handlers inside `state` blocks)
    int numParams = 0;
    int numRegs = 0;
    bool canWait = false;  // contains wait (handlers and tests only)
    std::vector<Ins> code;
    std::vector<SourceLoc> locs;  // per instruction
    std::string file;             // module path for module functions; empty = main source
};

/// Property names resolved at compile time.
struct MemberRef {
    enum class Kind : uint8_t {
        Named,  // entity var, map key, list/string length...
        Position,
        Rotation,
        Scale,
        Color,
        Name,
        Id,
        Enabled,
        Tags,
        Parent,
        State,
        X,
        Y,
        Z,
        R,
        G,
        B,
        A,
        Length,
    };
    Kind kind = Kind::Named;
    uint32_t sym = 0;
    std::string name;
};

struct FieldRef {
    std::string component;
    std::string field;
};

struct VarInfo {
    std::string name;
    uint32_t sym = 0;
    TypeSet type = kTAny;
    int init = -1;  // initializer proto
    bool isParam = false;
    bool hasRange = false;
    double min = 0;
    double max = 0;
    std::string doc;
    Json defaultValue;  // constant default (params), for the editor and the spec
    SourceLoc loc;
};

struct HandlerInfo {
    Trigger trigger = Trigger::Tick;
    std::string argument;  // event/key name
    uint32_t sym = 0;      // interned event name
    bool custom = false;
    int proto = -1;
    int state = -1;  // -1: behavior-level
    SourceLoc loc;
};

struct StateInfo {
    std::string name;
    int enter = -1;  // handler index
    int exit = -1;
    SourceLoc loc;
};

struct TestInfo {
    std::string name;
    int proto = -1;
    int behavior = -1;
    SourceLoc loc;
};

struct FnInfo {
    std::string name;  // qualified for module functions: "combat.damage"
    int proto = -1;
    int behavior = -1;
    std::vector<std::string> params;
    std::vector<TypeSet> paramTypes;
    TypeSet returns = kTAny;
    SourceLoc loc;
    std::string file;
};

struct BehaviorInfo {
    std::string name;
    std::string intent;
    std::vector<VarInfo> vars;
    std::vector<HandlerInfo> handlers;
    std::vector<StateInfo> states;
    std::vector<TestInfo> tests;
    int timerCount = 0;
    SourceLoc loc;
};

struct ModuleDep {
    std::string path;
    uint64_t hash = 0;
};

struct Program {
    std::vector<Value> constants;
    std::vector<Proto> protos;
    std::vector<BehaviorInfo> behaviors;
    std::vector<FnInfo> functions;
    std::vector<TestInfo> fileTests;
    std::vector<MemberRef> members;
    std::vector<FieldRef> fields;
    std::vector<std::shared_ptr<const BuiltinDef>> builtins;  // Call b
    /// CallM b: per method name, the implementation for each receiver type (null = none).
    std::vector<std::string> methodNames;
    std::vector<std::array<std::shared_ptr<const BuiltinDef>, 9>> methods;
    std::vector<ModuleDep> modules;
    uint64_t hash = 0;  // source + modules + compiler version: AOT cache key
    uint64_t registryGeneration = 0;

    size_t instructionCount() const;
    /// Human-readable listing (wander_check with disassemble=true; debugging).
    std::string disassemble() const;
};

uint64_t fnv1a(std::string_view data, uint64_t seed = 1469598103934665603ULL);

}  // namespace sky::wander
