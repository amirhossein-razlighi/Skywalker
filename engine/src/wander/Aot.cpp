// Wander AOT: bytecode -> C++ -> dylib, and the host side of the native ABI.

#include "skywalker/wander/Aot.h"

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "../native/Process.h"
#include "RuntimeInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/wander/Compiler.h"

namespace sky::wander {

namespace fs = std::filesystem;

namespace {
const char* kValueHeader =
#include "native_value_h.inc"
    ;
const char* kAotHeader =
#include "native_aot_h.inc"
    ;
}  // namespace

// ---------------------------------------------------------------------------
// Host side of the ABI
// ---------------------------------------------------------------------------

namespace {

int64_t hostExec(SkyAotFrame* f, uint32_t pc) {
    auto& st = *static_cast<ExecState*>(f->exec);
    auto& out = *static_cast<Outcome*>(f->outcome);
    size_t p = pc;
    try {
        return execOne(st, f->proto, &Value::fromAbi(f->R), p, out) ? static_cast<int64_t>(p) : -1;
    } catch (RuntimeError& e) {
        f->error = new RuntimeError(std::move(e));
    } catch (const std::exception& e) {
        SourceLoc loc = st.prog->protos[f->proto].locs[pc];
        f->error = new RuntimeError{loc, std::string("internal error: ") + e.what(), {}};
    }
    return -1;
}

int64_t hostExecRange(SkyAotFrame* f, uint32_t pc, uint32_t end) {
    auto& st = *static_cast<ExecState*>(f->exec);
    auto& out = *static_cast<Outcome*>(f->outcome);
    size_t p = pc;
    try {
        return execRange(st, f->proto, &Value::fromAbi(f->R), p, end, out) ? static_cast<int64_t>(p) : -1;
    } catch (RuntimeError& e) {
        f->error = new RuntimeError(std::move(e));
    } catch (const std::exception& e) {
        SourceLoc loc = st.prog->protos[f->proto].locs[p < st.prog->protos[f->proto].locs.size() ? p : pc];
        f->error = new RuntimeError{loc, std::string("internal error: ") + e.what(), {}};
    }
    return -1;
}

int hostTruthy(SkyAotFrame* f, const SkyValue* v) {
    auto& st = *static_cast<ExecState*>(f->exec);
    return truthy(st.scene, Value::fromAbi(v)) ? 1 : 0;
}

int64_t hostBudgetExceeded(SkyAotFrame* f, uint32_t pc) {
    auto& st = *static_cast<ExecState*>(f->exec);
    SourceLoc loc = st.prog->protos[f->proto].locs[pc];
    f->error = new RuntimeError{loc,
                                "execution budget exceeded: this handler ran more than " + std::to_string(Runtime::kBudget) +
                                    " steps (an endless loop?); spread long work over several ticks with `wait`",
                                {}};
    return -1;
}

void hostFree(SkyObject* o) { destroyObject(reinterpret_cast<Obj*>(o)); }

const SkyAotApi kApi{SKY_AOT_VERSION, sizeof(SkyAotApi), hostExec, hostExecRange, hostTruthy, hostBudgetExceeded, hostFree};

}  // namespace

const SkyAotApi* aotHostApi() { return &kApi; }

bool aotRun(ExecState& st, int proto, Value* regs, size_t pc, Outcome& out) {
    const NativeProgram* np = st.native;
    if (!np || proto < 0 || static_cast<size_t>(proto) >= np->fns.size() || !np->fns[proto]) return false;
    static const Value kNoConstants;
    SkyAotFrame f{};
    f.exec = &st;
    f.R = regs->abi();
    f.K = st.prog->constants.empty() ? kNoConstants.abi() : st.prog->constants.data()->abi();
    f.budget = &st.budget;
    f.outcome = &out;
    f.error = nullptr;
    f.proto = proto;
    f.pc = static_cast<uint32_t>(pc);
    f.self = st.self;
    f.dt = st.dt;
    np->fns[proto](&f);
    if (f.error) {
        std::unique_ptr<RuntimeError> e(static_cast<RuntimeError*>(f.error));
        throw RuntimeError(std::move(*e));
    }
    return true;
}

NativeProgram::~NativeProgram() {
    if (handle) dlclose(handle);
}

Json AotResult::toJson() const {
    return Json::object({{"library", library},
                         {"source", source},
                         {"cached", cached},
                         {"compile_ms", compileMs},
                         {"protos", protos},
                         {"inlined_instructions", inlined},
                         {"delegated_instructions", delegated}});
}

// ---------------------------------------------------------------------------
// Code generation
// ---------------------------------------------------------------------------

namespace {

std::string numLiteral(double d) {
    if (std::isnan(d)) return "(0.0/0.0)";
    if (std::isinf(d)) return d > 0 ? "(1.0/0.0)" : "(-1.0/0.0)";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return "(" + s + ")";
}

struct Operand {
    std::string ref;    // "R[3]" / "K[2]"
    bool knownNumber = false;
    std::string value;  // numeric C++ expression
    std::string check;  // "" if known, else "<ref>.type == SKY_NUMBER"
};

class Emitter {
public:
    explicit Emitter(const Program& p) : prog_(p) {}

