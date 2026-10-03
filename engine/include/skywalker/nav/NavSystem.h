#pragma once
// NavSystem: navigation for a scene — gathers walkable geometry (static colliders and static
// meshes), bakes / saves / loads the navmesh described by the scene's `navmesh` component,
// answers path queries, and runs nav agents with DetourCrowd steering while playing.
//
// Agents are updated in scene order with a fixed time step, so navigation replays exactly.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/nav/NavMesh.h"
#include "skywalker/physics/PhysicsWorld.h"
#include "skywalker/scene/Scene.h"

class dtCrowd;

namespace sky::physics {
class PhysicsSystem;
}

namespace sky::nav {

/// Bake settings from a navmesh component (defaults when absent).
BuildSettings settingsFrom(const NavMeshSurface* surface);

class NavSystem {
public:
    NavSystem(Scene& scene, physics::PhysicsSystem& physics, physics::PathResolver paths);
    ~NavSystem();
    NavSystem(const NavSystem&) = delete;
    NavSystem& operator=(const NavSystem&) = delete;

    /// World-space input triangles for the bake and their owner entities.
    void collectGeometry(const BuildSettings& settings, const std::string& mode, std::vector<Vec3>& triangles,
                         std::vector<EntityId>& owners);
    /// Bakes now from the scene's navmesh component (defaults when there is none).
    Result<BuildReport> build();
    /// Bakes now with explicit settings and input geometry mode (both | colliders | meshes).
    Result<BuildReport> build(const BuildSettings& settings, const std::string& geometry);
    /// The current navmesh, baked or loaded on demand (null if there is nothing walkable).
    /// While editing it is refreshed when the scene changes; while playing it is fixed.
    NavMesh* mesh();
    /// Last problem from mesh() (e.g. no geometry), for error messages.
    const std::string& lastError() const { return lastError_; }
    void invalidate() { checkedRevision_ = ~0ull; }

    // --- Play -----------------------------------------------------------------------------------
    void beginPlay();
    void endPlay();
    /// Steers agents (before the physics step): characters get desired velocities, other
    /// agents move their transforms. Returns agents that arrived this tick.
    std::vector<EntityId> update(float dt);
    /// Feeds character-driven agents' real positions back to the crowd (after the physics step).
    void afterPhysics();

    // --- Agent commands (Wander and tools) ------------------------------------------------------
    bool navigate(EntityId e, Vec3 target, EntityId follow = kNoEntity);
    bool stop(EntityId e);
    std::optional<bool> arrived(EntityId e) const;
    std::optional<float> pathLength(Vec3 from, Vec3 to);
    /// The corridor an agent is currently following (debug views), world space.
    std::vector<Vec3> agentPath(EntityId e);

private:
    struct Agent {
        int index = -1;
        uint64_t signature = 0;
        bool hasTarget = false;
        Vec3 target;
        bool arrived = false;
        float yOffset = 0;   // entity origin above the navmesh (non-character agents)
        Vec3 lastPosition;   // what we last wrote / fed, to detect teleports
        int stuckTicks = 0;
    };
    bool ensureCrowd();
    void syncAgents();
    void addAgent(EntityId e, const NavAgent& a, uint64_t sig);
    void removeAgent(std::map<EntityId, Agent>::iterator it);
    /// False when no navmesh polygon is near the target.
    bool requestTarget(Agent& ag, Vec3 target);

    Scene& scene_;
    physics::PhysicsSystem& physics_;
    physics::PathResolver paths_;
    std::unique_ptr<NavMesh> mesh_;
    uint64_t checkedRevision_ = ~0ull;
    std::string lastError_;
    bool playing_ = false;
    bool playMeshReady_ = false;
    dtCrowd* crowd_ = nullptr;
    float crowdMaxRadius_ = 0;
    std::map<EntityId, Agent> agents_;
    std::map<EntityId, EntityId> follows_;  // agent -> entity whose position is its destination
    std::string reportedError_;
};

}  // namespace sky::nav
