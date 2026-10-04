#include "skywalker/game/GameSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/game/SaveGame.h"

namespace sky::game {

namespace fs = std::filesystem;

namespace {

Error unknownKey(const std::string& where, const std::string& key, const std::vector<std::string>& valid) {
    std::string guess = str::closest(key, valid, 3);
    std::string list;
    for (const auto& v : valid) list += (list.empty() ? "" : ", ") + v;
    return Error::make("invalid_game_json", where + ": unknown field '" + key + "'",
                       (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + "valid fields: " + list);
}

Status checkKeys(const Json& obj, const std::string& where, const std::vector<std::string>& valid) {
    for (const auto& member : obj.members()) {
        if (std::find(valid.begin(), valid.end(), member.first) == valid.end()) return unknownKey(where, member.first, valid);
    }
    return {};
}

Error typeError(const std::string& field, const char* expected, const Json& got) {
    return Error::make("invalid_game_json", field + " must be " + expected + ", got " + Json::typeName(got.type()));
}

Status readString(const Json& obj, const char* key, std::string& out) {
    const Json* v = obj.find(key);
    if (!v || v->isNull()) return {};
    if (!v->isString()) return typeError(key, "a string", *v);
    out = v->asString();
    return {};
}

Status readBool(const Json& obj, const std::string& prefix, const char* key, bool& out) {
    const Json* v = obj.find(key);
    if (!v || v->isNull()) return {};
    if (!v->isBool()) return typeError(prefix + key, "true or false", *v);
    out = v->asBool();
    return {};
}

Status readInt(const Json& obj, const std::string& prefix, const char* key, int lo, int hi, int& out) {
    const Json* v = obj.find(key);
    if (!v || v->isNull()) return {};
    if (!v->isNumber()) return typeError(prefix + key, "a number", *v);
    double n = v->asNumber();
    if (n < lo || n > hi || n != static_cast<double>(static_cast<int>(n))) {
        return Error::make("invalid_game_json", prefix + key + " must be a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
    }
    out = static_cast<int>(n);
    return {};
}

Status readStrings(const Json& obj, const char* key, std::vector<std::string>& out) {
    const Json* v = obj.find(key);
    if (!v || v->isNull()) return {};
    if (!v->isArray()) return typeError(key, "an array of strings", *v);
    for (const auto& e : v->elements()) {
        if (!e.isString()) return typeError(std::string(key) + "[]", "a string", e);
        out.push_back(e.asString());
    }
    return {};
}

/// "1", "1.2", "1.2.3" (digits and dots): the macOS CFBundleShortVersionString shape.
bool validVersion(const std::string& v) {
    if (v.empty() || v.front() == '.' || v.back() == '.') return false;
    return std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; }) &&
           v.find("..") == std::string::npos;
}

bool validBundleId(const std::string& id) {
    if (id.empty() || id.find('.') == std::string::npos) return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-'; });
}

/// Project-relative paths stay inside the project.
bool relativeInside(const std::string& p) {
    if (p.empty()) return true;
    fs::path path = fs::path(p).lexically_normal();
    return !path.is_absolute() && !path.empty() && *path.begin() != "..";
}

bool isSceneFile(const std::string& name) {
    return name.size() > 9 && name.compare(name.size() - 9, 9, ".sky.json") == 0 && name[0] != '.';
}

bool skippedSceneDir(const std::string& name) {
    return name.empty() || name[0] == '.' || name == "build" || name == "node_modules" || name == "studio";
}

std::vector<std::string> listScenes(const std::string& projectDir) {
    std::vector<std::string> scenes;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (skippedSceneDir(name)) it.disable_recursion_pending();
            continue;
        }
        if (isSceneFile(name)) scenes.push_back(fs::relative(it->path(), projectDir, ec).generic_string());
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

}  // namespace

const std::vector<std::string>& GameSettings::qualityPresets() {
    static const std::vector<std::string> presets{"low", "medium", "high", "ultra"};
    return presets;
}

std::string slugify(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

namespace {
bool validMountName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}
}  // namespace

Result<GameSettings> GameSettings::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_game_json", "game.json must be a JSON object");
    GameSettings g;
    static const std::vector<std::string> keys{"id", "title", "genre", "mood", "pitch", "assets", "startScene", "window", "quality",
                                               "renderScale", "quitOnEscape", "pauseOnFocusLoss", "icon", "bundleId",
                                               "version", "copyright", "include", "exclude", "render", "mounts",
                                               "saves"};
    if (Status s = checkKeys(j, "game.json", keys); !s) return s.error();
    for (auto [key, out] : std::initializer_list<std::pair<const char*, std::string*>>{
             {"id", &g.id}, {"title", &g.title}, {"genre", &g.genre}, {"mood", &g.mood}, {"pitch", &g.pitch}, {"assets", &g.assets},
             {"startScene", &g.startScene}, {"icon", &g.icon}, {"bundleId", &g.bundleId}, {"copyright", &g.copyright}}) {
        if (Status s = readString(j, key, *out); !s) return s.error();
    }
    if (const Json* v = j.find("version"); v && !v->isNull()) {
        if (!v->isString()) return typeError("version", "a string like \"1.0.0\"", *v);
        g.version = v->asString();
    }
    if (const Json* w = j.find("window"); w && !w->isNull()) {
        if (!w->isObject()) return typeError("window", "an object", *w);
        static const std::vector<std::string> wk{"width", "height", "fullscreen", "resizable", "vsync"};
        if (Status s = checkKeys(*w, "game.json window", wk); !s) return s.error();
        if (Status s = readInt(*w, "window.", "width", 160, 16384, g.window.width); !s) return s.error();
        if (Status s = readInt(*w, "window.", "height", 120, 16384, g.window.height); !s) return s.error();
        if (Status s = readBool(*w, "window.", "fullscreen", g.window.fullscreen); !s) return s.error();
        if (Status s = readBool(*w, "window.", "resizable", g.window.resizable); !s) return s.error();
        if (Status s = readBool(*w, "window.", "vsync", g.window.vsync); !s) return s.error();
    }
    if (Status s = readString(j, "quality", g.quality); !s) return s.error();
    g.quality = str::lower(g.quality);
    if (const auto& presets = qualityPresets(); std::find(presets.begin(), presets.end(), g.quality) == presets.end()) {
        std::string guess = str::closest(g.quality, presets, 3);
        return Error::make("invalid_game_json", "quality '" + g.quality + "' is not a preset",
                           (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") + "presets: low, medium, high, ultra");
    }
    if (const Json* v = j.find("renderScale"); v && !v->isNull()) {
        if (!v->isNumber()) return typeError("renderScale", "a number", *v);
        float rs = v->asFloat();
        if (rs != 0.f && (rs < 0.33f || rs > 1.f)) {
            return Error::make("invalid_game_json", "renderScale must be 0 (automatic) or between 0.33 and 1",
                               "0.67 renders at two thirds of the window size and upscales with MetalFX");
        }
        g.renderScale = rs;
    }
    if (Status s = readBool(j, "", "quitOnEscape", g.quitOnEscape); !s) return s.error();
    if (Status s = readBool(j, "", "pauseOnFocusLoss", g.pauseOnFocusLoss); !s) return s.error();
    if (const Json* r = j.find("render"); r && !r->isNull()) {
        if (!r->isObject()) return typeError("render", "an object like {\"layers\": {\"1\": \"world\"}}", *r);
        static const std::vector<std::string> rk{"layers"};
        if (Status s = checkKeys(*r, "game.json render", rk); !s) return s.error();
        auto names = render::LayerNames::fromJson(r->get("layers"));
        if (!names) return names.error();
        g.renderLayers = *names;
    }
    if (Status s = readStrings(j, "include", g.include); !s) return s.error();
    if (Status s = readStrings(j, "exclude", g.exclude); !s) return s.error();
    if (const Json* m = j.find("mounts"); m && !m->isNull()) {
        if (!m->isObject()) return typeError("mounts", "an object like {\"kit\": \"../_kit\"}", *m);
        for (const auto& [name, folder] : m->members()) {
            if (!validMountName(name)) {
                return Error::make("invalid_game_json", "mount name '" + name + "' must be letters, digits, '_' or '-'",
                                   "e.g. \"mounts\": {\"kit\": \"../_kit\"} then use kit/characters/guard.prefab.json");
            }
            if (!folder.isString() || folder.asString().empty()) return typeError("mounts." + name, "a folder path", folder);
            g.mounts.emplace_back(name, folder.asString());
        }
    }

    if (const Json* sv = j.find("saves"); sv && !sv->isNull()) {  // save games: validated by SaveSettings
        auto saves = SaveSettings::fromJson(*sv);
        if (!saves) return saves.error();
        g.saves = *sv;
    }

    if (!validVersion(g.version)) {
        return Error::make("invalid_game_json", "version '" + g.version + "' must be digits separated by dots", "e.g. \"1.0.0\"");
    }
    if (!g.bundleId.empty() && !validBundleId(g.bundleId)) {
        return Error::make("invalid_game_json", "bundleId '" + g.bundleId + "' is not a reverse-DNS identifier",
                           "use letters, digits, '-' and '.', e.g. \"com.acme.skydash\"");
    }
    if (!relativeInside(g.startScene)) {
        return Error::make("invalid_game_json", "startScene must be a path inside the project, got '" + g.startScene + "'");
    }
    if (!relativeInside(g.icon)) {
        return Error::make("invalid_game_json", "icon must be a path inside the project, got '" + g.icon + "'");
    }
    g.fromFile = true;
    return g;
}

Result<GameSettings> GameSettings::load(const std::string& projectDir) {
    fs::path path = fs::path(projectDir) / "game.json";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return GameSettings{};
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc) return Error::make("invalid_game_json", "game.json: " + doc.error().message, doc.error().hint);
    auto settings = fromJson(*doc);
    if (!settings) return Error::make(settings.error().code, "game.json: " + settings.error().message, settings.error().hint);
    return settings;
}

Json GameSettings::toJson() const {
    Json j = Json::object();
    if (!id.empty()) j["id"] = id;
    if (!title.empty()) j["title"] = title;
    if (!genre.empty()) j["genre"] = genre;
    if (!mood.empty()) j["mood"] = mood;
    if (!pitch.empty()) j["pitch"] = pitch;
    if (!assets.empty()) j["assets"] = assets;
    if (!startScene.empty()) j["startScene"] = startScene;
    j["window"] = Json::object({{"width", window.width}, {"height", window.height}, {"fullscreen", window.fullscreen},
                                {"resizable", window.resizable}, {"vsync", window.vsync}});
    j["quality"] = quality;
    if (renderScale > 0.f) j["renderScale"] = renderScale;
    j["quitOnEscape"] = quitOnEscape;
    j["pauseOnFocusLoss"] = pauseOnFocusLoss;
    if (!icon.empty()) j["icon"] = icon;
    if (!bundleId.empty()) j["bundleId"] = bundleId;
    j["version"] = version;
    if (!copyright.empty()) j["copyright"] = copyright;
    auto list = [&](const char* key, const std::vector<std::string>& v) {
        if (v.empty()) return;
        Json a = Json::array();
        for (const auto& s : v) a.push(s);
        j[key] = std::move(a);
    };
    list("include", include);
    list("exclude", exclude);
    if (!mounts.empty()) {
        Json m = Json::object();
        for (const auto& [name, folder] : mounts) m[name] = folder;
        j["mounts"] = std::move(m);
    }
    if (!saves.isNull()) {
        j["saves"] = saves;
    }
    if (Json layers = renderLayers.toJson(); !layers.members().empty()) j["render"] = Json::object({{"layers", layers}});
    return j;
}

Status GameSettings::save(const std::string& projectDir) const {
    std::error_code ec;
    fs::create_directories(projectDir, ec);
    fs::path path = fs::path(projectDir) / "game.json";
    std::ofstream f(path);
    if (!f) return Error::make("io_error", "cannot write " + path.string());
    f << toJson().dump(2) << "\n";
    if (!f) return Error::make("io_error", "cannot write " + path.string());
    return {};
}

std::string GameSettings::displayName(const std::string& fallback) const {
    if (!title.empty()) return title;
    if (!id.empty()) return id;
    return fallback;
}

std::string GameSettings::effectiveBundleId() const {
    if (!bundleId.empty()) return bundleId;
    std::string slug = slugify(!id.empty() ? id : title);
    return "dev.skywalker.games." + (slug.empty() ? std::string("game") : slug);
}

void GameSettings::applyQuality(Environment& env) const {
    // "high" and "ultra" keep what the scene authored; lower presets trade effects for speed.
    if (quality == "low") {
        env.renderScale = std::min(env.renderScale, 0.67f);
        env.gi = 0.f;
        env.ssr = 0.f;
        env.ao = std::min(env.ao, 0.4f);
        env.godRays = 0.f;
        env.cloudMode = "flat";
        env.sharpen = std::max(env.sharpen, 0.5f);
    } else if (quality == "medium") {
        env.renderScale = std::min(env.renderScale, 0.85f);
        env.gi = std::min(env.gi, 0.5f);
        env.godRays = std::min(env.godRays, 0.5f);
    } else if (quality == "ultra") {
        env.renderScale = 1.f;
    }
    if (renderScale > 0.f) env.renderScale = renderScale;
}

Result<std::string> resolveStartScene(const std::string& projectDir, const GameSettings& settings, const std::string& override) {
    std::error_code ec;
    auto exists = [&](const std::string& rel) { return !rel.empty() && fs::is_regular_file(fs::path(projectDir) / rel, ec); };
    for (const std::string* wanted : {&override, &settings.startScene}) {
        if (wanted->empty()) continue;
        if (!exists(*wanted)) {
            std::vector<std::string> scenes = listScenes(projectDir);
            std::string guess = str::closest(*wanted, scenes, 6);
            std::string list;
            for (size_t i = 0; i < scenes.size() && i < 8; ++i) list += (i ? ", " : "") + scenes[i];
            return Error::make("scene_not_found", "start scene '" + *wanted + "' does not exist in the project",
                               (guess.empty() ? std::string() : "did you mean '" + guess + "'? ") +
                                   (list.empty() ? std::string("the project has no scenes") : "scenes: " + list));
        }
        return fs::path(*wanted).lexically_normal().generic_string();
    }
    if (exists("scenes/main.sky.json")) return std::string("scenes/main.sky.json");
    std::vector<std::string> scenes = listScenes(projectDir);
    if (scenes.empty()) {
        return Error::make("no_scene", "the project has no scene (*.sky.json)", "create one with scene_save, then set \"startScene\" in game.json");
    }
    return scenes.front();
}

}  // namespace sky::game

