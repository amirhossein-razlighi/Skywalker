// Canonical printer for Wander syntax trees: format(parse(format(m))) == format(m).
// Used by the graph view (graph -> code) and to normalize sources.

#include <cmath>
#include <cstdio>
#include <sstream>
#include <unordered_set>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/wander/Parser.h"

namespace sky::wander {

namespace {

int precedence(const Expr& e) {
    if (e.kind == Expr::Kind::Binary) {
        const std::string& op = e.text;
        if (op == "or") return 1;
        if (op == "and") return 2;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 4;  // comparisons and `in`
    }
    if (e.kind == Expr::Kind::Unary) return e.text == "not" ? 3 : 7;
    if (e.kind == Expr::Kind::Number && e.number < 0) return 7;  // prints as "-3"
    return 9;
}

std::string number(double d) {
    if (std::isnan(d)) return "(0 / 0)";
    if (std::isinf(d)) return d > 0 ? "(1 / 0)" : "(-1 / 0)";
    if (d == std::floor(d) && std::fabs(d) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", d);
        return std::string(buf) == "-0" ? "0" : buf;
    }
    for (int digits = 6; digits <= 17; ++digits) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*g", digits, d);
        double back = 0;
        if (str::parseDouble(buf, back) && back == d) {
            std::string s = buf;
            // The lexer reads "1e-05" fine; make sure there is no leading '.'-less exponent issue.
            return s;
        }
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    return buf;
}

std::string escape(const std::string& s, bool interp) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '{': out += "\\{"; break;
            default: out += c;
        }
    }
    (void)interp;
    return out;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    static const std::unordered_set<std::string> reserved(reservedWords().begin(), reservedWords().end());
    return !reserved.count(s);
}

std::string expr(const Expr* e, int minPrec = 0);

std::string args(const std::vector<ExprPtr>& a) {
    std::string out;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i) out += ", ";
        out += expr(a[i].get());
    }
    return out;
}

std::string exprRaw(const Expr& e);

// The object of `.member`, `[index]` or `.method()`: numbers need parentheses ("(3).x").
std::string postfixBase(const Expr* e) {
    if (e && e->kind == Expr::Kind::Number && !e->parenthesized) return "(" + exprRaw(*e) + ")";
    return expr(e, 8);
}

std::string exprRaw(const Expr& e) {
    switch (e.kind) {
        case Expr::Kind::Number: return number(e.number);
        case Expr::Kind::String: return "\"" + escape(e.text, false) + "\"";
        case Expr::Kind::Interp: {
            std::string out = "\"";
            for (size_t i = 0; i < e.parts.size(); ++i) {
                out += escape(e.parts[i], true);
                if (i < e.args.size()) out += "{" + expr(e.args[i].get()) + "}";
            }
            return out + "\"";
        }
        case Expr::Kind::Bool: return e.number != 0 ? "true" : "false";
        case Expr::Kind::None: return "none";
        case Expr::Kind::Color: return reflect::toHexColor(e.color);
        case Expr::Kind::Vector: return "(" + args(e.args) + ")";
        case Expr::Kind::List: return "[" + args(e.args) + "]";
        case Expr::Kind::Map: {
            std::string out = "{";
            for (size_t i = 0; i < e.parts.size() && i < e.args.size(); ++i) {
                if (i) out += ", ";
                out += isIdentifier(e.parts[i]) ? e.parts[i] : "\"" + escape(e.parts[i], false) + "\"";
                out += ": " + expr(e.args[i].get());
            }
            return out + "}";
        }
        case Expr::Kind::Ident: return e.text;
        case Expr::Kind::Member: return postfixBase(e.lhs.get()) + "." + e.text;
        case Expr::Kind::Index: return postfixBase(e.lhs.get()) + "[" + expr(e.rhs.get()) + "]";
        case Expr::Kind::Call: return e.text + "(" + args(e.args) + ")";
        case Expr::Kind::MethodCall: return postfixBase(e.lhs.get()) + "." + e.text + "(" + args(e.args) + ")";
        case Expr::Kind::Unary:
            if (e.text == "not") return "not " + expr(e.lhs.get(), 3);
            // "-x"; a bare literal operand would fold into a negative number, so keep it grouped.
            if (e.lhs && e.lhs->kind == Expr::Kind::Number && !e.lhs->parenthesized) return "-(" + exprRaw(*e.lhs) + ")";
            return "-" + expr(e.lhs.get(), 7);
        case Expr::Kind::Binary: {
            int p = precedence(e);
            return expr(e.lhs.get(), p) + " " + e.text + " " + expr(e.rhs.get(), p + 1);
        }
    }
    return "none";
}

std::string expr(const Expr* e, int minPrec) {
    if (!e) return "none";
    std::string s = exprRaw(*e);
    if (e->parenthesized && e->kind != Expr::Kind::Vector) return "(" + s + ")";
    if (precedence(*e) < minPrec) return "(" + s + ")";
    return s;
}

