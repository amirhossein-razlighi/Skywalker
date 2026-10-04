#include "skywalker/ui/Dialogue.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

#include "skywalker/core/Strings.h"

namespace sky::dialogue {

struct Expr {
    enum class Kind { Number, String, Bool, Null, Var, Call, Unary, Binary };
    Kind kind = Kind::Null;
    double number = 0;
    std::string text;  // string literal / variable name / function name / operator
    bool boolean = false;
    std::vector<ExprPtr> args;
    ExprPtr lhs, rhs;
};

Json Diagnostic::toJson() const {
    Json j = Json::object({{"severity", error ? "error" : "warning"}, {"line", line}, {"code", code}, {"message", message}});
    if (!hint.empty()) j["hint"] = hint;
    return j;
}

bool Script::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& d) { return d.error; });
}

const DialogueNode* Script::find(const std::string& title) const {
    auto it = index.find(title);
    return it == index.end() ? nullptr : &nodes[static_cast<size_t>(it->second)];
}

std::vector<std::string> Script::titles() const {
    std::vector<std::string> t;
    for (const auto& n : nodes) t.push_back(n.title);
    return t;
}

namespace {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

const std::set<std::string>& knownFunctions() {
    static const std::set<std::string> f{"visited", "visited_count", "random", "random_range", "dice", "round",
                                         "floor",   "ceil",          "abs",    "min",          "max",  "string", "number"};
    return f;
}

struct Tok {
    enum class K { Num, Str, Var, Ident, Op, End } k = K::End;
    std::string s;
    double n = 0;
};

class ExprParser {
public:
    ExprParser(std::string_view src, Script& script) : script_(script) { lex(src); }

    ExprPtr parse() {
        if (!error_.empty()) return nullptr;
        ExprPtr e = orExpr();
        if (error_.empty() && peek().k != Tok::K::End) fail("unexpected '" + peek().s + "' in expression");
        return error_.empty() ? e : nullptr;
    }
    const std::string& error() const { return error_; }

private:
    void fail(std::string m) {
        if (error_.empty()) error_ = std::move(m);
    }

