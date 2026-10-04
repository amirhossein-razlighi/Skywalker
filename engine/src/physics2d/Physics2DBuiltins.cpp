// Wander builtins for 2D physics (docs/PHYSICS.md "2D physics"). They mirror the 3D ones with a
// "2d" suffix: push2d / impulse2d / torque2d / velocity2d / set_velocity2d for bodies, raycast2d /
// overlap2d / point2d for queries, move2d / jump2d / grounded2d / drop_through2d for character2d.
// Vectors are (x, y, z) with z ignored; positions may be entities (their world position).

#include <algorithm>
#include <cmath>

#include "skywalker/engine/Engine.h"
#include "skywalker/physics2d/Physics2DSystem.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

Vec2 xy(Vec3 v) { return {v.x, v.y}; }

physics2d::Physics2DSystem& system2d(CallContext& c) {
    Engine* engine = c.service<Engine>();
    if (!engine) c.fail(c.def().name + "(): 2D physics is not available in this context");
    return engine->physics2d();
}

std::string quoted(CallContext& c, EntityRef id) { return "'" + c.scene().record(id)->name + "'"; }

/// The play world, synced first when `e` has the component but is not simulated yet (spawned this tick).
template <typename Component>
physics2d::Physics2DWorld* liveWorld(CallContext& c, EntityRef e, bool (physics2d::Physics2DWorld::*has)(EntityId) const) {
    physics2d::Physics2DSystem& sys = system2d(c);
    if (!sys.playing()) return nullptr;
    physics2d::Physics2DWorld& w = sys.queryWorld();
    if (!(w.*has)(e) && c.scene().get<Component>(e)) w.sync(c.scene(), 0.f);
    return &w;
}

/// `self` and the bodies it is part of: queries from a script never hit its own body.
physics2d::Filter2D selfFilter(CallContext& c) {
    physics2d::Filter2D f;
    const Scene& s = c.scene();
    for (const EntityRecord* r = s.record(c.self()); r; r = r->parent ? s.record(r->parent) : nullptr) {
        if (r->id == c.self() || s.get<Body2D>(r->id) || s.get<Character2D>(r->id)) f.exclude.push_back(r->id);
    }
    return f;
}

bool hasTag(const Scene& s, EntityId id, const std::string& tag) {
    const EntityRecord* rec = s.record(id);
    return rec && (tag.empty() || std::find(rec->tags.begin(), rec->tags.end(), tag) != rec->tags.end());
}

void def(BuiltinRegistry& reg, const char* name, std::vector<BuiltinParam> params, TypeSet returns, const char* category, const char* doc,
         const char* example, BuiltinImpl fn) {
    BuiltinDef d;
    d.name = name;
    d.params = std::move(params);
    d.returns = returns;
    d.category = category;
    d.doc = doc;
    d.example = example;
    d.fn = fn;
    d.owner = "engine";
    reg.add(std::move(d));
}

}  // namespace

