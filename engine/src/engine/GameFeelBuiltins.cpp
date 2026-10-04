// Game-feel builtins for action games: hit_stop (freeze frames on impacts), camera_shake (trauma-based
// 2D camera shake) and flash (a sprite flashes a color and fades). Deterministic: everything is
// counted in fixed ticks. Docs: docs/2D_AND_UI.md "Painted 2D: depth, motion and impact".

#include <algorithm>

#include "skywalker/engine/Engine.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

void def(BuiltinRegistry& reg, const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* doc,
         const char* example, BuiltinImpl fn) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = returns;
    d.category = "effects";
    d.doc = doc;
    d.example = example;
    d.owner = "engine";
    d.fn = fn;
    reg.add(std::move(d));
}

/// The camera2d a shake applies to: the given entity, else the active scene camera with camera2d, else any.
Camera2D* shakeTarget(CallContext& c, int argIndex) {
    Scene& s = c.scene();
    if (c.argc() > argIndex) {
        Camera2D* cam = s.get<Camera2D>(c.entity(argIndex));
        if (!cam) c.fail("camera_shake(): the entity has no camera2d component");
        return cam;
    }
    if (EntityId e = render2d::activeCamera(s); e) {
        if (Camera2D* cam = s.get<Camera2D>(e)) return cam;
    }
    for (EntityId e : s.entities()) {
        if (Camera2D* cam = s.get<Camera2D>(e)) return cam;
    }
    return nullptr;
}

}  // namespace

void registerGameFeelBuiltins(BuiltinRegistry& reg) {
    def(reg, "hit_stop", {{"seconds", kTNumber}, {"scale", kTNumber, true}}, kTNone,
        "Freeze frames on impact: the game clock stops (or runs at `scale`, 0..1) for `seconds` of real time, then "
        "resumes at the time scale. 0.04-0.12 s sells a hit; a longer, stronger stop extends the current one. UI, "
        "camera_shake and entities on the real clock keep moving. Per entity: set `process.timeScale` instead.",
        "on event \"hit\"\n  hit_stop(0.07)\n  camera_shake(0.35)\nend",
        [](CallContext& c) -> Value {
            c.runtime().requestHitStop(c.number(0), c.argc() > 1 ? c.number(1) : 0.0);
            return Value();
        });
    def(reg, "camera_shake", {{"trauma", kTNumber}, {"camera", kTEntity, true}}, kTNone,
        "Shakes the 2D camera: adds `trauma` (0..1; 0.2 a hit, 0.5 a heavy slam, 1 an explosion) to the camera2d (the "
        "active camera, or the given one). The offset grows with trauma squared (camera2d.shakeAmplitude world units at "
        "1) and decays by shakeDecay per second. Runs through hit-stops; replays identically.",
        "camera_shake(0.4)", [](CallContext& c) -> Value {
            if (Camera2D* cam = shakeTarget(c, 1)) render2d::addCameraShake(*cam, static_cast<float>(c.number(0)));
            return Value();
        });
    def(reg, "flash", {{"entity", kTEntity}, {"seconds", kTNumber, true}, {"color", kTColor, true}}, kTNone,
        "Flashes a sprite: it turns `color` (default white; its alpha is the strength) and fades back over `seconds` "
        "(default 0.12). Damage feedback, parries, pickups. For a constant tint set sprite.flash.",
        "flash(enemy, 0.15, #ffffff)", [](CallContext& c) -> Value {
            Sprite* sp = c.scene().get<Sprite>(c.entity(0));
            if (!sp) c.fail("flash(): the entity has no sprite component");
            const float seconds = c.argc() > 1 ? static_cast<float>(c.number(1)) : 0.12f;
            const Vec4 color = c.argc() > 2 ? c.color(2) : Vec4{1.f, 1.f, 1.f, 1.f};
            render2d::flashSprite(*sp, std::clamp(seconds, 0.01f, 10.f), color);
            return Value();
        });
    def(reg, "hit_stop_left", {}, kTNumber, "Real seconds of hit-stop left (0 when the game clock runs normally).",
        "if hit_stop_left() > 0 then return end",
        [](CallContext& c) -> Value { return Value::number(c.runtime().hitStopRemaining()); });
}

}  // namespace sky
