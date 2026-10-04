#include "skywalker/render2d/Particles2D.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "skywalker/render2d/Sprites.h"
#include "skywalker/scene/Scene.h"

namespace sky {

static_assert(std::is_standard_layout_v<Particles2D>);

const TypeInfo& Particles2D::type() {
    static const TypeInfo info{
        "particles2d",
        "Pixel-art particles on the 2D plane: rain streaks, snow, falling leaves and petals, fireflies, chimney puffs, "
        "puddle ripples. Drawn as sprites (nearest sampling, texel snapping, sorting layers) from a sheet or as solid "
        "pixel rectangles; simulated deterministically on fixed ticks. wrap = true tiles the emission box endlessly so "
        "weather fills whatever the camera shows. Create tuned ones with particles2d_create.",
        {
            SKY_FIELD(Particles2D, emitting, Bool, "Spawn new particles (live ones finish their life)"),
            SKY_FIELD(Particles2D, texture, String, "Grid sheet (png) or atlas; empty = solid rectangles of pixelSize texels"),
            SKY_FIELD_RANGE(Particles2D, columns, Int, "Sheet columns", 1, 1024),
            SKY_FIELD_RANGE(Particles2D, rows, Int, "Sheet rows", 1, 1024),
            SKY_FIELD(Particles2D, frames, String, "Frames to use (\"0-3,6\"; empty = all)"),
            SKY_FIELD_ENUM(Particles2D, animate, "random: each particle keeps one frame; life: frames play over its life; loop: at fps",
                           "random", "life", "loop"),
            SKY_FIELD_RANGE(Particles2D, fps, Float, "Frames per second (animate = loop)", 0.f, 120.f),
            SKY_FIELD_RANGE(Particles2D, pixelsPerUnit, Float, "Texels per world unit (match the art, e.g. 16)", 0.01f, 100000.f),
            SKY_FIELD(Particles2D, pixelSize, Vec2, "Untextured particles: [w, h] in texels (rain streak: [1, 4])"),
            SKY_FIELD_RANGE(Particles2D, rate, Float, "Particles per second", 0.f, 100000.f),
            SKY_FIELD_RANGE(Particles2D, burst, Int, "Particles emitted at once when play starts", 0, 100000),
            SKY_FIELD_RANGE(Particles2D, maxParticles, Int, "Cap on live particles", 0, 20000),
            SKY_FIELD_RANGE(Particles2D, lifetime, Float, "Seconds a particle lives", 0.01f, 600.f),
            SKY_FIELD_RANGE(Particles2D, lifetimeJitter, Float, "Random fraction of the lifetime", 0.f, 1.f),
            SKY_FIELD(Particles2D, area, Vec2, "Emission box [w, h] in world units, centered on the entity"),
            SKY_FIELD(Particles2D, wrap, Bool, "Tile the box endlessly around the view (weather that fills any camera)"),
            SKY_FIELD(Particles2D, velocity, Vec2, "Initial velocity (world units/s)"),
            SKY_FIELD(Particles2D, velocityJitter, Vec2, "Random +- added per axis"),
            SKY_FIELD(Particles2D, gravity, Vec2, "Acceleration (world units/s^2); [0.3, 0] works as wind"),
            SKY_FIELD_RANGE(Particles2D, drag, Float, "Velocity damping per second", 0.f, 100.f),
            SKY_FIELD_RANGE(Particles2D, sway, Float, "Side-to-side drift speed (leaves, snow)", 0.f, 100.f),
            SKY_FIELD_RANGE(Particles2D, swayFrequency, Float, "Sway cycles per second", 0.f, 50.f),
            SKY_FIELD_RANGE(Particles2D, flutter, Float, "Random-walk wander (fireflies, butterflies)", 0.f, 100.f),
            SKY_FIELD(Particles2D, color, Color, "Tint and opacity"),
            SKY_FIELD_RANGE(Particles2D, colorJitter, Float, "Per-particle brightness variation", 0.f, 1.f),
            SKY_FIELD(Particles2D, emissive, Color, "Glow color, alpha = strength (blooms)"),
            SKY_FIELD_RANGE(Particles2D, pulse, Float, "Twinkle depth of opacity and glow (fireflies)", 0.f, 1.f),
            SKY_FIELD_RANGE(Particles2D, pulseFrequency, Float, "Twinkles per second", 0.f, 50.f),
            SKY_FIELD_RANGE(Particles2D, fadeIn, Float, "Fraction of the life spent fading in", 0.f, 1.f),
            SKY_FIELD_RANGE(Particles2D, fadeOut, Float, "Fraction of the life spent fading out", 0.f, 1.f),
            SKY_FIELD_ENUM(Particles2D, sortingLayer, "Draw layer", "background", "midground", "default", "foreground", "overlay"),
            SKY_FIELD(Particles2D, order, Int, "Order within the layer"),
            SKY_FIELD(Particles2D, lit, Bool, "Lit by 2D lights (else full bright)"),
            SKY_FIELD(Particles2D, prewarm, Bool, "Start as if it had been running for a while"),
            SKY_FIELD(Particles2D, seed, Int, "Random seed (same seed, same particles)"),
        }};
    return info;
}

}  // namespace sky

