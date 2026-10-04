#pragma once
// Physics2DWorld: a Box2D v3 simulation of a Scene's 2D physics components on the XY plane.
//
// The scene stays the single source of truth. `sync()` reconciles the world with it every tick:
// bodies are created for new or newly enabled entities, rebuilt when their components (or the
// tilemap behind a tilemap collider) change, removed with their entities, teleported when something
// else moved their transform; kinematic bodies follow their transforms. `step()` then moves the
// character2d controllers, simulates one fixed tick in sub-steps and writes dynamic poses and
// velocities back into the scene (x, y and the rotation around Z; z, the other rotations and scale
// are kept).
//
// Determinism: entities are processed in scene order, Box2D runs single-threaded (it is
// deterministic for identical input sequences and compiled without FMA contraction), and every event
// list is sorted before use, so identical scenes and inputs replay identically.
//
// The header is Box2D-free so the engine, tools and tests can use it without Box2D includes.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/physics2d/TileColliders.h"
#include "skywalker/scene/Scene.h"

namespace sky::physics2d {

/// The tileset collision of a tilemap (solid ids and per-tile shapes), provided by the engine
/// (it reads *.tileset.json files). Null or an error = the map's own `solidTiles` only.
using TileCollisionProvider = std::function<Result<TileCollision>(const Tilemap& map)>;

struct Hit2D {
    EntityId entity = kNoEntity;
    Vec2 point;
    Vec2 normal;
    float distance = 0;
};

struct Filter2D {
    std::vector<EntityId> exclude;  // these entities (and the bodies they are part of) are ignored
    bool includeSensors = false;
    uint64_t layerMask = ~0ull;     // bit per collision layer (layerNames2D())
};

struct Event2D {
    enum class Kind { Begin, End, SensorEnter, SensorExit, Impact } kind = Kind::Begin;
    EntityId self = kNoEntity;
    EntityId other = kNoEntity;
    Vec2 point;
    Vec2 normal;        // pointing toward `self`
    float speed = 0;    // approach speed along the normal (units/s)
    float impulse = 0;  // Impact: estimated impulse (N s) = speed x the pair's reduced mass
};

struct Stats2D {
    int bodies = 0, dynamicBodies = 0, kinematicBodies = 0, staticBodies = 0;
    int shapes = 0, sensors = 0, chains = 0, characters = 0, joints = 0;
    int awakeBodies = 0, sleepingBodies = 0, contacts = 0;
};

/// Wireframe of one shape (segment pairs in world space, on the XY plane).
struct DebugShape2D {
    EntityId entity = kNoEntity;
    std::string kind;  // dynamic | kinematic | static | sensor | character | one_way
    bool sleeping = false;
    std::vector<Vec2> lines;
};

struct DebugDraw2D {
    std::vector<DebugShape2D> shapes;
    std::vector<std::pair<Vec2, Vec2>> contacts;  // point, normal (unit)
    std::vector<std::pair<Vec2, Vec2>> joints;    // anchor A, anchor B
};

struct WorldOptions2D {
    /// Entities simulated as dynamic regardless of their components (physics2d_settle).
    std::unordered_set<EntityId> forceDynamic;
    /// Dynamic/kinematic bodies not in forceDynamic are frozen in place (physics2d_settle).
    bool freezeOthers = false;
    /// step() writes dynamic poses and velocities into the scene. Off for what-if simulations.
    bool writeBack = true;
};

class Physics2DWorld {
public:
    explicit Physics2DWorld(TileCollisionProvider tiles = {}, WorldOptions2D options = {});
    ~Physics2DWorld();
    Physics2DWorld(const Physics2DWorld&) = delete;
    Physics2DWorld& operator=(const Physics2DWorld&) = delete;

    /// Reconciles the world with the scene (never writes to the scene). `dt` > 0 moves kinematic
    /// bodies smoothly to their transforms; 0 teleports them (queries, editing).
    void sync(const Scene& scene, float dt = 0.f);
    /// Moves characters, simulates one tick and writes back poses and velocities.
    void step(Scene& scene, float dt);

    /// Contacts, sensor overlaps and impacts from the last steps (sorted, deterministic).
    std::vector<Event2D> drainEvents();
    /// Joints that broke in the last steps (their `enabled` was set to false).
    std::vector<EntityId> drainBrokenJoints();
    /// Problems found while building (bad polygons, unknown layers, missing joint bodies...), deduplicated.
    std::vector<std::string> drainWarnings();

    // --- Bodies ----------------------------------------------------------------------------
    bool addForce(EntityId e, Vec2 force);      // N, over the next step
    bool addImpulse(EntityId e, Vec2 impulse);  // N s, instant
    bool addTorque(EntityId e, float torque);   // N m
    bool setVelocity(EntityId e, Vec2 velocity);
    std::optional<Vec2> velocity(EntityId e) const;
    bool hasBody(EntityId e) const;
    bool isSleeping(EntityId e) const;
    /// True when every dynamic body sleeps (or there are none).
    bool allAsleep() const;
    /// The transform (relative to the scene parent) that puts `e` where its body is.
    std::optional<Transform> localTransform(const Scene& scene, EntityId e) const;

    // --- Characters -----------------------------------------------------------------------------
    /// Desired horizontal input for the next step (-1..1, scaled by moveSpeed).
    bool move(EntityId e, float direction);
    /// Jumps now when grounded (or within coyote time); otherwise remembers the press for jumpBuffer
    /// seconds. Returns whether it jumped now. speed <= 0 = the character's jumpSpeed.
    bool jump(EntityId e, float speed);
    /// Falls through the one-way platform it stands on.
    bool dropThrough(EntityId e);
    std::optional<bool> grounded(EntityId e) const;
    bool hasCharacter(EntityId e) const;

    // --- Queries (colliders; deterministic order) -------------------------------------------------
    std::optional<Hit2D> raycast(Vec2 origin, Vec2 direction, float maxDistance, const Filter2D& filter = {}) const;
    /// Every hit along the ray, one per entity, nearest first.
    std::vector<Hit2D> raycastAll(Vec2 origin, Vec2 direction, float maxDistance, const Filter2D& filter = {}) const;
    /// Entities whose shapes overlap the circle / box, nearest first (ties by id).
    std::vector<EntityId> overlapCircle(Vec2 center, float radius, const Filter2D& filter = {}) const;
    std::vector<EntityId> overlapBox(Vec2 center, Vec2 halfExtents, float angleDegrees, const Filter2D& filter = {}) const;
    /// Entities whose shapes contain the point.
    std::vector<EntityId> pointQuery(Vec2 point, const Filter2D& filter = {}) const;

    // --- Introspection ---------------------------------------------------------------------------
    Stats2D stats() const;
    DebugDraw2D debugDraw() const;
    /// Number of merged collision pieces built for a tilemap collider (loops, boxes, polygons).
    struct TilemapInfo {
        int loops = 0, boxes = 0, polygons = 0, solidCells = 0;
    };
    std::optional<TilemapInfo> tilemapInfo(EntityId e) const;

    struct Impl;  // Box2D state (physics2d/Physics2DInternal.h)

private:
    std::unique_ptr<Impl> impl_;
};

/// Collision layer names in bit order (the same names as the 3D layers).
const std::vector<std::string>& layerNames2D();
/// "all" or comma-separated layer names -> bits; unknown names are reported in `unknown`.
uint64_t parseLayerMask(const std::string& mask, std::string* unknown = nullptr);

}  // namespace sky::physics2d
