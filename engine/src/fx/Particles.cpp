#include "skywalker/fx/Particles.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sky::fx {

namespace {

constexpr float kPi = 3.14159265358979f;

float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float lookId(const std::string& look) {
    if (look == "flame") return static_cast<float>(ParticleLook::Flame);
    if (look == "smoke") return static_cast<float>(ParticleLook::Smoke);
    if (look == "spark") return static_cast<float>(ParticleLook::Spark);
    if (look == "rain") return static_cast<float>(ParticleLook::Rain);
    if (look == "snow") return static_cast<float>(ParticleLook::Snow);
    if (look == "mist") return static_cast<float>(ParticleLook::Mist);
    return static_cast<float>(ParticleLook::Glow);
}

bool emissiveLook(float look) {
    auto l = static_cast<ParticleLook>(static_cast<int>(look));
    return l == ParticleLook::Glow || l == ParticleLook::Flame || l == ParticleLook::Spark;
}

Vec3 windVector(const Environment& env) {
    float a = radians(env.windDirection);
    return Vec3{std::sin(a), 0.f, std::cos(a)} * env.windSpeed;
}

/// Random unit vector within `spreadDeg` of `dir`.
Vec3 coneDirection(Random& rng, Vec3 dir, float spreadDeg) {
    dir = length(dir) > 1e-6f ? normalize(dir) : Vec3{0, 1, 0};
    float cosMax = std::cos(radians(std::clamp(spreadDeg, 0.f, 180.f)));
    float z = 1.f - rng.nextFloat() * (1.f - cosMax);
    float r = std::sqrt(std::max(0.f, 1.f - z * z));
    float phi = rng.nextFloat() * 2.f * kPi;
    Vec3 up = std::fabs(dir.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    Vec3 t = normalize(cross(up, dir));
    Vec3 b = cross(dir, t);
    return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + dir * z);
}

}  // namespace

Vec3 swirl(Vec3 p, float time, float scale) {
    // Sum of shear waves a*sin(k.p + w t) with a perpendicular to k: each term is
    // divergence-free, so the sum swirls like curl noise without sources or sinks.
    static const Vec3 ks[4] = {{0.81f, 0.32f, 0.49f}, {-0.37f, 0.88f, 0.29f}, {0.21f, -0.43f, 0.88f}, {-0.66f, -0.18f, -0.73f}};
    static const Vec3 as[4] = {{-0.37f, 0.f, 0.61f}, {0.92f, 0.39f, 0.f}, {0.f, 0.9f, 0.44f}, {0.74f, 0.f, -0.67f}};
    float inv = 1.f / std::max(scale, 1e-3f);
    Vec3 v{0, 0, 0};
    float amp = 1.f, freq = 1.f;
    for (int octave = 0; octave < 2; ++octave) {
        for (int i = 0; i < 4; ++i) {
            Vec3 k = ks[i] * (freq * inv * (1.f + 0.37f * static_cast<float>(i)));
            // a must be perpendicular to k: remove the parallel part.
            Vec3 a = as[i] - normalize(k) * dot(as[i], normalize(k));
            float ph = dot(k, p) + time * (0.6f + 0.23f * static_cast<float>(i)) * freq + static_cast<float>(i) * 1.7f;
            v = v + a * (amp * std::sin(ph));
        }
        amp *= 0.5f;
        freq *= 2.13f;
    }
    return v * 0.6f;
}

void ParticleSystem::reset() {
    states_.clear();
    time_ = 0;
}

void ParticleSystem::burst(EntityId emitter, int count) {
    if (count <= 0) return;
    states_[emitter].pendingBurst += std::min(count, 20000);
    gpuBursts_[emitter] += static_cast<uint64_t>(std::min(count, 1000000));
}

uint64_t ParticleSystem::burstSerial(EntityId emitter) const {
    auto it = gpuBursts_.find(emitter);
    return it == gpuBursts_.end() ? 0 : it->second;
}

size_t ParticleSystem::liveCount(EntityId emitter) const {
    auto it = states_.find(emitter);
    return it == states_.end() ? 0 : it->second.particles.size();
}

size_t ParticleSystem::totalLive() const {
    size_t n = 0;
    for (const auto& [id, s] : states_) n += s.particles.size();
    return n;
}

void ParticleSystem::spawn(State& s, const ParticleEmitter& em, const Mat4& world, int count) {
    const size_t cap = static_cast<size_t>(std::clamp(em.maxParticles, 1, 50000));
    Vec3 half = em.shapeSize * 0.5f;
    for (int i = 0; i < count && s.particles.size() < cap; ++i) {
        Random& r = s.rng;
        Vec3 local{0, 0, 0};
        Vec3 dir = coneDirection(r, em.direction, em.spread);
        if (em.shape == "box") {
            local = {r.range(-half.x, half.x), r.range(-half.y, half.y), r.range(-half.z, half.z)};
        } else if (em.shape == "sphere") {
            Vec3 u;
            do {
                u = {r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)};
            } while (dot(u, u) > 1.f);
            local = {u.x * half.x, u.y * half.y, u.z * half.z};
        } else if (em.shape == "disc" || em.shape == "cone") {
            float a = r.nextFloat() * 2.f * kPi, rad = std::sqrt(r.nextFloat());
            local = {std::cos(a) * rad * half.x, 0.f, std::sin(a) * rad * half.z};
            if (em.shape == "cone") dir = normalize(dir + Vec3{local.x, 0, local.z} * 0.8f);
        }
        Particle p;
        float speed = em.speed * (1.f + em.speedJitter * r.range(-1, 1));
        Vec3 vel = dir * speed;
        if (em.worldSpace) {
            p.pos = world.transformPoint(local);
            Vec3 wd = world.transformDir(vel);
            p.vel = length(wd) > 1e-6f ? normalize(wd) * speed : Vec3{0, 0, 0};
        } else {
            p.pos = local;
            p.vel = vel;
        }
        p.life = std::max(0.02f, em.lifetime * (1.f + em.lifetimeJitter * r.range(-1, 1)));
        float sz = 1.f + em.sizeJitter * r.range(-1, 1);
        p.size0 = em.sizeStart * sz;
        p.size1 = em.sizeEnd * sz;
        p.rot = r.nextFloat() * 2.f * kPi;
        p.spin = radians(em.spin) * r.range(-1, 1);
        p.seed = r.nextFloat();
        s.particles.push_back(p);
    }
}

