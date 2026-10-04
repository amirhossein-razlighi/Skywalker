#include "skywalker/physics/PhysicsWorld.h"

#include "JoltCommon.h"
#include "Shapes.h"
#include "Vehicles.h"

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "skywalker/core/Log.h"
#include "skywalker/render/MeshData.h"

namespace sky::physics {

std::shared_ptr<ShapeCache> makeShapeCache() { return std::make_shared<ShapeCache>(); }

namespace {

constexpr int kRoleSolid = 0;    // an entity's rigid body (owner of its compound shape)
constexpr int kRoleTrigger = 1;  // an entity's trigger (sensor) body

const char* motionName(JPH::EMotionType m) {
    switch (m) {
        case JPH::EMotionType::Static: return "static";
        case JPH::EMotionType::Kinematic: return "kinematic";
        case JPH::EMotionType::Dynamic: return "dynamic";
    }
    return "?";
}

JPH::EAllowedDOFs allowedDofs(const std::string& lockPos, const std::string& lockRot) {
    auto has = [](const std::string& s, char axis) { return s != "none" && s.find(axis) != std::string::npos; };
    using D = JPH::EAllowedDOFs;
    D dofs = D::None;
    if (!has(lockPos, 'x')) dofs |= D::TranslationX;
    if (!has(lockPos, 'y')) dofs |= D::TranslationY;
    if (!has(lockPos, 'z')) dofs |= D::TranslationZ;
    if (!has(lockRot, 'x')) dofs |= D::RotationX;
    if (!has(lockRot, 'y')) dofs |= D::RotationY;
    if (!has(lockRot, 'z')) dofs |= D::RotationZ;
    return dofs;
}

/// Product of local scales up the hierarchy ("lossy scale"): stable while the entity moves.
Vec3 chainScale(const Scene& s, EntityId id) {
    Vec3 out{1.f};
    for (const EntityRecord* r = s.record(id); r; r = r->parent ? s.record(r->parent) : nullptr) {
        if (const Transform* t = s.get<Transform>(r->id)) out = out * t->scale;
    }
    return out;
}

int depthOf(const Scene& s, EntityId id) {
    int d = 0;
    for (const EntityRecord* r = s.record(id); r && r->parent; r = s.record(r->parent)) ++d;
    return d;
}

/// Per-sync view of the scene hierarchy: children lists and memoized world matrices, so a sync
/// is O(entities) instead of re-walking parents and scanning for children per entity.
class SceneIndex {
public:
    explicit SceneIndex(const Scene& s) : s_(s) {
        world_.reserve(s.size());
        for (EntityId e : s.entities()) {
            if (const EntityRecord* r = s.record(e); r && r->parent) kids_[r->parent].push_back(e);
        }
    }
    const std::vector<EntityId>& children(EntityId e) const {
        static const std::vector<EntityId> none;
        auto it = kids_.find(e);
        return it == kids_.end() ? none : it->second;
    }
    const Mat4& world(EntityId e) {
        if (auto it = world_.find(e); it != world_.end()) return it->second;
        const EntityRecord* r = s_.record(e);
        const Transform* t = s_.get<Transform>(e);
        Mat4 local = t ? t->local() : Mat4{};
        Mat4 m = r && r->parent && s_.exists(r->parent) ? world(r->parent) * local : local;
        return world_.emplace(e, m).first->second;
    }

private:
    const Scene& s_;
    std::unordered_map<EntityId, std::vector<EntityId>> kids_;
    std::unordered_map<EntityId, Mat4> world_;
};

struct ContactKey {
    uint32_t b1 = 0, s1 = 0, b2 = 0, s2 = 0;
    auto operator<=>(const ContactKey&) const = default;
};

/// Filters for queries: excluded entities, triggers and a layer mask.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f) : f_(f) {}
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (!f_.includeTriggers && body.IsSensor()) return false;
        EntityId e = static_cast<EntityId>(body.GetUserData());
        return std::find(f_.exclude.begin(), f_.exclude.end(), e) == f_.exclude.end();
    }

private:
    const QueryFilter& f_;
};

class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(uint32_t mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return (mask_ >> userLayer(layer)) & 1u; }

