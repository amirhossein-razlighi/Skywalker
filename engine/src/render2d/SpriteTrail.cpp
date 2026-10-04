// sprite_trail afterimages and timed sprite flashes, advanced on fixed ticks (ecs/SpriteTrail.h).

#include "skywalker/ecs/SpriteTrail.h"

#include <algorithm>
#include <cmath>

#include "skywalker/ecs/Components.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/scene/Scene.h"

namespace sky {

const TypeInfo& SpriteTrail::type() {
    static const TypeInfo info{
        "sprite_trail",
        "Afterimages behind a moving sprite: dashes, lunges, fast slashes, a swooping boss. While the entity moves "
        "faster than minSpeed it records its pose and animation frame every `interval` seconds; the last `count` "
        "poses are drawn behind the sprite in `color`, fading out with age. Toggle `emitting` from Wander around a dash.",
        {
            SKY_FIELD(SpriteTrail, emitting, Bool, "Record new afterimages (existing ones still fade out)"),
            SKY_FIELD_RANGE(SpriteTrail, count, Int, "Afterimages drawn at most (3-10)", 0, 64),
            SKY_FIELD_RANGE(SpriteTrail, interval, Float, "Seconds between afterimages (0.02-0.06)", 0.005f, 2.f),
            SKY_FIELD_RANGE(SpriteTrail, minSpeed, Float, "World units per second needed to record (0 = always)", 0.f, 1000.f),
            SKY_FIELD(SpriteTrail, color, Color, "Tint of the afterimages"),
            SKY_FIELD_RANGE(SpriteTrail, opacity, Float, "Opacity of the newest afterimage", 0.f, 1.f),
            SKY_FIELD_RANGE(SpriteTrail, emissive, Float, "Glow of the afterimages (blooms in HDR)", 0.f, 50.f),
            SKY_FIELD(SpriteTrail, additive, Bool, "Blend additively (energy streaks) instead of as translucent copies"),
        }};
    return info;
}

namespace render2d {

void tickSpriteFx(Scene& scene, Assets2D& assets, float dt, const ProcessGate* gate) {
    auto& reg = scene.registry();
    const bool trails = reg.count<SpriteTrail>() > 0;
    for (EntityId e : scene.entities()) {
        Sprite* sp = scene.get<Sprite>(e);
        if (!sp) continue;
        const float k = gate ? gate->scale(e) : 1.f;
        if (k <= 0.f) continue;
        const float h = dt * k;
        if (sp->flashTimer_ > 0.f) sp->flashTimer_ = std::max(0.f, sp->flashTimer_ - h);
        if (!trails) continue;
        SpriteTrail* t = scene.get<SpriteTrail>(e);
        if (!t) continue;
        const Mat4 world = scene.worldMatrix(e);
        const Vec3 pos = world.translation();
        for (TrailSnapshot& s : t->snapshots_) s.age += h;
        const float life = static_cast<float>(std::max(1, t->count)) * std::max(1e-3f, t->interval);
        t->snapshots_.erase(std::remove_if(t->snapshots_.begin(), t->snapshots_.end(),
                                           [&](const TrailSnapshot& s) { return s.age >= life; }),
                            t->snapshots_.end());
        if (!t->started_) {
            t->started_ = true;
            t->lastPos_ = pos;
            t->timer_ = 0.f;
            continue;
        }
        const float speed = h > 0.f ? length(pos - t->lastPos_) / h : 0.f;
        t->lastPos_ = pos;
        t->timer_ += h;
        if (!t->emitting || !scene.isActive(e) || !sp->visible || speed < t->minSpeed) continue;
        if (t->timer_ < t->interval) continue;
        t->timer_ = std::fmod(t->timer_, std::max(1e-3f, t->interval));
        TrailSnapshot s;
        s.world = world;
        s.flipX = sp->flipX;
        std::string texture;
        int columns = 1, rows = 1, frame = -1;
        if (animatedFrame(scene, assets, e, texture, columns, rows, frame)) {
            s.texture = texture;
            s.columns = columns;
            s.rows = rows;
            s.frame = frame;
        }
        t->snapshots_.push_back(std::move(s));
        const size_t cap = static_cast<size_t>(std::max(1, t->count));
        if (t->snapshots_.size() > cap) t->snapshots_.erase(t->snapshots_.begin(), t->snapshots_.end() - static_cast<std::ptrdiff_t>(cap));
    }
}

void flashSprite(Sprite& sprite, float seconds, Vec4 color) {
    sprite.flashColor_ = color;
    sprite.flashDuration_ = std::max(1e-3f, seconds);
    sprite.flashTimer_ = sprite.flashDuration_;
}

}  // namespace render2d
}  // namespace sky
