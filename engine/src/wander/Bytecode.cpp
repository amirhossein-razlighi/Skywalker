#include "skywalker/wander/Bytecode.h"

#include <cstdio>
#include <sstream>

namespace sky::wander {

const char* opName(Op op) {
    static const char* names[] = {"NOP",      "MOVE",      "LOADK",     "LOADNONE", "LOADBOOL", "LOADSELF", "LOADENV",
                                  "GETVAR",   "SETVAR",    "GETMEMBER", "SETMEMBER", "GETFIELD", "SETFIELD", "INDEX",
                                  "SETINDEX", "ADD",       "SUB",       "MUL",      "DIV",      "MOD",      "NEG",
                                  "NOT",      "EQ",        "NE",        "LT",       "LE",       "GT",       "GE",
                                  "IN",       "JMP",       "JMPIF",     "JMPIFNOT", "NEWLIST",  "NEWMAP",   "MAKEVEC",
                                  "CONCAT",   "CALL",      "CALLM",     "CALLF",    "RET",      "RETNONE",  "EVERY",
                                  "AFTER",    "FORPREP",   "FORLOOP",   "ITERPREP", "ITERNEXT", "WAIT",     "GOTO",
                                  "STOP",     "EXPECT"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Op::Count), "opName table out of date");
    auto i = static_cast<size_t>(op);
    return i < static_cast<size_t>(Op::Count) ? names[i] : "?";
}

uint64_t fnv1a(std::string_view data, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

size_t Program::instructionCount() const {
    size_t n = 0;
    for (const auto& p : protos) n += p.code.size();
    return n;
}

namespace {
std::string rk(const Program& p, uint16_t x) {
    if (isK(x)) {
        const Value& k = p.constants[x & 0x7fff];
        std::string s = toDisplayString(k);
        if (k.isString()) s = "\"" + s + "\"";
        return "K" + std::to_string(x & 0x7fff) + "(" + (s.size() > 24 ? s.substr(0, 24) + "..." : s) + ")";
    }
    return "R" + std::to_string(x);
}
}  // namespace

std::string Program::disassemble() const {
    std::ostringstream os;
    for (size_t i = 0; i < protos.size(); ++i) {
        const Proto& p = protos[i];
        os << "proto " << i << " " << p.name << " (params " << p.numParams << ", regs " << p.numRegs << ")\n";
        for (size_t pc = 0; pc < p.code.size(); ++pc) {
            const Ins& in = p.code[pc];
            char head[48];
            std::snprintf(head, sizeof(head), "  %4zu  L%-4d %-10s", pc, p.locs[pc].line, opName(in.op));
            os << head;
            auto target = [&]() { return static_cast<int64_t>(pc) + 1 + in.sbx(); };
            switch (in.op) {
                case Op::Jmp: os << "-> " << target(); break;
                case Op::JmpIf:
                case Op::JmpIfNot:
                case Op::ForPrep:
                case Op::ForLoop:
                case Op::IterPrep:
                case Op::IterNext: os << "R" << in.a << " -> " << target(); break;
                case Op::LoadK: os << "R" << in.a << " " << rk(*this, static_cast<uint16_t>(in.b | kConstBit)); break;
                case Op::GetMember: os << "R" << in.a << " R" << in.b << " ." << members[in.c].name; break;
                case Op::SetMember: os << "R" << in.a << " ." << members[in.b].name << " " << rk(*this, in.c); break;
                case Op::GetField:
                    os << "R" << in.a << " R" << in.b << " ." << fields[in.c].component << "." << fields[in.c].field;
                    break;
                case Op::SetField:
                    os << "R" << in.a << " ." << fields[in.b].component << "." << fields[in.b].field << " " << rk(*this, in.c);
                    break;
                case Op::Call: os << "R" << in.a << " " << builtins[in.b]->name << "/" << in.c; break;
                case Op::CallM: os << "R" << in.a << " ." << methodNames[in.b] << "/" << in.c; break;
                case Op::CallF: os << "R" << in.a << " " << functions[in.b].name << "/" << in.c; break;
                case Op::SetVar: os << "var" << in.a << " " << rk(*this, in.b); break;
                case Op::Every:
                case Op::After: os << "R" << in.a << " " << rk(*this, in.b) << " timer" << in.c; break;
                case Op::Add:
                case Op::Sub:
                case Op::Mul:
                case Op::Div:
                case Op::Mod:
                case Op::Eq:
                case Op::Ne:
                case Op::Lt:
                case Op::Le:
                case Op::Gt:
                case Op::Ge:
                case Op::In: os << "R" << in.a << " " << rk(*this, in.b) << " " << rk(*this, in.c); break;
                case Op::Wait: os << rk(*this, in.b) << (in.x ? " frames" : " s"); break;
                default: os << in.a << " " << in.b << " " << in.c; break;
            }
            os << "\n";
        }
    }
    return os.str();
}

}  // namespace sky::wander
