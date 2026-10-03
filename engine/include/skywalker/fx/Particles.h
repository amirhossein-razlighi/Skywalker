#pragma once
// Particle simulation for `particles` components (fire, smoke, embers, sparks, rain, snow,
// mist, magic).
//
// CPU-side and deterministic: every emitter has its own seeded generator, so a play session
// replays exactly (agents can verify effects frame by frame). Forces: gravity/buoyancy,
// drag, environment wind and a divergence-free swirl field (sum of shear waves — cheap
// curl noise). Particles can collide with a floor plane, bounce, or splash into droplets.
// Fire emitters can light the scene: the light's strength follows the live flame energy,
// so it flickers naturally with the simulation.
//
// The renderer receives sorted ParticleInstances (see Renderer.h); the look (flame, smoke,
// rain...) is shaded procedurally on the GPU.

#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Random.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/Scene.h"

namespace sky::fx {

struct Particle {
    Vec3 pos;
    Vec3 vel;
    float age = 0, life = 1;
    float size0 = 0, size1 = 0;
    float rot = 0, spin = 0;
    float seed = 0;
    bool splash = false;  // a droplet spawned by an impact
};

class ParticleSystem {
public:
    /// Advances every emitter in the scene by dt seconds (removes state of deleted ones).
    void update(const Scene& scene, float dt);
    /// Forgets all particles (play/stop boundaries keep simulations reproducible).
    void reset();
    /// Emits `count` particles from an emitter right away (Wander burst()).
    void burst(EntityId emitter, int count);

    /// Appends the visible particles (sorted back to front) and the lights fires cast.
    void gather(const Scene& scene, const ViewCamera& camera, std::vector<ParticleInstance>& out,
                std::vector<LightItem>& lights) const;

    size_t liveCount(EntityId emitter) const;
    size_t totalLive() const;
    size_t emitterCount() const { return states_.size(); }

    static constexpr size_t kMaxRendered = 120000;

private:
    struct State {
        std::vector<Particle> particles;
        Random rng;
        float spawnCarry = 0;
        int pendingBurst = 0;
        bool started = false;
        float energy = 0;  // smoothed flame energy (drives the cast light)
        Vec3 lightPos;
        Mat4 world;
    };
    void step(State& s, const ParticleEmitter& em, const Mat4& world, const Environment& env, float dt);
    void spawn(State& s, const ParticleEmitter& em, const Mat4& world, int count);
    std::unordered_map<EntityId, State> states_;
    float time_ = 0;
};

/// Divergence-free swirl velocity at p (m/s per unit strength).
Vec3 swirl(Vec3 p, float time, float scale);

/// Named starting points: field patches for a `particles` component.
const std::vector<std::string>& particlePresets();
/// Returns the component patch for a preset (empty object for unknown names).
Json particlePreset(const std::string& name);
/// Composite effects (campfire, torch, explosion...): several emitters as children.
/// Each entry: {"name": child name, "position": [x,y,z]} and either "particles" or "fluid" (patch).
Json compositeEffect(const std::string& name);
const std::vector<std::string>& compositeEffects();

/// Volumetric fluid presets ("volume_fire", "volume_torch", "volume_smoke", "steam_vent",
/// "explosion_volume"): patches for a `fluid` component.
const std::vector<std::string>& fluidPresets();
Json fluidPreset(const std::string& name);

/// Water presets ("ocean", "calm_sea", "storm", "lake", "pool"): patches for a `water` component.
const std::vector<std::string>& waterPresets();
Json waterPreset(const std::string& name);

}  // namespace sky::fx
