#include "skywalker/wander/Compiler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <unordered_map>
#include <unordered_set>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"

namespace sky::wander {

const char* toString(Trigger t) {
    switch (t) {
        case Trigger::Start: return "start";
        case Trigger::Tick: return "tick";
        case Trigger::Event: return "event";
        case Trigger::Key: return "key";
        case Trigger::Click: return "click";
        case Trigger::Action: return "action";
    }
    return "?";
}

namespace {

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class Tok { Number, String, Color, Ident, Symbol, End };

struct Token {
    Tok kind;
    std::string text;
    double number = 0;
    Vec4 color;
    SourceLoc loc;
};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    Lexer(std::string_view src, std::vector<Diagnostic>& diags) : src_(src), diags_(diags) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skipSpaceAndComments();
            SourceLoc loc{line_, col_};
            if (pos_ >= src_.size()) {
                out.push_back({Tok::End, "<end of input>", 0, {}, loc});
                break;
            }
            char c = src_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
                out.push_back(number(loc));
            } else if (isIdentStart(c)) {
                size_t start = pos_;
                while (pos_ < src_.size() && isIdentChar(src_[pos_])) advance();
                out.push_back({Tok::Ident, std::string(src_.substr(start, pos_ - start)), 0, {}, loc});
            } else if (c == '"' || c == '\'') {
                out.push_back(string(loc, c));
            } else if (c == '#') {
                out.push_back(color(loc));
            } else {
                static const char* kTwo[] = {"<=", ">=", "==", "!="};
                std::string sym(1, c);
                for (const char* two : kTwo) {
                    if (src_.substr(pos_, 2) == two) sym = two;
                }
                if (std::string_view("()[],.=+-*/%<>!").find(c) == std::string_view::npos) {
                    error(loc, "unexpected_character", std::string("unexpected character '") + c + "'");
                    advance();
                    continue;
                }
                for (size_t i = 0; i < sym.size(); ++i) advance();
                out.push_back({Tok::Symbol, sym, 0, {}, loc});
            }
        }
        return out;
    }

private:
    void advance() {
        if (src_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    void error(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        diags_.push_back({Severity::Error, loc, std::move(code), std::move(msg), std::move(hint)});
    }

    bool colorAhead() const {
        // "#" followed by 3/4/6/8 hex digits and then a non-identifier character.
        size_t n = 0;
        while (pos_ + 1 + n < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_ + 1 + n]))) ++n;
        bool boundary = pos_ + 1 + n >= src_.size() || !isIdentChar(src_[pos_ + 1 + n]);
        return boundary && (n == 3 || n == 4 || n == 6 || n == 8);
    }

    void skipSpaceAndComments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == ';') {
                advance();
            } else if ((c == '-' && src_.substr(pos_, 2) == "--") || (c == '/' && src_.substr(pos_, 2) == "//") ||
                       (c == '#' && !colorAhead())) {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else {
                break;
            }
        }
    }

    Token number(SourceLoc loc) {
        size_t start = pos_;
        while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) advance();
        double v = 0;
        if (!str::parseDouble(src_.substr(start, pos_ - start), v)) {
            error(loc, "invalid_number", "invalid number '" + std::string(src_.substr(start, pos_ - start)) + "'");
        }
        return {Tok::Number, std::string(src_.substr(start, pos_ - start)), v, {}, loc};
    }

    Token string(SourceLoc loc, char quote) {
        advance();
        std::string s;
        while (pos_ < src_.size() && src_[pos_] != quote && src_[pos_] != '\n') {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                advance();
                char e = src_[pos_];
                s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
            } else {
                s.push_back(src_[pos_]);
            }
            advance();
        }
        if (pos_ >= src_.size() || src_[pos_] != quote) {
            error(loc, "unterminated_string", "string is missing its closing quote");
        } else {
            advance();
        }
        return {Tok::String, s, 0, {}, loc};
    }

    Token color(SourceLoc loc) {
        size_t start = pos_;
        advance();
        while (pos_ < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_]))) advance();
        std::string text(src_.substr(start, pos_ - start));
        Vec4 c;
        if (!reflect::parseHexColor(text, c)) error(loc, "invalid_color", "invalid color '" + text + "'");
        return {Tok::Color, text, 0, c, loc};
    }

    std::string_view src_;
    std::vector<Diagnostic>& diags_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