namespace sky::game {

std::vector<std::pair<std::string, std::string>> GameSettings::readMounts(const std::string& projectDir,
                                                                          std::vector<std::string>* warnings) {
    std::vector<std::pair<std::string, std::string>> out;
    auto warn = [&](std::string w) {
        if (warnings) warnings->push_back(std::move(w));
    };
    fs::path path = fs::path(projectDir) / "game.json";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return out;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc || !doc->isObject()) return out;
    const Json& m = doc->get("mounts");
    for (const auto& [name, folder] : m.members()) {
        if (!validMountName(name) || !folder.isString() || folder.asString().empty()) {
            warn("game.json mounts: ignoring '" + name + "' (names are letters, digits, '_' or '-'; values are folder paths)");
            continue;
        }
        std::string p = folder.asString();
        fs::path abs;
        if (p[0] == '~') {
            const char* home = std::getenv("HOME");
            abs = fs::path(home ? home : "") / p.substr(p.size() > 1 ? 2 : 1);
        } else if (fs::path(p).is_absolute()) {
            abs = p;
        } else {
            abs = fs::path(projectDir) / p;
        }
        abs = abs.lexically_normal();
        if (!fs::is_directory(abs, ec)) warn("game.json mounts: folder for '" + name + "' not found: " + abs.string());
        out.emplace_back(name, abs.string());
    }
    return out;
}

}  // namespace sky::game
