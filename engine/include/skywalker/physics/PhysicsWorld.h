#pragma once
// PhysicsWorld: a Jolt Physics simulation that mirrors a Scene.
//
// The scene stays the single source of truth. `sync()` reconciles the world with it every
// tick: bodies are created for new or newly enabled entities, rebuilt when their physics
// components / mesh / scale change, removed with their entities, teleported when something
// else moved their transform, and kinematic bodies follow their transforms. `step()` then
// simulates one fixed tick and writes dynamic poses and velocities back into the scene.
//
// Determinism: entities are processed in scene order, Jolt is built cross-platform
// deterministic, and every callback-derived list (contacts, triggers) is sorted before use,
// so identical scenes and inputs replay identically.
//
// The header is Jolt-free so the engine, tools and tests can use it without Jolt includes.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "skywalker/math/Math.h"
#include "skywalker/scene/Scene.h"

namespace sky {
struct MeshData;
}

namespace sky::physics {

/// CPU mesh for a mesh key ("cube", "asset:..."), or null (provided by the engine).
using MeshProvider = std::function<const MeshData*(const std::string& key)>;
/// Project-relative path -> absolute path (heightmaps).
using PathResolver = std::function<std::string(const std::string& path)>;

struct Hit {
    EntityId entity = kNoEntity;
    Vec3 point;
    Vec3 normal;
    float distance = 0;
};

struct QueryFilter {
    std::vector<EntityId> exclude;  // these entities (and their bodies' parts) are ignored
    bool includeTriggers = false;
    uint32_t layerMask = ~0u;       // bit per collision layer (see layerNames())
};

/// A collision shape for shape casts and overlap tests.
struct QueryShape {
    enum class Kind { Sphere, Box, Capsule } kind = Kind::Sphere;
    float radius = 0.5f;          // sphere / capsule
    float height = 1.f;           // capsule total height (along Y)
    Vec3 halfExtents{0.5f};       // box
    Vec3 rotation{0.f};           // Euler degrees
};

struct ContactEvent {
    enum class Kind { Collide, TriggerEnter, TriggerExit } kind = Kind::Collide;
    EntityId self = kNoEntity;
    EntityId other = kNoEntity;
    Vec3 point;
    Vec3 normal;      // pointing toward `self`
    float speed = 0;  // approach speed along the normal (m/s)
};

struct Stats {
    int bodies = 0, dynamicBodies = 0, kinematicBodies = 0, staticBodies = 0, triggers = 0;
    int activeBodies = 0, sleepingBodies = 0, characters = 0, joints = 0, contacts = 0;
};

/// Wireframe of one collider for debug views (line segment pairs in world space).
struct DebugShape {
    EntityId entity = kNoEntity;
    std::string kind;  // dynamic | kinematic | static | trigger | character
    bool sleeping = false;
    std::vector<Vec3> lines;
};

/// Cooked shapes shared between the worlds of one engine (play, edit queries, settling).
class ShapeCache;
std::shared_ptr<ShapeCache> makeShapeCache();

struct WorldOptions {
    /// Entities simulated as dynamic regardless of their components (physics_settle).
    std::unordered_set<EntityId> forceDynamic;
    /// Dynamic/kinematic bodies not in forceDynamic are frozen in place (physics_settle).
    bool freezeOthers = false;
    /// Use the shared worker pool for stepping (off for query-only worlds).
    bool multithreaded = true;
    /// step() writes dynamic poses, velocities and broken joints into the scene. Off for
    /// what-if simulations that read results with localTransform() (physics_settle).
    bool writeBack = true;
    std::shared_ptr<ShapeCache> shapeCache;  // null = a private cache
    uint32_t maxBodies = 65536;
};

class PhysicsWorld {
public:
    PhysicsWorld(MeshProvider meshes, PathResolver paths, WorldOptions options = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    /// Reconciles the world with the scene (never writes to the scene). `dt` > 0 moves
    /// kinematic bodies smoothly to their transforms; 0 teleports them (queries, editing).
    void sync(const Scene& scene, float dt = 0.f);
    /// Moves characters, simulates one step and writes dynamic poses/velocities back.
    void step(Scene& scene, float dt);

    /// Contacts and trigger enter/exit from the last steps (sorted, deterministic).
    std::vector<ContactEvent> drainEvents();
    /// Joints that broke in the last steps (their `enabled` was set to false).
    std::vector<EntityId> drainBrokenJoints();
    /// Problems found while building (unknown layers, unsupported shapes...), deduplicated.
    std::vector<std::string> drainWarnings();

    // --- Bodies ----------------------------------------------------------------------------
    bool addForce(EntityId e, Vec3 force);
    bool addImpulse(EntityId e, Vec3 impulse);
    bool addTorque(EntityId e, Vec3 torque);
    std::optional<Vec3> velocity(EntityId e) const;
    bool hasBody(EntityId e) const;
    bool isSleeping(EntityId e) const;
    /// True when every dynamic body is asleep (or there are none).
    bool allAsleep() const;
    /// The transform (relative to its scene parent, scale kept) that puts `e` where its body is.
    std::optional<Transform> localTransform(const Scene& scene, EntityId e) const;

    // --- Characters ---------------------------------------------------------------------------
    /// Desired horizontal direction for the next step (|dir| <= 1, scaled by moveSpeed).
    bool walk(EntityId e, Vec3 direction);
    /// Desired horizontal velocity in m/s for the next step (navigation).
    bool setDesiredVelocity(EntityId e, Vec3 velocity);
    bool jump(EntityId e, float speed);
    std::optional<bool> grounded(EntityId e) const;
    bool hasCharacter(EntityId e) const;

    // --- Queries (colliders, deterministic order) -----------------------------------------------
    std::optional<Hit> raycast(Vec3 origin, Vec3 direction, float maxDistance, const QueryFilter& filter = {}) const;
    /// Every hit along the ray (one per entity, nearest first).
    std::vector<Hit> raycastAll(Vec3 origin, Vec3 direction, float maxDistance, const QueryFilter& filter = {}) const;
    std::optional<Hit> shapecast(const QueryShape& shape, Vec3 origin, Vec3 direction, float maxDistance,
                                 const QueryFilter& filter = {}) const;
    /// Entities overlapping the shape at `center`, nearest first (ties by id).
    std::vector<EntityId> overlap(const QueryShape& shape, Vec3 center, const QueryFilter& filter = {}) const;

    // --- Introspection ---------------------------------------------------------------------------
    Stats stats() const;
    /// Collider wireframes (capped at `maxTriangles` per collider for big meshes).
    std::vector<DebugShape> debugShapes(int maxTrianglesPerShape = 4000) const;
    /// World-space triangles of static, solid colliders (navigation mesh input).
    void staticTriangles(std::vector<Vec3>& triangles, std::vector<EntityId>& owners) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Collision layer names, in layer-bit order.
const std::vector<std::string>& layerNameList();

}  // namespace sky::physics