    std::string run(size_t* inlined, size_t* delegated) {
        std::ostringstream os;
        os << "// Generated by Skywalker from Wander bytecode. Do not edit: regenerated when the behavior changes.\n";
        os << "// Program hash " << std::hex << prog_.hash << std::dec << ", " << prog_.protos.size() << " protos.\n";
        os << "#define SKY_AOT_GENERATED 1\n#include <math.h>\n#include <stdint.h>\n";
        std::string value = kValueHeader;
        std::string aot = kAotHeader;
        std::string inc = "#include \"skywalker/native/value.h\"";
        if (auto p = aot.find(inc); p != std::string::npos) aot.erase(p, inc.size());
        os << value << "\n" << aot << "\n";
        os << "namespace {\n";
        for (size_t i = 0; i < prog_.protos.size(); ++i) os << proto(static_cast<int>(i));
        os << "const SkyAotFn kTable[] = {";
        for (size_t i = 0; i < prog_.protos.size(); ++i) {
            os << (i ? ", " : "") << (skipped_.count(static_cast<int>(i)) ? "nullptr" : "p" + std::to_string(i));
        }
        if (prog_.protos.empty()) os << "nullptr";
        os << "};\n}  // namespace\n\n";
        os << "extern \"C\" __attribute__((visibility(\"default\"))) int sky_aot_init(const SkyAotApi* api, uint64_t* hash, "
              "uint32_t* count, const SkyAotFn** table) {\n"
              "    if (!api || api->version != SKY_AOT_VERSION || api->size < sizeof(SkyAotApi)) return -1;\n"
              "    sky_api = api;\n"
              "    *hash = "
           << prog_.hash << "ULL;\n    *count = " << prog_.protos.size()
           << "u;\n    *table = kTable;\n    return SKY_AOT_VERSION;\n}\n";
        if (inlined) *inlined = inlined_;
        if (delegated) *delegated = delegated_;
        return os.str();
    }

private:
    Operand rk(uint16_t x) const {
        Operand o;
        if (isK(x)) {
            const Value& k = prog_.constants[x & 0x7fff];
            o.ref = "K[" + std::to_string(x & 0x7fff) + "]";
            if (k.isNumber()) {
                o.knownNumber = true;
                o.value = numLiteral(k.num());
                return o;
            }
            o.value = o.ref + ".as.number";
            o.check = o.ref + ".type == SKY_NUMBER";
            return o;
        }
        o.ref = "R[" + std::to_string(x) + "]";
        o.value = o.ref + ".as.number";
        o.check = o.ref + ".type == SKY_NUMBER";
        return o;
    }

