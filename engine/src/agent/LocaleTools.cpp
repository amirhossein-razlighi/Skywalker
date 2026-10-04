// Localization tools (docs/LOCALIZATION.md): locale_list, locale_set, locale_check, locale_extract,
// locale_pseudo.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/locale/LocaleAudit.h"
#include "skywalker/locale/Localization.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

std::string projectRelative(Engine& engine, const std::string& path) {
    if (path.empty()) return {};
    std::string rel = engine.assets().relative(engine.resolvePath(path));
    return rel.empty() ? path : rel;
}

/// Texts of the project, with the open scene read from memory (it may have unsaved edits).
std::vector<loc::TextUse> projectTexts(Engine& engine) {
    const std::string open = projectRelative(engine, engine.scenePath());
    std::vector<loc::TextUse> uses = loc::scanProject(engine.config().projectDir);
    std::erase_if(uses, [&](const loc::TextUse& u) { return !open.empty() && u.file == open; });
    std::vector<loc::TextUse> live = loc::scanScene(engine.scene().toJson(), open.empty() ? "(open scene)" : open);
    uses.insert(uses.end(), live.begin(), live.end());
    return uses;
}

Json localeRows(loc::Localization& l) {
    const loc::StringTable& table = l.table();
    const std::string source = l.settings().source;
    Json rows = Json::array();
    for (const auto& code : l.available()) {
        size_t translated = 0;
        std::set<std::string> files;
        for (const auto& [key, e] : table.entries) {
            if (e.text.count(code)) {
                ++translated;
                files.insert(e.file);
            }
        }
        Json f = Json::array();
        for (const auto& name : files) f.push(name);
        rows.push(Json::object({{"code", code},
                                {"source", code == source},
                                {"strings", translated},
                                {"coverage", table.entries.empty() ? 1.0 : static_cast<double>(translated) / static_cast<double>(table.entries.size())},
                                {"files", f}}));
    }
    return rows;
}

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push(s);
    return a;
}

}  // namespace

void addLocaleTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"locale_list", "List locales",
             "Lists the game's locales: the current one and its fallback chain (pt-BR -> pt -> source), the source language, "
             "every locale with strings (count, coverage, files under locale/), game.json `localization` settings, strings "
             "requested at run time that no table has, and table file problems. Start here before translating or checking.",
             "render", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 loc::Localization& l = engine.world2d().localization();
                 l.refresh(true);
                 Json problems = Json::array();
                 for (const auto& p : l.table().problems) problems.push(p.toJson());
                 Json misses = Json::array();
                 for (const auto& m : l.misses()) misses.push(Json::object({{"key", m.key}, {"locale", m.locale}, {"count", m.count}}));
                 Json out = Json::object({{"current", l.locale()},
                                          {"chain", strings(l.chain())},
                                          {"source", l.settings().source},
                                          {"settings", l.settings().toJson()},
                                          {"keys", l.table().entries.size()},
                                          {"locales", localeRows(l)},
                                          {"runtimeMissing", misses},
                                          {"warnings", problems}});
                 return ToolResult::json(out, "locale " + l.locale() + ", " + std::to_string(l.available().size()) + " locale(s), " +
                                                  std::to_string(l.table().entries.size()) + " keys");
             }});

    reg.add({"locale_set", "Set the locale",
             "Switches the language the game shows: texts written as \"@key\" (ui.text, ui.placeholder, text.text), tr() calls and "
             "#line dialogue lines follow at once. While playing it acts like Wander's set_locale (undone when play stops); "
             "while editing it previews a language in the editor until changed. \"\" returns to the default (game.json "
             "localization.locale, else the source language). Then viewport_capture / ui_inspect to look for overflow. "
             "Example: {\"locale\": \"fr\"}.",
             "render", object({{"locale", string("Locale code (\"fr\", \"pt-BR\", \"en-XA\" for the pseudo-locale); \"\" = default")}}, {"locale"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 loc::Localization& l = engine.world2d().localization();
                 const auto scope = engine.playState() == PlayState::Editing ? loc::Localization::Scope::Preview : loc::Localization::Scope::Game;
                 const std::string code = a.get("locale").asString();
                 if (code.empty() || code == "default") {
                     l.clearLocale(scope);
                 } else if (Status s = l.setLocale(code, scope); !s) {
                     return fail(s);
                 }
                 Json out = Json::object({{"current", l.locale()},
                                          {"chain", strings(l.chain())},
                                          {"scope", scope == loc::Localization::Scope::Game ? "game" : "preview"},
                                          {"available", strings(l.available())},
                                          {"warnings", Json::array()}});
                 return ToolResult::json(out, "locale is now " + l.locale());
             }});

    reg.add({"locale_check", "Check the string tables",
             "Audits localization and returns findings an agent can fix one by one: per locale the keys with no translation "
             "(and coverage); keys the project uses (\"@key\" fields, tr(\"key\") in scripts, #line dialogue ids) that no table "
             "defines; unused keys; placeholder mismatches between a translation and the source ({name} missing or extra); "
             "invalid messages; plurals missing forms the language needs (ru: one/few/many, ar: zero..many); strings longer "
             "than their budget (the table's `max` column, or source length x `budget`, default game.json maxLengthRatio); "
             "characters no font covers (CJK, Arabic...: add a font); keys requested at run time but missing. Example: "
             "{\"locale\": \"de\", \"budget\": 1.3}.",
             "render",
             object({{"locale", string("Only this locale (default: all)")},
                     {"budget", number("Max length ratio translation / source, e.g. 1.3 (default game.json localization.maxLengthRatio)")},
                     {"max_findings", integer("Most findings listed per category (default 200)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 loc::CheckOptions o;
                 o.locale = a.get("locale").asString();
                 o.budget = a.get("budget").asNumber(0);
                 o.maxFindings = static_cast<size_t>(std::clamp<int64_t>(a.get("max_findings").asInt(200), 1, 5000));
                 Json out = loc::checkStrings(engine.world2d().localization(), projectTexts(engine), o, &engine.world2d().assets().fonts());
                 const Json& s = out.get("summary");
                 size_t missing = 0;
                 for (const auto& m : out.get("missing").elements()) missing += static_cast<size_t>(m.get("missing").asInt());
                 return ToolResult::json(out, std::to_string(s.get("keys").asInt()) + " keys: " + std::to_string(missing) + " missing translations, " +
                                                  std::to_string(s.get("undefined").asInt()) + " undefined, " +
                                                  std::to_string(s.get("placeholders").asInt()) + " placeholder mismatches, " +
                                                  std::to_string(s.get("overlong").asInt()) + " overlong");
             }});

    reg.add({"locale_extract", "Extract hard-coded texts",
             "Scans the open scene, scenes, prefabs, dialogue scripts and Wander code for hard-coded player-facing texts (ui.text, "
             "ui.placeholder, text.text, dialogue lines and choices without a #line id, `.text = \"...\"` in scripts) and proposes "
             "a key for each (\"main_menu.play\", \"dlg.intro.start.003\"); a text whose source string already exists reuses its "
             "key. With apply: true it writes the source strings to a CSV table (default locale/strings.csv), replaces the open "
             "scene's fields with \"@key\" (one undoable edit) and tags dialogue lines with #line:<key>; script strings are only "
             "reported (use tr(\"key\")). Example: {\"apply\": true}.",
             "render",
             object({{"apply", boolean("Write the table, update the open scene and tag dialogue lines (default false: proposals only)")},
                     {"file", string("CSV table to write (default locale/strings.csv)")},
                     {"max", integer("Most proposals returned (default 300)")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 loc::Localization& l = engine.world2d().localization();
                 l.refresh(true);
                 const std::string source = l.settings().source;
                 const std::vector<loc::TextUse> uses = projectTexts(engine);
                 std::set<std::string> taken;
                 std::map<std::string, std::string> bySource;  // source text -> existing key
                 for (const auto& [key, e] : l.table().entries) {
                     taken.insert(key);
                     if (auto t = e.text.find(source); t != e.text.end()) bySource.emplace(t->second, key);
                 }
                 for (const auto& u : uses) {
                     if (!u.key.empty()) taken.insert(u.key);
                 }
                 struct Proposal {
                     loc::TextUse use;
                     std::string key;
                     bool reuse = false;
                 };
                 std::vector<Proposal> proposals;
                 for (const auto& u : uses) {
                     if (!u.key.empty() || u.kind == "tr" || !loc::looksTranslatable(u.text)) continue;
                     Proposal p{u, {}, false};
                     if (auto it = bySource.find(u.text); it != bySource.end()) {
                         p.key = it->second;
                         p.reuse = true;
                     } else {
                         p.key = loc::proposeKey(u, taken);
                         bySource.emplace(u.text, p.key);  // the same text elsewhere reuses this key
                     }
                     proposals.push_back(std::move(p));
                 }
                 Json warnings = Json::array();
                 Json applied = Json::object();
                 if (a.get("apply").asBool()) {
                     const std::string open = projectRelative(engine, engine.scenePath());
                     std::vector<std::pair<std::string, std::string>> rows;
                     for (const auto& p : proposals) {
                         if (!p.reuse && p.use.kind != "wander") rows.emplace_back(p.key, p.use.text);
                     }
                     const std::string file = a.get("file").asString("locale/strings.csv");
                     auto written = loc::appendCsvRows(engine.resolvePath(file), source, rows);
                     if (!written) return ToolResult::error(written.error());
                     size_t fields = 0, lines = 0;
                     Status st = engine.edit(ctx.actor, "Localize texts", [&]() -> Status {
                         Scene& s = engine.scene();
                         for (const auto& p : proposals) {
                             const bool inOpen = p.use.file == open || p.use.file == "(open scene)";
                             if ((p.use.kind != "ui" && p.use.kind != "text") || !inOpen || !s.exists(p.use.entity)) continue;
                             const std::string comp = p.use.field.substr(0, p.use.field.find('.'));
                             const std::string field = p.use.field.substr(p.use.field.find('.') + 1);
                             if (Status r = s.patchComponent(p.use.entity, comp, Json::object({{field, "@" + p.key}})); !r) return r;
                             ++fields;
                         }
                         return {};
                     });
                     if (!st) return fail(st);
                     // Dialogue files: tag each line with its id (the text stays the source-language fallback).
                     std::map<std::string, std::map<int, std::string>> tags;
                     for (const auto& p : proposals) {
                         if ((p.use.kind != "dialogue" && p.use.kind != "choice") || p.use.line <= 0) continue;
                         if (p.use.file.empty() || p.use.file == open || !str::startsWith(fs::path(p.use.file).extension().string(), ".dialogue")) {
                             warnings.push("inline dialogue in '" + p.use.entityName + "' (" + p.use.file + "): add #line:" + p.key + " by hand");
                             continue;
                         }
                         tags[p.use.file][p.use.line] = p.key;
                     }
                     for (const auto& [file2, byLine] : tags) {
                         const std::string full = engine.resolvePath(file2);
                         std::ifstream in(full, std::ios::binary);
                         std::stringstream ss;
                         ss << in.rdbuf();
                         std::vector<std::string> text = str::split(ss.str(), '\n');
                         for (const auto& [ln, key] : byLine) {
                             if (ln - 1 < static_cast<int>(text.size())) {
                                 std::string& line = text[static_cast<size_t>(ln - 1)];
                                 const bool cr = !line.empty() && line.back() == '\r';
                                 if (cr) line.pop_back();
                                 line += " #line:" + key + (cr ? "\r" : "");
                                 ++lines;
                             }
                         }
                         std::ofstream out(full, std::ios::binary);
                         for (size_t i = 0; i < text.size(); ++i) out << text[i] << (i + 1 < text.size() ? "\n" : "");
                     }
                     for (const auto& p : proposals) {
                         if (p.use.kind == "wander") warnings.push(p.use.file + ":" + std::to_string(p.use.line) + ": use tr(\"" + p.key + "\") for \"" + p.use.text + "\"");
                     }
                     l.refresh(true);
                     applied = Json::object({{"file", file}, {"rows", written->size()}, {"fields", fields}, {"dialogueLines", lines}});
                 }
                 Json list = Json::array();
                 const size_t max = static_cast<size_t>(std::clamp<int64_t>(a.get("max").asInt(300), 1, 5000));
                 for (const auto& p : proposals) {
                     if (list.size() >= max) break;
                     Json j = p.use.toJson();
                     j["proposedKey"] = p.key;
                     if (p.reuse) j["reuse"] = true;
                     list.push(std::move(j));
                 }
                 Json out = Json::object({{"count", proposals.size()}, {"proposals", list}, {"warnings", warnings}});
                 if (!applied.isNull()) out["applied"] = applied;
                 return ToolResult::json(out, std::to_string(proposals.size()) + " hard-coded text(s)" + (applied.isNull() ? "" : ", applied"));
             }});

    reg.add({"locale_pseudo", "Generate a pseudo-locale",
             "Writes a pseudo-locale for layout testing: every string accented and about 30% longer inside brackets "
             "(\"Play\" -> \"[Ƥļáý~~]\"), placeholders, plurals and rich-text tags intact, so truncation, overflow and hard-coded "
             "texts (they stay plain) jump out. Writes locale/pseudo.csv with the locale en-XA and, by default, switches to it "
             "(like locale_set). Then viewport_capture and ui_inspect each menu; locale_set {\"locale\": \"\"} goes back. "
             "Example: {\"expand\": 0.4}.",
             "render",
             object({{"expand", number("Extra length, 0..2 (default 0.3 = 30% longer)")},
                     {"locale", string("Pseudo-locale code (default en-XA)")},
                     {"activate", boolean("Switch to it afterwards (default true)")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 loc::Localization& l = engine.world2d().localization();
                 l.refresh(true);
                 const std::string code = loc::normalizeLocale(a.get("locale").asString("en-XA"));
                 if (code.empty()) return ToolResult::error(Error::make("invalid_locale", "not a locale code: " + a.get("locale").asString()));
                 const double expand = std::clamp(a.get("expand").asNumber(0.3), 0.0, 2.0);
                 const std::string source = l.settings().source;
                 std::string csv = "key," + loc::csvField(code) + "\n";
                 size_t count = 0;
                 Json sample = Json::object();
                 for (const auto& [key, e] : l.table().entries) {
                     auto t = e.text.find(source);
                     if (t == e.text.end()) continue;
                     const std::string p = loc::pseudoLocalize(t->second, expand);
                     csv += loc::csvField(key) + "," + loc::csvField(p) + "\n";
                     if (sample.size() < 5) sample[key] = p;
                     ++count;
                 }
                 const std::string file = engine.resolvePath("locale/pseudo.csv");
                 std::error_code ec;
                 fs::create_directories(fs::path(file).parent_path(), ec);
                 std::ofstream(file, std::ios::binary) << csv;
                 l.refresh(true);
                 Json warnings = Json::array();
                 if (!count) warnings.push("no source strings yet: run locale_extract {\"apply\": true} first");
                 if (a.get("activate").asBool(true) && count) {
                     const auto scope = engine.playState() == PlayState::Editing ? loc::Localization::Scope::Preview : loc::Localization::Scope::Game;
                     if (Status s = l.setLocale(code, scope); !s) return fail(s);
                 }
                 Json out = Json::object({{"locale", code}, {"file", "locale/pseudo.csv"}, {"strings", count}, {"current", l.locale()},
                                          {"sample", sample}, {"warnings", warnings}});
                 return ToolResult::json(out, "pseudo-locale " + code + ": " + std::to_string(count) + " strings");
             }});
}

}  // namespace sky::tools