class Printer {
public:
    std::string str() const { return out_.str(); }

    void module(const Module& m) {
        for (const auto& u : m.uses) {
            comments(u.comments, 0);
            std::string stem = u.path;
            if (auto slash = stem.find_last_of('/'); slash != std::string::npos) stem = stem.substr(slash + 1);
            if (stem.size() > 7 && stem.substr(stem.size() - 7) == ".wander") stem.resize(stem.size() - 7);
            line(0, "use \"" + escape(u.path, false) + "\"" + (u.alias != stem && !u.alias.empty() ? " as " + u.alias : ""));
        }
        if (!m.uses.empty()) blank();
        for (const auto& c : m.consts) constDecl(c, 0);
        if (!m.consts.empty()) blank();
        for (const auto& f : m.fns) {
            fnDecl(f, 0);
            blank();
        }
        for (const auto& b : m.behaviors) {
            if (b.implicit) {
                comments(b.comments, 0);
                members(b, 0);
            } else {
                comments(b.comments, 0);
                std::string name = isIdentifier(b.name) ? b.name : "\"" + escape(b.name, false) + "\"";
                line(0, "behavior " + name);
                members(b, 1);
                line(0, "end");
            }
            blank();
        }
        for (const auto& t : m.tests) {
            testDecl(t, 0);
            blank();
        }
        comments(m.trailingComments, 0);
    }

private:
    void line(int indent, const std::string& text) {
        for (int i = 0; i < indent; ++i) out_ << "  ";
        out_ << text << "\n";
        lastBlank_ = false;
    }
    void blank() {
        if (!lastBlank_) out_ << "\n";
        lastBlank_ = true;
    }
    void comments(const std::vector<std::string>& cs, int indent) {
        for (const auto& c : cs) line(indent, c);
    }

    void members(const BehaviorDef& b, int indent) {
        if (!b.intent.empty()) line(indent, "intent \"" + escape(b.intent, false) + "\"");
        for (const auto& v : b.vars) {
            comments(v.comments, indent);
            std::string s = std::string(v.isParam ? "param " : "var ") + v.name;
            if (!v.type.empty()) s += ": " + v.type.text;
            s += " = " + expr(v.initial.get());
            if (v.isParam && v.minValue && v.maxValue) s += " in " + expr(v.minValue.get(), 5) + ".." + expr(v.maxValue.get(), 5);
            if (v.isParam && !v.doc.empty()) s += " \"" + escape(v.doc, false) + "\"";
            line(indent, s);
        }
        for (const auto& c : b.consts) constDecl(c, indent);
        bool first = b.intent.empty() && b.vars.empty() && b.consts.empty();
        auto sep = [&] {
            if (!first) blank();
            first = false;
        };
        for (const auto& f : b.fns) {
            sep();
            fnDecl(f, indent);
        }
        for (const auto& h : b.handlers) {
            sep();
            handler(h, indent);
        }
        for (const auto& s : b.states) {
            sep();
            comments(s.comments, indent);
            line(indent, "state " + s.name);
            for (size_t i = 0; i < s.handlers.size(); ++i) {
                handler(s.handlers[i], indent + 1);
            }
            line(indent, "end");
        }
        for (const auto& t : b.tests) {
            sep();
            testDecl(t, indent);
        }
    }

    void constDecl(const ConstDecl& c, int indent) {
        comments(c.comments, indent);
        line(indent, "const " + c.name + (c.type.empty() ? "" : ": " + c.type.text) + " = " + expr(c.value.get()));
    }

    void fnDecl(const FnDecl& f, int indent) {
        comments(f.comments, indent);
        std::string s = "fn " + f.name + "(";
        for (size_t i = 0; i < f.params.size(); ++i) {
            if (i) s += ", ";
            s += f.params[i].name;
            if (!f.params[i].type.empty()) s += ": " + f.params[i].type.text;
        }
        s += ")";
        if (!f.returns.empty()) s += " -> " + f.returns.text;
        line(indent, s);
        block(f.body, indent + 1);
        line(indent, "end");
    }

    void testDecl(const TestDecl& t, int indent) {
        comments(t.comments, indent);
        line(indent, "test \"" + escape(t.name, false) + "\"");
        block(t.body, indent + 1);
        line(indent, "end");
    }

    void handler(const Handler& h, int indent) {
        comments(h.comments, indent);
        std::string s = "on ";
        if (h.custom) {
            s += h.argument;
        } else {
            s += toString(h.trigger);
            if (h.trigger == Trigger::Event || h.trigger == Trigger::Key || h.trigger == Trigger::Action ||
                !h.argument.empty()) {
                s += " \"" + escape(h.argument, false) + "\"";
            }
        }
        if (!h.binding.empty()) s += " with " + h.binding;
        line(indent, s);
        block(h.body, indent + 1);
        line(indent, "end");
    }

