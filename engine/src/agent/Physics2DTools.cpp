// 2D physics tools (docs/PHYSICS.md "2D physics"): make sprites and tilemaps physical with presets,
// inspect the 2D world, settle bodies with a short simulation committed as one undoable edit, and
// query the 2D collision world (works while editing too).

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/physics2d/Physics2DSystem.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;

float r3(float x) { return std::round(x * 1000.f) / 1000.f; }
Json v2(Vec2 v) { return Json::array({r3(v.x), r3(v.y)}); }

Json vec2(std::string description) {
    return Json::object({{"type", "array"},
                         {"items", Json::object({{"type", "number"}})},
                         {"minItems", 2},
                         {"maxItems", 3},
                         {"description", std::move(description)}});
}

const std::vector<std::string>& presets2D() {
    static const std::vector<std::string> p{"platformer_player", "crate",       "ball",            "one_way_platform", "tilemap_collision",
                                            "static_ground",     "sensor_zone", "moving_platform", "remove"};
    return p;
}

Result<std::vector<EntityId>> resolveMany(Engine& engine, const Json& a) {
    std::vector<EntityId> out;
    auto addOne = [&](const Json& ref) -> Status {
        auto id = resolve(engine, ref);
        if (!id) return id.error();
        if (std::find(out.begin(), out.end(), *id) == out.end()) out.push_back(*id);
        return {};
    };
    if (a.contains("entity")) {
        if (Status s = addOne(a.get("entity")); !s) return s.error();
    }
    for (const auto& ref : a.get("entities").elements()) {
        if (Status s = addOne(ref); !s) return s.error();
    }
    if (out.empty()) return Error::make("invalid_arguments", "give `entity` or `entities`", "e.g. {\"entity\": \"Player\", \"preset\": \"platformer_player\"}");
    return out;
}

/// The sprite's size and center in the entity's local units (1 x 1 around the origin without a sprite).
struct SpriteBox {
    Vec2 size{1.f, 1.f};
    Vec2 center{0.f, 0.f};
    bool fromSprite = false;
};

SpriteBox spriteBox(Engine& engine, EntityId id) {
    SpriteBox out;
    const Sprite* sp = engine.scene().get<Sprite>(id);
    if (!sp) return out;
    render2d::FrameRef fr;
    Status st = engine.world2d().assets().frame(sp->texture, sp->frame, sp->columns, sp->rows, sp->region, fr);
    const float ppu = std::max(0.01f, sp->pixelsPerUnit);
    const float srcW = st && !fr.path.empty() ? fr.sourceW : ppu, srcH = st && !fr.path.empty() ? fr.sourceH : ppu;
    Vec2 size = sp->size;
    if (size.x <= 0 && size.y <= 0) size = {srcW / ppu, srcH / ppu};
    else if (size.x <= 0) size.x = size.y * srcW / std::max(1.f, srcH);
    else if (size.y <= 0) size.y = size.x * srcH / std::max(1.f, srcW);
    out.size = size;
    out.center = {(0.5f - sp->pivot.x) * size.x, (0.5f - sp->pivot.y) * size.y};
    out.fromSprite = true;
    return out;
}

Json mergeInto(Json base, const Json& overrides) {
    if (overrides.isObject()) {
        for (const auto& [k, v] : overrides.members()) base[k] = v;
    }
    return base;
}