    void lex(std::string_view s) {
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            Tok t;
            if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                size_t j = i;
                while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
                t.k = Tok::K::Num;
                if (!str::parseDouble(s.substr(i, j - i), t.n)) fail("bad number '" + std::string(s.substr(i, j - i)) + "'");
                i = j;
            } else if (c == '"') {
                size_t j = i + 1;
                std::string v;
                while (j < s.size() && s[j] != '"') {
                    if (s[j] == '\\' && j + 1 < s.size()) ++j;
                    v += s[j++];
                }
                if (j >= s.size()) fail("unterminated string");
                t.k = Tok::K::Str;
                t.s = v;
                i = j + 1;
            } else if (c == '$' || std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t j = i + 1;
                while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.')) ++j;
                t.k = c == '$' ? Tok::K::Var : Tok::K::Ident;
                t.s = std::string(s.substr(c == '$' ? i + 1 : i, j - (c == '$' ? i + 1 : i)));
                if (c == '$' && t.s.empty()) fail("'$' must be followed by a variable name");
                i = j;
            } else {
                static const char* ops[] = {"==", "!=", "<=", ">=", "&&", "||", "+", "-", "*", "/", "%", "<", ">", "!", "(", ")", ",", "="};
                bool matched = false;
                for (const char* op : ops) {
                    size_t len = std::char_traits<char>::length(op);
                    if (s.substr(i, len) == op) {
                        t.k = Tok::K::Op;
                        t.s = op;
                        i += len;
                        matched = true;
                        break;
                    }
                }
                if (!matched) {
                    fail(std::string("unexpected character '") + c + "'");
                    ++i;
                    continue;
                }
            }
            toks_.push_back(t);
        }
        toks_.push_back(Tok{});
    }

    const Tok& peek() const { return toks_[std::min(pos_, toks_.size() - 1)]; }
    Tok next() { return toks_[std::min(pos_++, toks_.size() - 1)]; }
    bool isOp(const char* op) const { return peek().k == Tok::K::Op && peek().s == op; }
    bool isWord(const char* w) const { return peek().k == Tok::K::Ident && str::lower(peek().s) == w; }

    static ExprPtr bin(std::string op, ExprPtr a, ExprPtr b) {
        auto e = std::make_shared<Expr>();
        e->kind = Expr::Kind::Binary;
        e->text = std::move(op);
        e->lhs = std::move(a);
        e->rhs = std::move(b);
        return e;
    }

    ExprPtr orExpr() {
        ExprPtr l = andExpr();
        while (isOp("||") || isWord("or") || isWord("xor")) {
            std::string op = isWord("xor") ? "xor" : "or";
            next();
            l = bin(op, l, andExpr());
        }
        return l;
    }
    ExprPtr andExpr() {
        ExprPtr l = notExpr();
        while (isOp("&&") || isWord("and")) {
            next();
            l = bin("and", l, notExpr());
        }
        return l;
    }
    ExprPtr notExpr() {
        if (isOp("!") || isWord("not")) {
            next();
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Unary;
            e->text = "not";
            e->lhs = notExpr();
            return e;
        }
        return cmpExpr();
    }
    ExprPtr cmpExpr() {
        ExprPtr l = addExpr();
        static const std::vector<std::pair<std::string, std::string>> words{
            {"eq", "=="}, {"is", "=="}, {"neq", "!="}, {"lt", "<"}, {"gt", ">"}, {"lte", "<="}, {"gte", ">="}};
        std::string op;
        if (peek().k == Tok::K::Op && (peek().s == "==" || peek().s == "!=" || peek().s == "<" || peek().s == ">" ||
                                       peek().s == "<=" || peek().s == ">=" || peek().s == "=")) {
            op = peek().s == "=" ? "==" : peek().s;
        } else if (peek().k == Tok::K::Ident) {
            for (const auto& [w, o] : words) {
                if (str::lower(peek().s) == w) op = o;
            }
        }
        if (op.empty()) return l;
        next();
        return bin(op, l, addExpr());
    }
    ExprPtr addExpr() {
        ExprPtr l = mulExpr();
        while (isOp("+") || isOp("-")) {
            std::string op = next().s;
            l = bin(op, l, mulExpr());
        }
        return l;
    }
    ExprPtr mulExpr() {
        ExprPtr l = unary();
        while (isOp("*") || isOp("/") || isOp("%")) {
            std::string op = next().s;
            l = bin(op, l, unary());
        }
        return l;
    }
    ExprPtr unary() {
        if (isOp("-")) {
            next();
            auto e = std::make_shared<Expr>();
            e->kind = Expr::Kind::Unary;
            e->text = "-";
            e->lhs = unary();
            return e;
        }
        return primary();
    }
    ExprPtr primary() {
        Tok t = next();
        auto e = std::make_shared<Expr>();
        switch (t.k) {
            case Tok::K::Num:
                e->kind = Expr::Kind::Number;
                e->number = t.n;
                return e;
            case Tok::K::Str:
                e->kind = Expr::Kind::String;
                e->text = t.s;
                return e;
            case Tok::K::Var:
                e->kind = Expr::Kind::Var;
                e->text = t.s;
                script_.varsRead.insert(t.s);
                return e;
            case Tok::K::Ident: {
                std::string w = str::lower(t.s);
                if (w == "true" || w == "false") {
                    e->kind = Expr::Kind::Bool;
                    e->boolean = w == "true";
                    return e;
                }
                if (w == "null" || w == "none") return e;
                if (!isOp("(")) {
                    fail("unknown name '" + t.s + "' (variables start with $, strings are quoted)");
                    return e;
                }
                next();
                e->kind = Expr::Kind::Call;
                e->text = w;
                script_.functions.insert(w);
                if (!knownFunctions().count(w)) {
                    std::vector<std::string> names(knownFunctions().begin(), knownFunctions().end());
                    std::string guess = str::closest(w, names, 3);
                    fail("unknown function '" + t.s + "'" + (guess.empty() ? "" : " (did you mean '" + guess + "'?)"));
                }
                if (!isOp(")")) {
                    while (true) {
                        e->args.push_back(orExpr());
                        if (isOp(",")) {
                            next();
                            continue;
                        }
                        break;
                    }
                }
                if (!isOp(")")) fail("expected ')' to close " + t.s + "(");
                else next();
                return e;
            }
            case Tok::K::Op:
                if (t.s == "(") {
                    ExprPtr inner = orExpr();
                    if (!isOp(")")) fail("expected ')'");
                    else next();
                    return inner;
                }
                fail("unexpected '" + t.s + "'");
                return e;
            case Tok::K::End: fail("expression is incomplete"); return e;
        }
        return e;
    }

    std::vector<Tok> toks_;
    size_t pos_ = 0;
    std::string error_;
    Script& script_;
};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

bool truthy(const Json& v) {
    if (v.isBool()) return v.asBool();
    if (v.isNumber()) return v.asNumber() != 0;
    if (v.isString()) return !v.asString().empty();
    return false;
}

double toNumber(const Json& v) {
    if (v.isNumber()) return v.asNumber();
    if (v.isBool()) return v.asBool() ? 1 : 0;
    double d = 0;
    if (v.isString() && str::parseDouble(v.asString(), d)) return d;
    return 0;
}

std::string toText(const Json& v) {
    if (v.isString()) return v.asString();
    if (v.isBool()) return v.asBool() ? "true" : "false";
    if (v.isNumber()) {
        double d = v.asNumber();
        if (std::fabs(d - std::round(d)) < 1e-9 && std::fabs(d) < 1e15) return std::to_string(static_cast<long long>(std::llround(d)));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", d);
        return buf;
    }
    return "";
}

struct Eval {
    VarStore& vars;
    Random& rng;
    const std::map<std::string, int>& visits;
    std::string error;

