// Physics2DWorld: scene -> Box2D reconciliation, stepping, events and write-back. Characters are in
// Character2DController.cpp, queries and debug drawing in Physics2DQueries.cpp.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "Physics2DInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render2d/Tilemap.h"

namespace sky::physics2d {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.f;
constexpr float kHitThreshold = 0.05f;  // units/s

/// Per-sync view of the hierarchy: memoized world matrices and children lists.
class Index2D {
public:
    explicit Index2D(const Scene& s) : s_(s) {
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

bool hasBodyAncestor(const Scene& s, EntityId e) {
    for (const EntityRecord* r = s.record(e); r && r->parent; r = s.record(r->parent)) {
        if (s.get<Body2D>(r->parent) || s.get<Character2D>(r->parent)) return true;
    }
    return false;
}

/// Product of local scales up the hierarchy: stable while the entity moves and turns.
Vec3 chainScale(const Scene& s, EntityId id) {
    Vec3 out{1.f};
    for (const EntityRecord* r = s.record(id); r; r = r->parent ? s.record(r->parent) : nullptr) {
        if (const Transform* t = s.get<Transform>(r->id)) out = out * t->scale;
    }
    return out;
}

/// `part`'s transform relative to its ancestor `owner` (local matrices multiplied down the chain), so
/// it does not change while the owner moves.
Mat4 relativeTo(const Scene& s, EntityId owner, EntityId part) {
    Mat4 m{};
    for (EntityId e = part; e && e != owner;) {
        const EntityRecord* r = s.record(e);
        if (!r) break;
        if (const Transform* t = s.get<Transform>(e)) m = t->local() * m;
        e = r->parent;
    }
    return m;
}

int depthOf(const Scene& s, EntityId id) {
    int d = 0;
    for (const EntityRecord* r = s.record(id); r && r->parent; r = s.record(r->parent)) ++d;
    return d;
}

/// The collider entities that shape `owner`'s body: its own, then descendants without a body of their own.
void collectParts(const Scene& s, const Index2D& index, EntityId e, std::vector<EntityId>& out) {
    for (EntityId c : index.children(e)) {
        if (!s.isActive(c) || s.get<Body2D>(c) || s.get<Character2D>(c)) continue;
        if (s.get<Collider2D>(c)) out.push_back(c);
        collectParts(s, index, c, out);
    }
}

b2BodyType bodyType(const Body2D* rb, bool forced, bool freezeOthers) {
    if (forced) return b2_dynamicBody;
    if (!rb) return b2_staticBody;
    if (freezeOthers) return b2_staticBody;
    if (rb->motion == "dynamic") return b2_dynamicBody;
    if (rb->motion == "kinematic") return b2_kinematicBody;
    return b2_staticBody;
}

/// Points of a Json [[x, y], ...] list.
std::vector<Vec2> jsonPoints(const Json& j) {
    std::vector<Vec2> out;
    for (const Json& p : j.elements()) {
        if (p.isArray() && p.size() >= 2) out.push_back({static_cast<float>(p[0].asNumber()), static_cast<float>(p[1].asNumber())});
    }
    return out;
}

bool samePose(const Pose2D& a, const Pose2D& b) {
    return std::abs(a.position.x - b.position.x) < 1e-4f && std::abs(a.position.y - b.position.y) < 1e-4f &&
           std::abs(std::remainder(a.angle - b.angle, 2.f * kPi)) < 1e-4f;
}

}  // namespace

Pose2D poseOf(const Mat4& m) {
    Pose2D p;
    Vec3 o = m.transformPoint(Vec3(0.f));
    Vec3 ax = m.transformDir({1.f, 0.f, 0.f});
    Vec3 ay = m.transformDir({0.f, 1.f, 0.f});
    p.position = {o.x, o.y};
    p.z = o.z;
    p.angle = std::atan2(ax.y, ax.x);
    p.scale = {std::sqrt(ax.x * ax.x + ax.y * ax.y), std::sqrt(ay.x * ay.x + ay.y * ay.y)};
    if (ax.x * ay.y - ax.y * ay.x < 0.f) p.scale.y = -p.scale.y;  // mirrored
    return p;
}

void Hash2D::reflected(const void* object, const TypeInfo& type, std::initializer_list<const char*> skip) {
    const auto* base = static_cast<const unsigned char*>(object);
    for (const FieldInfo& f : type.fields) {
        if (std::any_of(skip.begin(), skip.end(), [&](const char* n) { return f.name == n; })) continue;
        const unsigned char* p = base + f.offset;
        switch (f.type) {
            case FieldType::Float: bytes(p, sizeof(float)); break;
            case FieldType::Int: bytes(p, sizeof(int)); break;
            case FieldType::Bool: bytes(p, sizeof(bool)); break;
            case FieldType::Vec2: bytes(p, sizeof(Vec2)); break;
            case FieldType::Vec3: bytes(p, sizeof(Vec3)); break;
            case FieldType::Vec4:
            case FieldType::Color: bytes(p, sizeof(Vec4)); break;
            case FieldType::String:
            case FieldType::Enum: str(*reinterpret_cast<const std::string*>(p)); break;
            case FieldType::Json: str(reinterpret_cast<const Json*>(p)->dump()); break;
            case FieldType::Entity: {
                const auto* l = reinterpret_cast<const EntityLink*>(p);
                pod(l->id);
                str(l->name);
                break;
            }
            case FieldType::EntityList: break;
        }
    }
}

const std::vector<std::string>& layerNames2D() {
    static const std::vector<std::string> names{"default", "static", "player", "enemy", "projectile", "trigger", "debris"};
    return names;
}

uint64_t parseLayerMask(const std::string& mask, std::string* unknown) {
    std::string m = str::trim(mask);
    if (m.empty() || m == "all") return ~0ull;
    if (m == "none") return 0;
    uint64_t bits = 0;
    for (const std::string& part : str::split(m, ',')) {
        std::string name = str::trim(part);
        if (name.empty()) continue;
        const auto& names = layerNames2D();
        auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end()) {
            if (unknown && unknown->empty()) {
                std::string guess = str::closest(name, names, 3);
                *unknown = "unknown collision layer '" + name + "'" + (guess.empty() ? "" : " - did you mean '" + guess + "'?");
            }
            continue;
        }
        bits |= 1ull << static_cast<unsigned>(it - names.begin());
    }
    return bits;
}

b2Filter makeFilter(const std::string& layer, const std::string& mask, bool oneWay, std::string* unknown) {
    b2Filter f = b2DefaultFilter();
    const auto& names = layerNames2D();
    auto it = std::find(names.begin(), names.end(), layer);
    uint64_t layerBit = it == names.end() ? 1ull : 1ull << static_cast<unsigned>(it - names.begin());
    if (it == names.end() && unknown && unknown->empty()) unknown->assign("unknown collision layer '" + layer + "'");
    // One-way platforms only carry kOneWayBit, so character casts can leave them out; everything else
    // collides with them (their own mask still filters by layer).
    f.categoryBits = oneWay ? kOneWayBit : layerBit;
    f.maskBits = parseLayerMask(mask, unknown);
    if (!oneWay) f.maskBits |= kOneWayBit | kCharacterBit;
    return f;
}

bool preSolveOneWay(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void*) {
    const auto* a = static_cast<const ShapeTag*>(b2Shape_GetUserData(shapeIdA));
    const auto* b = static_cast<const ShapeTag*>(b2Shape_GetUserData(shapeIdB));
    const bool aOne = a && a->oneWay, bOne = b && b->oneWay;
    if (aOne == bOne) return true;
    // The manifold normal points from A to B: make it point from the platform to the other shape.
    Vec2 n = fromB2(manifold->normal);
    if (bOne) n = n * -1.f;
    const ShapeTag* platform = aOne ? a : b;
    return dot2(n, platform->up) > 0.7f;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

Physics2DWorld::Impl::Impl(TileCollisionProvider t, WorldOptions2D o) : tiles(std::move(t)), options(std::move(o)) {
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = toB2(settings.gravity);
    def.enableSleep = settings.allowSleep;
    def.hitEventThreshold = kHitThreshold;  // low: hits give contacts their approach speed; `impact` filters by impactSpeed
    def.workerCount = 1;  // single-threaded: deterministic and no task system needed
    world = b2CreateWorld(&def);
    b2World_SetPreSolveCallback(world, preSolveOneWay, this);
    b2BodyDef gd = b2DefaultBodyDef();
    gd.type = b2_staticBody;
    ground = b2CreateBody(world, &gd);
}

Physics2DWorld::Impl::~Impl() {
    if (B2_IS_NON_NULL(world)) b2DestroyWorld(world);
}

void Physics2DWorld::Impl::warn(const std::string& w) {
    if (warned.insert(w).second) warnings.push_back(w);
}

b2BodyId Physics2DWorld::Impl::bodyFor(EntityId e) const {
    auto o = ownerOf.find(e);
    EntityId owner = o == ownerOf.end() ? e : o->second;
    if (auto it = bodies.find(owner); it != bodies.end()) return it->second.id;
    if (auto it = chars.find(owner); it != chars.end()) return it->second.id;
    return b2_nullBodyId;
}

void Physics2DWorld::Impl::destroyBody(BodyEntry& b) {
    if (B2_IS_NON_NULL(b.id) && b2Body_IsValid(b.id)) b2DestroyBody(b.id);  // destroys its shapes, chains and joints
    b.id = b2_nullBodyId;
    b.shapes.clear();
    b.tags.clear();
}

void Physics2DWorld::Impl::destroyJoint(JointEntry& j) {
    if (B2_IS_NON_NULL(j.id) && b2Joint_IsValid(j.id)) b2DestroyJoint(j.id);
    j.id = b2_nullJointId;
}

void Physics2DWorld::Impl::destroyCharacter(CharEntry& c) {
    if (B2_IS_NON_NULL(c.id) && b2Body_IsValid(c.id)) b2DestroyBody(c.id);
    c.id = b2_nullBodyId;
}

void Physics2DWorld::Impl::sync(const Scene& s, float dt) {
    ++syncCount;
    // Scene-wide settings: the first enabled physics2d_world in scene order.
    Physics2DSettings next;
    for (EntityId e : s.entities()) {
        if (const Physics2DSettings* ps = s.get<Physics2DSettings>(e); ps && s.isActive(e)) {
            next = *ps;
            break;
        }
    }
    if (next.gravity != settings.gravity) b2World_SetGravity(world, toB2(next.gravity));
    if (next.allowSleep != settings.allowSleep) b2World_EnableSleeping(world, next.allowSleep);
    settings = next;
    syncBodies(s, dt);
    syncCharacters(s);
    syncJoints(s);
}

bool Physics2DWorld::Impl::buildBody(const Scene& s, EntityId e, const Pose2D& pose, BodyEntry& out) {
    const Body2D* rb = s.get<Body2D>(e);
    const bool forced = options.forceDynamic.count(e) != 0;
    b2BodyDef def = b2DefaultBodyDef();
    def.type = bodyType(rb, forced, options.freezeOthers && !forced);
    def.position = toB2(pose.position);
    def.rotation = b2MakeRot(pose.angle);
    def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(e));
    if (rb) {
        def.gravityScale = rb->gravityScale;
        def.linearDamping = rb->linearDamping;
        def.angularDamping = rb->angularDamping;
        def.fixedRotation = rb->fixedRotation;
        def.isBullet = rb->bullet;
        def.enableSleep = rb->allowSleep;
        def.isAwake = rb->startAwake;
        def.linearVelocity = toB2(rb->velocity);
        def.angularVelocity = rb->angularVelocity * kDeg;
    }
    out.entity = e;
    out.type = def.type;
    out.velocityDriven = def.type == b2_kinematicBody && rb && (rb->velocity != Vec2{0.f, 0.f} || rb->angularVelocity != 0.f);
    out.id = b2CreateBody(world, &def);
    out.lastPose = pose;
    out.lastVelocity = rb ? rb->velocity : Vec2{};
    out.lastAngular = rb ? rb->angularVelocity : 0.f;
    out.depth = depthOf(s, e);
    out.chains = 0;
    out.tilemap.reset();
    return true;
}

void Physics2DWorld::Impl::syncBodies(const Scene& s, float dt) {
    Index2D index(s);
    std::set<EntityId> seen;
    ownerOf.clear();
    for (EntityId e : s.entities()) {
        if (!s.isActive(e) || s.get<Character2D>(e)) continue;
        const Body2D* rb = s.get<Body2D>(e);
        const Collider2D* col = s.get<Collider2D>(e);
        const bool forced = options.forceDynamic.count(e) != 0;
        if (!rb && !col && !forced) continue;
        if (hasBodyAncestor(s, e)) continue;  // a part of an ancestor's body (collected below)
        std::vector<EntityId> parts;
        if (col) parts.push_back(e);
        collectParts(s, index, e, parts);
        if (parts.empty()) {
            if (rb) warn("'" + s.record(e)->name + "' has a body2d but no collider2d: add one (physics2d_add) or it collides with nothing");
            if (!rb) continue;
        }
        const Pose2D pose = poseOf(index.world(e));

        // Signature: everything that shapes the body (not its live velocity, not its own world pose).
        Hash2D h;
        h.pod(bodyType(rb, forced, options.freezeOthers && !forced));
        if (rb) h.reflected(rb, Body2D::type(), {"velocity", "angularVelocity", "sleeping"});
        h.pod(chainScale(s, e));
        for (EntityId p : parts) {
            const Collider2D* c = s.get<Collider2D>(p);
            h.pod(p);
            h.reflected(c, Collider2D::type());
            if (p != e) {
                const Mat4 rel = relativeTo(s, e, p);
                h.bytes(rel.m, sizeof(rel.m));
            }
            if (c->shape == "tilemap") {
                if (const Tilemap* tm = s.get<Tilemap>(p)) {
                    h.str(tm->tileset);
                    h.pod(tm->tileSize);
                    h.pod(tm->cellSize);
                    h.pod(tm->width);
                    h.pod(tm->height);
                    h.str(tm->layers.dump());
                    h.str(tm->solidTiles.dump());
                }
            }
            ownerOf[p] = e;
        }
        seen.insert(e);

        auto it = bodies.find(e);
        if (it != bodies.end() && it->second.signature == h.h && B2_IS_NON_NULL(it->second.id)) {
            BodyEntry& b = it->second;
            // Moved by something else (editor, scripts, teleport): follow it.
            if (!samePose(pose, b.lastPose)) {
                if (b.type == b2_kinematicBody && !b.velocityDriven && dt > 0.f) {
                    b2Body_SetTargetTransform(b.id, b2Transform{toB2(pose.position), b2MakeRot(pose.angle)}, dt);
                } else {
                    b2Body_SetTransform(b.id, toB2(pose.position), b2MakeRot(pose.angle));
                    if (b.type != b2_staticBody) b2Body_SetAwake(b.id, true);
                }
                b.lastPose = pose;
                if (b.type != b2_staticBody) {
                    for (auto& tag : b.tags) {
                        if (tag->oneWay) tag->up = rotate2({0.f, 1.f}, pose.angle);
                    }
                }
            } else if (b.type == b2_kinematicBody && !b.velocityDriven && dt > 0.f) {
                b2Body_SetLinearVelocity(b.id, {0.f, 0.f});
                b2Body_SetAngularVelocity(b.id, 0.f);
            }
            // Velocity written by a script or tool since the last write-back.
            if (rb && b.type != b2_staticBody) {
                if (rb->velocity != b.lastVelocity) {
                    b2Body_SetLinearVelocity(b.id, toB2(rb->velocity));
                    b2Body_SetAwake(b.id, true);
                    b.lastVelocity = rb->velocity;
                }
                if (rb->angularVelocity != b.lastAngular) {
                    b2Body_SetAngularVelocity(b.id, rb->angularVelocity * kDeg);
                    b2Body_SetAwake(b.id, true);
                    b.lastAngular = rb->angularVelocity;
                }
            }
            continue;
        }

        // (Re)build.
        if (it != bodies.end()) destroyBody(it->second);
        BodyEntry entry;
        buildBody(s, e, pose, entry);
        entry.signature = h.h;
        const b2Transform bodyXf{toB2(pose.position), b2MakeRot(pose.angle)};
        auto toBody = [&](const Mat4& m, Vec2 q) {
            Vec3 w = m.transformPoint({q.x, q.y, 0.f});
            return b2InvTransformPoint(bodyXf, b2Vec2{w.x, w.y});
        };
        const std::string& name = s.record(e)->name;
        for (EntityId p : parts) {
            const Collider2D* c = s.get<Collider2D>(p);
            const Mat4& m = index.world(p);
            const Pose2D pp = poseOf(m);
            const float scaleMax = std::max(std::abs(pp.scale.x), std::abs(pp.scale.y));
            // Collider-local point -> body-local point.
            auto local = [&](Vec2 q) { return toBody(m, c->offset + rotate2(q, c->rotation * kDeg)); };
            std::string unknown;
            auto addTag = [&](bool oneWay) {
                auto tag = std::make_unique<ShapeTag>();
                tag->entity = p;
                tag->owner = e;
                tag->oneWay = oneWay;
                tag->sensor = c->sensor;
                tag->layer = makeFilter(c->layer, "all", false, nullptr).categoryBits;
                tag->up = rotate2({0.f, 1.f}, pp.angle + c->rotation * kDeg);
                entry.tags.push_back(std::move(tag));
                return entry.tags.back().get();
            };
            auto shapeDef = [&](bool oneWay) {
                b2ShapeDef d = b2DefaultShapeDef();
                d.material.friction = c->friction;
                d.material.restitution = c->restitution;
                d.density = c->density;
                d.isSensor = c->sensor;
                d.filter = makeFilter(c->layer, c->mask, oneWay && !c->sensor, &unknown);
                d.enableSensorEvents = entry.type != b2_staticBody || c->sensor;  // sensors ignore static scenery
                d.enableContactEvents = !c->sensor;
                d.enableHitEvents = !c->sensor;
                d.enablePreSolveEvents = oneWay && !c->sensor;
                d.userData = addTag(oneWay && !c->sensor);
                d.updateBodyMass = false;
                return d;
            };
            auto addPolygon = [&](const std::vector<b2Vec2>& pts, bool oneWay) {
                if (pts.size() < 3 || pts.size() > B2_MAX_POLYGON_VERTICES) {
                    warn("'" + s.record(p)->name + "': a collider2d polygon needs 3 to 8 points");
                    return;
                }
                b2Hull hull = b2ComputeHull(pts.data(), static_cast<int>(pts.size()));
                if (hull.count == 0) {
                    warn("'" + s.record(p)->name + "': collider2d points do not form a convex polygon with area");
                    return;
                }
                b2Polygon poly = b2MakePolygon(&hull, 0.f);
                b2ShapeDef d = shapeDef(oneWay);
                entry.shapes.push_back(b2CreatePolygonShape(entry.id, &d, &poly));
            };
            const std::string& kind = c->shape;
            if (kind == "box") {
                Vec2 hs{c->size.x * 0.5f, c->size.y * 0.5f};
                addPolygon({local({-hs.x, -hs.y}), local({hs.x, -hs.y}), local({hs.x, hs.y}), local({-hs.x, hs.y})}, c->oneWay);
            } else if (kind == "circle") {
                b2ShapeDef d = shapeDef(c->oneWay);
                b2Circle circle{local({0.f, 0.f}), std::max(0.001f, c->radius * scaleMax)};
                entry.shapes.push_back(b2CreateCircleShape(entry.id, &d, &circle));
            } else if (kind == "capsule") {
                const float r = std::max(0.001f, c->radius * std::abs(pp.scale.x));
                const float half = std::max(0.f, c->height * 0.5f - c->radius);
                b2ShapeDef d = shapeDef(c->oneWay);
                b2Vec2 c1 = local({0.f, -half}), c2 = local({0.f, half});
                if (b2Distance(c1, c2) < 2.f * kLinearSlop) {
                    b2Circle circle{local({0.f, 0.f}), r};
                    entry.shapes.push_back(b2CreateCircleShape(entry.id, &d, &circle));
                } else {
                    b2Capsule cap{c1, c2, r};
                    entry.shapes.push_back(b2CreateCapsuleShape(entry.id, &d, &cap));
                }
            } else if (kind == "polygon") {
                std::vector<b2Vec2> pts;
                for (Vec2 q : jsonPoints(c->points)) pts.push_back(local(q));
                addPolygon(pts, c->oneWay);
            } else if (kind == "segment") {
                auto pts = jsonPoints(c->points);
                if (pts.size() != 2) {
                    warn("'" + s.record(p)->name + "': a collider2d segment needs exactly 2 points");
                } else {
                    b2ShapeDef d = shapeDef(c->oneWay);
                    b2Segment seg{local(pts[0]), local(pts[1])};
                    entry.shapes.push_back(b2CreateSegmentShape(entry.id, &d, &seg));
                }
            } else if (kind == "chain" || kind == "tilemap") {
                if (entry.type == b2_dynamicBody) {
                    warn("'" + s.record(p)->name + "': chain and tilemap colliders are for static or kinematic bodies (they have no mass)");
                }
                auto addChain = [&](const std::vector<b2Vec2>& pts, bool loop) {
                    if (pts.size() < 4) {
                        warn("'" + s.record(p)->name + "': a collider2d chain needs at least 4 points");
                        return;
                    }
                    b2ChainDef cd = b2DefaultChainDef();
                    cd.points = pts.data();
                    cd.count = static_cast<int>(pts.size());
                    cd.isLoop = loop;
                    b2SurfaceMaterial mat = b2DefaultSurfaceMaterial();
                    mat.friction = c->friction;
                    mat.restitution = c->restitution;
                    cd.materials = &mat;
                    cd.materialCount = 1;
                    cd.filter = makeFilter(c->layer, c->mask, false, &unknown);
                    cd.enableSensorEvents = entry.type != b2_staticBody;
                    cd.userData = addTag(false);
                    b2ChainId chain = b2CreateChain(entry.id, &cd);
                    std::vector<b2ShapeId> segs(static_cast<size_t>(b2Chain_GetSegmentCount(chain)));
                    b2Chain_GetSegments(chain, segs.data(), static_cast<int>(segs.size()));
                    entry.shapes.insert(entry.shapes.end(), segs.begin(), segs.end());
                    ++entry.chains;
                };
                if (kind == "chain") {
                    std::vector<b2Vec2> pts;
                    for (Vec2 q : jsonPoints(c->points)) pts.push_back(local(q));
                    addChain(pts, c->loop);
                } else if (const Tilemap* tm = s.get<Tilemap>(p)) {
                    auto grid = tiles::Grid::fromComponent(*tm);
                    if (!grid) {
                        warn("'" + s.record(p)->name + "': tilemap: " + grid.error().message);
                    } else {
                        TileCollision tc;
                        if (tiles) {
                            auto r = tiles(*tm);
                            if (r) tc = std::move(r.value());
                            else warn("'" + s.record(p)->name + "': " + r.error().message);
                        }
                        if (auto extra = tiles::parseIdList(tm->solidTiles); extra) {
                            tc.solidIds.insert(tc.solidIds.end(), extra->begin(), extra->end());
                        }
                        std::sort(tc.solidIds.begin(), tc.solidIds.end());
                        auto own = parseTileShapes(c->tileShapes, tm->tileSize);
                        if (!own) warn("'" + s.record(p)->name + "': " + own.error().message);
                        else for (auto& [id, shape] : own.value()) tc.shapes[id] = shape;
                        TileGeometry g = buildTileGeometry(grid.value(), tc, tm->cellSize, c->tileMerge != "boxes");
                        // Tile geometry is in the map's local space: no collider offset/rotation applied on top.
                        auto mapLocal = [&](Vec2 q) { return toBody(m, q); };
                        for (const auto& loop : g.loops) {
                            std::vector<b2Vec2> pts;
                            for (Vec2 q : loop) pts.push_back(mapLocal(q));
                            if (pts.size() == 3) {  // a chain needs 4 points: close a triangle as a polygon
                                addPolygon(pts, false);
                            } else {
                                addChain(pts, true);
                            }
                        }
                        for (const auto& b : g.boxes) {
                            addPolygon({mapLocal(b.min), mapLocal({b.max.x, b.min.y}), mapLocal(b.max), mapLocal({b.min.x, b.max.y})}, false);
                        }
                        for (const auto& poly : g.polygons) {
                            std::vector<b2Vec2> pts;
                            for (Vec2 q : poly.points) pts.push_back(mapLocal(q));
                            addPolygon(pts, poly.oneWay);
                        }
                        entry.tilemap = Physics2DWorld::TilemapInfo{static_cast<int>(g.loops.size()), static_cast<int>(g.boxes.size()),
                                                                    static_cast<int>(g.polygons.size()), g.solidCells};
                    }
                } else {
                    warn("'" + s.record(p)->name + "': collider2d shape \"tilemap\" needs a tilemap component on the same entity");
                }
            } else {
                warn("'" + s.record(p)->name + "': unknown collider2d shape '" + kind + "'");
            }
            if (!unknown.empty()) warn("'" + s.record(p)->name + "': " + unknown);
        }
        if (entry.type == b2_dynamicBody) {
            b2Body_ApplyMassFromShapes(entry.id);
            if (rb && rb->mass > 0.f) {
                b2MassData md = b2Body_GetMassData(entry.id);
                const float scale = md.mass > 0.f ? rb->mass / md.mass : 1.f;
                if (md.mass <= 0.f) md.rotationalInertia = rb->mass * 0.1f;
                else md.rotationalInertia *= scale;
                md.mass = rb->mass;
                b2Body_SetMassData(entry.id, md);
            }
        }
        (void)name;
        bodies[e] = std::move(entry);
    }
    for (auto it = bodies.begin(); it != bodies.end();) {
        if (!seen.count(it->first)) {
            destroyBody(it->second);
            it = bodies.erase(it);
        } else {
            ++it;
        }
    }
}

void Physics2DWorld::Impl::syncCharacters(const Scene& s) {
    Index2D index(s);
    std::set<EntityId> seen;
    for (EntityId e : s.entities()) {
        const Character2D* ch = s.get<Character2D>(e);
        if (!ch || !s.isActive(e)) continue;
        seen.insert(e);
        const Pose2D pose = poseOf(index.world(e));
        Hash2D h;
        h.pod(ch->height);
        h.pod(ch->radius);
        h.pod(ch->offset);
        h.str(ch->layer);
        h.str(ch->mask);
        CharEntry& c = chars[e];
        const bool fresh = B2_IS_NULL(c.id);
        c.settings = *ch;
        if (fresh || c.signature != h.h) {
            destroyCharacter(c);
            c.entity = e;
            c.signature = h.h;
            std::string unknown;
            b2Filter f = makeFilter(ch->layer, ch->mask, false, &unknown);
            if (!unknown.empty()) warn("'" + s.record(e)->name + "': " + unknown);
            c.category = f.categoryBits;
            c.mask = f.maskBits;
            b2BodyDef def = b2DefaultBodyDef();
            def.type = b2_kinematicBody;
            def.position = toB2(pose.position);
            def.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(e));
            def.enableSleep = false;
            c.id = b2CreateBody(world, &def);
            c.tag = std::make_unique<ShapeTag>();
            c.tag->entity = e;
            c.tag->owner = e;
            c.tag->character = true;
            c.tag->layer = f.categoryBits;
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.filter = f;
            sd.filter.categoryBits |= kCharacterBit;
            sd.enableSensorEvents = true;
            sd.enableContactEvents = true;
            sd.userData = c.tag.get();
            const float r = std::max(0.02f, ch->radius);
            const float half = std::max(0.f, ch->height * 0.5f - r);
            if (half < 2.f * kLinearSlop) {
                b2Circle circle{toB2(ch->offset), r};
                b2CreateCircleShape(c.id, &sd, &circle);
            } else {
                b2Capsule cap{toB2(ch->offset + Vec2{0.f, -half}), toB2(ch->offset + Vec2{0.f, half}), r};
                b2CreateCapsuleShape(c.id, &sd, &cap);
            }
            if (fresh) {
                c.position = pose.position;
                c.velocity = ch->velocity;
                c.lastVelocity = ch->velocity;
                c.lastPose = pose;
            }
        }
        c.z = pose.z;
        if (!samePose(pose, c.lastPose)) {  // teleported by something else
            c.position = pose.position;
            c.lastPose = pose;
            b2Body_SetTransform(c.id, toB2(c.position), b2Rot_identity);
        }
        if (ch->velocity != c.lastVelocity) {  // knockback, launch pads
            c.velocity = ch->velocity;
            c.lastVelocity = ch->velocity;
            if (c.velocity.y > 0.f) c.grounded = false;
        }
    }
    for (auto it = chars.begin(); it != chars.end();) {
        if (!seen.count(it->first)) {
            destroyCharacter(it->second);
            it = chars.erase(it);
        } else {
            ++it;
        }
    }
}

