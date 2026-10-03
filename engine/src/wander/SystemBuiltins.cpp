// Wander builtins for engine subsystems reached through Runtime hooks: audio, input
// actions, physics, characters and navigation. The hooks (Runtime::playAudio,
// Runtime::physics, ...) are installed by the engine; without them (sandboxes, bare
// runtimes) audio calls do nothing and physics calls report that physics is unavailable.

#include <algorithm>
#include <cmath>

#include "RuntimeInternal.h"
#include "skywalker/core/Strings.h"
#include "skywalker/wander/Builtins.h"

namespace sky::wander {

namespace {

void def(BuiltinRegistry& r, const char* name, std::vector<BuiltinParam> params, TypeSet ret, const char* category,
         const char* doc, const char* example, BuiltinImpl impl) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = ret;
    d.category = category;
    d.doc = doc;
    d.example = example;
    d.fn = impl;
    d.owner = "core";
    r.add(std::move(d));
}

std::string quoted(CallContext& c, EntityRef id) { return "'" + c.scene().record(id)->name + "'"; }

PhysicsHooks& physics(CallContext& c) {
    if (!c.runtime().physics) c.fail(c.def().name + "(): physics is not available in this context");
    return *c.runtime().physics;
}

const input::ActionState& actionState(CallContext& c) {
    const std::string& name = c.string(0);
    const auto& actions = c.input().actions;
    auto it = actions.find(name);
    if (it == actions.end()) {
        std::vector<std::string> names;
        for (const auto& entry : actions) names.push_back(entry.first);
        std::string guess = str::closest(name, names, 3);
        std::string list;
        for (const auto& k : names) list += (list.empty() ? "" : ", ") + k;
        c.fail(c.def().name + "(): unknown input action '" + name + "'" + (guess.empty() ? "" : " - did you mean '" + guess + "'?") +
               (list.empty() ? " (no actions are defined; see the input_map tool)" : " (actions: " + list + ")"));
    }
    return it->second;
}

}  // namespace