private:
    uint32_t mask_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct PhysicsWorld::Impl final : public JPH::ContactListener {
    struct BodyEntry {
        EntityId entity = kNoEntity;
        int role = kRoleSolid;
        JPH::BodyID id;
        uint64_t signature = 0;
        JPH::EMotionType motion = JPH::EMotionType::Static;
        bool sensor = false;
        int depth = 0;
        Mat4 lastWorld;      // the entity's world matrix when last synced / written back
        Vec3 lastVelocity;   // component values as last synced / written back
        Vec3 lastAngular;
    };
    struct CharEntry {
        EntityId entity = kNoEntity;
        JPH::Ref<JPH::CharacterVirtual> character;
        uint64_t signature = 0;
        CharacterController settings;  // copy used when stepping
        Vec3 desired;                  // horizontal velocity wanted this step (m/s)
        bool jumpRequested = false;
        float jumpSpeed = 0;
        Mat4 lastWorld;
        Vec3 lastVelocity;
    };
    enum class JointType { Fixed, Hinge, Ball, Slider, Distance };
    struct JointEntry {
        EntityId entity = kNoEntity;
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JointType type = JointType::Fixed;
        uint64_t signature = 0;
        JPH::BodyID a, b;
        float breakForce = 0;
    };
    struct AddRecord {
        ContactKey key;
        EntityId e1 = kNoEntity, e2 = kNoEntity;
        bool sensor = false;
        Vec3 point, normal;  // normal points from body 1 to body 2
        float speed = 0;
    };
    struct KeyInfo {
        EntityId e1 = kNoEntity, e2 = kNoEntity;
        bool sensor = false;
        bool retained = false;  // kept while a body sleeps (Jolt drops contacts of sleeping bodies)
    };
    struct PairInfo {
        bool sensor = false;
        Vec3 point, normal;  // normal from first to second entity of the pair
        float speed = 0;
    };

    MeshProvider meshes;
    PathResolver paths;
    WorldOptions options;
    std::shared_ptr<ShapeCache> cache;
    std::vector<std::string> warnings;
    std::set<std::string> warned;

    LayerMatrix matrix;
    BroadPhaseLayers bpLayers;
    ObjectVsBroadPhase objVsBp;
    JPH::PhysicsSystem system;
    std::unique_ptr<JPH::TempAllocator> temp;
    std::unique_ptr<JPH::JobSystemSingleThreaded> localJobs;

    std::map<std::pair<EntityId, int>, BodyEntry> bodies;
    std::map<EntityId, CharEntry> chars;
    std::map<EntityId, JointEntry> joints;
    std::unordered_map<EntityId, std::pair<EntityLink, EntityId>> jointTargets;  // joint -> (target link, entity)
    std::unordered_map<uint32_t, EntityId> bodyEntity;  // BodyID (incl. character inner bodies) -> entity
    std::vector<std::pair<uint32_t, uint32_t>> noCollide;  // sorted body pairs joined without collideConnected

    // Settings currently applied.
    Vec3 gravity{0.f, -9.81f, 0.f};
    int substeps = 1;
    bool allowSleep = true;
    bool enabled = true;
    std::string ignorePairs = "\x01";  // forces the first parse

    // Contacts (callbacks run on job threads; processed after each step in sorted order).
    std::mutex contactMutex;
    std::vector<AddRecord> adds;
    std::vector<ContactKey> removes;
    std::map<ContactKey, KeyInfo> contactKeys;
    std::map<std::pair<EntityId, EntityId>, PairInfo> characterTouches;  // this step
    std::map<std::pair<EntityId, EntityId>, bool> presence;              // pair -> is a trigger pair
    std::vector<ContactEvent> events;
    std::vector<EntityId> brokenJoints;
    float lastDt = 1.f / 60.f;
    uint64_t syncCount = 0;
    std::unique_ptr<VehicleSet> vehicles;  // wheeled vehicles (Vehicles.cpp)

    Impl(MeshProvider m, PathResolver p, WorldOptions o)
        : meshes(std::move(m)), paths(std::move(p)), options(std::move(o)) {
        ensureJoltInitialized();
        cache = options.shapeCache ? options.shapeCache : makeShapeCache();
        const JPH::uint maxBodies = std::max<JPH::uint>(options.maxBodies, 64);
        system.Init(maxBodies, 0, std::max<JPH::uint>(options.maxBodyPairs, 64), std::max<JPH::uint>(options.maxContactConstraints, 64),
                    bpLayers, objVsBp, matrix);
        system.SetContactListener(this);
        temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(8 * 1024 * 1024);
        if (!options.multithreaded) localJobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        VehicleHost host;
        host.system = &system;
        host.bodyOf = [this](EntityId e) { return solidOf(e) ? solidOf(e)->id : JPH::BodyID(); };
        host.entityOf = [this](uint32_t id) {
            auto it = bodyEntity.find(id);
            return it == bodyEntity.end() ? kNoEntity : it->second;
        };
        host.warn = [this](std::string msg) { warn(std::move(msg)); };
        host.meshes = meshes;
        vehicles = std::make_unique<VehicleSet>(std::move(host));
    }

    ~Impl() override {
        auto& bi = system.GetBodyInterfaceNoLock();
        vehicles.reset();  // constraints before the bodies they reference
        for (auto& [k, j] : joints) system.RemoveConstraint(j.constraint);
        joints.clear();
        chars.clear();  // CharacterVirtual removes its inner body
        for (auto& [k, b] : bodies) {
            bi.RemoveBody(b.id);
            bi.DestroyBody(b.id);
        }
        bodies.clear();
        system.SetContactListener(nullptr);
    }

    void warn(std::string msg) {
        if (warned.insert(msg).second) {
            log::warn("physics", msg);
            warnings.push_back(std::move(msg));
        }
    }

    ShapeContext shapeContext() {
        ShapeContext ctx;
        ctx.meshes = meshes;
        ctx.paths = paths;
        ctx.cache = cache.get();
        ctx.warnings = &warnings;
        return ctx;
    }

    JPH::BodyInterface& bi() { return system.GetBodyInterfaceNoLock(); }
    const JPH::BodyLockInterface& locks() const { return system.GetBodyLockInterfaceNoLock(); }

    const BodyEntry* solidOf(EntityId e) const {
        auto it = bodies.find({e, kRoleSolid});
        return it == bodies.end() ? nullptr : &it->second;
    }

    // --- Contact listener (job threads) ------------------------------------------------------
    static const SurfaceMaterial* surface(const JPH::Body& b, const JPH::SubShapeID& id) {
        const JPH::PhysicsMaterial* m = b.GetShape()->GetMaterial(id);
        if (!m || m == JPH::PhysicsMaterial::sDefault.GetPtr()) return nullptr;
        return static_cast<const SurfaceMaterial*>(m);  // every material Skywalker creates is a SurfaceMaterial
    }

    static void combine(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings& io) {
        const SurfaceMaterial* m1 = surface(b1, m.mSubShapeID1);
        const SurfaceMaterial* m2 = surface(b2, m.mSubShapeID2);
        float f1 = m1 ? m1->friction : b1.GetFriction(), f2 = m2 ? m2->friction : b2.GetFriction();
        float r1 = m1 ? m1->restitution : b1.GetRestitution(), r2 = m2 ? m2->restitution : b2.GetRestitution();
        io.mCombinedFriction = std::sqrt(std::max(f1, 0.f) * std::max(f2, 0.f));
        io.mCombinedRestitution = std::max(r1, r2);
    }

    JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        // Triggers ignore static level geometry (they detect things that move).
        if ((b1.IsSensor() && b2.IsStatic()) || (b2.IsSensor() && b1.IsStatic())) {
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        if (!noCollide.empty()) {
            uint32_t x = b1.GetID().GetIndexAndSequenceNumber(), y = b2.GetID().GetIndexAndSequenceNumber();
            if (x > y) std::swap(x, y);
            if (std::binary_search(noCollide.begin(), noCollide.end(), std::make_pair(x, y))) {
                return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
            }
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings& io) override {
        combine(b1, b2, m, io);
        AddRecord r;
        r.key = {b1.GetID().GetIndexAndSequenceNumber(), m.mSubShapeID1.GetValue(), b2.GetID().GetIndexAndSequenceNumber(),
                 m.mSubShapeID2.GetValue()};
        r.e1 = static_cast<EntityId>(b1.GetUserData());
        r.e2 = static_cast<EntityId>(b2.GetUserData());
        r.sensor = b1.IsSensor() || b2.IsSensor();
        JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        r.point = fromJolt(JPH::Vec3(p));
        r.normal = fromJolt(m.mWorldSpaceNormal);
        JPH::Vec3 v1 = b1.GetPointVelocity(p), v2 = b2.GetPointVelocity(p);
        r.speed = std::max(0.f, (v1 - v2).Dot(m.mWorldSpaceNormal));
        std::lock_guard lock(contactMutex);
        adds.push_back(r);
    }

    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings& io) override {
        combine(b1, b2, m, io);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        ContactKey k{pair.GetBody1ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID1().GetValue(),
                     pair.GetBody2ID().GetIndexAndSequenceNumber(), pair.GetSubShapeID2().GetValue()};
        std::lock_guard lock(contactMutex);
        removes.push_back(k);
    }

    // --- Settings ----------------------------------------------------------------------------
    void applySettings(const Scene& scene) {
        PhysicsSettings settings;
        for (EntityId e : scene.entities()) {
            if (const PhysicsSettings* s = scene.get<PhysicsSettings>(e); s && scene.isActive(e)) {
                settings = *s;
                break;
            }
        }
        if (!(settings.gravity == gravity)) {
            gravity = settings.gravity;
            system.SetGravity(toJolt(gravity));
        }
        substeps = std::clamp(settings.substeps, 1, 8);
        allowSleep = settings.allowSleep;
        enabled = settings.enabled;
        if (settings.ignorePairs != ignorePairs) {
            ignorePairs = settings.ignorePairs;
            for (auto& problem : matrix.setIgnoredPairs(ignorePairs)) warn(problem);
        }
    }

    // --- Desired state from the scene ---------------------------------------------------------
    struct Desired {
        EntityId entity = kNoEntity;
        int role = kRoleSolid;
        JPH::EMotionType motion = JPH::EMotionType::Static;
        bool sensor = false;
        JPH::ObjectLayer layer = kStatic;
        const RigidBody* body = nullptr;
        std::vector<ShapePart> parts;
        uint64_t signature = 0;
        Mat4 world;
    };

    static bool active(const Scene& s, EntityId e) { return s.isActive(e); }

    static bool hasBodyAncestor(const Scene& s, EntityId e) {
        for (const EntityRecord* r = s.record(e); r && r->parent; r = s.record(r->parent)) {
            if (s.get<RigidBody>(r->parent) || s.get<CharacterController>(r->parent)) return true;
        }
        return false;
    }

    /// Solid colliders below `owner` (stopping at entities that own their own body).
    void collectChildParts(const Scene& s, const SceneIndex& index, EntityId owner, EntityId at, const Mat4& rel,
                           Vec3 ownerScale, std::vector<ShapePart>& parts, Aabb& meshBounds, bool& anyMesh,
                           const RigidBody* body) {
        for (EntityId c : index.children(at)) {
            if (!s.get<Transform>(c) || !s.record(c)->enabled) continue;
            if (s.get<RigidBody>(c) || s.get<CharacterController>(c)) continue;
            if (isWheelVisual(s, owner, c)) continue;  // a vehicle's wheels ride on the suspension, not the chassis shape
            Mat4 m = rel * s.get<Transform>(c)->local();
            if (const MeshRenderer* mr = s.get<MeshRenderer>(c)) {
                // Imported meshes: the loaded data knows its bounds even before anything rendered it.
                const MeshData* md = meshes && mr->mesh.rfind("asset:", 0) == 0 ? meshes(mr->mesh) : nullptr;
                Aabb b = (md ? md->bounds : s.localBounds(c)).transformed(m);
                meshBounds.min = vmin(meshBounds.min, b.min);
                meshBounds.max = vmax(meshBounds.max, b.max);
                anyMesh = anyMesh || mr;
            }
            const Collider* col = s.get<Collider>(c);
            if (col && !col->isTrigger) {
                ShapePart p;
                p.entity = c;
                p.collider = col;
                p.mesh = s.get<MeshRenderer>(c);
                p.local = Mat4::scale(ownerScale) * m;
                p.friction = col->friction >= 0 ? col->friction : (body ? body->friction : 0.5f);
                p.restitution = col->restitution >= 0 ? col->restitution : (body ? body->restitution : 0.f);
                parts.push_back(p);
            }
            collectChildParts(s, index, owner, c, m, ownerScale, parts, meshBounds, anyMesh, body);
        }
    }

    uint64_t signatureOf(const Scene& s, const Desired& d) {
        Hasher h;
        h.pod(d.role);
        h.pod(d.motion);
        h.pod(d.sensor);
        h.pod(d.layer);
        h.pod(allowSleep);
        if (d.body) h.reflected(d.body, RigidBody::type(), {"velocity", "angularVelocity"});
        for (const ShapePart& p : d.parts) {
            h.pod(p.entity);
            h.mat(p.local);
            h.pod(p.friction);
            h.pod(p.restitution);
            if (p.collider) h.reflected(p.collider, Collider::type());
            if (p.mesh) {
                h.str(p.mesh->mesh);
                // Imported meshes can be hot-reloaded: include the loaded data's identity.
                const MeshData* md = meshes && p.mesh->mesh.rfind("asset:", 0) == 0 ? meshes(p.mesh->mesh) : nullptr;
                h.pod(md);
                h.pod(md ? md->vertexCount() : 0);
            }
            h.pod(p.hasFallbackBounds);
            if (p.hasFallbackBounds) h.pod(p.fallbackBounds);
        }
        if (const Vehicle* v = s.get<Vehicle>(d.entity)) {  // the chassis mass and center of mass
            h.pod(v->mass);
            h.pod(v->centerOfMass);
        }
        return h.h;
    }

    /// What the scene wants simulated. Static geometry that already has a body only gets a full
    /// (signature) check every 8th sync, staggered by entity; teleports are still seen every tick.
    std::vector<Desired> desiredBodies(const Scene& s, SceneIndex& index, bool fullCheck) {
        std::vector<Desired> out;
        out.reserve(bodies.size() + 8);
        for (EntityId e : s.entities()) {
            if (!active(s, e)) continue;
            if (s.get<CharacterController>(e)) continue;  // characters are not rigid bodies
            const RigidBody* rb = s.get<RigidBody>(e);
            const Collider* col = s.get<Collider>(e);
            bool forced = options.forceDynamic.count(e) != 0;
            const Mat4& world = index.world(e);
            if (!fullCheck && !forced && (rb || col) && (!rb || rb->motion == "static") && !(col && col->isTrigger) &&
                (syncCount + e) % 8 != 0) {
                auto it = bodies.find({e, kRoleSolid});
                if (it != bodies.end() && it->second.motion == JPH::EMotionType::Static && !hasBodyAncestor(s, e)) {
                    Desired d;  // unchanged as far as this tick is concerned
                    d.entity = e;
                    d.role = kRoleSolid;
                    d.motion = JPH::EMotionType::Static;
                    d.body = rb;
                    d.world = world;
                    d.signature = it->second.signature;
                    out.push_back(std::move(d));
                    continue;
                }
            }
            Vec3 scale = chainScale(s, e);

            // Trigger: its own kinematic sensor body (always awake so it sees sleeping bodies).
            if (col && col->isTrigger) {
                Desired d;
                d.entity = e;
                d.role = kRoleTrigger;
                d.motion = JPH::EMotionType::Kinematic;
                d.sensor = true;
                d.layer = objectLayer(rb ? layerIndex(rb->layer, kTrigger) : JPH::ObjectLayer{kTrigger}, false, true);
                ShapePart p;
                p.entity = e;
                p.collider = col;
                p.mesh = s.get<MeshRenderer>(e);
                p.local = Mat4::scale(scale);
                d.parts.push_back(p);
                d.world = world;
                d.signature = signatureOf(s, d);
                out.push_back(std::move(d));
            }

            bool owner = (rb || forced || (col && !col->isTrigger)) && !hasBodyAncestor(s, e);
            if (!owner) continue;
            Desired d;
            d.entity = e;
            d.role = kRoleSolid;
            d.body = rb;
            d.world = world;
            if (rb) {
                d.motion = rb->motion == "dynamic"     ? JPH::EMotionType::Dynamic
                           : rb->motion == "kinematic" ? JPH::EMotionType::Kinematic
                                                       : JPH::EMotionType::Static;
            }
            if (forced) d.motion = JPH::EMotionType::Dynamic;
            else if (options.freezeOthers) d.motion = JPH::EMotionType::Static;
            if (d.motion == JPH::EMotionType::Dynamic && rb && allowedDofs(rb->lockPosition, rb->lockRotation) == JPH::EAllowedDOFs::None) {
                d.motion = JPH::EMotionType::Kinematic;  // fully locked: Jolt needs at least one free axis
            }
            JPH::ObjectLayer user = rb ? layerIndex(rb->layer, kDefault) : JPH::ObjectLayer{kStatic};
            if (!rb && d.motion == JPH::EMotionType::Static) user = kStatic;
            if (rb && rb->layer == "default" && d.motion == JPH::EMotionType::Static) user = kStatic;
            d.layer = objectLayer(user, d.motion == JPH::EMotionType::Static);

            if (col && !col->isTrigger) {
                ShapePart p;
                p.entity = e;
                p.collider = col;
                p.mesh = s.get<MeshRenderer>(e);
                p.local = Mat4::scale(scale);
                p.friction = col->friction >= 0 ? col->friction : (rb ? rb->friction : 0.5f);
                p.restitution = col->restitution >= 0 ? col->restitution : (rb ? rb->restitution : 0.f);
                d.parts.push_back(p);
            }
            Aabb meshBounds{Vec3(1e30f), Vec3(-1e30f)};
            bool anyMesh = false;
            collectChildParts(s, index, e, e, Mat4{}, scale, d.parts, meshBounds, anyMesh, rb);
            if (d.parts.empty()) {
                if (col && col->isTrigger) continue;  // only a trigger: no solid body
                ShapePart p;  // implicit "auto" collider
                p.entity = e;
                p.mesh = s.get<MeshRenderer>(e);
                p.local = Mat4::scale(scale);
                p.friction = rb ? rb->friction : 0.5f;
                p.restitution = rb ? rb->restitution : 0.f;
                if (!p.mesh && anyMesh) {
                    p.hasFallbackBounds = true;
                    p.fallbackBounds = meshBounds;
                }
                d.parts.push_back(p);
            }
            d.signature = signatureOf(s, d);
            out.push_back(std::move(d));
        }
        return out;
    }

    // --- Bodies --------------------------------------------------------------------------------
    void removeBody(std::map<std::pair<EntityId, int>, BodyEntry>::iterator it) {
        uint32_t idv = it->second.id.GetIndexAndSequenceNumber();
        bi().RemoveBody(it->second.id);
        bi().DestroyBody(it->second.id);
        bodyEntity.erase(idv);
        purgeContacts(idv);
        bodies.erase(it);
    }

    void purgeContacts(uint32_t idv) {
        for (auto it = contactKeys.begin(); it != contactKeys.end();) {
            if (it->first.b1 == idv || it->first.b2 == idv) it = contactKeys.erase(it);
            else ++it;
        }
    }

    bool createBody(const Scene& s, const Desired& d) {
        bool dynamic = d.motion == JPH::EMotionType::Dynamic;
        std::vector<std::string> shapeWarnings;
        ShapeContext ctx = shapeContext();
        ctx.warnings = &shapeWarnings;
        JPH::RefConst<JPH::Shape> shape = buildBodyShape(ctx, d.parts, dynamic);
        for (auto& w : shapeWarnings) warn(std::move(w));
        if (!shape) {
            warn("entity #" + std::to_string(d.entity) + ": no collision shape could be built");
            return false;
        }
        const Vehicle* vehicle = dynamic && d.role == kRoleSolid ? s.get<Vehicle>(d.entity) : nullptr;
        if (vehicle) shape = vehicleChassisShape(shape, *vehicle);
        Decomposed pose = decompose(d.world);
        JPH::BodyCreationSettings bcs(shape, JPH::RVec3(toJolt(pose.translation)), pose.rotation, d.motion, d.layer);
        bcs.mUserData = d.entity;
        bcs.mIsSensor = d.sensor;
        bcs.mCollideKinematicVsNonDynamic = d.sensor;
        bcs.mAllowSleeping = allowSleep && !d.sensor;
        bcs.mFriction = d.body ? d.body->friction : 0.5f;
        bcs.mRestitution = d.body ? d.body->restitution : 0.f;
        if (const RigidBody* rb = d.body) {
            bcs.mLinearDamping = rb->linearDamping;
            bcs.mAngularDamping = rb->angularDamping;
            bcs.mGravityFactor = rb->gravityScale;
            bcs.mMotionQuality = rb->ccd ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
            if (dynamic) {
                bcs.mAllowedDOFs = allowedDofs(rb->lockPosition, rb->lockRotation);
                bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bcs.mMassPropertiesOverride.mMass = std::max(vehicle ? vehicle->mass : rb->mass, 0.001f);
                bcs.mLinearVelocity = toJolt(rb->velocity);
                bcs.mAngularVelocity = toJolt(rb->angularVelocity * (kPi / 180.f));
            }
        }
        bcs.mMaxLinearVelocity = 500.f;
        JPH::Body* body = bi().CreateBody(bcs);
        if (!body) {
            warn("physics body limit reached (" + std::to_string(options.maxBodies) + "); entity #" + std::to_string(d.entity) +
                 " has no body");
            return false;
        }
        bool awake = d.motion != JPH::EMotionType::Static && (!d.body || d.body->startAwake || d.sensor || !dynamic);
        bi().AddBody(body->GetID(), awake ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        BodyEntry e;
        e.entity = d.entity;
        e.role = d.role;
        e.id = body->GetID();
        e.signature = d.signature;
        e.motion = d.motion;
        e.sensor = d.sensor;
        e.depth = depthOf(s, d.entity);
        e.lastWorld = d.world;
        if (d.body) {
            e.lastVelocity = d.body->velocity;
            e.lastAngular = d.body->angularVelocity;
        }
        bodyEntity[e.id.GetIndexAndSequenceNumber()] = d.entity;
        bodies[{d.entity, d.role}] = e;
        return true;
    }

    void updateBody(BodyEntry& e, const Desired& d, float dt) {
        bool moved = !(std::equal(std::begin(d.world.m), std::end(d.world.m), std::begin(e.lastWorld.m)));
        if (e.motion == JPH::EMotionType::Kinematic) {
            Decomposed pose = decompose(d.world);
            // Follow the transform with a velocity (so riders and pushed bodies react) unless it
            // jumped: a teleport must not fling everything it touches.
            bool teleport = distance(d.world.translation(), e.lastWorld.translation()) > 2.f;
            if (dt > 0 && !teleport) {
                bi().MoveKinematic(e.id, JPH::RVec3(toJolt(pose.translation)), pose.rotation, dt);
            } else if (moved) {
                bi().SetPositionAndRotation(e.id, JPH::RVec3(toJolt(pose.translation)), pose.rotation, JPH::EActivation::DontActivate);
                bi().SetLinearAndAngularVelocity(e.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            }
        } else if (moved) {
            Decomposed pose = decompose(d.world);
            bool dynamic = e.motion == JPH::EMotionType::Dynamic;
            bi().SetPositionAndRotation(e.id, JPH::RVec3(toJolt(pose.translation)), pose.rotation,
                                        dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        e.lastWorld = d.world;
        if (e.motion == JPH::EMotionType::Dynamic && d.body) {
            if (!(d.body->velocity == e.lastVelocity)) {
                bi().SetLinearVelocity(e.id, toJolt(d.body->velocity));
                e.lastVelocity = d.body->velocity;
            }
            if (!(d.body->angularVelocity == e.lastAngular)) {
                bi().SetAngularVelocity(e.id, toJolt(d.body->angularVelocity * (kPi / 180.f)));
                e.lastAngular = d.body->angularVelocity;
            }
        }
    }

    // --- Characters ------------------------------------------------------------------------------
    static Vec3 feetFromEntity(const Mat4& world, const CharacterController& c) {
        return world.translation() + c.offset - Vec3{0.f, c.height * 0.5f, 0.f};
    }

    void createCharacter(const Scene& s, EntityId e, const CharacterController& c, uint64_t sig) {
        JPH::Ref<JPH::CharacterVirtualSettings> cs = new JPH::CharacterVirtualSettings();
        float radius = std::clamp(c.radius, 0.05f, c.height * 0.5f);
        cs->mShape = characterShape(c.height, radius);
        cs->mMaxSlopeAngle = radians(std::clamp(c.maxSlope, 0.f, 89.f));
        cs->mMaxStrength = c.pushStrength;
        cs->mMass = c.mass;
        cs->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);  // only the lower cap supports
        cs->mInnerBodyShape = cs->mShape;
        cs->mInnerBodyLayer = objectLayer(layerIndex(c.layer, kPlayer), false);
        cs->mEnhancedInternalEdgeRemoval = true;
        Mat4 world = s.worldMatrix(e);
        Vec3 feet = feetFromEntity(world, c);
        CharEntry entry;
        entry.entity = e;
        entry.character = new JPH::CharacterVirtual(cs, JPH::RVec3(toJolt(feet)), JPH::Quat::sIdentity(), e, &system);
        entry.character->SetLinearVelocity(toJolt(c.velocity));
        entry.signature = sig;
        entry.settings = c;
        entry.lastWorld = world;
        entry.lastVelocity = c.velocity;
        if (!entry.character->GetInnerBodyID().IsInvalid()) {
            bodyEntity[entry.character->GetInnerBodyID().GetIndexAndSequenceNumber()] = e;
        }
        chars[e] = std::move(entry);
    }

    void removeCharacter(std::map<EntityId, CharEntry>::iterator it) {
        JPH::BodyID inner = it->second.character->GetInnerBodyID();
        if (!inner.IsInvalid()) {
            bodyEntity.erase(inner.GetIndexAndSequenceNumber());
            purgeContacts(inner.GetIndexAndSequenceNumber());
        }
        chars.erase(it);
    }

    void syncCharacters(const Scene& s) {
        std::set<EntityId> wanted;
        for (EntityId e : s.entities()) {
            const CharacterController* c = s.get<CharacterController>(e);
            if (!c || !active(s, e)) continue;
            wanted.insert(e);
            Hasher h;
            h.reflected(c, CharacterController::type(), {"velocity"});
            auto it = chars.find(e);
            if (it != chars.end() && it->second.signature != h.h) {
                removeCharacter(it);
                it = chars.end();
            }
            if (it == chars.end()) {
                createCharacter(s, e, *c, h.h);
                continue;
            }
            CharEntry& ce = it->second;
            Mat4 world = s.worldMatrix(e);
            if (!std::equal(std::begin(world.m), std::end(world.m), std::begin(ce.lastWorld.m))) {
                // Something else moved the entity (script, agent, teleporter): follow it.
                if (!(world.translation() == ce.lastWorld.translation())) {
                    ce.character->SetPosition(JPH::RVec3(toJolt(feetFromEntity(world, *c))));
                }
                ce.lastWorld = world;
            }
            if (!(c->velocity == ce.lastVelocity)) {  // knockback / launch pads
                ce.character->SetLinearVelocity(toJolt(c->velocity));
                ce.lastVelocity = c->velocity;
            }
        }
        for (auto it = chars.begin(); it != chars.end();) {
            if (!wanted.count(it->first)) {
                auto next = std::next(it);
                removeCharacter(it);
                it = next;
            } else {
                ++it;
            }
        }
    }

    void stepCharacters(float dt) {
        for (auto& [e, ce] : chars) {
            const CharacterController& c = ce.settings;
            JPH::CharacterVirtual& ch = *ce.character;
            ch.UpdateGroundVelocity();
            bool onGround = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            JPH::Vec3 current = ch.GetLinearVelocity();
            JPH::Vec3 ground = ch.GetGroundVelocity();
            JPH::Vec3 desired = toJolt(ce.desired);
            desired.SetY(0);
            JPH::Vec3 horizontal;
            JPH::Vec3 currentH(current.GetX(), 0, current.GetZ());
            if (onGround) {
                horizontal = desired;
            } else {
                horizontal = currentH + (desired - currentH) * std::clamp(c.airControl, 0.f, 1.f);
            }
            float vy = current.GetY();
            bool movingAway = current.GetY() - ground.GetY() > 0.1f;
            if (onGround && !movingAway) {
                vy = ground.GetY();
                if (ce.jumpRequested) vy += ce.jumpSpeed;
            }
            vy -= c.gravity * dt;
            JPH::Vec3 v = horizontal + JPH::Vec3(0, vy, 0);
            if (onGround && !movingAway) v += JPH::Vec3(ground.GetX(), 0, ground.GetZ());
            ch.SetLinearVelocity(v);

            JPH::CharacterVirtual::ExtendedUpdateSettings us;
            float step = std::max(c.stepHeight, 0.f);
            us.mStickToFloorStepDown = JPH::Vec3(0, -std::max(step, 0.05f), 0);
            us.mWalkStairsStepUp = JPH::Vec3(0, step, 0);
            JPH::ObjectLayer layer = objectLayer(layerIndex(c.layer, kPlayer), false);
            ch.ExtendedUpdate(dt, JPH::Vec3(0, -c.gravity, 0), us, system.GetDefaultBroadPhaseLayerFilter(layer),
                              system.GetDefaultLayerFilter(layer), {}, {}, *temp);
            ce.desired = {};
            ce.jumpRequested = false;

            // Contacts with the character (it is not part of Jolt's contact listener). Impact speed
            // uses the velocity before the move resolved the collision.
            const JPH::Vec3 vel = v;
            for (const auto& contact : ch.GetActiveContacts()) {
                if (!contact.mHadCollision || contact.mIsSensorB || contact.mWasDiscarded) continue;
                EntityId other = kNoEntity;
                if (!contact.mBodyB.IsInvalid()) {
                    auto it = bodyEntity.find(contact.mBodyB.GetIndexAndSequenceNumber());
                    if (it != bodyEntity.end()) other = it->second;
                } else if (!contact.mCharacterIDB.IsInvalid()) {
                    for (const auto& [oe, oc] : chars) {
                        if (oc.character->GetID() == contact.mCharacterIDB) other = oe;
                    }
                }
                if (other == kNoEntity || other == e) continue;
                auto key = std::minmax(e, other);
                if (characterTouches.count(key)) continue;
                PairInfo info;
                info.point = fromJolt(JPH::Vec3(contact.mPosition));
                // mContactNormal points toward the character; store it from first to second of the pair.
                Vec3 towardChar = fromJolt(contact.mContactNormal);
                info.normal = key.first == e ? -towardChar : towardChar;
                info.speed = std::max(0.f, -(vel - contact.mLinearVelocity).Dot(contact.mContactNormal));
                characterTouches[key] = info;
            }
        }
    }

    void writeBackCharacters(Scene& s) {
        for (auto& [e, ce] : chars) {
            Transform* t = s.get<Transform>(e);
            CharacterController* c = s.get<CharacterController>(e);
            if (!t || !c) continue;
            Vec3 feet = fromJolt(JPH::Vec3(ce.character->GetPosition()));
            Vec3 worldPos = feet + Vec3{0.f, c->height * 0.5f, 0.f} - c->offset;
            const EntityRecord* r = s.record(e);
            Mat4 parent = r->parent ? s.worldMatrix(r->parent) : Mat4{};
            t->position = r->parent ? parent.inverse().transformPoint(worldPos) : worldPos;
            Vec3 v = fromJolt(ce.character->GetLinearVelocity());
            // Face the walking direction (yaw only; -Z is forward).
            Vec3 horizontal{v.x, 0.f, v.z};
            if (c->turnSpeed > 0 && length(horizontal) > 0.2f) {
                float target = degrees(std::atan2(-horizontal.x, -horizontal.z));
                float delta = std::remainder(target - t->rotation.y, 360.f);
                float maxTurn = c->turnSpeed * lastDt;
                t->rotation.y = std::remainder(t->rotation.y + std::clamp(delta, -maxTurn, maxTurn), 360.f);
            }
            c->velocity = v;
            ce.lastVelocity = v;
            ce.lastWorld = s.worldMatrix(e);
        }
        if (!chars.empty()) s.markDirty();
    }

    // --- Joints ------------------------------------------------------------------------------------
    /// The body that moves `e`: its own, or the one of the ancestor whose compound it is part of.
    const BodyEntry* bodyFor(const Scene& s, EntityId e) const {
        for (EntityId cur = e; cur != kNoEntity;) {
            if (const BodyEntry* b = solidOf(cur)) return b;
            const EntityRecord* r = s.record(cur);
            cur = r ? r->parent : kNoEntity;
        }
        return nullptr;
    }

    void removeJoint(std::map<EntityId, JointEntry>::iterator it) {
        system.RemoveConstraint(it->second.constraint);
        joints.erase(it);
    }

    void syncJoints(const Scene& s) {
        std::set<EntityId> wanted;
        bool pairsChanged = false;
        for (EntityId e : s.entities()) {
            const Joint* j = s.get<Joint>(e);
            if (!j || !j->enabled || !active(s, e)) continue;
            const BodyEntry* a = bodyFor(s, e);
            if (!a) {
                warn("joint on entity #" + std::to_string(e) + ": the entity has no body (add a body component)");
                continue;
            }
            JPH::BodyID bId = JPH::BodyID();
            EntityId target = kNoEntity;
            if (!j->target.empty()) {
                // Links bound to an id resolve directly; name-only links scan the scene, so cache them while the
                // name still matches.
                auto& cached = jointTargets[e];
                const EntityRecord* cr = cached.second ? s.record(cached.second) : nullptr;
                if (cached.first != j->target || !cr || (!j->target.id && cr->name != j->target.name)) {
                    cached = {j->target, s.resolve(j->target, e)};
                }
                target = cached.second;
                const std::string label = j->target.name.empty() ? formatEntityRef(j->target.id) : j->target.name;
                if (target == kNoEntity) {
                    warn("joint on entity #" + std::to_string(e) + ": target \"" + label +
                         "\" not found (entity_refs lists broken links)");
                    continue;
                }
                const BodyEntry* b = bodyFor(s, target);
                if (!b) {
                    warn("joint on entity #" + std::to_string(e) + ": target \"" + label + "\" has no body");
                    continue;
                }
                if (b->id == a->id) {
                    warn("joint on entity #" + std::to_string(e) + ": target is part of the same body");
                    continue;
                }
                bId = b->id;
            }
            Hasher h;
            h.reflected(j, Joint::type());
            h.pod(a->id.GetIndexAndSequenceNumber());
            h.pod(bId.GetIndexAndSequenceNumber());
            wanted.insert(e);
            auto it = joints.find(e);
            if (it != joints.end() && it->second.signature == h.h) continue;
            if (it != joints.end()) removeJoint(it);
            pairsChanged = true;
            createJoint(s, e, *j, a->id, bId, target, h.h);
        }
        for (auto it = joints.begin(); it != joints.end();) {
            if (!wanted.count(it->first)) {
                auto next = std::next(it);
                removeJoint(it);
                it = next;
                pairsChanged = true;
            } else {
                ++it;
            }
        }
        if (pairsChanged) {
            noCollide.clear();
            for (EntityId e : wanted) {
                auto it = joints.find(e);
                const Joint* j = s.get<Joint>(e);
                if (it == joints.end() || !j || j->collideConnected || it->second.b.IsInvalid()) continue;
                uint32_t x = it->second.a.GetIndexAndSequenceNumber(), y = it->second.b.GetIndexAndSequenceNumber();
                noCollide.emplace_back(std::min(x, y), std::max(x, y));
            }
            std::sort(noCollide.begin(), noCollide.end());
        }
    }

    void createJoint(const Scene& s, EntityId e, const Joint& j, JPH::BodyID aId, JPH::BodyID bId, EntityId target, uint64_t sig) {
        Mat4 world = s.worldMatrix(e);
        JPH::RVec3 anchor(toJolt(world.transformPoint(j.anchor)));
        Vec3 axisV = normalize(world.transformDir(j.axis));
        if (length(axisV) < 0.5f) axisV = {0, 1, 0};
        JPH::Vec3 axis = toJolt(axisV);
        JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
        bool limited = j.limitMin < j.limitMax;
        JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
        JointType type = JointType::Fixed;
        if (j.kind == "hinge") {
            auto* hs = new JPH::HingeConstraintSettings();
            hs->mPoint1 = hs->mPoint2 = anchor;
            hs->mHingeAxis1 = hs->mHingeAxis2 = axis;
            hs->mNormalAxis1 = hs->mNormalAxis2 = normal;
            if (limited) {
                hs->mLimitsMin = std::clamp(radians(j.limitMin), -JPH::JPH_PI, 0.f);
                hs->mLimitsMax = std::clamp(radians(j.limitMax), 0.f, JPH::JPH_PI);
            }
            if (j.motorForce > 0) hs->mMotorSettings.SetTorqueLimit(j.motorForce);
            settings = hs;
            type = JointType::Hinge;
        } else if (j.kind == "ball") {
            auto* ps = new JPH::PointConstraintSettings();
            ps->mPoint1 = ps->mPoint2 = anchor;
            settings = ps;
            type = JointType::Ball;
        } else if (j.kind == "slider") {
            auto* ss = new JPH::SliderConstraintSettings();
            ss->mPoint1 = ss->mPoint2 = anchor;
            ss->SetSliderAxis(axis);
            if (limited) {
                ss->mLimitsMin = std::min(j.limitMin, 0.f);
                ss->mLimitsMax = std::max(j.limitMax, 0.f);
            }
            if (j.motorForce > 0) ss->mMotorSettings.SetForceLimit(j.motorForce);
            settings = ss;
            type = JointType::Slider;
        } else if (j.kind == "distance" || j.kind == "spring") {
            auto* ds = new JPH::DistanceConstraintSettings();
            ds->mPoint1 = anchor;
            Vec3 other = target ? s.worldMatrix(target).transformPoint(j.connectedAnchor) : j.connectedAnchor;
            ds->mPoint2 = JPH::RVec3(toJolt(other));
            float rest = distance(fromJolt(JPH::Vec3(anchor)), other);
            if (limited) {
                ds->mMinDistance = std::max(j.limitMin, 0.f);
                ds->mMaxDistance = std::max(j.limitMax, ds->mMinDistance);
            } else {
                ds->mMinDistance = ds->mMaxDistance = rest;
            }
            if (j.kind == "spring") {
                ds->mLimitsSpringSettings.mFrequency = std::max(j.stiffness, 0.f);
                ds->mLimitsSpringSettings.mDamping = std::max(j.damping, 0.f);
            }
            settings = ds;
            type = JointType::Distance;
        } else {
            auto* fs = new JPH::FixedConstraintSettings();
            fs->mAutoDetectPoint = true;
            settings = fs;
            type = JointType::Fixed;
        }
        settings->mConstraintPriority = static_cast<JPH::uint32>(e);  // unique => deterministic order
        JPH::TwoBodyConstraint* c = nullptr;
        if (bId.IsInvalid()) {
            JPH::BodyLockWrite la(system.GetBodyLockInterfaceNoLock(), aId);
            if (la.Succeeded()) c = settings->Create(la.GetBody(), JPH::Body::sFixedToWorld);
        } else {
            const JPH::BodyID ids[2] = {aId, bId};  // the lock keeps a pointer to this array
            JPH::BodyLockMultiWrite lock(system.GetBodyLockInterfaceNoLock(), ids, 2);
            JPH::Body* ba = lock.GetBody(0);
            JPH::Body* bb = lock.GetBody(1);
            if (ba && bb) c = settings->Create(*ba, *bb);
        }
        if (!c) {
            warn("joint on entity #" + std::to_string(e) + " could not be created");
            return;
        }
        if (type == JointType::Hinge && j.motorForce > 0) {
            auto* hc = static_cast<JPH::HingeConstraint*>(c);
            hc->SetMotorState(JPH::EMotorState::Velocity);
            hc->SetTargetAngularVelocity(radians(j.motorSpeed));
        } else if (type == JointType::Slider && j.motorForce > 0) {
            auto* sc = static_cast<JPH::SliderConstraint*>(c);
            sc->SetMotorState(JPH::EMotorState::Velocity);
            sc->SetTargetVelocity(j.motorSpeed);
        }
        c->SetUserData(e);
        system.AddConstraint(c);
        bi().ActivateBody(aId);
        if (!bId.IsInvalid()) bi().ActivateBody(bId);
        JointEntry entry;
        entry.entity = e;
        entry.constraint = c;
        entry.type = type;
        entry.signature = sig;
        entry.a = aId;
        entry.b = bId;
        entry.breakForce = j.breakForce;
        joints[e] = std::move(entry);
    }

    void checkBreaking(Scene* s, float dt) {
        float subDt = dt / static_cast<float>(substeps);
        for (auto& [e, j] : joints) {
            if (j.breakForce <= 0 || !j.constraint->GetEnabled()) continue;
            float lambda = 0;
            JPH::TwoBodyConstraint* c = j.constraint.GetPtr();
            switch (j.type) {
                case JointType::Fixed: lambda = static_cast<JPH::FixedConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
                case JointType::Hinge: lambda = static_cast<JPH::HingeConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
                case JointType::Ball: lambda = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
                case JointType::Slider: lambda = static_cast<JPH::SliderConstraint*>(c)->GetTotalLambdaPosition().Length(); break;
                case JointType::Distance: lambda = std::fabs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition()); break;
            }
            if (lambda / subDt <= j.breakForce) continue;
            c->SetEnabled(false);
            brokenJoints.push_back(e);
            if (s) {
                if (Joint* comp = s->get<Joint>(e)) comp->enabled = false;
            }
        }
    }

    // --- Contacts after a step ------------------------------------------------------------------------
    bool dynamicAsleep(uint32_t idv) {
        JPH::BodyID id(idv);
        JPH::BodyLockRead lock(locks(), id);
        if (!lock.Succeeded()) return false;
        const JPH::Body& b = lock.GetBody();
        return b.IsDynamic() && !b.IsActive();
    }
    bool settledActive(uint32_t idv) {
        JPH::BodyID id(idv);
        JPH::BodyLockRead lock(locks(), id);
        if (!lock.Succeeded()) return true;
        const JPH::Body& b = lock.GetBody();
        return b.IsStatic() || b.IsActive();
    }

    void processContacts() {
        std::vector<AddRecord> added;
        std::vector<ContactKey> removed;
        {
            std::lock_guard lock(contactMutex);
            added.swap(adds);
            removed.swap(removes);
        }
        std::sort(added.begin(), added.end(), [](const AddRecord& x, const AddRecord& y) { return x.key < y.key; });
        std::sort(removed.begin(), removed.end());
        std::map<std::pair<EntityId, EntityId>, PairInfo> fresh;  // begin info per pair, first by key order
        std::set<ContactKey> addedKeys;
        for (const AddRecord& r : added) {
            if (!bodyEntity.count(r.key.b1) || !bodyEntity.count(r.key.b2)) continue;  // body removed meanwhile
            addedKeys.insert(r.key);
            auto [it, inserted] = contactKeys.try_emplace(r.key, KeyInfo{r.e1, r.e2, r.sensor, false});
            if (!inserted) it->second.retained = false;
            if (r.e1 == r.e2) continue;
            auto key = std::minmax(r.e1, r.e2);
            if (!fresh.count(key)) {
                PairInfo info;
                info.sensor = r.sensor;
                info.point = r.point;
                info.normal = key.first == r.e1 ? r.normal : -r.normal;
                info.speed = r.speed;
                fresh[key] = info;
            }
        }
        for (const ContactKey& k : removed) {
            auto it = contactKeys.find(k);
            if (it == contactKeys.end() || addedKeys.count(k)) continue;
            if (!it->second.sensor && (dynamicAsleep(k.b1) || dynamicAsleep(k.b2))) {
                it->second.retained = true;  // the pile fell asleep; it is still touching
            } else {
                contactKeys.erase(it);
            }
        }
        // Retained contacts whose bodies woke up again and were not re-detected are gone.
        for (auto it = contactKeys.begin(); it != contactKeys.end();) {
            if (it->second.retained && !addedKeys.count(it->first) && settledActive(it->first.b1) && settledActive(it->first.b2)) {
                it = contactKeys.erase(it);
            } else {
                ++it;
            }
        }

        std::map<std::pair<EntityId, EntityId>, bool> now;
        for (const auto& [k, info] : contactKeys) {
            if (info.e1 == info.e2) continue;
            auto key = std::minmax(info.e1, info.e2);
            bool& sensor = now[key];
            sensor = sensor || info.sensor;
        }
        for (const auto& [key, info] : characterTouches) {
            now.try_emplace(key, false);
            fresh.try_emplace(key, info);
        }
        characterTouches.clear();

        auto emitPair = [&](ContactEvent::Kind kind, std::pair<EntityId, EntityId> key, const PairInfo* info) {
            ContactEvent a, b;
            a.kind = b.kind = kind;
            a.self = key.first;
            a.other = key.second;
            b.self = key.second;
            b.other = key.first;
            if (info) {
                a.point = b.point = info->point;
                a.normal = -info->normal;  // toward the first entity
                b.normal = info->normal;
                a.speed = b.speed = info->speed;
            }
            events.push_back(a);
            events.push_back(b);
        };
        for (const auto& [key, sensor] : now) {
            if (presence.count(key)) continue;
            auto f = fresh.find(key);
            emitPair(sensor ? ContactEvent::Kind::TriggerEnter : ContactEvent::Kind::Collide, key,
                     f == fresh.end() ? nullptr : &f->second);
        }
        for (const auto& [key, sensor] : presence) {
            if (!now.count(key) && sensor) emitPair(ContactEvent::Kind::TriggerExit, key, nullptr);
        }
        presence = std::move(now);
    }

    // --- Write back ---------------------------------------------------------------------------------
    std::optional<Transform> localFor(const Scene& s, EntityId e, JPH::BodyID id) const {
        const Transform* t = s.get<Transform>(e);
        const EntityRecord* r = s.record(e);
        if (!t || !r) return std::nullopt;
        JPH::RVec3 pos;
        JPH::Quat rot;
        system.GetBodyInterfaceNoLock().GetPositionAndRotation(id, pos, rot);
        Transform out = *t;
        Vec3 wp = fromJolt(JPH::Vec3(pos));
        if (r->parent) {
            Mat4 parent = s.worldMatrix(r->parent);
            Decomposed pd = decompose(parent);
            out.position = parent.inverse().transformPoint(wp);
            out.rotation = quatToEuler(pd.rotation.Conjugated() * rot);
        } else {
            out.position = wp;
            out.rotation = quatToEuler(rot);
        }
        return out;
    }

    void writeBackBodies(Scene& s) {
        std::vector<BodyEntry*> order;
        for (auto& [k, b] : bodies) {
            if (b.motion == JPH::EMotionType::Dynamic && !b.sensor) order.push_back(&b);
        }
        std::stable_sort(order.begin(), order.end(), [](const BodyEntry* x, const BodyEntry* y) { return x->depth < y->depth; });
        for (BodyEntry* b : order) {
            bool awake = bi().IsActive(b->id);
            RigidBody* rb = s.get<RigidBody>(b->entity);
            if (awake) {
                if (auto local = localFor(s, b->entity, b->id)) {
                    Transform* t = s.get<Transform>(b->entity);
                    t->position = local->position;
                    t->rotation = local->rotation;
                }
            }
            if (rb) {
                rb->velocity = fromJolt(bi().GetLinearVelocity(b->id));
                rb->angularVelocity = fromJolt(bi().GetAngularVelocity(b->id)) * (180.f / kPi);
                b->lastVelocity = rb->velocity;
                b->lastAngular = rb->angularVelocity;
            }
        }
        if (!order.empty()) {
            SceneIndex index(s);
            for (auto& [k, b] : bodies) b.lastWorld = index.world(b.entity);
            s.markDirty();
        }
    }
};

// ---------------------------------------------------------------------------
// PhysicsWorld
// ---------------------------------------------------------------------------

PhysicsWorld::PhysicsWorld(MeshProvider meshes, PathResolver paths, WorldOptions options)
    : impl_(std::make_unique<Impl>(std::move(meshes), std::move(paths), std::move(options))) {}

PhysicsWorld::~PhysicsWorld() = default;

void PhysicsWorld::sync(const Scene& scene, float dt) {
    Impl& m = *impl_;
    m.applySettings(scene);
    SceneIndex index(scene);
    // Query/edit worlds (dt == 0) sync only when the scene changed: always check everything.
    std::vector<Impl::Desired> desired = m.desiredBodies(scene, index, dt <= 0.f);
    ++m.syncCount;
    std::unordered_map<uint64_t, const Impl::Desired*> byKey;  // entity * 2 + role
    byKey.reserve(desired.size() * 2);
    for (const auto& d : desired) byKey[d.entity * 2 + static_cast<uint64_t>(d.role)] = &d;

    // Joints must go before the bodies they reference.
    std::set<uint32_t> doomed;
    for (auto& [key, b] : m.bodies) {
        auto it = byKey.find(key.first * 2 + static_cast<uint64_t>(key.second));
        if (it == byKey.end() || it->second->signature != b.signature) doomed.insert(b.id.GetIndexAndSequenceNumber());
    }
    for (auto it = m.joints.begin(); it != m.joints.end();) {
        if (doomed.count(it->second.a.GetIndexAndSequenceNumber()) ||
            (!it->second.b.IsInvalid() && doomed.count(it->second.b.GetIndexAndSequenceNumber()))) {
            auto next = std::next(it);
            m.removeJoint(it);
            it = next;
        } else {
            ++it;
        }
    }
    m.vehicles->forgetBodies(doomed);
    for (auto it = m.bodies.begin(); it != m.bodies.end();) {
        if (doomed.count(it->second.id.GetIndexAndSequenceNumber())) {
            auto next = std::next(it);
            m.removeBody(it);
            it = next;
        } else {
            ++it;
        }
    }
    int created = 0;
    for (const auto& d : desired) {
        auto it = m.bodies.find({d.entity, d.role});
        if (it == m.bodies.end()) {
            created += m.createBody(scene, d) ? 1 : 0;
        } else {
            m.updateBody(it->second, d, dt);
        }
    }
    if (created > 32) m.system.OptimizeBroadPhase();
    m.syncCharacters(scene);
    m.syncJoints(scene);
    m.vehicles->sync(scene);
}

void PhysicsWorld::step(Scene& scene, float dt) {
    Impl& m = *impl_;
    // physics_world.enabled pauses play simulation; what-if worlds (no write-back) always run.
    if ((!m.enabled && m.options.writeBack) || dt <= 0) return;
    m.lastDt = dt;
    if (m.options.writeBack) m.stepCharacters(dt);  // what-if worlds keep characters as still obstacles
    m.vehicles->preStep(scene, dt);
    JPH::JobSystem& jobs = m.localJobs ? static_cast<JPH::JobSystem&>(*m.localJobs) : sharedJobSystem();
    {
        static std::mutex sharedPoolMutex;  // the shared pool steps one world at a time
        std::unique_lock lock(sharedPoolMutex, std::defer_lock);
        if (!m.localJobs) lock.lock();
        JPH::EPhysicsUpdateError err = m.system.Update(dt, m.substeps, m.temp.get(), &jobs);
        if (err != JPH::EPhysicsUpdateError::None) {
            m.warn("physics step overflowed its buffers (too many contacts/bodies in one place); results may be degraded");
        }
    }
    m.processContacts();
    m.checkBreaking(m.options.writeBack ? &scene : nullptr, dt);
    if (m.options.writeBack) {
        m.writeBackBodies(scene);
        m.writeBackCharacters(scene);
    }
    m.vehicles->postStep(m.options.writeBack ? &scene : nullptr, dt, m.substeps);  // after the chassis moved
}

std::vector<ContactEvent> PhysicsWorld::drainEvents() {
    std::vector<ContactEvent> out;
    out.swap(impl_->events);
    std::stable_sort(out.begin(), out.end(), [](const ContactEvent& a, const ContactEvent& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.self != b.self) return a.self < b.self;
        return a.other < b.other;
    });
    return out;
}

std::vector<EntityId> PhysicsWorld::drainBrokenJoints() {
    std::vector<EntityId> out;
    out.swap(impl_->brokenJoints);
    return out;
}

std::vector<std::string> PhysicsWorld::drainWarnings() {
    std::vector<std::string> out;
    out.swap(impl_->warnings);
    for (const auto& w : out) impl_->warned.insert(w);
    return out;
}

bool PhysicsWorld::addForce(EntityId e, Vec3 force) {
    const auto* b = impl_->solidOf(e);
    if (!b || b->motion != JPH::EMotionType::Dynamic) return false;
    impl_->bi().AddForce(b->id, toJolt(force));
    impl_->bi().ActivateBody(b->id);
    return true;
}

bool PhysicsWorld::addImpulse(EntityId e, Vec3 impulse) {
    const auto* b = impl_->solidOf(e);
    if (!b || b->motion != JPH::EMotionType::Dynamic) return false;
    impl_->bi().AddImpulse(b->id, toJolt(impulse));
    return true;
}

bool PhysicsWorld::addTorque(EntityId e, Vec3 torque) {
    const auto* b = impl_->solidOf(e);
    if (!b || b->motion != JPH::EMotionType::Dynamic) return false;
    impl_->bi().AddTorque(b->id, toJolt(torque));
    impl_->bi().ActivateBody(b->id);
    return true;
}

std::optional<Vec3> PhysicsWorld::velocity(EntityId e) const {
    if (auto it = impl_->chars.find(e); it != impl_->chars.end()) return fromJolt(it->second.character->GetLinearVelocity());
    const auto* b = impl_->solidOf(e);
    if (!b) return std::nullopt;
    return fromJolt(impl_->system.GetBodyInterfaceNoLock().GetLinearVelocity(b->id));
}

bool PhysicsWorld::hasBody(EntityId e) const { return impl_->solidOf(e) != nullptr; }

bool PhysicsWorld::isSleeping(EntityId e) const {
    const auto* b = impl_->solidOf(e);
    return b && b->motion == JPH::EMotionType::Dynamic && !impl_->system.GetBodyInterfaceNoLock().IsActive(b->id);
}

bool PhysicsWorld::allAsleep() const {
    for (const auto& [k, b] : impl_->bodies) {
        if (b.motion == JPH::EMotionType::Dynamic && !b.sensor && impl_->system.GetBodyInterfaceNoLock().IsActive(b.id)) return false;
    }
    return true;
}

std::optional<Transform> PhysicsWorld::localTransform(const Scene& scene, EntityId e) const {
    const auto* b = impl_->solidOf(e);
    if (!b) return std::nullopt;
    return impl_->localFor(scene, e, b->id);
}

bool PhysicsWorld::walk(EntityId e, Vec3 direction) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    Vec3 d{direction.x, 0.f, direction.z};
    float len = length(d);
    if (len > 1.f) d = d / len;
    it->second.desired = d * it->second.settings.moveSpeed;
    return true;
}

