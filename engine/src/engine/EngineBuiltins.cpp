// Wander builtins that need the engine (effects, water, ...). Every subsystem adds its
// builtins here through the registry: write `registerXxxBuiltins(reg)` in your module
// and call it from registerEngineBuiltins below. Implementations reach the engine with
// `c.service<Engine>()` (null in sandboxes and bare runtimes: degrade gracefully).

#include <algorithm>
#include <mutex>

#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

void registerEffectsBuiltins(BuiltinRegistry& reg) {
    BuiltinDef burst;
    burst.name = "burst";
    burst.params = {{"entity_or_count", kTEntity | kTNumber}, {"count", kTNumber, true}};
    burst.returns = kTNone;
    burst.category = "effects";
    burst.doc = "Emits particles right now from self's particles component: burst(n), or burst(entity, n) for another "
                "emitter. Explosions, muzzle flashes, impacts.";
    burst.example = "burst(40)";
    burst.owner = "engine";
    burst.fn = [](CallContext& c) -> Value {
        EntityRef target = c.self();
        double count = 0;
        if (c.argc() == 2) {
            target = c.entity(0);
            count = c.number(1);
        } else {
            count = c.number(0);
        }
        if (!c.scene().get<ParticleEmitter>(target)) c.fail("burst(): the entity has no particles component");
        if (Engine* engine = c.service<Engine>()) {
            engine->particles().burst(target, static_cast<int>(std::clamp(count, 0.0, 20000.0)));
        }
        return Value();
    };
    reg.add(std::move(burst));

    BuiltinDef water;
    water.name = "water_height";
    water.params = {{"x_or_point", kTNumber | kTVec | kTEntity}, {"z", kTNumber, true}};
    water.returns = kTNumber;
    water.category = "effects";
    water.doc = "Height of the animated water surface at (x, z) or at a point — the same surface that is rendered "
                "(0 where there is no water). Boats, buoyancy, splashes.";
    water.example = "self.position = (self.position.x, water_height(self.position) - 0.2, self.position.z)";
    water.owner = "engine";
    water.fn = [](CallContext& c) -> Value {
        float x, z;
        if (c.argc() == 1) {
            Vec3 p = c.point(0);
            x = p.x;
            z = p.z;
        } else {
            x = static_cast<float>(c.number(0));
            z = static_cast<float>(c.number(1));
        }
        float h = 0;
        if (Engine* engine = c.service<Engine>()) {
            if (!engine->waterHeight(x, z, h)) h = 0;
        }
        return Value::number(h);
    };
    reg.add(std::move(water));
}

}  // namespace

void registerEngineBuiltins() {
    static std::once_flag once;
    std::call_once(once, [] {
        BuiltinRegistry& reg = BuiltinRegistry::global();
        registerEffectsBuiltins(reg);
        // Subsystem builtins: one line each.
    });
}

}  // namespace sky