    Json operator()(const ExprPtr& e) {
        if (!e) return {};
        switch (e->kind) {
            case Expr::Kind::Number: return e->number;
            case Expr::Kind::String: return e->text;
            case Expr::Kind::Bool: return e->boolean;
            case Expr::Kind::Null: return {};
            case Expr::Kind::Var: return vars.get ? vars.get(e->text) : Json();
            case Expr::Kind::Unary: {
                Json v = (*this)(e->lhs);
                if (e->text == "not") return !truthy(v);
                return -toNumber(v);
            }
            case Expr::Kind::Binary: {
                const std::string& op = e->text;
                if (op == "and") return truthy((*this)(e->lhs)) && truthy((*this)(e->rhs));
                if (op == "or") return truthy((*this)(e->lhs)) || truthy((*this)(e->rhs));
                Json a = (*this)(e->lhs), b = (*this)(e->rhs);
                if (op == "xor") return truthy(a) != truthy(b);
                if (op == "==" || op == "!=") {
                    bool eq;
                    if ((a.isNumber() || a.isBool()) && (b.isNumber() || b.isBool())) eq = toNumber(a) == toNumber(b);
                    else eq = toText(a) == toText(b) && a.isNull() == b.isNull();
                    return op == "==" ? eq : !eq;
                }
                if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                    if (a.isString() && b.isString()) {
                        int c = a.asString().compare(b.asString());
                        return op == "<" ? c < 0 : op == ">" ? c > 0 : op == "<=" ? c <= 0 : c >= 0;
                    }
                    double x = toNumber(a), y = toNumber(b);
                    return op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
                }
                if (op == "+" && (a.isString() || b.isString())) return toText(a) + toText(b);
                double x = toNumber(a), y = toNumber(b);
                if (op == "+") return x + y;
                if (op == "-") return x - y;
                if (op == "*") return x * y;
                if (op == "/") {
                    if (y == 0) {
                        error = "division by zero";
                        return 0.0;
                    }
                    return x / y;
                }
                if (op == "%") return y == 0 ? 0.0 : std::fmod(x, y);
                return {};
            }
            case Expr::Kind::Call: {
                std::vector<Json> a;
                for (const auto& arg : e->args) a.push_back((*this)(arg));
                auto num = [&](size_t i) { return i < a.size() ? toNumber(a[i]) : 0.0; };
                const std::string& f = e->text;
                if (f == "visited" || f == "visited_count") {
                    auto it = visits.find(a.empty() ? "" : toText(a[0]));
                    int n = it == visits.end() ? 0 : it->second;
                    return f == "visited" ? Json(n > 0) : Json(n);
                }
                if (f == "random") return static_cast<double>(rng.nextFloat());
                if (f == "random_range") {
                    double lo = num(0), hi = num(1);
                    return std::floor(lo + (hi - lo + 1) * static_cast<double>(rng.nextFloat()));
                }
                if (f == "dice") return std::floor(1 + std::max(1.0, num(0)) * static_cast<double>(rng.nextFloat()));
                if (f == "round") return std::round(num(0));
                if (f == "floor") return std::floor(num(0));
                if (f == "ceil") return std::ceil(num(0));
                if (f == "abs") return std::fabs(num(0));
                if (f == "min") return std::min(num(0), num(1));
                if (f == "max") return std::max(num(0), num(1));
                if (f == "string") return a.empty() ? std::string() : toText(a[0]);
                if (f == "number") return num(0);
                return {};
            }
        }
        return {};
    }

    std::string text(const TextTemplate& t) {
        std::string out;
        for (const auto& p : t.parts) out += p.expr ? toText((*this)(p.expr)) : p.literal;
        return out;
    }
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

struct Entry {
    int indent = 0;
    int line = 0;
    std::string text;
};

class Parser {
public:
    explicit Parser(Script& s) : s_(s) {}

    void error(int line, std::string code, std::string message, std::string hint = {}) {
        s_.diagnostics.push_back({true, line, std::move(code), std::move(message), std::move(hint)});
    }
    void warn(int line, std::string code, std::string message, std::string hint = {}) {
        s_.diagnostics.push_back({false, line, std::move(code), std::move(message), std::move(hint)});
    }

    ExprPtr expr(std::string_view src, int line) {
        ExprParser p(src, s_);
        ExprPtr e = p.parse();
        if (!e) error(line, "invalid_expression", p.error() + " in \"" + std::string(src) + "\"");
        return e;
    }

    TextTemplate tmpl(std::string_view src, int line) {
        TextTemplate t;
        t.source = std::string(src);
        std::string lit;
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '\\' && i + 1 < src.size() && (src[i + 1] == '{' || src[i + 1] == '}' || src[i + 1] == '#')) {
                lit += src[++i];
            } else if (src[i] == '{') {
                size_t close = src.find('}', i);
                if (close == std::string_view::npos) {
                    error(line, "unclosed_brace", "'{' without a matching '}'", "write \\{ for a literal brace");
                    lit += src.substr(i);
                    break;
                }
                if (!lit.empty()) t.parts.push_back({lit, nullptr});
                lit.clear();
                t.parts.push_back({"", expr(src.substr(i + 1, close - i - 1), line)});
                i = close;
            } else {
                lit += src[i];
            }
        }
        if (!lit.empty()) t.parts.push_back({lit, nullptr});
        return t;
    }

    /// Splits trailing "#tags" and a trailing "<<if cond>>" off a line.
    void trailing(std::string& text, Json& tags, ExprPtr& cond, int line) {
        // Tags: whitespace-separated tokens at the end that start with '#' (a lone "#..." stays text).
        for (;;) {
            std::string t = str::trim(text);
            size_t sp = t.find_last_of(" \t");
            if (sp == std::string::npos) break;
            std::string last = t.substr(sp + 1);
            if (last.size() < 2 || last[0] != '#') break;
            std::string body = last.substr(1);
            size_t colon = body.find(':');
            if (colon == std::string::npos) tags[body] = true;
            else tags[body.substr(0, colon)] = body.substr(colon + 1);
            text = t.substr(0, sp);
        }
        std::string t = str::trim(text);
        if (t.size() > 4 && t.compare(t.size() - 2, 2, ">>") == 0) {
            size_t open = t.rfind("<<");
            if (open != std::string::npos) {
                std::string inner = str::trim(t.substr(open + 2, t.size() - open - 4));
                if (str::startsWith(inner, "if ")) {
                    cond = expr(inner.substr(3), line);
                    t = str::trim(t.substr(0, open));
                }
            }
        }
        text = t;
    }

    static bool isCommand(const Entry& e, const char* word) {
        if (!str::startsWith(e.text, "<<")) return false;
        std::string inner = str::trim(e.text.substr(2, e.text.size() >= 4 ? e.text.size() - 4 : 0));
        std::string w = inner.substr(0, inner.find_first_of(" \t"));
        return w == word;
    }

    void node(DialogueNode& n, std::vector<Entry> entries) {
        node_ = &n;
        entries_ = std::move(entries);
        i_ = 0;
        block(0, false);
        while (i_ < entries_.size()) {  // stray terminators at the top level
            error(entries_[i_].line, "unexpected", "unexpected \"" + entries_[i_].text + "\"",
                  "<<elseif>>, <<else>> and <<endif>> must follow an <<if>>");
            ++i_;
            block(0, false);
        }
    }