void ParticleSystem::step(State& s, const ParticleEmitter& em, const Mat4& world, const Environment& env, float dt) {
    // Age and retire.
    auto& ps = s.particles;
    for (size_t i = 0; i < ps.size();) {
        ps[i].age += dt;
        if (ps[i].age >= ps[i].life) {
            ps[i] = ps.back();
            ps.pop_back();
        } else {
            ++i;
        }
    }
    // Emit.
    int n = 0;
    if (em.emitting) {
        if (!s.started) n += em.burst;
        s.spawnCarry += std::max(0.f, em.rate) * dt;
        int whole = static_cast<int>(s.spawnCarry);
        s.spawnCarry -= static_cast<float>(whole);
        n += whole;
    }
    s.started = true;
    n += s.pendingBurst;
    s.pendingBurst = 0;
    size_t firstNew = ps.size();
    spawn(s, em, world, n);

    Mat4 toLocal = em.worldSpace ? Mat4{} : world.inverse();
    Vec3 gravity{0.f, -em.gravity, 0.f};
    Vec3 wind = windVector(env) * em.wind;
    if (!em.worldSpace) {
        gravity = toLocal.transformDir(gravity);
        wind = toLocal.transformDir(wind);
    }
    for (size_t i = firstNew; i < ps.size(); ++i) ps[i].vel = ps[i].vel + wind * 0.7f;  // born moving with the air

    float relax = 1.f - std::exp(-std::max(em.drag, 0.f) * dt);
    std::vector<Particle> droplets;
    for (size_t i = 0; i < ps.size();) {
        Particle& p = ps[i];
        if (p.splash) {
            p.vel.y -= 9.81f * dt;
        } else {
            Vec3 air = wind;
            if (em.turbulence > 0.f) air = air + swirl(p.pos + Vec3{p.seed * 7.f, 0, 0}, time_, em.turbulenceScale) * em.turbulence;
            p.vel = p.vel + gravity * dt + (air - p.vel) * relax;
        }
        p.pos = p.pos + p.vel * dt;
        p.rot += p.spin * dt;
        bool dead = false;
        if (em.collide) {
            float y = em.worldSpace ? p.pos.y : world.transformPoint(p.pos).y;
            if (y < em.floorHeight) {
                if (p.splash) {
                    dead = true;
                } else if (em.splash > 0) {
                    for (int k = 0; k < em.splash; ++k) {
                        Particle d;
                        d.splash = true;
                        d.pos = p.pos;
                        if (em.worldSpace) d.pos.y = em.floorHeight + 0.01f;
                        float a = s.rng.nextFloat() * 2.f * kPi, h = s.rng.range(0.6f, 1.4f);
                        d.vel = Vec3{std::cos(a) * h, s.rng.range(1.2f, 2.4f), std::sin(a) * h};
                        d.life = s.rng.range(0.18f, 0.32f);
                        d.size0 = d.size1 = std::max(em.sizeStart, 0.004f) * 0.9f;
                        d.seed = s.rng.nextFloat();
                        droplets.push_back(d);
                    }
                    dead = true;
                } else if (em.bounce > 0.f) {
                    if (em.worldSpace) p.pos.y = em.floorHeight;
                    p.vel.y = std::fabs(p.vel.y) * em.bounce;
                    p.vel.x *= 0.7f;
                    p.vel.z *= 0.7f;
                } else {
                    dead = true;
                }
            }
        }
        if (dead) {
            ps[i] = ps.back();
            ps.pop_back();
        } else {
            ++i;
        }
    }
    const size_t cap = static_cast<size_t>(std::clamp(em.maxParticles, 1, 50000)) * 2;
    for (auto& d : droplets) {
        if (ps.size() >= cap) break;
        ps.push_back(d);
    }

    // Flame energy -> the light this emitter casts (follows the live fire, so it flickers).
    if (em.light > 0.f) {
        float e = 0, expected = std::max(1.f, (em.rate > 0 ? em.rate * em.lifetime : static_cast<float>(em.burst)) * 0.5f);
        Vec3 c{0, 0, 0};
        float w = 0;
        for (const Particle& p : ps) {
            if (p.splash) continue;
            float k = 1.f - p.age / p.life;
            e += k;
            c = c + p.pos * k;
            w += k;
        }
        float target = e / expected;
        s.energy += (target - s.energy) * (1.f - std::exp(-dt * 14.f));
        Vec3 center = world.translation();
        if (w > 0) {
            Vec3 mean = c * (1.f / w);
            if (!em.worldSpace) mean = world.transformPoint(mean);
            s.lightPos = lerp(center, mean, 0.6f);
        } else {
            s.lightPos = center;
        }
    }
}

