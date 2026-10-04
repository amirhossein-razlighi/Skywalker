// Localization audits: scanning the project for texts and string keys, checking the tables
// (locale_check), proposing keys for hard-coded texts (locale_extract) and writing CSV rows.

#include "skywalker/locale/LocaleAudit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/text/Font.h"
#include "skywalker/ui/Dialogue.h"

namespace sky::loc {

namespace fs = std::filesystem;

Json TextUse::toJson() const {
    Json j = Json::object({{"kind", kind}, {"field", field}, {"text", text}});
    if (!file.empty()) j["file"] = file;
    if (line) j["line"] = line;
    if (entity) j["entity"] = entity;
    if (!entityName.empty()) j["entityName"] = entityName;
    if (!key.empty()) j["key"] = key;
    if (!context.empty()) j["context"] = context;
    return j;
}

namespace {

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string slug(std::string_view s, size_t max = 32) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
        if (out.size() >= max) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

/// The text use of a field: its key when it is written "@key".
TextUse fieldUse(const std::string& kind, const std::string& field, const std::string& text, const std::string& file, uint64_t entity,
                 const std::string& name, const std::string& context) {
    TextUse u;
    u.kind = kind;
    u.field = field;
    u.text = text;
    u.file = file;
    u.entity = entity;
    u.entityName = name;
    u.context = context;
    u.key = Localization::keyOf(text);
    return u;
}

void scanEntity(const Json& e, const std::string& file, const std::string& context, std::vector<TextUse>& out) {
    const uint64_t id = static_cast<uint64_t>(e.get("id").asInt());
    const std::string name = e.get("name").asString();
    const Json& comps = e.get("components");
    if (const Json* ui = comps.find("ui")) {
        for (const char* f : {"text", "placeholder"}) {
            const std::string& t = ui->get(f).asString();
            if (!t.empty()) out.push_back(fieldUse("ui", std::string("ui.") + f, t, file, id, name, context));
        }
    }
    if (const Json* tx = comps.find("text")) {
        const std::string& t = tx->get("text").asString();
        if (!t.empty()) out.push_back(fieldUse("text", "text.text", t, file, id, name, context));
    }
    if (const Json* d = comps.find("dialogue")) {
        const std::string& src = d->get("source").asString();
        if (!src.empty()) {
            for (auto u : scanDialogue(src, file)) {
                u.entity = id;
                u.entityName = name;
                out.push_back(std::move(u));
            }
        }
    }
    for (const auto& b : e.get("behaviors").elements()) {
        for (auto u : scanWander(b.get("source").asString(), file)) {
            u.entity = id;
            u.entityName = name;
            u.context = b.get("name").asString();
            out.push_back(std::move(u));
        }
    }
}

void scanPrefabNode(const Json& node, const std::string& file, const std::string& context, std::vector<TextUse>& out) {
    scanEntity(node, file, context, out);
    for (const auto& c : node.get("children").elements()) scanPrefabNode(c, file, context, out);
}

int lineAt(std::string_view s, size_t pos) { return 1 + static_cast<int>(std::count(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(pos), '\n')); }

}  // namespace

std::vector<TextUse> scanScene(const Json& doc, const std::string& file) {
    std::vector<TextUse> out;
    const std::string fileName = fs::path(file).filename().string();
    // Keys are named after the scene file, or the scene's name when it has no file yet ("(open scene)").
    const std::string context = slug(file.empty() || file[0] == '(' ? doc.get("name").asString() : fileName.substr(0, fileName.find('.')));
    if (doc.contains("entities")) {
        // Canvas names make better key prefixes than the scene's: "main_menu.play".
        std::map<uint64_t, std::string> names;
        std::map<uint64_t, uint64_t> parents;
        for (const auto& e : doc.get("entities").elements()) {
            names[static_cast<uint64_t>(e.get("id").asInt())] = e.get("name").asString();
            parents[static_cast<uint64_t>(e.get("id").asInt())] = static_cast<uint64_t>(e.get("parent").asInt());
        }
        for (const auto& e : doc.get("entities").elements()) {
            std::string ctx = context;
            for (uint64_t p = static_cast<uint64_t>(e.get("parent").asInt()), guard = 0; p && guard < 64; p = parents[p], ++guard) {
                if (!parents[p]) ctx = slug(names[p]);  // the root ancestor (usually the canvas)
            }
            scanEntity(e, file, ctx.empty() ? context : ctx, out);
        }
    } else if (doc.contains("root")) {
        scanPrefabNode(doc.get("root"), file, slug(doc.get("name").asString(context)), out);
    }
    return out;
}

std::vector<TextUse> scanDialogue(std::string_view source, const std::string& file) {
    std::vector<TextUse> out;
    auto script = dialogue::parse(source);
    for (const auto& node : script->nodes) {
        for (const auto& in : node.code) {
            auto add = [&](const std::string& kind, const dialogue::TextTemplate& t, const Json& tags, int line, const std::string& speaker) {
                TextUse u;
                u.kind = kind;
                u.field = kind;
                u.file = file;
                u.line = line;
                u.text = t.source;
                u.context = node.title;
                u.entityName = speaker;
                u.key = tags.get("line").isString() ? tags.get("line").asString() : Localization::keyOf(t.source);
                out.push_back(std::move(u));
            };
            if (in.op == dialogue::Instr::Op::Line) add("dialogue", in.text, in.tags, in.line, in.speaker);
            if (in.op == dialogue::Instr::Op::Choices) {
                for (const auto& o : in.options) add("choice", o.text, o.tags, o.line, "");
            }
        }
    }
    return out;
}

std::vector<TextUse> scanWander(std::string_view source, const std::string& file) {
    std::vector<TextUse> out;
    static const std::regex trCall(R"(\btr\(\s*"([^"\\]+)\")");
    static const std::regex keyLiteral(R"re("@([A-Za-z0-9_.:/-]+)")re");
    static const std::regex textAssign(R"re(\.(?:text|placeholder)\s*=\s*"([^"\\{]*)")re");
    const std::string s(source);
    for (auto it = std::sregex_iterator(s.begin(), s.end(), trCall); it != std::sregex_iterator(); ++it) {
        TextUse u;
        u.kind = "tr";
        u.field = "script";
        u.file = file;
        u.line = lineAt(s, static_cast<size_t>(it->position(0)));
        u.key = (*it)[1].str();
        u.text = "tr(\"" + u.key + "\")";
        out.push_back(std::move(u));
    }
    for (auto it = std::sregex_iterator(s.begin(), s.end(), keyLiteral); it != std::sregex_iterator(); ++it) {
        TextUse u;
        u.kind = "wander";
        u.field = "script";
        u.file = file;
        u.line = lineAt(s, static_cast<size_t>(it->position(0)));
        u.key = (*it)[1].str();
        u.text = "@" + u.key;
        out.push_back(std::move(u));
    }
    for (auto it = std::sregex_iterator(s.begin(), s.end(), textAssign); it != std::sregex_iterator(); ++it) {
        const std::string text = (*it)[1].str();
        if (!looksTranslatable(text)) continue;
        TextUse u;
        u.kind = "wander";
        u.field = "script";
        u.file = file;
        u.line = lineAt(s, static_cast<size_t>(it->position(0)));
        u.text = text;
        out.push_back(std::move(u));
    }
    return out;
}

std::vector<TextUse> scanProject(const std::string& projectDir) {
    std::vector<TextUse> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name.empty() || name[0] == '.' || name == "build" || name == "node_modules" || name == "shots") it.disable_recursion_pending();
            continue;
        }
        if (name.ends_with(".sky.json") || name.ends_with(".prefab.json") || name.ends_with(".wander") || name.ends_with(".dialogue")) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& p : files) {
        std::error_code rel;
        std::string file = fs::relative(p, projectDir, rel).generic_string();
        const std::string text = readText(p);
        const std::string name = p.filename().string();
        std::vector<TextUse> found;
        if (name.ends_with(".wander")) {
            found = scanWander(text, file);
        } else if (name.ends_with(".dialogue")) {
            found = scanDialogue(text, file);
        } else if (auto doc = Json::parse(text)) {
            found = scanScene(*doc, file);
        }
        out.insert(out.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }
    return out;
}

bool looksTranslatable(std::string_view text) {
    std::string t = str::trim(text);
    if (t.empty() || t[0] == '@' || t[0] == '#') return false;
    size_t letters = 0;
    bool space = false;
    for (unsigned char c : t) {
        if (std::isalpha(c) || c >= 0x80) ++letters;
        space = space || c == ' ';
    }
    if (letters < 2) return false;
    // A path or a file name ("audio/hit.wav", "prefabs/coin.prefab.json") is not text.
    if (!space && (t.find('/') != std::string::npos || (t.find('.') != std::string::npos && t.find('.') + 1 < t.size() &&
                                                        std::isalpha(static_cast<unsigned char>(t[t.find('.') + 1]))))) {
        return false;
    }
    return true;
}

std::string proposeKey(const TextUse& use, std::set<std::string>& taken) {
    std::string base;
    const std::string ctx = slug(use.context.empty() ? fs::path(use.file).stem().string() : use.context);
    if (use.kind == "dialogue" || use.kind == "choice") {
        std::string stem = fs::path(use.file).filename().string();
        stem = slug(stem.substr(0, stem.find('.')));
        base = "dlg." + (stem.empty() ? std::string("inline") : stem) + "." + (ctx.empty() ? std::string("node") : ctx);
        for (int n = 1;; ++n) {
            char num[16];
            std::snprintf(num, sizeof(num), "%03d", n);
            std::string k = base + "." + num;
            if (taken.insert(k).second) return k;
        }
    }
    std::string what = slug(use.entityName);
    if (what.empty() || use.kind == "wander") what = slug(use.text, 24);
    if (use.field == "ui.placeholder") what += "_hint";
    base = (ctx.empty() ? std::string("text") : ctx) + "." + (what.empty() ? std::string("text") : what);
    std::string k = base;
    for (int n = 2; !taken.insert(k).second; ++n) k = base + "_" + std::to_string(n);
    return k;
}

size_t visibleLength(std::string_view message) {
    size_t n = 0;
    (void)transformLiterals(message, [&](const std::string& t) {
        for (unsigned char c : t) n += (c & 0xC0) != 0x80 ? 1 : 0;  // UTF-8 code points
        return t;
    });
    return n;
}

namespace {

struct Findings {
    size_t max;
    std::map<std::string, Json> lists;
    std::map<std::string, size_t> counts;
    void add(const std::string& cat, Json item) {
        size_t& n = counts[cat];
        ++n;
        Json& l = lists[cat];
        if (l.isNull()) l = Json::array();
        if (l.size() < max) l.push(std::move(item));
    }
};

}  // namespace

Json checkStrings(Localization& loc, const std::vector<TextUse>& uses, const CheckOptions& options, text::FontLibrary* fonts) {
    loc.refresh(true);
    const StringTable& table = loc.table();
    const LocaleSettings settings = loc.settings();
    const std::string source = settings.source;
    std::vector<std::string> locales;
    for (const auto& l : loc.available()) {
        if (l == source) continue;
        if (!options.locale.empty() && l != normalizeLocale(options.locale)) continue;
        locales.push_back(l);
    }
    const double budget = options.budget > 0 ? options.budget : settings.maxLengthRatio;
    Findings f{options.maxFindings, {}, {}};
    Json warnings = Json::array();
    if (!options.locale.empty() && locales.empty() && normalizeLocale(options.locale) != source) {
        warnings.push("no strings for locale '" + options.locale + "'");
    }

    // Keys used by the project, and where.
    std::map<std::string, const TextUse*> used;
    for (const auto& u : uses) {
        if (!u.key.empty()) used.emplace(u.key, &u);
    }
    for (const auto& [key, u] : used) {
        if (table.entries.count(key)) continue;
        // Dialogue lines with a #line id fall back to the script's own text: that is the source string.
        if ((u->kind == "dialogue" || u->kind == "choice") && Localization::keyOf(u->text).empty()) continue;
        Json item = u->toJson();
        item["key"] = key;
        f.add("undefined", std::move(item));
    }

    for (const auto& [key, entry] : table.entries) {
        const auto src = entry.text.find(source);
        const std::string* srcText = src != entry.text.end() ? &src->second : nullptr;
        if (!used.count(key)) f.add("unused", Json::object({{"key", key}, {"file", entry.file}, {"line", entry.line}}));
        const std::set<std::string> srcArgs = srcText ? placeholders(*srcText) : std::set<std::string>{};
        for (const auto& [l, text] : entry.text) {
            if (!options.locale.empty() && l != normalizeLocale(options.locale) && l != source) continue;
            if (Status s = validateMessage(text); !s) {
                f.add("syntax", Json::object({{"key", key}, {"locale", l}, {"message", s.error().message}}));
            }
            for (const auto& [arg, sel] : pluralSelectors(text)) {
                if (sel.count("other") == 0) continue;  // reported as syntax
                Json missing = Json::array();
                for (Plural p : pluralCategories(l)) {
                    if (p != Plural::Other && !sel.count(toString(p))) missing.push(toString(p));
                }
                if (missing.size()) {
                    f.add("plurals", Json::object({{"key", key}, {"locale", l}, {"argument", arg}, {"missing", missing},
                                                   {"hint", "the language distinguishes these forms; without them `other` is used"}}));
                }
            }
            if (l == source || !srcText) continue;
            const std::set<std::string> args = placeholders(text);
            if (args != srcArgs) {
                Json miss = Json::array(), extra = Json::array();
                for (const auto& a : srcArgs) {
                    if (!args.count(a)) miss.push(a);
                }
                for (const auto& a : args) {
                    if (!srcArgs.count(a)) extra.push(a);
                }
                f.add("placeholders", Json::object({{"key", key}, {"locale", l}, {"missing", miss}, {"extra", extra}}));
            }
            const size_t len = visibleLength(text), srcLen = visibleLength(*srcText);
            size_t limit = entry.maxLength > 0 ? static_cast<size_t>(entry.maxLength) : 0;
            if (budget > 0 && srcLen >= 4) {
                const auto ratioLimit = static_cast<size_t>(std::ceil(static_cast<double>(srcLen) * budget));
                limit = limit ? std::min(limit, ratioLimit) : ratioLimit;
            }
            if (limit && len > limit) {
                f.add("overlong", Json::object({{"key", key}, {"locale", l}, {"length", len}, {"sourceLength", srcLen}, {"limit", limit}}));
            }
        }
        if (srcText && entry.maxLength > 0 && visibleLength(*srcText) > static_cast<size_t>(entry.maxLength)) {
            f.add("overlong", Json::object({{"key", key}, {"locale", source}, {"length", visibleLength(*srcText)}, {"limit", entry.maxLength}}));
        }
        if (!srcText) f.add("no_source", Json::object({{"key", key}, {"file", entry.file}, {"line", entry.line}}));
    }

    // Missing translations, per locale.
    Json missing = Json::array();
    for (const auto& l : locales) {
        Json keys = Json::array();
        size_t count = 0;
        for (const auto& [key, entry] : table.entries) {
            bool found = false;
            for (const auto& c : fallbackChain(l, source)) {
                if (c == source) break;
                found = found || entry.text.count(c);
            }
            if (found) continue;
            ++count;
            if (keys.size() < options.maxFindings) keys.push(key);
        }
        const size_t total = table.entries.size();
        missing.push(Json::object({{"locale", l},
                                   {"missing", count},
                                   {"translated", total - count},
                                   {"coverage", total ? static_cast<double>(total - count) / static_cast<double>(total) : 1.0},
                                   {"keys", keys}}));
    }

    // Characters no font has (scripts the UI fonts do not cover).
    Json glyphs = Json::array();
    if (fonts) {
        std::shared_ptr<text::Font> def = fonts->defaultFont();
        std::vector<std::string> check = locales;
        check.push_back(source);
        for (const auto& l : check) {
            std::set<uint32_t> lacking;
            for (const auto& [key, entry] : table.entries) {
                auto t = entry.text.find(l);
                if (t == entry.text.end()) continue;
                const std::string& s = t->second;
                for (size_t i = 0; i < s.size();) {
                    uint32_t cp = text::decodeUtf8(s, i);
                    if (cp < 0x20 || cp == 0x7F || (cp >= 0x2000 && cp <= 0x200F) || cp == 0xFEFF) continue;
                    text::Font* font = fonts->fallbackFor(cp, def.get());  // the preferred font when none has it
                    if (!font || !font->hasGlyph(cp)) lacking.insert(cp);
                }
            }
            if (lacking.empty()) continue;
            std::string chars;
            for (uint32_t cp : lacking) {
                if (chars.size() > 120) break;
                text::appendUtf8(chars, cp);
            }
            glyphs.push(Json::object({{"locale", l},
                                      {"count", lacking.size()},
                                      {"characters", chars},
                                      {"hint", "no built-in font has these characters: add a font that covers the script (docs/LOCALIZATION.md "
                                               "\"Fonts\") and use it for this locale"}}));
        }
    }

    Json runtime = Json::array();
    for (const auto& m : loc.misses()) runtime.push(Json::object({{"key", m.key}, {"locale", m.locale}, {"count", m.count}}));
    Json problems = Json::array();
    for (const auto& p : table.problems) problems.push(p.toJson());

    Json summary = Json::object({{"keys", table.entries.size()}, {"locales", static_cast<uint64_t>(locales.size())}});
    for (const char* cat : {"undefined", "unused", "placeholders", "syntax", "plurals", "overlong", "no_source"}) {
        summary[cat] = f.counts.count(cat) ? f.counts[cat] : 0;
        if (!f.lists.count(cat)) f.lists[cat] = Json::array();
    }
    summary["glyphs"] = glyphs.size();
    summary["runtimeMissing"] = runtime.size();
    summary["fileProblems"] = problems.size();
    return Json::object({{"source", source},
                         {"current", loc.locale()},
                         {"summary", summary},
                         {"missing", missing},
                         {"undefined", f.lists["undefined"]},
                         {"unused", f.lists["unused"]},
                         {"placeholders", f.lists["placeholders"]},
                         {"syntax", f.lists["syntax"]},
                         {"plurals", f.lists["plurals"]},
                         {"overlong", f.lists["overlong"]},
                         {"noSource", f.lists["no_source"]},
                         {"glyphs", glyphs},
                         {"runtimeMissing", runtime},
                         {"fileProblems", problems},
                         {"warnings", warnings}});
}

Result<std::vector<std::string>> appendCsvRows(const std::string& file, const std::string& sourceLocale,
                                               const std::vector<std::pair<std::string, std::string>>& rows) {
    std::error_code ec;
    std::string existing = fs::exists(file, ec) ? readText(file) : std::string();
    StringTable current = parseCsv(existing, file);
    // Which column holds the source language.
    std::vector<std::string> header;
    size_t sourceCol = 1;
    if (existing.empty()) {
        header = {"key", sourceLocale, "comment"};
        existing = "key," + csvField(sourceLocale) + ",comment\n";
    } else {
        std::string first = existing.substr(0, existing.find('\n'));
        header = str::split(first, ',');
        bool found = false;
        for (size_t i = 1; i < header.size(); ++i) {
            if (normalizeLocale(str::trim(header[i])) == normalizeLocale(sourceLocale)) {
                sourceCol = i;
                found = true;
            }
        }
        if (!found) return Error::make("invalid_csv", file + " has no " + sourceLocale + " column", "add the source language column to its header");
        if (existing.back() != '\n') existing += '\n';
    }
    std::vector<std::string> written;
    for (const auto& [key, text] : rows) {
        if (current.entries.count(key) || std::find(written.begin(), written.end(), key) != written.end()) continue;
        std::vector<std::string> cells(std::max<size_t>(header.size(), sourceCol + 1));
        cells[0] = key;
        cells[sourceCol] = text;
        std::string line;
        for (size_t i = 0; i < cells.size(); ++i) line += (i ? "," : "") + csvField(cells[i]);
        existing += line + "\n";
        written.push_back(key);
    }
    fs::create_directories(fs::path(file).parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) return Error::make("io_error", "cannot write " + file);
    out << existing;
    return written;
}

}  // namespace sky::loc
