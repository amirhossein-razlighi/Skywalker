// Locale codes, CLDR plural rules and number formatting for the languages games ship in most.
// The rules follow CLDR's cardinal plural rules (operands n, i, v); languages not listed use the
// English rules (one / other).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "skywalker/core/Strings.h"
#include "skywalker/locale/Localization.h"

namespace sky::loc {

// --- codes ----------------------------------------------------------------------------------

std::string normalizeLocale(std::string_view code) {
    std::string c(str::trim(code));
    if (size_t cut = c.find_first_of(".@"); cut != std::string::npos) c.resize(cut);  // "pt_BR.UTF-8", "sr@latin"
    std::replace(c.begin(), c.end(), '_', '-');
    if (c.empty() || c == "C" || c == "POSIX" || c == "c") return {};
    std::vector<std::string> parts = str::split(c, '-');
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string p = parts[i];
        if (p.empty()) continue;
        if (i == 0) {
            p = str::lower(p);
        } else if (p.size() == 4) {  // script: Hant, Latn
            p = str::lower(p);
            p[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        } else if (p.size() == 2 || (p.size() == 3 && std::isdigit(static_cast<unsigned char>(p[0])))) {  // region: BR, 419
            for (auto& ch : p) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        out += (out.empty() ? "" : "-") + p;
    }
    return out;
}

std::string languageOf(std::string_view locale) {
    std::string l = normalizeLocale(locale);
    size_t dash = l.find('-');
    return dash == std::string::npos ? l : l.substr(0, dash);
}

std::vector<std::string> fallbackChain(const std::string& locale, const std::string& source) {
    std::vector<std::string> chain;
    auto add = [&](const std::string& c) {
        if (!c.empty() && std::find(chain.begin(), chain.end(), c) == chain.end()) chain.push_back(c);
    };
    std::string l = normalizeLocale(locale);
    while (!l.empty()) {
        add(l);
        size_t dash = l.rfind('-');
        if (dash == std::string::npos) break;
        l.resize(dash);
    }
    add(normalizeLocale(source));
    return chain;
}

std::string systemLocale() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (v && *v) {
            std::string l = normalizeLocale(v);
            if (!l.empty()) return l;
            if (std::string(var) == "LC_ALL") return {};  // LC_ALL=C means C
        }
    }
    return {};
}

// --- plural rules ---------------------------------------------------------------------------

const char* toString(Plural p) {
    switch (p) {
        case Plural::Zero: return "zero";
        case Plural::One: return "one";
        case Plural::Two: return "two";
        case Plural::Few: return "few";
        case Plural::Many: return "many";
        case Plural::Other: return "other";
    }
    return "other";
}

std::optional<Plural> pluralFromString(std::string_view s) {
    for (Plural p : {Plural::Zero, Plural::One, Plural::Two, Plural::Few, Plural::Many, Plural::Other}) {
        if (s == toString(p)) return p;
    }
    return std::nullopt;
}

namespace {

enum class Rule { Other, English, French, Portuguese, PortugalPt, Slavic, Polish, Czech, Arabic, Hebrew, Romanian, Hindi, Latvian, Lithuanian };

Rule ruleFor(const std::string& locale) {
    const std::string lang = languageOf(locale);
    static const std::set<std::string> none{"ja", "zh", "ko", "vi", "th", "id", "ms", "lo", "my", "km", "yue", "fil", "jv", "su"};
    if (none.count(lang)) return lang == "fil" ? Rule::English : Rule::Other;
    if (lang == "fr") return Rule::French;
    if (lang == "pt") return normalizeLocale(locale) == "pt-PT" ? Rule::PortugalPt : Rule::Portuguese;
    if (lang == "ru" || lang == "uk" || lang == "be" || lang == "sr" || lang == "hr" || lang == "bs") return Rule::Slavic;
    if (lang == "pl") return Rule::Polish;
    if (lang == "cs" || lang == "sk") return Rule::Czech;
    if (lang == "ar") return Rule::Arabic;
    if (lang == "he" || lang == "iw") return Rule::Hebrew;
    if (lang == "ro" || lang == "mo") return Rule::Romanian;
    if (lang == "hi" || lang == "bn" || lang == "fa" || lang == "gu" || lang == "kn" || lang == "zu" || lang == "am") return Rule::Hindi;
    if (lang == "lv") return Rule::Latvian;
    if (lang == "lt") return Rule::Lithuanian;
    return Rule::English;
}

/// Visible fraction digits of `n` as a short decimal (1.50 -> 1.5 -> v = 1), at most 6.
int visibleDigits(double n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", std::fabs(n));
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    size_t dot = s.find('.');
    return dot == std::string::npos ? 0 : static_cast<int>(s.size() - dot - 1);
}

}  // namespace