void ParticleSystem::update(const Scene& scene, float dt) {
    if (dt <= 0.f) return;
    time_ += dt;
    std::vector<EntityId> alive;
    for (EntityId e : scene.entities()) {
        const ParticleEmitter* em = scene.get<ParticleEmitter>(e);
        if (!em || em->simulation == "gpu") continue;  // GPU emitters are simulated by the renderer
        alive.push_back(e);
        if (!scene.isActive(e)) continue;
        auto [it, inserted] = states_.try_emplace(e);
        State& s = it->second;
        Mat4 world = scene.worldMatrix(e);
        s.world = world;
        if (inserted || !s.started) {
            s.rng.reseed(static_cast<uint64_t>(e) * 7919u + static_cast<uint64_t>(em->seed) * 104729u + 17u);
            if (em->prewarm && em->rate > 0.f) {
                float warm = std::min(em->lifetime * (1.f + em->lifetimeJitter), 12.f);
                for (float t = 0; t < warm; t += 1.f / 20.f) step(s, *em, world, scene.environment(), 1.f / 20.f);
            }
        }
        step(s, *em, world, scene.environment(), dt);
    }
    std::sort(alive.begin(), alive.end());
    for (auto it = states_.begin(); it != states_.end();) {
        if (!std::binary_search(alive.begin(), alive.end(), it->first)) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

void ParticleSystem::gather(const Scene& scene, const ViewCamera& camera, std::vector<ParticleInstance>& out,
                            std::vector<LightItem>& lights) const {
    size_t first = out.size();
    Vec3 eye = camera.eye;
    Vec3 fwd = normalize(camera.target - camera.eye);
    for (const auto& [id, s] : states_) {
        if (!scene.exists(id) || !scene.isActive(id)) continue;
        const ParticleEmitter* em = scene.get<ParticleEmitter>(id);
        if (!em || em->simulation == "gpu") continue;
        float look = lookId(em->look);
        bool emissive = emissiveLook(look);
        Vec4 c0{toLinear(em->colorStart.x), toLinear(em->colorStart.y), toLinear(em->colorStart.z), em->colorStart.w};
        Vec4 c1{toLinear(em->colorEnd.x), toLinear(em->colorEnd.y), toLinear(em->colorEnd.z), em->colorEnd.w};
        float gain = emissive ? em->intensity : 1.f;
        Mat4 world = scene.worldMatrix(id);
        for (const Particle& p : s.particles) {
            if (out.size() - first >= kMaxRendered) break;
            Vec3 pos = em->worldSpace ? p.pos : world.transformPoint(p.pos);
            Vec3 vel = em->worldSpace ? p.vel : world.transformDir(p.vel);
            pos += vel * renderTimeOffset_;
            float t = std::clamp(p.age / p.life, 0.f, 1.f);
            float size = p.size0 + (p.size1 - p.size0) * t;
            if (dot(pos - eye, fwd) < -size * 2.f) continue;  // behind the camera
            ParticleInstance pi{};
            pi.position[0] = pos.x;
            pi.position[1] = pos.y;
            pi.position[2] = pos.z;
            pi.size = size;
            Vec4 c = p.splash ? c0 : Vec4{c0.x + (c1.x - c0.x) * t, c0.y + (c1.y - c0.y) * t, c0.z + (c1.z - c0.z) * t,
                                           c0.w + (c1.w - c0.w) * t};
            pi.color[0] = c.x * gain;
            pi.color[1] = c.y * gain;
            pi.color[2] = c.z * gain;
            pi.color[3] = c.w;
            pi.velocity[0] = vel.x * em->stretch;
            pi.velocity[1] = vel.y * em->stretch;
            pi.velocity[2] = vel.z * em->stretch;
            pi.rotation = p.rot;
            pi.age = t;
            pi.look = p.splash ? static_cast<float>(ParticleLook::Splash) : look;
            pi.seed = p.seed;
            pi.softness = em->softness;
            out.push_back(pi);
        }
        if (em->light > 0.f && s.energy > 0.01f) {
            LightItem li;
            li.kind = LightItem::Kind::Point;
            li.position = s.lightPos + Vec3{0, 0.15f, 0};
            li.color = em->lightColor.xyz();
            float flicker = 0.86f + 0.08f * std::sin(time_ * 17.f + static_cast<float>(id)) + 0.06f * std::sin(time_ * 31.7f);
            li.intensity = em->light * std::min(s.energy, 1.6f) * flicker;
            li.range = em->lightRange;
            lights.push_back(li);
        }
    }
    // Back to front: one sorted, globally blended stream (premultiplied alpha + additive).
    std::vector<std::pair<float, size_t>> keys;
    keys.reserve(out.size() - first);
    for (size_t i = first; i < out.size(); ++i) {
        const auto& p = out[i];
        Vec3 d{p.position[0] - eye.x, p.position[1] - eye.y, p.position[2] - eye.z};
        keys.emplace_back(-dot(d, d), i);
    }
    std::sort(keys.begin(), keys.end());
    std::vector<ParticleInstance> sorted;
    sorted.reserve(keys.size());
    for (const auto& [k, i] : keys) sorted.push_back(out[i]);
    std::copy(sorted.begin(), sorted.end(), out.begin() + static_cast<std::ptrdiff_t>(first));
}

// ---------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------

namespace {

struct Preset {
    const char* name;
    const char* json;
};

const Preset kParticlePresets[] = {
    {"fire", R"({"look":"flame","rate":34,"lifetime":0.6,"lifetimeJitter":0.35,"shape":"disc","shapeSize":[0.6,0,0.6],
        "speed":0.45,"speedJitter":0.5,"spread":6,"gravity":-1.4,"drag":2.5,"turbulence":0.5,"turbulenceScale":0.45,
        "sizeStart":1.0,"sizeEnd":0.75,"sizeJitter":0.35,"colorStart":"#ffb04c","colorEnd":"#ff7a26","intensity":2.7,
        "softness":0.25,"wind":0.35,"light":4,"lightColor":"#ffa45a","lightRange":9,"maxParticles":160})"},
    {"embers", R"({"look":"glow","rate":12,"lifetime":2.6,"lifetimeJitter":0.5,"shape":"disc","shapeSize":[0.5,0,0.5],
        "speed":1.9,"speedJitter":0.5,"spread":25,"gravity":-1.0,"drag":0.7,"turbulence":1.8,"turbulenceScale":0.7,
        "sizeStart":0.022,"sizeEnd":0.008,"colorStart":"#ffb44a","colorEnd":"#ff3c0a00","intensity":9,"stretch":0.02,
        "softness":0.05,"maxParticles":200})"},
    {"smoke", R"({"look":"smoke","rate":5,"lifetime":8,"lifetimeJitter":0.3,"shape":"disc","shapeSize":[0.4,0,0.4],
        "speed":1.0,"speedJitter":0.3,"spread":12,"gravity":-0.3,"drag":0.35,"turbulence":0.5,"turbulenceScale":2.2,
        "sizeStart":0.6,"sizeEnd":4.5,"sizeJitter":0.3,"colorStart":"#4a4644aa","colorEnd":"#8a8a8e00","spin":20,
        "softness":1.2,"wind":1,"maxParticles":120})"},
    {"steam", R"({"look":"smoke","rate":12,"lifetime":3,"lifetimeJitter":0.3,"shape":"disc","shapeSize":[0.3,0,0.3],
        "speed":1.2,"spread":10,"gravity":-0.9,"drag":0.6,"turbulence":0.7,"turbulenceScale":0.8,"sizeStart":0.2,
        "sizeEnd":1.6,"colorStart":"#e8ecf266","colorEnd":"#f4f6fa00","spin":30,"softness":0.6,"wind":1,"maxParticles":150})"},
    {"sparks", R"({"look":"spark","rate":50,"lifetime":0.9,"lifetimeJitter":0.4,"shape":"point","speed":6,"speedJitter":0.5,
        "spread":55,"gravity":9.8,"drag":0.3,"turbulence":0,"sizeStart":0.03,"sizeEnd":0.015,"colorStart":"#fff0b0",
        "colorEnd":"#ff5010","intensity":24,"stretch":0.045,"collide":true,"floorHeight":0,"bounce":0.35,"softness":0.02,
        "wind":0.2,"maxParticles":400})"},
    {"rain", R"({"look":"rain","rate":4500,"lifetime":1.7,"lifetimeJitter":0.15,"shape":"box","shapeSize":[36,0,36],
        "direction":[0,-1,0],"speed":9,"speedJitter":0.12,"spread":2,"gravity":9.8,"drag":0,"turbulence":0,
        "sizeStart":0.011,"sizeEnd":0.011,"sizeJitter":0.3,"colorStart":"#c8d2e070","colorEnd":"#c8d2e070","stretch":0.032,
        "collide":true,"floorHeight":0,"splash":2,"softness":0.02,"wind":1,"maxParticles":12000})"},
    {"snow", R"({"look":"snow","rate":700,"lifetime":11,"lifetimeJitter":0.2,"shape":"box","shapeSize":[40,0,40],
        "direction":[0,-1,0],"speed":0.8,"spread":20,"gravity":0.25,"drag":1.6,"turbulence":0.7,"turbulenceScale":2.5,
        "sizeStart":0.035,"sizeEnd":0.035,"sizeJitter":0.4,"colorStart":"#ffffffe6","colorEnd":"#ffffffe6","spin":90,
        "collide":true,"floorHeight":0,"softness":0.05,"wind":0.8,"maxParticles":9000})"},
    {"mist", R"({"look":"mist","rate":4,"lifetime":16,"lifetimeJitter":0.3,"shape":"box","shapeSize":[24,0.6,24],
        "speed":0.12,"spread":90,"gravity":0,"drag":0.4,"turbulence":0.25,"turbulenceScale":5,"sizeStart":5,"sizeEnd":8,
        "sizeJitter":0.3,"colorStart":"#c4ccd438","colorEnd":"#c4ccd400","spin":6,"softness":2.5,"wind":0.4,
        "maxParticles":90})"},
    {"spray", R"({"look":"mist","rate":40,"lifetime":1.6,"lifetimeJitter":0.4,"shape":"disc","shapeSize":[2,0,2],
        "speed":3,"speedJitter":0.5,"spread":35,"gravity":3,"drag":1.2,"turbulence":1,"turbulenceScale":1,"sizeStart":0.4,
        "sizeEnd":2.2,"colorStart":"#f2f6fa80","colorEnd":"#f2f6fa00","spin":30,"softness":0.8,"wind":1,"maxParticles":200})"},
    {"dust", R"({"look":"glow","rate":30,"lifetime":9,"lifetimeJitter":0.4,"shape":"box","shapeSize":[6,3,6],"speed":0.04,
        "spread":180,"gravity":0,"drag":1,"turbulence":0.12,"turbulenceScale":1.5,"sizeStart":0.012,"sizeEnd":0.012,
        "colorStart":"#fff4dc00","colorEnd":"#fff4dc00","intensity":0.9,"softness":0.02,"wind":0.1,"maxParticles":400})"},
    {"fireflies", R"({"look":"glow","rate":2.5,"lifetime":7,"lifetimeJitter":0.4,"shape":"box","shapeSize":[12,2,12],
        "speed":0.25,"spread":180,"gravity":0,"drag":0.8,"turbulence":0.9,"turbulenceScale":1.6,"sizeStart":0.06,
        "sizeEnd":0.04,"colorStart":"#d4ff6a","colorEnd":"#a6ff3000","intensity":9,"softness":0.05,"wind":0.1,
        "maxParticles":60})"},
    {"magic", R"({"look":"glow","rate":60,"lifetime":1.8,"lifetimeJitter":0.4,"shape":"sphere","shapeSize":[0.8,0.8,0.8],
        "speed":0.5,"spread":180,"gravity":-0.6,"drag":0.5,"turbulence":2.2,"turbulenceScale":0.6,"sizeStart":0.06,
        "sizeEnd":0.0,"colorStart":"#9a7aff","colorEnd":"#40e8ff00","intensity":12,"stretch":0.04,"light":2,
        "lightColor":"#8a7aff","lightRange":5,"softness":0.05,"maxParticles":300})"},
    {"fireball", R"({"look":"flame","rate":0,"burst":70,"lifetime":1.1,"lifetimeJitter":0.4,"shape":"sphere",
        "shapeSize":[1,1,1],"speed":5,"speedJitter":0.6,"spread":180,"gravity":-2,"drag":2.4,"turbulence":2,
        "turbulenceScale":1,"sizeStart":2.2,"sizeEnd":0.6,"sizeJitter":0.4,"colorStart":"#ffd890","colorEnd":"#ff400a",
        "intensity":12,"spin":60,"softness":0.5,"light":40,"lightColor":"#ff8a40","lightRange":30,"prewarm":false,
        "maxParticles":200})"},
    {"debris_smoke", R"({"look":"smoke","rate":0,"burst":30,"lifetime":6,"lifetimeJitter":0.4,"shape":"sphere",
        "shapeSize":[1.5,1.5,1.5],"speed":3,"speedJitter":0.6,"spread":180,"gravity":-0.8,"drag":1.4,"turbulence":0.8,
        "turbulenceScale":2,"sizeStart":1.5,"sizeEnd":7,"colorStart":"#2a2624cc","colorEnd":"#5a585800","spin":25,
        "softness":1.5,"wind":1,"prewarm":false,"maxParticles":60})"},
    {"shrapnel", R"({"look":"spark","rate":0,"burst":120,"lifetime":1.4,"lifetimeJitter":0.5,"shape":"sphere",
        "shapeSize":[0.5,0.5,0.5],"speed":14,"speedJitter":0.6,"spread":180,"gravity":9.8,"drag":0.4,"sizeStart":0.04,
        "sizeEnd":0.02,"colorStart":"#fff2c0","colorEnd":"#ff400a","intensity":30,"stretch":0.05,"collide":true,
        "floorHeight":0,"bounce":0.3,"softness":0.02,"prewarm":false,"maxParticles":300})"},
    // --- GPU particles (simulation "gpu": compute shaders, visuals only) -----------------------
    {"ember_storm", R"({"simulation":"gpu","look":"glow","facing":"velocity","rate":4000,"lifetime":4.5,"lifetimeJitter":0.5,
        "shape":"box","shapeSize":[16,3,16],"speed":0.6,"spread":180,"gravity":-0.35,"drag":0.9,
        "turbulence":3.2,"turbulenceScale":2.6,"wind":1.4,"sizeStart":0.022,"sizeEnd":0.008,
        "sizeJitter":0.5,"stretch":0.035,
        "colorGradient":"#fff3c4@0 #ffb347@0.15 #ff5a12@0.55 #a01800@0.85 #40000000@1",
        "opacityCurve":"0@0 1@0.08 1@0.8 0@1","intensity":7,"softness":0.05,"maxParticles":25000,
        "light":3,"lightColor":"#ff8a3a","lightRange":14})"},
    {"magic_vortex", R"({"simulation":"gpu","look":"glow","facing":"ribbon","trailLength":0.6,"trailSegments":24,"rate":1500,
        "lifetime":2.6,"lifetimeJitter":0.4,"shape":"disc","shapeSize":[2.4,0,2.4],"speed":0.3,
        "spread":30,"gravity":0,"drag":1.6,"turbulence":0.6,"turbulenceScale":1.2,"wind":0,
        "field":"vortex","fieldStrength":3.5,"fieldRadius":1.0,"fieldPull":1.5,"fieldLift":1.2,
        "sizeStart":0.022,"sizeEnd":0.008,
        "colorGradient":"#c8fbff@0 #50d8ff@0.25 #8a5cff@0.6 #ff40c800@1",
        "opacityCurve":"0@0 1@0.1 1@0.75 0@1","hueVariation":0.06,"intensity":2.5,"softness":0.05,
        "maxParticles":8000,"light":2,"lightColor":"#7a8cff","lightRange":7})"},
    {"dust_storm", R"({"simulation":"gpu","look":"smoke","rate":500,"lifetime":9,"lifetimeJitter":0.4,"shape":"box",
        "shapeSize":[44,5,44],"speed":0.8,"spread":60,"direction":[1,0.15,0],"gravity":-0.05,"drag":0.7,
        "turbulence":2.4,"turbulenceScale":7,"wind":1.8,"sizeStart":3,"sizeEnd":8,"sizeJitter":0.4,
        "colorStart":"#c2a27a18","colorEnd":"#c9b08c00","opacityCurve":"0@0 1@0.2 1@0.7 0@1","spin":12,
        "softness":2.5,"maxParticles":6000})"},
    {"falling_leaves", R"({"simulation":"gpu","look":"glow","facing":"mesh","mesh":"fx:leaf","rate":50,"lifetime":16,
        "lifetimeJitter":0.2,"shape":"box","shapeSize":[16,0,16],"direction":[0,-1,0],"speed":0.3,
        "spread":30,"gravity":1.6,"drag":2.2,"turbulence":1.4,"turbulenceScale":1.8,"wind":0.8,
        "sizeStart":0.14,"sizeEnd":0.14,"sizeJitter":0.35,"colorStart":"#c75b19","colorEnd":"#c75b19",
        "hueVariation":0.05,"spin":160,"roughness":0.55,"collide":true,"floorHeight":0,
        "depthCollision":true,"stick":true,"maxParticles":8000})"},
    {"snow_heavy", R"({"simulation":"gpu","look":"snow","rate":12000,"lifetime":14,"lifetimeJitter":0.2,"shape":"box",
        "shapeSize":[44,0,44],"direction":[0,-1,0],"speed":0.9,"spread":20,"gravity":1.4,"drag":1.4,
        "turbulence":1.1,"turbulenceScale":3,"wind":1,"sizeStart":0.032,"sizeEnd":0.032,
        "sizeJitter":0.45,"colorStart":"#ffffffee","colorEnd":"#ffffffee",
        "opacityCurve":"1@0 1@0.85 0@1","spin":90,"collide":true,"floorHeight":0,"depthCollision":true,
        "stick":true,"softness":0.04,"maxParticles":200000})"},
    {"smoke_column_gpu", R"({"simulation":"gpu","look":"smoke","rate":90,"lifetime":14,"lifetimeJitter":0.3,"shape":"disc",
        "shapeSize":[1.4,0,1.4],"speed":2.2,"speedJitter":0.3,"spread":10,"gravity":-0.55,"drag":0.35,
        "turbulence":0.9,"turbulenceScale":3.5,"wind":1,"sizeStart":1.1,"sizeEnd":10,"sizeJitter":0.3,
        "colorGradient":"#4a4644c0@0 #6a6866a0@0.35 #a4a4a800@1","spin":14,"softness":2,
        "maxParticles":2500})"},
    {"sparks_gpu", R"({"simulation":"gpu","look":"spark","facing":"velocity","rate":900,"lifetime":1.6,"lifetimeJitter":0.5,
        "shape":"point","direction":[0.4,0.7,0],"speed":7.5,"speedJitter":0.45,"spread":28,
        "gravity":9.8,"drag":0.18,"turbulence":0,"sizeStart":0.018,"sizeEnd":0.008,"stretch":0.028,
        "colorGradient":"#ffffff@0 #fff1a8@0.1 #ffa030@0.5 #ff3000@1","intensity":12,"collide":true,
        "floorHeight":0,"depthCollision":true,"bounce":0.38,"friction":0.35,"subEmitOn":"collision",
        "subEmitCount":2,"subEmitInherit":0.25,"softness":0.02,"wind":0.15,"maxParticles":60000,
        "light":2,"lightColor":"#ffa04a","lightRange":7})"},
    {"embers_gpu", R"({"simulation":"gpu","look":"glow","facing":"velocity","rate":0,"lifetime":1.1,"lifetimeJitter":0.5,
        "shape":"point","speed":0.9,"speedJitter":0.6,"spread":70,"gravity":1.5,"drag":1.6,"turbulence":0.6,
        "turbulenceScale":0.4,"sizeStart":0.014,"sizeEnd":0.004,"stretch":0.02,"colorGradient":"#ffd27a@0 #ff6a10@0.5 #60080000@1",
        "intensity":12,"collide":true,"floorHeight":0,"depthCollision":true,"bounce":0.2,"softness":0.02,"prewarm":false,
        "maxParticles":60000})"},
    {"rain_gpu", R"({"simulation":"gpu","look":"rain","facing":"velocity","rate":32000,"lifetime":1.6,"lifetimeJitter":0.15,
        "shape":"box","shapeSize":[36,0,36],"direction":[0,-1,0],"speed":10,"speedJitter":0.12,"spread":2,"gravity":9.8,
        "drag":0,"turbulence":0,"sizeStart":0.011,"sizeEnd":0.011,"sizeJitter":0.3,"colorStart":"#c8d2e070",
        "colorEnd":"#c8d2e070","stretch":0.032,"collide":true,"floorHeight":0,"depthCollision":true,"subEmitOn":"collision",
        "subEmitCount":2,"subEmitInherit":0,"softness":0.02,"wind":1,"maxParticles":80000})"},
    {"splashes_gpu", R"({"simulation":"gpu","look":"rain","facing":"velocity","rate":0,"lifetime":0.28,"lifetimeJitter":0.3,
        "shape":"point","direction":[0,1,0],"speed":1.8,"speedJitter":0.4,"spread":50,"gravity":9.8,"drag":0,
        "sizeStart":0.008,"sizeEnd":0.006,"stretch":0.025,"colorStart":"#d4dcea90","colorEnd":"#d4dcea00","softness":0.01,
        "prewarm":false,"maxParticles":120000})"},
    {"rockets_gpu", R"({"simulation":"gpu","look":"spark","facing":"ribbon","trailLength":0.55,"trailSegments":14,"rate":1.4,
        "lifetime":1.45,"lifetimeJitter":0.2,"shape":"disc","shapeSize":[8,0,8],"speed":15,
        "speedJitter":0.15,"spread":6,"gravity":9.8,"drag":0.1,"sizeStart":0.05,"sizeEnd":0.03,
        "colorGradient":"#fff4d0@0 #ffb050@1","intensity":6,"subEmitOn":"death","subEmitCount":900,
        "subEmitInherit":0.15,"softness":0.05,"prewarm":false,"maxParticles":64,"light":2,
        "lightColor":"#ffc07a","lightRange":10})"},
    {"firework_burst_gpu", R"({"simulation":"gpu","look":"spark","facing":"ribbon","trailLength":0.5,"trailSegments":12,"rate":0,
        "lifetime":2.2,"lifetimeJitter":0.35,"shape":"point","speed":9,"speedJitter":0.12,"spread":180,
        "gravity":3.2,"drag":1.3,"sizeStart":0.06,"sizeEnd":0.02,
        "colorGradient":"#ffffff@0 #ff5a8c@0.12 #ff2a6a@0.6 #50001800@1","opacityCurve":"1@0 1@0.6 0@1",
        "hueVariation":1,"intensity":4,"softness":0.05,"prewarm":false,"maxParticles":30000,"light":4,
        "lightColor":"#ff8ab0","lightRange":30})"},
    {"mist_gpu", R"({"simulation":"gpu","look":"mist","rate":160,"lifetime":5,"lifetimeJitter":0.4,"shape":"box",
        "shapeSize":[6,0.4,2],"speed":2.4,"speedJitter":0.5,"spread":65,"gravity":-0.25,"drag":1.1,"turbulence":1.2,
        "turbulenceScale":2.5,"wind":1,"sizeStart":1.2,"sizeEnd":6,"sizeJitter":0.4,"colorStart":"#eef4fa55",
        "colorEnd":"#eef4fa00","opacityCurve":"0@0 1@0.15 0.6@0.6 0@1","spin":20,"softness":1.8,"maxParticles":4000})"},
    {"droplets_gpu", R"({"simulation":"gpu","look":"rain","facing":"velocity","rate":3500,"lifetime":1.1,"lifetimeJitter":0.4,
        "shape":"box","shapeSize":[6,0.3,1.5],"speed":4.5,"speedJitter":0.5,"spread":55,"gravity":9.8,"drag":0.4,
        "sizeStart":0.012,"sizeEnd":0.008,"stretch":0.02,"colorStart":"#dfe8f2a0","colorEnd":"#dfe8f200","collide":true,
        "floorHeight":0,"depthCollision":true,"softness":0.02,"wind":0.6,"maxParticles":8000})"},
};

