// Wander builtins for characters (docs/CHARACTERS.md): hand and foot IK, turning in place, look-at.

#include <cmath>

#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

/// The animator entity of a character (the entity itself or its nearest animated ancestor).
EntityId characterOf(CallContext& c, int arg) {
    EntityId e = c.entity(arg);
    Engine* engine = c.service<Engine>();
    EntityId a = engine ? engine->animation().animatorFor(e) : kNoEntity;
    if (!a) c.fail(c.def().name + "(): " + c.display(c.arg(arg)) + " has no animator (add one with animator_setup)");
    return a;
}

void patchIk(CallContext& c, EntityId e, const Json& patch) {
    if (Status s = c.scene().patchComponent(e, "characterIk", patch); !s) c.fail(c.def().name + "(): " + s.error().message);
}

void def(BuiltinRegistry& reg, const char* name, std::vector<BuiltinParam> params, const char* doc, const char* example, BuiltinImpl fn) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = kTNone;
    d.category = "animation";
    d.doc = doc;
    d.example = example;
    d.owner = "engine";
    d.fn = fn;
    reg.add(std::move(d));
}

}  // namespace

void registerCharacterBuiltins(BuiltinRegistry& reg) {
    def(reg, "hand_ik", {{"entity", kTEntity}, {"side", kTString}, {"target", kTEntity | kTNone}, {"weight", kTNumber, true}},
        "Makes a character's hand reach an entity (\"left\" or \"right\"): a grip point on a staff held in the other hand, a "
        "ledge, a rail, a door handle. `none` (or weight 0) releases it. Adds the characterIk component if needed.",
        "hand_ik(self, \"left\", find(\"Staff Grip\"), 1)", [](CallContext& c) -> Value {
            EntityId e = characterOf(c, 0);
            const std::string& side = c.string(1);
            if (side != "left" && side != "right") c.fail("hand_ik(): side is \"left\" or \"right\", not \"" + side + "\"");
            const bool none = c.arg(2).isNone();
            float w = c.argc() > 3 ? static_cast<float>(c.number(3)) : 1.f;
            Json link = none ? Json() : Json(static_cast<int64_t>(c.entity(2)));
            patchIk(c, e, Json::object({{side + "Hand", link}, {side + "HandWeight", none ? 0.f : std::clamp(w, 0.f, 1.f)}}));
            return Value();
        });
    def(reg, "foot_ik", {{"entity", kTEntity}, {"on", kTBool}, {"weight", kTNumber, true}},
        "Turns automatic foot planting on or off (feet on stairs, slopes and rocks, pelvis lowered, feet locked while planted); "
        "the optional weight fades it (0..1). Adds the characterIk component if needed.",
        "foot_ik(self, not swimming)", [](CallContext& c) -> Value {
            EntityId e = characterOf(c, 0);
            Json patch = Json::object({{"feet", c.boolean(1)}});
            if (c.argc() > 2) patch["feetWeight"] = std::clamp(static_cast<float>(c.number(2)), 0.f, 1.f);
            patchIk(c, e, patch);
            return Value();
        });
    def(reg, "turn_in_place", {{"entity", kTEntity}, {"toward", kTNumber | kTPoint}},
        "Turns a standing character in place toward a world yaw (degrees, 0 = -Z) or to face a point / entity, at "
        "characterIk.turnSpeed; planted feet stay locked and re-plant in small steps. Controllers can read the float `turn` "
        "(degrees left) and bool `turning` parameters to play turn clips.",
        "turn_in_place(self, find(\"Door\"))", [](CallContext& c) -> Value {
            EntityId e = characterOf(c, 0);
            Engine* engine = c.service<Engine>();
            if (!engine) return Value();
            float yaw;
            if (c.arg(1).isNumber()) {
                yaw = static_cast<float>(c.number(1));
            } else {
                Vec3 to = c.point(1) - c.scene().worldMatrix(e).translation();
                if (to.x * to.x + to.z * to.z < 1e-8f) return Value();
                yaw = degrees(std::atan2(-to.x, -to.z));
            }
            if (Status s = engine->animation().turnInPlace(e, yaw); !s) c.fail("turn_in_place(): " + s.error().message);
            return Value();
        });
    def(reg, "look_at", {{"entity", kTEntity}, {"target", kTEntity | kTNone}, {"weight", kTNumber, true}},
        "Turns a character's head and upper spine toward an entity (look-at IK, clamped by animator.lookAtLimit), or stops "
        "with `none`; the turn fades in and out smoothly.",
        "look_at(self, find(\"Player\"), 0.8)", [](CallContext& c) -> Value {
            EntityId e = characterOf(c, 0);
            const bool none = c.arg(1).isNone();
            Json patch = Json::object({{"lookAt", none ? Json() : Json(static_cast<int64_t>(c.entity(1)))}});
            if (c.argc() > 2) patch["lookAtWeight"] = std::clamp(static_cast<float>(c.number(2)), 0.f, 1.f);
            if (Status s = c.scene().patchComponent(e, "animator", patch); !s) c.fail("look_at(): " + s.error().message);
            return Value();
        });
}

}  // namespace sky
