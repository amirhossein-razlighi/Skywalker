#pragma once
// World2D: the engine subsystem for 2D, text, UI and dialogue. It owns the 2D asset caches, the
// UI system and the running dialogues, and plugs them into the engine's frame and tick:
//
//   Engine::frame  -> adjustView (camera2d) -> gather (sprites, tiles, text, lights, UI quads)
//   Engine::step   -> preTick (UI input, dialogue) -> Wander -> postTick (animations, cameras)
//   Engine::update -> preview (animations while editing)
//
// Everything simulated here runs on the fixed tick and resets on stop, so play sessions replay.

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/locale/Localization.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/ui/Dialogue.h"
#include "skywalker/ui/UiSystem.h"
#include "skywalker/wander/Runtime.h"

namespace sky {

namespace wander {
class BuiltinRegistry;
}
/// Registers play_anim, tile_at, set_tile, start_dialogue, dialogue_var, ... (UiBuiltins.cpp).
void registerUiBuiltins(wander::BuiltinRegistry& reg);

class World2D {
public:
    explicit World2D(std::string projectDir);
    ~World2D();

    render2d::Assets2D& assets() { return *assets_; }
    ui::UiSystem& ui() { return *ui_; }
    /// String tables and the game's locale; "@key" texts and #line:<id> dialogue lines go through it (docs/LOCALIZATION.md).
    loc::Localization& localization() { return *localization_; }

    // --- Frame ------------------------------------------------------------------------
    /// camera2d: orthographic size, pixel snapping and bounds for the camera entity's view.
    /// Returns the world units per texel to snap sprites to (0 = no snapping).
    float adjustView(const Scene& scene, EntityId camera, ViewCamera& view, int width, int height);
    /// Fills frame.render2d: world sprites/tiles/text/lights, then UI quads.
    void gather(const Scene& scene, FrameData& frame, const std::vector<EntityId>& selection, float time, float pixelSnap);
    /// Topmost UI element (`ui` = true) or 2D world entity (sprite, tilemap, text) under a pixel.
    EntityId pick(const Scene& scene, const FrameData& frame, float x, float y, bool ui) const;
    /// Replaces the small marker boxes of 2D entities with their real screen rectangles.
    void refineVisible(const FrameData& frame, std::vector<VisibleEntity>& visible, const Scene& scene) const;

    // --- Simulation ---------------------------------------------------------------------
    /// UI input (mouse, keys, typed text) and dialogues; consumes the typed text.
    void preTick(Scene& scene, wander::InputState& input, wander::Runtime& runtime, float dt);
    /// Size of the view the player sees (the normalized mouse position maps into it). Default 1920x1080.
    void setViewport(int width, int height);
    int viewportWidth() const { return viewW_; }
    int viewportHeight() const { return viewH_; }
    void postTick(Scene& scene, wander::Runtime& runtime, float dt);
    void preview(Scene& scene, float dt);
    void onPlay(Scene& scene, wander::Runtime& runtime);
    void reset();

    // --- Dialogue -------------------------------------------------------------------------
    /// The runner entity to use: `preferred` if it has a dialogue component, else the first one.
    EntityId dialogueRunner(const Scene& scene, EntityId preferred = kNoEntity) const;
    Status startDialogue(Scene& scene, EntityId runner, const std::string& node, wander::Runtime* runtime);
    Status advanceDialogue(Scene& scene, EntityId runner, wander::Runtime* runtime);
    Status chooseDialogue(Scene& scene, EntityId runner, int index, wander::Runtime* runtime);
    Status stopDialogue(Scene& scene, EntityId runner, wander::Runtime* runtime);
    /// The parsed script of a runner component (cached by file time / source).
    Result<std::shared_ptr<const dialogue::Script>> script(const DialogueRunner& runner);
    Json dialogueState(const Scene& scene, EntityId runner) const;

    /// Wander builtins (play_anim, start_dialogue, dialogue_var, tile_at, ...).
    Result<Json> callBuiltin(Scene& scene, wander::Runtime& runtime, const std::string& fn, const std::vector<Json>& args, EntityId self);

    void setProjectDir(const std::string& dir);
    void invalidate(const std::string& absolutePath);

private:
    struct Conversation {
        std::unique_ptr<dialogue::Runner> runner;
        float shown = 0;          // typewriter characters revealed
        int lineSerial = 0;       // bumps with every new line
        bool awaitingRelease = false;
    };
    void syncState(Scene& scene, EntityId e, Conversation& c);
    std::string localizedText(const std::string& text, const Scene& scene, EntityId e);
    void updateDefaultUi(Scene& scene, EntityId e, Conversation& c, wander::Runtime* runtime);
    void hideDefaultUi(Scene& scene);
    dialogue::VarStore vars(Scene& scene, EntityId e);
    dialogue::RunnerEvents events(Scene& scene, EntityId e, wander::Runtime* runtime);
    bool typing(const Scene& scene, EntityId e, const Conversation& c) const;
    void finishIfEnded(Scene& scene, EntityId e, Conversation& c, wander::Runtime* runtime);

    std::unique_ptr<render2d::Assets2D> assets_;
    std::unique_ptr<ui::UiSystem> ui_;
    std::unique_ptr<loc::Localization> localization_;
    std::unordered_map<EntityId, Conversation> conversations_;
    struct CachedScript {
        int64_t mtime = -2;
        std::string source;
        std::shared_ptr<const dialogue::Script> script;
    };
    std::unordered_map<std::string, CachedScript> scripts_;
    bool pointerWasDown_ = false;
    int viewW_ = 1920, viewH_ = 1080;
    std::vector<std::string> pendingKeys_;
};

}  // namespace sky
