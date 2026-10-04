#pragma once
// Localization audits for agents (docs/LOCALIZATION.md): where the project's texts are, which
// string keys they use, and what is wrong with the tables (locale_check, locale_extract).

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/locale/Localization.h"

namespace sky::text {
class FontLibrary;
}

namespace sky::loc {

/// One text in the project: a field, a dialogue line or a script string.
struct TextUse {
    std::string kind;   // ui | text | dialogue | choice | wander | tr
    std::string file;   // project-relative ("" = the open scene)
    int line = 0;       // dialogue / script line (1-based)
    uint64_t entity = 0;
    std::string entityName;
    std::string field;  // ui.text, ui.placeholder, text.text, line, choice, script
    std::string text;   // as written
    std::string key;    // the string key it uses ("" = hard-coded text)
    std::string context;  // dialogue node / behavior name / canvas: helps naming keys
    Json toJson() const;
};

/// Texts of a scene or prefab document (ui / text components, inline dialogue, behaviors' scripts).
std::vector<TextUse> scanScene(const Json& doc, const std::string& file);
/// Lines and choices of a .dialogue script (`#line:<id>` tags are keys).
std::vector<TextUse> scanDialogue(std::string_view source, const std::string& file);
/// tr("key") calls, "@key" literals and hard-coded `.text = "..."` assignments in Wander code.
std::vector<TextUse> scanWander(std::string_view source, const std::string& file);
/// Every scene, prefab, .wander and .dialogue file under the project (build folders and dot-folders skipped).
std::vector<TextUse> scanProject(const std::string& projectDir);

/// Whether a hard-coded text is something a player reads (letters, not a path, a number or a key).
bool looksTranslatable(std::string_view text);
/// A readable key for a hard-coded text ("main_menu.play", "dlg.intro.start.003"), unique in `taken` (which it extends).
std::string proposeKey(const TextUse& use, std::set<std::string>& taken);

struct CheckOptions {
    std::string locale;         // only this locale ("" = every locale)
    double budget = 0;          // max translation / source length (0 = game.json maxLengthRatio, per-key "max" always applies)
    size_t maxFindings = 200;   // per category
};
/// locale_check: missing keys per locale, keys used but undefined, unused keys, placeholder mismatches, invalid
/// messages, incomplete plurals, overlong strings, characters no font covers, and runtime misses.
Json checkStrings(Localization& loc, const std::vector<TextUse>& uses, const CheckOptions& options, text::FontLibrary* fonts);

/// Appends rows to a CSV string table (creating it with "key,<source>,comment"); existing keys are skipped.
/// Returns the keys written.
Result<std::vector<std::string>> appendCsvRows(const std::string& file, const std::string& sourceLocale,
                                               const std::vector<std::pair<std::string, std::string>>& rows);

/// Codepoints of a message's literal text (what a player reads; arguments count as nothing).
size_t visibleLength(std::string_view message);
/// Plural / selectordinal arguments of a message and the selectors each one has.
std::vector<std::pair<std::string, std::set<std::string>>> pluralSelectors(std::string_view message);

}  // namespace sky::loc
