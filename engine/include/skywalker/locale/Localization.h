#pragma once
// Localization (docs/LOCALIZATION.md): string tables, plural rules, message formatting and the
// running game's locale.
//
//   locale/*.csv   key + one column per locale ("key,en,fr,pt-BR", optional "comment", "max")
//   locale/*.po    gettext: msgctxt = key (msgid = source text), or msgid = key; plurals become
//                  {count, plural, ...} messages
//
// Messages use an ICU-style subset: {name}, {n, number}, {n, number, integer|percent},
// {n, plural, =0 {none} one {# item} other {# items}}, {g, select, female {...} other {...}},
// {n, selectordinal, one {#st} two {#nd} few {#rd} other {#th}}, with CLDR plural rules for common
// languages and locale number formatting. Text fields (ui.text, ui.placeholder, text.text) holding
// "@key" show the localized string ("@@" escapes a literal '@'); dialogue lines tagged
// #line:<id> are localized by id. The locale is game.json `localization.locale`, the system locale
// in a shipped game (useSystemLocale), set_locale() while playing (reset on stop) or locale_set.
// Lookups fall back along the chain pt-BR -> pt -> the source language; misses are recorded.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::loc {

// --- Locale codes ---------------------------------------------------------------------------

/// Canonical BCP 47-ish form: "pt_BR.UTF-8" -> "pt-BR", "EN" -> "en", "zh_hant_tw" -> "zh-Hant-TW".
/// "" for empty, "C" and "POSIX".
std::string normalizeLocale(std::string_view code);
/// The language subtag ("pt-BR" -> "pt").
std::string languageOf(std::string_view locale);
/// Where lookups go, most specific first, ending with the source language: pt-BR -> pt -> en.
std::vector<std::string> fallbackChain(const std::string& locale, const std::string& source);
/// The process's locale from LC_ALL / LC_MESSAGES / LANG (normalized; "" when unset or C).
std::string systemLocale();

// --- Plural rules and numbers (CLDR) ----------------------------------------------------------

enum class Plural { Zero, One, Two, Few, Many, Other };
const char* toString(Plural p);
std::optional<Plural> pluralFromString(std::string_view s);
/// Cardinal category of `n` as formatted with `fractionDigits` visible decimals (-1: as many as `n` shows).
Plural pluralCategory(const std::string& locale, double n, int fractionDigits = -1);
/// Ordinal category (1st, 2nd, 3rd...): English rules; other languages use `other`.
Plural ordinalCategory(const std::string& locale, double n);
/// Categories a language distinguishes (what a complete plural message covers), in CLDR order.
std::vector<Plural> pluralCategories(const std::string& locale);
/// msgstr[0..n] order of a gettext catalog for the language.
std::vector<Plural> gettextPluralOrder(const std::string& locale);
/// Locale number formatting: "" (up to 3 decimals), "integer", "percent"; grouping and separators per language.
std::string formatNumber(double n, const std::string& locale, std::string_view style = "");

// --- Messages ---------------------------------------------------------------------------------

/// Formats an ICU-style message. Problems (syntax errors, missing arguments) go to `problems`; the
/// output always renders what it can (a missing argument shows as {name}).
std::string formatMessage(std::string_view pattern, const Json& args, const std::string& locale,
                          std::vector<std::string>* problems = nullptr);
/// Syntax check: unbalanced braces, plural / select without an `other` case, unknown argument types.
Status validateMessage(std::string_view pattern);
/// Argument names a message uses ({name}, {n, plural, ...}, nested ones too).
std::set<std::string> placeholders(std::string_view pattern);
/// Rewrites the literal text of a message (not argument names, keywords or selectors), e.g. for pseudo-locales.
std::string transformLiterals(std::string_view pattern, const std::function<std::string(const std::string&)>& fn);
/// Accented, padded text for layout testing ("Play" -> "[Ƥļàý~~]"): braces, rich-text tags and
/// arguments survive; `expand` 0.3 makes it about 30% longer.
std::string pseudoLocalize(std::string_view message, double expand = 0.3);

// --- String tables ----------------------------------------------------------------------------