void Physics2DWorld::Impl::syncJoints(const Scene& s) {
    std::set<EntityId> seen;
    Index2D index(s);
    for (EntityId e : s.entities()) {
        const Joint2D* j = s.get<Joint2D>(e);
        if (!j || !s.isActive(e) || !j->enabled) continue;
        b2BodyId a = bodyFor(e);
        if (B2_IS_NULL(a)) {
            warn("'" + s.record(e)->name + "': joint2d needs a body2d (or collider2d) on the same entity");
            continue;
        }
        EntityId otherEntity = kNoEntity;
        b2BodyId b = ground;
        if (!j->other.empty()) {
            otherEntity = s.resolve(j->other, e);
            b2BodyId ob = otherEntity ? bodyFor(otherEntity) : b2_nullBodyId;
            if (B2_IS_NULL(ob)) {
                warn("'" + s.record(e)->name + "': joint2d other '" + (j->other.name.empty() ? std::to_string(j->other.id) : j->other.name) +
                     "' has no 2D body: the joint holds to the world");
                otherEntity = kNoEntity;
            } else {
                b = ob;
            }
        }
        if (B2_ID_EQUALS(a, b)) {
            warn("'" + s.record(e)->name + "': joint2d connects a body to itself");
            continue;
        }
        Hash2D h;
        h.reflected(j, Joint2D::type(), {"enabled", "target"});
        h.pod(a);
        h.pod(b);
        seen.insert(e);
        auto it = joints.find(e);
        if (it != joints.end() && it->second.signature == h.h && B2_IS_NON_NULL(it->second.id) && b2Joint_IsValid(it->second.id)) {
            JointEntry& je = it->second;
            if (je.kind == "target") {
                Vec2 target = j->target;
                if (je.follow) target = poseOf(index.world(je.follow)).position;
                b2MouseJoint_SetTarget(je.id, toB2(target));
            }
            continue;
        }
        if (it != joints.end()) destroyJoint(it->second);
        JointEntry je;
        je.entity = e;
        je.signature = h.h;
        je.kind = j->kind;
        je.breakForce = j->breakForce;
        // Body A (reference) = the other body or the world; body B = this entity's body, so motors move this
        // body: positive speeds turn it counter-clockwise (revolute) or slide it along +axis (prismatic).
        std::swap(a, b);
        const Mat4& mw = index.world(e);
        Vec3 wb = mw.transformPoint({j->anchor.x, j->anchor.y, 0.f});
        const b2Vec2 worldThis{wb.x, wb.y};
        b2Vec2 worldOther = toB2(j->otherAnchor);
        if (otherEntity) {
            Vec3 wo = index.world(otherEntity).transformPoint({j->otherAnchor.x, j->otherAnchor.y, 0.f});
            worldOther = {wo.x, wo.y};
        } else if (j->kind != "distance") {
            worldOther = worldThis;  // pinned to the world where it is
        }
        // Single-pivot joints share the anchor point on both bodies.
        const b2Vec2 localA = b2Body_GetLocalPoint(a, worldThis), localB = b2Body_GetLocalPoint(b, worldThis);
        Vec3 axisW = mw.transformDir({j->axis.x, j->axis.y, 0.f});
        const b2Vec2 axisA = b2Body_GetLocalVector(a, b2Normalize(b2Vec2{axisW.x, axisW.y}));
        const float refAngle = b2RelativeAngle(b2Body_GetRotation(b), b2Body_GetRotation(a));
        const bool limit = j->limitMin < j->limitMax;
        const bool motor = j->motorForce > 0.f;
        const bool spring = j->stiffness > 0.f;
        if (j->kind == "revolute") {
            b2RevoluteJointDef d = b2DefaultRevoluteJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = localA;
            d.localAnchorB = localB;
            d.referenceAngle = refAngle;
            d.enableLimit = limit;
            d.lowerAngle = j->limitMin * kDeg;
            d.upperAngle = j->limitMax * kDeg;
            d.enableMotor = motor;
            d.maxMotorTorque = j->motorForce;
            d.motorSpeed = j->motorSpeed * kDeg;
            d.enableSpring = spring;
            d.hertz = j->stiffness;
            d.dampingRatio = j->damping;
            d.collideConnected = j->collideConnected;
            je.id = b2CreateRevoluteJoint(world, &d);
        } else if (j->kind == "prismatic") {
            b2PrismaticJointDef d = b2DefaultPrismaticJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = localA;
            d.localAnchorB = localB;
            d.localAxisA = axisA;
            d.referenceAngle = refAngle;
            d.enableLimit = limit;
            d.lowerTranslation = j->limitMin;
            d.upperTranslation = j->limitMax;
            d.enableMotor = motor;
            d.maxMotorForce = j->motorForce;
            d.motorSpeed = j->motorSpeed;
            d.enableSpring = spring;
            d.hertz = j->stiffness;
            d.dampingRatio = j->damping;
            d.collideConnected = j->collideConnected;
            je.id = b2CreatePrismaticJoint(world, &d);
        } else if (j->kind == "distance") {
            b2DistanceJointDef d = b2DefaultDistanceJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = b2Body_GetLocalPoint(a, worldOther);
            d.localAnchorB = localB;
            const float current = b2Distance(worldThis, worldOther);
            d.length = j->length > 0.f ? j->length : std::max(current, 0.01f);
            d.enableLimit = limit;
            d.minLength = limit ? j->limitMin : 0.f;
            d.maxLength = limit ? j->limitMax : 1e6f;
            d.enableMotor = motor;
            d.maxMotorForce = j->motorForce;
            d.motorSpeed = j->motorSpeed;
            d.enableSpring = spring;
            d.hertz = j->stiffness;
            d.dampingRatio = j->damping;
            d.collideConnected = j->collideConnected;
            je.id = b2CreateDistanceJoint(world, &d);
        } else if (j->kind == "weld") {
            b2WeldJointDef d = b2DefaultWeldJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = localA;
            d.localAnchorB = localB;
            d.referenceAngle = refAngle;
            d.linearHertz = j->stiffness;
            d.angularHertz = j->stiffness;
            d.linearDampingRatio = j->damping;
            d.angularDampingRatio = j->damping;
            d.collideConnected = j->collideConnected;
            je.id = b2CreateWeldJoint(world, &d);
        } else if (j->kind == "wheel") {
            b2WheelJointDef d = b2DefaultWheelJointDef();
            d.bodyIdA = a;
            d.bodyIdB = b;
            d.localAnchorA = localA;
            d.localAnchorB = localB;
            d.localAxisA = axisA;
            d.enableSpring = true;
            d.hertz = spring ? j->stiffness : 4.f;
            d.dampingRatio = j->damping;
            d.enableLimit = limit;
            d.lowerTranslation = j->limitMin;
            d.upperTranslation = j->limitMax;
            d.enableMotor = motor;
            d.maxMotorTorque = j->motorForce;
            d.motorSpeed = j->motorSpeed * kDeg;
            d.collideConnected = j->collideConnected;
            je.id = b2CreateWheelJoint(world, &d);
        } else if (j->kind == "target") {
            b2MouseJointDef d = b2DefaultMouseJointDef();
            d.bodyIdA = ground;
            d.bodyIdB = b;
            je.follow = otherEntity;
            d.target = b2Body_GetWorldCenterOfMass(b);  // the grab point: the body's center
            d.hertz = spring ? j->stiffness : 5.f;
            d.dampingRatio = j->damping;
            d.maxForce = j->maxForce;
            d.collideConnected = true;
            je.id = b2CreateMouseJoint(world, &d);
            b2MouseJoint_SetTarget(je.id, otherEntity ? toB2(poseOf(index.world(otherEntity)).position) : toB2(j->target));
        } else {
            std::vector<std::string> kinds{"revolute", "prismatic", "distance", "weld", "wheel", "target"};
            std::string guess = str::closest(j->kind, kinds, 3);
            warn("'" + s.record(e)->name + "': unknown joint2d kind '" + j->kind + "'" + (guess.empty() ? "" : " - did you mean '" + guess + "'?"));
            continue;
        }
        b2Body_SetAwake(b, true);
        if (!B2_ID_EQUALS(a, ground)) b2Body_SetAwake(a, true);
        joints[e] = std::move(je);
    }
    for (auto it = joints.begin(); it != joints.end();) {
        if (!seen.count(it->first)) {
            destroyJoint(it->second);
            it = joints.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

void Physics2DWorld::Impl::step(Scene& s, float dt) {
    if (!settings.enabled || dt <= 0.f) return;
    moveCharacters(dt);
    b2World_Step(world, dt, std::clamp(settings.substeps, 1, 16));
    collectEvents();
    checkBrokenJoints(s);
    if (options.writeBack) {
        writeBack(s);
        writeBackCharacters(s);
    }
}

void Physics2DWorld::Impl::collectEvents() {
    auto tagOf = [](b2ShapeId id) -> const ShapeTag* {
        return b2Shape_IsValid(id) ? static_cast<const ShapeTag*>(b2Shape_GetUserData(id)) : nullptr;
    };
    std::vector<Event2D> out;
    auto both = [&](Event2D::Kind kind, const ShapeTag* a, const ShapeTag* b, Vec2 point, Vec2 normalAB, float speed, float impulse) {
        if (!a || !b || a->owner == b->owner) return;
        out.push_back({kind, a->owner, b->owner, point, normalAB * -1.f, speed, impulse});
        out.push_back({kind, b->owner, a->owner, point, normalAB, speed, impulse});
    };
    b2ContactEvents ce = b2World_GetContactEvents(world);
    // Approach speeds come from hit events (begin events carry no pre-solve velocity).
    std::map<std::pair<EntityId, EntityId>, float> hitSpeed;
    for (int i = 0; i < ce.hitCount; ++i) {
        const ShapeTag* a = tagOf(ce.hitEvents[i].shapeIdA);
        const ShapeTag* b = tagOf(ce.hitEvents[i].shapeIdB);
        if (!a || !b) continue;
        float& sp = hitSpeed[{std::min(a->owner, b->owner), std::max(a->owner, b->owner)}];
        sp = std::max(sp, ce.hitEvents[i].approachSpeed);
    }
    for (int i = 0; i < ce.beginCount; ++i) {
        const b2ContactBeginTouchEvent& ev = ce.beginEvents[i];
        const b2Manifold& m = ev.manifold;
        Vec2 point{};
        float approach = 0.f;
        for (int k = 0; k < m.pointCount; ++k) {
            point = point + fromB2(m.points[k].point) * (1.f / static_cast<float>(m.pointCount));
            approach = std::max(approach, -m.points[k].normalVelocity);
        }
        if (const ShapeTag* a = tagOf(ev.shapeIdA), *b = tagOf(ev.shapeIdB); a && b) {
            auto it = hitSpeed.find({std::min(a->owner, b->owner), std::max(a->owner, b->owner)});
            if (it != hitSpeed.end()) approach = std::max(approach, it->second);
        }
        both(Event2D::Kind::Begin, tagOf(ev.shapeIdA), tagOf(ev.shapeIdB), point, fromB2(m.normal), approach, 0.f);
    }
    for (int i = 0; i < ce.endCount; ++i) {
        const b2ContactEndTouchEvent& ev = ce.endEvents[i];
        both(Event2D::Kind::End, tagOf(ev.shapeIdA), tagOf(ev.shapeIdB), {}, {}, 0.f, 0.f);
    }
    for (int i = 0; i < ce.hitCount; ++i) {
        const b2ContactHitEvent& ev = ce.hitEvents[i];
        if (ev.approachSpeed < settings.impactSpeed) continue;
        auto massOf = [](b2ShapeId id) {
            b2BodyId b = b2Shape_GetBody(id);
            return b2Body_GetType(b) == b2_dynamicBody ? b2Body_GetMass(b) : 0.f;
        };
        const float ma = massOf(ev.shapeIdA), mb = massOf(ev.shapeIdB);
        const float reduced = ma > 0.f && mb > 0.f ? ma * mb / (ma + mb) : std::max(ma, mb);
        both(Event2D::Kind::Impact, tagOf(ev.shapeIdA), tagOf(ev.shapeIdB), fromB2(ev.point), fromB2(ev.normal), ev.approachSpeed,
             ev.approachSpeed * reduced);
    }
    b2SensorEvents se = b2World_GetSensorEvents(world);
    for (int i = 0; i < se.beginCount; ++i) {
        const ShapeTag* sensor = tagOf(se.beginEvents[i].sensorShapeId);
        const ShapeTag* visitor = tagOf(se.beginEvents[i].visitorShapeId);
        if (sensor && visitor && sensor->owner != visitor->owner) out.push_back({Event2D::Kind::SensorEnter, sensor->owner, visitor->owner, {}, {}, 0.f, 0.f});
    }
    for (int i = 0; i < se.endCount; ++i) {
        const ShapeTag* sensor = tagOf(se.endEvents[i].sensorShapeId);
        const ShapeTag* visitor = tagOf(se.endEvents[i].visitorShapeId);
        if (sensor && visitor && sensor->owner != visitor->owner) out.push_back({Event2D::Kind::SensorExit, sensor->owner, visitor->owner, {}, {}, 0.f, 0.f});
    }
    // Several shapes of one body pair report one event; order is (kind, self, other).
    std::stable_sort(out.begin(), out.end(), [](const Event2D& a, const Event2D& b) {
        return std::tie(a.kind, a.self, a.other) < std::tie(b.kind, b.self, b.other);
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const Event2D& a, const Event2D& b) { return a.kind == b.kind && a.self == b.self && a.other == b.other; }),
              out.end());
    events.insert(events.end(), out.begin(), out.end());
    if (events.size() > 4096) events.erase(events.begin(), events.end() - 4096);
}

void Physics2DWorld::Impl::checkBrokenJoints(Scene& s) {
    for (auto& [e, je] : joints) {
        if (je.breakForce <= 0.f || B2_IS_NULL(je.id) || !b2Joint_IsValid(je.id)) continue;
        if (len2(fromB2(b2Joint_GetConstraintForce(je.id))) <= je.breakForce) continue;
        b2Joint_WakeBodies(je.id);
        destroyJoint(je);
        if (Joint2D* j = s.get<Joint2D>(e); j && options.writeBack) j->enabled = false;
        broken.push_back(e);
    }
}

void Physics2DWorld::Impl::writeBack(Scene& s) {
    std::vector<BodyEntry*> order;
    for (auto& [e, b] : bodies) {
        if (b.type == b2_dynamicBody || (b.type == b2_kinematicBody && b.velocityDriven)) order.push_back(&b);
    }
    std::stable_sort(order.begin(), order.end(), [](const BodyEntry* x, const BodyEntry* y) { return x->depth < y->depth; });
    bool moved = false;
    for (BodyEntry* b : order) {
        Body2D* rb = s.get<Body2D>(b->entity);
        Transform* t = s.get<Transform>(b->entity);
        const bool awake = b2Body_IsAwake(b->id);
        if (awake && t) {
            const b2Transform xf = b2Body_GetTransform(b->id);
            const Vec2 p = fromB2(xf.p);
            const float angle = b2Rot_GetAngle(xf.q);
            const EntityRecord* r = s.record(b->entity);
            if (r && r->parent && s.exists(r->parent)) {
                const Mat4 parent = s.worldMatrix(r->parent);
                const Pose2D pp = poseOf(parent);
                Vec3 local = parent.inverse().transformPoint({p.x, p.y, pp.z + t->position.z});
                t->position.x = local.x;
                t->position.y = local.y;
                t->rotation.z = (angle - pp.angle) / kDeg;
            } else {
                t->position.x = p.x;
                t->position.y = p.y;
                t->rotation.z = angle / kDeg;
            }
            b->lastPose.position = p;
            b->lastPose.angle = angle;
            moved = true;
        }
        if (rb) {
            rb->velocity = fromB2(b2Body_GetLinearVelocity(b->id));
            rb->angularVelocity = b2Body_GetAngularVelocity(b->id) / kDeg;
            rb->sleeping = !awake;
            b->lastVelocity = rb->velocity;
            b->lastAngular = rb->angularVelocity;
        }
    }
    if (moved) s.markDirty();
}

// ---------------------------------------------------------------------------
// Physics2DWorld
// ---------------------------------------------------------------------------

Physics2DWorld::Physics2DWorld(TileCollisionProvider tiles, WorldOptions2D options)
    : impl_(std::make_unique<Impl>(std::move(tiles), std::move(options))) {}

Physics2DWorld::~Physics2DWorld() = default;

void Physics2DWorld::sync(const Scene& scene, float dt) { impl_->sync(scene, dt); }

void Physics2DWorld::step(Scene& scene, float dt) { impl_->step(scene, dt); }

std::vector<Event2D> Physics2DWorld::drainEvents() { return std::exchange(impl_->events, {}); }

std::vector<EntityId> Physics2DWorld::drainBrokenJoints() { return std::exchange(impl_->broken, {}); }

std::vector<std::string> Physics2DWorld::drainWarnings() { return std::exchange(impl_->warnings, {}); }

namespace {
const BodyEntry* simulated(const Physics2DWorld::Impl& impl, EntityId e) {
    auto it = impl.bodies.find(e);
    if (it == impl.bodies.end() || B2_IS_NULL(it->second.id)) return nullptr;
    return &it->second;
}
}  // namespace

bool Physics2DWorld::addForce(EntityId e, Vec2 force) {
    const BodyEntry* b = simulated(*impl_, e);
    if (!b || b->type != b2_dynamicBody) return false;
    b2Body_ApplyForceToCenter(b->id, toB2(force), true);
    return true;
}

bool Physics2DWorld::addImpulse(EntityId e, Vec2 impulse) {
    const BodyEntry* b = simulated(*impl_, e);
    if (!b || b->type != b2_dynamicBody) return false;
    b2Body_ApplyLinearImpulseToCenter(b->id, toB2(impulse), true);
    return true;
}

bool Physics2DWorld::addTorque(EntityId e, float torque) {
    const BodyEntry* b = simulated(*impl_, e);
    if (!b || b->type != b2_dynamicBody) return false;
    b2Body_ApplyTorque(b->id, torque, true);
    return true;
}

bool Physics2DWorld::setVelocity(EntityId e, Vec2 v) {
    if (const BodyEntry* b = simulated(*impl_, e); b && b->type != b2_staticBody) {
        b2Body_SetLinearVelocity(b->id, toB2(v));
        b2Body_SetAwake(b->id, true);
        return true;
    }
    if (auto it = impl_->chars.find(e); it != impl_->chars.end()) {
        it->second.velocity = v;
        if (v.y > 0.f) it->second.grounded = false;
        return true;
    }
    return false;
}

std::optional<Vec2> Physics2DWorld::velocity(EntityId e) const {
    if (const BodyEntry* b = simulated(*impl_, e)) return fromB2(b2Body_GetLinearVelocity(b->id));
    if (auto it = impl_->chars.find(e); it != impl_->chars.end()) return it->second.velocity;
    return std::nullopt;
}

bool Physics2DWorld::hasBody(EntityId e) const { return simulated(*impl_, e) != nullptr; }

bool Physics2DWorld::isSleeping(EntityId e) const {
    const BodyEntry* b = simulated(*impl_, e);
    return b && !b2Body_IsAwake(b->id);
}

bool Physics2DWorld::allAsleep() const {
    for (const auto& [e, b] : impl_->bodies) {
        if (b.type == b2_dynamicBody && b2Body_IsAwake(b.id)) return false;
    }
    return true;
}

std::optional<Transform> Physics2DWorld::localTransform(const Scene& scene, EntityId e) const {
    const Transform* t = scene.get<Transform>(e);
    if (!t) return std::nullopt;
    Transform out = *t;
    Vec2 p;
    float angle = 0.f;
    if (const BodyEntry* b = simulated(*impl_, e)) {
        const b2Transform xf = b2Body_GetTransform(b->id);
        p = fromB2(xf.p);
        angle = b2Rot_GetAngle(xf.q);
    } else if (auto it = impl_->chars.find(e); it != impl_->chars.end()) {
        p = it->second.position;
        angle = poseOf(scene.worldMatrix(e)).angle;
    } else {
        return std::nullopt;
    }
    const EntityRecord* r = scene.record(e);
    if (r && r->parent && scene.exists(r->parent)) {
        const Mat4 parent = scene.worldMatrix(r->parent);
        const Pose2D pp = poseOf(parent);
        Vec3 local = parent.inverse().transformPoint({p.x, p.y, pp.z + t->position.z});
        out.position.x = local.x;
        out.position.y = local.y;
        out.rotation.z = (angle - pp.angle) / kDeg;
    } else {
        out.position.x = p.x;
        out.position.y = p.y;
        out.rotation.z = angle / kDeg;
    }
    return out;
}

std::optional<Physics2DWorld::TilemapInfo> Physics2DWorld::tilemapInfo(EntityId e) const {
    auto it = impl_->bodies.find(e);
    if (it == impl_->bodies.end()) return std::nullopt;
    return it->second.tilemap;
}

}  // namespace sky::physics2d
