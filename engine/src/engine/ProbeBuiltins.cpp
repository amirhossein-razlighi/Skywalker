// Wander builtins for reflection probes (docs/RENDERING.md "Reflection probes").

#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

using namespace wander;

void registerProbeBuiltins(BuiltinRegistry& reg) {
    BuiltinDef bake;
    bake.name = "probe_bake";
    bake.params = {{"probe", kTEntity, true}};
    bake.returns = kTNone;
    bake.category = "render";
    bake.doc = "Re-captures a reflection probe (or every probe without an argument) over the next rendered frames: after "
               "opening a door, switching the room's lights or rearranging furniture, so `once` probes reflect the new "
               "state. Rendering only; the simulation is unaffected.";
    bake.example = "probe_bake(find(\"Hall Probe\"))";
    bake.owner = "engine";
    bake.fn = [](CallContext& c) -> Value {
        EntityId target = 0;
        if (c.argc() == 1) {
            target = c.entity(0);
            if (!c.scene().get<ReflectionProbe>(target)) c.fail("probe_bake(): the entity has no reflection_probe component");
        }
        if (Engine* engine = c.service<Engine>()) engine->renderer().invalidateReflectionProbes(target);
        return Value();
    };
    reg.add(std::move(bake));
}

}  // namespace sky