struct FnSig {
    int minArgs;
    int maxArgs;
};

const std::unordered_map<std::string, FnSig>& functions() {
    static const std::unordered_map<std::string, FnSig> fns{
        {"find", {1, 1}},     {"nearest", {1, 1}},   {"count", {1, 1}},     {"distance", {2, 2}},
        {"direction", {2, 2}}, {"sin", {1, 1}},      {"cos", {1, 1}},       {"tan", {1, 1}},
        {"abs", {1, 1}},      {"sqrt", {1, 1}},      {"floor", {1, 1}},     {"ceil", {1, 1}},
        {"round", {1, 1}},    {"sign", {1, 1}},      {"min", {2, 2}},       {"max", {2, 2}},
        {"clamp", {3, 3}},    {"lerp", {3, 3}},      {"random", {0, 2}},    {"chance", {1, 1}},
        {"burst", {1, 2}},    {"water_height", {1, 2}},
        // audio builtins
        {"play", {1, 1}},     {"play_sound", {1, 2}}, {"stop_sound", {1, 1}}, {"music", {1, 2}}, {"set_volume", {2, 2}},
        // input builtins
        {"action", {1, 1}},   {"pressed", {1, 1}},    {"released", {1, 1}},   {"axis", {1, 1}},
        {"vec", {3, 3}},      {"color", {3, 4}},     {"length", {1, 1}},    {"normalize", {1, 1}},
        {"dot", {2, 2}},      {"cross", {2, 2}},     {"key", {1, 1}},       {"exists", {1, 1}},
        {"str", {1, 1}},      {"spawn", {1, 3}},     {"tagged", {2, 2}},    {"forward", {1, 1}},
    };
    return fns;
}

const std::unordered_set<std::string>& reservedWords() {
    static const std::unordered_set<std::string> words{
        "behavior", "intent", "var", "on", "end", "if", "then", "elif", "else", "every", "after", "repeat",
        "times", "move", "by", "toward", "at", "rotate", "look", "emit", "to", "destroy", "log", "stop",
        "let", "set", "and", "or", "not", "true", "false", "none"};
    return words;
}

const std::vector<std::string>& builtinNames() {
    static const std::vector<std::string> names{"self", "dt", "time", "frame", "pi"};
    return names;
}

const std::vector<std::string>& shorthandProps() {
    static const std::vector<std::string> props{"position", "rotation", "scale", "color", "name", "id", "enabled"};
    return props;
}

// ---------------------------------------------------------------------------
// Parser + checker
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(std::vector<Token> tokens, std::vector<Diagnostic>& diags, const std::vector<std::string>& components)
        : toks_(std::move(tokens)), diags_(diags), components_(components) {}

    std::unique_ptr<Program> run() {
        auto program = std::make_unique<Program>();
        program_ = program.get();
        if (isWord("behavior")) {
            while (!atEnd() && !failed()) {
                if (!isWord("behavior")) {
                    error(peek().loc, "expected_behavior", "expected 'behavior' but found '" + peek().text + "'",
                          "a file contains one or more `behavior Name ... end` blocks");
                    break;
                }
                program->behaviors.push_back(behavior());
            }
        } else {
            // Bare handlers: wrap in an anonymous behaviour for convenience.
            BehaviorDef b;
            b.name = "Main";
            b.loc = peek().loc;
            members(b, /*untilEnd=*/false);
            program->behaviors.push_back(std::move(b));
        }
        program->nodeCount = nodeCounter_;
        return program;
    }