const Preset kFluidPresets[] = {
    {"volume_fire", R"({"size":[2.0,2.8,2.0],"resolution":100,"sourceOffset":[0,0.1,0],"sourceRadius":0.55,"fuel":1.5,
        "heat":1.0,"smoke":0.5,"buoyancy":0.55,"vorticity":1.1,"turbulence":1.4,"burnRate":5.0,"cooling":4.5,
        "smokeFade":0.3,"speed":0.2,"flameIntensity":1.1,"flameTemperature":1750,"smokeColor":"#2a2725",
        "smokeDensity":0.9,"light":5,"lightColor":"#ffa055","lightRange":10})"},
    {"volume_torch", R"({"size":[0.5,1.2,0.5],"resolution":64,"sourceOffset":[0,0.05,0],"sourceRadius":0.08,"fuel":1.1,
        "heat":1.0,"smoke":0.25,"buoyancy":1.1,"vorticity":0.6,"turbulence":0.5,"burnRate":2.6,"cooling":1.8,
        "smokeFade":0.6,"speed":0.5,"flameIntensity":1.0,"flameTemperature":1750,"smokeColor":"#2a2725",
        "smokeDensity":0.6,"light":2.6,"lightColor":"#ffa055","lightRange":7})"},
    {"volume_smoke", R"({"size":[4,12,4],"resolution":96,"sourceOffset":[0,0.3,0],"sourceRadius":0.7,"fuel":0,"heat":0.8,
        "smoke":1.2,"buoyancy":0.6,"vorticity":0.35,"turbulence":0.5,"cooling":0.25,"smokeFade":0.08,"speed":1.2,
        "smokeColor":"#3a3836","smokeDensity":1.0,"light":0})"},
    {"steam_vent", R"({"size":[1.2,4,1.2],"resolution":72,"sourceOffset":[0,0.05,0],"sourceRadius":0.18,"fuel":0,
        "heat":0.9,"smoke":1.0,"buoyancy":0.9,"vorticity":0.5,"turbulence":0.7,"cooling":0.9,"smokeFade":0.55,"speed":1.6,
        "smokeColor":"#dfe4ea","smokeDensity":0.45,"light":0})"},
    {"explosion_volume", R"({"size":[10,12,10],"resolution":110,"sourceOffset":[0,1.2,0],"sourceRadius":1.2,"fuel":4,
        "heat":1.6,"smoke":3.2,"buoyancy":1.5,"vorticity":2.2,"turbulence":3.0,"burnRate":6.0,"cooling":2.8,
        "smokeFade":0.06,"speed":9,"flameIntensity":1.4,"flameTemperature":2100,"smokeColor":"#1c1a19","smokeDensity":2.6,
        "light":30,"lightColor":"#ff9a4a","lightRange":40,"emitting":false,"burst":0.16})"},
};