private:
    std::vector<Instr>& code() { return node_->code; }

    void block(int minIndent, bool /*inIf*/) {
        while (i_ < entries_.size()) {
            const Entry& e = entries_[i_];
            // <<elseif>>, <<else>>, <<endif>> end a block: ifBlock() consumes them, node() reports strays.
            if (isCommand(e, "elseif") || isCommand(e, "else") || isCommand(e, "endif")) return;
            if (e.indent < minIndent) return;
            if (str::startsWith(e.text, "->")) {
                options(e.indent);
            } else if (str::startsWith(e.text, "<<")) {
                command();
            } else {
                lineStmt();
            }
        }
    }

    void options(int indent) {
        const size_t ci = code().size();
        Instr group;
        group.op = Instr::Op::Choices;
        group.line = entries_[i_].line;
        code().push_back(group);
        std::vector<size_t> fixups;
        while (i_ < entries_.size() && entries_[i_].indent == indent && str::startsWith(entries_[i_].text, "->")) {
            const Entry& e = entries_[i_];
            Option opt;
            opt.line = e.line;
            std::string t = str::trim(e.text.substr(2));
            trailing(t, opt.tags, opt.condition, e.line);
            if (t.empty()) error(e.line, "empty_option", "this option has no text");
            opt.text = tmpl(t, e.line);
            ++i_;
            opt.target = static_cast<int>(code().size());
            // The option's body: following lines indented deeper than the option.
            if (i_ < entries_.size() && entries_[i_].indent > indent) block(entries_[i_].indent, false);
            fixups.push_back(code().size());
            Instr go;
            go.op = Instr::Op::Goto;
            go.line = e.line;
            code().push_back(go);
            code()[ci].options.push_back(std::move(opt));
        }
        const int end = static_cast<int>(code().size());
        for (size_t f : fixups) code()[f].target = end;
        code()[ci].target = end;
    }

    void lineStmt() {
        const Entry& e = entries_[i_++];
        Instr in;
        in.op = Instr::Op::Line;
        in.line = e.line;
        std::string t = e.text;
        trailing(t, in.tags, in.condition, e.line);
        // "Speaker: text" (a short name before the first colon, not part of an expression or tag).
        size_t colon = t.find(':');
        if (colon != std::string::npos && colon > 0 && colon <= 48 && colon + 1 < t.size() && t[colon + 1] == ' ') {
            std::string who = str::trim(t.substr(0, colon));
            bool name = !who.empty() && who.find_first_of("{}<>#\"") == std::string::npos;
            if (name) {
                in.speaker = who;
                t = str::trim(t.substr(colon + 1));
            }
        }
        in.text = tmpl(t, e.line);
        code().push_back(std::move(in));
    }

    void command() {
        const Entry& e = entries_[i_];
        std::string t = e.text;
        Json tags = Json::object();
        ExprPtr unusedCond;
        if (t.size() < 4 || t.compare(t.size() - 2, 2, ">>") != 0) {
            // Trailing tags after a command are allowed.
            trailing(t, tags, unusedCond, e.line);
        }
        if (t.size() < 4 || t.compare(t.size() - 2, 2, ">>") != 0) {
            error(e.line, "unclosed_command", "command is missing '>>': " + e.text);
            ++i_;
            return;
        }
        std::string inner = str::trim(t.substr(2, t.size() - 4));
        std::string word = inner.substr(0, inner.find_first_of(" \t"));
        std::string rest = str::trim(inner.substr(word.size()));
        Instr in;
        in.line = e.line;
        if (word == "if") {
            ++i_;
            ifBlock(rest, e.line, e.indent);
            return;
        }
        ++i_;
        if (word == "set" || word == "declare") {
            in.op = word == "set" ? Instr::Op::Set : Instr::Op::Declare;
            if (rest.empty() || rest[0] != '$') {
                error(e.line, "invalid_set", "<<" + word + ">> needs a $variable, e.g. <<" + word + " $trust to 1>>");
                return;
            }
            size_t j = 1;
            while (j < rest.size() && (std::isalnum(static_cast<unsigned char>(rest[j])) || rest[j] == '_' || rest[j] == '.')) ++j;
            in.var = rest.substr(1, j - 1);
            std::string tail = str::trim(rest.substr(j));
            static const char* ops[] = {"+=", "-=", "*=", "/=", "="};
            in.assignOp.clear();
            for (const char* op : ops) {
                if (str::startsWith(tail, op)) {
                    in.assignOp = op;
                    tail = str::trim(tail.substr(std::char_traits<char>::length(op)));
                    break;
                }
            }
            if (in.assignOp.empty() && str::startsWith(tail, "to ")) {
                in.assignOp = "=";
                tail = str::trim(tail.substr(3));
            }
            if (in.assignOp.empty()) {
                error(e.line, "invalid_set", "expected 'to', '=', '+=', '-=', '*=' or '/=' after $" + in.var);
                return;
            }
            in.value = expr(tail, e.line);
            s_.varsWritten.insert(in.var);
            if (in.assignOp != "=") s_.varsRead.insert(in.var);
            if (in.op == Instr::Op::Declare) {
                VarStore none;
                Random rng;
                std::map<std::string, int> visits;
                Eval ev{none, rng, visits, {}};
                s_.declared[in.var] = ev(in.value);
            }
            code().push_back(std::move(in));
        } else if (word == "jump") {
            in.op = Instr::Op::Jump;
            in.node = rest;
            if (rest.empty()) error(e.line, "invalid_jump", "<<jump>> needs a node title");
            s_.jumps[node_->title].insert(rest);
            jumpLines_.push_back({rest, e.line});
            code().push_back(std::move(in));
        } else if (word == "stop") {
            in.op = Instr::Op::Stop;
            code().push_back(std::move(in));
        } else if (word == "wait") {
            in.op = Instr::Op::Wait;
            in.value = expr(rest.empty() ? "1" : rest, e.line);
            code().push_back(std::move(in));
        } else if (word == "elseif" || word == "else" || word == "endif") {
            error(e.line, "unexpected", "<<" + word + ">> without <<if>>");
        } else if (word.empty()) {
            error(e.line, "empty_command", "empty command <<>>");
        } else {
            in.op = Instr::Op::Command;
            in.command = word;
            s_.commands.insert(word);
            // Arguments: whitespace separated, "quoted strings" kept together, {expr} interpolated.
            size_t j = 0;
            while (j < rest.size()) {
                while (j < rest.size() && std::isspace(static_cast<unsigned char>(rest[j]))) ++j;
                if (j >= rest.size()) break;
                std::string arg;
                if (rest[j] == '"') {
                    size_t close = rest.find('"', j + 1);
                    arg = rest.substr(j + 1, close == std::string::npos ? std::string::npos : close - j - 1);
                    j = close == std::string::npos ? rest.size() : close + 1;
                } else {
                    size_t end = rest.find_first_of(" \t", j);
                    arg = rest.substr(j, end == std::string::npos ? std::string::npos : end - j);
                    j = end == std::string::npos ? rest.size() : end;
                }
                in.args.push_back(tmpl(arg, e.line));
            }
            code().push_back(std::move(in));
        }
    }

    void ifBlock(const std::string& condSrc, int line, int indent) {
        std::vector<size_t> exits;
        size_t jif = code().size();
        Instr j;
        j.op = Instr::Op::JumpIfFalse;
        j.line = line;
        j.condition = expr(condSrc, line);
        code().push_back(std::move(j));
        bool hasJif = true;
        block(indent, true);
        bool closed = false;
        while (i_ < entries_.size()) {
            const Entry& e = entries_[i_];
            if (isCommand(e, "endif")) {
                ++i_;
                closed = true;
                break;
            }
            Instr go;
            go.op = Instr::Op::Goto;
            go.line = e.line;
            exits.push_back(code().size());
            code().push_back(go);
            if (hasJif) code()[jif].target = static_cast<int>(code().size());
            hasJif = false;
            std::string inner = str::trim(e.text.substr(2, e.text.size() >= 4 ? e.text.size() - 4 : 0));
            ++i_;
            if (str::startsWith(inner, "elseif")) {
                jif = code().size();
                Instr k;
                k.op = Instr::Op::JumpIfFalse;
                k.line = e.line;
                k.condition = expr(str::trim(inner.substr(6)), e.line);
                code().push_back(std::move(k));
                hasJif = true;
            }
            block(indent, true);
        }
        if (!closed) error(line, "missing_endif", "<<if>> without <<endif>>");
        if (hasJif) code()[jif].target = static_cast<int>(code().size());
        for (size_t x : exits) code()[x].target = static_cast<int>(code().size());
    }