private:
    // --- token helpers --------------------------------------------------------
    const Token& peek(size_t ahead = 0) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
    bool atEnd() const { return peek().kind == Tok::End; }
    bool isWord(std::string_view w, size_t ahead = 0) const {
        return peek(ahead).kind == Tok::Ident && peek(ahead).text == w;
    }
    bool isSym(std::string_view s) const { return peek().kind == Tok::Symbol && peek().text == s; }
    const Token& next() {
        const Token& t = peek();
        if (pos_ < toks_.size() - 1) ++pos_;
        return t;
    }
    bool failed() const { return errorCount_ > 25; }

    void error(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        ++errorCount_;
        diags_.push_back({Severity::Error, loc, std::move(code), std::move(msg), std::move(hint)});
    }
    void warning(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        diags_.push_back({Severity::Warning, loc, std::move(code), std::move(msg), std::move(hint)});
    }

    bool expectWord(std::string_view w, std::string_view context) {
        if (isWord(w)) {
            next();
            return true;
        }
        error(peek().loc, "expected_keyword",
              "expected '" + std::string(w) + "' " + std::string(context) + " but found '" + peek().text + "'");
        return false;
    }
    bool expectSym(std::string_view s, std::string_view context) {
        if (isSym(s)) {
            next();
            return true;
        }
        error(peek().loc, "expected_symbol",
              "expected '" + std::string(s) + "' " + std::string(context) + " but found '" + peek().text + "'");
        return false;
    }

    // --- structure --------------------------------------------------------------
    BehaviorDef behavior() {
        BehaviorDef b;
        b.loc = next().loc;  // 'behavior'
        if (peek().kind == Tok::Ident && !reservedWords().count(peek().text)) {
            b.name = next().text;
        } else if (peek().kind == Tok::String) {
            b.name = next().text;
        } else {
            error(peek().loc, "expected_name", "expected a behavior name after 'behavior'");
        }
        members(b, /*untilEnd=*/true);
        return b;
    }

    void members(BehaviorDef& b, bool untilEnd) {
        behavior_ = &b;
        while (!failed()) {
            if (atEnd()) {
                if (untilEnd) error(peek().loc, "missing_end", "behavior '" + b.name + "' is missing its 'end'");
                return;
            }
            if (untilEnd && isWord("end")) {
                next();
                return;
            }
            if (isWord("intent")) {
                next();
                if (peek().kind != Tok::String) {
                    error(peek().loc, "expected_string", "intent must be a quoted string");
                } else {
                    b.intent = next().text;
                }
            } else if (isWord("var")) {
                SourceLoc loc = next().loc;
                VarDecl v;
                v.loc = loc;
                if (peek().kind != Tok::Ident || reservedWords().count(peek().text)) {
                    error(peek().loc, "expected_name", "expected a variable name after 'var'");
                    next();
                    continue;
                }
                v.name = next().text;
                if (expectSym("=", "after var name")) v.initial = expression();
                b.vars.push_back(std::move(v));
            } else if (isWord("on")) {
                b.handlers.push_back(handler());
            } else {
                error(peek().loc, "unexpected_token",
                      "unexpected '" + peek().text + "' in behavior body",
                      "a behavior contains `intent \"...\"`, `var name = value` and `on <trigger> ... end` blocks");
                next();
            }
        }
    }

    Handler handler() {
        Handler h;
        h.loc = next().loc;  // 'on'
        const Token& t = next();
        static const std::vector<std::string> triggers{"start", "tick", "event", "key", "click", "action"};
        if (t.text == "start") {
            h.trigger = Trigger::Start;
        } else if (t.text == "tick" || t.text == "update") {
            h.trigger = Trigger::Tick;
        } else if (t.text == "event" || t.text == "key" || t.text == "action") {
            h.trigger = t.text == "event" ? Trigger::Event : t.text == "key" ? Trigger::Key : Trigger::Action;
            if (peek().kind != Tok::String) {
                error(peek().loc, "expected_string", "'on " + t.text + "' needs a quoted name, e.g. on " + t.text +
                                                         (t.text == "key" ? " \"space\"" : t.text == "action" ? " \"jump\"" : " \"door_opened\""));
            } else {
                h.argument = next().text;
            }
        } else if (t.text == "click") {
            h.trigger = Trigger::Click;
        } else {
            std::string guess = str::closest(t.text, triggers);
            error(t.loc, "unknown_trigger", "unknown trigger '" + t.text + "' (use start, tick, event, key, click, action)",
                  guess.empty() ? "" : "did you mean '" + guess + "'?");
        }
        scopes_.clear();
        scopes_.emplace_back();
        h.body = block({"end"});
        expectWord("end", "to close the 'on' handler");
        return h;
    }

    Block block(std::initializer_list<std::string_view> terminators) {
        Block out;
        scopes_.emplace_back();
        while (!atEnd() && !failed()) {
            bool stop = false;
            for (auto term : terminators) stop = stop || isWord(term);
            if (stop) break;
            if (StmtPtr s = statement()) out.push_back(std::move(s));
        }
        scopes_.pop_back();
        return out;
    }

    StmtPtr make(Stmt::Kind k, SourceLoc loc) {
        auto s = std::make_unique<Stmt>(k, loc);
        s->nodeId = ++nodeCounter_;
        return s;
    }

    StmtPtr statement() {
        const Token& t = peek();
        SourceLoc loc = t.loc;
        if (t.kind != Tok::Ident) {
            error(loc, "unexpected_token", "expected a statement but found '" + t.text + "'");
            next();
            return nullptr;
        }
        const std::string& w = t.text;
        if (w == "let") {
            next();
            auto s = make(Stmt::Kind::Let, loc);
            if (peek().kind != Tok::Ident || reservedWords().count(peek().text)) {
                error(peek().loc, "expected_name", "expected a name after 'let'");
                return nullptr;
            }
            s->name = next().text;
            expectSym("=", "after the variable name");
            s->value = expression();
            scopes_.back().insert(s->name);
            return s;
        }
        if (w == "set") {
            next();
            auto s = make(Stmt::Kind::Assign, loc);
            s->target = expression();
            checkAssignable(s->target.get());
            expectWord("to", "in 'set <target> to <value>'");
            s->value = expression();
            return s;
        }
        if (w == "if") return ifStatement();
        if (w == "every" || w == "after") {
            next();
            auto s = make(w == "every" ? Stmt::Kind::Every : Stmt::Kind::After, loc);
            s->value = expression();
            if (isWord("seconds") || isWord("second") || isWord("s")) next();
            s->body = block({"end"});
            expectWord("end", "to close '" + w + "'");
            return s;
        }
        if (w == "repeat") {
            next();
            auto s = make(Stmt::Kind::Repeat, loc);
            s->value = expression();
            expectWord("times", "after the repeat count");
            s->body = block({"end"});
            expectWord("end", "to close 'repeat'");
            return s;
        }
        if (w == "move") {
            next();
            auto target = expression();
            if (isWord("by")) {
                next();
                auto s = make(Stmt::Kind::Move, loc);
                s->target = std::move(target);
                s->value = expression();
                return s;
            }
            if (isWord("toward") || isWord("towards") || isWord("to")) {
                next();
                auto s = make(Stmt::Kind::MoveToward, loc);
                s->target = std::move(target);
                s->value = expression();
                expectWord("at", "in 'move <entity> toward <point> at <speed>'");
                s->extra = expression();
                return s;
            }
            error(peek().loc, "expected_keyword", "expected 'by' or 'toward' after 'move <entity>'",
                  "move self by (0, 1, 0)   |   move self toward find(\"Goal\") at 2");
            return nullptr;
        }
        if (w == "rotate") {
            next();
            auto s = make(Stmt::Kind::Rotate, loc);
            s->target = expression();
            expectWord("by", "in 'rotate <entity> by <degrees vector>'");
            s->value = expression();
            return s;
        }
        if (w == "look") {
            next();
            auto s = make(Stmt::Kind::Look, loc);
            s->target = expression();
            expectWord("at", "in 'look <entity> at <point>'");
            s->value = expression();
            return s;
        }
        if (w == "emit") {
            next();
            auto s = make(Stmt::Kind::Emit, loc);
            if (peek().kind != Tok::String) {
                error(peek().loc, "expected_string", "emit needs a quoted event name, e.g. emit \"coin_collected\"");
                return nullptr;
            }
            s->name = next().text;
            if (isWord("to")) {
                next();
                s->extra = expression();
            }
            return s;
        }
        if (w == "destroy") {
            next();
            auto s = make(Stmt::Kind::Destroy, loc);
            s->target = expression();
            return s;
        }
        if (w == "log" || w == "print") {
            next();
            auto s = make(Stmt::Kind::Log, loc);
            s->value = expression();
            return s;
        }
        if (w == "stop") {
            next();
            return make(Stmt::Kind::Stop, loc);
        }
        // Assignment or call.
        auto e = expression();
        if (!e) return nullptr;
        if (isSym("=")) {
            next();
            auto s = make(Stmt::Kind::Assign, loc);
            checkAssignable(e.get());
            s->target = std::move(e);
            s->value = expression();
            return s;
        }
        if (e->kind == Expr::Kind::Call) {
            auto s = make(Stmt::Kind::Call, loc);
            s->value = std::move(e);
            return s;
        }
        error(loc, "not_a_statement", "this expression does nothing on its own",
              "did you mean to assign it (x = ...) or use it in an if?");
        return nullptr;
    }

    StmtPtr ifStatement() {
        auto s = make(Stmt::Kind::If, next().loc);
        auto cond = expression();
        expectWord("then", "after the if condition");
        Block body = block({"elif", "else", "end"});
        s->branches.emplace_back(std::move(cond), std::move(body));
        while (isWord("elif")) {
            next();
            auto c = expression();
            expectWord("then", "after the elif condition");
            Block b = block({"elif", "else", "end"});
            s->branches.emplace_back(std::move(c), std::move(b));
        }
        if (isWord("else")) {
            next();
            Block b = block({"end"});
            s->branches.emplace_back(nullptr, std::move(b));
        }
        expectWord("end", "to close 'if'");
        return s;
    }

    void checkAssignable(const Expr* e) {
        if (!e) return;
        if (e->kind == Expr::Kind::Ident) {
            if (e->text == "self" || e->text == "dt" || e->text == "time" || e->text == "frame" || e->text == "pi") {
                error(e->loc, "readonly", "'" + e->text + "' cannot be assigned");
            }
            return;
        }
        if (e->kind == Expr::Kind::Member) return;
        error(e->loc, "not_assignable", "left side of an assignment must be a variable or property like self.position");
    }

    // --- expressions -------------------------------------------------------------
    ExprPtr mk(Expr::Kind k, SourceLoc loc) { return std::make_unique<Expr>(k, loc); }

    ExprPtr expression() { return orExpr(); }

    ExprPtr binary(ExprPtr lhs, const std::string& op, SourceLoc loc, ExprPtr rhs) {
        auto e = mk(Expr::Kind::Binary, loc);
        e->text = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    ExprPtr orExpr() {
        auto l = andExpr();
        while (isWord("or")) {
            SourceLoc loc = next().loc;
            l = binary(std::move(l), "or", loc, andExpr());
        }
        return l;
    }
    ExprPtr andExpr() {
        auto l = notExpr();
        while (isWord("and")) {
            SourceLoc loc = next().loc;
            l = binary(std::move(l), "and", loc, notExpr());
        }
        return l;
    }
    ExprPtr notExpr() {
        if (isWord("not") || isSym("!")) {
            SourceLoc loc = next().loc;
            auto e = mk(Expr::Kind::Unary, loc);
            e->text = "not";
            e->lhs = notExpr();
            return e;
        }
        return comparison();
    }
    ExprPtr comparison() {
        auto l = additive();
        static const std::vector<std::string> ops{"<", "<=", ">", ">=", "==", "!="};
        while (peek().kind == Tok::Symbol && std::find(ops.begin(), ops.end(), peek().text) != ops.end()) {
            const Token& t = next();
            l = binary(std::move(l), t.text, t.loc, additive());
        }
        if (isWord("is")) {  // forgiving: "a is b" == "a == b"
            SourceLoc loc = next().loc;
            l = binary(std::move(l), "==", loc, additive());
        }
        return l;
    }
    ExprPtr additive() {
        auto l = multiplicative();
        while (isSym("+") || isSym("-")) {
            const Token& t = next();
            l = binary(std::move(l), t.text, t.loc, multiplicative());
        }
        return l;
    }
    ExprPtr multiplicative() {
        auto l = unary();
        while (isSym("*") || isSym("/") || isSym("%")) {
            const Token& t = next();
            l = binary(std::move(l), t.text, t.loc, unary());
        }
        return l;
    }
    ExprPtr unary() {
        if (isSym("-")) {
            SourceLoc loc = next().loc;
            auto e = mk(Expr::Kind::Unary, loc);
            e->text = "-";
            e->lhs = unary();
            return e;
        }
        return postfix();
    }
    ExprPtr postfix() {
        auto e = primary();
        while (e && isSym(".")) {
            SourceLoc loc = next().loc;
            if (peek().kind != Tok::Ident) {
                error(peek().loc, "expected_name", "expected a property name after '.'");
                return e;
            }
            auto m = mk(Expr::Kind::Member, loc);
            m->text = next().text;
            m->lhs = std::move(e);
            checkMember(*m);
            e = std::move(m);
        }
        return e;
    }

    void checkMember(const Expr& m) {
        // Only `self.<name>` is statically checkable.
        if (!m.lhs || m.lhs->kind != Expr::Kind::Ident || m.lhs->text != "self") return;
        const std::string& n = m.text;
        if (std::find(shorthandProps().begin(), shorthandProps().end(), n) != shorthandProps().end()) return;
        if (std::find(components_.begin(), components_.end(), n) != components_.end()) return;
        if (behavior_) {
            for (const auto& v : behavior_->vars) {
                if (v.name == n) return;
            }
        }
        std::vector<std::string> candidates = shorthandProps();
        candidates.insert(candidates.end(), components_.begin(), components_.end());
        if (behavior_) {
            for (const auto& v : behavior_->vars) candidates.push_back(v.name);
        }
        std::string guess = str::closest(n, candidates);
        warning(m.loc, "undeclared_var",
                "self." + n + " is not a component, built-in property or declared var; it reads entity vars (none if unset)",
                guess.empty() ? "declare it with `var " + n + " = <value>` at the top of the behavior"
                              : "did you mean self." + guess + "?");
    }

    bool isLocal(const std::string& name) const {
        for (const auto& scope : scopes_) {
            if (scope.count(name)) return true;
        }
        return false;
    }

    ExprPtr primary() {
        const Token& t = peek();
        SourceLoc loc = t.loc;
        switch (t.kind) {
            case Tok::Number: {
                auto e = mk(Expr::Kind::Number, loc);
                e->number = next().number;
                return e;
            }
            case Tok::String: {
                auto e = mk(Expr::Kind::String, loc);
                e->text = next().text;
                return e;
            }
            case Tok::Color: {
                auto e = mk(Expr::Kind::Color, loc);
                e->color = next().color;
                return e;
            }
            case Tok::Symbol:
                if (t.text == "(") return parenOrVector();
                error(loc, "unexpected_token", "expected a value but found '" + t.text + "'");
                next();
                return nullptr;
            case Tok::End:
                error(loc, "unexpected_end", "expected a value but the script ended");
                return nullptr;
            case Tok::Ident: break;
        }
        std::string name = next().text;
        if (name == "true" || name == "false") {
            auto e = mk(Expr::Kind::Bool, loc);
            e->number = name == "true" ? 1 : 0;
            return e;
        }
        if (name == "none" || name == "nothing") return mk(Expr::Kind::None, loc);
        if (isSym("(")) return call(name, loc);
        if (reservedWords().count(name)) {
            error(loc, "unexpected_keyword", "'" + name + "' cannot be used as a value here");
            return nullptr;
        }
        auto e = mk(Expr::Kind::Ident, loc);
        e->text = name;
        const auto& builtins = builtinNames();
        bool known = isLocal(name) || std::find(builtins.begin(), builtins.end(), name) != builtins.end();
        bool isVar = false;
        if (!known && behavior_) {
            for (const auto& v : behavior_->vars) isVar = isVar || v.name == name;
        }
        if (isVar) {
            // Bare var name is sugar for self.<var>.
            auto self = mk(Expr::Kind::Ident, loc);
            self->text = "self";
            auto m = mk(Expr::Kind::Member, loc);
            m->text = name;
            m->lhs = std::move(self);
            return m;
        }
        if (!known) {
            std::vector<std::string> candidates = builtins;
            for (const auto& scope : scopes_) candidates.insert(candidates.end(), scope.begin(), scope.end());
            std::string guess = str::closest(name, candidates);
            error(loc, "unknown_name", "unknown name '" + name + "'",
                  guess.empty() ? "declare it with `let " + name + " = ...` or `var " + name + " = ...`"
                                : "did you mean '" + guess + "'?");
        }
        return e;
    }

    ExprPtr call(const std::string& name, SourceLoc loc) {
        auto e = mk(Expr::Kind::Call, loc);
        e->text = name;
        next();  // '('
        if (!isSym(")")) {
            while (true) {
                e->args.push_back(expression());
                if (isSym(",")) {
                    next();
                    continue;
                }
                break;
            }
        }
        expectSym(")", "to close the call to " + name + "()");
        auto it = functions().find(name);
        if (it == functions().end()) {
            std::vector<std::string> names = builtinFunctions();
            std::string guess = str::closest(name, names);
            error(loc, "unknown_function", "unknown function '" + name + "'",
                  guess.empty() ? "see wander_reference for the list of functions" : "did you mean '" + guess + "'?");
        } else {
            int n = static_cast<int>(e->args.size());
            if (n < it->second.minArgs || n > it->second.maxArgs) {
                std::string expected = it->second.minArgs == it->second.maxArgs
                                           ? std::to_string(it->second.minArgs)
                                           : std::to_string(it->second.minArgs) + "-" + std::to_string(it->second.maxArgs);
                error(loc, "wrong_arity",
                      name + "() takes " + expected + " argument(s) but was given " + std::to_string(n));
            }
        }
        return e;
    }

    ExprPtr parenOrVector() {
        SourceLoc loc = next().loc;  // '('
        auto first = expression();
        if (isSym(")")) {
            next();
            return first;
        }
        auto v = mk(Expr::Kind::Vector, loc);
        v->args.push_back(std::move(first));
        while (isSym(",")) {
            next();
            v->args.push_back(expression());
        }
        expectSym(")", "to close the vector");
        if (v->args.size() < 2 || v->args.size() > 4) {
            error(loc, "invalid_vector", "vectors have 2, 3 or 4 components, e.g. (0, 1, 0)");
        }
        return v;
    }

    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::vector<Diagnostic>& diags_;
    const std::vector<std::string>& components_;
    std::vector<std::unordered_set<std::string>> scopes_;
    BehaviorDef* behavior_ = nullptr;
    Program* program_ = nullptr;
    int nodeCounter_ = 0;
    int errorCount_ = 0;
};

}  // namespace

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
                std::string t = toString(h.trigger);
                if (!h.argument.empty()) t += " \"" + h.argument + "\"";
                handlers.push(t);
            }
            Json vars = Json::array();
            for (const auto& v : b.vars) vars.push(v.name);
            behaviors.push(Json::object({{"name", b.name}, {"intent", b.intent}, {"vars", vars}, {"handlers", handlers}}));
        }
        out["behaviors"] = behaviors;
    }
    return out;
}