bool PhysicsWorld::setDesiredVelocity(EntityId e, Vec3 v) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    it->second.desired = {v.x, 0.f, v.z};
    return true;
}

bool PhysicsWorld::jump(EntityId e, float speed) {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return false;
    if (it->second.character->GetGroundState() != JPH::CharacterBase::EGroundState::OnGround) return false;
    it->second.jumpRequested = true;
    it->second.jumpSpeed = speed > 0 ? speed : it->second.settings.jumpSpeed;
    return true;
}

std::optional<bool> PhysicsWorld::grounded(EntityId e) const {
    auto it = impl_->chars.find(e);
    if (it == impl_->chars.end()) return std::nullopt;
    return it->second.character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

bool PhysicsWorld::hasCharacter(EntityId e) const { return impl_->chars.count(e) != 0; }

// --- Vehicles ----------------------------------------------------------------------------------

std::vector<EntityId> PhysicsWorld::vehicleEntities() const { return impl_->vehicles->entities(); }

std::optional<VehicleTelemetry> PhysicsWorld::vehicle(EntityId e) const { return impl_->vehicles->telemetry(e); }

bool PhysicsWorld::setVehicleInput(EntityId e, std::optional<VehicleInput> input) { return impl_->vehicles->setInput(e, input); }

bool PhysicsWorld::shiftVehicle(EntityId e, int gear) { return impl_->vehicles->shift(e, gear); }

VehicleStats PhysicsWorld::vehicleStats() const { return impl_->vehicles->stats(); }

// --- Queries ---------------------------------------------------------------------------------

std::vector<Hit> PhysicsWorld::raycastAll(Vec3 origin, Vec3 direction, float maxDistance, const QueryFilter& filter) const {
    std::vector<Hit> out;
    Vec3 dir = normalize(direction);
    if (length(dir) < 0.5f || maxDistance <= 0) return out;
    JPH::RRayCast ray(JPH::RVec3(toJolt(origin)), toJolt(dir * maxDistance));
    JPH::RayCastSettings settings;
    settings.mTreatConvexAsSolid = true;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    QueryBodyFilter bodyFilter(filter);
    QueryLayerFilter layerFilter(filter.layerMask);
    impl_->system.GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, {}, layerFilter, bodyFilter);
    std::map<EntityId, Hit> best;
    for (const auto& h : collector.mHits) {
        JPH::BodyLockRead lock(impl_->locks(), h.mBodyID);
        if (!lock.Succeeded()) continue;
        const JPH::Body& body = lock.GetBody();
        Hit hit;
        hit.entity = static_cast<EntityId>(body.GetUserData());
        hit.distance = h.mFraction * maxDistance;
        JPH::RVec3 p = ray.GetPointOnRay(h.mFraction);
        hit.point = fromJolt(JPH::Vec3(p));
        hit.normal = fromJolt(body.GetWorldSpaceSurfaceNormal(h.mSubShapeID2, p));
        if (dot(hit.normal, dir) > 0 && h.mFraction <= 0.f) hit.normal = -dir;  // started inside
        auto it = best.find(hit.entity);
        if (it == best.end() || hit.distance < it->second.distance) best[hit.entity] = hit;
    }
    for (auto& [e, h] : best) out.push_back(h);
    std::sort(out.begin(), out.end(), [](const Hit& a, const Hit& b) {
        return a.distance != b.distance ? a.distance < b.distance : a.entity < b.entity;
    });
    return out;
}

