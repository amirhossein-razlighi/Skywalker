// Wander builtins that need the engine (effects, water, ...). Every subsystem adds its
// builtins here through the registry: write `registerXxxBuiltins(reg)` in your module
// and call it from registerEngineBuiltins below. Implementations reach the engine with
// `c.service<Engine>()` (null in sandboxes and bare runtimes: degrade gracefully).

#include <algorithm>
#include <mutex>

#include "skywalker/engine/Engine.h"
#include "skywalker/ui/World2D.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

void registerRenderLayerBuiltins(wander::BuiltinRegistry& reg);  // RenderBuiltins.cpp: layer_mask
void registerCharacterBuiltins(wander::BuiltinRegistry& reg);  // CharacterBuiltins.cpp: hand_ik, foot_ik, turn_in_place, look_at

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

// Animation (AnimationSystem through the runtime's `animation` hook).
Value animationCall(CallContext& c, const char* fn) {
    Engine* engine = c.service<Engine>();
    if (!engine || !engine->runtime().animation) c.fail(std::string(fn) + "() is not available in this context");
    EntityRef target = c.entity(0);
    std::vector<Json> rest;
    for (int i = 1; i < c.argc(); ++i) rest.push_back(toJson(c.arg(i)));
    auto r = engine->runtime().animation(fn, target, rest);
    if (!r) c.fail(std::string(fn) + "(): " + r.error().message + (r.error().hint.empty() ? "" : " (" + r.error().hint + ")"));
    return fromJson(r.value());
}

void registerAnimationBuiltins(BuiltinRegistry& reg) {
    auto def = [&](const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* doc, const char* example,
                   BuiltinImpl fn) {
        BuiltinDef d;
        d.name = name;
        d.params = std::move(params);
        d.returns = returns;
        d.category = "animation";
        d.doc = doc;
        d.example = example;
        d.owner = "engine";
        d.fn = fn;
        reg.add(std::move(d));
    };
    def("set_param", {{"entity", kTEntity}, {"name", kTString}, {"value", kTNumber | kTBool}}, kTNone,
        "Sets an animator controller parameter (blend spaces and transitions read them).", "set_param(self, \"speed\", 3)",
        [](CallContext& c) { return animationCall(c, "set_param"); });
    def("trigger", {{"entity", kTEntity}, {"name", kTString}}, kTNone, "Fires an animator trigger (jump, attack, wave).",
        "trigger(self, \"jump\")", [](CallContext& c) { return animationCall(c, "trigger"); });
    def("play_animation", {{"entity", kTEntity}, {"clip", kTString}, {"fade", kTNumber, true}, {"loop", kTBool, true}}, kTNone,
        "Crossfades straight to a clip or state (seconds of fade, default 0.2).", "play_animation(self, \"wave\", 0.3)",
        [](CallContext& c) { return animationCall(c, "play_animation"); });
    def("anim_state", {{"entity", kTEntity}}, kTString, "Name of the animator's current state.", "if anim_state(self) == \"idle\" then",
        [](CallContext& c) { return animationCall(c, "anim_state"); });
    def("play_sequence", {{"entity", kTEntity}, {"start", kTNumber, true}}, kTNone,
        "Plays the entity's sequencer (a cutscene) from `start` seconds.", "play_sequence(find(\"Intro\"))",
        [](CallContext& c) { return animationCall(c, "play_sequence"); });
}

// Player/platform builtins (the standalone player; the editor ignores them).
void registerPlatformBuiltins(BuiltinRegistry& reg) {
    BuiltinDef lock;
    lock.name = "cursor_lock";
    lock.params = {{"on", kTBool}};
    lock.returns = kTNone;
    lock.category = "input";
    lock.doc = "Hides and captures the mouse cursor (first-person look: read mouse movement with axis(\"look\")) or gives it "
               "back. Honored by the standalone player; resets when the game stops.";
    lock.example = "cursor_lock(true)";
    lock.owner = "engine";
    lock.fn = [](CallContext& c) -> Value {
        if (Engine* engine = c.service<Engine>()) engine->setCursorLocked(c.boolean(0));
        return Value();
    };
    reg.add(std::move(lock));

    BuiltinDef quit;
    quit.name = "quit_game";
    quit.returns = kTNone;
    quit.category = "input";
    quit.doc = "Closes the game (a \"Quit\" menu button). The standalone player exits; the editor ignores it.";
    quit.example = "quit_game()";
    quit.owner = "engine";
    quit.fn = [](CallContext& c) -> Value {
        if (Engine* engine = c.service<Engine>()) engine->requestQuit();
        return Value();
    };
    reg.add(std::move(quit));
}

}  // namespace

void registerFlowBuiltins(wander::BuiltinRegistry& reg);  // EngineFlow.cpp

void registerEngineBuiltins() {
    static std::once_flag once;
    std::call_once(once, [] {
        BuiltinRegistry& reg = BuiltinRegistry::global();
        registerEffectsBuiltins(reg);
        registerAnimationBuiltins(reg);
        registerUiBuiltins(reg);  // 2D, UI, dialogue (World2D)
        registerPlatformBuiltins(reg);
        registerFlowBuiltins(reg);  // pause_game, time_scale, teleport (EngineFlow.cpp)
        // Subsystem builtins: one line each.
        registerRenderLayerBuiltins(reg);  // render layers (RenderBuiltins.cpp)
        registerCharacterBuiltins(reg);  // character IK and turning (CharacterBuiltins.cpp)
    });
}

}  // namespace sky
