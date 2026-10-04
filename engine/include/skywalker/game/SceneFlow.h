#pragma once
// Runtime scene flow (docs/SCENE_FLOW.md): a game moving between scenes while it plays (menus ->
// levels -> credits), entities that persist across changes, additive sub-scenes (rooms, streaming
// chunks, UI overlays) with ownership tracking, time-sliced asset preloading with progress and an
// optional loading scene, and screen transitions (fade, crossfade) as data and state.
//
// Every change happens at the end of a tick (Engine::step), never while scripts run, and always at
// least one tick after it was asked for, so the leaving scene hears `on scene_unloading`. Scenes loaded
// at run time get fresh entity ids (ids are never reused in a play session), so references held by
// persistent entities can never land on an entity of the new scene. Stopping play discards all of it:
// the editor gets the scene that was open when play started.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Renderer.h"

namespace sky {
class Engine;
}

namespace sky::game {

/// How the screen changes: none (cut), fade (to `color` and back), crossfade (old frame over the new one).
struct TransitionSpec {
    std::string kind = "none";
    float duration = 0.f;  // seconds for each half of a fade, or the whole crossfade
    Vec4 color{0.f, 0.f, 0.f, 1.f};

    static Result<TransitionSpec> fromJson(const Json& j);
    Json toJson() const;
};

/// game.json: "scenes" (aliases) and "sceneFlow" (persistent entities, loading scene, default transition).
struct SceneFlowSettings {
    std::vector<std::pair<std::string, std::string>> aliases;  // "menu" -> "scenes/menu.sky.json"
    std::vector<std::string> persistent;  // entity names that survive scene changes (besides `persistent` components)
    std::string loadingScene;             // alias or path shown while the next scene preloads ("" = none)
    TransitionSpec transition;            // default for change_scene
    int preloadPerTick = 4;               // assets loaded per tick while preloading (time slicing)

    static Result<SceneFlowSettings> fromJson(const Json& scenes, const Json& flow);
};

struct ChangeOptions {
    bool hasTransition = false;
    TransitionSpec transition;
    std::vector<std::string> keep;  // more entities (names or "#id") to carry over this time
    std::string spawnAt;            // entity of the new scene the player (persistent.spawn, else tag "player") moves to
    bool hasLoading = false;
    std::string loading;            // loading scene for this change ("" = none)
    bool immediate = false;         // tools: skip transition and preloading, change now (between ticks)
    /// Save games: the new scene's entities get the ids they had when the save was made (file id -> id;
    /// a file id not listed keeps its own id; an id in use gets a fresh one). Off: fresh ids for all.
    bool exactIds = false;
    std::vector<std::pair<uint64_t, uint64_t>> ids;

    static Result<ChangeOptions> fromJson(const Json& j);
};

struct AdditiveOptions {
    std::string id;              // handle ("" = the alias or file name, made unique)
    uint64_t parent = 0;         // load under this entity (0 = at the root)
    bool hasOffset = false;
    Vec3 offset;                 // moves the sub-scene's root entities

    static Result<AdditiveOptions> fromJson(const Json& j);
};

class SceneFlow {
public:
    explicit SceneFlow(Engine& engine);
    ~SceneFlow();
    SceneFlow(const SceneFlow&) = delete;
    SceneFlow& operator=(const SceneFlow&) = delete;

    /// game.json settings (re-read when the file changes).
    SceneFlowSettings settings() const;
    /// Alias or path -> project-relative scene file. Errors: scene_not_found with did-you-mean (aliases and files).
    Result<std::string> resolve(const std::string& idOrPath) const;
    /// The id agents and scripts see for a scene file: its alias if it has one, else the path.
    std::string idOf(const std::string& path) const;

    // Play lifecycle (Engine::play / stop / step).
    void beginPlay();
    void endPlay();
    /// End of a tick: advances the pending change, loads / unloads requested sub-scenes.
    void endTick();

    // Requests (while playing). From scripts they run at the end of the tick.
    Status requestChange(const std::string& target, const ChangeOptions& options);
    /// Returns the handle. `now`: load between ticks (tools) instead of at the end of the tick (scripts).
    Result<std::string> loadAdditive(const std::string& target, const AdditiveOptions& options, bool now);
    struct UnloadResult {
        size_t removed = 0;
        std::vector<uint64_t> orphans;  // entities added under the sub-scene at run time: moved to the root, kept
    };
    /// `now`: remove between ticks (tools); otherwise `on scene_unloading` is heard first and it goes a tick later.
    Result<UnloadResult> unload(const std::string& handle, bool now);

    // State.
    /// The current scene's id (alias or path); "" while editing.
    const std::string& current() const;
    /// The current scene's project-relative file ("" while editing or for an unsaved scene).
    const std::string& currentPath() const;
    /// The ids the current scene's entities got, as (id in the scene file, id now) pairs where they differ,
    /// sorted. Empty for the scene play started in. Save games keep it so a load can rebuild the same ids.
    const std::vector<std::pair<uint64_t, uint64_t>>& sceneIds() const;
    /// Drops a scene change under way, its transition and requested sub-scenes (a save game was loaded).
    void cancel();
    /// 0..1 while a change preloads; 1 when nothing is loading.
    double progress() const;
    bool busy() const;
    /// The transition overlay for this frame (FrameData::fade).
    FrameData::ScreenFade fade() const;
    /// Everything for scene_flow_info: scenes, sub-scenes, persistent entities, the pending change, the transition.
    Json info() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace sky::game