std::optional<Hit> PhysicsWorld::raycast(Vec3 origin, Vec3 direction, float maxDistance, const QueryFilter& filter) const {
    auto all = raycastAll(origin, direction, maxDistance, filter);
    if (all.empty()) return std::nullopt;
    return all.front();
}

namespace {

JPH::RefConst<JPH::Shape> queryShape(const QueryShape& s) {
    switch (s.kind) {
        case QueryShape::Kind::Sphere: return new JPH::SphereShape(std::max(s.radius, 0.001f));
        case QueryShape::Kind::Box: {
            Vec3 h = vmax(s.halfExtents, Vec3(0.001f));
            return new JPH::BoxShape(toJolt(h), std::min(JPH::cDefaultConvexRadius, std::min({h.x, h.y, h.z}) * 0.5f));
        }
        case QueryShape::Kind::Capsule: {
            float r = std::max(s.radius, 0.001f);
            float half = s.height * 0.5f - r;
            if (half <= 1e-3f) return new JPH::SphereShape(r);
            return new JPH::CapsuleShape(half, r);
        }
    }
    return new JPH::SphereShape(0.5f);
}

}  // namespace

std::optional<Hit> PhysicsWorld::shapecast(const QueryShape& shape, Vec3 origin, Vec3 direction, float maxDistance,
                                           const QueryFilter& filter) const {
    Vec3 dir = normalize(direction);
    if (length(dir) < 0.5f || maxDistance <= 0) return std::nullopt;
    JPH::RefConst<JPH::Shape> s = queryShape(shape);
    JPH::RMat44 start = JPH::RMat44::sRotationTranslation(eulerToQuat(shape.rotation), JPH::RVec3(toJolt(origin)));
    JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(s, JPH::Vec3::sOne(), start, toJolt(dir * maxDistance));
    JPH::ShapeCastSettings settings;
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    QueryBodyFilter bodyFilter(filter);
    QueryLayerFilter layerFilter(filter.layerMask);
    impl_->system.GetNarrowPhaseQueryNoLock().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layerFilter, bodyFilter);
    std::optional<Hit> best;
    for (const auto& h : collector.mHits) {
        JPH::BodyLockRead lock(impl_->locks(), h.mBodyID2);
        if (!lock.Succeeded()) continue;
        Hit hit;
        hit.entity = static_cast<EntityId>(lock.GetBody().GetUserData());
        hit.distance = h.mFraction * maxDistance;
        hit.point = fromJolt(h.mContactPointOn2);
        hit.normal = -normalize(fromJolt(h.mPenetrationAxis));
        if (!best || hit.distance < best->distance || (hit.distance == best->distance && hit.entity < best->entity)) best = hit;
    }
    return best;
}

