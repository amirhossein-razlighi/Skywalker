#pragma once
// physics builtins: the engine services behind Wander's physics, character and navigation
// functions (push, impulse, raycast, walk, navigate, ...). The engine implements this interface
// (skywalker/physics) and installs it on the runtime; without it those builtins report that
// physics is unavailable. Kept separate from the interpreter so a new VM can reuse it as is.

#include <optional>
#include <vector>

#include "skywalker/math/Math.h"
#include "skywalker/scene/Scene.h"

namespace sky::wander {

struct RayHitInfo {
    EntityId entity = kNoEntity;
    Vec3 point;
    Vec3 normal;
    float distance = 0;
};

class PhysicsHooks {
public:
    virtual ~PhysicsHooks() = default;

    // Rigid bodies. False when the entity has no dynamic body in the running simulation.
    virtual bool addForce(EntityId e, Vec3 force) = 0;      // N, applied over the next step
    virtual bool addImpulse(EntityId e, Vec3 impulse) = 0;  // N s, instant velocity change
    virtual bool addTorque(EntityId e, Vec3 torque) = 0;    // N m
    /// Current linear velocity of a body or character.
    virtual std::optional<Vec3> velocity(EntityId e) = 0;

    // Queries against the physics world (colliders, not render meshes). Triggers are ignored.
    virtual std::optional<RayHitInfo> raycast(Vec3 origin, Vec3 direction, float maxDistance, EntityId ignore) = 0;
    /// Entities whose colliders overlap the sphere, nearest first (ties by id).
    virtual std::vector<EntityId> overlapSphere(Vec3 center, float radius, EntityId ignore) = 0;

    // Character controllers. False when the entity has no character.
    virtual bool walk(EntityId e, Vec3 direction) = 0;  // desired direction for this tick (|dir| <= 1)
    virtual bool jump(EntityId e, float speed) = 0;     // speed <= 0 = the character's jumpSpeed; false if airborne
    virtual std::optional<bool> grounded(EntityId e) = 0;

    // Navigation. False when the entity has no nav_agent.
    /// Walk to `target`; with `follow` set the destination tracks that entity as it moves.
    virtual bool navigate(EntityId e, Vec3 target, EntityId follow) = 0;
    virtual bool stopNavigation(EntityId e) = 0;
    virtual std::optional<bool> arrived(EntityId e) = 0;
    /// Walking distance along the navmesh; nullopt when unreachable or there is no navmesh.
    virtual std::optional<float> pathLength(Vec3 from, Vec3 to) = 0;
};

}  // namespace sky::wander
