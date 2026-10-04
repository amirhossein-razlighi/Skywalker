// ICU-style message format subset: {name}, {n, number[, integer|percent]}, {n, plural, ...},
// {n, selectordinal, ...}, {x, select, ...}, '#' inside plural cases, apostrophe quoting
// ('{' and '' as in ICU). Messages parse into a small tree that is formatted, inspected
// (placeholders) or rewritten (pseudo-localization) without touching the argument syntax.

#include <cmath>
#include <cstdlib>
#include <memory>

#include "skywalker/core/Strings.h"
#include "skywalker/locale/LocaleAudit.h"
#include "skywalker/locale/Localization.h"

namespace sky::loc {

namespace {

struct Node;
using Nodes = std::vector<Node>;

struct Case {
    std::string selector;  // "one", "other", "=0", "female"
    Nodes body;
};

struct Node {
    enum class Kind { Text, Arg, Pound } kind = Kind::Text;
    std::string text;      // Text
    std::string name;      // Arg
    std::string type;      // "" | number | plural | selectordinal | select
    std::string style;     // number style
    double offset = 0;     // plural offset
    std::vector<Case> cases;
};

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Nodes parse() {
        Nodes out = message(0, false);
        if (pos_ < s_.size()) problem("unexpected '}' at " + std::to_string(pos_ + 1));
        return out;
    }
    const std::vector<std::string>& problems() const { return problems_; }

private:
    std::string_view s_;
    size_t pos_ = 0;
    std::vector<std::string> problems_;

    void problem(std::string m) {
        if (problems_.size() < 20) problems_.push_back(std::move(m));
    }
    bool atEnd() const { return pos_ >= s_.size(); }
    void skipSpace() {
        while (!atEnd() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    std::string word() {
        skipSpace();
        size_t start = pos_;
        while (!atEnd() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_' || s_[pos_] == '=' ||
                            s_[pos_] == '-' || s_[pos_] == '.' || s_[pos_] == ':')) {
            ++pos_;
        }
        return std::string(s_.substr(start, pos_ - start));
    }

    /// Text and arguments until an unmatched '}' (depth > 0) or the end.
    Nodes message(int depth, bool inPlural) {
        Nodes out;
        std::string lit;
        auto flush = [&] {
            if (!lit.empty()) out.push_back(Node{Node::Kind::Text, lit, {}, {}, {}, 0, {}});
            lit.clear();
        };
        while (!atEnd()) {
            char c = s_[pos_];
            if (c == '\'') {
                // '' is a quote; '{...' quotes through the next lone quote; any other ' is literal.
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '\'') {
                    lit += '\'';
                    pos_ += 2;
                } else if (pos_ + 1 < s_.size() && (s_[pos_ + 1] == '{' || s_[pos_ + 1] == '}' || (inPlural && s_[pos_ + 1] == '#'))) {
                    ++pos_;
                    while (!atEnd()) {
                        if (s_[pos_] == '\'') {
                            if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '\'') {
                                lit += '\'';
                                pos_ += 2;
                                continue;
                            }
                            ++pos_;
                            break;
                        }
                        lit += s_[pos_++];
                    }
                } else {
                    lit += c;
                    ++pos_;
                }
            } else if (c == '{') {
                flush();
                ++pos_;
                out.push_back(argument(depth + 1));
            } else if (c == '}') {
                if (depth == 0) {
                    problem("unexpected '}' (write '}' in quotes for a literal brace)");
                    lit += c;
                    ++pos_;
                    continue;
                }
                break;
            } else if (c == '#' && inPlural) {
                flush();
                out.push_back(Node{Node::Kind::Pound, {}, {}, {}, {}, 0, {}});
                ++pos_;
            } else {
                lit += c;
                ++pos_;
            }
        }
        flush();
        return out;
    }

    Node argument(int depth) {
        Node n;
        n.kind = Node::Kind::Arg;
        n.name = word();
        skipSpace();
        if (n.name.empty()) problem("an argument has no name ('{' without a name)");
        if (!atEnd() && s_[pos_] == ',') {
            ++pos_;
            n.type = word();
            skipSpace();
            if (n.type != "number" && n.type != "plural" && n.type != "select" && n.type != "selectordinal") {
                problem("unknown argument type '" + n.type + "' in {" + n.name + "} (number, plural, select, selectordinal)");
            }
            if (!atEnd() && s_[pos_] == ',') {
                ++pos_;
                if (n.type == "number") {
                    n.style = word();
                } else {
                    cases(n, depth);
                }
            } else if (n.type != "number") {
                problem("{" + n.name + ", " + n.type + "} needs cases, e.g. one {# item} other {# items}");
            }
        }
        skipSpace();
        if (atEnd() || s_[pos_] != '}') {
            problem("argument {" + n.name + " is not closed with '}'");
        } else {
            ++pos_;
        }
        return n;
    }