std::vector<EntityId> PhysicsWorld::overlap(const QueryShape& shape, Vec3 center, const QueryFilter& filter) const {
    JPH::RefConst<JPH::Shape> s = queryShape(shape);
    JPH::RMat44 xf = JPH::RMat44::sRotationTranslation(eulerToQuat(shape.rotation), JPH::RVec3(toJolt(center)));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryBodyFilter bodyFilter(filter);
    QueryLayerFilter layerFilter(filter.layerMask);
    impl_->system.GetNarrowPhaseQueryNoLock().CollideShape(s, JPH::Vec3::sOne(), xf, settings, JPH::RVec3::sZero(), collector, {},
                                                          layerFilter, bodyFilter);
    std::map<EntityId, float> nearest;
    for (const auto& h : collector.mHits) {
        JPH::BodyLockRead lock(impl_->locks(), h.mBodyID2);
        if (!lock.Succeeded()) continue;
        EntityId e = static_cast<EntityId>(lock.GetBody().GetUserData());
        float d = distance(fromJolt(h.mContactPointOn2), center);
        auto it = nearest.find(e);
        if (it == nearest.end() || d < it->second) nearest[e] = d;
    }
    std::vector<std::pair<float, EntityId>> sorted;
    for (auto& [e, d] : nearest) sorted.emplace_back(d, e);
    std::sort(sorted.begin(), sorted.end());
    std::vector<EntityId> out;
    for (auto& [d, e] : sorted) out.push_back(e);
    return out;
}