    static std::string both(const Operand& a, const Operand& b) {
        std::string c;
        if (!a.check.empty()) c = a.check;
        if (!b.check.empty()) c += (c.empty() ? "" : " && ") + b.check;
        return c.empty() ? "1" : c;
    }

    std::string delegate(size_t pc) {
        ++delegated_;
        return "    { if (sky_api->exec(f, " + std::to_string(pc) + "u) < 0) return 0; }\n";
    }
    // Delegate an instruction that may branch: continue at pc+1 or jump to `target`.
    std::string delegateBranch(size_t pc, size_t target) {
        ++delegated_;
        return "    { int64_t n = sky_api->exec(f, " + std::to_string(pc) + "u); if (n < 0) return 0; if (n != " +
               std::to_string(pc + 1) + ") goto L" + std::to_string(target) + "; }\n";
    }
    std::string fallback(size_t pc, const std::string& fast) {
        ++inlined_;
        return fast + " else { if (sky_api->exec(f, " + std::to_string(pc) + "u) < 0) return 0; }\n";
    }
    std::string chargeAndJump(size_t pc, int64_t units, size_t target) {
        std::string s;
        if (units > 0) {
            s += "*budget -= " + std::to_string(units) + "; if (*budget < 0) { sky_api->budget_exceeded(f, " +
                 std::to_string(pc) + "u); return 0; } ";
        }
        return s + "goto L" + std::to_string(target) + ";";
    }

    // Pure core builtins translated inline (same libm calls as the VM).
    std::string inlineCall(const Ins& in, size_t pc, bool probe = false) {
        const auto& def = prog_.builtins[in.b];
        if (!def || def->owner != "core") return {};
        const std::string& n = def->name;
        std::string a = "R[" + std::to_string(in.a) + "]";
        std::string b = "R[" + std::to_string(in.a + 1) + "]";
        static const std::map<std::string, std::string> unary{
            {"sin", "sin(x)"},     {"cos", "cos(x)"},     {"tan", "tan(x)"},     {"abs", "fabs(x)"},
            {"floor", "floor(x)"}, {"ceil", "ceil(x)"},   {"exp", "exp(x)"},     {"sqrt", "sqrt(0.0 < x ? x : 0.0)"},
            {"atan", "atan(x)"},   {"rad", "x * 3.14159265358979323846 / 180.0"},
            {"deg", "x * 180.0 / 3.14159265358979323846"}};
        if (in.c == 1) {
            auto it = unary.find(n);
            if (it == unary.end()) return {};
            std::string expr = it->second;
            std::string out;
            for (char ch : expr) {
                if (ch == 'x') out += "(" + a + ".as.number)";
                else out += ch;
            }
            if (probe) return "yes";
            return fallback(pc, "    if (" + a + ".type == SKY_NUMBER) sky_set_number(&" + a + ", " + out + ");");
        }
        if (in.c == 2 && (n == "min" || n == "max" || n == "atan2" || n == "pow")) {
            if (probe) return "yes";
            std::string x = a + ".as.number", y = b + ".as.number";
            std::string expr = n == "min" ? "(" + y + " < " + x + " ? " + y + " : " + x + ")"
                               : n == "max" ? "(" + x + " < " + y + " ? " + y + " : " + x + ")"
                               : n == "atan2" ? "atan2(" + x + ", " + y + ")"
                                              : "pow(" + x + ", " + y + ")";
            return fallback(pc, "    if (" + a + ".type == SKY_NUMBER && " + b + ".type == SKY_NUMBER) { double t_ = " + expr +
                                    "; sky_set_number(&" + a + ", t_); }");
        }
        return {};
    }