    void cases(Node& n, int depth) {
        const bool plural = n.type == "plural" || n.type == "selectordinal";
        while (true) {
            std::string sel = word();
            if (sel.empty()) break;
            if (plural && str::startsWith(sel, "offset:")) {
                n.offset = std::atof(sel.c_str() + 7);
                continue;
            }
            skipSpace();
            if (atEnd() || s_[pos_] != '{') {
                problem("case '" + sel + "' of {" + n.name + "} needs a {message}");
                break;
            }
            ++pos_;
            Case c;
            c.selector = sel;
            c.body = message(depth + 1, plural);
            if (atEnd()) {
                problem("case '" + sel + "' of {" + n.name + "} is not closed with '}'");
            } else {
                ++pos_;
            }
            n.cases.push_back(std::move(c));
            skipSpace();
            if (!atEnd() && s_[pos_] == '}') break;
        }
        bool other = false;
        for (const auto& c : n.cases) other = other || c.selector == "other";
        if (!other) problem("{" + n.name + ", " + n.type + "} has no 'other' case");
        if (plural) {
            for (const auto& c : n.cases) {
                if (c.selector[0] != '=' && !pluralFromString(c.selector)) {
                    problem("'" + c.selector + "' is not a plural category (zero, one, two, few, many, other, or =N)");
                }
            }
        }
    }
};

struct Formatter {
    const Json& args;
    const std::string& locale;
    std::vector<std::string>* problems;

    void note(std::string m) const {
        if (problems && problems->size() < 20) problems->push_back(std::move(m));
    }

    static bool number(const Json& v, double& out) {
        if (v.isNumber()) {
            out = v.asNumber();
            return true;
        }
        if (v.isString()) return str::parseDouble(v.asString(), out);
        return false;
    }

    std::string valueText(const Json& v) const {
        switch (v.type()) {
            case Json::Type::Number: return formatNumber(v.asNumber(), locale);
            case Json::Type::String: return v.asString();
            case Json::Type::Bool: return v.asBool() ? "true" : "false";
            case Json::Type::Null: return "";
            default: return v.dump();
        }
    }

    std::string run(const Nodes& nodes, const std::string* pound) const {
        std::string out;
        for (const Node& n : nodes) {
            switch (n.kind) {
                case Node::Kind::Text: out += n.text; break;
                case Node::Kind::Pound: out += pound ? *pound : "#"; break;
                case Node::Kind::Arg: out += arg(n); break;
            }
        }
        return out;
    }

    std::string arg(const Node& n) const {
        const Json* v = args.isObject() ? args.find(n.name) : nullptr;
        if (!v) {
            note("missing argument '" + n.name + "'");
            return "{" + n.name + "}";
        }
        if (n.type.empty()) return valueText(*v);
        if (n.type == "number") {
            double d = 0;
            if (!number(*v, d)) return valueText(*v);
            return formatNumber(d, locale, n.style);
        }
        if (n.type == "select") {
            const std::string key = v->isString() ? v->asString() : valueText(*v);
            const Case* other = nullptr;
            for (const auto& c : n.cases) {
                if (c.selector == key) return run(c.body, nullptr);
                if (c.selector == "other") other = &c;
            }
            return other ? run(other->body, nullptr) : std::string();
        }
        // plural / selectordinal
        double d = 0;
        if (!number(*v, d)) {
            note("argument '" + n.name + "' of a plural must be a number");
            return valueText(*v);
        }
        const double shown = d - n.offset;
        const std::string pound = formatNumber(shown, locale);
        const Plural cat = n.type == "plural" ? pluralCategory(locale, shown) : ordinalCategory(locale, shown);
        const Case* match = nullptr;
        const Case* other = nullptr;
        for (const auto& c : n.cases) {
            if (c.selector[0] == '=' && std::atof(c.selector.c_str() + 1) == d) {
                match = &c;
                break;
            }
        }
        for (const auto& c : n.cases) {
            if (!match && c.selector == toString(cat)) match = &c;
            if (c.selector == "other") other = &c;
        }
        if (!match) match = other;
        return match ? run(match->body, &pound) : pound;
    }
};

void collect(const Nodes& nodes, std::set<std::string>& out) {
    for (const Node& n : nodes) {
        if (n.kind != Node::Kind::Arg) continue;
        out.insert(n.name);
        for (const auto& c : n.cases) collect(c.body, out);
    }
}

/// Literal text written back so the parser reads it unchanged.
std::string quote(const std::string& text, bool inPlural) {
    std::string out;
    for (char c : text) {
        if (c == '\'') {
            out += "''";
        } else if (c == '{' || c == '}' || (inPlural && c == '#')) {
            out += '\'';
            out += c;
            out += '\'';
        } else {
            out += c;
        }
    }
    return out;
}