// Composite items: {"name", "preset" (particle preset), or "fluid" (fluid preset), "position",
// optional "particles"/"fluid_overrides" patches}.
const Preset kComposites[] = {
    {"campfire", R"([{"name":"Fire","fluid":"volume_fire","position":[0,0,0]},
                    {"name":"Embers","preset":"embers","position":[0,0.3,0]}])"},
    {"torch", R"([{"name":"Flame","fluid":"volume_torch","position":[0,0,0]},
                 {"name":"Embers","preset":"embers","position":[0,0.1,0],"particles":{"rate":3,"shapeSize":[0.08,0,0.08]}}])"},
    {"explosion", R"([{"name":"Fireball","fluid":"explosion_volume","position":[0,0,0]},
                     {"name":"Shrapnel","preset":"shrapnel","position":[0,0.5,0]}])"},
    {"burning_barrel", R"([{"name":"Fire","fluid":"volume_fire","position":[0,0,0],"fluid_overrides":{"size":[1,2.4,1],
                            "sourceRadius":0.24,"resolution":80,"light":4,"lightRange":8}},
                          {"name":"Embers","preset":"embers","position":[0,0.2,0],"particles":{"rate":7,"shapeSize":[0.4,0,0.4]}}])"},
    {"sparks_shower", R"([{"name":"Sparks","preset":"sparks_gpu","position":[0,1.2,0],"particles":{"subEmitter":"Embers"}},
                          {"name":"Embers","preset":"embers_gpu","position":[0,0,0]}])"},
    {"fireworks", R"([{"name":"Rockets","preset":"rockets_gpu","position":[0,0,0],"particles":{"subEmitter":"Bursts"}},
                      {"name":"Bursts","preset":"firework_burst_gpu","position":[0,0,0]}])"},
    {"rain_heavy", R"([{"name":"Rain","preset":"rain_gpu","position":[0,12,0],"particles":{"subEmitter":"Splashes"}},
                       {"name":"Splashes","preset":"splashes_gpu","position":[0,0,0]}])"},
    {"waterfall_mist", R"([{"name":"Mist","preset":"mist_gpu","position":[0,0.2,0]},
                           {"name":"Droplets","preset":"droplets_gpu","position":[0,0.3,0]}])"},
    {"sprite_campfire", R"([{"name":"Flames","preset":"fire","position":[0,0.05,0]},
                           {"name":"Embers","preset":"embers","position":[0,0.3,0]},
                           {"name":"Smoke","preset":"smoke","position":[0,1.4,0]}])"},
};

