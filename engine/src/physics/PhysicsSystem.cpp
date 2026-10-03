#include "skywalker/physics/PhysicsSystem.h"

#include <algorithm>

#include "skywalker/nav/NavSystem.h"
#include "skywalker/wander/Runtime.h"

namespace sky::physics {

PhysicsSystem::PhysicsSystem(Scene& scene, MeshProvider meshes, PathResolver paths)
    : scene_(scene), meshes_(std::move(meshes)), paths_(std::move(paths)), cache_(makeShapeCache()) {}

PhysicsSystem::~PhysicsSystem() = default;

std::unique_ptr<PhysicsWorld> PhysicsSystem::makeWorld(WorldOptions options) const {
    options.shapeCache = cache_;
    return std::make_unique<PhysicsWorld>(meshes_, paths_, std::move(options));
}

void PhysicsSystem::beginPlay() {
    play_.reset();
    playing_ = true;
}

void PhysicsSystem::endPlay() {
    if (play_) collectWarnings(*play_);
    play_.reset();
    playing_ = false;
    editRevision_ = ~0ull;  // the scene is restored from the play snapshot
}

PhysicsWorld& PhysicsSystem::ensurePlayWorld() {
    if (!play_) {
        play_ = makeWorld({});
        play_->sync(scene_, 0.f);
    }
    return *play_;
}

PhysicsWorld& PhysicsSystem::queryWorld() {
    if (playing_) return ensurePlayWorld();
    if (!edit_) {
        WorldOptions o;
        o.multithreaded = false;
        o.writeBack = false;
        edit_ = makeWorld(std::move(o));
    }
    if (editRevision_ != scene_.revision()) {
        edit_->sync(scene_, 0.f);
        editRevision_ = scene_.revision();
        collectWarnings(*edit_);
    }
    return *edit_;
}

void PhysicsSystem::collectWarnings(PhysicsWorld& world) {
    for (auto& w : world.drainWarnings()) {
        if (std::find(warnings_.begin(), warnings_.end(), w) != warnings_.end()) continue;
        warnings_.push_back(std::move(w));
        while (warnings_.size() > 50) warnings_.pop_front();
    }
}

void PhysicsSystem::step(float dt, wander::Runtime& runtime) {
    PhysicsWorld& world = ensurePlayWorld();
    world.sync(scene_, dt);
    if (nav_) {
        for (EntityId e : nav_->update(dt)) runtime.emit("arrived", e);
    }
    world.step(scene_, dt);
    if (nav_) nav_->afterPhysics();
    for (const ContactEvent& ev : world.drainEvents()) {
        wander::Runtime::Contact c;
        c.trigger = ev.kind == ContactEvent::Kind::Collide        ? wander::Trigger::Collide
                    : ev.kind == ContactEvent::Kind::TriggerEnter ? wander::Trigger::TriggerEnter
                                                                  : wander::Trigger::TriggerExit;
        c.self = ev.self;
        c.other = ev.other;
        c.point = ev.point;
        c.normal = ev.normal;
        c.speed = ev.speed;
        runtime.queueContact(c);
    }
    for (EntityId e : world.drainBrokenJoints()) runtime.emit("joint_broken", e);
    collectWarnings(world);
}

// ---------------------------------------------------------------------------
// physics builtins (wander::PhysicsHooks)
// ---------------------------------------------------------------------------

namespace {

/// The play world, synced first when `e` has the component but is not simulated yet (spawned this tick).
template <typename Component, typename Has>
PhysicsWorld* liveWorld(PhysicsSystem& sys, Scene& scene, EntityId e, Has has) {
    PhysicsWorld* w = sys.playing() ? &sys.queryWorld() : nullptr;
    if (w && !has(*w) && scene.get<Component>(e)) w->sync(scene, 0.f);
    return w;
}

}  // namespace

bool PhysicsSystem::addForce(EntityId e, Vec3 force) {
    auto* w = liveWorld<RigidBody>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasBody(e); });
    return w && w->addForce(e, force);
}

bool PhysicsSystem::addImpulse(EntityId e, Vec3 impulse) {
    auto* w = liveWorld<RigidBody>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasBody(e); });
    return w && w->addImpulse(e, impulse);
}

bool PhysicsSystem::addTorque(EntityId e, Vec3 torque) {
    auto* w = liveWorld<RigidBody>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasBody(e); });
    return w && w->addTorque(e, torque);
}

std::optional<Vec3> PhysicsSystem::velocity(EntityId e) {
    if (!playing_) {
        if (const RigidBody* rb = scene_.get<RigidBody>(e)) return rb->velocity;
        if (const CharacterController* c = scene_.get<CharacterController>(e)) return c->velocity;
        return std::nullopt;
    }
    return queryWorld().velocity(e);
}

std::optional<wander::RayHitInfo> PhysicsSystem::raycast(Vec3 origin, Vec3 direction, float maxDistance, EntityId ignore) {
    QueryFilter f;
    if (ignore) f.exclude.push_back(ignore);
    auto hit = queryWorld().raycast(origin, direction, maxDistance, f);
    if (!hit) return std::nullopt;
    return wander::RayHitInfo{hit->entity, hit->point, hit->normal, hit->distance};
}

std::vector<EntityId> PhysicsSystem::overlapSphere(Vec3 center, float radius, EntityId ignore) {
    QueryFilter f;
    if (ignore) f.exclude.push_back(ignore);
    QueryShape s;
    s.radius = std::max(radius, 0.001f);
    return queryWorld().overlap(s, center, f);
}

bool PhysicsSystem::walk(EntityId e, Vec3 direction) {
    auto* w = liveWorld<CharacterController>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasCharacter(e); });
    return w && w->walk(e, direction);
}

bool PhysicsSystem::jump(EntityId e, float speed) {
    auto* w = liveWorld<CharacterController>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasCharacter(e); });
    return w && w->jump(e, speed);
}

std::optional<bool> PhysicsSystem::grounded(EntityId e) {
    auto* w = liveWorld<CharacterController>(*this, scene_, e, [&](PhysicsWorld& pw) { return pw.hasCharacter(e); });
    if (!w) return scene_.get<CharacterController>(e) ? std::optional<bool>(false) : std::nullopt;
    return w->grounded(e);
}

bool PhysicsSystem::navigate(EntityId e, Vec3 target, EntityId follow) { return nav_ && nav_->navigate(e, target, follow); }

bool PhysicsSystem::stopNavigation(EntityId e) { return nav_ && nav_->stop(e); }

std::optional<bool> PhysicsSystem::arrived(EntityId e) {
    if (!nav_) return std::nullopt;
    return nav_->arrived(e);
}

std::optional<float> PhysicsSystem::pathLength(Vec3 from, Vec3 to) {
    if (!nav_) return std::nullopt;
    return nav_->pathLength(from, to);
}

}  // namespace sky::physics