Plural pluralCategory(const std::string& locale, double value, int fractionDigits) {
    const double n = std::fabs(value);
    const int v = fractionDigits >= 0 ? fractionDigits : visibleDigits(n);
    const auto i = static_cast<long long>(std::floor(n));
    const bool integer = v == 0;
    const long long i10 = i % 10, i100 = i % 100;
    switch (ruleFor(locale)) {
        case Rule::Other: return Plural::Other;
        case Rule::English: return i == 1 && integer ? Plural::One : Plural::Other;
        case Rule::French: return i == 0 || i == 1 ? Plural::One : Plural::Other;
        case Rule::Portuguese: return i == 0 || i == 1 ? Plural::One : Plural::Other;
        case Rule::PortugalPt: return i == 1 && integer ? Plural::One : Plural::Other;
        case Rule::Hindi: return i == 0 || n == 1 ? Plural::One : Plural::Other;
        case Rule::Slavic:
            if (!integer) return Plural::Other;
            if (i10 == 1 && i100 != 11) return Plural::One;
            if (i10 >= 2 && i10 <= 4 && (i100 < 12 || i100 > 14)) return Plural::Few;
            return Plural::Many;
        case Rule::Polish:
            if (!integer) return Plural::Other;
            if (i == 1) return Plural::One;
            if (i10 >= 2 && i10 <= 4 && (i100 < 12 || i100 > 14)) return Plural::Few;
            return Plural::Many;
        case Rule::Czech:
            if (!integer) return Plural::Many;
            if (i == 1) return Plural::One;
            if (i >= 2 && i <= 4) return Plural::Few;
            return Plural::Other;
        case Rule::Arabic: {
            if (n != std::floor(n)) return Plural::Other;
            const long long n100 = i % 100;
            if (i == 0) return Plural::Zero;
            if (i == 1) return Plural::One;
            if (i == 2) return Plural::Two;
            if (n100 >= 3 && n100 <= 10) return Plural::Few;
            if (n100 >= 11 && n100 <= 99) return Plural::Many;
            return Plural::Other;
        }
        case Rule::Hebrew:
            if ((i == 1 && integer) || (i == 0 && !integer)) return Plural::One;
            if (i == 2 && integer) return Plural::Two;
            return Plural::Other;
        case Rule::Romanian:
            if (i == 1 && integer) return Plural::One;
            if (!integer || n == 0 || (i100 >= 2 && i100 <= 19)) return Plural::Few;
            return Plural::Other;
        case Rule::Latvian:
            if (n == 0 || (i100 >= 11 && i100 <= 19)) return Plural::Zero;
            if (i10 == 1 && i100 != 11) return Plural::One;
            return Plural::Other;
        case Rule::Lithuanian:
            if (!integer) return Plural::Many;
            if (i10 == 1 && (i100 < 11 || i100 > 19)) return Plural::One;
            if (i10 >= 2 && i10 <= 9 && (i100 < 11 || i100 > 19)) return Plural::Few;
            return Plural::Other;
    }
    return Plural::Other;
}

Plural ordinalCategory(const std::string& locale, double value) {
    if (languageOf(locale) != "en") return Plural::Other;
    const auto i = static_cast<long long>(std::floor(std::fabs(value)));
    const long long i10 = i % 10, i100 = i % 100;
    if (i10 == 1 && i100 != 11) return Plural::One;
    if (i10 == 2 && i100 != 12) return Plural::Two;
    if (i10 == 3 && i100 != 13) return Plural::Few;
    return Plural::Other;
}