void registerSystemBuiltins(BuiltinRegistry& r) {
    const TypeSet N = kTNumber, V = kTVec, S = kTString, E = kTEntity, B = kTBool;

    // --- audio ---------------------------------------------------------------------------
    def(r, "play", {{"e", E}}, kTNone, "audio", "Starts the entity's audio component (its clip, volume, spatial settings).",
        "play(self)", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (c.runtime().playAudio) {
                std::string err = c.runtime().playAudio(id);
                if (!err.empty()) c.fail("play(): " + err);
            }
            return {};
        });
    def(r, "stop_sound", {{"e", E}}, kTNone, "audio", "Stops the entity's audio component.", "stop_sound(self)",
        [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (c.runtime().stopAudio) c.runtime().stopAudio(id);
            return {};
        });
    def(r, "play_sound", {{"clip", S}, {"volume", N, true}}, kTNone, "audio",
        "Plays a one-shot sound at self (positional), e.g. a hit or a pickup.", "play_sound(\"audio/hit.wav\", 0.8)",
        [](CallContext& c) -> Value {
            if (c.runtime().playSound) {
                std::string err = c.runtime().playSound(c.string(0), c.argc() > 1 ? static_cast<float>(c.number(1)) : 1.f, c.self());
                if (!err.empty()) c.fail("play_sound(): " + err);
            }
            return {};
        });
    def(r, "music", {{"clip", S}, {"fade", N, true}}, kTNone, "audio",
        "Crossfades the music to a clip (\"\" fades out); fade in seconds.", "music(\"audio/theme.wav\", 2)",
        [](CallContext& c) -> Value {
            if (c.runtime().playMusic) {
                std::string err = c.runtime().playMusic(c.string(0), c.argc() > 1 ? static_cast<float>(c.number(1)) : -1.f);
                if (!err.empty()) c.fail("music(): " + err);
            }
            return {};
        });
    def(r, "set_volume", {{"bus", S}, {"volume", N}}, kTNone, "audio",
        "Sets a mixer bus volume (master, music, sfx, ambience, voice, ui), 0..1.", "set_volume(\"music\", 0.5)",
        [](CallContext& c) -> Value {
            const std::string bus = str::lower(c.string(0));
            static const std::vector<std::string> buses{"master", "music", "sfx", "ambience", "voice", "ui"};
            if (std::find(buses.begin(), buses.end(), bus) == buses.end()) {
                std::string guess = str::closest(bus, buses, 3);
                c.fail("set_volume(): unknown bus '" + bus + "' (buses: master, music, sfx, ambience, voice, ui)" +
                       (guess.empty() ? "" : " - did you mean '" + guess + "'?"));
            }
            if (c.runtime().setBusVolume) c.runtime().setBusVolume(bus, static_cast<float>(c.number(1)));
            return {};
        });

    // --- input actions (input.json; keyboard, mouse and gamepad share names) -------------------
    def(r, "action", {{"name", S}}, B, "input", "Whether an input action is held (\"jump\", \"fire\" from input.json).",
        "if action(\"fire\") then ... end", [](CallContext& c) { return Value::boolean(actionState(c).held); });
    def(r, "pressed", {{"name", S}}, B, "input", "Whether an input action was pressed this tick.",
        "if pressed(\"jump\") then jump(self) end", [](CallContext& c) { return Value::boolean(actionState(c).pressed); });
    def(r, "released", {{"name", S}}, B, "input", "Whether an input action was released this tick.", "released(\"aim\")",
        [](CallContext& c) { return Value::boolean(actionState(c).released); });
    def(r, "axis", {{"name", S}}, N | V, "input",
        "Value of an axis action: a number (-1..1), or a vector (x, y, 0) for 2D axes (x right, y forward/up).",
        "walk(self, (axis(\"move\").x, 0, -axis(\"move\").y))", [](CallContext& c) {
            const input::ActionState& st = actionState(c);
            if (st.vec2) return Value::vec({st.x, st.y, 0.f});
            return Value::number(st.x);
        });

    // --- physics -----------------------------------------------------------------------------
    def(r, "push", {{"e", E}, {"force", V}}, kTNone, "physics", "Applies a continuous force (N) to a dynamic body this step.",
        "push(self, (0, 0, -20))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (!physics(c).addForce(id, c.vec(1))) c.fail("push(): " + quoted(c, id) + " has no dynamic body (add a body with motion \"dynamic\")");
            return {};
        });
    def(r, "impulse", {{"e", E}, {"impulse", V}}, kTNone, "physics", "An instant kick (N s) to a dynamic body.",
        "impulse(self, (0, 6, 0))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (!physics(c).addImpulse(id, c.vec(1))) c.fail("impulse(): " + quoted(c, id) + " has no dynamic body (add a body with motion \"dynamic\")");
            return {};
        });
    def(r, "torque", {{"e", E}, {"torque", V}}, kTNone, "physics", "Applies a torque (N m) to a dynamic body.",
        "torque(self, (0, 5, 0))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (!physics(c).addTorque(id, c.vec(1))) c.fail("torque(): " + quoted(c, id) + " has no dynamic body (add a body with motion \"dynamic\")");
            return {};
        });
    def(r, "velocity", {{"e", E}}, V, "physics", "Current linear velocity of a body or character (m/s).",
        "if length(velocity(self)) > 10 then ... end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            auto v = physics(c).velocity(id);
            if (!v) c.fail("velocity(): " + quoted(c, id) + " has no body or character");
            return Value::vec(*v);
        });
    def(r, "raycast", {{"origin", kTPoint}, {"direction", V}, {"max_distance", N, true}}, E | kTNone, "physics",
        "First collider hit along a ray (self is ignored); sets hit_point, hit_normal and hit_distance.",
        "let ground = raycast(self, (0, -1, 0), 2)", [](CallContext& c) {
            Vec3 origin = c.point(0), dir = c.vec(1);
            double maxDist = c.argc() > 2 ? c.number(2) : 1000.0;
            if (length(dir) < 1e-6f) c.fail("raycast(): direction must not be zero");
            ExecState& st = CallContextAccess(c);
            st.lastHit = physics(c).raycast(origin, normalize(dir), static_cast<float>(std::max(0.0, maxDist)), c.self());
            return st.lastHit ? Value::entity(st.lastHit->entity) : Value();
        });
    def(r, "overlap_sphere", {{"center", kTPoint}, {"radius", N}, {"tag", S, true}}, E | kTNone, "physics",
        "Nearest entity (not self) whose collider overlaps the sphere, optionally with a tag; none if empty.",
        "let enemy = overlap_sphere(self, 3, \"enemy\")", [](CallContext& c) {
            Vec3 center = c.point(0);
            double radius = c.number(1);
            std::string tag = c.argc() > 2 ? c.string(2) : "";
            for (EntityId id : physics(c).overlapSphere(center, static_cast<float>(std::max(0.0, radius)), c.self())) {
                const EntityRecord* rec = c.scene().record(id);
                if (!rec) continue;
                if (!tag.empty() && std::find(rec->tags.begin(), rec->tags.end(), tag) == rec->tags.end()) continue;
                return Value::entity(id);
            }
            return Value();
        });
    def(r, "walk", {{"e", E}, {"direction", V}}, kTNone, "character",
        "Moves a character this tick (|direction| 1 = its moveSpeed). Call every tick while walking.",
        "walk(self, (axis(\"move\").x, 0, -axis(\"move\").y))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (!physics(c).walk(id, c.vec(1))) c.fail("walk(): " + quoted(c, id) + " has no character component");
            return {};
        });
    def(r, "jump", {{"e", E}, {"speed", N, true}}, B, "character",
        "Makes a grounded character jump (default: its jumpSpeed); false when airborne.", "if pressed(\"jump\") then jump(self) end",
        [](CallContext& c) {
            EntityRef id = c.entity(0);
            float speed = c.argc() > 1 ? static_cast<float>(c.number(1)) : 0.f;
            if (!c.scene().get<CharacterController>(id)) c.fail("jump(): " + quoted(c, id) + " has no character component");
            return Value::boolean(physics(c).jump(id, speed));
        });
    def(r, "grounded", {{"e", E}}, B, "character", "Whether a character stands on the ground.", "if grounded(self) then ... end",
        [](CallContext& c) {
            EntityRef id = c.entity(0);
            auto g = physics(c).grounded(id);
            if (!g) c.fail("grounded(): " + quoted(c, id) + " has no character component");
            return Value::boolean(*g);
        });

    // --- navigation ------------------------------------------------------------------------------
    def(r, "navigate", {{"e", E}, {"target", kTPoint}}, kTNone, "navigation",
        "Walks a nav agent to a point, or follows an entity as it moves. `on event \"arrived\"` fires on arrival.",
        "navigate(self, find(\"Player\"))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            EntityId follow = c.arg(1).isEntity() ? c.entity(1) : kNoEntity;
            if (!physics(c).navigate(id, c.point(1), follow)) c.fail("navigate(): " + quoted(c, id) + " has no nav_agent component");
            return {};
        });
    def(r, "stop_navigation", {{"e", E}}, kTNone, "navigation", "Stops a nav agent.", "stop_navigation(self)",
        [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            if (!physics(c).stopNavigation(id)) c.fail("stop_navigation(): " + quoted(c, id) + " has no nav_agent");
            return {};
        });
    def(r, "arrived", {{"e", E}}, B, "navigation", "Whether a nav agent reached its destination.", "if arrived(self) then ... end",
        [](CallContext& c) {
            EntityRef id = c.entity(0);
            auto a = physics(c).arrived(id);
            if (!a) c.fail("arrived(): " + quoted(c, id) + " has no nav_agent component");
            return Value::boolean(*a);
        });
    def(r, "path_length", {{"from", kTPoint}, {"to", kTPoint}}, N | kTNone, "navigation",
        "Walking distance along the navmesh; none if unreachable or there is no navmesh.", "path_length(self, find(\"Exit\"))",
        [](CallContext& c) {
            auto len = physics(c).pathLength(c.point(0), c.point(1));
            return len ? Value::number(*len) : Value();
        });

    // Triggers delivered by subsystems (documented for wander_reference).
    r.addTrigger({"collide", "Two bodies touched: on collide (\"name or tag\")? — other, contact_point, contact_normal, impact.",
                  "", "physics"});
    r.addTrigger({"trigger_enter", "Something entered this trigger collider: on trigger_enter (\"name or tag\")? — other.", "",
                  "physics"});
    r.addTrigger({"trigger_exit", "Something left this trigger collider: on trigger_exit (\"name or tag\")? — other.", "", "physics"});
    r.addTrigger({"action", "An input action was pressed: on action \"jump\" (actions come from input.json).", "", "input"});
}

}  // namespace sky::wander
