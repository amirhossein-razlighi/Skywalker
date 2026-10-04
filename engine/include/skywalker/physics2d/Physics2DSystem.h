#pragma once
// Physics2DSystem: the engine's 2D physics subsystem (Box2D). Owns the play-time Physics2DWorld
// (built when play starts, stepped every fixed tick, discarded on stop so nothing survives a stop),
// an edit-time mirror for queries that follows the scene, and the debug drawing of shapes, contacts
// and joints. Wander's 2D builtins (push2d, raycast2d, move2d, ...) reach it as a runtime service.
//
// Per tick (Engine::step, after the 3D physics step): sync the world with the scene -> characters ->
// Box2D step in sub-steps -> write back x/y and the Z rotation -> contacts, sensors and impacts become
// Wander events (on collide, on trigger_enter, on trigger_exit, on collide_end, on impact).

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/physics2d/Physics2DWorld.h"

namespace sky {
struct Frame2D;
}
namespace sky::render2d {
class Assets2D;
}
namespace sky::wander {
class Runtime;
}

namespace sky::physics2d {

class Physics2DSystem {
public:
    Physics2DSystem(Scene& scene, TileCollisionProvider tiles);
    ~Physics2DSystem();
    Physics2DSystem(const Physics2DSystem&) = delete;
    Physics2DSystem& operator=(const Physics2DSystem&) = delete;

    void beginPlay();
    void endPlay();
    /// One fixed tick: sync, move characters, simulate, write back, deliver events to `runtime`.
    void step(float dt, wander::Runtime& runtime);
    bool playing() const { return playing_; }
    /// The running simulation (null while editing or before the first tick).
    Physics2DWorld* playWorld() { return play_.get(); }
    /// World for queries: the running simulation while playing, otherwise an edit-time mirror.
    Physics2DWorld& queryWorld();
    /// A separate world (what-if simulations: physics2d_settle).
    std::unique_ptr<Physics2DWorld> makeWorld(WorldOptions2D options) const;
    /// Whether the scene uses 2D physics at all (any body2d, collider2d or character2d).
    bool inUse() const;
    /// Recent warnings (bad shapes, unknown layers, missing joint bodies...).
    std::vector<std::string> recentWarnings() const { return {warnings_.begin(), warnings_.end()}; }
    const TileCollisionProvider& tiles() const { return tiles_; }

    /// physics2d_world.debugDraw: shapes (by kind), contacts and joints as world-space lines.
    bool debugDrawEnabled() const;
    void gatherDebug(Frame2D& frame);

private:
    Physics2DWorld& ensurePlayWorld();
    void collectWarnings(Physics2DWorld& world);

    Scene& scene_;
    TileCollisionProvider tiles_;
    std::unique_ptr<Physics2DWorld> play_;
    std::unique_ptr<Physics2DWorld> edit_;
    uint64_t editRevision_ = ~0ull;
    bool playing_ = false;
    std::deque<std::string> warnings_;
};

/// Tile collision from a project's tilesets: `solid` ids and the `collision` table of *.tileset.json.
TileCollisionProvider tileCollisionFrom(render2d::Assets2D& assets);

}  // namespace sky::physics2d