public:
    std::vector<std::pair<std::string, int>> jumpLines_;

private:
    Script& s_;
    DialogueNode* node_ = nullptr;
    std::vector<Entry> entries_;
    size_t i_ = 0;
};

}  // namespace

TextTemplate parseTemplate(std::string_view text) {
    Script scratch;
    Parser parser(scratch);
    return parser.tmpl(text, 0);
}

std::shared_ptr<const Script> parse(std::string_view source) {
    auto script = std::make_shared<Script>();
    Parser parser(*script);
    std::vector<std::string> lines = str::split(source, '\n');
    enum class Mode { Headers, Body } mode = Mode::Headers;
    DialogueNode current;
    std::vector<Entry> body;
    bool haveNode = false;
    auto finish = [&](int endLine) {
        if (!haveNode) return;
        if (current.title.empty()) {
            parser.error(current.line, "missing_title", "node without a title: header", "start every node with \"title: Name\"");
        } else if (script->index.count(current.title)) {
            parser.error(current.line, "duplicate_node", "node \"" + current.title + "\" is defined twice");
        }
        if (!current.title.empty() && !script->index.count(current.title)) {
            script->index[current.title] = static_cast<int>(script->nodes.size());
        }
        script->nodes.push_back(std::move(current));
        parser.node(script->nodes.back(), std::move(body));
        (void)endLine;
        current = DialogueNode{};
        body.clear();
        haveNode = false;
        mode = Mode::Headers;
    };
    for (size_t li = 0; li < lines.size(); ++li) {
        std::string raw = lines[li];
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const int lineNo = static_cast<int>(li) + 1;
        int indent = 0;
        size_t k = 0;
        for (; k < raw.size() && (raw[k] == ' ' || raw[k] == '\t'); ++k) indent += raw[k] == '\t' ? 4 : 1;
        std::string text = str::trim(raw);
        if (text.empty() || str::startsWith(text, "//")) continue;
        if (mode == Mode::Headers) {
            if (text == "---") {
                if (!haveNode) parser.error(lineNo, "missing_title", "'---' without a title: header");
                mode = Mode::Body;
                haveNode = true;
                continue;
            }
            if (text == "===") {
                parser.error(lineNo, "unexpected", "'===' without a node body", "a node is: title: Name / --- / lines / ===");
                continue;
            }
            size_t colon = text.find(':');
            if (colon == std::string::npos) {
                parser.error(lineNo, "expected_header", "expected a header (\"title: Name\") or '---', got \"" + text + "\"",
                             "every node starts with title: Name, then ---, its lines, and === to close it");
                continue;
            }
            if (!haveNode) {
                current = DialogueNode{};
                current.line = lineNo;
                haveNode = true;
            }
            std::string key = str::trim(text.substr(0, colon)), value = str::trim(text.substr(colon + 1));
            if (key == "title") current.title = value;
            else current.headers[key] = value;
            continue;
        }
        if (text == "===") {
            finish(lineNo);
            continue;
        }
        body.push_back({indent, lineNo, text});
    }
    if (haveNode) {
        if (mode == Mode::Body) parser.warn(static_cast<int>(lines.size()), "missing_end", "the last node is not closed with '==='");
        else parser.error(current.line, "missing_body", "node \"" + current.title + "\" has headers but no '---' body");
        if (mode == Mode::Body) finish(static_cast<int>(lines.size()));
    }
    // Jump targets.
    for (const auto& [target, line] : parser.jumpLines_) {
        if (!script->index.count(target)) {
            std::string guess = str::closest(target, script->titles(), 3);
            parser.error(line, "unknown_node", "<<jump " + target + ">>: no node titled \"" + target + "\"",
                         guess.empty() ? "" : "did you mean \"" + guess + "\"?");
        }
    }
    std::stable_sort(script->diagnostics.begin(), script->diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.line < b.line; });
    return script;
}