namespace sky::render2d {

namespace {

uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float uniform(uint64_t& s) { return static_cast<float>(splitmix(s) >> 40) / static_cast<float>(1ull << 24); }  // [0, 1)
float signedUnit(uint64_t& s) { return uniform(s) * 2.f - 1.f; }

float wrapInto(float v, float period) {
    if (period <= 1e-6f) return v;
    v = std::fmod(v, period);
    return v < 0.f ? v + period : v;
}

void spawn(Particles2D& p, Vec3 origin, int frameCount) {
    Particle2D q;
    const float u = uniform(p.rng_), v = uniform(p.rng_);
    if (p.wrap) {
        q.pos = {u * p.area.x, v * p.area.y};  // box-local; drawn at the tiling copy nearest the view
    } else {
        q.pos = {origin.x + (u - 0.5f) * p.area.x, origin.y + (v - 0.5f) * p.area.y};
    }
    q.vel = {p.velocity.x + signedUnit(p.rng_) * p.velocityJitter.x, p.velocity.y + signedUnit(p.rng_) * p.velocityJitter.y};
    q.life = std::max(0.01f, p.lifetime * (1.f + signedUnit(p.rng_) * p.lifetimeJitter));
    q.phase = uniform(p.rng_) * 6.2831853f;
    q.shade = 1.f - uniform(p.rng_) * p.colorJitter;
    q.frame = frameCount > 1 ? static_cast<int>(splitmix(p.rng_) % static_cast<uint64_t>(frameCount)) : 0;
    p.particles_.push_back(q);
}

void simulate(Particles2D& p, Vec3 origin, float dt, int frameCount) {
    // Age and move the live particles (order is kept: draws stay stable).
    size_t keep = 0;
    for (size_t i = 0; i < p.particles_.size(); ++i) {
        Particle2D q = p.particles_[i];
        q.age += dt;
        if (q.age >= q.life) continue;
        q.vel = q.vel + Vec2{p.gravity.x, p.gravity.y} * dt;
        if (p.flutter > 0.f) {
            q.vel.x += signedUnit(p.rng_) * p.flutter * dt * 6.f;
            q.vel.y += signedUnit(p.rng_) * p.flutter * dt * 6.f;
        }
        if (p.drag > 0.f) q.vel = q.vel * std::max(0.f, 1.f - p.drag * dt);
        q.pos = q.pos + q.vel * dt;
        if (p.sway > 0.f) q.pos.x += std::cos(q.phase + q.age * p.swayFrequency * 6.2831853f) * p.sway * dt;
        if (p.wrap) q.pos = {wrapInto(q.pos.x, p.area.x), wrapInto(q.pos.y, p.area.y)};
        p.particles_[keep++] = q;
    }
    p.particles_.resize(keep);
    if (!p.emitting || p.rate <= 0.f) return;
    p.carry_ += p.rate * dt;
    while (p.carry_ >= 1.f) {
        p.carry_ -= 1.f;
        if (static_cast<int>(p.particles_.size()) >= p.maxParticles) {
            p.carry_ = std::min(p.carry_, 1.f);
            break;
        }
        spawn(p, origin, frameCount);
    }
}

}  // namespace

std::vector<int> particleFrames(Assets2D& assets, const Particles2D& p) {
    if (p.texture.empty()) return {0};
    const int count = std::max(1, assets.frameCount(p.texture, p.columns, p.rows));
    std::vector<int> out;
    if (!p.frames.empty()) {
        if (auto ids = tiles::parseIdList(Json(p.frames))) {
            for (uint32_t id : ids.value()) {
                if (static_cast<int>(id) < count) out.push_back(static_cast<int>(id));
            }
        }
    }
    if (out.empty()) {
        for (int i = 0; i < count; ++i) out.push_back(i);
    }
    return out;
}

void stepParticles2D(Particles2D& p, Vec3 origin, float dt, int frameCount, uint64_t entitySalt) {
    if (!p.started_) {
        p.started_ = true;
        p.rng_ = (static_cast<uint64_t>(static_cast<uint32_t>(p.seed)) << 32) ^ (entitySalt * 0x9E3779B97F4A7C15ull) ^ 0x2D2D2D2Dull;
        p.particles_.clear();
        p.carry_ = 0.f;
        for (int i = 0; i < std::min(p.burst, p.maxParticles); ++i) spawn(p, origin, frameCount);
        if (p.prewarm && p.emitting && p.rate > 0.f) {
            const float warm = std::min(30.f, p.lifetime * (1.f + p.lifetimeJitter));
            for (float t = 0.f; t < warm; t += 1.f / 20.f) simulate(p, origin, 1.f / 20.f, frameCount);
        }
    }
    simulate(p, origin, dt, frameCount);
    for (; p.pendingBurst_ > 0; --p.pendingBurst_) {
        if (static_cast<int>(p.particles_.size()) >= p.maxParticles) {
            p.pendingBurst_ = 0;
            break;
        }
        spawn(p, origin, frameCount);
    }
}

void tickParticles2D(Scene& scene, Assets2D& assets, float dt, const ProcessGate* gate) {
    for (EntityId e : scene.entities()) {
        Particles2D* p = scene.get<Particles2D>(e);
        if (!p || !scene.isActive(e)) continue;
        const float k = gate ? gate->scale(e) : 1.f;
        if (k <= 0.f) continue;
        const int frames = static_cast<int>(particleFrames(assets, *p).size());
        stepParticles2D(*p, scene.worldMatrix(e).translation(), dt * k, frames, static_cast<uint64_t>(e));
    }
}

Vec3 particleWorldPosition(const Particles2D& p, const Particle2D& q, Vec3 origin, Vec3 eye) {
    if (!p.wrap) return {q.pos.x, q.pos.y, origin.z};
    // Box-local position -> the copy of the endless tiling nearest to the eye.
    float x = origin.x - p.area.x * 0.5f + q.pos.x, y = origin.y - p.area.y * 0.5f + q.pos.y;
    if (p.area.x > 1e-6f) x -= p.area.x * std::round((x - eye.x) / p.area.x);
    if (p.area.y > 1e-6f) y -= p.area.y * std::round((y - eye.y) / p.area.y);
    return {x, y, origin.z};
}

const std::vector<std::string>& particles2dPresets() {
    static const std::vector<std::string> names{"rain", "drizzle", "snow", "leaves", "petals", "fireflies",
                                                "smoke", "ripples", "dust", "sparkle"};
    return names;
}

Json particles2dPreset(const std::string& name) {
    static const std::pair<const char*, const char*> table[] = {
        {"rain", R"({"pixelSize": [1, 4], "color": "#c4dcffb4", "colorJitter": 0.25, "velocity": [-2.2, -21], "velocityJitter": [0.4, 3],
                     "lifetime": 0.45, "lifetimeJitter": 0.5, "rate": 900, "maxParticles": 900, "area": [30, 20], "wrap": true,
                     "fadeIn": 0.05, "fadeOut": 0.25, "sortingLayer": "overlay"})"},
        {"drizzle", R"({"pixelSize": [1, 3], "color": "#c4dcff8c", "colorJitter": 0.25, "velocity": [-1.2, -15], "velocityJitter": [0.3, 2],
                        "lifetime": 0.5, "lifetimeJitter": 0.5, "rate": 260, "maxParticles": 400, "area": [30, 20], "wrap": true,
                        "fadeIn": 0.05, "fadeOut": 0.3, "sortingLayer": "overlay"})"},
        {"snow", R"({"pixelSize": [1, 1], "color": "#f6faff", "colorJitter": 0.2, "velocity": [0.35, -1.4], "velocityJitter": [0.3, 0.4],
                     "sway": 0.7, "swayFrequency": 0.45, "lifetime": 10, "lifetimeJitter": 0.3, "rate": 110, "maxParticles": 1400,
                     "area": [30, 20], "wrap": true, "fadeIn": 0.08, "fadeOut": 0.12, "sortingLayer": "overlay"})"},
        {"leaves", R"({"pixelSize": [2, 1], "color": "#d9873a", "colorJitter": 0.3, "velocity": [0.7, -1.1], "velocityJitter": [0.3, 0.3],
                       "sway": 1.4, "swayFrequency": 0.6, "lifetime": 6, "lifetimeJitter": 0.3, "rate": 1.5, "maxParticles": 40,
                       "area": [6, 2], "fadeIn": 0.1, "fadeOut": 0.25, "sortingLayer": "foreground"})"},
        {"petals", R"({"pixelSize": [1, 1], "color": "#ffc4d8", "colorJitter": 0.2, "velocity": [0.8, -0.7], "velocityJitter": [0.3, 0.3],
                       "sway": 1.0, "swayFrequency": 0.8, "lifetime": 7, "lifetimeJitter": 0.3, "rate": 3, "maxParticles": 60,
                       "area": [16, 8], "fadeIn": 0.1, "fadeOut": 0.3, "sortingLayer": "foreground"})"},
        {"fireflies", R"({"pixelSize": [1, 1], "color": "#f4ff9e", "emissive": "#e6ff6a", "velocity": [0, 0], "velocityJitter": [0.3, 0.3],
                          "flutter": 0.7, "drag": 1.2, "pulse": 0.9, "pulseFrequency": 0.6, "lifetime": 8, "lifetimeJitter": 0.4,
                          "rate": 3, "maxParticles": 60, "area": [10, 6], "fadeIn": 0.2, "fadeOut": 0.3, "sortingLayer": "foreground"})"},
        {"smoke", R"({"pixelSize": [2, 2], "color": "#e4e0d8a8", "colorJitter": 0.15, "velocity": [0.2, 0.75], "velocityJitter": [0.08, 0.12],
                      "gravity": [0.12, 0], "sway": 0.25, "swayFrequency": 0.5, "lifetime": 3.5, "lifetimeJitter": 0.3, "rate": 4,
                      "maxParticles": 40, "area": [0.25, 0.1], "fadeIn": 0.1, "fadeOut": 0.6, "sortingLayer": "foreground"})"},
        {"ripples", R"({"pixelSize": [3, 1], "color": "#d6e8ff90", "animate": "life", "velocity": [0, 0], "velocityJitter": [0, 0],
                        "lifetime": 0.55, "lifetimeJitter": 0.2, "rate": 30, "maxParticles": 80, "area": [30, 20], "wrap": true,
                        "fadeIn": 0, "fadeOut": 0.4, "sortingLayer": "midground"})"},
        {"dust", R"({"pixelSize": [1, 1], "color": "#fff2c4a0", "velocity": [0.1, 0.05], "velocityJitter": [0.1, 0.1], "flutter": 0.15,
                     "drag": 0.5, "pulse": 0.5, "pulseFrequency": 0.4, "lifetime": 6, "lifetimeJitter": 0.3, "rate": 4,
                     "maxParticles": 60, "area": [10, 6], "fadeIn": 0.25, "fadeOut": 0.35, "sortingLayer": "foreground"})"},
        {"sparkle", R"({"pixelSize": [1, 1], "color": "#ffffff", "emissive": "#ffffff80", "velocity": [0, 0], "velocityJitter": [0, 0],
                        "lifetime": 0.4, "lifetimeJitter": 0.4, "rate": 10, "maxParticles": 40, "area": [8, 2], "fadeIn": 0.3,
                        "fadeOut": 0.5, "sortingLayer": "midground"})"},
    };
    for (const auto& [n, j] : table) {
        if (name == n) return Json::parse(j).value();
    }
    return Json();
}

}  // namespace sky::render2d