CompileResult compile(std::string_view source, const std::vector<std::string>& knownComponents) {
    CompileResult result;
    if (source.size() > 256 * 1024) {
        result.diagnostics.push_back({Severity::Error, {}, "too_large", "script exceeds 256 KiB", ""});
        return result;
    }
    auto tokens = Lexer(source, result.diagnostics).run();
    std::unique_ptr<Program> program = Parser(std::move(tokens), result.diagnostics, knownComponents).run();
    std::stable_sort(result.diagnostics.begin(), result.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
    if (result.errorCount() == 0) result.program = std::move(program);
    return result;
}

const std::vector<std::string>& builtinFunctions() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& [k, v] : functions()) n.push_back(k);
        std::sort(n.begin(), n.end());
        return n;
    }();
    return names;
}

const char* referenceText() {
    return R"WANDER(Wander language reference (Skywalker ECPS)

A behavior pairs a natural-language intent with deterministic code:

behavior Patrol
  intent "Walk between two points and turn red when the player is close."
  var speed = 2                      -- per-entity state, read as `speed` or `self.speed`
  var home = (0, 0, 0)

  on start
    home = self.position
  end

  on tick
    move self by (sin(time) * speed * dt, 0, 0)
    let player = find("Player")
    if exists(player) and distance(self, player) < 3 then
      self.color = #ff4040
    else
      set self.color to #40c0ff
    end
    every 2 seconds
      emit "patrol_ping"
    end
  end

  on event "alarm"
    speed = speed * 2
  end
end

Triggers:   on start | on tick | on event "name" | on key "space" | on click | on action "jump"
Statements: let x = v | target = v | set target to v | if c then .. elif c then .. else .. end
            every <sec> .. end | after <sec> .. end | repeat <n> times .. end   (n <= 1000)
            move <e> by <vec> | move <e> toward <point|entity> at <speed> | rotate <e> by <deg vec>
            look <e> at <point|entity> | emit "evt" (to <e>) | destroy <e> | log <value> | stop
Values:     numbers, "strings", true/false, none, colors #rgb/#rrggbb/#rrggbbaa, vectors (x, y, z)
Operators:  + - * / %   < <= > >= == !=   and or not      ("text" + anything concatenates)
Names:      self, dt (seconds this tick), time (seconds since play), frame, pi, locals, vars
Properties: e.position e.rotation (degrees) e.scale e.color e.name e.id e.enabled
            e.<component>.<field> e.g. self.light.intensity, self.mesh.roughness
            e.<var> for entity vars; v.x v.y v.z on vectors; c.r c.g c.b c.a on colors
Functions:  find(name) nearest(tag) count(tag) tagged(e, tag) exists(e) spawn(mesh | "prefab:path", pos?, name?)
            distance(a, b) direction(a, b) forward(e) length(v) normalize(v) dot(a, b) cross(a, b)
            vec(x, y, z) color(r, g, b, a?) sin cos tan abs sqrt floor ceil round sign min max
            clamp(x, lo, hi) lerp(a, b, t) random() random(hi) random(lo, hi) chance(p) key(name) str(v)
Input:      action("jump") held? pressed("jump") this tick? released("jump") axis("move") -> number, or vec (x, y, 0)
            for 2D actions (x right, y forward/up); `on action "jump"` fires when it is pressed. Actions come from
            input.json (see the input_map tool): keyboard, mouse and gamepad share the same names.
Audio:      play(e) starts e's audio component; play_sound("audio/hit.wav", volume?) one-shot at self; stop_sound(e)
            music("audio/theme.wav", fade?) crossfades the music (music("") fades out); set_volume("music", 0.5)
Effects:    burst(n) / burst(e, n) emits n particles now (particles component; explosions, muzzle flashes)
            water_height(x, z) / water_height(pos): the animated water surface height (boats, buoyancy)
            e.particles.rate / .emitting / .colorStart ... and e.water.windSpeed ... like any component
Comments:   -- comment   // comment   # comment (a '#' followed by a space)
Rules:      no while-loops (every handler always terminates); randomness is seeded (replayable);
            entities are -Z forward; rotations are Euler degrees (pitch X, yaw Y, roll Z).
)WANDER";
}

}  // namespace sky::wander