    // Whether an instruction is translated inline (or is a branch handled natively).
    bool nativeOp(const Ins& in) {
        switch (in.op) {
            case Op::Nop:
            case Op::Move:
            case Op::LoadNone:
            case Op::LoadBool:
            case Op::LoadSelf:
            case Op::Add:
            case Op::Sub:
            case Op::Mul:
            case Op::Div:
            case Op::Mod:
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne:
            case Op::Neg:
            case Op::Not:
            case Op::Jmp:
            case Op::JmpIf:
            case Op::JmpIfNot:
            case Op::JmpCmp:
            case Op::ForPrep:
            case Op::ForLoop:
            case Op::IterPrep:
            case Op::IterNext:
            case Op::MakeVec: return true;
            case Op::LoadK: return true;
            case Op::LoadEnv: return static_cast<Env>(in.b) == Env::Dt;
            case Op::GetMember: {
                auto k = prog_.members[in.c].kind;
                return k == MemberRef::Kind::X || k == MemberRef::Kind::Y || k == MemberRef::Kind::Z;
            }
            case Op::Call: return !inlineCall(in, 0, true).empty();
            default: return false;
        }
    }

    // Native code pays off for computation; protos that mostly call back into the VM
    // (property access, builtins with side effects) stay interpreted.
    bool worthCompiling(const Proto& P) {
        size_t compute = 0, delegated = 0;
        bool loop = false;
        for (size_t pc = 0; pc < P.code.size(); ++pc) {
            const Ins& in = P.code[pc];
            if (nativeOp(in)) {
                if (in.op != Op::Move && in.op != Op::LoadK && in.op != Op::LoadNone && in.op != Op::LoadBool &&
                    in.op != Op::Nop && in.op != Op::Jmp && in.op != Op::LoadSelf) {
                    ++compute;
                }
            } else {
                ++delegated;
            }
            if ((in.op == Op::Jmp && in.sbx() < 0) || in.op == Op::ForLoop || in.op == Op::IterNext) loop = true;
        }
        // Loops amortize the call-back cost; straight-line code must be mostly computation.
        return compute > 0 && (loop ? compute * 4 >= delegated : compute >= delegated * 2);
    }

    std::string proto(int index) {
        const Proto& P = prog_.protos[index];
        if (P.code.empty() || !worthCompiling(P)) {
            skipped_.insert(index);
            return "";
        }
        std::ostringstream os;
        std::set<size_t> resume{0};
        std::set<size_t> targets{0};
        for (size_t pc = 0; pc < P.code.size(); ++pc) {
            const Ins& in = P.code[pc];
            if (in.op == Op::Wait) resume.insert(pc + 1);
            switch (in.op) {
                case Op::Jmp:
                case Op::JmpIf:
                case Op::JmpIfNot:
                case Op::ForPrep:
                case Op::ForLoop:
                case Op::IterPrep:
                case Op::IterNext: targets.insert(static_cast<size_t>(static_cast<int64_t>(pc) + 1 + in.sbx())); break;
                case Op::JmpCmp: targets.insert(static_cast<size_t>(static_cast<int64_t>(pc) + 1 + static_cast<int16_t>(in.c))); break;
                default: break;
            }
        }
        targets.insert(resume.begin(), resume.end());
        os << "// " << P.name << "\n";
        os << "int p" << index << "(SkyAotFrame* f) {\n";
        os << "    SkyValue* R = f->R;\n    const SkyValue* K = f->K;\n    int64_t* budget = f->budget;\n";
        os << "    (void)R; (void)K; (void)budget;\n";
        os << "    switch (f->pc) {\n";
        for (size_t r : resume) os << "        case " << r << "u: goto L" << r << ";\n";
        os << "        default: { int64_t p = f->pc; while (p >= 0) p = sky_api->exec(f, (uint32_t)p); return 0; }\n    }\n";
        for (size_t pc = 0; pc < P.code.size();) {
            const Ins& in = P.code[pc];
            os << "L" << pc << ":  // " << opName(in.op) << " line " << P.locs[pc].line << "\n";
            if (!nativeOp(in)) {
                // A straight run of VM instructions: one call (split at jump targets).
                size_t end = pc + 1;
                while (end < P.code.size() && !nativeOp(P.code[end]) && !targets.count(end)) ++end;
                if (end - pc > 1) {
                    delegated_ += end - pc;
                    os << "    { if (sky_api->exec_range(f, " << pc << "u, " << end << "u) < 0) return 0; }\n";
                    for (size_t k = pc + 1; k < end; ++k) os << "L" << k << ":\n";
                    pc = end;
                    continue;
                }
            }
            os << ins(in, pc);
            ++pc;
        }
        os << "L" << P.code.size() << ":\n    return 0;\n}\n\n";
        return os.str();
    }