/// Applies a preset to one entity (inside an edit). Returns the components it set; adds warnings.
Result<Json> applyPreset2D(Engine& engine, EntityId id, const std::string& preset, const Json& overrides, Json& warnings) {
    Scene& s = engine.scene();
    Json changed = Json::array();
    auto patch = [&](const char* comp, const Json& value) -> Status {
        Json v = mergeInto(value, overrides.get(comp));
        if (Status st = s.patchComponent(id, comp, v); !st) return st;
        changed.push(comp);
        return {};
    };
    auto remove = [&](const char* comp) -> Status {
        if (const ComponentKind* k = s.componentKind(comp); k && k->has(s, id)) return s.patchComponent(id, comp, Json());
        return {};
    };
    if (preset == "remove") {
        for (const char* c : {"body2d", "collider2d", "character2d", "joint2d"}) {
            if (Status st = remove(c); !st) return st.error();
        }
        changed.push("removed");
        return changed;
    }
    if (preset == "tilemap_collision") {
        if (!s.get<Tilemap>(id)) {
            return Error::make("no_tilemap", "'" + s.record(id)->name + "' has no tilemap component",
                               "use tilemap_create first, or pick the entity that holds the tilemap");
        }
        if (Status st = remove("body2d"); !st) return st.error();
        Json col = Json::object({{"shape", "tilemap"}, {"tileMerge", "chains"}, {"layer", "static"}});
        if (Status st = patch("collider2d", col); !st) return st.error();
        const Tilemap* tm = s.get<Tilemap>(id);
        bool anySolid = false;
        for (const auto& l : tm->layers.elements()) anySolid = anySolid || !l.get("solid").isNull();
        if (!anySolid) warnings.push("no tilemap layer is solid yet: set \"solid\": true on the ground layer (or \"tiles\" with solidTiles)");
        return changed;
    }
    const SpriteBox box = spriteBox(engine, id);
    if (!box.fromSprite && preset != "sensor_zone") warnings.push("'" + s.record(id)->name + "' has no sprite: the collider is 1 x 1 around its origin");
    if (preset == "platformer_player") {
        for (const char* c : {"body2d", "collider2d"}) {
            if (Status st = remove(c); !st) return st.error();
        }
        const float h = std::clamp(box.size.y * 0.9f, 0.1f, 50.f);
        const float r = std::clamp(box.size.x * 0.4f, 0.05f, h * 0.5f);
        Json ch = Json::object({{"height", r3(h)},
                                {"radius", r3(r)},
                                {"offset", v2({box.center.x, box.center.y - box.size.y * 0.05f})},
                                {"layer", "player"}});
        if (Status st = patch("character2d", ch); !st) return st.error();
        return changed;
    }
    if (Status st = remove("character2d"); !st) return st.error();
    Json col = Json::object({{"shape", "box"}, {"size", v2(box.size)}, {"offset", v2(box.center)}});
    Json body;
    if (preset == "crate") {
        body = Json::object({{"motion", "dynamic"}});
        col["friction"] = 0.6f;
    } else if (preset == "ball") {
        body = Json::object({{"motion", "dynamic"}});
        col = Json::object({{"shape", "circle"}, {"radius", r3(std::min(box.size.x, box.size.y) * 0.5f)}, {"offset", v2(box.center)},
                            {"restitution", 0.6f}, {"friction", 0.4f}});
    } else if (preset == "moving_platform") {
        body = Json::object({{"motion", "kinematic"}});
    } else if (preset == "one_way_platform") {
        col["oneWay"] = true;
        col["layer"] = "static";
    } else if (preset == "static_ground") {
        col["layer"] = "static";
    } else if (preset == "sensor_zone") {
        col["sensor"] = true;
        col["layer"] = "trigger";
    }
    if (body.isObject()) {
        if (Status st = patch("body2d", body); !st) return st.error();
    } else if (preset != "one_way_platform" || (s.get<Body2D>(id) && s.get<Body2D>(id)->motion != "kinematic")) {
        if (Status st = remove("body2d"); !st) return st.error();  // static: a collider alone
    }
    if (Status st = patch("collider2d", col); !st) return st.error();
    return changed;
}

Json hitJson(const Scene& s, const physics2d::Hit2D& h) {
    const EntityRecord* r = s.record(h.entity);
    return Json::object({{"entity", h.entity}, {"name", r ? r->name : ""}, {"point", v2(h.point)}, {"normal", v2(h.normal)}, {"distance", r3(h.distance)}});
}

Result<uint64_t> layerMask(const Json& layers) {
    if (!layers.isArray() || layers.size() == 0) return ~0ull;
    uint64_t mask = 0;
    const auto& names = physics2d::layerNames2D();
    for (const auto& l : layers.elements()) {
        auto it = std::find(names.begin(), names.end(), l.asString());
        if (it == names.end()) {
            std::string guess = str::closest(l.asString(), names, 3);
            return Error::make("unknown_layer", "unknown collision layer \"" + l.asString() + "\"",
                               guess.empty() ? "layers: default, static, player, enemy, projectile, trigger, debris" : "did you mean \"" + guess + "\"?");
        }
        mask |= 1ull << static_cast<unsigned>(it - names.begin());
    }
    return mask;
}