struct Diagnostic {
    std::string file;
    int line = 0;
    std::string code;
    std::string message;
    Json toJson() const;
};

struct StringEntry {
    std::map<std::string, std::string> text;  // locale -> message
    std::string comment;
    int maxLength = 0;  // characters; 0 = no explicit budget
    std::string file;   // where it was defined (first file)
    int line = 0;
};

struct StringTable {
    std::map<std::string, StringEntry> entries;  // key -> entry
    std::set<std::string> locales;               // every locale that has a column / catalog
    std::vector<Diagnostic> problems;

    /// Adds the other table's strings (existing locale texts are kept; conflicts are diagnostics).
    void merge(const StringTable& other);
};

/// CSV (RFC 4180): a header "key,<locale>,<locale>...", optional "comment" and "max" columns; '#' rows are comments.
StringTable parseCsv(std::string_view text, const std::string& file);
/// gettext .po. `localeHint` (from the file name) is used when the header has no Language.
StringTable parsePo(std::string_view text, const std::string& file, const std::string& localeHint, const std::string& source);
/// One CSV field, quoted when needed.
std::string csvField(std::string_view s);

// --- Settings (game.json "localization") --------------------------------------------------------

struct LocaleSettings {
    std::string source = "en";   // the language the game is written in (fallback of every chain)
    std::string locale;          // the game's locale ("" = source)
    bool useSystemLocale = true; // a shipped game follows the player's system locale when it has strings for it
    double maxLengthRatio = 0;   // locale_check flags translations longer than source x ratio (0 = only per-key "max")

    static Result<LocaleSettings> fromJson(const Json& j);
    Json toJson() const;
};

// --- The running game's localization -------------------------------------------------------------

class Localization {
public:
    explicit Localization(std::string projectDir);
    ~Localization();
    Localization(const Localization&) = delete;
    Localization& operator=(const Localization&) = delete;

    void setProjectDir(std::string dir);
    const std::string& projectDir() const;

    /// game.json `localization` (re-read when the file changes).
    const LocaleSettings& settings();

    // Locale --------------------------------------------------------------------------------
    enum class Scope {
        Preview,  // the editor and tools while editing (kept until changed)
        Game      // set_locale() / locale_set while playing: undone when play stops (replays exactly)
    };
    /// The effective locale: play-time choice, else the preview choice, else the system locale (shipped
    /// games, when useSystemLocale), else game.json, else the source language.
    std::string locale();
    std::vector<std::string> chain();
    /// Errors: unknown_locale (no strings for it; did-you-mean) unless `allowEmpty`.
    Status setLocale(const std::string& code, Scope scope, bool allowEmpty = false);
    void clearLocale(Scope scope);
    /// The OS locale (set by the standalone player; the editor and tests leave it empty).
    void setSystemLocale(std::string code);
    /// Locales with strings, plus the source language, sorted.
    std::vector<std::string> available();

    // Lookups -------------------------------------------------------------------------------
    /// The raw message for a key along the chain (nullptr when no locale of the chain has it).
    const std::string* find(std::string_view key);
    /// The formatted string; a missing key returns the key itself and is recorded.
    std::string tr(std::string_view key, const Json& args = Json::object());
    /// A text field's display: "@key" -> tr(key, vars), "@@x" -> "@x", anything else unchanged.
    std::string text(std::string_view text, const Json& vars = Json::object());
    /// A dialogue line: by `lineId` when given, else by "@key" text; `source` is the script's text (fallback).
    std::string line(const std::string& lineId, const std::string& source);
    /// "@key" -> "key" (empty when the text is not a key reference).
    static std::string keyOf(std::string_view text);

    // Reports and maintenance --------------------------------------------------------------
    struct Miss {
        std::string key;
        std::string locale;
        int count = 0;
    };
    std::vector<Miss> misses() const;
    void clearMisses();
    /// Every string of every table (after a refresh).
    const StringTable& table();
    /// Re-reads locale/*.csv and *.po when they changed (at most once a second unless forced).
    void refresh(bool force = false);
    /// Bumps whenever the tables reload or the locale changes.
    uint64_t revision() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sky::loc
