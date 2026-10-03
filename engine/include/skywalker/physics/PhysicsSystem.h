#pragma once
// PhysicsSystem: the engine's physics subsystem. Owns the play-time PhysicsWorld (built when
// play starts, stepped every fixed tick, discarded on stop), an edit-time mirror for queries
// (raycasts, overlaps, navmesh input) that follows the scene, and the shape cache shared by
// both. It also implements Wander's physics/character/navigation builtins.
//
// Per tick (Engine::step): Wander tick -> sync world with scene -> navigation steering ->
// characters + physics step -> write back dynamic transforms -> contacts become Wander events.

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/wander/PhysicsHooks.h"

namespace sky::wander {
class Runtime;
}
namespace sky::nav {
class NavSystem;
}

namespace sky::physics {

class PhysicsSystem final : public wander::PhysicsHooks {
public:
    PhysicsSystem(Scene& scene, MeshProvider meshes, PathResolver paths);
    ~PhysicsSystem() override;
    PhysicsSystem(const PhysicsSystem&) = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    void setNavigation(nav::NavSystem* nav) { nav_ = nav; }
    nav::NavSystem* navigation() const { return nav_; }

    // --- Play lifecycle ---------------------------------------------------------------------------
    void beginPlay();
    void endPlay();
    /// One fixed tick: sync, steer agents, simulate, write back, deliver contacts to `runtime`.
    void step(float dt, wander::Runtime& runtime);
    bool playing() const { return playing_; }
    /// The running simulation (null while editing or before the first tick).
    PhysicsWorld* playWorld() { return play_.get(); }

    /// World for queries: the running simulation while playing, otherwise an edit-time mirror
    /// re-synced whenever the scene changed.
    PhysicsWorld& queryWorld();
    /// A separate world sharing this system's shape cache (what-if simulations, settling).
    std::unique_ptr<PhysicsWorld> makeWorld(WorldOptions options) const;

    const MeshProvider& meshes() const { return meshes_; }
    const PathResolver& paths() const { return paths_; }
    /// Recent physics warnings (unknown layers, unsupported shapes, missing joint targets...).
    std::vector<std::string> recentWarnings() const { return {warnings_.begin(), warnings_.end()}; }

    // --- wander::PhysicsHooks -------------------------------------------------------------------
    bool addForce(EntityId e, Vec3 force) override;
    bool addImpulse(EntityId e, Vec3 impulse) override;
    bool addTorque(EntityId e, Vec3 torque) override;
    std::optional<Vec3> velocity(EntityId e) override;
    std::optional<wander::RayHitInfo> raycast(Vec3 origin, Vec3 direction, float maxDistance, EntityId ignore) override;
    std::vector<EntityId> overlapSphere(Vec3 center, float radius, EntityId ignore) override;
    bool walk(EntityId e, Vec3 direction) override;
    bool jump(EntityId e, float speed) override;
    std::optional<bool> grounded(EntityId e) override;
    bool navigate(EntityId e, Vec3 target, EntityId follow) override;
    bool stopNavigation(EntityId e) override;
    std::optional<bool> arrived(EntityId e) override;
    std::optional<float> pathLength(Vec3 from, Vec3 to) override;

private:
    PhysicsWorld& ensurePlayWorld();
    void collectWarnings(PhysicsWorld& world);

    Scene& scene_;
    MeshProvider meshes_;
    PathResolver paths_;
    std::shared_ptr<ShapeCache> cache_;
    std::unique_ptr<PhysicsWorld> play_;
    std::unique_ptr<PhysicsWorld> edit_;
    uint64_t editRevision_ = ~0ull;
    bool playing_ = false;
    nav::NavSystem* nav_ = nullptr;
    std::deque<std::string> warnings_;
};

}  // namespace sky::physics
