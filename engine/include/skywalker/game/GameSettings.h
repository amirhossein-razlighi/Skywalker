#pragma once
// A game's shipping settings: the project's `game.json`.
//
// `game.json` already described a project for people and agents (id, title, genre, mood, pitch).
// It now also configures the standalone player and `skywalker build`: which scene starts, the window,
// the app's identity (bundle id, version, icon) and quality. See docs/SHIPPING.md.

#include <string>
#include <utility>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/RenderLayers.h"

namespace sky {
struct Environment;
}

namespace sky::game {

struct WindowSettings {
    int width = 1280;  // content size in points (a Retina display renders 2x the pixels)
    int height = 720;
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
};

struct GameSettings {
    // Description (people, agents, the studio)
    std::string id, title, genre, mood, pitch;
    std::string assets;  // where the art comes from, e.g. "Poly Haven (CC0)" (informational)
    // Player
    std::string startScene;  // project-relative scene; "" = scenes/main.sky.json, else the first scene found
    WindowSettings window;
    std::string quality = "high";  // low | medium | high | ultra
    float renderScale = 0.f;       // internal resolution override 0.33..1 (0 = quality preset / scene setting)
    bool quitOnEscape = false;     // Escape closes the game (otherwise the game decides, e.g. the "pause" action)
    bool pauseOnFocusLoss = true;  // pause the simulation and sound while another app is in front
    // App identity (`skywalker build`)
    std::string icon;        // project-relative PNG, at least 512x512 (1024 recommended)
    std::string bundleId;    // reverse-DNS, e.g. "com.acme.skydash" (default: dev.skywalker.games.<id>)
    std::string version = "1.0.0";
    std::string copyright;
    // Packaging
    std::vector<std::string> include;  // extra project files/folders to ship (globs: "audio/**", "data/*.json")
    std::vector<std::string> exclude;  // files to leave out even when referenced (globs)
    // Shared folders outside the project: "mounts": {"kit": "../_kit"} makes "kit/..." paths resolve into
    // ../_kit (relative to the project, absolute, or ~). Packaged games get a copy of what they use.
    std::vector<std::pair<std::string, std::string>> mounts;
    // Rendering: "render": {"layers": {"1": "world", "2": "player"}} names the 20 render layers
    render::LayerNames renderLayers;
    // Save games: "saves": {"version": 2, "maxSlots": 20, "compress": false, "migrate": "..."} (game/SaveGame.h)
    Json saves;

    bool fromFile = false;  // a game.json was found

    /// Strict: unknown keys, wrong types and bad values are errors with did-you-mean hints.
    static Result<GameSettings> fromJson(const Json& j);
    /// `<projectDir>/game.json`; a missing file gives the defaults (fromFile = false).
    static Result<GameSettings> load(const std::string& projectDir);
    Json toJson() const;
    /// Writes `<projectDir>/game.json`.
    Status save(const std::string& projectDir) const;

    /// Name shown to players: title, else id, else `fallback`.
    std::string displayName(const std::string& fallback = "Skywalker Game") const;
    std::string effectiveBundleId() const;
    /// Applies the quality preset and renderScale to a scene's environment (the player does this after loading a scene).
    void applyQuality(Environment& env) const;

    static const std::vector<std::string>& qualityPresets();

    /// The mounts of `<projectDir>/game.json` as (name, absolute folder), read leniently (only the
    /// "mounts" key, so a project with other game.json problems still finds its kit). Problems
    /// (bad names, missing folders) go to `warnings`.
    static std::vector<std::pair<std::string, std::string>> readMounts(const std::string& projectDir,
                                                                        std::vector<std::string>* warnings = nullptr);
};

/// Lower-case letters, digits and '-' only ("Sky Dash!" -> "sky-dash").
std::string slugify(const std::string& name);

/// The scene the game starts in: `override`, else settings.startScene, else scenes/main.sky.json, else the first
/// scene file (sorted) in the project. Errors when the project has no scene.
Result<std::string> resolveStartScene(const std::string& projectDir, const GameSettings& settings, const std::string& override = {});

}  // namespace sky::game