// --- Introspection ----------------------------------------------------------------------------

Stats PhysicsWorld::stats() const {
    Stats st;
    const auto& bi = impl_->system.GetBodyInterfaceNoLock();
    for (const auto& [k, b] : impl_->bodies) {
        ++st.bodies;
        if (b.sensor) {
            ++st.triggers;
            continue;
        }
        switch (b.motion) {
            case JPH::EMotionType::Dynamic:
                ++st.dynamicBodies;
                if (bi.IsActive(b.id)) ++st.activeBodies;
                else ++st.sleepingBodies;
                break;
            case JPH::EMotionType::Kinematic: ++st.kinematicBodies; break;
            case JPH::EMotionType::Static: ++st.staticBodies; break;
        }
    }
    st.characters = static_cast<int>(impl_->chars.size());
    st.joints = static_cast<int>(impl_->joints.size());
    st.contacts = static_cast<int>(impl_->presence.size());
    st.vehicles = static_cast<int>(impl_->vehicles->size());
    return st;
}

namespace {

void appendTriangles(const JPH::TransformedShape& ts, int maxTriangles, std::vector<Vec3>& tris) {
    JPH::AABox box = ts.GetWorldSpaceBounds();
    box.ExpandBy(JPH::Vec3::sReplicate(0.01f));
    // Triangles can only be pulled from leaf shapes: flatten compounds / decorated shapes first.
    JPH::AllHitCollisionCollector<JPH::TransformedShapeCollector> leaves;
    ts.CollectTransformedShapes(box, leaves);
    constexpr int kBatch = 256;
    JPH::Float3 verts[kBatch * 3];
    int total = 0;
    for (const JPH::TransformedShape& leaf : leaves.mHits) {
        JPH::Shape::GetTrianglesContext ctx;
        leaf.GetTrianglesStart(ctx, box, JPH::RVec3::sZero());
        while (total < maxTriangles) {
            int n = leaf.GetTrianglesNext(ctx, kBatch, verts);
            if (n <= 0) break;
            for (int i = 0; i < n * 3; ++i) tris.push_back({verts[i].x, verts[i].y, verts[i].z});
            total += n;
        }
    }
}

}  // namespace

