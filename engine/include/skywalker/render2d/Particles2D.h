#pragma once
// Simulation of `particles2d` emitters (pixel-art weather, leaves, fireflies, puffs, ripples).
// Deterministic: fixed ticks, a per-emitter seeded generator, no wall clock. Drawn by gather2D.

#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Components.h"
#include "skywalker/scene/Process.h"

namespace sky {
class Scene;
}

namespace sky::render2d {

class Assets2D;

/// The sheet frames an emitter draws (its `frames` selector over the texture's frames; [0] when untextured).
std::vector<int> particleFrames(Assets2D& assets, const Particles2D& p);

/// Advances one emitter by dt seconds (spawning at `origin`, the entity's world position).
void stepParticles2D(Particles2D& p, Vec3 origin, float dt, int frameCount, uint64_t entitySalt);

/// Advances every active particles2d emitter by one tick (paused entities hold still).
void tickParticles2D(Scene& scene, Assets2D& assets, float dt, const ProcessGate* gate = nullptr);

/// Where particle `q` of `p` is drawn: its world position (wrap emitters pick the copy of the endless
/// tiling nearest to `eye`).
Vec3 particleWorldPosition(const Particles2D& p, const Particle2D& q, Vec3 origin, Vec3 eye);

/// Presets for particles2d_create: rain, drizzle, snow, leaves, petals, fireflies, smoke, ripples, dust, sparkle.
const std::vector<std::string>& particles2dPresets();
/// The component fields of a preset (JSON patch for the particles2d component); null if unknown.
Json particles2dPreset(const std::string& name);

}  // namespace sky::render2d