std::string serialize(const Nodes& nodes, bool inPlural, const std::function<std::string(const std::string&)>& fn) {
    std::string out;
    for (const Node& n : nodes) {
        switch (n.kind) {
            case Node::Kind::Text: out += quote(fn ? fn(n.text) : n.text, inPlural); break;
            case Node::Kind::Pound: out += '#'; break;
            case Node::Kind::Arg: {
                out += "{" + n.name;
                if (!n.type.empty()) {
                    out += ", " + n.type;
                    if (n.type == "number" && !n.style.empty()) out += ", " + n.style;
                    if (!n.cases.empty()) {
                        out += ",";
                        if (n.offset != 0) out += " offset:" + formatNumber(n.offset, "en");
                        const bool plural = n.type == "plural" || n.type == "selectordinal";
                        for (const auto& c : n.cases) out += " " + c.selector + " {" + serialize(c.body, plural, fn) + "}";
                    }
                }
                out += "}";
                break;
            }
        }
    }
    return out;
}

/// Text decorations that must survive rewriting: rich-text tags (<b>, <color=gold>) and escapes.
std::string accentRun(const std::string& text) {
    static const char* kMap[26] = {"á", "ƀ", "ç", "đ", "é", "ƒ", "ĝ", "ĥ", "í", "ĵ", "ķ", "ļ", "ɱ",
                                   "ñ", "ó", "þ", "ǫ", "ŕ", "š", "ţ", "ú", "ṽ", "ŵ", "ẋ", "ý", "ž"};
    static const char* kUpper[26] = {"Á", "Ɓ", "Ç", "Đ", "É", "Ƒ", "Ĝ", "Ĥ", "Í", "Ĵ", "Ķ", "Ļ", "Ṁ",
                                     "Ñ", "Ó", "Ƥ", "Ǫ", "Ŕ", "Š", "Ţ", "Ú", "Ṽ", "Ŵ", "Ẋ", "Ý", "Ž"};
    std::string out;
    bool inTag = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<') inTag = true;
        if (inTag) {
            out += c;
            if (c == '>') inTag = false;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            out += kMap[c - 'a'];
        } else if (c >= 'A' && c <= 'Z') {
            out += kUpper[c - 'A'];
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

std::string formatMessage(std::string_view pattern, const Json& args, const std::string& locale, std::vector<std::string>* problems) {
    if (pattern.find_first_of("{}'") == std::string_view::npos) return std::string(pattern);  // the common case: plain text
    Parser p(pattern);
    Nodes nodes = p.parse();
    if (problems) problems->insert(problems->end(), p.problems().begin(), p.problems().end());
    Formatter f{args, locale, problems};
    return f.run(nodes, nullptr);
}

Status validateMessage(std::string_view pattern) {
    Parser p(pattern);
    (void)p.parse();
    if (p.problems().empty()) return {};
    return Error::make("invalid_message", p.problems().front(),
                       "messages use {name}, {n, plural, one {# item} other {# items}}, {x, select, a {...} other {...}}; quote "
                       "literal braces as '{'");
}

std::set<std::string> placeholders(std::string_view pattern) {
    std::set<std::string> out;
    Parser p(pattern);
    collect(p.parse(), out);
    return out;
}

namespace {
void selectors(const Nodes& nodes, std::vector<std::pair<std::string, std::set<std::string>>>& out) {
    for (const Node& n : nodes) {
        if (n.kind != Node::Kind::Arg) continue;
        if (n.type == "plural" || n.type == "selectordinal") {
            std::set<std::string> sel;
            for (const auto& c : n.cases) sel.insert(c.selector);
            out.emplace_back(n.name, std::move(sel));
        }
        for (const auto& c : n.cases) selectors(c.body, out);
    }
}
}  // namespace

std::vector<std::pair<std::string, std::set<std::string>>> pluralSelectors(std::string_view message) {
    std::vector<std::pair<std::string, std::set<std::string>>> out;
    Parser p(message);
    selectors(p.parse(), out);
    return out;
}

std::string transformLiterals(std::string_view pattern, const std::function<std::string(const std::string&)>& fn) {
    Parser p(pattern);
    Nodes nodes = p.parse();
    if (!p.problems().empty()) return fn ? fn(std::string(pattern)) : std::string(pattern);  // not a valid message: plain text
    return serialize(nodes, false, fn);
}

std::string pseudoLocalize(std::string_view message, double expand) {
    size_t letters = 0;
    std::string out = transformLiterals(message, [&](const std::string& t) {
        for (char c : t) letters += std::isalpha(static_cast<unsigned char>(c)) ? 1 : 0;
        return accentRun(t);
    });
    const auto pad = static_cast<size_t>(std::ceil(static_cast<double>(letters) * std::max(0.0, expand)));
    return "[" + out + std::string(pad, '~') + "]";
}

}  // namespace sky::loc