std::vector<DebugShape> PhysicsWorld::debugShapes(int maxTrianglesPerShape) const {
    std::vector<DebugShape> out;
    auto toLines = [](const std::vector<Vec3>& tris, std::vector<Vec3>& lines) {
        lines.reserve(tris.size() * 2);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                lines.push_back(tris[i + k]);
                lines.push_back(tris[i + (k + 1) % 3]);
            }
        }
    };
    for (const auto& [k, b] : impl_->bodies) {
        JPH::BodyLockRead lock(impl_->locks(), b.id);
        if (!lock.Succeeded()) continue;
        DebugShape d;
        d.entity = b.entity;
        d.kind = b.sensor ? "trigger" : motionName(b.motion);
        d.sleeping = b.motion == JPH::EMotionType::Dynamic && !lock.GetBody().IsActive();
        std::vector<Vec3> tris;
        appendTriangles(lock.GetBody().GetTransformedShape(), maxTrianglesPerShape, tris);
        toLines(tris, d.lines);
        out.push_back(std::move(d));
    }
    for (const auto& [e, c] : impl_->chars) {
        DebugShape d;
        d.entity = e;
        d.kind = "character";
        std::vector<Vec3> tris;
        appendTriangles(c.character->GetTransformedShape(), maxTrianglesPerShape, tris);
        toLines(tris, d.lines);
        out.push_back(std::move(d));
    }
    return out;
}

void PhysicsWorld::staticTriangles(std::vector<Vec3>& triangles, std::vector<EntityId>& owners) const {
    for (const auto& [k, b] : impl_->bodies) {
        if (b.sensor || b.motion != JPH::EMotionType::Static) continue;
        JPH::BodyLockRead lock(impl_->locks(), b.id);
        if (!lock.Succeeded()) continue;
        size_t before = triangles.size();
        appendTriangles(lock.GetBody().GetTransformedShape(), 4'000'000, triangles);
        owners.insert(owners.end(), (triangles.size() - before) / 3, b.entity);
    }
}

}  // namespace sky::physics