Json warningsJson(Engine& engine, Json extra = Json::array()) {
    for (const auto& w : engine.physics2d().recentWarnings()) extra.push(w);
    return extra;
}

}  // namespace

void addPhysics2DTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"physics2d_info", "2D physics info",
             "The 2D physics world (Box2D) at a glance: settings (gravity, sub-steps, debug draw), counts (bodies by motion, "
             "awake/sleeping, shapes, sensors, chains, characters, joints, contacts), each body and character2d with its "
             "velocity, sleeping/grounded state, tilemap collision pieces (merged loops/boxes/polygons), and warnings (bad "
             "polygons, unknown layers, missing joint bodies). Works while editing (an edit-time mirror) and playing. "
             "Pass entity for one entity only. Example: {\"entity\": \"Player\"}.",
             "physics",
             object({{"entity", schema::entity("Only this entity")}, {"limit", integer("Max bodies listed (default 50)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 physics2d::Physics2DSystem& sys = engine.physics2d();
                 EntityId only = kNoEntity;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     only = *id;
                 }
                 physics2d::Physics2DWorld& w = sys.queryWorld();
                 const physics2d::Stats2D st = w.stats();
                 Physics2DSettings settings;
                 for (EntityId e : s.entities()) {
                     if (const Physics2DSettings* ps = s.get<Physics2DSettings>(e); ps && s.isActive(e)) {
                         settings = *ps;
                         break;
                     }
                 }
                 const size_t limit = static_cast<size_t>(std::clamp<int64_t>(a.get("limit").asInt(50), 1, 1000));
                 Json bodies = Json::array(), characters = Json::array(), tilemaps = Json::array();
                 for (EntityId e : s.entities()) {
                     if (only && e != only) continue;
                     if (w.hasCharacter(e) && characters.size() < limit) {
                         characters.push(Json::object({{"entity", e},
                                                       {"name", s.record(e)->name},
                                                       {"grounded", w.grounded(e).value_or(false)},
                                                       {"velocity", v2(w.velocity(e).value_or(Vec2{}))}}));
                     }
                     if (!w.hasBody(e)) continue;
                     const Body2D* b = s.get<Body2D>(e);
                     if (bodies.size() < limit) {
                         bodies.push(Json::object({{"entity", e},
                                                   {"name", s.record(e)->name},
                                                   {"motion", b ? b->motion : "static"},
                                                   {"sleeping", w.isSleeping(e)},
                                                   {"velocity", v2(w.velocity(e).value_or(Vec2{}))}}));
                     }
                     if (auto ti = w.tilemapInfo(e)) {
                         tilemaps.push(Json::object({{"entity", e},
                                                     {"name", s.record(e)->name},
                                                     {"solidCells", ti->solidCells},
                                                     {"loops", ti->loops},
                                                     {"boxes", ti->boxes},
                                                     {"polygons", ti->polygons}}));
                     }
                 }
                 Json out = Json::object(
                     {{"playing", sys.playing()},
                      {"settings", Json::object({{"gravity", v2(settings.gravity)},
                                                 {"substeps", settings.substeps},
                                                 {"allowSleep", settings.allowSleep},
                                                 {"impactSpeed", settings.impactSpeed},
                                                 {"debugDraw", settings.debugDraw}})},
                      {"stats", Json::object({{"bodies", st.bodies},
                                              {"dynamic", st.dynamicBodies},
                                              {"kinematic", st.kinematicBodies},
                                              {"static", st.staticBodies},
                                              {"awake", st.awakeBodies},
                                              {"sleeping", st.sleepingBodies},
                                              {"shapes", st.shapes},
                                              {"sensors", st.sensors},
                                              {"chains", st.chains},
                                              {"characters", st.characters},
                                              {"joints", st.joints},
                                              {"contacts", st.contacts}})},
                      {"bodies", bodies},
                      {"characters", characters},
                      {"tilemaps", tilemaps},
                      {"warnings", warningsJson(engine)}});
                 char text[200];
                 std::snprintf(text, sizeof(text), "%d bodies (%d dynamic, %d awake), %d characters, %d joints, %d contacts%s", st.bodies,
                               st.dynamicBodies, st.awakeBodies, st.characters, st.joints, st.contacts, sys.playing() ? "" : " (editing)");
                 return ToolResult::json(out, text);
             }});

    reg.add({"physics2d_add", "Make 2D physical",
             "Make 2D entities physical with a preset (one undoable edit); colliders are fitted to the sprite. "
             "platformer_player = character2d controller (drive with move2d/jump2d in Wander); crate = dynamic box; ball = "
             "bouncy dynamic circle; one_way_platform = static box you can jump through from below; tilemap_collision = "
             "merged collision from the entity's tilemap (solid layers, per-tile slopes and one-way tiles); static_ground = "
             "static box; sensor_zone = trigger area (on trigger_enter / on trigger_exit); moving_platform = kinematic box "
             "(move it with its transform or body2d.velocity); remove = strip the 2D physics components. `overrides` patches "
             "the components: {\"collider2d\": {\"restitution\": 0.9}, \"character2d\": {\"jumpSpeed\": 14}}. Example: "
             "{\"entity\": \"Player\", \"preset\": \"platformer_player\"}.",
             "physics",
             object({{"entity", schema::entity("Entity (or use entities)")},
                     {"entities", array(schema::entity(), "Entities to change")},
                     {"preset", enumeration(presets2D(), "What the entities become")},
                     {"overrides", Json::object({{"type", "object"},
                                                 {"description", "Component patches: {\"body2d\": {...}, \"collider2d\": {...}, \"character2d\": {...}}"}})}},
                    {"preset"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto ids = resolveMany(engine, a);
                 if (!ids) return ToolResult::error(ids.error());
                 const std::string preset = a.get("preset").asString();
                 if (std::find(presets2D().begin(), presets2D().end(), preset) == presets2D().end()) {
                     std::string guess = str::closest(preset, presets2D(), 4);
                     return ToolResult::error(Error::make("invalid_arguments", "unknown preset '" + preset + "'",
                                                          guess.empty() ? "presets: platformer_player, crate, ball, one_way_platform, "
                                                                          "tilemap_collision, static_ground, sensor_zone, moving_platform, remove"
                                                                        : "did you mean '" + guess + "'?"));
                 }
                 Json results = Json::array(), warnings = Json::array();
                 Status st = engine.edit(ctx.actor, "Physics 2D: " + preset, [&]() -> Status {
                     for (EntityId id : *ids) {
                         auto changed = applyPreset2D(engine, id, preset, a.get("overrides"), warnings);
                         if (!changed) return changed.error();
                         Json comps = Json::object();
                         for (const char* c : {"body2d", "collider2d", "character2d"}) {
                             const ComponentKind* k = engine.scene().componentKind(c);
                             if (k && k->has(engine.scene(), id)) comps[c] = k->toJson(engine.scene(), id);
                         }
                         results.push(Json::object({{"id", id}, {"name", engine.scene().record(id)->name}, {"set", *changed}, {"components", comps}}));
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string text = preset + " applied to " + std::to_string(results.size()) + " entit" + (results.size() == 1 ? "y" : "ies") +
                                    "; press play (sim_control) to simulate";
                 return ToolResult::json(Json::object({{"entities", results}, {"warnings", warnings}}), text);
             }});

    reg.add({"physics2d_settle", "Settle with 2D physics",
             "Drop 2D objects with a real simulation and keep where they come to rest: only the listed entities move (as "
             "dynamic bodies, even without body2d), everything else is frozen; the result is ONE undoable edit. Use after "
             "placing crates, rocks or props on a level so they rest on the ground and on each other. Example: "
             "{\"entities\": [\"Crate 1\", \"Crate 2\"], \"seconds\": 3}.",
             "physics",
             object({{"entity", schema::entity("Entity to settle (or use entities)")},
                     {"entities", array(schema::entity(), "Entities to settle")},
                     {"seconds", number("Max simulated seconds (default 4, max 30); stops early when everything sleeps")},
                     {"freeze_others", boolean("Keep other dynamic bodies still (default true)")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 if (engine.playState() != PlayState::Editing) {
                     return ToolResult::error(Error::make("not_editing", "physics2d_settle works while editing", "stop the simulation first (sim_control stop)"));
                 }
                 auto ids = resolveMany(engine, a);
                 if (!ids) return ToolResult::error(ids.error());
                 Scene& s = engine.scene();
                 physics2d::WorldOptions2D o;
                 o.forceDynamic.insert(ids->begin(), ids->end());
                 o.freezeOthers = a.get("freeze_others").asBool(true);
                 o.writeBack = false;
                 auto world = engine.physics2d().makeWorld(std::move(o));
                 world->sync(s, 0.f);
                 const float dt = static_cast<float>(Engine::kFixedDt);
                 const int maxTicks = static_cast<int>(std::clamp(a.get("seconds").asFloat(4.f), 0.1f, 30.f) * 60.f);
                 int ticks = 0;
                 bool asleep = false;
                 for (; ticks < maxTicks; ++ticks) {
                     world->step(s, dt);
                     if (ticks % 15 == 14 && world->allAsleep()) {
                         asleep = true;
                         ++ticks;
                         break;
                     }
                 }
                 Json moved = Json::array(), warnings = Json::array();
                 for (const auto& w : world->drainWarnings()) warnings.push(w);
                 std::vector<std::pair<EntityId, Transform>> finals;
                 for (EntityId id : *ids) {
                     auto t = world->localTransform(s, id);
                     if (!t) {
                         warnings.push("'" + s.record(id)->name + "' could not be simulated (no collider2d, or part of another body)");
                         continue;
                     }
                     const Transform* before = s.get<Transform>(id);
                     const float dist = before ? std::hypot(t->position.x - before->position.x, t->position.y - before->position.y) : 0.f;
                     finals.emplace_back(id, *t);
                     moved.push(Json::object({{"id", id},
                                              {"name", s.record(id)->name},
                                              {"position", v2({t->position.x, t->position.y})},
                                              {"rotation", r3(t->rotation.z)},
                                              {"moved", r3(dist)},
                                              {"sleeping", world->isSleeping(id)}}));
                 }
                 if (finals.empty()) {
                     return ToolResult::error(Error::make("nothing_to_settle", "none of the entities could be simulated",
                                                          "give them a collider2d (physics2d_add preset crate) or settle the top-level entity"));
                 }
                 Status st = engine.edit(ctx.actor, "Settle " + std::to_string(finals.size()) + " 2D objects", [&]() -> Status {
                     for (const auto& [id, t] : finals) {
                         Status ps = s.patchComponent(id, "transform",
                                                      Json::object({{"position", reflect::vec3ToJson(t.position)}, {"rotation", reflect::vec3ToJson(t.rotation)}}));
                         if (!ps) return ps;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 char text[160];
                 std::snprintf(text, sizeof(text), "settled %zu objects in %.2f simulated s (%s); one undo step", finals.size(),
                               static_cast<double>(ticks) * Engine::kFixedDt, asleep ? "all at rest" : "time limit reached");
                 return ToolResult::json(Json::object({{"entities", moved},
                                                       {"seconds", std::round(ticks * Engine::kFixedDt * 100.0) / 100.0},
                                                       {"atRest", asleep},
                                                       {"warnings", warnings}}),
                                         text);
             }});

    reg.add({"physics2d_query", "2D physics query",
             "Ask the 2D collision world (collider2d shapes, character2d capsules) on the XY plane — works while editing "
             "and playing. raycast = first hit along a ray; raycast_all = every entity along it; overlap_circle / "
             "overlap_box = entities touching the shape at origin (is this spot free?); point = entities containing the "
             "point. Sensors are ignored unless include_sensors. Example: {\"type\": \"raycast\", \"origin\": [2, 5], "
             "\"direction\": [0, -1]}.",
             "physics",
             object({{"type", enumeration({"raycast", "raycast_all", "overlap_circle", "overlap_box", "point"}, "Query kind")},
                     {"origin", vec2("Ray start, shape center or point [x, y]")},
                     {"direction", vec2("Ray direction [x, y] (default [0, -1])")},
                     {"max_distance", number("Max ray distance (default 100)")},
                     {"radius", number("overlap_circle radius (default 0.5)")},
                     {"half_extents", vec2("overlap_box half size [x, y] (default [0.5, 0.5])")},
                     {"angle", number("overlap_box rotation in degrees")},
                     {"exclude", array(schema::entity(), "Entities to ignore")},
                     {"include_sensors", boolean("Also report sensors")},
                     {"layers", array(Json::object({{"type", "string"}}), "Only these collision layers")}},
                    {"type", "origin"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 physics2d::Filter2D f;
                 f.includeSensors = a.get("include_sensors").asBool(false);
                 auto mask = layerMask(a.get("layers"));
                 if (!mask) return ToolResult::error(mask.error());
                 f.layerMask = *mask;
                 for (const auto& ref : a.get("exclude").elements()) {
                     auto id = resolve(engine, ref);
                     if (!id) return ToolResult::error(id.error());
                     f.exclude.push_back(*id);
                 }
                 Vec2 origin, dir{0.f, -1.f}, half{0.5f, 0.5f};
                 if (!reflect::jsonToVec2(a.get("origin"), origin)) {
                     return ToolResult::error(Error::make("invalid_arguments", "origin must be [x, y]", "e.g. {\"origin\": [2, 5]}"));
                 }
                 if (a.contains("direction")) reflect::jsonToVec2(a.get("direction"), dir);
                 if (a.contains("half_extents")) reflect::jsonToVec2(a.get("half_extents"), half);
                 const float maxDist = std::clamp(a.get("max_distance").asFloat(100.f), 0.f, 100000.f);
                 physics2d::Physics2DWorld& w = engine.physics2d().queryWorld();
                 const std::string type = a.get("type").asString();
                 auto list = [&](const std::vector<EntityId>& ids) {
                     Json out = Json::array();
                     for (EntityId e : ids) out.push(Json::object({{"id", e}, {"name", s.record(e)->name}}));
                     return out;
                 };
                 if (type == "overlap_circle" || type == "overlap_box" || type == "point") {
                     std::vector<EntityId> ids = type == "point"          ? w.pointQuery(origin, f)
                                                 : type == "overlap_box" ? w.overlapBox(origin, half, a.get("angle").asFloat(0.f), f)
                                                                         : w.overlapCircle(origin, a.get("radius").asFloat(0.5f), f);
                     Json l = list(ids);
                     std::string text = l.size() ? std::to_string(l.size()) + " entities" : (type == "point" ? "nothing at the point" : "the space is free");
                     return ToolResult::json(Json::object({{"entities", l}, {"free", l.size() == 0}, {"warnings", warningsJson(engine)}}), text);
                 }
                 if (std::hypot(dir.x, dir.y) < 1e-6f) {
                     return ToolResult::error(Error::make("invalid_arguments", "direction must not be zero", "e.g. {\"direction\": [0, -1]}"));
                 }
                 if (type == "raycast_all") {
                     Json hits = Json::array();
                     for (const auto& h : w.raycastAll(origin, dir, maxDist, f)) hits.push(hitJson(s, h));
                     return ToolResult::json(Json::object({{"hits", hits}, {"warnings", warningsJson(engine)}}), std::to_string(hits.size()) + " hits");
                 }
                 auto hit = w.raycast(origin, dir, maxDist, f);
                 if (!hit) {
                     return ToolResult::json(Json::object({{"hit", Json()}, {"warnings", warningsJson(engine)}}),
                                             "no hit within " + std::to_string(static_cast<int>(maxDist)) + " units");
                 }
                 Json h = hitJson(s, *hit);
                 return ToolResult::json(Json::object({{"hit", h}, {"warnings", warningsJson(engine)}}),
                                         "hit " + describe(s, hit->entity) + " at " + h.get("point").dump() + ", distance " + h.get("distance").dump());
             }});
}

}  // namespace sky::tools