Json lint(const Script& s, const std::string& startNode) {
    std::vector<Diagnostic> diags = s.diagnostics;
    if (s.nodes.empty()) diags.push_back({true, 1, "empty_script", "the script has no nodes", "title: Start / --- / lines / ==="});
    if (!startNode.empty() && !s.nodes.empty() && !s.index.count(startNode)) {
        std::string guess = str::closest(startNode, s.titles(), 3);
        diags.push_back({true, 1, "missing_start", "no node titled \"" + startNode + "\" to start from",
                         guess.empty() ? "nodes: " + str::trim([&] {
                             std::string all;
                             for (const auto& t : s.titles()) all += t + " ";
                             return all;
                         }())
                                       : "did you mean \"" + guess + "\"?"});
    }
    // Reachability from the start node through jumps.
    if (s.index.count(startNode)) {
        std::set<std::string> seen{startNode};
        std::deque<std::string> queue{startNode};
        while (!queue.empty()) {
            std::string cur = queue.front();
            queue.pop_front();
            if (auto it = s.jumps.find(cur); it != s.jumps.end()) {
                for (const auto& t : it->second) {
                    if (seen.insert(t).second) queue.push_back(t);
                }
            }
        }
        for (const auto& n : s.nodes) {
            if (!seen.count(n.title)) {
                diags.push_back({false, n.line, "unreachable", "node \"" + n.title + "\" is never jumped to from \"" + startNode + "\"",
                                 "fine if the game starts it directly with start_dialogue(\"" + n.title + "\")"});
            }
        }
    }
    for (const auto& n : s.nodes) {
        if (n.code.empty()) diags.push_back({false, n.line, "empty_node", "node \"" + n.title + "\" has no lines", ""});
    }
    for (const auto& v : s.varsRead) {
        if (!s.varsWritten.count(v) && !s.declared.count(v)) {
            diags.push_back({false, 0, "unset_variable", "$" + v + " is read but never set or declared",
                             "<<declare $" + v + " = 0>> at the start, or set it from Wander with dialogue_var(\"" + v + "\", value)"});
        }
    }
    int errors = 0, warnings = 0;
    Json arr = Json::array();
    for (const auto& d : diags) {
        (d.error ? errors : warnings)++;
        arr.push(d.toJson());
    }
    Json nodes = Json::array();
    for (const auto& n : s.nodes) {
        int lines = 0, choices = 0;
        for (const auto& in : n.code) {
            lines += in.op == Instr::Op::Line;
            choices += in.op == Instr::Op::Choices ? static_cast<int>(in.options.size()) : 0;
        }
        Json jumps = Json::array();
        if (auto it = s.jumps.find(n.title); it != s.jumps.end()) {
            for (const auto& t : it->second) jumps.push(t);
        }
        nodes.push(Json::object({{"title", n.title}, {"line", n.line}, {"lines", lines}, {"choices", choices}, {"jumps", jumps}}));
    }
    Json vars = Json::array();
    std::set<std::string> allVars = s.varsRead;
    allVars.insert(s.varsWritten.begin(), s.varsWritten.end());
    for (const auto& v : allVars) vars.push("$" + v);
    Json commands = Json::array();
    for (const auto& c : s.commands) commands.push(c);
    return Json::object({{"ok", errors == 0}, {"errors", errors}, {"warnings", warnings}, {"diagnostics", arr}, {"nodes", nodes},
                         {"variables", vars}, {"commands", commands}});
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

Runner::Runner(std::shared_ptr<const Script> script, uint64_t seed) : script_(std::move(script)), rng_(seed) {}

int Runner::visited(const std::string& node) const {
    auto it = visits_.find(node);
    return it == visits_.end() ? 0 : it->second;
}

void Runner::stop() {
    state_ = State::Ended;
    choices_.clear();
}

Status Runner::enter(const std::string& node, const RunnerEvents& events) {
    auto it = script_->index.find(node);
    if (it == script_->index.end()) {
        std::string guess = str::closest(node, script_->titles(), 3);
        state_ = State::Ended;
        return Error::make("unknown_node", "no dialogue node \"" + node + "\"", guess.empty() ? "" : "did you mean \"" + guess + "\"?");
    }
    node_ = node;
    nodeIndex_ = it->second;
    pc_ = 0;
    ++visits_[node];
    if (events.node) events.node(node);
    return {};
}

Status Runner::start(const std::string& node, VarStore& vars, const RunnerEvents& events) {
    state_ = State::Idle;
    choices_.clear();
    line_ = {};
    for (const auto& [name, value] : script_->declared) {
        if (vars.get && vars.set && vars.get(name).isNull()) vars.set(name, value);
    }
    if (Status s = enter(node, events); !s) return s;
    return run(vars, events);
}

Status Runner::advance(VarStore& vars, const RunnerEvents& events) {
    if (state_ != State::Line && state_ != State::Waiting) return {};
    return run(vars, events);
}

Status Runner::choose(int index, VarStore& vars, const RunnerEvents& events) {
    if (state_ != State::Choices) return Error::make("no_choices", "the dialogue is not waiting for a choice");
    if (index < 0 || index >= static_cast<int>(choices_.size())) {
        return Error::make("invalid_choice", "choice " + std::to_string(index) + " is out of range (0-" +
                                                 std::to_string(static_cast<int>(choices_.size()) - 1) + ")");
    }
    const Instr& group = script_->nodes[static_cast<size_t>(nodeIndex_)].code[static_cast<size_t>(choiceInstr_)];
    pc_ = group.options[static_cast<size_t>(choices_[static_cast<size_t>(index)].option)].target;
    choices_.clear();
    return run(vars, events);
}

Status Runner::update(float dt, VarStore& vars, const RunnerEvents& events) {
    if (state_ != State::Waiting) return {};
    wait_ -= dt;
    if (wait_ > 0.f) return {};
    return run(vars, events);
}

namespace {
/// The template a line or choice shows: its localized text (by `#line:<id>` tag, or "@key" text) when the
/// localizer has one, otherwise the script's own.
TextTemplate localized(const TextTemplate& t, const Json& tags, const RunnerEvents& events) {
    if (!events.localize) return t;
    const std::string id = tags.get("line").isString() ? tags.get("line").asString() : std::string();
    if (id.empty() && (t.source.empty() || t.source[0] != '@')) return t;
    std::string text = events.localize(id, t.source);
    return text == t.source ? t : parseTemplate(text);
}
}  // namespace

Status Runner::run(VarStore& vars, const RunnerEvents& events) {
    Eval ev{vars, rng_, visits_, {}};
    for (int steps = 0; steps < 100000; ++steps) {
        const auto& code = script_->nodes[static_cast<size_t>(nodeIndex_)].code;
        if (pc_ >= static_cast<int>(code.size())) {
            state_ = State::Ended;
            return {};
        }
        const Instr& in = code[static_cast<size_t>(pc_)];
        switch (in.op) {
            case Instr::Op::Line: {
                ++pc_;
                if (in.condition && !truthy(ev(in.condition))) break;
                line_.speaker = events.speaker && !in.speaker.empty() ? events.speaker(in.speaker) : in.speaker;
                line_.text = ev.text(localized(in.text, in.tags, events));
                line_.tags = in.tags;
                state_ = State::Line;
                return {};
            }
            case Instr::Op::Choices: {
                choices_.clear();
                for (size_t k = 0; k < in.options.size(); ++k) {
                    const Option& o = in.options[k];
                    if (o.condition && !truthy(ev(o.condition))) continue;
                    choices_.push_back({ev.text(localized(o.text, o.tags, events)), o.tags, static_cast<int>(k)});
                }
                if (choices_.empty()) {
                    pc_ = in.target;  // nothing available: skip the group
                    break;
                }
                choiceInstr_ = pc_;
                state_ = State::Choices;
                return {};
            }
            case Instr::Op::Jump:
                if (Status s = enter(in.node, events); !s) return s;
                break;
            case Instr::Op::JumpIfFalse: pc_ = truthy(ev(in.condition)) ? pc_ + 1 : in.target; break;
            case Instr::Op::Goto: pc_ = in.target; break;
            case Instr::Op::Set:
            case Instr::Op::Declare: {
                ++pc_;
                Json v = ev(in.value);
                if (in.op == Instr::Op::Declare && vars.get && !vars.get(in.var).isNull()) break;
                if (in.assignOp != "=") {
                    Json cur = vars.get ? vars.get(in.var) : Json();
                    double a = toNumber(cur), b = toNumber(v);
                    if (in.assignOp == "+=" && (cur.isString() || v.isString())) v = toText(cur) + toText(v);
                    else if (in.assignOp == "+=") v = a + b;
                    else if (in.assignOp == "-=") v = a - b;
                    else if (in.assignOp == "*=") v = a * b;
                    else if (in.assignOp == "/=") v = b == 0 ? 0.0 : a / b;
                }
                if (vars.set) vars.set(in.var, v);
                break;
            }
            case Instr::Op::Command: {
                ++pc_;
                std::vector<std::string> args;
                for (const auto& a : in.args) args.push_back(ev.text(a));
                if (events.command) events.command(in.command, args);
                break;
            }
            case Instr::Op::Wait: {
                ++pc_;
                wait_ = static_cast<float>(std::max(0.0, toNumber(ev(in.value))));
                state_ = State::Waiting;
                return {};
            }
            case Instr::Op::Stop: state_ = State::Ended; return {};
        }
        if (!ev.error.empty()) {
            state_ = State::Ended;
            return Error::make("dialogue_error", "line " + std::to_string(in.line) + ": " + ev.error);
        }
    }
    state_ = State::Ended;
    return Error::make("dialogue_loop", "the dialogue loops without showing a line (node \"" + node_ + "\")",
                       "make sure jumps in a cycle pass through a line or a choice");
}

Json preview(std::shared_ptr<const Script> script, const std::string& start, const Json& choices, const Json& varsIn, int maxSteps) {
    Json vars = varsIn.isObject() ? varsIn : Json::object();
    Json transcript = Json::array();
    VarStore store;
    store.get = [&](const std::string& n) { return vars.get(n); };
    store.set = [&](const std::string& n, const Json& v) { vars[n] = v; };
    RunnerEvents events;
    events.command = [&](const std::string& c, const std::vector<std::string>& args) {
        Json a = Json::array();
        for (const auto& s : args) a.push(s);
        transcript.push(Json::object({{"type", "command"}, {"name", c}, {"args", a}, {"event", "dialogue:" + c}}));
    };
    events.node = [&](const std::string& n) { transcript.push(Json::object({{"type", "node"}, {"title", n}})); };
    Runner runner(std::move(script));
    std::string stopped = "ended";
    Json pending;
    auto fail = [&](const Status& s) {
        stopped = "error";
        transcript.push(Json::object({{"type", "error"}, {"message", s.error().message}}));
    };
    if (Status s = runner.start(start, store, events); !s) fail(s);
    size_t nextChoice = 0;
    for (int step = 0; step < maxSteps && stopped != "error"; ++step) {
        auto state = runner.state();
        if (state == Runner::State::Line) {
            const auto& l = runner.line();
            Json j = Json::object({{"type", "line"}, {"node", runner.node()}, {"speaker", l.speaker}, {"text", l.text}});
            if (!l.tags.members().empty()) j["tags"] = l.tags;
            transcript.push(j);
            if (Status s = runner.advance(store, events); !s) fail(s);
        } else if (state == Runner::State::Waiting) {
            transcript.push(Json::object({{"type", "wait"}}));
            if (Status s = runner.advance(store, events); !s) fail(s);
        } else if (state == Runner::State::Choices) {
            Json options = Json::array();
            for (const auto& c : runner.choices()) options.push(c.text);
            if (nextChoice >= choices.size()) {
                stopped = "awaiting_choice";
                pending = options;
                break;
            }
            const Json& pick = choices[nextChoice++];
            int index = -1;
            if (pick.isNumber()) {
                index = static_cast<int>(pick.asInt());
            } else {
                std::string want = str::lower(pick.asString());
                for (size_t k = 0; k < runner.choices().size(); ++k) {
                    if (str::lower(runner.choices()[k].text).find(want) != std::string::npos) {
                        index = static_cast<int>(k);
                        break;
                    }
                }
            }
            Json entry = Json::object({{"type", "choices"}, {"options", options}});
            if (index < 0 || index >= static_cast<int>(runner.choices().size())) {
                entry["error"] = "choice " + pick.dump() + " does not match an available option";
                transcript.push(entry);
                stopped = "error";
                break;
            }
            entry["chosen"] = index;
            entry["chosenText"] = runner.choices()[static_cast<size_t>(index)].text;
            transcript.push(entry);
            if (Status s = runner.choose(index, store, events); !s) fail(s);
        } else {
            break;
        }
        if (step + 1 == maxSteps) stopped = "max_steps";
    }
    if (stopped == "ended" && runner.state() != Runner::State::Ended) stopped = "max_steps";
    Json visited = Json::object();
    for (const auto& n : runner.script().nodes) {
        if (int v = runner.visited(n.title)) visited[n.title] = v;
    }
    Json out = Json::object({{"transcript", transcript}, {"stopped", stopped}, {"vars", vars}, {"visited", visited}});
    if (!pending.isNull()) out["pendingChoices"] = pending;
    return out;
}

}  // namespace sky::dialogue
