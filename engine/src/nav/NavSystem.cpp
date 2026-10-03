#include "skywalker/nav/NavSystem.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "skywalker/core/Log.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/render/MeshData.h"

namespace sky::nav {

namespace {

constexpr int kMaxAgents = 512;

const NavMeshSurface* surfaceOf(const Scene& s) {
    for (EntityId e : s.entities()) {
        if (const NavMeshSurface* n = s.get<NavMeshSurface>(e); n && s.isActive(e)) return n;
    }
    return nullptr;
}

bool tagged(const Scene& s, EntityId e, const char* tag) {
    const EntityRecord* r = s.record(e);
    return r && std::find(r->tags.begin(), r->tags.end(), tag) != r->tags.end();
}

/// Moves at runtime: dynamic/kinematic bodies, characters, agents — and anything under them.
bool movesAtRuntime(const Scene& s, EntityId e) {
    for (const EntityRecord* r = s.record(e); r; r = r->parent ? s.record(r->parent) : nullptr) {
        if (const RigidBody* rb = s.get<RigidBody>(r->id); rb && rb->motion != "static") return true;
        if (s.get<CharacterController>(r->id) || s.get<NavAgent>(r->id)) return true;
    }
    return false;
}

bool partOfBody(const Scene& s, EntityId e) {
    for (const EntityRecord* r = s.record(e); r; r = r->parent ? s.record(r->parent) : nullptr) {
        if (s.get<RigidBody>(r->id)) return true;
    }
    return false;
}

uint64_t agentSignature(const NavAgent& a) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    mix(&a.speed, sizeof(float));
    mix(&a.acceleration, sizeof(float));
    mix(&a.radius, sizeof(float));
    mix(&a.height, sizeof(float));
    mix(a.avoidance.data(), a.avoidance.size());
    return h;
}

dtCrowdAgentParams paramsFor(const NavAgent& a) {
    dtCrowdAgentParams p{};
    p.radius = std::max(a.radius, 0.05f);
    p.height = std::max(a.height, 0.1f);
    p.maxAcceleration = std::max(a.acceleration, 0.01f);
    p.maxSpeed = std::max(a.speed, 0.f);
    p.collisionQueryRange = p.radius * 12.f;
    p.pathOptimizationRange = p.radius * 30.f;
    p.separationWeight = 2.f;
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO | DT_CROWD_SEPARATION;
    if (a.avoidance != "none") p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
    p.obstacleAvoidanceType = a.avoidance == "low" ? 0 : a.avoidance == "high" ? 3 : 1;
    return p;
}

float horizontalDistance(Vec3 a, Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

}  // namespace

BuildSettings settingsFrom(const NavMeshSurface* n) {
    BuildSettings s;
    if (!n) return s;
    s.agentRadius = n->agentRadius;
    s.agentHeight = n->agentHeight;
    s.agentMaxClimb = n->maxClimb;
    s.agentMaxSlope = n->maxSlope;
    s.cellSize = n->cellSize;
    s.cellHeight = n->cellHeight;
    s.tileSize = n->tileSize;
    return s;
}

NavSystem::NavSystem(Scene& scene, physics::PhysicsSystem& physics, physics::PathResolver paths)
    : scene_(scene), physics_(physics), paths_(std::move(paths)) {}

NavSystem::~NavSystem() { dtFreeCrowd(crowd_); }

// ---------------------------------------------------------------------------
// Geometry & baking
// ---------------------------------------------------------------------------

void NavSystem::collectGeometry(const BuildSettings&, const std::string& mode, std::vector<Vec3>& tris,
                                std::vector<EntityId>& owners) {
    if (mode != "meshes") {
        std::vector<Vec3> ct;
        std::vector<EntityId> co;
        physics_.queryWorld().staticTriangles(ct, co);
        for (size_t t = 0; t < co.size(); ++t) {
            if (tagged(scene_, co[t], "nav_ignore")) continue;
            tris.insert(tris.end(), {ct[t * 3], ct[t * 3 + 1], ct[t * 3 + 2]});
            owners.push_back(co[t]);
        }
    }
    if (mode == "colliders") return;
    for (EntityId e : scene_.entities()) {
        const MeshRenderer* m = scene_.get<MeshRenderer>(e);
        if (!m || !m->visible || m->shading == "water" || !scene_.isActive(e) || tagged(scene_, e, "nav_ignore")) continue;
        if (movesAtRuntime(scene_, e)) continue;
        const Collider* col = scene_.get<Collider>(e);
        if (col && col->isTrigger) continue;
        if (mode == "both" && (col || partOfBody(scene_, e))) continue;  // its collider already contributed
        const MeshData* data = physics_.meshes() ? physics_.meshes()(m->mesh) : nullptr;
        if (!data) continue;
        Mat4 w = scene_.worldMatrix(e);
        bool mirrored = dot(cross(Vec3{w.at(0, 0), w.at(0, 1), w.at(0, 2)}, Vec3{w.at(1, 0), w.at(1, 1), w.at(1, 2)}),
                            Vec3{w.at(2, 0), w.at(2, 1), w.at(2, 2)}) < 0;
        auto vert = [&](uint32_t i) {
            const float* v = &data->vertices[static_cast<size_t>(i) * MeshData::kFloatsPerVertex];
            return w.transformPoint({v[0], v[1], v[2]});
        };
        for (size_t i = 0; i + 2 < data->indices.size(); i += 3) {
            uint32_t a = data->indices[i], b = data->indices[i + 1], c = data->indices[i + 2];
            if (a >= data->vertexCount() || b >= data->vertexCount() || c >= data->vertexCount()) continue;
            if (mirrored) std::swap(b, c);
            tris.insert(tris.end(), {vert(a), vert(b), vert(c)});
            owners.push_back(e);
        }
        // Double-sided thin meshes (planes, quads) are walkable from above only; Recast handles that.
    }
}

Result<BuildReport> NavSystem::build() {
    const NavMeshSurface* surf = surfaceOf(scene_);
    return build(settingsFrom(surf), surf ? surf->geometry : "both");
}

Result<BuildReport> NavSystem::build(const BuildSettings& bs, const std::string& geometry) {
    std::vector<Vec3> tris;
    std::vector<EntityId> owners;
    collectGeometry(bs, geometry, tris, owners);
    auto m = std::make_unique<NavMesh>();
    auto r = m->build(tris, bs);
    if (!r) {
        lastError_ = r.error().message;
        return r.error();
    }
    if (crowd_) {  // the crowd points into the old mesh: restart it (agents rejoin on the next update)
        agents_.clear();
        dtFreeCrowd(crowd_);
        crowd_ = nullptr;
        crowdMaxRadius_ = 0;
    }
    mesh_ = std::move(m);
    checkedRevision_ = scene_.revision();
    if (playing_) playMeshReady_ = true;
    lastError_.clear();
    return r;
}

NavMesh* NavSystem::mesh() {
    if (playing_ && playMeshReady_) return mesh_ && mesh_->valid() ? mesh_.get() : nullptr;
    if (!playing_ && mesh_ && checkedRevision_ == scene_.revision()) return mesh_->valid() ? mesh_.get() : nullptr;
    const NavMeshSurface* surf = surfaceOf(scene_);
    BuildSettings bs = settingsFrom(surf);
    std::vector<Vec3> tris;
    std::vector<EntityId> owners;
    collectGeometry(bs, surf ? surf->geometry : "both", tris, owners);
    uint64_t hash = NavMesh::hashInput(tris, bs);
    checkedRevision_ = scene_.revision();
    if (playing_) playMeshReady_ = true;
    if (mesh_ && mesh_->valid() && mesh_->sourceHash() == hash) return mesh_.get();

    if (surf && !surf->data.empty()) {
        auto loaded = std::make_unique<NavMesh>();
        if (Status st = loaded->load(paths_(surf->data)); st) {
            if (loaded->sourceHash() == hash || !surf->autoBuild) {
                if (loaded->sourceHash() != hash) {
                    log::warn("nav", "the saved navmesh " + surf->data + " is stale (the level changed); run nav_build");
                }
                mesh_ = std::move(loaded);
                lastError_.clear();
                return mesh_.get();
            }
            log::info("nav", "the saved navmesh " + surf->data + " is stale; rebuilding in memory (run nav_build to save)");
        } else if (!surf->autoBuild) {
            lastError_ = st.error().message;
            mesh_.reset();
            return nullptr;
        }
    }
    auto m = std::make_unique<NavMesh>();
    auto r = m->build(tris, bs);
    if (!r) {
        lastError_ = r.error().message + (r.error().hint.empty() ? "" : " (" + r.error().hint + ")");
        mesh_.reset();
        return nullptr;
    }
    lastError_.clear();
    mesh_ = std::move(m);
    return mesh_.get();
}

// ---------------------------------------------------------------------------
// Play: crowd
// ---------------------------------------------------------------------------

void NavSystem::beginPlay() {
    endPlay();
    playing_ = true;
    playMeshReady_ = false;
}

void NavSystem::endPlay() {
    agents_.clear();
    follows_.clear();
    dtFreeCrowd(crowd_);
    crowd_ = nullptr;
    crowdMaxRadius_ = 0;
    reportedError_.clear();
    playing_ = false;
    playMeshReady_ = false;
    checkedRevision_ = ~0ull;
}

bool NavSystem::ensureCrowd() {
    float maxRadius = 0;
    bool any = false;
    for (EntityId e : scene_.entities()) {
        if (const NavAgent* a = scene_.get<NavAgent>(e); a && scene_.isActive(e)) {
            maxRadius = std::max(maxRadius, a->radius);
            any = true;
        }
    }
    if (!any && !crowd_) return false;
    if (crowd_ && maxRadius <= crowdMaxRadius_) return true;
    NavMesh* nm = mesh();
    if (!nm) {
        if (reportedError_ != lastError_) {
            reportedError_ = lastError_;
            log::warn("nav", "nav agents cannot move: " + lastError_);
        }
        return false;
    }
    // (Re)create the crowd; existing agents are re-added at their current positions.
    agents_.clear();
    dtFreeCrowd(crowd_);
    crowd_ = dtAllocCrowd();
    crowdMaxRadius_ = std::max(maxRadius, 1.f);
    if (!crowd_ || !crowd_->init(kMaxAgents, crowdMaxRadius_, nm->detour())) {
        dtFreeCrowd(crowd_);
        crowd_ = nullptr;
        return false;
    }
    dtObstacleAvoidanceParams p;
    std::memcpy(&p, crowd_->getObstacleAvoidanceParams(0), sizeof(p));
    const unsigned char quality[4][3] = {{5, 2, 1}, {5, 2, 2}, {7, 2, 3}, {7, 3, 3}};
    for (int i = 0; i < 4; ++i) {
        p.velBias = 0.5f;
        p.adaptiveDivs = quality[i][0];
        p.adaptiveRings = quality[i][1];
        p.adaptiveDepth = quality[i][2];
        crowd_->setObstacleAvoidanceParams(i, &p);
    }
    return true;
}

void NavSystem::addAgent(EntityId e, const NavAgent& a, uint64_t sig) {
    Agent ag;
    ag.signature = sig;
    Vec3 pos = scene_.worldMatrix(e).translation();
    Vec3 feet = pos;
    if (const CharacterController* c = scene_.get<CharacterController>(e)) {
        feet = pos + c->offset - Vec3{0.f, c->height * 0.5f, 0.f};
    }
    NavMesh* nm = mesh();
    auto onMesh = nm ? nm->nearestPoint(feet, {a.radius * 2.f + 1.f, a.height + 2.f, a.radius * 2.f + 1.f}) : std::nullopt;
    Vec3 start = onMesh ? *onMesh : feet;
    ag.yOffset = pos.y - start.y;
    dtCrowdAgentParams params = paramsFor(a);
    float p[3] = {start.x, start.y, start.z};
    ag.index = crowd_->addAgent(p, &params);
    if (ag.index < 0) {
        log::warn("nav", "too many nav agents (limit " + std::to_string(kMaxAgents) + "); #" + std::to_string(e) + " is idle");
        return;
    }
    ag.lastPosition = pos;
    agents_[e] = ag;
}

void NavSystem::removeAgent(std::map<EntityId, Agent>::iterator it) {
    if (crowd_ && it->second.index >= 0) crowd_->removeAgent(it->second.index);
    agents_.erase(it);
}

void NavSystem::syncAgents() {
    for (auto it = agents_.begin(); it != agents_.end();) {
        const NavAgent* a = scene_.get<NavAgent>(it->first);
        if (!a || !scene_.isActive(it->first)) {
            auto next = std::next(it);
            removeAgent(it);
            it = next;
        } else {
            ++it;
        }
    }
    for (EntityId e : scene_.entities()) {
        const NavAgent* a = scene_.get<NavAgent>(e);
        if (!a || !scene_.isActive(e)) continue;
        uint64_t sig = agentSignature(*a);
        auto it = agents_.find(e);
        if (it == agents_.end()) {
            addAgent(e, *a, sig);
            continue;
        }
        Agent& ag = it->second;
        if (ag.signature != sig) {
            dtCrowdAgentParams params = paramsFor(*a);
            crowd_->updateAgentParameters(ag.index, &params);
            ag.signature = sig;
        }
        // Teleported by a script or an agent: restart the crowd agent there (characters are fed in afterPhysics).
        if (!scene_.get<CharacterController>(e)) {
            Vec3 pos = scene_.worldMatrix(e).translation();
            if (distance(pos, ag.lastPosition) > 1e-3f) {
                Agent keep = ag;
                removeAgent(it);
                addAgent(e, *a, sig);
                if (auto again = agents_.find(e); again != agents_.end() && keep.hasTarget) {
                    requestTarget(again->second, keep.target);
                }
            }
        }
    }
}

bool NavSystem::requestTarget(Agent& ag, Vec3 target) {
    ag.target = target;
    ag.hasTarget = true;
    ag.arrived = false;
    ag.stuckTicks = 0;
    NavMesh* nm = mesh();
    if (!nm || !crowd_) return false;
    dtQueryFilter filter;
    float pos[3] = {target.x, target.y, target.z}, ext[3] = {4.f, 8.f, 4.f}, nearest[3];
    dtPolyRef ref = 0;
    nm->query()->findNearestPoly(pos, ext, &filter, &ref, nearest);
    if (!ref) return false;
    crowd_->requestMoveTarget(ag.index, ref, nearest);
    return true;
}

std::vector<EntityId> NavSystem::update(float dt) {
    std::vector<EntityId> arrivedNow;
    if (!ensureCrowd()) return arrivedNow;
    syncAgents();
    for (auto& [e, ag] : agents_) {
        NavAgent* a = scene_.get<NavAgent>(e);
        if (!a) continue;
        if (auto f = follows_.find(e); f != follows_.end() && a->navigating) {
            if (!scene_.exists(f->second)) {
                follows_.erase(f);
            } else {
                Vec3 fp = scene_.worldMatrix(f->second).translation();
                if (a->autoRepath && distance(fp, a->destination) > 0.5f) a->destination = fp;
            }
        }
        if (a->navigating) {
            if (!ag.hasTarget || distance(ag.target, a->destination) > 0.01f) {
                if (!requestTarget(ag, a->destination)) {
                    log::warn("nav", "#" + std::to_string(e) + " cannot navigate: the destination is not near the navmesh");
                    ag.hasTarget = false;
                    a->navigating = false;
                    follows_.erase(e);
                }
            }
        } else if (ag.hasTarget) {
            crowd_->resetMoveTarget(ag.index);
            ag.hasTarget = false;
            follows_.erase(e);
        }
    }
    crowd_->update(dt, nullptr);
    physics::PhysicsWorld* world = physics_.playWorld();
    for (auto& [e, ag] : agents_) {
        NavAgent* a = scene_.get<NavAgent>(e);
        const dtCrowdAgent* ca = crowd_->getAgent(ag.index);
        if (!a || !ca || !ca->active) continue;
        Vec3 pos{ca->npos[0], ca->npos[1], ca->npos[2]};
        Vec3 vel{ca->vel[0], ca->vel[1], ca->vel[2]};
        const CharacterController* character = scene_.get<CharacterController>(e);
        if (character && world && world->hasCharacter(e)) {
            if (ag.hasTarget) world->setDesiredVelocity(e, vel);
        } else if (Transform* t = scene_.get<Transform>(e)) {
            Vec3 worldPos = pos + Vec3{0.f, ag.yOffset, 0.f};
            const EntityRecord* r = scene_.record(e);
            t->position = r->parent ? scene_.worldMatrix(r->parent).inverse().transformPoint(worldPos) : worldPos;
            if (a->turnSpeed > 0 && std::sqrt(vel.x * vel.x + vel.z * vel.z) > 0.1f) {
                float target = degrees(std::atan2(-vel.x, -vel.z));
                float delta = std::remainder(target - t->rotation.y, 360.f);
                float maxTurn = a->turnSpeed * dt;
                t->rotation.y = std::remainder(t->rotation.y + std::clamp(delta, -maxTurn, maxTurn), 360.f);
            }
            ag.lastPosition = scene_.worldMatrix(e).translation();
            scene_.markDirty();
        }
        if (!ag.hasTarget) continue;
        // Arrival: within the stopping distance of the goal, or stopped at the end of a partial path.
        float tolerance = std::max(a->stoppingDistance, 0.05f) + 0.05f;
        bool there = horizontalDistance(pos, ag.target) <= tolerance && std::fabs(pos.y - ag.target.y) < a->height;
        float speed = length(vel);
        if (!there && ca->targetState == DT_CROWDAGENT_TARGET_VALID && ca->ncorners == 0 && speed < 0.05f) {
            there = ++ag.stuckTicks > 30;  // reached the closest reachable point
        } else {
            ag.stuckTicks = 0;
        }
        if (there) {
            crowd_->resetMoveTarget(ag.index);
            ag.hasTarget = false;
            ag.arrived = true;
            follows_.erase(e);
            a->navigating = false;
            arrivedNow.push_back(e);
        }
    }
    return arrivedNow;
}

void NavSystem::afterPhysics() {
    if (!crowd_) return;
    physics::PhysicsWorld* world = physics_.playWorld();
    for (auto& [e, ag] : agents_) {
        const CharacterController* c = scene_.get<CharacterController>(e);
        if (!c || !world || !world->hasCharacter(e)) continue;
        dtCrowdAgent* ca = crowd_->getEditableAgent(ag.index);
        if (!ca || !ca->active) continue;
        Vec3 feet = scene_.worldMatrix(e).translation() + c->offset - Vec3{0.f, c->height * 0.5f, 0.f};
        ca->npos[0] = feet.x;
        ca->npos[1] = feet.y;
        ca->npos[2] = feet.z;
    }
}

bool NavSystem::navigate(EntityId e, Vec3 target, EntityId follow) {
    NavAgent* a = scene_.get<NavAgent>(e);
    if (!a) return false;
    a->destination = target;
    a->navigating = true;
    if (follow) follows_[e] = follow;
    else follows_.erase(e);
    if (auto it = agents_.find(e); it != agents_.end()) it->second.arrived = false;
    return true;
}

bool NavSystem::stop(EntityId e) {
    NavAgent* a = scene_.get<NavAgent>(e);
    if (!a) return false;
    a->navigating = false;
    if (auto it = agents_.find(e); it != agents_.end()) {
        if (crowd_ && it->second.hasTarget) crowd_->resetMoveTarget(it->second.index);
        it->second.hasTarget = false;
    }
    follows_.erase(e);
    return true;
}

std::optional<bool> NavSystem::arrived(EntityId e) const {
    const NavAgent* a = scene_.get<NavAgent>(e);
    if (!a) return std::nullopt;
    auto it = agents_.find(e);
    return it != agents_.end() && it->second.arrived && !a->navigating;
}

std::optional<float> NavSystem::pathLength(Vec3 from, Vec3 to) {
    NavMesh* nm = mesh();
    if (!nm) return std::nullopt;
    PathResult p = nm->findPath(from, to);
    if (!p.found || p.partial) return std::nullopt;
    return p.length;
}

std::vector<Vec3> NavSystem::agentPath(EntityId e) {
    std::vector<Vec3> out;
    auto it = agents_.find(e);
    if (!crowd_ || it == agents_.end()) return out;
    const dtCrowdAgent* ca = crowd_->getAgent(it->second.index);
    if (!ca || !ca->active || !it->second.hasTarget) return out;
    out.push_back({ca->npos[0], ca->npos[1], ca->npos[2]});
    NavMesh* nm = mesh();
    if (!nm) return out;
    PathResult p = nm->findPath(out.front(), it->second.target);
    for (size_t i = 1; i < p.points.size(); ++i) out.push_back(p.points[i]);
    return out;
}

}  // namespace sky::nav
