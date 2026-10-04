// The running game's localization: game.json settings, the locale and its fallback chain, string
// tables loaded from locale/ (hot reloaded), lookups and missing-key records.

#include "skywalker/locale/Localization.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"

namespace sky::loc {

namespace fs = std::filesystem;

// --- settings -------------------------------------------------------------------------------

Result<LocaleSettings> LocaleSettings::fromJson(const Json& j) {
    LocaleSettings s;
    if (j.isNull()) return s;
    auto bad = [](const std::string& m, const std::string& hint = {}) { return Error::make("invalid_game_json", "game.json localization: " + m, hint); };
    if (!j.isObject()) return bad("must be an object, e.g. {\"source\": \"en\", \"locale\": \"fr\"}");
    static const std::vector<std::string> keys{"source", "locale", "useSystemLocale", "maxLengthRatio"};
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        std::string guess = str::closest(k, keys, 3);
        return bad("unknown field '" + k + "'",
                   (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + "valid fields: source, locale, useSystemLocale, maxLengthRatio");
    }
    for (auto [key, out] : {std::pair<const char*, std::string*>{"source", &s.source}, {"locale", &s.locale}}) {
        const Json* v = j.find(key);
        if (!v || v->isNull()) continue;
        if (!v->isString()) return bad(std::string(key) + " must be a locale code such as \"en\" or \"pt-BR\"");
        *out = normalizeLocale(v->asString());
    }
    if (s.source.empty()) return bad("source must be a locale code such as \"en\"");
    if (const Json* v = j.find("useSystemLocale"); v && !v->isNull()) {
        if (!v->isBool()) return bad("useSystemLocale must be true or false");
        s.useSystemLocale = v->asBool();
    }
    if (const Json* v = j.find("maxLengthRatio"); v && !v->isNull()) {
        if (!v->isNumber() || v->asNumber() < 0 || v->asNumber() > 10) return bad("maxLengthRatio must be a number from 0 to 10 (e.g. 1.3)");
        s.maxLengthRatio = v->asNumber();
    }
    return s;
}

Json LocaleSettings::toJson() const {
    Json j = Json::object({{"source", source}, {"useSystemLocale", useSystemLocale}});
    if (!locale.empty()) j["locale"] = locale;
    if (maxLengthRatio > 0) j["maxLengthRatio"] = maxLengthRatio;
    return j;
}

// --- Localization ---------------------------------------------------------------------------

namespace {

double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

/// File identity for change detection: last write time (compared for equality only) and size.
std::string stampOf(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    auto size = fs::file_size(p, ec);
    return std::to_string(static_cast<long long>(t.time_since_epoch().count())) + ":" + std::to_string(ec ? 0 : size);
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The locale a .po file is for: its name (fr.po, pt_BR.po), else its folder (locale/fr/game.po).
std::string poLocaleHint(const fs::path& p) {
    std::string stem = p.stem().string();
    std::string lang = languageOf(stem);
    if (!lang.empty() && lang.size() <= 3 && std::all_of(lang.begin(), lang.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return stem;
    return p.parent_path().filename().string();
}

}  // namespace

struct Localization::Impl {
    std::string projectDir;
    // game.json
    std::string settingsStamp = "?";
    LocaleSettings settings;
    // tables
    StringTable table;
    std::string signature = "?";
    double lastCheck = -1e9;
    uint64_t revision = 1;
    // locale choices
    std::string preview, game, system;
    // misses: (key, locale) -> count
    std::map<std::pair<std::string, std::string>, int> misses;

    void refreshSettings() {
        const fs::path file = fs::path(projectDir) / "game.json";
        std::error_code ec;
        const std::string stamp = fs::exists(file, ec) ? stampOf(file) : "none";
        if (stamp == settingsStamp) return;
        settingsStamp = stamp;
        settings = LocaleSettings{};
        if (stamp == "none") return;
        auto doc = Json::parse(readText(file));
        if (!doc) return;
        auto s = LocaleSettings::fromJson(doc->get("localization"));
        if (s) settings = *s;
        else log::warn("locale", s.error().message);
        ++revision;
    }

    void reload(bool force) {
        const double now = nowSeconds();
        if (!force && now - lastCheck < 1.0) return;
        lastCheck = now;
        refreshSettings();
        const fs::path dir = fs::path(projectDir) / "locale";
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(dir, ec)) {
            for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = str::lower(it->path().extension().string());
                if (ext == ".csv" || ext == ".po") files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        std::string sig;
        for (const auto& f : files) sig += f.string() + "|" + stampOf(f) + "\n";
        if (sig == signature) return;
        signature = sig;
        StringTable merged;
        for (const auto& f : files) {
            std::error_code rel;
            std::string name = fs::relative(f, projectDir, rel).generic_string();
            if (rel || name.empty()) name = f.string();
            const std::string text = readText(f);
            StringTable t = str::lower(f.extension().string()) == ".po" ? parsePo(text, name, poLocaleHint(f), settings.source)
                                                                         : parseCsv(text, name);
            merged.merge(t);
        }
        for (const auto& p : merged.problems) log::warn("locale", p.file + ":" + std::to_string(p.line) + ": " + p.message);
        table = std::move(merged);
        ++revision;
    }

    /// Whether a locale has its own strings (some locale of its chain other than the source), or is the source language.
    bool hasStrings(const std::string& code) const {
        if (languageOf(code) == languageOf(settings.source)) return true;
        for (const auto& c : fallbackChain(code, settings.source)) {
            if (c != settings.source && table.locales.count(c)) return true;
        }
        return false;
    }

    std::string current() {
        reload(false);
        if (!game.empty()) return game;
        if (!preview.empty()) return preview;
        if (settings.useSystemLocale && !system.empty() && hasStrings(system)) return system;
        if (!settings.locale.empty()) return settings.locale;
        return settings.source;
    }

    /// The message and the locale it was found in.
    const std::string* find(std::string_view key, std::string* foundIn) {
        const std::string loc = current();
        auto it = table.entries.find(std::string(key));
        if (it == table.entries.end()) return nullptr;
        for (const auto& c : fallbackChain(loc, settings.source)) {
            auto t = it->second.text.find(c);
            if (t != it->second.text.end()) {
                if (foundIn) *foundIn = c;
                return &t->second;
            }
        }
        return nullptr;
    }

    void miss(std::string_view key) {
        const std::string loc = current();
        int& n = misses[{std::string(key), loc}];
        if (n++ == 0) log::warn("locale", "missing string '" + std::string(key) + "' (" + loc + ")");
    }
};

Localization::Localization(std::string projectDir) : impl_(std::make_unique<Impl>()) { impl_->projectDir = std::move(projectDir); }
Localization::~Localization() = default;

void Localization::setProjectDir(std::string dir) {
    impl_->projectDir = std::move(dir);
    impl_->settingsStamp = "?";
    impl_->signature = "?";
    impl_->lastCheck = -1e9;
    impl_->misses.clear();
}

const std::string& Localization::projectDir() const { return impl_->projectDir; }

const LocaleSettings& Localization::settings() {
    impl_->refreshSettings();
    return impl_->settings;
}

std::string Localization::locale() { return impl_->current(); }

std::vector<std::string> Localization::chain() { return fallbackChain(impl_->current(), impl_->settings.source); }

Status Localization::setLocale(const std::string& code, Scope scope, bool allowEmpty) {
    impl_->reload(false);
    const std::string c = normalizeLocale(code);
    if (c.empty()) return Error::make("invalid_locale", "'" + code + "' is not a locale code", "use codes like \"en\", \"fr\", \"pt-BR\"");
    if (!allowEmpty && !impl_->hasStrings(c)) {
        std::vector<std::string> have = available();
        std::string guess = str::closest(c, have, 3);
        std::string list;
        for (const auto& h : have) list += (list.empty() ? "" : ", ") + h;
        return Error::make("unknown_locale", "no strings for locale '" + c + "' (available: " + list + ")",
                           guess.empty() ? "add a column to a locale/*.csv table or a locale/" + c + ".po catalog"
                                         : "did you mean '" + guess + "'?");
    }
    (scope == Scope::Game ? impl_->game : impl_->preview) = c;
    ++impl_->revision;
    return {};
}

void Localization::clearLocale(Scope scope) {
    std::string& slot = scope == Scope::Game ? impl_->game : impl_->preview;
    if (slot.empty()) return;
    slot.clear();
    ++impl_->revision;
}

void Localization::setSystemLocale(std::string code) {
    impl_->system = normalizeLocale(code);
    ++impl_->revision;
}

std::vector<std::string> Localization::available() {
    impl_->reload(false);
    std::set<std::string> all = impl_->table.locales;
    all.insert(impl_->settings.source);
    return {all.begin(), all.end()};
}

const std::string* Localization::find(std::string_view key) { return impl_->find(key, nullptr); }

std::string Localization::tr(std::string_view key, const Json& args) {
    std::string foundIn;
    const std::string* msg = impl_->find(key, &foundIn);
    if (!msg) {
        impl_->miss(key);
        return std::string(key);
    }
    return formatMessage(*msg, args, foundIn);
}

std::string Localization::keyOf(std::string_view text) {
    if (text.size() < 2 || text[0] != '@' || text[1] == '@') return {};
    for (size_t i = 1; i < text.size(); ++i) {
        char c = text[i];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-' || c == ':' || c == '/')) return {};
    }
    return std::string(text.substr(1));
}

std::string Localization::text(std::string_view text, const Json& vars) {
    if (text.size() < 2 || text[0] != '@') return std::string(text);
    if (text[1] == '@') return std::string(text.substr(1));
    const std::string key = keyOf(text);
    return key.empty() ? std::string(text) : tr(key, vars);
}

std::string Localization::line(const std::string& lineId, const std::string& source) {
    const std::string key = lineId.empty() ? keyOf(source) : lineId;
    if (key.empty()) return source;
    if (const std::string* msg = impl_->find(key, nullptr)) return *msg;
    // A line id with no string yet shows the script's own text; record it unless that text is already the
    // right language (the source).
    if (lineId.empty() || languageOf(impl_->current()) != languageOf(impl_->settings.source)) impl_->miss(key);
    return lineId.empty() ? key : source;
}

std::vector<Localization::Miss> Localization::misses() const {
    std::vector<Miss> out;
    for (const auto& [k, n] : impl_->misses) out.push_back({k.first, k.second, n});
    return out;
}

void Localization::clearMisses() { impl_->misses.clear(); }

const StringTable& Localization::table() {
    impl_->reload(false);
    return impl_->table;
}

void Localization::refresh(bool force) { impl_->reload(force); }

uint64_t Localization::revision() const { return impl_->revision; }

}  // namespace sky::loc