void registerPhysics2DBuiltins(BuiltinRegistry& reg) {
    const TypeSet N = kTNumber, V = kTVec, S = kTString, E = kTEntity, B = kTBool;

    // --- bodies -----------------------------------------------------------------------------
    def(reg, "push2d", {{"e", E}, {"force", V}}, kTNone, "physics2d", "Applies a continuous force (N, x/y) to a dynamic body2d this tick.",
        "push2d(self, (axis(\"move\").x * 20, 0, 0))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            auto* w = liveWorld<Body2D>(c, id, &physics2d::Physics2DWorld::hasBody);
            if (!w || !w->addForce(id, xy(c.vec(1)))) c.fail("push2d(): " + quoted(c, id) + " has no dynamic body2d (motion \"dynamic\")");
            return {};
        });
    def(reg, "impulse2d", {{"e", E}, {"impulse", V}}, kTNone, "physics2d", "An instant kick (N s, x/y) to a dynamic body2d.",
        "impulse2d(self, (0, 8, 0))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            auto* w = liveWorld<Body2D>(c, id, &physics2d::Physics2DWorld::hasBody);
            if (!w || !w->addImpulse(id, xy(c.vec(1)))) c.fail("impulse2d(): " + quoted(c, id) + " has no dynamic body2d (motion \"dynamic\")");
            return {};
        });
    def(reg, "torque2d", {{"e", E}, {"torque", N}}, kTNone, "physics2d", "Applies a torque (N m, counter-clockwise) to a dynamic body2d.",
        "torque2d(self, 5)", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            auto* w = liveWorld<Body2D>(c, id, &physics2d::Physics2DWorld::hasBody);
            if (!w || !w->addTorque(id, static_cast<float>(c.number(1)))) {
                c.fail("torque2d(): " + quoted(c, id) + " has no dynamic body2d (motion \"dynamic\")");
            }
            return {};
        });
    def(reg, "velocity2d", {{"e", E}}, V, "physics2d", "Current velocity of a body2d or character2d (units/s, z = 0).",
        "if abs(velocity2d(self).x) > 8 then ... end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            physics2d::Physics2DSystem& sys = system2d(c);
            std::optional<Vec2> v;
            if (sys.playing()) v = sys.queryWorld().velocity(id);
            if (!v) {
                if (const Body2D* b = c.scene().get<Body2D>(id)) v = b->velocity;
                else if (const Character2D* ch = c.scene().get<Character2D>(id)) v = ch->velocity;
            }
            if (!v) c.fail("velocity2d(): " + quoted(c, id) + " has no body2d or character2d");
            return Value::vec({v->x, v->y, 0.f});
        });
    def(reg, "set_velocity2d", {{"e", E}, {"velocity", V}}, kTNone, "physics2d",
        "Sets the velocity of a body2d (dynamic or kinematic) or character2d (knockback, launch pads).",
        "set_velocity2d(self, (velocity2d(self).x, 14, 0))", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            const Vec2 v = xy(c.vec(1));
            auto* w = liveWorld<Body2D>(c, id, &physics2d::Physics2DWorld::hasBody);
            if (w && w->setVelocity(id, v)) return {};
            if (Body2D* b = c.scene().get<Body2D>(id)) {
                b->velocity = v;
                return {};
            }
            if (Character2D* ch = c.scene().get<Character2D>(id)) {
                ch->velocity = v;
                return {};
            }
            c.fail("set_velocity2d(): " + quoted(c, id) + " has no body2d or character2d");
        });

    // --- queries ------------------------------------------------------------------------------
    def(reg, "raycast2d", {{"origin", kTPoint}, {"direction", V}, {"max_distance", N, true}}, E | kTNone, "physics2d",
        "First 2D collider hit along a ray on the XY plane (self is ignored, sensors too); sets hit_point, hit_normal and "
        "hit_distance.",
        "let ground = raycast2d(self, (0, -1, 0), 1.2)", [](CallContext& c) {
            const Vec2 origin = xy(c.point(0)), dir = xy(c.vec(1));
            const double maxDist = c.argc() > 2 ? c.number(2) : 1000.0;
            if (std::sqrt(dir.x * dir.x + dir.y * dir.y) < 1e-6f) c.fail("raycast2d(): direction must not be zero (x/y)");
            auto hit = system2d(c).queryWorld().raycast(origin, dir, static_cast<float>(std::max(0.0, maxDist)), selfFilter(c));
            if (!hit) {
                c.clearLastHit();
                return Value();
            }
            c.setLastHit(hit->entity, {hit->point.x, hit->point.y, 0.f}, {hit->normal.x, hit->normal.y, 0.f}, hit->distance);
            return Value::entity(hit->entity);
        });
    def(reg, "overlap2d", {{"center", kTPoint}, {"radius", N}, {"tag", S, true}}, E | kTNone, "physics2d",
        "Nearest entity (not self) whose 2D collider overlaps the circle, optionally with a tag; sensors are ignored; none if empty.",
        "let enemy = overlap2d(self, 3, \"enemy\")", [](CallContext& c) {
            const Vec2 center = xy(c.point(0));
            const std::string tag = c.argc() > 2 ? c.string(2) : "";
            for (EntityId id : system2d(c).queryWorld().overlapCircle(center, static_cast<float>(std::max(0.0, c.number(1))), selfFilter(c))) {
                if (hasTag(c.scene(), id, tag)) return Value::entity(id);
            }
            return Value();
        });
    def(reg, "point2d", {{"point", kTPoint}, {"tag", S, true}}, E | kTNone, "physics2d",
        "Entity whose 2D collider contains the point (sensors included, self excluded), optionally with a tag; none if empty.",
        "let under = point2d((2, 1, 0), \"crate\")", [](CallContext& c) {
            physics2d::Filter2D f = selfFilter(c);
            f.includeSensors = true;
            const std::string tag = c.argc() > 1 ? c.string(1) : "";
            for (EntityId id : system2d(c).queryWorld().pointQuery(xy(c.point(0)), f)) {
                if (hasTag(c.scene(), id, tag)) return Value::entity(id);
            }
            return Value();
        });

    // --- character2d -------------------------------------------------------------------------------
    def(reg, "move2d", {{"e", E}, {"x", N}}, kTNone, "character2d",
        "Runs a character2d this tick: -1 = full speed left, 1 = right (scaled by moveSpeed, with acceleration). Call every tick.",
        "move2d(self, axis(\"move\").x)", [](CallContext& c) -> Value {
            EntityRef id = c.entity(0);
            auto* w = liveWorld<Character2D>(c, id, &physics2d::Physics2DWorld::hasCharacter);
            if (!c.scene().get<Character2D>(id)) c.fail("move2d(): " + quoted(c, id) + " has no character2d component");
            if (w) w->move(id, static_cast<float>(c.number(1)));
            return {};
        });
    def(reg, "jump2d", {{"e", E}, {"speed", N, true}}, B, "character2d",
        "Makes a character2d jump (default: its jumpSpeed). True when it jumps now (grounded or within coyote time); otherwise the "
        "press is buffered and the jump happens on landing within jumpBuffer seconds.",
        "if pressed(\"jump\") then jump2d(self) end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            if (!c.scene().get<Character2D>(id)) c.fail("jump2d(): " + quoted(c, id) + " has no character2d component");
            auto* w = liveWorld<Character2D>(c, id, &physics2d::Physics2DWorld::hasCharacter);
            const float speed = c.argc() > 1 ? static_cast<float>(c.number(1)) : 0.f;
            return Value::boolean(w && w->jump(id, speed));
        });
    def(reg, "grounded2d", {{"e", E}}, B, "character2d", "Whether a character2d stands on walkable ground (a slope up to maxSlope or a platform).",
        "if grounded2d(self) then play_anim(self, \"run\") end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            const Character2D* ch = c.scene().get<Character2D>(id);
            if (!ch) c.fail("grounded2d(): " + quoted(c, id) + " has no character2d component");
            auto* w = liveWorld<Character2D>(c, id, &physics2d::Physics2DWorld::hasCharacter);
            auto g = w ? w->grounded(id) : std::optional<bool>(ch->grounded);
            return Value::boolean(g.value_or(false));
        });
    def(reg, "drop_through2d", {{"e", E}}, B, "character2d",
        "Drops a character2d through the one-way platform it stands on; false when it stands on solid ground.",
        "if pressed(\"down\") and pressed(\"jump\") then drop_through2d(self) end", [](CallContext& c) {
            EntityRef id = c.entity(0);
            if (!c.scene().get<Character2D>(id)) c.fail("drop_through2d(): " + quoted(c, id) + " has no character2d component");
            auto* w = liveWorld<Character2D>(c, id, &physics2d::Physics2DWorld::hasCharacter);
            return Value::boolean(w && w->dropThrough(id));
        });

    // Triggers delivered by 2D physics besides collide / trigger_enter / trigger_exit (shared with 3D).
    reg.addTrigger({"collide_end", "Two 2D bodies stopped touching: on collide_end — other.", "{}", "physics2d"});
    reg.addTrigger({"impact", "A 2D body hit this one faster than physics2d_world.impactSpeed: on impact — other, data.",
                    "{point: vec, normal: vec, speed: number, impulse: number}", "physics2d"});
}

}  // namespace sky