const Preset kWaterPresets[] = {
    {"ocean", R"({"windSpeed":9,"choppiness":1.3,"waveScale":1,"patchSize":240,"size":0,"depth":80,
        "deepColor":"#03141c","shallowColor":"#1f8a7c","clarity":7,"foam":1,"roughness":0.04})"},
    {"calm_sea", R"({"windSpeed":4.5,"choppiness":0.9,"waveScale":0.8,"patchSize":160,"size":0,"depth":40,
        "deepColor":"#05202a","shallowColor":"#2aa595","clarity":10,"foam":0.5,"roughness":0.03})"},
    {"storm", R"({"windSpeed":19,"choppiness":1.8,"waveScale":1,"patchSize":420,"size":0,"depth":200,
        "deepColor":"#0b1a1e","shallowColor":"#3a6a66","clarity":3,"foam":2.4,"roughness":0.07})"},
    {"lake", R"({"windSpeed":3,"choppiness":0.8,"waveScale":0.7,"patchSize":60,"size":300,"depth":8,
        "deepColor":"#0a1c16","shallowColor":"#4a7a4c","clarity":3,"foam":0.2,"roughness":0.05})"},
    {"puddle", R"({"windSpeed":0.4,"choppiness":0.3,"waveScale":0.04,"patchSize":4,"size":2.5,"depth":0.05,
        "deepColor":"#020304","shallowColor":"#3a3834","clarity":0.6,"foam":0,"roughness":0.02})"},
    {"pool", R"({"windSpeed":1.5,"choppiness":0.5,"waveScale":0.6,"patchSize":18,"size":12,"depth":2,
        "deepColor":"#0a5a7a","shallowColor":"#7ae6f0","clarity":30,"foam":0,"roughness":0.02})"},
};