std::vector<Plural> pluralCategories(const std::string& locale) {
    using P = Plural;
    switch (ruleFor(locale)) {
        case Rule::Other: return {P::Other};
        case Rule::Slavic:
        case Rule::Polish: return {P::One, P::Few, P::Many, P::Other};
        case Rule::Czech: return {P::One, P::Few, P::Many, P::Other};
        case Rule::Arabic: return {P::Zero, P::One, P::Two, P::Few, P::Many, P::Other};
        case Rule::Hebrew: return {P::One, P::Two, P::Other};
        case Rule::Romanian: return {P::One, P::Few, P::Other};
        case Rule::Latvian: return {P::Zero, P::One, P::Other};
        case Rule::Lithuanian: return {P::One, P::Few, P::Many, P::Other};
        default: return {P::One, P::Other};
    }
}

std::vector<Plural> gettextPluralOrder(const std::string& locale) {
    using P = Plural;
    switch (ruleFor(locale)) {
        case Rule::Other: return {P::Other};
        case Rule::Slavic:
        case Rule::Polish: return {P::One, P::Few, P::Many};
        case Rule::Czech: return {P::One, P::Few, P::Other};
        case Rule::Arabic: return {P::Zero, P::One, P::Two, P::Few, P::Many, P::Other};
        case Rule::Hebrew: return {P::One, P::Two, P::Other};
        case Rule::Romanian: return {P::One, P::Few, P::Other};
        case Rule::Latvian: return {P::Zero, P::One, P::Other};
        case Rule::Lithuanian: return {P::One, P::Few, P::Other};
        default: return {P::One, P::Other};
    }
}

// --- numbers --------------------------------------------------------------------------------

namespace {

struct NumberSymbols {
    const char* decimal = ".";
    const char* group = ",";
    int minGrouping = 1;            // es, pl: no separator below 10 000
    const char* percentGap = "";    // between the number and '%'
};

NumberSymbols symbolsFor(const std::string& locale) {
    const std::string lang = languageOf(locale);
    const std::string full = normalizeLocale(locale);
    static const char* kNbsp = "\xC2\xA0";        // U+00A0
    static const char* kNarrowNbsp = "\xE2\x80\xAF";  // U+202F
    if (lang == "fr") return {",", kNarrowNbsp, 1, kNarrowNbsp};
    if (lang == "de" || lang == "nl" || lang == "da" || lang == "id" || lang == "tr" || lang == "it" || lang == "el" ||
        lang == "ro" || lang == "hr" || lang == "sr" || lang == "sl") {
        return {",", ".", 1, lang == "de" || lang == "da" || lang == "el" ? kNbsp : ""};
    }
    if (lang == "es") return {",", ".", 2, kNbsp};
    if (lang == "pt") return {",", full == "pt-PT" ? kNbsp : ".", full == "pt-PT" ? 2 : 1, ""};
    if (lang == "ru" || lang == "uk" || lang == "be" || lang == "cs" || lang == "sk" || lang == "sv" || lang == "fi" ||
        lang == "nb" || lang == "no" || lang == "nn" || lang == "hu" || lang == "bg" || lang == "lt" || lang == "lv" || lang == "et") {
        return {",", kNbsp, 1, kNbsp};
    }
    if (lang == "pl") return {",", kNbsp, 2, ""};
    return {};  // en, ja, zh, ko, ar (Latin digits), he, hi, th...
}

}  // namespace

std::string formatNumber(double n, const std::string& locale, std::string_view style) {
    if (!std::isfinite(n)) return std::isnan(n) ? "NaN" : (n < 0 ? "-\xE2\x88\x9E" : "\xE2\x88\x9E");
    const NumberSymbols sym = symbolsFor(locale);
    const bool percent = style == "percent";
    double v = percent ? n * 100.0 : n;
    const int decimals = style == "integer" || percent ? 0 : 3;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, std::fabs(v));
    std::string digits = buf;
    std::string frac;
    if (size_t dot = digits.find('.'); dot != std::string::npos) {
        frac = digits.substr(dot + 1);
        digits.resize(dot);
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
    }
    std::string grouped;
    const bool group = static_cast<int>(digits.size()) >= 4 + (sym.minGrouping - 1);
    for (size_t k = 0; k < digits.size(); ++k) {
        if (group && k > 0 && (digits.size() - k) % 3 == 0) grouped += sym.group;
        grouped += digits[k];
    }
    const bool negative = v < 0 && (grouped != "0" || !frac.empty());
    std::string out = (negative ? "-" : "") + grouped;
    if (!frac.empty()) out += sym.decimal + frac;
    if (percent) out += std::string(sym.percentGap) + "%";
    return out;
}

}  // namespace sky::loc