    std::string ins(const Ins& in, size_t pc) {
        std::string A = "R[" + std::to_string(in.a) + "]";
        auto target = [&](int32_t off) { return static_cast<size_t>(static_cast<int64_t>(pc) + 1 + off); };
        switch (in.op) {
            case Op::Nop: ++inlined_; return "";
            case Op::Move: {
                std::string B = "R[" + std::to_string(in.b) + "]";
                return fallback(pc, "    if (" + B + ".type < SKY_STRING) { sky_release(&" + A + "); " + A + " = " + B + "; }");
            }
            case Op::LoadK: {
                const Value& k = prog_.constants[in.b];
                if (k.isNumber()) {
                    ++inlined_;
                    return "    sky_set_number(&" + A + ", " + numLiteral(k.num()) + ");\n";
                }
                if (!k.isObject()) {
                    ++inlined_;
                    return "    sky_release(&" + A + "); " + A + " = K[" + std::to_string(in.b) + "];\n";
                }
                return delegate(pc);
            }
            case Op::LoadNone: ++inlined_; return "    sky_set_none(&" + A + ");\n";
            case Op::LoadSelf: ++inlined_; return "    sky_release(&" + A + "); " + A + " = sky_entity(f->self);\n";
            case Op::LoadEnv:
                if (static_cast<Env>(in.b) == Env::Dt) {
                    ++inlined_;
                    return "    sky_set_number(&" + A + ", f->dt);\n";
                }
                return delegate(pc);
            case Op::GetMember: {
                auto k = prog_.members[in.c].kind;
                int idx = k == MemberRef::Kind::X ? 0 : k == MemberRef::Kind::Y ? 1 : k == MemberRef::Kind::Z ? 2 : -1;
                if (idx < 0) return delegate(pc);
                std::string B = "R[" + std::to_string(in.b) + "]";
                return fallback(pc, "    if (" + B + ".type == SKY_VEC) sky_set_number(&" + A + ", " + B + ".as.vec[" +
                                        std::to_string(idx) + "]);");
            }
            case Op::MakeVec: {
                std::string check, args;
                for (int i = 0; i < 4; ++i) {
                    std::string r = "R[" + std::to_string(in.b + i) + "]";
                    if (i < in.c) {
                        check += (check.empty() ? "" : " && ") + r + ".type == SKY_NUMBER";
                        args += (i ? ", " : "") + std::string("(float)") + r + ".as.number";
                    } else if (i < 3) {
                        args += (i ? ", " : "") + std::string("0.0f");
                    }
                }
                if (in.c < 2 || in.c > 4) return delegate(pc);
                std::string make = in.c == 4 ? "sky_color(" + args + ")" : "sky_vec(" + args + ")";
                return fallback(pc, "    if (" + check + ") { SkyValue t_ = " + make + "; sky_release(&" + A + "); " + A + " = t_; }");
            }
            case Op::LoadBool: ++inlined_; return "    sky_set_bool(&" + A + ", " + std::to_string(in.b ? 1 : 0) + ");\n";
            case Op::Add:
            case Op::Sub:
            case Op::Mul: {
                Operand b = rk(in.b), c = rk(in.c);
                const char* sym = in.op == Op::Add ? "+" : in.op == Op::Sub ? "-" : "*";
                std::string s = "    if (" + both(b, c) + ") sky_set_number(&" + A + ", " + b.value + " " + sym + " " + c.value + ");";
                // Vector math in float, component by component (as the VM does).
                if (!b.knownNumber && !c.knownNumber) {
                    std::string v = " else if (" + b.ref + ".type == SKY_VEC && " + c.ref + ".type == SKY_VEC) { SkyValue t_ = sky_vec(";
                    for (int i = 0; i < 3; ++i) {
                        v += (i ? ", " : "") + b.ref + ".as.vec[" + std::to_string(i) + "] " + sym + " " + c.ref + ".as.vec[" +
                             std::to_string(i) + "]";
                    }
                    s += v + "); sky_release(&" + A + "); " + A + " = t_; }";
                }
                if (in.op == Op::Mul && !b.knownNumber) {
                    std::string f = "(float)(" + c.value + ")";
                    std::string cn = c.check.empty() ? "1" : c.check;
                    s += " else if (" + b.ref + ".type == SKY_VEC && " + cn + ") { SkyValue t_ = sky_vec(" + b.ref + ".as.vec[0] * " + f +
                         ", " + b.ref + ".as.vec[1] * " + f + ", " + b.ref + ".as.vec[2] * " + f + "); sky_release(&" + A + "); " + A +
                         " = t_; }";
                }
                return fallback(pc, s);
            }
            case Op::Div:
            case Op::Mod: {
                Operand b = rk(in.b), c = rk(in.c);
                std::string expr = in.op == Op::Div ? b.value + " / " + c.value : "fmod(" + b.value + ", " + c.value + ")";
                return fallback(pc, "    if (" + both(b, c) + " && " + c.value + " != 0) sky_set_number(&" + A + ", " + expr + ");");
            }
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne: {
                Operand b = rk(in.b), c = rk(in.c);
                const char* sym = in.op == Op::Lt   ? "<"
                                  : in.op == Op::Le ? "<="
                                  : in.op == Op::Gt ? ">"
                                  : in.op == Op::Ge ? ">="
                                  : in.op == Op::Eq ? "=="
                                                    : "!=";
                return fallback(pc, "    if (" + both(b, c) + ") sky_set_bool(&" + A + ", " + b.value + " " + sym + " " +
                                        c.value + ");");
            }
            case Op::Neg: {
                std::string B = "R[" + std::to_string(in.b) + "]";
                return fallback(pc, "    if (" + B + ".type == SKY_NUMBER) sky_set_number(&" + A + ", -" + B + ".as.number);");
            }
            case Op::Not: {
                std::string B = "R[" + std::to_string(in.b) + "]";
                std::string t = "sky_truthy(f, &" + B + ")";
                return fallback(pc, "    if (" + B + ".type <= SKY_NUMBER) { int t_ = " + t + "; sky_set_bool(&" + A + ", " +
                                        (in.x ? "t_" : "!t_") + "); }");
            }
            case Op::Jmp: {
                ++inlined_;
                int32_t off = in.sbx();
                return "    " + chargeAndJump(pc, off < 0 ? -off : 0, target(off)) + "\n";
            }
            case Op::JmpIf:
            case Op::JmpIfNot: {
                ++inlined_;
                return std::string("    if (") + (in.op == Op::JmpIf ? "" : "!") + "sky_truthy(f, &" + A + ")) goto L" +
                       std::to_string(target(in.sbx())) + ";\n";
            }
            case Op::JmpCmp: {
                ++inlined_;
                Operand b = rk(in.a), c = rk(in.b);
                static const char* cmp[] = {"<", "<=", ">", ">=", "==", "!="};
                auto off = static_cast<int16_t>(in.c);
                size_t t = static_cast<size_t>(static_cast<int64_t>(pc) + 1 + off);
                std::string cond = b.value + " " + cmp[in.x & 7] + " " + c.value;
                std::string taken = (in.x & 8) ? "(!(" + cond + "))" : "(" + cond + ")";
                return "    if (" + both(b, c) + ") { if " + taken + " { " + chargeAndJump(pc, off < 0 ? -off : 0, t) +
                       " } } else " + delegateBranch(pc, t).substr(4);
            }
            case Op::ForPrep: {
                ++inlined_;
                std::string r0 = "R[" + std::to_string(in.a) + "]", r1 = "R[" + std::to_string(in.a + 1) + "]",
                            r2 = "R[" + std::to_string(in.a + 2) + "]", r3 = "R[" + std::to_string(in.a + 3) + "]";
                std::string cond = in.x ? "(d_ > 0 ? s_ <= l_ : s_ >= l_)" : "(d_ > 0 ? s_ < l_ : s_ > l_)";
                size_t t = target(in.sbx());
                return "    if (" + r0 + ".type == SKY_NUMBER && " + r1 + ".type == SKY_NUMBER && " + r2 +
                       ".type == SKY_NUMBER && " + r2 + ".as.number != 0) {\n        double s_ = " + r0 +
                       ".as.number, l_ = " + r1 + ".as.number, d_ = " + r2 + ".as.number;\n        if (!" + cond +
                       ") goto L" + std::to_string(t) + ";\n        sky_set_number(&" + r3 + ", s_);\n    } else " +
                       delegateBranch(pc, t).substr(4);
            }
            case Op::ForLoop: {
                ++inlined_;
                std::string r0 = "R[" + std::to_string(in.a) + "]", r1 = "R[" + std::to_string(in.a + 1) + "]",
                            r2 = "R[" + std::to_string(in.a + 2) + "]", r3 = "R[" + std::to_string(in.a + 3) + "]";
                int32_t off = in.sbx();
                std::string cond = in.x ? "(d_ > 0 ? v_ <= l_ : v_ >= l_)" : "(d_ > 0 ? v_ < l_ : v_ > l_)";
                return "    { double d_ = " + r2 + ".as.number, v_ = " + r0 + ".as.number + d_, l_ = " + r1 +
                       ".as.number;\n      if (" + cond + ") { " + r0 + ".as.number = v_; sky_set_number(&" + r3 + ", v_); " +
                       chargeAndJump(pc, -off, target(off)) + " } }\n";
            }
            case Op::IterPrep:
            case Op::IterNext: return delegateBranch(pc, target(in.sbx()));
            case Op::Call: {
                std::string s = inlineCall(in, pc);
                return s.empty() ? delegate(pc) : s;
            }
            default: return delegate(pc);
        }
    }