    void block(const Block& b, int indent) {
        for (const auto& s : b) statement(*s, indent);
    }

    void statement(const Stmt& s, int in) {
        comments(s.comments, in);
        auto E = [](const ExprPtr& e) { return expr(e.get()); };
        switch (s.kind) {
            case Stmt::Kind::Let:
            case Stmt::Kind::Const:
                line(in, std::string(s.kind == Stmt::Kind::Let ? "let " : "const ") + s.name +
                             (s.type.empty() ? "" : ": " + s.type.text) + " = " + E(s.value));
                return;
            case Stmt::Kind::Assign: line(in, E(s.target) + " = " + E(s.value)); return;
            case Stmt::Kind::OpAssign: line(in, E(s.target) + " " + s.name + "= " + E(s.value)); return;
            case Stmt::Kind::If: {
                for (size_t i = 0; i < s.branches.size(); ++i) {
                    const auto& [cond, body] = s.branches[i];
                    if (!cond) {
                        line(in, "else");
                    } else {
                        line(in, std::string(i == 0 ? "if " : "elif ") + expr(cond.get()) + " then");
                    }
                    block(body, in + 1);
                }
                line(in, "end");
                return;
            }
            case Stmt::Kind::While:
                line(in, "while " + E(s.value));
                block(s.body, in + 1);
                line(in, "end");
                return;
            case Stmt::Kind::For: {
                std::string h = "for " + s.name + (s.name2.empty() ? "" : ", " + s.name2) + " in ";
                if (s.isRange) {
                    h += expr(s.value.get(), 5) + (s.inclusive ? "..=" : "..") + expr(s.extra.get(), 5);
                    if (s.extra2) h += " step " + E(s.extra2);
                } else {
                    h += E(s.value);
                }
                line(in, h);
                block(s.body, in + 1);
                line(in, "end");
                return;
            }
            case Stmt::Kind::Repeat:
                line(in, "repeat " + E(s.value) + " times");
                block(s.body, in + 1);
                line(in, "end");
                return;
            case Stmt::Kind::Every:
            case Stmt::Kind::After:
                line(in, std::string(s.kind == Stmt::Kind::Every ? "every " : "after ") + E(s.value) + " seconds");
                block(s.body, in + 1);
                line(in, "end");
                return;
            case Stmt::Kind::Wait:
                if (s.waitKind == WaitKind::Until) line(in, "wait until " + E(s.value));
                else if (s.waitKind == WaitKind::Frames) line(in, "wait frames " + E(s.value));
                else line(in, "wait " + E(s.value));
                return;
            case Stmt::Kind::Break: line(in, "break"); return;
            case Stmt::Kind::Continue: line(in, "continue"); return;
            case Stmt::Kind::Return: line(in, s.value ? "return " + E(s.value) : "return"); return;
            case Stmt::Kind::Stop: line(in, "stop"); return;
            case Stmt::Kind::GoTo: line(in, "go to " + s.name); return;
            case Stmt::Kind::Move: line(in, "move " + E(s.target) + " by " + E(s.value)); return;
            case Stmt::Kind::MoveToward: line(in, "move " + E(s.target) + " toward " + E(s.value) + " at " + E(s.extra)); return;
            case Stmt::Kind::Rotate: line(in, "rotate " + E(s.target) + " by " + E(s.value)); return;
            case Stmt::Kind::Look: line(in, "look " + E(s.target) + " at " + E(s.value)); return;
            case Stmt::Kind::Emit: {
                std::string t = "emit \"" + escape(s.name, false) + "\"";
                if (s.value) t += " with " + E(s.value);
                if (s.extra) t += " to " + E(s.extra);
                line(in, t);
                return;
            }
            case Stmt::Kind::Destroy: line(in, "destroy " + E(s.target)); return;
            case Stmt::Kind::Log: line(in, "log " + E(s.value)); return;
            case Stmt::Kind::Call: line(in, E(s.value)); return;
            case Stmt::Kind::Expect: line(in, "expect " + E(s.value) + (s.extra ? " " + E(s.extra) : "")); return;
            case Stmt::Kind::Press: line(in, "press " + E(s.value)); return;
            case Stmt::Kind::Hold: line(in, "hold " + E(s.value)); return;
            case Stmt::Kind::Release: line(in, "release " + E(s.value)); return;
            case Stmt::Kind::Click: line(in, "click " + E(s.value)); return;
        }
    }

    std::ostringstream out_;
    bool lastBlank_ = true;
};

}  // namespace

std::string formatExpr(const Expr& e) { return expr(&e); }

std::string format(const Module& module) {
    Printer p;
    p.module(module);
    std::string s = p.str();
    while (!s.empty() && (s.back() == '\n')) s.pop_back();
    return s + "\n";
}

}  // namespace sky::wander
