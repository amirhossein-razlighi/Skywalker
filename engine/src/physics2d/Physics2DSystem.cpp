#include "skywalker/physics2d/Physics2DSystem.h"

#include <algorithm>

#include "skywalker/ecs/Components.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/wander/Runtime.h"

namespace sky::physics2d {

Physics2DSystem::Physics2DSystem(Scene& scene, TileCollisionProvider tiles) : scene_(scene), tiles_(std::move(tiles)) {}

Physics2DSystem::~Physics2DSystem() = default;

std::unique_ptr<Physics2DWorld> Physics2DSystem::makeWorld(WorldOptions2D options) const {
    return std::make_unique<Physics2DWorld>(tiles_, std::move(options));
}

bool Physics2DSystem::inUse() const {
    auto& reg = scene_.registry();  // count() is not const in the registry
    return reg.count<Body2D>() + reg.count<Collider2D>() + reg.count<Character2D>() > 0;
}

void Physics2DSystem::beginPlay() {
    play_.reset();
    playing_ = true;
}

void Physics2DSystem::endPlay() {
    if (play_) collectWarnings(*play_);
    play_.reset();  // the scene is restored from the play snapshot; the next play builds a fresh world
    playing_ = false;
    editRevision_ = ~0ull;
}

Physics2DWorld& Physics2DSystem::ensurePlayWorld() {
    if (!play_) {
        play_ = makeWorld({});
        play_->sync(scene_, 0.f);
    }
    return *play_;
}

Physics2DWorld& Physics2DSystem::queryWorld() {
    if (playing_) return ensurePlayWorld();
    if (!edit_) {
        WorldOptions2D o;
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

void Physics2DSystem::collectWarnings(Physics2DWorld& world) {
    for (auto& w : world.drainWarnings()) {
        if (std::find(warnings_.begin(), warnings_.end(), w) != warnings_.end()) continue;
        warnings_.push_back(std::move(w));
        while (warnings_.size() > 50) warnings_.pop_front();
    }
}

void Physics2DSystem::step(float dt, wander::Runtime& runtime) {
    if (!play_ && !inUse()) return;  // 3D-only scenes pay nothing
    Physics2DWorld& world = ensurePlayWorld();
    world.sync(scene_, dt);
    world.step(scene_, dt);
    auto vec = [](Vec2 v) { return Vec3{v.x, v.y, 0.f}; };
    for (const Event2D& ev : world.drainEvents()) {
        switch (ev.kind) {
            case Event2D::Kind::Begin:
            case Event2D::Kind::SensorEnter:
            case Event2D::Kind::SensorExit: {
                wander::Runtime::Contact c;
                c.trigger = ev.kind == Event2D::Kind::Begin         ? wander::Trigger::Collide
                            : ev.kind == Event2D::Kind::SensorEnter ? wander::Trigger::TriggerEnter
                                                                    : wander::Trigger::TriggerExit;
                c.self = ev.self;
                c.other = ev.other;
                c.point = vec(ev.point);
                c.normal = vec(ev.normal);
                c.speed = ev.speed;
                runtime.queueContact(c);
                break;
            }
            case Event2D::Kind::End: runtime.emit("collide_end", ev.self, wander::Value::map(), ev.other); break;
            case Event2D::Kind::Impact: {
                wander::Value data = wander::Value::map();
                wander::MapObj& m = data.mutMap();
                m.set("point", wander::Value::vec(vec(ev.point)));
                m.set("normal", wander::Value::vec(vec(ev.normal)));
                m.set("speed", wander::Value::number(ev.speed));
                m.set("impulse", wander::Value::number(ev.impulse));
                runtime.emit("impact", ev.self, std::move(data), ev.other);
                break;
            }
        }
    }
    for (EntityId e : world.drainBrokenJoints()) runtime.emit("joint_broken", e);
    collectWarnings(world);
}

bool Physics2DSystem::debugDrawEnabled() const {
    for (EntityId e : scene_.entities()) {
        if (const Physics2DSettings* s = scene_.get<Physics2DSettings>(e); s && scene_.isActive(e)) return s->debugDraw;
    }
    return false;
}

void Physics2DSystem::gatherDebug(Frame2D& frame) {
    if (!inUse() || !debugDrawEnabled()) return;
    const DebugDraw2D d = queryWorld().debugDraw();
    auto color = [](const DebugShape2D& s) -> Vec4 {
        if (s.kind == "sensor") return {1.f, 0.85f, 0.2f, 0.9f};
        if (s.kind == "one_way") return {0.3f, 0.9f, 1.f, 0.9f};
        if (s.kind == "character") return {1.f, 0.3f, 0.8f, 1.f};
        if (s.kind == "static") return {0.35f, 0.85f, 0.45f, 0.8f};
        if (s.kind == "kinematic") return {0.25f, 0.85f, 1.f, 0.9f};
        return s.sleeping ? Vec4{0.5f, 0.55f, 0.8f, 0.8f} : Vec4{1.f, 0.62f, 0.15f, 1.f};
    };
    const float z = 0.01f;  // just in front of the sprite plane
    for (const DebugShape2D& s : d.shapes) {
        const Vec4 c = color(s);
        for (size_t i = 0; i + 1 < s.lines.size(); i += 2) {
            frame.debugLines.push_back({{s.lines[i].x, s.lines[i].y, z}, {s.lines[i + 1].x, s.lines[i + 1].y, z}, c});
        }
    }
    for (const auto& [p, n] : d.contacts) {
        frame.debugLines.push_back({{p.x, p.y, z}, {p.x + n.x * 0.3f, p.y + n.y * 0.3f, z}, {1.f, 0.2f, 0.2f, 1.f}});
    }
    for (const auto& [a, b] : d.joints) frame.debugLines.push_back({{a.x, a.y, z}, {b.x, b.y, z}, {0.9f, 0.9f, 0.9f, 1.f}});
}

TileCollisionProvider tileCollisionFrom(render2d::Assets2D& assets) {
    return [&assets](const Tilemap& map) -> Result<TileCollision> {
        TileCollision out;
        if (map.tileset.empty()) return out;
        auto ts = assets.tileset(map.tileset, map.tileSize);
        if (!ts) return ts.error();
        out.solidIds = ts->solid;
        auto shapes = parseTileShapes(ts->collision, ts->tileSize);
        if (!shapes) return shapes.error();
        out.shapes = std::move(shapes.value());
        return out;
    };
}

}  // namespace sky::physics2d