    const Program& prog_;
    size_t inlined_ = 0;
    size_t delegated_ = 0;
    std::set<int> skipped_;  // protos left to the VM
};

}  // namespace

std::string generateCpp(const Program& prog, size_t* inlined, size_t* delegated) {
    return Emitter(prog).run(inlined, delegated);
}

std::string findCxxCompiler() {
    if (const char* env = std::getenv("SKY_CXX"); env && *env) {
        std::string p = native::whichExecutable(env);
        if (!p.empty()) return p;
    }
    for (const char* c : {"clang++", "c++", "g++"}) {
        std::string p = native::whichExecutable(c);
        if (!p.empty()) return p;
    }
    if (fs::exists("/usr/bin/clang++")) return "/usr/bin/clang++";
    return {};
}

namespace {

Result<std::shared_ptr<NativeProgram>> loadLibrary(const std::string& path, const Program& prog) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* err = dlerror();
        return Error::make("load_failed", std::string("cannot load ") + path + ": " + (err ? err : "unknown error"));
    }
    auto np = std::make_shared<NativeProgram>();
    np->handle = h;
    np->library = path;
    auto init = reinterpret_cast<SkyAotInitFn>(dlsym(h, "sky_aot_init"));
    if (!init) return Error::make("load_failed", path + " has no sky_aot_init (not a Wander AOT library)");
    uint64_t hash = 0;
    uint32_t count = 0;
    const SkyAotFn* table = nullptr;
    int v = init(aotHostApi(), &hash, &count, &table);
    if (v != SKY_AOT_VERSION) return Error::make("version_mismatch", path + " was built for another AOT ABI version");
    if (hash != prog.hash || count != prog.protos.size() || !table) {
        return Error::make("stale_library", path + " does not match the program (rebuild with force=true)");
    }
    np->hash = hash;
    np->fns.assign(table, table + count);
    return np;
}

}  // namespace

