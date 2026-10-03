#include "skywalker/wander/Parser.h"

#include <algorithm>
#include <cctype>
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
        case Trigger::Enter: return "enter";
        case Trigger::Exit: return "exit";
    }
    return "?";
}

bool ParseResult::ok() const {
    return std::none_of(diagnostics.begin(), diagnostics.end(),
                        [](const Diagnostic& d) { return d.severity == Severity::Error; });
}

const std::vector<std::string>& reservedWords() {
    static const std::vector<std::string> words{
        "behavior", "intent", "var", "param", "const", "fn", "on", "end", "if", "then", "elif", "else", "while",
        "do", "for", "in", "every", "after", "repeat", "times", "wait", "until", "break", "continue", "return",
        "stop", "go", "goto", "state", "test", "expect", "use", "with", "move", "by", "toward", "at", "rotate",
        "look", "emit", "to", "destroy", "log", "print", "let", "set", "and", "or", "not", "true", "false",
        "none", "self", "other", "dt", "time", "frame", "pi", "state_time"};
    return words;
}

namespace {

bool isReserved(const std::string& w) {
    static const std::unordered_set<std::string> set(reservedWords().begin(), reservedWords().end());
    return set.count(w) != 0;
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

enum class Tok {
    Number,
    String,       // complete string without interpolation
    InterpBegin,  // "text{    (text = literal before the first {)
    InterpMid,    // }text{
    InterpEnd,    // }text"
    Color,
    Ident,
    Symbol,
    End
};

struct Token {
    Tok kind;
    std::string text;
    double number = 0;
    Vec4 color;
    SourceLoc loc;
};

struct Comment {
    int line;
    std::string text;
};

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    Lexer(std::string_view src, std::vector<Diagnostic>& diags) : src_(src), diags_(diags) {}

    std::vector<Token> run(std::vector<Comment>& comments) {
        std::vector<Token> out;
        // Interpolation: each entry is the brace depth inside an open `{...}` of a string.
        std::vector<int> interp;
        while (true) {
            skipSpaceAndComments(comments);
            SourceLoc loc{line_, col_};
            if (pos_ >= src_.size()) {
                if (!interp.empty()) error(loc, "unterminated_string", "string interpolation is missing its closing '}'");
                out.push_back({Tok::End, "<end of input>", 0, {}, loc});
                break;
            }
            char c = src_[pos_];
            if (!interp.empty() && c == '}' && interp.back() == 0) {
                // Back into the string literal.
                interp.pop_back();
                advance();
                out.push_back(stringPart(loc, '"', /*afterInterp=*/true, interp));
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && pos_ + 1 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) &&
                 !(pos_ > 0 && src_[pos_ - 1] == '.'))) {
                out.push_back(number(loc));
            } else if (isIdentStart(c)) {
                size_t start = pos_;
                while (pos_ < src_.size() && isIdentChar(src_[pos_])) advance();
                out.push_back({Tok::Ident, std::string(src_.substr(start, pos_ - start)), 0, {}, loc});
            } else if (c == '"' || c == '\'') {
                advance();
                out.push_back(stringPart(loc, c, false, interp));
            } else if (c == '#') {
                out.push_back(color(loc));
            } else {
                static const char* kThree[] = {"..="};
                static const char* kTwo[] = {"<=", ">=", "==", "!=", "+=", "-=", "*=", "/=", "..", "->"};
                std::string sym(1, c);
                for (const char* three : kThree) {
                    if (src_.substr(pos_, 3) == three) sym = three;
                }
                if (sym.size() == 1) {
                    for (const char* two : kTwo) {
                        if (src_.substr(pos_, 2) == two) sym = two;
                    }
                }
                if (std::string_view("()[]{},.:=+-*/%<>!|?").find(c) == std::string_view::npos) {
                    error(loc, "unexpected_character", std::string("unexpected character '") + c + "'");
                    advance();
                    continue;
                }
                if (!interp.empty()) {
                    if (c == '{') ++interp.back();
                    if (c == '}') --interp.back();
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
        diags_.push_back({Severity::Error, loc, std::move(code), std::move(msg), std::move(hint), {}});
    }

    bool colorAhead() const {
        // "#" followed by 3/4/6/8 hex digits and then a non-identifier character.
        size_t n = 0;
        while (pos_ + 1 + n < src_.size() && std::isxdigit(static_cast<unsigned char>(src_[pos_ + 1 + n]))) ++n;
        bool boundary = pos_ + 1 + n >= src_.size() || !isIdentChar(src_[pos_ + 1 + n]);
        return boundary && (n == 3 || n == 4 || n == 6 || n == 8);
    }

    void skipSpaceAndComments(std::vector<Comment>& comments) {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c)) || c == ';') {
                advance();
            } else if ((c == '-' && src_.substr(pos_, 2) == "--") || (c == '/' && src_.substr(pos_, 2) == "//") ||
                       (c == '#' && !colorAhead())) {
                int line = line_;
                size_t start = pos_;
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
                comments.push_back({line, str::trim(src_.substr(start, pos_ - start))});
            } else {
                break;
            }
        }
    }

    Token number(SourceLoc loc) {
        size_t start = pos_;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                advance();
            } else if (c == '.' && !(pos_ + 1 < src_.size() && src_[pos_ + 1] == '.')) {
                advance();  // a decimal point, but not the start of a `..` range
            } else if ((c == 'e' || c == 'E') && pos_ + 1 < src_.size() &&
                       (std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) ||
                        ((src_[pos_ + 1] == '-' || src_[pos_ + 1] == '+') && pos_ + 2 < src_.size() &&
                         std::isdigit(static_cast<unsigned char>(src_[pos_ + 2]))))) {
                advance();
                if (src_[pos_] == '-' || src_[pos_] == '+') advance();
            } else {
                break;
            }
        }
        std::string text(src_.substr(start, pos_ - start));
        double v = 0;
        if (!str::parseDouble(text, v)) error(loc, "invalid_number", "invalid number '" + text + "'");
        if (pos_ < src_.size() && isIdentStart(src_[pos_])) {
            size_t s = pos_;
            while (pos_ < src_.size() && isIdentChar(src_[pos_])) advance();
            std::string suffix(src_.substr(s, pos_ - s));
            error(loc, "invalid_number", "unexpected '" + suffix + "' right after the number " + text,
                  "put a space or an operator between them, e.g. `2 * x`");
        }
        return {Tok::Number, text, v, {}, loc};
    }

    // Reads string content up to the closing quote or an interpolation `{`.
    Token stringPart(SourceLoc loc, char quote, bool afterInterp, std::vector<int>& interp) {
        std::string s;
        while (pos_ < src_.size() && src_[pos_] != quote && src_[pos_] != '\n') {
            char c = src_[pos_];
            if (c == '\\' && pos_ + 1 < src_.size()) {
                advance();
                char e = src_[pos_];
                s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                advance();
                continue;
            }
            if (c == '{' && quote == '"') {
                advance();
                interp.push_back(0);
                return {afterInterp ? Tok::InterpMid : Tok::InterpBegin, s, 0, {}, loc};
            }
            s.push_back(c);
            advance();
        }
        if (pos_ >= src_.size() || src_[pos_] != quote) {
            error(loc, "unterminated_string", "string is missing its closing quote");
        } else {
            advance();
        }
        return {afterInterp ? Tok::InterpEnd : Tok::String, s, 0, {}, loc};
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
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(std::vector<Token> tokens, std::vector<Comment> comments, std::vector<Diagnostic>& diags)
        : toks_(std::move(tokens)), comments_(std::move(comments)), diags_(diags) {}

    // A lone expression (graph pins, tool arguments).
    ExprPtr runExpression() {
        ExprPtr e = expression();
        if (!atEnd()) {
            error(peek().loc, "unexpected_token", "unexpected '" + describe(peek()) + "' after the expression");
        }
        return e;
    }

    std::shared_ptr<Module> run() {
        auto m = std::make_shared<Module>();
        BehaviorDef implicitB;
        implicitB.name = "Main";
        implicitB.implicit = true;
        bool hasImplicit = false;
        while (!atEnd() && !failed()) {
            std::vector<std::string> comments = takeComments();
            const Token& t = peek();
            if (isWord("use")) {
                UseDecl u;
                u.comments = std::move(comments);
                u.loc = next().loc;
                if (peek().kind != Tok::String) {
                    error(peek().loc, "expected_string", "use needs a quoted module path, e.g. use \"scripts/combat\"");
                    next();
                    continue;
                }
                u.path = next().text;
                if (isWord("as")) {
                    next();
                    u.alias = name("after 'as'");
                }
                if (u.alias.empty()) {
                    std::string stem = u.path;
                    if (auto slash = stem.find_last_of('/'); slash != std::string::npos) stem = stem.substr(slash + 1);
                    if (stem.size() > 7 && stem.substr(stem.size() - 7) == ".wander") stem.resize(stem.size() - 7);
                    u.alias = stem;
                }
                m->uses.push_back(std::move(u));
            } else if (isWord("behavior")) {
                m->behaviors.push_back(behavior(std::move(comments)));
            } else if (isWord("const")) {
                m->consts.push_back(constDecl(std::move(comments)));
            } else if (isWord("fn")) {
                m->fns.push_back(fnDecl(std::move(comments)));
            } else if (isWord("test")) {
                m->tests.push_back(testDecl(std::move(comments)));
            } else if (isWord("on") || isWord("var") || isWord("param") || isWord("intent") || isWord("state")) {
                if (!hasImplicit) implicitB.loc = t.loc;
                hasImplicit = true;
                member(implicitB, std::move(comments));
            } else {
                error(t.loc, "unexpected_token", "unexpected '" + t.text + "' at the top level",
                      "a script contains `behavior Name ... end` blocks (or bare `on ...` handlers), plus optional "
                      "`use`, `const`, `fn` and `test` declarations");
                next();
            }
        }
        if (hasImplicit) {
            if (m->behaviors.empty()) {
                // A file without `behavior` blocks is one behavior: file-level fns, consts and
                // tests belong to it (so they can use its vars).
                for (auto& f : m->fns) implicitB.fns.push_back(std::move(f));
                for (auto& c : m->consts) implicitB.consts.push_back(std::move(c));
                for (auto& tst : m->tests) implicitB.tests.push_back(std::move(tst));
                m->fns.clear();
                m->consts.clear();
                m->tests.clear();
            }
            m->behaviors.insert(m->behaviors.begin(), std::move(implicitB));
        }
        m->trailingComments = takeAllComments();
        m->nodeCount = nodeCounter_;
        return m;
    }

private:
    // --- token helpers --------------------------------------------------------
    const Token& peek(size_t ahead = 0) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
    bool atEnd() const { return peek().kind == Tok::End; }
    bool isWord(std::string_view w, size_t ahead = 0) const {
        return peek(ahead).kind == Tok::Ident && peek(ahead).text == w;
    }
    bool isSym(std::string_view s, size_t ahead = 0) const {
        return peek(ahead).kind == Tok::Symbol && peek(ahead).text == s;
    }
    const Token& next() {
        const Token& t = peek();
        lastLine_ = t.loc.line;
        if (pos_ < toks_.size() - 1) ++pos_;
        return t;
    }
    bool failed() const { return errorCount_ > 30; }

    void error(SourceLoc loc, std::string code, std::string msg, std::string hint = {}) {
        ++errorCount_;
        diags_.push_back({Severity::Error, loc, std::move(code), std::move(msg), std::move(hint), {}});
    }

    bool expectWord(std::string_view w, std::string_view context) {
        if (isWord(w)) {
            next();
            return true;
        }
        error(peek().loc, "expected_keyword",
              "expected '" + std::string(w) + "' " + std::string(context) + " but found '" + describe(peek()) + "'");
        return false;
    }
    bool expectSym(std::string_view s, std::string_view context) {
        if (isSym(s)) {
            next();
            return true;
        }
        error(peek().loc, "expected_symbol",
              "expected '" + std::string(s) + "' " + std::string(context) + " but found '" + describe(peek()) + "'");
        return false;
    }
    static std::string describe(const Token& t) {
        switch (t.kind) {
            case Tok::String: return "\"" + t.text + "\"";
            case Tok::InterpBegin: return "\"" + t.text + "{";
            default: return t.text;
        }
    }

    std::string name(std::string_view context) {
        if (peek().kind == Tok::Ident && !isReserved(peek().text)) return next().text;
        if (peek().kind == Tok::Ident) {
            error(peek().loc, "reserved_word", "'" + peek().text + "' is a reserved word and cannot be used as a name",
                  "pick another name, e.g. '" + peek().text + "_value'");
            next();
            return {};
        }
        error(peek().loc, "expected_name", "expected a name " + std::string(context) + " but found '" + describe(peek()) + "'");
        return {};
    }

    // Comments that appear before the current token (since the last consumed one).
    std::vector<std::string> takeComments() {
        std::vector<std::string> out;
        int line = peek().loc.line;
        while (commentPos_ < comments_.size() && comments_[commentPos_].line < line) {
            out.push_back(comments_[commentPos_++].text);
        }
        return out;
    }
    std::vector<std::string> takeAllComments() {
        std::vector<std::string> out;
        while (commentPos_ < comments_.size()) out.push_back(comments_[commentPos_++].text);
        return out;
    }

    TypeRef typeAnnotation() {
        // name ('|' name)* '?'?
        TypeRef t;
        t.loc = peek().loc;
        while (peek().kind == Tok::Ident) {
            t.text += next().text;
            if (isSym("|")) {
                next();
                t.text += "|";
                continue;
            }
            break;
        }
        if (isSym("?")) {
            next();
            t.text += "?";
        }
        if (t.text.empty()) error(t.loc, "expected_type", "expected a type name (number, bool, string, vec, color, entity, list, map, any)");
        return t;
    }

    // --- declarations -------------------------------------------------------------
    BehaviorDef behavior(std::vector<std::string> comments) {
        BehaviorDef b;
        b.comments = std::move(comments);
        b.loc = next().loc;  // 'behavior'
        if (peek().kind == Tok::Ident && !isReserved(peek().text)) {
            b.name = next().text;
        } else if (peek().kind == Tok::String) {
            b.name = next().text;
        } else {
            error(peek().loc, "expected_name", "expected a behavior name after 'behavior'");
        }
        while (!failed()) {
            if (atEnd()) {
                error(peek().loc, "missing_end", "behavior '" + b.name + "' is missing its 'end'");
                break;
            }
            if (isWord("end")) {
                next();
                break;
            }
            member(b, takeComments());
        }
        return b;
    }

    void member(BehaviorDef& b, std::vector<std::string> comments) {
        if (isWord("intent")) {
            next();
            if (peek().kind != Tok::String) {
                error(peek().loc, "expected_string", "intent must be a quoted string (no {interpolation})");
                next();
            } else {
                b.intent = next().text;
            }
        } else if (isWord("var") || isWord("param")) {
            b.vars.push_back(varDecl(std::move(comments)));
        } else if (isWord("const")) {
            b.consts.push_back(constDecl(std::move(comments)));
        } else if (isWord("fn")) {
            b.fns.push_back(fnDecl(std::move(comments)));
        } else if (isWord("on")) {
            b.handlers.push_back(handler(std::move(comments)));
        } else if (isWord("state")) {
            b.states.push_back(stateDecl(std::move(comments)));
        } else if (isWord("test")) {
            b.tests.push_back(testDecl(std::move(comments)));
        } else {
            error(peek().loc, "unexpected_token", "unexpected '" + describe(peek()) + "' in behavior body",
                  "a behavior contains intent \"...\", var/param/const declarations, fn definitions, "
                  "`on <trigger> ... end` handlers, `state Name ... end` blocks and `test \"...\" ... end` blocks");
            next();
        }
    }

    VarDecl varDecl(std::vector<std::string> comments) {
        VarDecl v;
        v.comments = std::move(comments);
        v.isParam = peek().text == "param";
        v.loc = next().loc;
        v.name = name(v.isParam ? "after 'param'" : "after 'var'");
        if (isSym(":")) {
            next();
            v.type = typeAnnotation();
        }
        if (expectSym("=", v.isParam ? "after the param name (params need a default value)" : "after the var name")) {
            noIn_ = v.isParam;
            v.initial = expression();
            noIn_ = false;
        }
        if (v.isParam) {
            if (isWord("in")) {
                next();
                v.minValue = expression();
                if (!isSym("..") && !isSym("..=")) {
                    error(peek().loc, "expected_symbol", "expected '..' in the param range, e.g. param speed = 3 in 0..10");
                } else {
                    next();
                    v.maxValue = expression();
                }
            }
            if (peek().kind == Tok::String) v.doc = next().text;
        }
        return v;
    }

    ConstDecl constDecl(std::vector<std::string> comments) {
        ConstDecl c;
        c.comments = std::move(comments);
        c.loc = next().loc;  // 'const'
        c.name = name("after 'const'");
        if (isSym(":")) {
            next();
            c.type = typeAnnotation();
        }
        if (expectSym("=", "after the const name")) c.value = expression();
        return c;
    }

    FnDecl fnDecl(std::vector<std::string> comments) {
        FnDecl f;
        f.comments = std::move(comments);
        f.loc = next().loc;  // 'fn'
        f.name = name("after 'fn'");
        if (expectSym("(", "after the function name")) {
            if (!isSym(")")) {
                while (!failed()) {
                    Param p;
                    p.loc = peek().loc;
                    p.name = name("for a parameter");
                    if (p.name.empty() && !isSym(",") && !isSym(")")) next();
                    if (isSym(":")) {
                        next();
                        p.type = typeAnnotation();
                    }
                    f.params.push_back(std::move(p));
                    if (isSym(",")) {
                        next();
                        continue;
                    }
                    break;
                }
            }
            expectSym(")", "to close the parameter list");
        }
        if (isSym("->") || isSym(":")) {
            next();
            f.returns = typeAnnotation();
        }
        f.body = block({"end"});
        expectWord("end", "to close fn '" + f.name + "'");
        return f;
    }

    TestDecl testDecl(std::vector<std::string> comments) {
        TestDecl t;
        t.comments = std::move(comments);
        t.loc = next().loc;  // 'test'
        if (peek().kind == Tok::String) {
            t.name = next().text;
        } else {
            error(peek().loc, "expected_string", "test needs a quoted name, e.g. test \"loses hp when hit\"");
        }
        t.body = block({"end"});
        expectWord("end", "to close the test");
        return t;
    }

    StateDecl stateDecl(std::vector<std::string> comments) {
        StateDecl s;
        s.comments = std::move(comments);
        s.loc = next().loc;  // 'state'
        s.name = name("after 'state'");
        while (!failed()) {
            if (atEnd()) {
                error(peek().loc, "missing_end", "state '" + s.name + "' is missing its 'end'");
                break;
            }
            if (isWord("end")) {
                next();
                break;
            }
            if (isWord("on")) {
                s.handlers.push_back(handler(takeComments()));
            } else {
                error(peek().loc, "unexpected_token", "unexpected '" + describe(peek()) + "' in state '" + s.name + "'",
                      "a state contains handlers only: on enter, on exit, on tick, on event \"name\", ...");
                next();
            }
        }
        return s;
    }

    Handler handler(std::vector<std::string> comments) {
        Handler h;
        h.comments = std::move(comments);
        h.loc = next().loc;  // 'on'
        const Token& t = next();
        if (t.kind != Tok::Ident) {
            error(t.loc, "unknown_trigger", "expected a trigger after 'on' (start, tick, event, key, click, enter, exit)");
        } else if (t.text == "start") {
            h.trigger = Trigger::Start;
        } else if (t.text == "tick" || t.text == "update") {
            h.trigger = Trigger::Tick;
        } else if (t.text == "enter") {
            h.trigger = Trigger::Enter;
        } else if (t.text == "exit") {
            h.trigger = Trigger::Exit;
        } else if (t.text == "event" || t.text == "key") {
            h.trigger = t.text == "event" ? Trigger::Event : Trigger::Key;
            if (peek().kind != Tok::String) {
                error(peek().loc, "expected_string", "'on " + t.text + "' needs a quoted name, e.g. on " + t.text +
                                                         (t.text == "key" ? " \"space\"" : " \"door_opened\""));
            } else {
                h.argument = next().text;
            }
        } else if (t.text == "click") {
            h.trigger = Trigger::Click;
        } else {
            // A trigger word registered by an engine subsystem (`on contact`), checked by the compiler.
            h.trigger = Trigger::Event;
            h.custom = true;
            h.argument = t.text;
        }
        if (isWord("with")) {
            next();
            h.binding = name("after 'with'");
        }
        h.body = block({"end"});
        expectWord("end", "to close the 'on " + std::string(toString(h.trigger)) + "' handler");
        return h;
    }

    Block block(std::initializer_list<std::string_view> terminators) {
        Block out;
        while (!atEnd() && !failed()) {
            bool stop = false;
            for (auto term : terminators) stop = stop || isWord(term);
            if (stop) break;
            std::vector<std::string> c = takeComments();
            if (StmtPtr s = statement()) {
                s->comments = std::move(c);
                out.push_back(std::move(s));
            }
        }
        return out;
    }

    StmtPtr make(Stmt::Kind k, SourceLoc loc) {
        auto s = std::make_unique<Stmt>(k, loc);
        s->nodeId = ++nodeCounter_;
        return s;
    }

    void optionalWord(std::initializer_list<std::string_view> words) {
        for (auto w : words) {
            if (isWord(w)) {
                next();
                return;
            }
        }
    }

    StmtPtr statement() {
        const Token& t = peek();
        SourceLoc loc = t.loc;
        if (t.kind != Tok::Ident) {
            error(loc, "unexpected_token", "expected a statement but found '" + describe(t) + "'");
            next();
            return nullptr;
        }
        const std::string w = t.text;
        if (w == "let" || w == "const") {
            next();
            auto s = make(w == "let" ? Stmt::Kind::Let : Stmt::Kind::Const, loc);
            s->name = name(w == "let" ? "after 'let'" : "after 'const'");
            if (isSym(":")) {
                next();
                s->type = typeAnnotation();
            }
            expectSym("=", "after the variable name");
            s->value = expression();
            return s;
        }
        if (w == "set") {
            next();
            auto s = make(Stmt::Kind::Assign, loc);
            s->target = expression();
            expectWord("to", "in 'set <target> to <value>'");
            s->value = expression();
            return s;
        }
        if (w == "if") return ifStatement();
        if (w == "while") {
            next();
            auto s = make(Stmt::Kind::While, loc);
            s->value = expression();
            optionalWord({"do"});
            s->body = block({"end"});
            expectWord("end", "to close 'while'");
            return s;
        }
        if (w == "for") return forStatement();
        if (w == "every" || w == "after") {
            next();
            auto s = make(w == "every" ? Stmt::Kind::Every : Stmt::Kind::After, loc);
            s->value = expression();
            optionalWord({"seconds", "second", "s"});
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
        if (w == "wait") {
            next();
            auto s = make(Stmt::Kind::Wait, loc);
            if (isWord("until")) {
                next();
                s->waitKind = WaitKind::Until;
                s->value = expression();
            } else if (isWord("frames") || isWord("frame")) {
                next();
                s->waitKind = WaitKind::Frames;
                s->value = expression();
            } else {
                s->value = expression();
                if (isWord("frames") || isWord("frame")) {
                    next();
                    s->waitKind = WaitKind::Frames;
                } else {
                    optionalWord({"seconds", "second", "s"});
                }
            }
            return s;
        }
        if (w == "break" || w == "continue") {
            next();
            return make(w == "break" ? Stmt::Kind::Break : Stmt::Kind::Continue, loc);
        }
        if (w == "return") {
            next();
            auto s = make(Stmt::Kind::Return, loc);
            if (startsExpression() && peek().loc.line == loc.line) s->value = expression();
            return s;
        }
        if (w == "stop") {
            next();
            return make(Stmt::Kind::Stop, loc);
        }
        if (w == "go" || w == "goto") {
            next();
            if (w == "go") expectWord("to", "in 'go to <State>'");
            auto s = make(Stmt::Kind::GoTo, loc);
            s->name = name("after 'go to' (a state name)");
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
            for (int i = 0; i < 2; ++i) {
                if (isWord("with") && !s->value) {
                    next();
                    s->value = expression();
                } else if (isWord("to") && !s->extra) {
                    next();
                    s->extra = expression();
                }
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
        if (w == "expect") {
            next();
            auto s = make(Stmt::Kind::Expect, loc);
            s->value = expression();
            if (isSym(",")) next();
            if ((peek().kind == Tok::String || peek().kind == Tok::InterpBegin) && peek().loc.line == loc.line) {
                s->extra = expression();
            }
            return s;
        }
        if (w == "press" || w == "hold" || w == "release" || w == "click") {
            // Test-only input commands (`click` is also a trigger word, but never a statement there).
            if (w == "click" || peek(1).kind == Tok::String || peek(1).kind == Tok::InterpBegin) {
                next();
                auto s = make(w == "press" ? Stmt::Kind::Press
                                : w == "hold"  ? Stmt::Kind::Hold
                                : w == "release" ? Stmt::Kind::Release
                                                 : Stmt::Kind::Click,
                              loc);
                s->value = expression();
                return s;
            }
        }
        // Assignment or call.
        auto e = expression();
        if (!e) return nullptr;
        if (isSym("=")) {
            next();
            auto s = make(Stmt::Kind::Assign, loc);
            s->target = std::move(e);
            s->value = expression();
            return s;
        }
        if (isSym("+=") || isSym("-=") || isSym("*=") || isSym("/=")) {
            auto s = make(Stmt::Kind::OpAssign, loc);
            s->name = std::string(1, next().text[0]);
            s->target = std::move(e);
            s->value = expression();
            return s;
        }
        if (e->kind == Expr::Kind::Call || e->kind == Expr::Kind::MethodCall) {
            auto s = make(Stmt::Kind::Call, loc);
            s->value = std::move(e);
            return s;
        }
        if (e->kind == Expr::Kind::Binary && e->text == "==") {
            error(e->loc, "not_a_statement", "this comparison does nothing on its own",
                  "use `=` to assign, or put the comparison in an `if`");
            return nullptr;
        }
        error(loc, "not_a_statement", "this expression does nothing on its own",
              "did you mean to assign it (x = ...), call a function, or use it in an if?");
        return nullptr;
    }

    bool startsExpression() const {
        const Token& t = peek();
        switch (t.kind) {
            case Tok::Number:
            case Tok::String:
            case Tok::InterpBegin:
            case Tok::Color: return true;
            case Tok::Symbol: return t.text == "(" || t.text == "[" || t.text == "{" || t.text == "-" || t.text == "!";
            case Tok::Ident: {
                static const std::unordered_set<std::string> stmtWords{
                    "end", "else", "elif", "let", "set", "if", "while", "for", "every", "after", "repeat", "wait",
                    "break", "continue", "return", "stop", "go", "goto", "move", "rotate", "look", "emit",
                    "destroy", "log", "print", "expect", "const"};
                return !stmtWords.count(t.text);
            }
            default: return false;
        }
    }

    StmtPtr ifStatement() {
        auto s = make(Stmt::Kind::If, next().loc);
        auto cond = expression();
        optionalWord({"then"});
        Block body = block({"elif", "else", "end"});
        s->branches.emplace_back(std::move(cond), std::move(body));
        while (isWord("elif") || (isWord("else") && isWord("if", 1))) {
            if (isWord("else")) next();
            next();
            auto c = expression();
            optionalWord({"then"});
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

    StmtPtr forStatement() {
        auto s = make(Stmt::Kind::For, next().loc);
        s->name = name("after 'for' (the loop variable)");
        if (isSym(",")) {
            next();
            s->name2 = name("as the second loop variable");
        }
        expectWord("in", "in 'for <name> in <list or range>'");
        noIn_ = true;
        s->value = expression();
        noIn_ = false;
        if (isSym("..") || isSym("..=")) {
            s->inclusive = next().text == "..=";
            s->isRange = true;
            s->extra = expression();
            if (isWord("step")) {
                next();
                s->extra2 = expression();
            }
        }
        optionalWord({"do"});
        s->body = block({"end"});
        expectWord("end", "to close 'for'");
        return s;
    }

    // --- expressions ---------------------------------------------------------------
    ExprPtr mk(Expr::Kind k, SourceLoc loc) { return std::make_unique<Expr>(k, loc); }

    ExprPtr expression() {
        if (++depth_ > 200) {
            error(peek().loc, "too_deep", "expression is nested too deeply");
            --depth_;
            return nullptr;
        }
        auto e = orExpr();
        --depth_;
        return e;
    }

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
        while (true) {
            if (peek().kind == Tok::Symbol && std::find(ops.begin(), ops.end(), peek().text) != ops.end()) {
                const Token& t = next();
                l = binary(std::move(l), t.text, t.loc, additive());
            } else if (isWord("is")) {  // forgiving: "a is b" == "a == b", "a is not b" == "a != b"
                SourceLoc loc = next().loc;
                bool negate = isWord("not");
                if (negate) next();
                l = binary(std::move(l), negate ? "!=" : "==", loc, additive());
            } else if (isWord("in") && !noIn_) {
                SourceLoc loc = next().loc;
                l = binary(std::move(l), "in", loc, additive());
            } else if (isWord("not") && isWord("in", 1) && !noIn_) {
                SourceLoc loc = next().loc;
                next();
                auto inner = binary(std::move(l), "in", loc, additive());
                auto neg = mk(Expr::Kind::Unary, loc);
                neg->text = "not";
                neg->lhs = std::move(inner);
                l = std::move(neg);
            } else {
                break;
            }
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
            auto operand = unary();
            if (operand && operand->kind == Expr::Kind::Number && !operand->parenthesized) {
                operand->number = -operand->number;  // fold "-3" into a literal
                operand->loc = loc;
                return operand;
            }
            auto e = mk(Expr::Kind::Unary, loc);
            e->text = "-";
            e->lhs = std::move(operand);
            return e;
        }
        return postfix();
    }
    ExprPtr postfix() {
        auto e = primary();
        while (e) {
            if (isSym(".")) {
                SourceLoc loc = next().loc;
                if (peek().kind != Tok::Ident) {
                    error(peek().loc, "expected_name", "expected a property name after '.'");
                    return e;
                }
                std::string member = next().text;
                if (isSym("(") && peek().loc.line == loc.line) {
                    auto m = mk(Expr::Kind::MethodCall, loc);
                    m->text = member;
                    m->lhs = std::move(e);
                    callArgs(*m, member);
                    e = std::move(m);
                } else {
                    auto m = mk(Expr::Kind::Member, loc);
                    m->text = member;
                    m->lhs = std::move(e);
                    e = std::move(m);
                }
            } else if (isSym("[") && peek().loc.line == lastLine_) {
                SourceLoc loc = next().loc;
                auto ix = mk(Expr::Kind::Index, loc);
                ix->lhs = std::move(e);
                ix->rhs = expression();
                expectSym("]", "to close the index");
                e = std::move(ix);
            } else {
                break;
            }
        }
        return e;
    }

    void callArgs(Expr& call, const std::string& fname) {
        next();  // '('
        if (!isSym(")")) {
            while (!failed()) {
                bool saved = noIn_;
                noIn_ = false;
                call.args.push_back(expression());
                noIn_ = saved;
                if (isSym(",")) {
                    next();
                    continue;
                }
                break;
            }
        }
        expectSym(")", "to close the call to " + fname + "()");
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
            case Tok::InterpBegin: return interpolation();
            case Tok::InterpMid:
            case Tok::InterpEnd:
                error(loc, "unexpected_token", "unexpected '}' in string interpolation");
                next();
                return nullptr;
            case Tok::Color: {
                auto e = mk(Expr::Kind::Color, loc);
                e->color = next().color;
                return e;
            }
            case Tok::Symbol:
                if (t.text == "(") return parenOrVector();
                if (t.text == "[") return listLiteral();
                if (t.text == "{") return mapLiteral();
                error(loc, "unexpected_token", "expected a value but found '" + t.text + "'");
                next();
                return nullptr;
            case Tok::End:
                error(loc, "unexpected_end", "expected a value but the script ended");
                return nullptr;
            case Tok::Ident: break;
        }
        std::string word = next().text;
        if (word == "true" || word == "false") {
            auto e = mk(Expr::Kind::Bool, loc);
            e->number = word == "true" ? 1 : 0;
            return e;
        }
        if (word == "none" || word == "nothing") return mk(Expr::Kind::None, loc);
        if (isSym("(") && peek().loc.line == loc.line) {
            auto e = mk(Expr::Kind::Call, loc);
            e->text = word;
            callArgs(*e, word);
            return e;
        }
        static const std::unordered_set<std::string> valueWords{"self", "other", "dt", "time", "frame", "pi",
                                                                "state", "state_time"};
        if (isReserved(word) && !valueWords.count(word)) {
            error(loc, "unexpected_keyword", "'" + word + "' cannot be used as a value here");
            return nullptr;
        }
        auto e = mk(Expr::Kind::Ident, loc);
        e->text = word;
        return e;
    }

    ExprPtr interpolation() {
        auto e = mk(Expr::Kind::Interp, peek().loc);
        e->parts.push_back(next().text);  // InterpBegin
        while (!failed()) {
            if (isSym("}") || peek().kind == Tok::InterpMid || peek().kind == Tok::InterpEnd) {
                error(peek().loc, "empty_interpolation", "empty {} in a string", "write \\{ for a literal brace");
                e->args.push_back(mk(Expr::Kind::None, peek().loc));
            } else {
                bool saved = noIn_;
                noIn_ = false;
                e->args.push_back(expression());
                noIn_ = saved;
            }
            if (peek().kind == Tok::InterpMid) {
                e->parts.push_back(next().text);
                continue;
            }
            if (peek().kind == Tok::InterpEnd) {
                e->parts.push_back(next().text);
                break;
            }
            error(peek().loc, "expected_symbol", "expected '}' to close the {interpolation} in the string");
            e->parts.push_back("");
            break;
        }
        return e;
    }

    ExprPtr parenOrVector() {
        SourceLoc loc = next().loc;  // '('
        bool saved = noIn_;
        noIn_ = false;
        auto first = expression();
        if (isSym(")")) {
            next();
            noIn_ = saved;
            if (first) first->parenthesized = true;
            return first;
        }
        auto v = mk(Expr::Kind::Vector, loc);
        v->args.push_back(std::move(first));
        while (isSym(",")) {
            next();
            v->args.push_back(expression());
        }
        noIn_ = saved;
        expectSym(")", "to close the vector");
        if (v->args.size() < 2 || v->args.size() > 4) {
            error(loc, "invalid_vector", "vectors have 2, 3 or 4 components, e.g. (0, 1, 0)");
        }
        return v;
    }

    ExprPtr listLiteral() {
        auto e = mk(Expr::Kind::List, next().loc);  // '['
        bool saved = noIn_;
        noIn_ = false;
        while (!isSym("]") && !atEnd() && !failed()) {
            e->args.push_back(expression());
            if (isSym(",")) {
                next();
                continue;
            }
            break;
        }
        noIn_ = saved;
        expectSym("]", "to close the list");
        return e;
    }

    ExprPtr mapLiteral() {
        auto e = mk(Expr::Kind::Map, next().loc);  // '{'
        bool saved = noIn_;
        noIn_ = false;
        while (!isSym("}") && !atEnd() && !failed()) {
            if (peek().kind == Tok::Ident || peek().kind == Tok::String) {
                e->parts.push_back(next().text);
            } else {
                error(peek().loc, "expected_name", "map keys are names or quoted strings, e.g. {hp: 3, \"max hp\": 5}");
                next();
                continue;
            }
            expectSym(":", "after the map key");
            e->args.push_back(expression());
            if (isSym(",")) {
                next();
                continue;
            }
            break;
        }
        noIn_ = saved;
        expectSym("}", "to close the map");
        return e;
    }

    std::vector<Token> toks_;
    std::vector<Comment> comments_;
    size_t commentPos_ = 0;
    size_t pos_ = 0;
    int lastLine_ = 1;
    std::vector<Diagnostic>& diags_;
    int nodeCounter_ = 0;
    int errorCount_ = 0;
    int depth_ = 0;
    bool noIn_ = false;
};

}  // namespace

ExprPtr parseExpression(std::string_view text, std::vector<Diagnostic>* diagnostics) {
    std::vector<Diagnostic> diags;
    std::vector<Comment> comments;
    auto tokens = Lexer(text, diags).run(comments);
    ExprPtr e = Parser(std::move(tokens), std::move(comments), diags).runExpression();
    bool ok = std::none_of(diags.begin(), diags.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
    if (diagnostics) *diagnostics = std::move(diags);
    return ok ? std::move(e) : nullptr;
}

ParseResult parse(std::string_view source) {
    ParseResult r;
    if (source.size() > 512 * 1024) {
        r.diagnostics.push_back({Severity::Error, {}, "too_large", "script exceeds 512 KiB", "", {}});
        r.module = std::make_shared<Module>();
        return r;
    }
    std::vector<Comment> comments;
    auto tokens = Lexer(source, r.diagnostics).run(comments);
    r.module = Parser(std::move(tokens), std::move(comments), r.diagnostics).run();
    std::stable_sort(r.diagnostics.begin(), r.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.loc.line != b.loc.line ? a.loc.line < b.loc.line : a.loc.column < b.loc.column;
    });
    return r;
}

}  // namespace sky::wander
