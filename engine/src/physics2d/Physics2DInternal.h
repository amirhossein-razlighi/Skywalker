#pragma once
// Internals of Physics2DWorld shared by its source files (world, characters, queries). Box2D is only
// included here and in those files.

#include <box2d/box2d.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "skywalker/ecs/Components.h"
#include "skywalker/physics2d/Physics2DWorld.h"

namespace sky::physics2d {

// Collision category bits beyond the user layers (bit i = layerNames2D()[i]).
constexpr uint64_t kOneWayBit = 1ull << 40;     // one-way platforms (their only category: see makeFilter)
constexpr uint64_t kCharacterBit = 1ull << 41;  // character2d proxy bodies
constexpr float kSkin = 0.02f;                  // character2d: contact distance kept from the ground
constexpr float kLinearSlop = 0.005f;           // Box2D's B2_LINEAR_SLOP (internal to Box2D)

inline b2Vec2 toB2(Vec2 v) { return {v.x, v.y}; }
inline Vec2 fromB2(b2Vec2 v) { return {v.x, v.y}; }
inline float dot2(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float len2(Vec2 a) { return std::sqrt(a.x * a.x + a.y * a.y); }
inline Vec2 rotate2(Vec2 v, float radians) {
    float c = std::cos(radians), s = std::sin(radians);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

/// A 2D pose and scale read from an entity's world matrix.
struct Pose2D {
    Vec2 position;
    float angle = 0.f;  // radians, counter-clockwise
    Vec2 scale{1.f, 1.f};
    float z = 0.f;
};
Pose2D poseOf(const Mat4& world);

/// What a Box2D shape belongs to (shape user data).
struct ShapeTag {
    EntityId entity = kNoEntity;  // the entity with the collider2d / character2d
    EntityId owner = kNoEntity;   // the entity whose body it is part of (events and queries report this one)
    bool oneWay = false;
    bool sensor = false;
    bool character = false;
    uint64_t layer = 1;           // its layer bit (queries filter by it)
    Vec2 up{0.f, 1.f};            // one-way: world direction it blocks from (refreshed when the body moves)
};

/// FNV-1a style change detection over reflected fields.
struct Hash2D {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* data, size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void pod(const T& v) {
        bytes(&v, sizeof(T));
    }
    void str(const std::string& s) {
        pod(s.size());
        bytes(s.data(), s.size());
    }
    void reflected(const void* object, const TypeInfo& type, std::initializer_list<const char*> skip = {});
};

struct BodyEntry {
    EntityId entity = kNoEntity;
    b2BodyId id = b2_nullBodyId;
    b2BodyType type = b2_staticBody;
    bool velocityDriven = false;  // kinematic body moved by its body2d.velocity (else it follows its transform)
    uint64_t signature = 0;
    Pose2D lastPose;              // world pose last set or written back (teleport detection)
    Vec2 lastVelocity;            // body2d.velocity last written back (script writes detection)
    float lastAngular = 0.f;
    int depth = 0;
    std::vector<std::unique_ptr<ShapeTag>> tags;
    std::vector<b2ShapeId> shapes;  // every shape, chain segments included
    int chains = 0;
    std::optional<Physics2DWorld::TilemapInfo> tilemap;
};

struct JointEntry {
    EntityId entity = kNoEntity;
    b2JointId id = b2_nullJointId;
    uint64_t signature = 0;
    std::string kind;
    EntityId follow = kNoEntity;  // target joint: the entity it pulls toward
    float breakForce = 0.f;
};

struct CharEntry {
    EntityId entity = kNoEntity;
    b2BodyId id = b2_nullBodyId;  // kinematic proxy: sensors and dynamic bodies see the character
    std::unique_ptr<ShapeTag> tag;
    Character2D settings;
    uint64_t signature = 0;
    uint64_t category = 0, mask = ~0ull;
    Vec2 position;                // entity origin, world
    float z = 0.f;
    Pose2D lastPose;
    Vec2 velocity;
    Vec2 lastVelocity;            // character2d.velocity last written back
    bool grounded = false;
    Vec2 groundNormal{0.f, 1.f};
    b2ShapeId groundShape = b2_nullShapeId;
    float coyote = 0.f;
    float jumpBuffer = 0.f;
    float pendingJumpSpeed = 0.f;
    float desired = 0.f;          // move2d input for the next step
    float dropTimer = 0.f;        // falling through one-way platforms
};

struct Physics2DWorld::Impl {
    Impl(TileCollisionProvider t, WorldOptions2D o);
    ~Impl();

    TileCollisionProvider tiles;
    WorldOptions2D options;
    b2WorldId world = b2_nullWorldId;
    b2BodyId ground = b2_nullBodyId;  // static anchor for joints to the world

    std::map<EntityId, BodyEntry> bodies;
    std::map<EntityId, JointEntry> joints;
    std::map<EntityId, CharEntry> chars;
    std::map<EntityId, EntityId> ownerOf;  // collider entity -> body owner (queries, excludes)

    Physics2DSettings settings;
    std::vector<Event2D> events;
    std::vector<EntityId> broken;
    std::vector<std::string> warnings;
    std::set<std::string> warned;
    uint64_t syncCount = 0;

    void warn(const std::string& w);

    // --- sync (Physics2DWorld.cpp) ---
    void sync(const Scene& s, float dt);
    void syncBodies(const Scene& s, float dt);
    void syncJoints(const Scene& s);
    void syncCharacters(const Scene& s);
    void destroyBody(BodyEntry& b);
    void destroyJoint(JointEntry& j);
    void destroyCharacter(CharEntry& c);
    bool buildBody(const Scene& s, EntityId e, const Pose2D& pose, BodyEntry& out);
    b2BodyId bodyFor(EntityId e) const;

    // --- step ---
    void step(Scene& s, float dt);
    void collectEvents();
    void checkBrokenJoints(Scene& s);
    void writeBack(Scene& s);

    // --- characters (Character2DController.cpp) ---
    void moveCharacters(float dt);
    void moveCharacter(CharEntry& c, float dt);
    void writeBackCharacters(Scene& s);

    // --- queries (Physics2DQueries.cpp) ---
    b2QueryFilter queryFilter(const Filter2D& f) const;
    bool accept(b2ShapeId shape, const Filter2D& f) const;
};

/// Category and mask of a regular shape on `layer` colliding with `mask`.
b2Filter makeFilter(const std::string& layer, const std::string& mask, bool oneWay, std::string* unknown);

/// Box2D pre-solve: one-way platforms block only from their up side.
bool preSolveOneWay(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context);

}  // namespace sky::physics2d