Result<AotResult> compileNative(const Program& prog, const AotOptions& options) {
    if (options.cacheDir.empty()) return Error::make("invalid_argument", "no cache directory for native code");
    std::error_code ec;
    fs::create_directories(options.cacheDir, ec);
    if (ec) return Error::make("io_error", "cannot create " + options.cacheDir + ": " + ec.message());
    char name[64];
    std::snprintf(name, sizeof(name), "w_%016llx_v%d_%d", static_cast<unsigned long long>(prog.hash), SKY_AOT_VERSION,
                  kCompilerVersion);
    AotResult result;
    result.source = (fs::path(options.cacheDir) / (std::string(name) + ".cpp")).string();
    result.library = (fs::path(options.cacheDir) / (std::string(name) + ".dylib")).string();
    for (const auto& p : prog.protos) result.protos += p.code.empty() ? 0 : 1;

    if (!options.force && fs::exists(result.library)) {
        auto lib = loadLibrary(result.library, prog);
        if (lib) {
            result.native = lib.value();
            result.cached = true;
            return result;
        }
        fs::remove(result.library, ec);  // stale or broken: rebuild
    }

    std::string code = generateCpp(prog, &result.inlined, &result.delegated);
    {
        std::ofstream f(result.source, std::ios::binary | std::ios::trunc);
        f << code;
        if (!f) return Error::make("io_error", "cannot write " + result.source);
    }
    std::string cxx = options.compiler.empty() ? findCxxCompiler() : options.compiler;
    if (cxx.empty()) {
        return Error::make("no_compiler", "no C++ compiler found (clang++)",
                           "install the Xcode command line tools: xcode-select --install");
    }
    // Build into a temporary name, then rename: a crash mid-build never leaves a broken library.
    std::string tmp = result.library + ".tmp" + std::to_string(::getpid());
    // -ffp-contract=off: no fused multiply-add, so float results match the VM bit for bit.
    std::vector<std::string> argv{cxx, "-O2", "-shared", "-fPIC", "-std=c++20", "-w", "-fno-exceptions", "-fno-rtti",
                                  "-ffp-contract=off"};
    for (const auto& fl : options.flags) argv.push_back(fl);
    argv.insert(argv.end(), {"-o", tmp, result.source});
    auto t0 = std::chrono::steady_clock::now();
    auto pr = native::runProcess(argv, options.cacheDir, 300);
    result.compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!pr) return pr.error();
    result.compilerOutput = pr->output;
    if (pr->exitCode != 0) {
        fs::remove(tmp, ec);
        std::string out = pr->output.size() > 4000 ? pr->output.substr(0, 4000) + "..." : pr->output;
        return Error::make("compile_failed", "the C++ compiler failed (" + std::to_string(pr->exitCode) + "):\n" + out,
                           "the behavior keeps running in the VM; report this output as an engine bug");
    }
    fs::rename(tmp, result.library, ec);
    if (ec) return Error::make("io_error", "cannot write " + result.library + ": " + ec.message());
    auto lib = loadLibrary(result.library, prog);
    if (!lib) return lib.error();
    result.native = lib.value();
    return result;
}

}  // namespace sky::wander