template <size_t N>
std::vector<std::string> names(const Preset (&list)[N]) {
    std::vector<std::string> out;
    for (const auto& p : list) out.emplace_back(p.name);
    return out;
}

template <size_t N>
Json find(const Preset (&list)[N], const std::string& name, bool array) {
    for (const auto& p : list) {
        if (name == p.name) {
            auto j = Json::parse(p.json);
            if (j) return j.value();
        }
    }
    return array ? Json::array() : Json::object();
}

}  // namespace

const std::vector<std::string>& particlePresets() {
    static const std::vector<std::string> n = names(kParticlePresets);
    return n;
}

Json particlePreset(const std::string& name) {
    Json j = find(kParticlePresets, name, false);
    if (j.size()) j["preset"] = name;
    return j;
}

const std::vector<std::string>& compositeEffects() {
    static const std::vector<std::string> n = names(kComposites);
    return n;
}

Json compositeEffect(const std::string& name) {
    Json list = find(kComposites, name, true);
    for (auto& item : list.elements()) {
        if (item.contains("fluid")) {
            Json patch = fluidPreset(item.get("fluid").asString());
            for (const auto& [k, v] : item.get("fluid_overrides").members()) patch[k] = v;
            item["fluid"] = patch;
        } else {
            Json patch = particlePreset(item.get("preset").asString());
            for (const auto& [k, v] : item.get("particles").members()) patch[k] = v;
            item["particles"] = patch;
        }
    }
    return list;
}

const std::vector<std::string>& fluidPresets() {
    static const std::vector<std::string> n = names(kFluidPresets);
    return n;
}

Json fluidPreset(const std::string& name) {
    Json j = find(kFluidPresets, name, false);
    if (j.size()) j["preset"] = name;
    return j;
}

const std::vector<std::string>& waterPresets() {
    static const std::vector<std::string> n = names(kWaterPresets);
    return n;
}

Json waterPreset(const std::string& name) { return find(kWaterPresets, name, false); }

}  // namespace sky::fx
