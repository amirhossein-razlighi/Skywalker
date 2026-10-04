// Wander compiler: name resolution, gradual type checking and register bytecode emission.

#include "skywalker/wander/Compiler.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/wander/Parser.h"

namespace sky::wander {

namespace {

constexpr int kMaxRegs = 240;
constexpr int kMaxModules = 64;

struct ConstSym {
    Value value;
    TypeSet type = kTAny;
    SourceLoc loc;
};

template <typename V>
using NameMap = std::map<std::string, V, std::less<>>;

struct ModuleInfo {
    std::string path;
    std::shared_ptr<Module> ast;
    uint64_t hash = 0;
    NameMap<int> fns;  // -> Program::functions
    NameMap<ConstSym> consts;
    NameMap<ModuleInfo*> uses;
    bool declared = false;
};

/// Declarations visible from a piece of code (file or module level, plus behavior level).
struct DeclScope {
    const NameMap<ConstSym>* consts = nullptr;
    const NameMap<int>* fns = nullptr;
    const NameMap<ModuleInfo*>* uses = nullptr;
    std::string file;
};

struct BehaviorScope {
    int index = -1;
    const BehaviorDef* def = nullptr;
    NameMap<int> vars;  // -> BehaviorInfo::vars
    NameMap<ConstSym> consts;
    NameMap<int> fns;
    NameMap<int> states;
};

struct Local {
    std::string name;
    int reg = 0;
    TypeSet type = kTAny;
    bool isConst = false;
    bool annotated = false;
};

struct Operand {
    uint16_t rk = 0;
    TypeSet type = kTAny;
};

// One step of an assignable path: a.b, a[i], a.<component>.<field>.
struct Step {
    enum class Kind { Member, Index, Field } kind;
    int member = -1;  // Program::members / Program::fields index
    const Expr* index = nullptr;
    TypeSet type = kTAny;  // static type of the value at this step
    SourceLoc loc;
};

struct Chain {
    enum class Base { Local, Var, Value } base = Base::Value;
    int localReg = -1;
    int var = -1;
    const Expr* baseExpr = nullptr;
    TypeSet baseType = kTAny;
    std::vector<Step> steps;
    std::vector<int> regs;  // regs[0] = base value, regs[i+1] = value after steps[i]
};

bool isComparison(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

std::string quote(const std::string& s) { return "'" + s + "'"; }

// ---------------------------------------------------------------------------
// Static typing of operators (mirrors the VM's semantics exactly)
// ---------------------------------------------------------------------------

/// Result type of `a op b` for single runtime types; nullopt if the VM raises an error.
std::optional<VType> binaryResult(const std::string& op, VType a, VType b) {
    using T = VType;
    auto numeric = [](T t) { return t == T::Number || t == T::Bool; };
    if (op == "==" || op == "!=") return T::Bool;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        if (numeric(a) && numeric(b)) return T::Bool;
        if (a == T::String && b == T::String) return T::Bool;
        return std::nullopt;
    }
    if (op == "in") {
        if (b == T::List || b == T::Map || b == T::String) return T::Bool;
        return std::nullopt;
    }
    if (op == "+") {
        if (a == T::String || b == T::String) return T::String;
        if (numeric(a) && numeric(b)) return T::Number;
        if (a == T::Vec && b == T::Vec) return T::Vec;
        if (a == T::Color && b == T::Color) return T::Color;
        if (a == T::List && b == T::List) return T::List;
        return std::nullopt;
    }
    if (op == "-") {
        if (numeric(a) && numeric(b)) return T::Number;
        if (a == T::Vec && b == T::Vec) return T::Vec;
        return std::nullopt;
    }
    if (op == "*") {
        if (numeric(a) && numeric(b)) return T::Number;
        if (a == T::Vec && (b == T::Vec || numeric(b))) return T::Vec;
        if (numeric(a) && b == T::Vec) return T::Vec;
        if (a == T::Color && numeric(b)) return T::Color;
        if (numeric(a) && b == T::Color) return T::Color;
        return std::nullopt;
    }
    if (op == "/") {
        if (numeric(a) && numeric(b)) return T::Number;
        if (a == T::Vec && numeric(b)) return T::Vec;
        return std::nullopt;
    }
    if (op == "%") {
        if (numeric(a) && numeric(b)) return T::Number;
        return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// The compiler
// ---------------------------------------------------------------------------

class Compiler {
public:
    Compiler(const CompileOptions& opts, std::vector<Diagnostic>& diags, Program& prog)
        : opts_(opts),
          reg_(opts.registry ? *opts.registry : BuiltinRegistry::global()),
          diags_(diags),
          prog_(prog) {
        for (const auto& c : opts_.components) components_.insert(c);
    }

    void run(const Module& main, std::string_view source) {
        prog_.registryGeneration = reg_.generation();
        uint64_t hash = fnv1a(source);
        hash = fnv1a(std::to_string(kCompilerVersion), hash);

        // Pass 1: modules, file-level declarations, behavior declarations.
        mainDecl_.file = "";
        for (const auto& u : main.uses) {
            if (ModuleInfo* m = loadModule(u)) {
                if (mainUses_.count(u.alias)) error(u.loc, "duplicate_name", "module alias " + quote(u.alias) + " is used twice");
                mainUses_[u.alias] = m;
            }
        }
        for (const auto& m : modules_) {
            prog_.modules.push_back({m->path, m->hash});
            hash = fnv1a(m->path, hash ^ m->hash);
        }
        prog_.hash = hash;
        mainDecl_.consts = &mainConsts_;
        mainDecl_.fns = &mainFns_;
        mainDecl_.uses = &mainUses_;
        declareConsts(main.consts, mainConsts_, &mainDecl_, nullptr);
        for (const auto& f : main.fns) declareFn(f, mainFns_, -1, "", "");

        for (size_t bi = 0; bi < main.behaviors.size(); ++bi) {
            const BehaviorDef& b = main.behaviors[bi];
            for (size_t bj = 0; bj < bi; ++bj) {
                if (main.behaviors[bj].name == b.name) {
                    error(b.loc, "duplicate_name", "behavior " + quote(b.name) + " is defined twice in this script");
                }
            }
            declareBehavior(b, static_cast<int>(bi));
        }

        // Pass 2: bodies.
        for (auto& m : modules_) compileModuleBodies(*m);
        for (const auto& f : main.fns) compileFn(f, mainFns_.at(f.name), &mainDecl_, nullptr);
        for (size_t bi = 0; bi < behaviors_.size(); ++bi) compileBehavior(*behaviors_[bi]);
        for (const auto& t : main.tests) compileTest(t, &mainDecl_, nullptr);
    }

private:
    // --- diagnostics -------------------------------------------------------------
    void error(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        diags_.push_back({Severity::Error, loc, std::move(code), std::move(msg), std::move(hint), file_});
    }
    void warning(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        diags_.push_back({Severity::Warning, loc, std::move(code), std::move(msg), std::move(hint), file_});
    }
    static std::string didYouMean(const std::string& word, const std::vector<std::string>& candidates) {
        std::string guess = str::closest(word, candidates);
        return guess.empty() ? "" : "did you mean " + quote(guess) + "?";
    }

    // --- modules -----------------------------------------------------------------
    ModuleInfo* loadModule(const UseDecl& u) {
        auto norm = normalizeModulePath(u.path);
        if (!norm) {
            error(u.loc, "invalid_module_path", norm.error().message, norm.error().hint);
            return nullptr;
        }
        const std::string& path = norm.value();
        if (auto it = moduleByPath_.find(path); it != moduleByPath_.end()) {
            if (!it->second->declared) {
                error(u.loc, "module_cycle", "modules use each other in a cycle (" + path + ")",
                      "move the shared functions into a third module");
                return nullptr;
            }
            return it->second;
        }
        if (!opts_.loadModule) {
            error(u.loc, "modules_unavailable", "cannot load module " + quote(path) + " here (no project)",
                  "modules are loaded from the project folder when the script runs in the engine");
            return nullptr;
        }
        if (modules_.size() >= kMaxModules) {
            error(u.loc, "too_many_modules", "a script can use at most 64 modules");
            return nullptr;
        }
        auto text = opts_.loadModule(path);
        if (!text) {
            error(u.loc, "unknown_module", "cannot load module " + quote(path) + ": " + text.error().message,
                  text.error().hint.empty() ? "modules are .wander files in the project, e.g. scripts/combat.wander"
                                            : text.error().hint);
            return nullptr;
        }
        auto m = std::make_unique<ModuleInfo>();
        ModuleInfo* mi = m.get();
        mi->path = path;
        mi->hash = fnv1a(text.value());
        moduleByPath_[path] = mi;
        modules_.push_back(std::move(m));

        ParseResult pr = parse(text.value());
        mi->ast = pr.module;
        std::string savedFile = file_;
        file_ = path;
        for (auto d : pr.diagnostics) {
            d.file = path;
            diags_.push_back(std::move(d));
        }
        const Module& ast = *mi->ast;
        for (const auto& b : ast.behaviors) {
            error(b.loc, "module_has_behavior",
                  "modules may only contain fn, const and use declarations (found behavior " + quote(b.name) + ")",
                  "attach behaviors to entities with behavior_set; put shared code here");
        }
        for (const auto& t : ast.tests) {
            (void)t;
        }
        for (const auto& use : ast.uses) {
            if (ModuleInfo* dep = loadModule(use)) mi->uses[use.alias] = dep;
        }
        DeclScope scope{&mi->consts, &mi->fns, &mi->uses, path};
        declareConsts(ast.consts, mi->consts, &scope, nullptr);
        std::string stem = path.substr(0, path.size() - 7);
        if (auto slash = stem.find_last_of('/'); slash != std::string::npos) stem = stem.substr(slash + 1);
        for (const auto& f : ast.fns) declareFn(f, mi->fns, -1, stem + ".", path);
        mi->declared = true;
        file_ = savedFile;
        return mi;
    }

    void compileModuleBodies(ModuleInfo& m) {
        std::string saved = file_;
        file_ = m.path;
        DeclScope scope{&m.consts, &m.fns, &m.uses, m.path};
        for (const auto& f : m.ast->fns) compileFn(f, m.fns.at(f.name), &scope, nullptr);
        for (const auto& t : m.ast->tests) compileTest(t, &scope, nullptr);
        file_ = saved;
    }

    // --- declarations -------------------------------------------------------------
    TypeSet annotation(const TypeRef& t) {
        if (t.empty()) return kTAny;
        TypeSet ts = parseTypeName(t.text);
        if (!ts) {
            static const std::vector<std::string> names{"number", "bool", "string", "vec", "color", "entity",
                                                        "list", "map", "any", "none"};
            std::string word = t.text;
            if (!word.empty() && word.back() == '?') word.pop_back();
            error(t.loc, "unknown_type", "unknown type " + quote(t.text), didYouMean(word, names).empty()
                                                                              ? "types: number, bool, string, vec, color, entity, list, map, any (add ? for 'or none')"
                                                                              : didYouMean(word, names));
            return kTAny;
        }
        return ts;
    }

    void declareConsts(const std::vector<ConstDecl>& consts, NameMap<ConstSym>& out, const DeclScope* scope,
                       BehaviorScope* bs) {
        for (const auto& c : consts) {
            if (c.name.empty()) continue;
            if (out.count(c.name)) {
                error(c.loc, "duplicate_name", "const " + quote(c.name) + " is defined twice");
                continue;
            }
            std::optional<Value> v = c.value ? constEval(*c.value, scope, bs, &out) : std::nullopt;
            if (!v) {
                if (c.value) {
                    error(c.value->loc, "not_constant", "the value of const " + quote(c.name) + " must be known before the game runs",
                          "use literals, other consts and arithmetic; use `var` for values computed at run time");
                }
                continue;
            }
            TypeSet t = tbit(v->type());
            TypeSet declared = annotation(c.type);
            if (!(declared & t)) {
                error(c.loc, "type_mismatch", "const " + quote(c.name) + " is declared " + typeSetName(declared) +
                                                  " but its value is a " + typeName(v->type()));
            }
            out[c.name] = {*v, t, c.loc};
        }
    }

    void declareFn(const FnDecl& f, NameMap<int>& out, int behavior, const std::string& prefix, const std::string& file) {
        if (f.name.empty()) return;
        if (out.count(f.name)) {
            error(f.loc, "duplicate_name", "fn " + quote(f.name) + " is defined twice");
            return;
        }
        FnInfo info;
        info.name = prefix + f.name;
        info.behavior = behavior;
        info.loc = f.loc;
        info.file = file;
        std::set<std::string> seen;
        for (const auto& p : f.params) {
            if (!p.name.empty() && !seen.insert(p.name).second) {
                error(p.loc, "duplicate_name", "parameter " + quote(p.name) + " appears twice");
            }
            info.params.push_back(p.name);
            info.paramTypes.push_back(annotation(p.type));
        }
        info.returns = annotation(f.returns);
        if (reg_.find(f.name) && prefix.empty()) {
            warning(f.loc, "shadows_builtin", "fn " + quote(f.name) + " hides the builtin function with the same name");
        }
        int idx = static_cast<int>(prog_.functions.size());
        prog_.functions.push_back(std::move(info));
        // Reserve the proto now so recursive and forward calls can reference it.
        Proto p;
        p.name = prog_.functions[idx].name;
        p.kind = ProtoKind::Function;
        p.behavior = behavior;
        p.numParams = static_cast<int>(f.params.size());
        p.file = file;
        prog_.functions[idx].proto = static_cast<int>(prog_.protos.size());
        prog_.protos.push_back(std::move(p));
        out[f.name] = idx;
    }

    void declareBehavior(const BehaviorDef& b, int index) {
        auto bs = std::make_unique<BehaviorScope>();
        bs->index = index;
        bs->def = &b;
        BehaviorInfo info;
        info.name = b.name;
        info.intent = b.intent;
        info.loc = b.loc;
        prog_.behaviors.push_back(std::move(info));

        declareConsts(b.consts, bs->consts, &mainDecl_, bs.get());
        for (const auto& v : b.vars) {
            if (v.name.empty()) continue;
            if (bs->vars.count(v.name)) {
                error(v.loc, "duplicate_name", "var " + quote(v.name) + " is declared twice");
                continue;
            }
            if (bs->consts.count(v.name)) error(v.loc, "duplicate_name", quote(v.name) + " is both a const and a var");
            VarInfo vi;
            vi.name = v.name;
            vi.sym = intern(v.name);
            vi.isParam = v.isParam;
            vi.doc = v.doc;
            vi.loc = v.loc;
            vi.type = annotation(v.type);
            if (v.initial) {
                if (auto c = constEval(*v.initial, &mainDecl_, bs.get(), nullptr)) {
                    vi.defaultValue = toJson(*c);
                    if (v.type.empty()) vi.type = typeOfConstInit(*c);
                    if (!(vi.type & tbit(c->type()))) {
                        error(v.initial->loc, "type_mismatch", "var " + quote(v.name) + " is declared " +
                                                                    typeSetName(vi.type) + " but starts as a " +
                                                                    typeName(c->type()));
                    }
                }
            }
            if (v.isParam) {
                if (v.minValue || v.maxValue) {
                    auto lo = v.minValue ? constEval(*v.minValue, &mainDecl_, bs.get(), nullptr) : std::nullopt;
                    auto hi = v.maxValue ? constEval(*v.maxValue, &mainDecl_, bs.get(), nullptr) : std::nullopt;
                    if (!lo || !hi || !lo->isNumber() || !hi->isNumber()) {
                        error(v.loc, "invalid_range", "param range bounds must be constant numbers, e.g. in 0..10");
                    } else {
                        vi.hasRange = true;
                        vi.min = std::min(lo->num(), hi->num());
                        vi.max = std::max(lo->num(), hi->num());
                    }
                }
                if (vi.defaultValue.isNull() && v.initial) {
                    error(v.initial->loc, "not_constant", "a param's default must be a constant (it is shown in the editor)",
                          "use a literal like 3 or (0, 1, 0); compute derived values in `on start`");
                }
            }
            bs->vars[v.name] = static_cast<int>(prog_.behaviors[index].vars.size());
            prog_.behaviors[index].vars.push_back(std::move(vi));
        }
        for (const auto& f : b.fns) declareFn(f, bs->fns, index, b.name + ".", "");
        for (size_t si = 0; si < b.states.size(); ++si) {
            const auto& s = b.states[si];
            if (s.name.empty()) continue;
            if (bs->states.count(s.name)) {
                error(s.loc, "duplicate_name", "state " + quote(s.name) + " is defined twice");
                continue;
            }
            bs->states[s.name] = static_cast<int>(si);
            StateInfo st;
            st.name = s.name;
            st.loc = s.loc;
            prog_.behaviors[index].states.push_back(st);
        }
        behaviors_.push_back(std::move(bs));
    }

    static TypeSet typeOfConstInit(const Value& v) {
        // `var target = none` means "something, later": don't lock the type to none.
        if (v.isNone()) return kTAny;
        return tbit(v.type());
    }

    // --- constant evaluation -------------------------------------------------------
    std::optional<Value> lookupConst(const std::string& name, const DeclScope* scope, const BehaviorScope* bs,
                                     const NameMap<ConstSym>* pending) {
        if (fn_ && findLocal(name)) return std::nullopt;  // a local shadows the const
        if (pending) {
            if (auto it = pending->find(name); it != pending->end()) return it->second.value;
        }
        if (bs) {
            if (auto it = bs->consts.find(name); it != bs->consts.end()) return it->second.value;
        }
        if (scope && scope->consts) {
            if (auto it = scope->consts->find(name); it != scope->consts->end()) return it->second.value;
        }
        if (name == "pi") return Value::number(3.14159265358979323846);
        return std::nullopt;
    }

    std::optional<Value> constEval(const Expr& e, const DeclScope* scope, const BehaviorScope* bs,
                                   const NameMap<ConstSym>* pending) {
        switch (e.kind) {
            case Expr::Kind::Number: return Value::number(e.number);
            case Expr::Kind::String: return Value::string(e.text);
            case Expr::Kind::Bool: return Value::boolean(e.number != 0);
            case Expr::Kind::None: return Value();
            case Expr::Kind::Color: return Value::color(e.color);
            case Expr::Kind::Vector: {
                float c[4] = {0, 0, 0, 1};
                for (size_t i = 0; i < e.args.size() && i < 4; ++i) {
                    auto v = e.args[i] ? constEval(*e.args[i], scope, bs, pending) : std::nullopt;
                    if (!v || !v->isNumber()) return std::nullopt;
                    c[i] = static_cast<float>(v->num());
                }
                if (e.args.size() == 4) return Value::color({c[0], c[1], c[2], c[3]});
                return Value::vec({c[0], c[1], c[2]});
            }
            case Expr::Kind::List: {
                std::vector<Value> items;
                for (const auto& a : e.args) {
                    auto v = a ? constEval(*a, scope, bs, pending) : std::nullopt;
                    if (!v) return std::nullopt;
                    items.push_back(*v);
                }
                return Value::list(std::move(items));
            }
            case Expr::Kind::Map: {
                Value m = Value::map();
                for (size_t i = 0; i < e.args.size() && i < e.parts.size(); ++i) {
                    auto v = e.args[i] ? constEval(*e.args[i], scope, bs, pending) : std::nullopt;
                    if (!v) return std::nullopt;
                    m.mutMap().set(e.parts[i], *v);
                }
                return m;
            }
            case Expr::Kind::Ident: return lookupConst(e.text, scope, bs, pending);
            case Expr::Kind::Unary: {
                auto v = e.lhs ? constEval(*e.lhs, scope, bs, pending) : std::nullopt;
                if (!v) return std::nullopt;
                if (e.text == "-" && v->isNumber()) return Value::number(-v->num());
                if (e.text == "-" && v->isVec()) return Value::vec(-v->v());
                if (e.text == "not") return Value::boolean(!v->truthyData());
                return std::nullopt;
            }
            case Expr::Kind::Binary: {
                auto a = e.lhs ? constEval(*e.lhs, scope, bs, pending) : std::nullopt;
                auto b = e.rhs ? constEval(*e.rhs, scope, bs, pending) : std::nullopt;
                if (!a || !b) return std::nullopt;
                if (a->isNumber() && b->isNumber()) {
                    double x = a->num(), y = b->num();
                    if (e.text == "+") return Value::number(x + y);
                    if (e.text == "-") return Value::number(x - y);
                    if (e.text == "*") return Value::number(x * y);
                    if (e.text == "/" && y != 0) return Value::number(x / y);
                    if (e.text == "%" && y != 0) return Value::number(std::fmod(x, y));
                }
                if (a->isVec() && b->isNumber() && e.text == "*") return Value::vec(a->v() * static_cast<float>(b->num()));
                if (a->isNumber() && b->isVec() && e.text == "*") return Value::vec(b->v() * static_cast<float>(a->num()));
                if (a->isVec() && b->isVec() && e.text == "+") return Value::vec(a->v() + b->v());
                if (a->isVec() && b->isVec() && e.text == "-") return Value::vec(a->v() - b->v());
                if (a->isString() && b->isString() && e.text == "+") return Value::string(a->str() + b->str());
                return std::nullopt;
            }
            default: return std::nullopt;
        }
    }

    // --- function state ------------------------------------------------------------
    struct Loop {
        std::vector<size_t> breaks;
        std::vector<size_t> continues;
        size_t continueTarget = SIZE_MAX;  // known for while loops (backward jump)
        size_t localsDepth = 0;
    };
    struct Fn {
        int proto = -1;
        ProtoKind kind = ProtoKind::Handler;
        const DeclScope* decl = nullptr;
        BehaviorScope* bs = nullptr;
        std::vector<std::vector<Local>> blocks;
        int freeReg = 0;
        std::vector<Loop> loops;
        TypeSet returns = kTAny;
        bool returnAnnotated = false;
        bool isEvent = false;
        int labelPc = -1;
        bool regOverflow = false;
    };
    Fn* fn_ = nullptr;

    Proto& P() { return prog_.protos[fn_->proto]; }
    size_t pc() const { return prog_.protos[fn_->proto].code.size(); }

    size_t emit(Op op, int a, int b, int c, SourceLoc loc, uint8_t x = 0) {
        Ins in;
        in.op = op;
        in.x = x;
        in.a = static_cast<uint16_t>(a);
        in.b = static_cast<uint16_t>(b);
        in.c = static_cast<uint16_t>(c);
        P().code.push_back(in);
        P().locs.push_back(loc);
        return P().code.size() - 1;
    }
    size_t emitJump(Op op, int a, SourceLoc loc) {
        size_t at = emit(op, a, 0, 0, loc);
        return at;
    }
    void patchHere(size_t at) {
        patchTo(at, pc());
        fn_->labelPc = static_cast<int>(pc());
    }
    void patchTo(size_t at, size_t target) {
        int32_t off = static_cast<int32_t>(target) - static_cast<int32_t>(at + 1);
        Ins& in = P().code[at];
        if (in.op == Op::JmpCmp) {
            if (off < -32768 || off > 32767) {
                error(P().locs[at], "too_complex", "this function is too large (a branch spans more than 32767 instructions)",
                      "split it into smaller functions");
                return;
            }
            in.c = static_cast<uint16_t>(static_cast<int16_t>(off));
            return;
        }
        in.setSbx(off);
    }
    void emitJumpTo(Op op, int a, size_t target, SourceLoc loc) {
        size_t at = emit(op, a, 0, 0, loc);
        patchTo(at, target);
    }
    void markLabel() { fn_->labelPc = static_cast<int>(pc()); }

    int allocReg(SourceLoc loc) {
        int r = fn_->freeReg++;
        if (fn_->freeReg > kMaxRegs) {
            if (!fn_->regOverflow) {
                error(loc, "too_complex", "this function needs too many registers (more than " + std::to_string(kMaxRegs) + ")",
                      "split it into smaller functions");
            }
            fn_->regOverflow = true;
            fn_->freeReg = kMaxRegs;
            return kMaxRegs - 1;
        }
        P().numRegs = std::max(P().numRegs, fn_->freeReg);
        return r;
    }
    int allocRegs(int n, SourceLoc loc) {
        int base = fn_->freeReg;
        for (int i = 0; i < n; ++i) allocReg(loc);
        return std::min(base, kMaxRegs - 1);
    }

    uint16_t constant(const Value& v, SourceLoc loc) {
        auto key = [&]() -> std::string {
            switch (v.type()) {
                case VType::Number: {
                    char buf[40];
                    std::snprintf(buf, sizeof(buf), "n%.17g", v.num());
                    return buf;
                }
                case VType::String: return "s" + v.str();
                default: return {};
            }
        }();
        if (!key.empty()) {
            if (auto it = constIndex_.find(key); it != constIndex_.end()) return it->second;
        }
        if (prog_.constants.size() >= 0x7fff) {
            error(loc, "too_complex", "too many constants in one script");
            return 0;
        }
        auto idx = static_cast<uint16_t>(prog_.constants.size());
        prog_.constants.push_back(v);
        if (!key.empty()) constIndex_[key] = idx;
        return idx;
    }
    uint16_t rk(const Value& v, SourceLoc loc) { return constant(v, loc) | kConstBit; }

    int memberRef(const std::string& name) {
        auto it = memberIndex_.find(name);
        if (it != memberIndex_.end()) return it->second;
        MemberRef m;
        m.name = name;
        m.sym = intern(name);
        using K = MemberRef::Kind;
        static const std::unordered_map<std::string, K> kinds{
            {"position", K::Position}, {"rotation", K::Rotation}, {"scale", K::Scale}, {"color", K::Color},
            {"name", K::Name},         {"id", K::Id},             {"enabled", K::Enabled}, {"tags", K::Tags},
            {"parent", K::Parent},     {"state", K::State},       {"x", K::X},         {"y", K::Y},
            {"z", K::Z},               {"r", K::R},               {"g", K::G},         {"b", K::B},
            {"a", K::A},               {"length", K::Length}};
        if (auto k = kinds.find(name); k != kinds.end()) m.kind = k->second;
        int idx = static_cast<int>(prog_.members.size());
        prog_.members.push_back(std::move(m));
        memberIndex_[name] = idx;
        return idx;
    }

    int fieldRef(const std::string& comp, const std::string& field) {
        std::string key = comp + "." + field;
        if (auto it = fieldIndex_.find(key); it != fieldIndex_.end()) return it->second;
        int idx = static_cast<int>(prog_.fields.size());
        prog_.fields.push_back({comp, field});
        fieldIndex_[key] = idx;
        return idx;
    }

    int builtinRef(const std::shared_ptr<const BuiltinDef>& def) {
        auto it = builtinIndex_.find(def.get());
        if (it != builtinIndex_.end()) return it->second;
        int idx = static_cast<int>(prog_.builtins.size());
        prog_.builtins.push_back(def);
        builtinIndex_[def.get()] = idx;
        return idx;
    }

    int hiddenBuiltin(const char* name, SourceLoc loc) {
        auto def = reg_.find(name);
        if (!def) {
            error(loc, "internal", std::string("missing internal builtin ") + name);
            return 0;
        }
        return builtinRef(def);
    }

    int methodRef(const std::string& name) {
        auto it = methodIndex_.find(name);
        if (it != methodIndex_.end()) return it->second;
        int idx = static_cast<int>(prog_.methodNames.size());
        prog_.methodNames.push_back(name);
        std::array<std::shared_ptr<const BuiltinDef>, 9> impls;
        for (uint32_t t = 0; t < 9; ++t) impls[t] = reg_.findMethod(name, static_cast<VType>(t));
        prog_.methods.push_back(std::move(impls));
        methodIndex_[name] = idx;
        return idx;
    }

    // --- locals --------------------------------------------------------------------
    Local* findLocal(const std::string& name) {
        for (auto it = fn_->blocks.rbegin(); it != fn_->blocks.rend(); ++it) {
            for (auto l = it->rbegin(); l != it->rend(); ++l) {
                if (l->name == name) return &*l;
            }
        }
        return nullptr;
    }
    std::vector<std::string> visibleNames() {
        std::vector<std::string> out{"self",          "other",         "dt",     "time",      "frame",
                                     "pi",            "state",         "state_time", "contact_point", "contact_normal",
                                     "impact",        "hit_point",     "hit_normal", "hit_distance"};
        for (const auto& b : fn_->blocks) {
            for (const auto& l : b) out.push_back(l.name);
        }
        if (fn_->bs) {
            for (const auto& [n, v] : fn_->bs->vars) out.push_back(n);
            for (const auto& [n, v] : fn_->bs->consts) out.push_back(n);
        }
        if (fn_->decl && fn_->decl->consts) {
            for (const auto& [n, v] : *fn_->decl->consts) out.push_back(n);
        }
        return out;
    }
    Local& declareLocal(const std::string& name, TypeSet type, bool isConst, bool annotated, SourceLoc loc) {
        if (fn_->bs && fn_->bs->vars.count(name)) {
            warning(loc, "shadows_var", "local " + quote(name) + " hides the behavior var " + quote(name),
                    "to change the var, assign it without `let`: " + name + " = ...");
        }
        Local l;
        l.name = name;
        l.reg = allocReg(loc);
        l.type = type;
        l.isConst = isConst;
        l.annotated = annotated;
        fn_->blocks.back().push_back(l);
        return fn_->blocks.back().back();
    }
    void pushBlock() { fn_->blocks.emplace_back(); }
    void popBlock() {
        fn_->blocks.pop_back();
        fn_->freeReg = localsEnd();
    }
    int localsEnd() const {
        int n = 0;
        for (const auto& b : fn_->blocks) n += static_cast<int>(b.size());
        return n;
    }
    // Locals occupy the low registers in declaration order; everything above is scratch.
    void freeTemps() { fn_->freeReg = localsEnd(); }

    // --- body compilation ------------------------------------------------------------
    struct FnScopeGuard {
        Compiler& c;
        Fn* saved;
        FnScopeGuard(Compiler& comp, Fn* f) : c(comp), saved(comp.fn_) { c.fn_ = f; }
        ~FnScopeGuard() { c.fn_ = saved; }
    };

    void beginBody(Fn& f) {
        f.blocks.emplace_back();
        f.freeReg = 0;
    }

    void endBody(SourceLoc loc) {
        if (fn_->kind == ProtoKind::Function) {
            emit(Op::RetNone, 0, 0, 0, loc);
        } else {
            emit(Op::Stop, 0, 0, 0, loc);
        }
    }

    void compileFn(const FnDecl& f, int fnIndex, const DeclScope* decl, BehaviorScope* bs) {
        FnInfo& info = prog_.functions[fnIndex];
        Fn st;
        st.proto = info.proto;
        st.kind = ProtoKind::Function;
        st.decl = decl;
        st.bs = bs;
        st.returns = info.returns;
        st.returnAnnotated = !f.returns.empty();
        FnScopeGuard g(*this, &st);
        beginBody(st);
        for (size_t i = 0; i < f.params.size(); ++i) {
            Local l;
            l.name = f.params[i].name;
            l.reg = allocReg(f.params[i].loc);
            l.type = info.paramTypes[i];
            l.annotated = !f.params[i].type.empty();
            st.blocks.back().push_back(l);
        }
        block(f.body);
        endBody(f.loc);
        P().numRegs = std::max(P().numRegs, static_cast<int>(f.params.size()));
    }

    int newProto(std::string name, ProtoKind kind, int behavior, int state) {
        Proto p;
        p.name = std::move(name);
        p.kind = kind;
        p.behavior = behavior;
        p.state = state;
        p.file = file_;
        prog_.protos.push_back(std::move(p));
        return static_cast<int>(prog_.protos.size() - 1);
    }

    void compileBehavior(BehaviorScope& bs) {
        const BehaviorDef& b = *bs.def;
        BehaviorInfo& info = prog_.behaviors[bs.index];
        timerCounter_ = 0;
        // Var initializers.
        for (const auto& v : b.vars) {
            auto it = bs.vars.find(v.name);
            if (it == bs.vars.end() || !v.initial) continue;
            int proto = newProto(b.name + ".var " + v.name, ProtoKind::VarInit, bs.index, -1);
            Fn st;
            st.proto = proto;
            st.kind = ProtoKind::VarInit;
            st.decl = &mainDecl_;
            st.bs = &bs;
            FnScopeGuard g(*this, &st);
            beginBody(st);
            int r = allocReg(v.loc);
            TypeSet t = exprTo(*v.initial, r);
            emit(Op::Ret, r, 0, 0, v.loc);
            VarInfo& vi = prog_.behaviors[bs.index].vars[it->second];
            vi.init = proto;
            if (v.type.empty() && vi.defaultValue.isNull() && !(t & kTNone)) vi.type = t;
            if (!v.type.empty() && !(vi.type & t)) {
                error(v.initial->loc, "type_mismatch", "var " + quote(v.name) + " is declared " + typeSetName(vi.type) +
                                                            " but starts as " + typeSetName(t));
            }
        }
        for (const auto& f : b.fns) compileFn(f, bs.fns.at(f.name), &mainDecl_, &bs);
        for (const auto& h : b.handlers) compileHandler(h, bs, -1);
        for (size_t si = 0; si < b.states.size(); ++si) {
            for (const auto& h : b.states[si].handlers) compileHandler(h, bs, static_cast<int>(si));
        }
        for (const auto& t : b.tests) compileTest(t, &mainDecl_, &bs);
        info.timerCount = timerCounter_;
        (void)info;
    }

    void compileHandler(const Handler& h, BehaviorScope& bs, int state) {
        BehaviorInfo& info = prog_.behaviors[bs.index];
        if ((h.trigger == Trigger::Enter || h.trigger == Trigger::Exit) && state < 0) {
            error(h.loc, "enter_outside_state", std::string("'on ") + toString(h.trigger) + "' belongs inside a state block",
                  "state Idle\n  on enter ... end\nend");
        }
        if (h.custom) {
            if (!reg_.trigger(h.argument)) {
                std::vector<std::string> names{"start", "tick", "event", "key", "click", "enter", "exit"};
                for (const auto& t : reg_.triggers()) names.push_back(t.name);
                std::string guess = str::closest(h.argument, names);
                error(h.loc, "unknown_trigger", "unknown trigger " + quote(h.argument),
                      guess.empty() ? "triggers: start, tick, event \"name\", key \"name\", click (and enter/exit in states)"
                                    : "did you mean " + quote(guess) + "?");
            }
        }
        HandlerInfo hi;
        hi.trigger = h.trigger;
        hi.argument = h.argument;
        hi.custom = h.custom;
        hi.sym = h.argument.empty() ? 0 : intern(h.argument);
        hi.state = state;
        hi.loc = h.loc;
        std::string label = std::string("on ") + (h.custom ? h.argument : toString(h.trigger));
        if (!h.custom && !h.argument.empty()) label += " \"" + h.argument + "\"";
        std::string stateName = state >= 0 ? bs.def->states[state].name + "." : "";
        hi.proto = newProto(bs.def->name + "." + stateName + label, ProtoKind::Handler, bs.index, state);
        int handlerIndex = static_cast<int>(info.handlers.size());
        if (state >= 0) {
            StateInfo& si = info.states[state];
            if (h.trigger == Trigger::Enter) {
                if (si.enter >= 0) error(h.loc, "duplicate_handler", "state " + quote(si.name) + " has two 'on enter' handlers");
                si.enter = handlerIndex;
            } else if (h.trigger == Trigger::Exit) {
                if (si.exit >= 0) error(h.loc, "duplicate_handler", "state " + quote(si.name) + " has two 'on exit' handlers");
                si.exit = handlerIndex;
            }
        }
        info.handlers.push_back(hi);

        Fn st;
        st.proto = hi.proto;
        st.kind = ProtoKind::Handler;
        st.decl = &mainDecl_;
        st.bs = &bs;
        st.isEvent = h.trigger == Trigger::Event;
        FnScopeGuard g(*this, &st);
        beginBody(st);
        if (st.isEvent) {
            Local l;
            l.name = h.binding.empty() ? "data" : h.binding;
            l.reg = allocReg(h.loc);
            st.blocks.back().push_back(l);
            P().numParams = 1;
        }
        block(h.body);
        endBody(h.loc);
    }

    void compileTest(const TestDecl& t, const DeclScope* decl, BehaviorScope* bs) {
        TestInfo ti;
        ti.name = t.name;
        ti.behavior = bs ? bs->index : -1;
        ti.loc = t.loc;
        ti.proto = newProto("test \"" + t.name + "\"", ProtoKind::Test, ti.behavior, -1);
        Fn st;
        st.proto = ti.proto;
        st.kind = ProtoKind::Test;
        st.decl = decl;
        st.bs = bs;
        FnScopeGuard g(*this, &st);
        beginBody(st);
        block(t.body);
        endBody(t.loc);
        if (bs) {
            for (const auto& other : prog_.behaviors[bs->index].tests) {
                if (other.name == t.name) error(t.loc, "duplicate_name", "two tests are named \"" + t.name + "\"");
            }
            prog_.behaviors[bs->index].tests.push_back(ti);
        } else {
            prog_.fileTests.push_back(ti);
        }
    }

    // --- statements ------------------------------------------------------------------
    void block(const Block& b) {
        pushBlock();
        // Scratch registers of the enclosing statement's header (a loop bound such as `counts[k]`, an
        // if condition) are dead once the body starts. Release them, so locals declared in the body
        // take the next local slot: otherwise a local could sit above localsEnd() and the next
        // statement's temporaries would overwrite it.
        freeTemps();
        bool terminated = false;
        for (const auto& s : b) {
            if (terminated) {
                warning(s->loc, "unreachable_code", "this statement can never run (it follows return/stop/break/go to)");
                terminated = false;  // warn once per block
            }
            statement(*s);
            freeTemps();
            auto k = s->kind;
            if (k == Stmt::Kind::Return || k == Stmt::Kind::Stop || k == Stmt::Kind::Break ||
                k == Stmt::Kind::Continue || k == Stmt::Kind::GoTo) {
                terminated = true;
            }
        }
        popBlock();
        freeTemps();
    }

    bool inHandlerLike() const { return fn_->kind == ProtoKind::Handler; }

    void statement(const Stmt& s) {
        switch (s.kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const: {
                if (s.name.empty() || !s.value) return;
                TypeSet declared = annotation(s.type);
                // Evaluate before declaring: `let x = x + 1` reads the outer x.
                int r = allocReg(s.loc);
                TypeSet t = exprTo(*s.value, r);
                if (!s.type.empty() && !(declared & t)) {
                    error(s.value->loc, "type_mismatch", quote(s.name) + " is declared " + typeSetName(declared) +
                                                             " but the value is " + typeSetName(t));
                }
                if (Local* existing = findLocalInCurrentBlock(s.name)) {
                    // Forgiving: a second `let x` in the same block re-assigns x.
                    warning(s.loc, "redeclared", quote(s.name) + " is already declared in this block; this assigns it",
                            "drop the `let`: " + s.name + " = ...");
                    existing->type |= t;
                    existing->isConst = existing->isConst || s.kind == Stmt::Kind::Const;
                    moveInto(existing->reg, r, s.loc);
                    return;
                }
                // The value's register becomes the local's register.
                freeTo(r);
                declareLocal(s.name, s.type.empty() ? t : declared, s.kind == Stmt::Kind::Const, !s.type.empty(), s.loc);
                return;
            }
            case Stmt::Kind::Assign: {
                if (!s.target || !s.value) return;
                assign(*s.target, *s.value, s.loc);
                return;
            }
            case Stmt::Kind::OpAssign: {
                if (!s.target || !s.value) return;
                Operand cur = operand(*s.target);
                int curReg = toReg(cur, s.loc);
                Operand rhs = operand(*s.value);
                int out = allocReg(s.loc);
                TypeSet t = binaryOp(s.name, Operand{static_cast<uint16_t>(curReg), cur.type}, rhs, out, s.loc);
                assignOperand(*s.target, Operand{static_cast<uint16_t>(out), t}, s.loc);
                return;
            }
            case Stmt::Kind::If: {
                std::vector<size_t> ends;
                for (size_t i = 0; i < s.branches.size(); ++i) {
                    const auto& [cond, body] = s.branches[i];
                    if (!cond) {
                        block(body);
                        break;
                    }
                    Jumps skip = condJump(*cond, /*jumpIfTrue=*/false);
                    block(body);
                    bool last = i + 1 == s.branches.size();
                    if (!last) ends.push_back(emitJump(Op::Jmp, 0, s.loc));
                    patchAllHere(skip);
                }
                for (size_t e : ends) patchHere(e);
                return;
            }
            case Stmt::Kind::While: {
                if (!s.value) return;
                size_t top = pc();
                markLabel();
                Jumps exit = condJump(*s.value, false);
                fn_->loops.push_back({});
                fn_->loops.back().continueTarget = top;
                block(s.body);
                emitJumpTo(Op::Jmp, 0, top, s.loc);
                patchAllHere(exit);
                finishLoop(SIZE_MAX);
                return;
            }
            case Stmt::Kind::Repeat: {
                if (!s.value) return;
                pushBlock();
                int base = allocRegs(4, s.loc);
                reserveLoopRegs(base, 4);
                emit(Op::LoadK, base, constant(Value::number(0), s.loc), 0, s.loc);
                TypeSet t = exprTo(*s.value, base + 1);
                requireType(t, kTNumber, s.value->loc, "the repeat count");
                emit(Op::LoadK, base + 2, constant(Value::number(1), s.loc), 0, s.loc);
                numericLoop(base, false, s.body, s.loc);
                popBlock();
                return;
            }
            case Stmt::Kind::For: {
                if (!s.value || s.name.empty()) return;
                pushBlock();
                int base = allocRegs(4, s.loc);
                reserveLoopRegs(base, 4);
                if (s.isRange) {
                    if (!s.name2.empty()) error(s.loc, "invalid_for", "a range loop has one variable: for i in 0..10");
                    TypeSet a = exprTo(*s.value, base);
                    requireType(a, kTNumber, s.value->loc, "the range start");
                    if (s.extra) requireType(exprTo(*s.extra, base + 1), kTNumber, s.extra->loc, "the range end");
                    if (s.extra2) {
                        requireType(exprTo(*s.extra2, base + 2), kTNumber, s.extra2->loc, "the step");
                    } else {
                        emit(Op::LoadK, base + 2, constant(Value::number(1), s.loc), 0, s.loc);
                    }
                    numericLoop(base, s.inclusive, s.body, s.loc, &s.name);
                } else {
                    TypeSet t = exprTo(*s.value, base);
                    if (!(t & (kTList | kTMap | kTString))) {
                        error(s.value->loc, "type_mismatch", "cannot loop over a " + typeSetName(t),
                              "loop over a list, map or string, or a range like 0..10");
                    }
                    iterLoop(base, s, t);
                }
                popBlock();
                return;
            }
            case Stmt::Kind::Every:
            case Stmt::Kind::After: {
                if (!s.value) return;
                if (!inHandlerLike()) {
                    error(s.loc, "timer_outside_handler", std::string(s.kind == Stmt::Kind::Every ? "every" : "after") +
                                                              " can only be used in handlers (on ...)",
                          "in a fn or test, use wait");
                }
                Operand interval = operand(*s.value);
                requireType(interval.type, kTNumber, s.value->loc, "the interval");
                int flag = allocReg(s.loc);
                int slot = timerCounter_++;
                emit(s.kind == Stmt::Kind::Every ? Op::Every : Op::After, flag, interval.rk, slot, s.loc);
                size_t skip = emitJump(Op::JmpIfNot, flag, s.loc);
                freeTemps();
                block(s.body);
                patchHere(skip);
                return;
            }
            case Stmt::Kind::Wait: {
                if (!s.value) return;
                if (fn_->kind == ProtoKind::Function || fn_->kind == ProtoKind::VarInit) {
                    error(s.loc, "wait_outside_handler", "wait can only be used directly in handlers and tests, not in fn",
                          "move the waiting part into the handler that calls this fn");
                    return;
                }
                P().canWait = true;
                if (s.waitKind == WaitKind::Until) {
                    size_t top = pc();
                    markLabel();
                    Jumps done = condJump(*s.value, true);
                    emit(Op::Wait, 0, rk(Value::number(1), s.loc), 0, s.loc, 1);
                    emitJumpTo(Op::Jmp, 0, top, s.loc);
                    patchAllHere(done);
                    return;
                }
                Operand amount = operand(*s.value);
                requireType(amount.type, kTNumber, s.value->loc, s.waitKind == WaitKind::Frames ? "the frame count" : "the wait time");
                emit(Op::Wait, 0, amount.rk, 0, s.loc, s.waitKind == WaitKind::Frames ? 1 : 0);
                return;
            }
            case Stmt::Kind::Break:
            case Stmt::Kind::Continue: {
                if (fn_->loops.empty()) {
                    error(s.loc, "outside_loop", std::string(s.kind == Stmt::Kind::Break ? "break" : "continue") +
                                                     " can only be used inside a loop");
                    return;
                }
                Loop& loop = fn_->loops.back();
                if (s.kind == Stmt::Kind::Continue && loop.continueTarget != SIZE_MAX) {
                    emitJumpTo(Op::Jmp, 0, loop.continueTarget, s.loc);
                } else {
                    size_t j = emitJump(Op::Jmp, 0, s.loc);
                    (s.kind == Stmt::Kind::Break ? loop.breaks : loop.continues).push_back(j);
                }
                return;
            }
            case Stmt::Kind::Return: {
                if (fn_->kind != ProtoKind::Function) {
                    if (s.value) {
                        error(s.loc, "return_value_in_handler", "handlers and tests cannot return a value",
                              "use `stop` (or a bare `return`) to leave the handler");
                    }
                    emit(Op::Stop, 0, 0, 0, s.loc);
                    return;
                }
                if (!s.value) {
                    if (fn_->returnAnnotated && !(fn_->returns & kTNone)) {
                        error(s.loc, "type_mismatch", "this fn must return " + typeSetName(fn_->returns));
                    }
                    emit(Op::RetNone, 0, 0, 0, s.loc);
                    return;
                }
                int r = allocReg(s.loc);
                TypeSet t = exprTo(*s.value, r);
                if (fn_->returnAnnotated && !(fn_->returns & t)) {
                    error(s.value->loc, "type_mismatch", "this fn returns " + typeSetName(fn_->returns) + " but the value is " +
                                                             typeSetName(t));
                }
                emit(Op::Ret, r, 0, 0, s.loc);
                return;
            }
            case Stmt::Kind::Stop: emit(Op::Stop, 0, 0, 0, s.loc); return;
            case Stmt::Kind::GoTo: {
                if (!fn_->bs) {
                    error(s.loc, "goto_outside_behavior", "go to can only be used inside a behavior with states");
                    return;
                }
                if (fn_->kind == ProtoKind::Test || fn_->kind == ProtoKind::VarInit) {
                    error(s.loc, "goto_in_test", "go to can only be used in handlers and fns",
                          "in a test, emit an event the behavior reacts to");
                    return;
                }
                auto it = fn_->bs->states.find(s.name);
                if (it == fn_->bs->states.end()) {
                    std::vector<std::string> names;
                    for (const auto& [n, i] : fn_->bs->states) names.push_back(n);
                    error(s.loc, "unknown_state", "behavior " + quote(fn_->bs->def->name) + " has no state " + quote(s.name),
                          names.empty() ? "declare states with `state Name ... end` inside the behavior"
                                        : didYouMean(s.name, names));
                    return;
                }
                emit(Op::GoTo, 0, it->second, 0, s.loc);
                return;
            }
            case Stmt::Kind::Move: statementCall("__move_by", {s.target.get(), s.value.get()}, s.loc); return;
            case Stmt::Kind::MoveToward:
                statementCall("__move_toward", {s.target.get(), s.value.get(), s.extra.get()}, s.loc);
                return;
            case Stmt::Kind::Rotate: statementCall("__rotate", {s.target.get(), s.value.get()}, s.loc); return;
            case Stmt::Kind::Look: statementCall("__look", {s.target.get(), s.value.get()}, s.loc); return;
            case Stmt::Kind::Destroy: statementCall("__destroy", {s.target.get()}, s.loc); return;
            case Stmt::Kind::Log: statementCall("__log", {s.value.get()}, s.loc); return;
            case Stmt::Kind::Emit: {
                Expr name(Expr::Kind::String, s.loc);
                name.text = s.name;
                Expr none(Expr::Kind::None, s.loc);
                const Expr* payload = s.value ? s.value.get() : &none;
                if (s.extra) {
                    statementCall("__emit_to", {&name, payload, s.extra.get()}, s.loc);
                } else {
                    statementCall("__emit", {&name, payload}, s.loc);
                }
                return;
            }
            case Stmt::Kind::Call: {
                if (!s.value) return;
                int r = allocReg(s.loc);
                exprTo(*s.value, r);
                return;
            }
            case Stmt::Kind::Expect: {
                if (fn_->kind != ProtoKind::Test) {
                    error(s.loc, "test_only", "expect can only be used inside test blocks");
                    return;
                }
                if (!s.value) return;
                std::string text = formatExpr(*s.value);
                if (s.extra) {
                    if (s.extra->kind != Expr::Kind::String) {
                        error(s.extra->loc, "expected_string", "the expect message must be a plain string");
                    } else {
                        text = s.extra->text + " (" + text + ")";
                    }
                }
                const Expr& e = *s.value;
                if (e.kind == Expr::Kind::Binary && isComparison(e.text) && e.lhs && e.rhs) {
                    int base = allocRegs(2, s.loc);
                    TypeSet ta = exprTo(*e.lhs, base);
                    TypeSet tb = exprTo(*e.rhs, base + 1);
                    int cond = allocReg(s.loc);
                    binaryOp(e.text, Operand{static_cast<uint16_t>(base), ta}, Operand{static_cast<uint16_t>(base + 1), tb},
                             cond, e.loc);
                    emit(Op::Expect, cond, constant(Value::string(text), s.loc), base, s.loc, 1);
                } else {
                    int cond = allocReg(s.loc);
                    exprTo(e, cond);
                    emit(Op::Expect, cond, constant(Value::string(text), s.loc), 0, s.loc, 0);
                }
                return;
            }
            case Stmt::Kind::Press:
            case Stmt::Kind::Hold:
            case Stmt::Kind::Release:
            case Stmt::Kind::Click: {
                static const char* names[] = {"__press", "__hold", "__release", "__click"};
                int i = s.kind == Stmt::Kind::Press ? 0 : s.kind == Stmt::Kind::Hold ? 1 : s.kind == Stmt::Kind::Release ? 2 : 3;
                if (fn_->kind != ProtoKind::Test) {
                    error(s.loc, "test_only", std::string(names[i] + 2) + " can only be used inside test blocks",
                          "in game code, read input with key(\"w\") or `on key \"space\"`");
                    return;
                }
                statementCall(names[i], {s.value.get()}, s.loc);
                return;
            }
        }
    }

    Local* findLocalInCurrentBlock(const std::string& name) {
        for (auto& l : fn_->blocks.back()) {
            if (l.name == name) return &l;
        }
        return nullptr;
    }

    void freeTo(int r) { fn_->freeReg = r; }

    void requireType(TypeSet got, TypeSet want, SourceLoc loc, const std::string& what) {
        if (!(got & want)) {
            error(loc, "type_mismatch", what + " must be " + typeSetName(want) + ", got " + typeSetName(got));
        }
    }

    // Loop registers stay allocated (they are not named locals, so pin them as hidden locals).
    void reserveLoopRegs(int base, int n) {
        for (int i = 0; i < n; ++i) {
            Local l;
            l.name = "";  // hidden
            l.reg = base + i;
            fn_->blocks.back().push_back(l);
        }
    }

    Local& hiddenLocal(int reg) {
        for (auto& l : fn_->blocks.back()) {
            if (l.reg == reg) return l;
        }
        return fn_->blocks.back().back();
    }

    void numericLoop(int base, bool inclusive, const Block& body, SourceLoc loc, const std::string* var = nullptr) {
        size_t prep = emit(Op::ForPrep, base, 0, 0, loc, inclusive ? 1 : 0);
        size_t top = pc();
        markLabel();
        fn_->loops.push_back({});
        if (var) {
            // The loop variable is the 4th (hidden) loop register.
            Local& l = hiddenLocal(base + 3);
            l.name = *var;
            l.type = kTNumber;
        }
        block(body);
        size_t cont = pc();
        markLabel();
        size_t loop = emit(Op::ForLoop, base, 0, 0, loc, inclusive ? 1 : 0);
        patchTo(loop, top);
        patchHere(prep);
        finishLoop(cont);
    }

    void iterLoop(int base, const Stmt& s, TypeSet collType) {
        bool two = !s.name2.empty();
        size_t prep = emit(Op::IterPrep, base, 0, 0, s.loc, two ? 1 : 0);
        size_t top = pc();
        markLabel();
        fn_->loops.push_back({});
        TypeSet first = kTAny, second = kTAny;
        if (collType == kTString) first = kTString;
        if (collType == kTMap) first = kTString;
        if (two && collType == kTList) first = kTNumber;
        Local& a = hiddenLocal(base + 2);
        a.name = s.name;
        a.type = first;
        if (two) {
            Local& b = hiddenLocal(base + 3);
            b.name = s.name2;
            b.type = second;
        }
        block(s.body);
        size_t cont = pc();
        markLabel();
        size_t next = emit(Op::IterNext, base, 0, 0, s.loc, two ? 1 : 0);
        patchTo(next, top);
        patchHere(prep);
        finishLoop(cont);
    }

    void finishLoop(size_t continueTarget) {
        Loop loop = std::move(fn_->loops.back());
        fn_->loops.pop_back();
        for (size_t j : loop.continues) patchTo(j, continueTarget);
        for (size_t j : loop.breaks) patchHere(j);
    }

    void statementCall(const char* builtin, std::initializer_list<const Expr*> args, SourceLoc loc) {
        for (const Expr* a : args) {
            if (!a) return;  // parse error already reported
        }
        int b = hiddenBuiltin(builtin, loc);
        const BuiltinDef& def = *prog_.builtins[b];
        int base = allocRegs(std::max<int>(1, static_cast<int>(args.size())), loc);
        int i = 0;
        for (const Expr* a : args) {
            TypeSet t = exprTo(*a, base + i);
            if (i < static_cast<int>(def.params.size()) && !(def.params[i].type & t)) {
                error(a->loc, "type_mismatch", describeStatementArg(builtin, i) + " must be " +
                                                   typeSetName(def.params[i].type) + ", got " + typeSetName(t));
            }
            ++i;
        }
        emit(Op::Call, base, b, static_cast<int>(args.size()), loc);
    }

    static std::string describeStatementArg(const char* builtin, int i) {
        std::string n = builtin;
        if (n == "__move_by") return i == 0 ? "the entity to move" : "the move offset";
        if (n == "__move_toward") return i == 0 ? "the entity to move" : i == 1 ? "the destination" : "the speed";
        if (n == "__rotate") return i == 0 ? "the entity to rotate" : "the rotation (degrees vector)";
        if (n == "__look") return i == 0 ? "the entity to turn" : "the point to look at";
        if (n == "__destroy") return "the entity to destroy";
        if (n == "__emit_to") return i == 2 ? "the event receiver" : "the event payload";
        return "argument " + std::to_string(i + 1);
    }

    // --- assignment ------------------------------------------------------------------
    // Builds the chain for an assignable expression; returns false (with a diagnostic)
    // when the expression cannot be assigned.
    bool buildChain(const Expr& target, Chain& ch) {
        std::vector<const Expr*> path;
        const Expr* cur = &target;
        while (cur->kind == Expr::Kind::Member || cur->kind == Expr::Kind::Index) {
            path.push_back(cur);
            cur = cur->lhs.get();
            if (!cur) return false;
        }
        std::reverse(path.begin(), path.end());
        ch.baseExpr = cur;
        size_t start = 0;
        if (cur->kind == Expr::Kind::Ident) {
            const std::string& n = cur->text;
            if (Local* l = findLocal(n)) {
                ch.base = Chain::Base::Local;
                ch.localReg = l->reg;
                ch.baseType = l->type;
                if (path.empty() && l->isConst) {
                    error(target.loc, "const_assign", quote(n) + " is a constant and cannot be changed");
                    return false;
                }
            } else if (int v = varIndex(n); v >= 0) {
                ch.base = Chain::Base::Var;
                ch.var = v;
                ch.baseType = varType(v);
            } else if (n == "self" && !path.empty() && path[0]->kind == Expr::Kind::Member && varIndex(path[0]->text) >= 0) {
                ch.base = Chain::Base::Var;
                ch.var = varIndex(path[0]->text);
                ch.baseType = varType(ch.var);
                start = 1;
            } else if (path.empty()) {
                if (n == "self" || n == "other" || n == "dt" || n == "time" || n == "frame" || n == "pi" ||
                    n == "state" || n == "state_time") {
                    error(target.loc, "readonly", quote(n) + " cannot be assigned");
                } else if (isConstName(n)) {
                    error(target.loc, "const_assign", quote(n) + " is a constant and cannot be changed");
                } else if (fn_->decl && fn_->decl->uses && fn_->decl->uses->count(n)) {
                    error(target.loc, "not_assignable", quote(n) + " is a module");
                } else {
                    std::string guess = str::closest(n, visibleNames());
                    error(target.loc, "unknown_name", "unknown variable " + quote(n),
                          guess.empty() ? (fn_->bs ? "declare it with `let " + n + " = ...` (local) or `var " + n +
                                                         " = ...` at the top of the behavior (per entity)"
                                                   : "declare it with `let " + n + " = ...`")
                                        : "did you mean " + quote(guess) + "?");
                }
                return false;
            }
        }
        if (ch.base == Chain::Base::Value && path.empty()) {
            error(target.loc, "not_assignable",
                  "the left side of an assignment must be a variable, a property (self.position) or an element (list[0])");
            return false;
        }
        for (size_t i = start; i < path.size(); ++i) {
            const Expr* p = path[i];
            Step st;
            st.loc = p->loc;
            if (p->kind == Expr::Kind::Index) {
                st.kind = Step::Kind::Index;
                st.index = p->rhs.get();
                if (!st.index) return false;
            } else if (i + 1 < path.size() && path[i + 1]->kind == Expr::Kind::Member && components_.count(p->text) &&
                       stepBaseCanBeEntity(ch, i, start)) {
                st.kind = Step::Kind::Field;
                st.member = fieldRef(p->text, path[i + 1]->text);
                st.type = fieldType(p->text, path[i + 1]->text, path[i + 1]->loc);
                ++i;
            } else {
                st.kind = Step::Kind::Member;
                st.member = memberRef(p->text);
            }
            ch.steps.push_back(st);
        }
        if (ch.base == Chain::Base::Value && cur->kind == Expr::Kind::Ident && (cur->text == "self" || cur->text == "other")) {
            ch.baseType = cur->text == "self" ? kTEntity : (kTEntity | kTNone);
        }
        return true;
    }

    bool stepBaseCanBeEntity(const Chain&, size_t, size_t) const { return true; }

    int varIndex(const std::string& name) const {
        if (!fn_->bs) return -1;
        auto it = fn_->bs->vars.find(name);
        return it == fn_->bs->vars.end() ? -1 : it->second;
    }
    TypeSet varType(int v) const { return prog_.behaviors[fn_->bs->index].vars[v].type; }

    bool isConstName(const std::string& n) const {
        if (fn_->bs && fn_->bs->consts.count(n)) return true;
        return fn_->decl && fn_->decl->consts && fn_->decl->consts->count(n);
    }

    // Loads the chain values regs[0..upto] (upto = number of steps applied).
    void loadChain(Chain& ch, size_t upto, SourceLoc loc) {
        ch.regs.clear();
        int r0;
        TypeSet t;
        if (ch.base == Chain::Base::Local) {
            r0 = ch.localReg;
            t = ch.baseType;
        } else if (ch.base == Chain::Base::Var) {
            r0 = allocReg(loc);
            emit(Op::GetVar, r0, ch.var, 0, loc);
            t = ch.baseType;
        } else {
            r0 = allocReg(loc);
            t = exprTo(*ch.baseExpr, r0);
            ch.baseType = t;
        }
        ch.regs.push_back(r0);
        TypeSet curType = t;
        for (size_t i = 0; i < upto; ++i) {
            Step& st = ch.steps[i];
            int r = allocReg(st.loc);
            switch (st.kind) {
                case Step::Kind::Member:
                    st.type = memberType(curType, prog_.members[st.member], st.loc);
                    emit(Op::GetMember, r, ch.regs.back(), st.member, st.loc);
                    break;
                case Step::Kind::Field: emit(Op::GetField, r, ch.regs.back(), st.member, st.loc); break;
                case Step::Kind::Index: {
                    Operand ix = operand(*st.index);
                    st.type = indexType(curType, ix.type, st.loc);
                    emit(Op::Index, r, ch.regs.back(), ix.rk, st.loc);
                    break;
                }
            }
            curType = st.type;
            ch.regs.push_back(r);
        }
    }

    // Stores `valueRk` into step `i` of regs[i], then writes regs[i] back up the chain.
    void storeChain(Chain& ch, size_t i, uint16_t valueRk, SourceLoc loc) {
        while (true) {
            Step& st = ch.steps[i];
            int obj = ch.regs[i];
            switch (st.kind) {
                case Step::Kind::Member: emit(Op::SetMember, obj, st.member, valueRk, st.loc); break;
                case Step::Kind::Field: emit(Op::SetField, obj, st.member, valueRk, st.loc); break;
                case Step::Kind::Index: {
                    Operand ix = operand(*st.index);
                    emit(Op::SetIndex, obj, ix.rk, valueRk, st.loc);
                    break;
                }
            }
            TypeSet objType = i == 0 ? ch.baseType : ch.steps[i - 1].type;
            if (objType == kTEntity) return;  // entities write through; nothing above changes
            if (i == 0) break;
            --i;
            valueRk = static_cast<uint16_t>(obj);
        }
        // The base itself.
        if (ch.base == Chain::Base::Var) emit(Op::SetVar, ch.var, ch.regs[0], 0, loc);
        // Local: modified in place. Value base: an entity (written through) or a temporary.
    }

    void assign(const Expr& target, const Expr& value, SourceLoc loc) {
        // Plain local: evaluate straight into its register when safe.
        if (target.kind == Expr::Kind::Ident) {
            if (Local* l = findLocal(target.text)) {
                if (l->isConst) {
                    error(target.loc, "const_assign", quote(target.text) + " is a constant and cannot be changed");
                    return;
                }
                int tmp = allocReg(loc);
                TypeSet t = exprTo(value, tmp);
                checkAssignType(*l, t, value.loc);
                moveInto(l->reg, tmp, loc);
                return;
            }
        }
        Operand v = operand(value);
        assignOperand(target, v, loc);
    }

    void checkAssignType(Local& l, TypeSet t, SourceLoc loc) {
        if (l.annotated) {
            if (!(l.type & t)) {
                error(loc, "type_mismatch", quote(l.name) + " is declared " + typeSetName(l.type) + " but the value is " +
                                                typeSetName(t));
            }
        } else {
            l.type |= t;  // widen
        }
    }

    // Retargets the last instruction (which wrote `tmp`) to `dst` when nothing jumps between.
    void moveInto(int dst, int tmp, SourceLoc loc) {
        if (dst == tmp) return;
        if (!P().code.empty() && fn_->labelPc != static_cast<int>(pc())) {
            Ins& last = P().code.back();
            switch (last.op) {
                case Op::Move:
                case Op::LoadK:
                case Op::LoadNone:
                case Op::LoadBool:
                case Op::LoadSelf:
                case Op::LoadEnv:
                case Op::GetVar:
                case Op::GetMember:
                case Op::GetField:
                case Op::Index:
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                case Op::Neg:
                case Op::Eq:
                case Op::Ne:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge:
                case Op::In:
                case Op::NewList:
                case Op::NewMap:
                case Op::MakeVec:
                case Op::Concat:
                    if (last.a == tmp) {
                        last.a = static_cast<uint16_t>(dst);
                        return;
                    }
                    break;
                case Op::Not:
                    if (last.a == tmp) {
                        last.a = static_cast<uint16_t>(dst);
                        return;
                    }
                    break;
                default: break;
            }
        }
        emit(Op::Move, dst, tmp, 0, loc);
    }

    void assignOperand(const Expr& target, Operand v, SourceLoc loc) {
        if (target.kind == Expr::Kind::Ident) {
            if (Local* l = findLocal(target.text)) {
                if (l->isConst) {
                    error(target.loc, "const_assign", quote(target.text) + " is a constant and cannot be changed");
                    return;
                }
                checkAssignType(*l, v.type, target.loc);
                if (isK(v.rk)) {
                    emit(Op::LoadK, l->reg, v.rk & 0x7fff, 0, loc);
                } else if (v.rk != l->reg) {
                    emit(Op::Move, l->reg, v.rk, 0, loc);
                }
                return;
            }
        }
        Chain ch;
        if (!buildChain(target, ch)) return;
        if (ch.steps.empty()) {
            if (ch.base == Chain::Base::Var) {
                VarInfo& vi = prog_.behaviors[fn_->bs->index].vars[ch.var];
                if (!(vi.type & v.type)) {
                    error(target.loc, "type_mismatch", "var " + quote(vi.name) + " holds " + typeSetName(vi.type) +
                                                           " but the value is " + typeSetName(v.type),
                          "declare it with a wider type, e.g. var " + vi.name + ": any = ...");
                }
                emit(Op::SetVar, ch.var, v.rk, 0, loc);
            }
            return;
        }
        // Make sure the value survives evaluating the chain (it may live in a scratch reg).
        loadChain(ch, ch.steps.size() - 1, loc);
        const Step& last = ch.steps.back();
        if (last.kind == Step::Kind::Field) {
            TypeSet ft = last.type;
            if (!(ft & v.type)) {
                const FieldRef& f = prog_.fields[last.member];
                error(target.loc, "type_mismatch", f.component + "." + f.field + " is a " + typeSetName(ft) +
                                                       " but the value is " + typeSetName(v.type));
            }
        } else if (last.kind == Step::Kind::Member) {
            TypeSet objType = ch.steps.size() >= 2 ? ch.steps[ch.steps.size() - 2].type : ch.baseType;
            checkMemberWrite(objType, prog_.members[last.member], v.type, last.loc);
        }
        if (ch.steps.size() == 1 && last.kind == Step::Kind::Member && ch.base == Chain::Base::Value && ch.baseExpr &&
            ch.baseExpr->kind == Expr::Kind::Ident && ch.baseExpr->text == "self" &&
            prog_.members[last.member].kind == MemberRef::Kind::Named) {
            warnUndeclaredSelf(prog_.members[last.member].name, target.loc, true);
        }
        storeChain(ch, ch.steps.size() - 1, v.rk, loc);
    }

    // --- expressions -----------------------------------------------------------------
    int toReg(Operand o, SourceLoc loc) {
        if (!isK(o.rk)) return o.rk;
        int r = allocReg(loc);
        emit(Op::LoadK, r, o.rk & 0x7fff, 0, loc);
        return r;
    }

    static bool containsMethodCall(const Expr* e) {
        if (!e) return false;
        if (e->kind == Expr::Kind::MethodCall) return true;
        if (containsMethodCall(e->lhs.get()) || containsMethodCall(e->rhs.get())) return true;
        for (const auto& a : e->args) {
            if (containsMethodCall(a.get())) return true;
        }
        return false;
    }

    // Constants become K operands, locals their own register, everything else a scratch register.
    Operand operand(const Expr& e, bool copyLocals = false) {
        switch (e.kind) {
            case Expr::Kind::Number: return {rk(Value::number(e.number), e.loc), kTNumber};
            case Expr::Kind::String: return {rk(Value::string(e.text), e.loc), kTString};
            case Expr::Kind::Ident: {
                if (Local* l = findLocal(e.text); l && !copyLocals) return {static_cast<uint16_t>(l->reg), l->type};
                if (!findLocal(e.text)) {
                    if (auto c = resolveConst(e.text)) return {rk(c->value, e.loc), c->type};
                }
                break;
            }
            default: break;
        }
        int r = allocReg(e.loc);
        TypeSet t = exprTo(e, r);
        return {static_cast<uint16_t>(r), t};
    }

    const ConstSym* resolveConst(const std::string& n) const {
        if (fn_->bs) {
            if (auto it = fn_->bs->consts.find(n); it != fn_->bs->consts.end()) return &it->second;
        }
        if (fn_->decl && fn_->decl->consts) {
            if (auto it = fn_->decl->consts->find(n); it != fn_->decl->consts->end()) return &it->second;
        }
        return nullptr;
    }

    using Jumps = std::vector<size_t>;

    // Emits code for a condition; the returned jumps are taken when it equals `jumpIfTrue`.
    // `and`/`or`/`not` become jump chains and comparisons fused compare-and-jumps, so no
    // booleans are materialized.
    Jumps condJump(const Expr& cond, bool jumpIfTrue) {
        Jumps j = jumpIf(cond, jumpIfTrue);
        freeTemps();
        return j;
    }

    void patchAllHere(const Jumps& js) {
        for (size_t j : js) patchHere(j);
    }

    Jumps jumpIf(const Expr& e, bool sense) {
        if (e.kind == Expr::Kind::Unary && e.text == "not" && e.lhs) return jumpIf(*e.lhs, !sense);
        if (e.kind == Expr::Kind::Binary && (e.text == "and" || e.text == "or") && e.lhs && e.rhs) {
            bool isAnd = e.text == "and";
            if (isAnd != sense) {  // (a and b) is false / (a or b) is true: either side decides
                Jumps a = jumpIf(*e.lhs, sense);
                Jumps b = jumpIf(*e.rhs, sense);
                a.insert(a.end(), b.begin(), b.end());
                return a;
            }
            Jumps skip = jumpIf(*e.lhs, !sense);  // the left side settles it the other way
            Jumps b = jumpIf(*e.rhs, sense);
            patchAllHere(skip);
            return b;
        }
        if (e.kind == Expr::Kind::Binary && isComparison(e.text) && e.lhs && e.rhs) {
            bool rhsMutates = containsMethodCall(e.rhs.get());
            Operand a = operand(*e.lhs, rhsMutates);
            Operand b = operand(*e.rhs);
            checkBinary(e.text, a, b, e.loc);
            static const std::unordered_map<std::string, int> kinds{{"<", 0}, {"<=", 1}, {">", 2}, {">=", 3}, {"==", 4}, {"!=", 5}};
            int kind = kinds.at(e.text) | (sense ? 0 : 8);
            return {emit(Op::JmpCmp, a.rk, b.rk, 0, e.loc, static_cast<uint8_t>(kind))};
        }
        Operand o = operand(e);
        int r = toReg(o, e.loc);
        return {emitJump(sense ? Op::JmpIf : Op::JmpIfNot, r, e.loc)};
    }

    TypeSet exprTo(const Expr& e, int dst) {
        switch (e.kind) {
            case Expr::Kind::Number: emit(Op::LoadK, dst, constant(Value::number(e.number), e.loc), 0, e.loc); return kTNumber;
            case Expr::Kind::String: emit(Op::LoadK, dst, constant(Value::string(e.text), e.loc), 0, e.loc); return kTString;
            case Expr::Kind::Bool: emit(Op::LoadBool, dst, e.number != 0 ? 1 : 0, 0, e.loc); return kTBool;
            case Expr::Kind::None: emit(Op::LoadNone, dst, 0, 0, e.loc); return kTNone;
            case Expr::Kind::Color: emit(Op::LoadK, dst, constant(Value::color(e.color), e.loc), 0, e.loc); return kTColor;
            case Expr::Kind::Vector: {
                if (auto c = constEval(e, fn_->decl, fn_->bs, nullptr)) {
                    emit(Op::LoadK, dst, constant(*c, e.loc), 0, e.loc);
                    return tbit(c->type());
                }
                int n = static_cast<int>(e.args.size());
                int base = allocRegs(n, e.loc);
                for (int i = 0; i < n; ++i) {
                    if (!e.args[i]) continue;
                    TypeSet t = exprTo(*e.args[i], base + i);
                    requireType(t, kTNumber, e.args[i]->loc, "a vector component");
                }
                emit(Op::MakeVec, dst, base, n, e.loc);
                return n == 4 ? kTColor : kTVec;
            }
            case Expr::Kind::List: {
                int n = static_cast<int>(e.args.size());
                int base = allocRegs(std::max(n, 1), e.loc);
                for (int i = 0; i < n; ++i) {
                    if (e.args[i]) exprTo(*e.args[i], base + i);
                }
                emit(Op::NewList, dst, base, n, e.loc);
                return kTList;
            }
            case Expr::Kind::Map: {
                int n = static_cast<int>(std::min(e.args.size(), e.parts.size()));
                std::set<std::string> seen;
                int base = allocRegs(std::max(2 * n, 1), e.loc);
                for (int i = 0; i < n; ++i) {
                    if (!seen.insert(e.parts[i]).second) {
                        error(e.loc, "duplicate_name", "map key " + quote(e.parts[i]) + " appears twice");
                    }
                    emit(Op::LoadK, base + 2 * i, constant(Value::string(e.parts[i]), e.loc), 0, e.loc);
                    if (e.args[i]) exprTo(*e.args[i], base + 2 * i + 1);
                }
                emit(Op::NewMap, dst, base, n, e.loc);
                return kTMap;
            }
            case Expr::Kind::Interp: {
                int n = 0;
                for (size_t i = 0; i < e.parts.size(); ++i) {
                    if (!e.parts[i].empty()) ++n;
                    if (i < e.args.size()) ++n;
                }
                int base = allocRegs(std::max(n, 1), e.loc);
                int k = 0;
                for (size_t i = 0; i < e.parts.size(); ++i) {
                    if (!e.parts[i].empty()) {
                        emit(Op::LoadK, base + k++, constant(Value::string(e.parts[i]), e.loc), 0, e.loc);
                    }
                    if (i < e.args.size()) {
                        if (e.args[i]) exprTo(*e.args[i], base + k);
                        ++k;
                    }
                }
                emit(Op::Concat, dst, base, n, e.loc);
                return kTString;
            }
            case Expr::Kind::Ident: return identTo(e, dst);
            case Expr::Kind::Member: return memberTo(e, dst);
            case Expr::Kind::Index: {
                if (!e.lhs || !e.rhs) return kTAny;
                Operand obj = operand(*e.lhs);
                int objReg = toReg(obj, e.loc);
                Operand ix = operand(*e.rhs);
                TypeSet t = indexType(obj.type, ix.type, e.loc);
                emit(Op::Index, dst, objReg, ix.rk, e.loc);
                return t;
            }
            case Expr::Kind::Call: return callTo(e, dst);
            case Expr::Kind::MethodCall: return methodCallTo(e, dst);
            case Expr::Kind::Unary: {
                if (!e.lhs) return kTAny;
                Operand o = operand(*e.lhs);
                int r = toReg(o, e.loc);
                if (e.text == "not") {
                    emit(Op::Not, dst, r, 0, e.loc);
                    return kTBool;
                }
                if (!(o.type & (kTNumber | kTBool | kTVec))) {
                    error(e.loc, "type_mismatch", "cannot negate a " + typeSetName(o.type));
                }
                emit(Op::Neg, dst, r, 0, e.loc);
                return (o.type & kTVec) ? (o.type & (kTNumber | kTBool) ? (kTNumber | kTVec) : kTVec) : kTNumber;
            }
            case Expr::Kind::Binary: {
                if (!e.lhs || !e.rhs) return kTAny;
                if (e.text == "and" || e.text == "or") {
                    exprTo(*e.lhs, dst);
                    size_t j = emitJump(e.text == "and" ? Op::JmpIfNot : Op::JmpIf, dst, e.loc);
                    int saved = fn_->freeReg;
                    exprTo(*e.rhs, dst);
                    fn_->freeReg = saved;
                    patchHere(j);
                    emit(Op::Not, dst, dst, 0, e.loc, 1);  // to bool
                    return kTBool;
                }
                bool rhsMutates = containsMethodCall(e.rhs.get());
                Operand a = operand(*e.lhs, rhsMutates);
                Operand b = operand(*e.rhs);
                return binaryOp(e.text, a, b, dst, e.loc);
            }
        }
        return kTAny;
    }

    TypeSet binaryOp(const std::string& op, Operand a, Operand b, int dst, SourceLoc loc) {
        TypeSet result = checkBinary(op, a, b, loc);
        static const std::unordered_map<std::string, Op> ops{
            {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Mod}, {"==", Op::Eq},
            {"!=", Op::Ne}, {"<", Op::Lt},  {"<=", Op::Le}, {">", Op::Gt},  {">=", Op::Ge}, {"in", Op::In}};
        auto it = ops.find(op);
        if (it == ops.end()) {
            error(loc, "internal", "unknown operator " + op);
            return kTAny;
        }
        emit(it->second, dst, a.rk, b.rk, loc);
        return result;
    }

    // Static check: reports when no combination of the possible types is valid.
    TypeSet checkBinary(const std::string& op, Operand a, Operand b, SourceLoc loc) {
        TypeSet result = 0;
        bool anyValid = false;
        for (uint32_t i = 0; i < 9; ++i) {
            if (!(a.type & (1u << i))) continue;
            for (uint32_t j = 0; j < 9; ++j) {
                if (!(b.type & (1u << j))) continue;
                if (auto r = binaryResult(op, static_cast<VType>(i), static_cast<VType>(j))) {
                    anyValid = true;
                    result |= tbit(*r);
                }
            }
        }
        if (!anyValid && a.type && b.type) {
            std::string hint;
            if (op == "+" && ((a.type & kTNumber) || (b.type & kTNumber))) hint = "to join text and numbers, use a string: \"score: {score}\"";
            error(loc, "type_mismatch", "cannot apply '" + op + "' to " + typeSetName(a.type) + " and " + typeSetName(b.type), hint);
            result = kTAny;
        }
        return result ? result : kTAny;
    }

    TypeSet identTo(const Expr& e, int dst) {
        const std::string& n = e.text;
        if (Local* l = findLocal(n)) {
            if (l->reg != dst) emit(Op::Move, dst, l->reg, 0, e.loc);
            return l->type;
        }
        if (n == "self") {
            emit(Op::LoadSelf, dst, 0, 0, e.loc);
            return kTEntity;
        }
        static const std::unordered_map<std::string, std::pair<Env, TypeSet>> env{
            {"dt", {Env::Dt, kTNumber}},       {"time", {Env::Time, kTNumber}},
            {"frame", {Env::Frame, kTNumber}}, {"other", {Env::Other, kTEntity | kTNone}},
            {"state", {Env::State, kTString | kTNone}}, {"state_time", {Env::StateTime, kTNumber}}};
        if (auto it = env.find(n); it != env.end()) {
            emit(Op::LoadEnv, dst, static_cast<int>(it->second.first), 0, e.loc);
            return it->second.second;
        }
        if (n == "pi") {
            emit(Op::LoadK, dst, constant(Value::number(3.14159265358979323846), e.loc), 0, e.loc);
            return kTNumber;
        }
        if (int v = varIndex(n); v >= 0) {
            emit(Op::GetVar, dst, v, 0, e.loc);
            return varType(v);
        }
        if (const ConstSym* c = resolveConst(n)) {
            emit(Op::LoadK, dst, constant(c->value, e.loc), 0, e.loc);
            return c->type;
        }
        // Contact and raycast details: not reserved, so vars and locals may shadow them.
        static const std::unordered_map<std::string, std::pair<Env, TypeSet>> details{
            {"contact_point", {Env::ContactPoint, kTVec | kTNone}}, {"contact_normal", {Env::ContactNormal, kTVec | kTNone}},
            {"impact", {Env::Impact, kTNumber}},                     {"hit_point", {Env::HitPoint, kTVec | kTNone}},
            {"hit_normal", {Env::HitNormal, kTVec | kTNone}},        {"hit_distance", {Env::HitDistance, kTNumber | kTNone}}};
        if (auto it = details.find(n); it != details.end()) {
            emit(Op::LoadEnv, dst, static_cast<int>(it->second.first), 0, e.loc);
            return it->second.second;
        }
        if (fn_->decl && fn_->decl->uses && fn_->decl->uses->count(n)) {
            error(e.loc, "module_as_value", quote(n) + " is a module; call its functions like " + n + ".name(...)");
            return kTAny;
        }
        if (fn_->bs && fn_->bs->states.count(n)) {
            error(e.loc, "state_as_value", quote(n) + " is a state; switch to it with `go to " + n +
                                               "`, or compare names: state == \"" + n + "\"");
            return kTAny;
        }
        std::vector<std::string> names = visibleNames();
        std::string guess = str::closest(n, names);
        std::string hint;
        if (!guess.empty()) {
            hint = "did you mean " + quote(guess) + "?";
        } else if (n == "data" || n == "payload") {
            hint = "event payloads are available as `data` inside `on event` handlers";
        } else {
            hint = fn_->bs ? "declare it with `let " + n + " = ...` (local) or `var " + n + " = ...` (per entity)"
                           : "declare it with `let " + n + " = ...`";
        }
        error(e.loc, "unknown_name", "unknown name " + quote(n), hint);
        return kTAny;
    }

    TypeSet fieldType(const std::string& comp, const std::string& field, SourceLoc loc) {
        for (const TypeInfo* ti : opts_.componentTypes) {
            if (!ti || ti->name != comp) continue;
            const FieldInfo* f = ti->field(field);
            if (!f) {
                std::string guess = str::closest(field, ti->fieldNames());
                error(loc, "unknown_field", comp + " has no field " + quote(field),
                      guess.empty() ? "fields: " + joinNames(ti->fieldNames()) : "did you mean " + quote(guess) + "?");
                return kTAny;
            }
            switch (f->type) {
                case FieldType::Float:
                case FieldType::Int: return kTNumber;
                case FieldType::Bool: return kTBool;
                case FieldType::String:
                case FieldType::Enum: return kTString;
                case FieldType::Vec3: return kTVec;
                case FieldType::Color: return kTColor;
                case FieldType::Vec2: return kTVec;  // read as (x, y, 0)
                case FieldType::Vec4:
                case FieldType::Json: return kTAny;
            }
        }
        return kTAny;
    }

    static std::string joinNames(const std::vector<std::string>& v) {
        std::string out;
        for (const auto& s : v) out += (out.empty() ? "" : ", ") + s;
        return out;
    }

    TypeSet memberType(TypeSet obj, const MemberRef& m, SourceLoc loc) {
        using K = MemberRef::Kind;
        TypeSet out = 0;
        bool valid = false;
        if (obj & kTEntity) {
            valid = true;
            switch (m.kind) {
                case K::Position:
                case K::Rotation:
                case K::Scale: out |= kTVec; break;
                case K::Color: out |= kTColor | kTNone; break;
                case K::Name: out |= kTString; break;
                case K::Id: out |= kTNumber; break;
                case K::Enabled: out |= kTBool; break;
                case K::Tags: out |= kTList; break;
                case K::Parent: out |= kTEntity | kTNone; break;
                case K::State: out |= kTString | kTNone; break;
                default: out |= kTAny; break;
            }
        }
        if (obj & kTVec) {
            if (m.kind == K::X || m.kind == K::Y || m.kind == K::Z || m.kind == K::Length) {
                valid = true;
                out |= kTNumber;
            }
        }
        if (obj & kTColor) {
            if (m.kind == K::R || m.kind == K::G || m.kind == K::B || m.kind == K::A) {
                valid = true;
                out |= kTNumber;
            }
        }
        if (obj & kTMap) {
            valid = true;
            out |= kTAny;
        }
        if (obj & (kTList | kTString)) {
            if (m.kind == K::Length) {
                valid = true;
                out |= kTNumber;
            }
        }
        if (obj & kTNone) valid = valid || obj == kTAny;
        if (!valid) {
            std::vector<std::string> props;
            if (obj & kTVec) props = {"x", "y", "z", "length"};
            if (obj & kTColor) props = {"r", "g", "b", "a"};
            if (obj & (kTList | kTString)) props = {"length"};
            std::string hint = props.empty() ? "" : "properties: " + joinNames(props);
            if (obj & (kTList | kTString | kTMap)) {
                if (reg_.hasMethodNamed(m.name)) hint = quote(m.name) + " is a method: call it with ()";
            }
            std::string guess = str::closest(m.name, props);
            if (!guess.empty()) hint = "did you mean " + quote(guess) + "?";
            error(loc, "unknown_property", "a " + typeSetName(obj) + " has no property " + quote(m.name), hint);
            return kTAny;
        }
        return out ? out : kTAny;
    }

    void checkMemberWrite(TypeSet obj, const MemberRef& m, TypeSet value, SourceLoc loc) {
        using K = MemberRef::Kind;
        if (obj == kTEntity) {
            switch (m.kind) {
                case K::Position:
                case K::Rotation:
                case K::Scale:
                    if (!(value & (kTVec | kTEntity))) {
                        error(loc, "type_mismatch", m.name + " must be a vector like (0, 1, 0), got " + typeSetName(value));
                    }
                    return;
                case K::Color:
                    if (!(value & kTColor)) error(loc, "type_mismatch", "color must be a color like #ff8800, got " + typeSetName(value));
                    return;
                case K::Id:
                case K::Tags:
                case K::State:
                    error(loc, "readonly", m.name + " is read-only" +
                                               std::string(m.kind == K::Tags ? " (use add_tag/remove_tag)" : m.kind == K::State ? " (use go to)" : ""));
                    return;
                case K::Parent:
                    error(loc, "readonly", "parent is read-only (use set_parent(e, parent))");
                    return;
                default: return;
            }
        }
        if (obj == kTVec || obj == kTColor) {
            memberType(obj, m, loc);
            if (!(value & (kTNumber | kTBool))) error(loc, "type_mismatch", "." + m.name + " must be a number, got " + typeSetName(value));
            return;
        }
        if (obj == kTList || obj == kTString) {
            error(loc, "readonly", "cannot assign ." + m.name + " of a " + typeSetName(obj));
        }
    }

    TypeSet indexType(TypeSet obj, TypeSet index, SourceLoc loc) {
        if (!(obj & (kTList | kTMap | kTString))) {
            error(loc, "type_mismatch", "cannot index a " + typeSetName(obj) + " with []", "[] works on lists, maps and strings");
            return kTAny;
        }
        if (obj == kTList && !(index & kTNumber)) error(loc, "type_mismatch", "list indexes are numbers, got " + typeSetName(index));
        if (obj == kTMap && !(index & kTString)) error(loc, "type_mismatch", "map keys are strings, got " + typeSetName(index));
        if (obj == kTString) return kTString;
        return kTAny;
    }

    TypeSet memberTo(const Expr& e, int dst) {
        if (!e.lhs) return kTAny;
        const Expr& obj = *e.lhs;
        // Module constant: alias.NAME
        if (obj.kind == Expr::Kind::Ident && !findLocal(obj.text) && fn_->decl && fn_->decl->uses) {
            if (auto it = fn_->decl->uses->find(obj.text); it != fn_->decl->uses->end() && varIndex(obj.text) < 0) {
                ModuleInfo* m = it->second;
                if (auto c = m->consts.find(e.text); c != m->consts.end()) {
                    emit(Op::LoadK, dst, constant(c->second.value, e.loc), 0, e.loc);
                    return c->second.type;
                }
                std::vector<std::string> names;
                for (const auto& [n, v] : m->consts) names.push_back(n);
                for (const auto& [n, v] : m->fns) names.push_back(n);
                error(e.loc, "unknown_name", "module " + quote(obj.text) + " has no constant " + quote(e.text),
                      m->fns.count(e.text) ? quote(e.text) + " is a function: call it with ()" : didYouMean(e.text, names));
                return kTAny;
            }
        }
        // self.<var>
        if (obj.kind == Expr::Kind::Ident && obj.text == "self" && !findLocal("self")) {
            if (int v = varIndex(e.text); v >= 0) {
                emit(Op::GetVar, dst, v, 0, e.loc);
                return varType(v);
            }
        }
        // <entity>.<component>.<field>
        if (obj.kind == Expr::Kind::Member && components_.count(obj.text) && obj.lhs) {
            Operand base = operand(*obj.lhs);
            int r = toReg(base, obj.loc);
            if (!(base.type & kTEntity) && base.type != kTAny) {
                error(obj.loc, "type_mismatch", "components belong to entities; got a " + typeSetName(base.type));
            }
            TypeSet t = fieldType(obj.text, e.text, e.loc);
            emit(Op::GetField, dst, r, fieldRef(obj.text, e.text), e.loc);
            return t | kTNone;  // none when the entity lacks the component
        }
        Operand o = operand(obj);
        int r = toReg(o, obj.loc);
        if (components_.count(e.text) && (o.type & kTEntity)) {
            error(e.loc, "component_as_value", quote(e.text) + " is a component; read one of its fields, e.g. ." + e.text +
                                                   "." + firstField(e.text));
            return kTAny;
        }
        const MemberRef& m = prog_.members[memberRef(e.text)];
        TypeSet t = memberType(o.type, m, e.loc);
        if (obj.kind == Expr::Kind::Ident && obj.text == "self" && m.kind == MemberRef::Kind::Named) {
            warnUndeclaredSelf(e.text, e.loc, false);
        }
        emit(Op::GetMember, dst, r, memberRef(e.text), e.loc);
        return t;
    }

    void warnUndeclaredSelf(const std::string& name, SourceLoc loc, bool write) {
        std::vector<std::string> candidates{"position", "rotation", "scale", "color", "name", "id", "enabled", "tags",
                                            "parent", "state"};
        for (const auto& c : components_) candidates.push_back(c);
        if (fn_->bs) {
            for (const auto& [n, v] : fn_->bs->vars) candidates.push_back(n);
        }
        std::string guess = str::closest(name, candidates);
        warning(loc, "undeclared_var",
                "self." + name + " is not a property, component or declared var; it " +
                    (write ? "sets" : "reads") + " a free-form entity var" + (write ? "" : " (none if unset)"),
                guess.empty() ? "declare it with `var " + name + " = <value>` at the top of the behavior"
                              : "did you mean self." + guess + "?");
    }

    std::string firstField(const std::string& comp) {
        for (const TypeInfo* ti : opts_.componentTypes) {
            if (ti && ti->name == comp && !ti->fields.empty()) return ti->fields.front().name;
        }
        return "<field>";
    }

    // Arguments into consecutive registers starting at `base`.
    std::vector<TypeSet> argsTo(const std::vector<ExprPtr>& args, int base) {
        std::vector<TypeSet> types;
        for (size_t i = 0; i < args.size(); ++i) {
            types.push_back(args[i] ? exprTo(*args[i], base + static_cast<int>(i)) : kTAny);
        }
        return types;
    }

    bool checkArity(const std::string& what, int argc, int minArgs, int maxArgs, SourceLoc loc, const std::string& sig) {
        if (argc >= minArgs && (maxArgs < 0 || argc <= maxArgs)) return true;
        std::string expected = maxArgs < 0 ? "at least " + std::to_string(minArgs)
                               : minArgs == maxArgs ? std::to_string(minArgs)
                                                    : std::to_string(minArgs) + " to " + std::to_string(maxArgs);
        error(loc, "wrong_arity", what + " takes " + expected + " argument" + (expected == "1" ? "" : "s") + " but was given " +
                                      std::to_string(argc),
              sig.empty() ? "" : "usage: " + sig);
        return false;
    }

    void checkBuiltinArgs(const BuiltinDef& def, const std::vector<TypeSet>& types, const std::vector<ExprPtr>& args,
                          const std::string& label) {
        for (size_t i = 0; i < types.size(); ++i) {
            const BuiltinParam* p = i < def.params.size() ? &def.params[i]
                                    : (def.variadic && !def.params.empty()) ? &def.params.back()
                                                                            : nullptr;
            if (!p || !args[i]) continue;
            if (!(p->type & types[i])) {
                error(args[i]->loc, "type_mismatch", label + " argument " + std::to_string(i + 1) + " (" + p->name +
                                                         ") must be " + typeSetName(p->type) + ", got " + typeSetName(types[i]),
                      "usage: " + def.signature());
            }
        }
    }

    const FnInfo* resolveFn(const std::string& n, int* indexOut) {
        if (fn_->bs) {
            if (auto it = fn_->bs->fns.find(n); it != fn_->bs->fns.end()) {
                *indexOut = it->second;
                return &prog_.functions[it->second];
            }
        }
        if (fn_->decl && fn_->decl->fns) {
            if (auto it = fn_->decl->fns->find(n); it != fn_->decl->fns->end()) {
                *indexOut = it->second;
                return &prog_.functions[it->second];
            }
        }
        return nullptr;
    }

    TypeSet userCall(const FnInfo& f, int fnIndex, const std::vector<ExprPtr>& args, int dst, SourceLoc loc,
                     const std::string& label) {
        int argc = static_cast<int>(args.size());
        std::string sig = label + "(" + joinNames(f.params) + ")";
        checkArity(label + "()", argc, static_cast<int>(f.params.size()), static_cast<int>(f.params.size()), loc, sig);
        int base = callBase(dst, std::max(argc, 1), loc);
        auto types = argsTo(args, base);
        for (size_t i = 0; i < types.size() && i < f.paramTypes.size(); ++i) {
            if (args[i] && !(f.paramTypes[i] & types[i])) {
                error(args[i]->loc, "type_mismatch", label + "() parameter " + quote(f.params[i]) + " is " +
                                                         typeSetName(f.paramTypes[i]) + ", got " + typeSetName(types[i]));
            }
        }
        emit(Op::CallF, base, fnIndex, argc, loc);
        if (base != dst) emit(Op::Move, dst, base, 0, loc);
        return f.returns;
    }

    // Where a call's arguments go (the result lands in the first). When `dst` is the newest
    // scratch register, the call is built right there and needs no final move.
    int callBase(int dst, int n, SourceLoc loc) {
        if (dst == fn_->freeReg - 1 && dst >= localsEnd()) {
            allocRegs(n - 1, loc);
            return dst;
        }
        return allocRegs(n, loc);
    }

    TypeSet callTo(const Expr& e, int dst) {
        const std::string& n = e.text;
        int fnIndex = -1;
        if (const FnInfo* f = resolveFn(n, &fnIndex)) return userCall(*f, fnIndex, e.args, dst, e.loc, n);
        auto def = reg_.find(n);
        if (!def || def->hidden) {
            std::vector<std::string> names = reg_.functionNames();
            if (fn_->bs) {
                for (const auto& [fname, i] : fn_->bs->fns) names.push_back(fname);
            }
            if (fn_->decl && fn_->decl->fns) {
                for (const auto& [fname, i] : *fn_->decl->fns) names.push_back(fname);
            }
            std::string guess = str::closest(n, names);
            std::string hint = guess.empty() ? "see wander_reference for the list of functions" : "did you mean " + quote(guess) + "?";
            if (reg_.hasMethodNamed(n)) hint = quote(n) + " is a method: call it on a value, e.g. items." + n + "(...)";
            if (n == "len" || n == "size") hint = "use x.length for lists and strings";
            error(e.loc, "unknown_function", "unknown function " + quote(n), hint);
            for (const auto& a : e.args) {
                if (a) operand(*a);
            }
            return kTAny;
        }
        int argc = static_cast<int>(e.args.size());
        checkArity(n + "()", argc, def->minArgs(), def->maxArgs(), e.loc, def->signature());
        int base = callBase(dst, std::max(argc, 1), e.loc);
        auto types = argsTo(e.args, base);
        checkBuiltinArgs(*def, types, e.args, n + "()");
        emit(Op::Call, base, builtinRef(def), argc, e.loc);
        if (base != dst) emit(Op::Move, dst, base, 0, e.loc);
        return def->returns;
    }

    TypeSet methodCallTo(const Expr& e, int dst) {
        if (!e.lhs) return kTAny;
        const Expr& recv = *e.lhs;
        // Module function: alias.fn(...)
        if (recv.kind == Expr::Kind::Ident && !findLocal(recv.text) && varIndex(recv.text) < 0 && fn_->decl &&
            fn_->decl->uses) {
            if (auto it = fn_->decl->uses->find(recv.text); it != fn_->decl->uses->end()) {
                ModuleInfo* m = it->second;
                auto f = m->fns.find(e.text);
                if (f == m->fns.end()) {
                    std::vector<std::string> names;
                    for (const auto& [fname, i] : m->fns) names.push_back(fname);
                    error(e.loc, "unknown_function", "module " + quote(recv.text) + " has no function " + quote(e.text),
                          didYouMean(e.text, names));
                    return kTAny;
                }
                return userCall(prog_.functions[f->second], f->second, e.args, dst, e.loc, recv.text + "." + e.text);
            }
        }
        int argc = static_cast<int>(e.args.size());
        int base = allocRegs(argc + 2, e.loc);
        // The receiver: evaluate as an assignable chain when possible so mutations stick.
        Chain ch;
        bool lvalue = isLvalueExpr(recv);
        bool mutating = false;
        TypeSet recvType;
        int mi = methodRef(e.text);
        for (const auto& impl : prog_.methods[mi]) mutating = mutating || (impl && impl->mutates);
        if (lvalue && mutating && buildChain(recv, ch)) {
            loadChain(ch, ch.steps.size(), e.loc);
            recvType = ch.steps.empty() ? ch.baseType : ch.steps.back().type;
            emit(Op::Move, base, ch.regs.back(), 0, e.loc);
        } else {
            lvalue = false;
            recvType = exprTo(recv, base);
        }
        auto types = argsTo(e.args, base + 1);
        // Static checks against every receiver type it may be.
        bool anyImpl = false;
        TypeSet returns = 0;
        for (uint32_t t = 0; t < 9; ++t) {
            if (!(recvType & (1u << t))) continue;
            const auto& impl = prog_.methods[mi][t];
            if (!impl) continue;
            anyImpl = true;
            returns |= impl->returns;
            if (recvType == (1u << t)) {
                checkArity(e.text + "()", argc, impl->minArgs(), impl->maxArgs(), e.loc, impl->signature());
                checkBuiltinArgs(*impl, types, e.args, e.text + "()");
            }
        }
        if (!anyImpl && recvType != kTAny) {
            std::vector<std::string> names;
            for (uint32_t t = 0; t < 9; ++t) {
                if (recvType & (1u << t)) {
                    for (auto& nm : reg_.methodNames(static_cast<VType>(t))) names.push_back(nm);
                }
            }
            std::string hint = didYouMean(e.text, names);
            if (hint.empty() && reg_.find(e.text)) hint = quote(e.text) + " is a function: call it as " + e.text + "(value, ...)";
            if (hint.empty() && !names.empty()) hint = "methods: " + joinNames(names);
            error(e.loc, "unknown_method", "a " + typeSetName(recvType) + " has no method " + quote(e.text), hint);
        } else if (!anyImpl && !reg_.hasMethodNamed(e.text)) {
            std::string hint;
            if (reg_.find(e.text)) hint = quote(e.text) + " is a function: call it as " + e.text + "(value, ...)";
            error(e.loc, "unknown_method", "no type has a method " + quote(e.text), hint);
        }
        emit(Op::CallM, base, mi, argc, e.loc);
        if (lvalue) {
            // Write the updated receiver back where it came from.
            if (ch.steps.empty()) {
                if (ch.base == Chain::Base::Local) {
                    emit(Op::Move, ch.localReg, base, 0, e.loc);
                } else if (ch.base == Chain::Base::Var) {
                    emit(Op::SetVar, ch.var, base, 0, e.loc);
                }
            } else {
                storeChain(ch, ch.steps.size() - 1, static_cast<uint16_t>(base), e.loc);
            }
        }
        int result = base + argc + 1;
        if (result != dst) emit(Op::Move, dst, result, 0, e.loc);
        return returns ? returns : kTAny;
    }

    bool isLvalueExpr(const Expr& e) {
        const Expr* cur = &e;
        while (cur && (cur->kind == Expr::Kind::Member || cur->kind == Expr::Kind::Index)) cur = cur->lhs.get();
        if (!cur || cur->kind != Expr::Kind::Ident) return false;
        if (Local* l = findLocal(cur->text)) return !l->isConst || cur != &e;
        if (varIndex(cur->text) >= 0) return true;
        return (cur->text == "self" || cur->text == "other") && cur != &e;
    }

    const CompileOptions& opts_;
    const BuiltinRegistry& reg_;
    std::vector<Diagnostic>& diags_;
    Program& prog_;
    std::set<std::string> components_;
    std::string file_;

    std::vector<std::unique_ptr<ModuleInfo>> modules_;
    std::map<std::string, ModuleInfo*> moduleByPath_;
    NameMap<ConstSym> mainConsts_;
    NameMap<int> mainFns_;
    NameMap<ModuleInfo*> mainUses_;
    DeclScope mainDecl_;
    std::vector<std::unique_ptr<BehaviorScope>> behaviors_;

    std::unordered_map<std::string, uint16_t> constIndex_;
    std::unordered_map<std::string, int> memberIndex_;
    std::unordered_map<std::string, int> fieldIndex_;
    std::unordered_map<const BuiltinDef*, int> builtinIndex_;
    std::unordered_map<std::string, int> methodIndex_;
    int timerCounter_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

size_t CompileResult::errorCount() const {
    return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
                                             [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}

Json diagnosticToJson(const Diagnostic& d) {
    Json j = Json::object({{"severity", d.severity == Severity::Error ? "error" : "warning"},
                           {"line", d.loc.line},
                           {"column", d.loc.column},
                           {"code", d.code},
                           {"message", d.message}});
    if (!d.hint.empty()) j["hint"] = d.hint;
    if (!d.file.empty()) j["file"] = d.file;
    return j;
}

Json CompileResult::toJson() const {
    Json diags = Json::array();
    for (const auto& d : diagnostics) diags.push(diagnosticToJson(d));
    Json out = Json::object({{"ok", ok()}, {"errors", errorCount()}, {"diagnostics", diags}});
    if (program) {
        Json behaviors = Json::array();
        for (const auto& b : program->behaviors) {
            Json handlers = Json::array();
            for (const auto& h : b.handlers) {
                std::string t = h.custom ? h.argument : toString(h.trigger);
                if (!h.custom && !h.argument.empty()) t += " \"" + h.argument + "\"";
                if (h.state >= 0) t = b.states[h.state].name + ": " + t;
                handlers.push(t);
            }
            Json vars = Json::array();
            Json params = Json::array();
            for (const auto& v : b.vars) {
                if (v.isParam) {
                    Json p = Json::object({{"name", v.name}, {"type", typeSetName(v.type)}, {"default", v.defaultValue}});
                    if (v.hasRange) {
                        p["min"] = v.min;
                        p["max"] = v.max;
                    }
                    if (!v.doc.empty()) p["doc"] = v.doc;
                    params.push(p);
                } else {
                    vars.push(v.name);
                }
            }
            Json states = Json::array();
            for (const auto& s : b.states) states.push(s.name);
            Json tests = Json::array();
            for (const auto& t : b.tests) tests.push(t.name);
            Json j = Json::object({{"name", b.name}, {"intent", b.intent}, {"vars", vars}, {"handlers", handlers}});
            if (params.size()) j["params"] = params;
            if (states.size()) j["states"] = states;
            if (tests.size()) j["tests"] = tests;
            behaviors.push(j);
        }
        out["behaviors"] = behaviors;
        Json fns = Json::array();
        for (const auto& f : program->functions) fns.push(f.name);
        if (fns.size()) out["functions"] = fns;
        if (!program->modules.empty()) {
            Json mods = Json::array();
            for (const auto& m : program->modules) mods.push(m.path);
            out["modules"] = mods;
        }
    }
    return out;
}

Result<std::string> normalizeModulePath(std::string_view path) {
    std::string p = str::trim(path);
    if (p.empty()) return Error::make("invalid_module_path", "empty module path");
    if (p[0] == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos) {
        return Error::make("invalid_module_path", "module paths are relative to the project, like \"scripts/combat\"");
    }
    std::vector<std::string> parts;
    for (const auto& part : str::split(p, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return Error::make("invalid_module_path", "module paths cannot contain '..'");
        parts.push_back(part);
    }
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : "/") + part;
    if (out.size() < 7 || out.substr(out.size() - 7) != ".wander") out += ".wander";
    return out;
}

CompileResult compile(std::string_view source, const CompileOptions& options) {
    CompileResult result;
    ParseResult pr = parse(source);
    result.module = pr.module;
    result.diagnostics = std::move(pr.diagnostics);
    auto program = std::make_shared<Program>();
    if (result.errorCount() == 0) {
        Compiler(options, result.diagnostics, *program).run(*pr.module, source);
    }
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.file != b.file) return a.file < b.file;
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
    if (result.errorCount() == 0) result.program = std::move(program);
    return result;
}

CompileResult compile(std::string_view source, const std::vector<std::string>& knownComponents) {
    CompileOptions o;
    o.components = knownComponents;
    return compile(source, o);
}

std::vector<std::string> builtinFunctions() { return BuiltinRegistry::global().functionNames(); }

}  // namespace sky::wander
