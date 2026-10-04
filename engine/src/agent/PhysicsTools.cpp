// Physics tools: make things physical with presets, query the collision world (works while
// editing too), settle props with a short simulation that commits as one undoable edit, and
// look at colliders. See docs/PHYSICS.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/physics/DebugDraw.h"
#include "skywalker/physics/PhysicsSystem.h"
#include "skywalker/render/MeshData.h"

namespace sky::tools {

namespace {

using namespace schema;

Json r3(Vec3 v) {
    auto r = [](float x) { return std::round(x * 1000.f) / 1000.f; };
    return Json::array({r(v.x), r(v.y), r(v.z)});
}

/// "entities": [...] or "entity": x, resolved in order without duplicates.
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
    if (out.empty()) return Error::make("invalid_arguments", "give `entity` or `entities`");
    return out;
}

Json hitJson(const Scene& s, const physics::Hit& h) {
    const EntityRecord* r = s.record(h.entity);
    return Json::object({{"entity", h.entity},
                         {"name", r ? r->name : ""},
                         {"point", r3(h.point)},
                         {"normal", r3(h.normal)},
                         {"distance", std::round(h.distance * 1000.f) / 1000.f}});
}

Result<uint32_t> layerMask(const Json& layers) {
    if (!layers.isArray() || layers.size() == 0) return ~0u;
    uint32_t mask = 0;
    const auto& names = physics::layerNameList();
    for (const auto& l : layers.elements()) {
        auto it = std::find(names.begin(), names.end(), l.asString());
        if (it == names.end()) {
            std::string guess = str::closest(l.asString(), names, 3);
            return Error::make("unknown_layer", "unknown collision layer \"" + l.asString() + "\"",
                               guess.empty() ? "layers: default, static, player, enemy, projectile, trigger, debris"
                                             : "did you mean \"" + guess + "\"?");
        }
        mask |= 1u << static_cast<uint32_t>(it - names.begin());
    }
    return mask;
}

/// World bounds of the meshes under `id` (or a 1 m box around it).
/// World bounds of the meshes under `id` (imported meshes measured from their data, so this works
/// before anything was rendered), or a 1 m box around it.
Aabb boundsOf(Engine& engine, EntityId id) {
    const Scene& s = engine.scene();
    std::vector<EntityId> all;
    collectSubtree(s, id, all);
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    bool any = false;
    for (EntityId e : all) {
        const MeshRenderer* m = s.get<MeshRenderer>(e);
        if (!m) continue;
        const MeshData* md = str::startsWith(m->mesh, "asset:") ? engine.cpuMesh(m->mesh) : nullptr;
        Aabb b = (md ? md->bounds : s.localBounds(e)).transformed(s.worldMatrix(e));
        box.min = vmin(box.min, b.min);
        box.max = vmax(box.max, b.max);
        any = true;
    }
    if (any) return box;
    Vec3 p = s.worldMatrix(id).translation();
    return {p - Vec3(0.5f), p + Vec3(0.5f)};
}

float estimateMass(const Aabb& b) {
    Vec3 size = b.max - b.min;
    float volume = std::max(size.x, 0.05f) * std::max(size.y, 0.05f) * std::max(size.z, 0.05f);
    float mass = std::clamp(volume * 300.f, 0.1f, 5000.f);  // ~ hollow props / wooden crates
    return std::round(mass * 100.f) / 100.f;
}

const std::vector<std::string>& presets() {
    static const std::vector<std::string> p{"prop",          "static_level", "kinematic_platform", "player_character",
                                            "npc_character", "trigger_zone", "debris",             "projectile",
                                            "remove"};
    return p;
}

Json mergeInto(Json base, const Json& overrides) {
    if (overrides.isObject()) {
        for (const auto& [k, v] : overrides.members()) base[k] = v;
    }
    return base;
}

/// Applies a preset to one entity (inside an edit). Returns the components it set.
Result<Json> applyPreset(Engine& engine, EntityId id, const std::string& preset, const Json& overrides, Json& children) {
    Scene& s = engine.scene();
    Json changed = Json::array();
    auto patch = [&](EntityId e, const char* comp, const Json& value) -> Status {
        Json v = value.isNull() ? value : mergeInto(value, overrides.get(comp));
        if (Status st = s.patchComponent(e, comp, v); !st) return st;
        changed.push(comp);
        return {};
    };
    auto remove = [&](EntityId e, const char* comp) -> Status {
        if (const ComponentKind* k = s.componentKind(comp); k && k->has(s, e)) return s.patchComponent(e, comp, Json());
        return {};
    };
    Aabb b = boundsOf(engine, id);
    if (preset == "remove") {
        for (const char* c : {"body", "collider", "character", "joint"}) {
            if (Status st = remove(id, c); !st) return st.error();
        }
        changed.push("removed");
        return changed;
    }
    if (preset == "static_level") {
        // Every mesh in the subtree becomes static geometry (exact triangles for models).
        std::vector<EntityId> all;
        collectSubtree(s, id, all);
        for (EntityId e : all) {
            const MeshRenderer* m = s.get<MeshRenderer>(e);
            if (!m || s.get<Collider>(e)) continue;
            bool model = str::startsWith(m->mesh, "asset:") || m->mesh == "torus";
            if (Status st = remove(e, "body"); !st) return st.error();
            if (Status st = patch(e, "collider", Json::object({{"shape", model ? "mesh" : "auto"}})); !st) return st.error();
            if (e != id) children.push(Json::object({{"id", e}, {"name", s.record(e)->name}}));
        }
        if (!s.get<Collider>(id) && !s.get<MeshRenderer>(id) && children.size() == 0) {
            if (Status st = patch(id, "collider", Json::object({{"shape", "auto"}})); !st) return st.error();
        }
        return changed;
    }
    if (preset == "player_character" || preset == "npc_character") {
        Vec3 pos = s.worldMatrix(id).translation();
        float height = std::clamp(b.max.y - b.min.y, 0.5f, 10.f);
        float radius = std::clamp(std::max(b.max.x - b.min.x, b.max.z - b.min.z) * 0.5f, 0.1f, height * 0.5f);
        Vec3 center = b.center() - pos;
        for (const char* c : {"body", "collider"}) {
            if (Status st = remove(id, c); !st) return st.error();
        }
        Json ch = Json::object({{"height", std::round(height * 1000.f) / 1000.f},
                                {"radius", std::round(radius * 1000.f) / 1000.f},
                                {"offset", r3(center)},
                                {"layer", preset == "player_character" ? "player" : "enemy"}});
        if (preset == "npc_character") ch["moveSpeed"] = 3.f;
        if (Status st = patch(id, "character", ch); !st) return st.error();
        return changed;
    }
    if (preset == "trigger_zone") {
        if (Status st = remove(id, "body"); !st) return st.error();
        if (Status st = patch(id, "collider", Json::object({{"shape", "auto"}, {"isTrigger", true}})); !st) return st.error();
        return changed;
    }
    Json body = Json::object({{"motion", "dynamic"}, {"mass", estimateMass(b)}});
    if (preset == "kinematic_platform") body = Json::object({{"motion", "kinematic"}});
    if (preset == "debris") {
        body["layer"] = "debris";
        body["mass"] = std::min(estimateMass(b), 5.f);
    }
    if (preset == "projectile") {
        body["layer"] = "projectile";
        body["ccd"] = true;
        body["mass"] = std::min(estimateMass(b), 1.f);
    }
    if (const Collider* c = s.get<Collider>(id); c && c->isTrigger) {
        if (Status st = patch(id, "collider", Json::object({{"isTrigger", false}})); !st) return st.error();
    }
    if (Status st = remove(id, "character"); !st) return st.error();
    if (Status st = patch(id, "body", body); !st) return st.error();
    if (overrides.contains("collider") && !s.get<Collider>(id)) {
        if (Status st = patch(id, "collider", Json::object()); !st) return st.error();
    }
    return changed;
}

ToolResult debugCapture(Engine& engine, const Json& a) {
    physics::PhysicsWorld& world = engine.physics().queryWorld();
    CaptureOptions o;
    o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(768), 64, 2048));
    o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(432), 64, 2048));
    o.annotate = false;
    o.editorOverlays = false;
    std::string view = a.get("view").asString("editor");
    Aabb focus{Vec3(1e30f), Vec3(-1e30f)};
    bool haveFocus = false;
    if (a.contains("focus")) {
        auto id = resolve(engine, a.get("focus"));
        if (!id) return ToolResult::error(id.error());
        focus = boundsOf(engine, *id);
        haveFocus = true;
    }
    std::vector<physics::DebugShape> shapes = world.debugShapes();
    if (view == "top") {
        if (!haveFocus) {
            for (const auto& d : shapes) {
                for (const Vec3& p : d.lines) {
                    if (length(p) > 5000.f) continue;
                    focus.min = vmin(focus.min, p);
                    focus.max = vmax(focus.max, p);
                }
            }
            if (focus.min.x > focus.max.x) focus = {Vec3(-10.f), Vec3(10.f)};
            // Huge ground slabs would make everything tiny: clamp the framed area.
            Vec3 c = focus.center();
            Vec3 half = vmin(focus.extents(), Vec3(150.f));
            focus = {c - half, c + half};
        }
        o.hasCustomView = true;
        o.customView = debugdraw::topDown(focus, static_cast<float>(o.width) / static_cast<float>(o.height));
        o.fog = false;
    } else if (view == "scene") {
        o.useSceneCamera = true;
    } else if (haveFocus) {
        o.hasCustomView = true;
        o.customView = engine.camera().toView();
        Vec3 c = focus.center();
        float r = std::max(length(focus.extents()), 0.5f);
        o.customView.target = c;
        o.customView.eye = c + normalize(Vec3{1.f, 0.8f, 1.3f}) * (r / std::sin(radians(o.customView.fovDeg * 0.5f)));
    }
    auto cap = engine.capture(o);
    if (!cap) return ToolResult::error(cap.error());
    debugdraw::Canvas canvas(cap->image, cap->frame.camera);
    bool showStatic = a.get("show_static").asBool(true);
    int counts[6] = {0, 0, 0, 0, 0, 0};
    for (const auto& d : shapes) {
        debugdraw::Rgba color{255, 160, 40, 230};  // dynamic, awake
        int k = 0;
        if (d.kind == "dynamic" && d.sleeping) {
            color = {120, 140, 200, 200};
            k = 1;
        } else if (d.kind == "static") {
            if (!showStatic) continue;
            color = {90, 220, 120, 150};
            k = 2;
        } else if (d.kind == "kinematic") {
            color = {60, 220, 255, 230};
            k = 3;
        } else if (d.kind == "trigger") {
            color = {255, 80, 220, 230};
            k = 4;
        } else if (d.kind == "character") {
            color = {255, 240, 60, 240};
            k = 5;
        }
        ++counts[k];
        canvas.lines(d.lines, color, 1);
    }
    physics::Stats st = world.stats();
    Json stats = Json::object({{"bodies", st.bodies},
                               {"dynamic", st.dynamicBodies},
                               {"active", st.activeBodies},
                               {"sleeping", st.sleepingBodies},
                               {"kinematic", st.kinematicBodies},
                               {"static", st.staticBodies},
                               {"triggers", st.triggers},
                               {"characters", st.characters},
                               {"joints", st.joints},
                               {"contacts", st.contacts}});
    Json warnings = Json::array();
    for (const auto& w : engine.physics().recentWarnings()) warnings.push(w);
    std::ostringstream os;
    os << "Collider wireframes (" << (engine.playState() == PlayState::Editing ? "edit-time world" : "running simulation")
       << "): orange = dynamic awake, blue-gray = sleeping, green = static, cyan = kinematic, magenta = trigger, "
          "yellow = character.\n"
       << st.bodies << " bodies (" << st.dynamicBodies << " dynamic: " << st.activeBodies << " awake, " << st.sleepingBodies
       << " asleep; " << st.kinematicBodies << " kinematic; " << st.staticBodies << " static; " << st.triggers
       << " triggers), " << st.characters << " characters, " << st.joints << " joints, " << st.contacts << " touching pairs.";
    if (warnings.size()) os << "\nWarnings: " << warnings.size() << " (see structured.warnings)";
    ToolResult r = ToolResult::text(os.str());
    r.structured = Json::object({{"stats", stats}, {"warnings", warnings}, {"view", view}});
    if (a.get("include_image").asBool(true)) {
        std::vector<uint8_t> png = encodePng(cap->image);
        r.image(str::base64Encode(png.data(), png.size()));
    }
    return r;
}

}  // namespace

void addPhysicsTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"physics_add", "Make physical",
             "Make entities physical with a preset (one undoable edit). prop = dynamic rigid body with a collider fitted "
             "to its mesh and a mass estimated from its size; static_level = static colliders for every mesh under the "
             "entity (triangle-exact for imported models) — use on level roots; kinematic_platform = moved by its "
             "transform/scripts, carries and pushes things; player_character / npc_character = capsule character "
             "controller fitted to the mesh (drive with walk/jump in Wander); trigger_zone = sensor that fires `on "
             "trigger_enter`/`on trigger_exit`; debris = light, does not collide with characters; projectile = fast "
             "(continuous collision); remove = strip body/collider/character/joint. `overrides` patches the components, "
             "e.g. {\"body\": {\"mass\": 80, \"restitution\": 0.6}, \"collider\": {\"shape\": \"sphere\"}}. Example: "
             "{\"entities\": [\"Crate 1\", \"Crate 2\"], \"preset\": \"prop\"}.",
             "physics",
             object({{"entity", schema::entity("Entity (or use entities)")},
                     {"entities", array(schema::entity(), "Entities to change")},
                     {"preset", enumeration(presets(), "What the entities become")},
                     {"overrides", Json::object({{"type", "object"},
                                                 {"description", "Component patches, e.g. {\"body\": {...}, \"collider\": {...}, "
                                                                 "\"character\": {...}}"}})}},
                    {"preset"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto ids = resolveMany(engine, a);
                 if (!ids) return ToolResult::error(ids.error());
                 const std::string preset = a.get("preset").asString();
                 Json results = Json::array();
                 Status st = engine.edit(ctx.actor, "Physics: " + preset, [&]() -> Status {
                     for (EntityId id : *ids) {
                         Json children = Json::array();
                         auto changed = applyPreset(engine, id, preset, a.get("overrides"), children);
                         if (!changed) return changed.error();
                         Json item = Json::object({{"id", id}, {"name", engine.scene().record(id)->name}, {"set", *changed}});
                         if (children.size()) item["children"] = children;
                         Json comps = Json::object();
                         for (const char* c : {"body", "collider", "character"}) {
                             const ComponentKind* k = engine.scene().componentKind(c);
                             if (k && k->has(engine.scene(), id)) comps[c] = k->toJson(engine.scene(), id);
                         }
                         item["components"] = comps;
                         results.push(item);
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string text = preset + " applied to " + std::to_string(results.size()) + " entit" +
                                    (results.size() == 1 ? "y" : "ies") + "; press play (sim_control) to simulate";
                 return ToolResult::json(Json::object({{"entities", results}}), text);
             }});

    reg.add({"physics_query", "Physics query",
             "Ask the collision world (colliders, not render meshes) — works while editing and playing. raycast = first "
             "hit along a ray; raycast_all = every entity along it; shapecast = sweep a sphere/box/capsule (will this "
             "fit / where does it land?); overlap = entities touching a shape at `origin` (is this spot free?). Triggers "
             "are ignored unless include_triggers. Example: {\"type\": \"raycast\", \"origin\": [0, 10, 0], "
             "\"direction\": [0, -1, 0]}.",
             "physics",
             object({{"type", enumeration({"raycast", "raycast_all", "shapecast", "overlap"}, "Query kind")},
                     {"origin", vec3("Ray/sweep start, or the overlap shape center")},
                     {"direction", vec3("Ray/sweep direction (default [0,-1,0])")},
                     {"max_distance", number("Max distance in m (default 100)")},
                     {"shape", enumeration({"sphere", "box", "capsule"}, "shapecast/overlap shape (default sphere)")},
                     {"radius", number("Sphere/capsule radius (default 0.5)")},
                     {"height", number("Capsule total height (default 1.8)")},
                     {"half_extents", vec3("Box half size (default [0.5,0.5,0.5])")},
                     {"rotation", vec3("Shape rotation, Euler degrees")},
                     {"exclude", array(schema::entity(), "Entities to ignore")},
                     {"include_triggers", boolean("Also report trigger zones")},
                     {"layers", array(Json::object({{"type", "string"}}), "Only these collision layers")}},
                    {"type", "origin"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 physics::QueryFilter f;
                 f.includeTriggers = a.get("include_triggers").asBool(false);
                 auto mask = layerMask(a.get("layers"));
                 if (!mask) return ToolResult::error(mask.error());
                 f.layerMask = *mask;
                 for (const auto& ref : a.get("exclude").elements()) {
                     auto id = resolve(engine, ref);
                     if (!id) return ToolResult::error(id.error());
                     f.exclude.push_back(*id);
                 }
                 Vec3 origin, dir{0, -1, 0};
                 reflect::jsonToVec3(a.get("origin"), origin);
                 if (a.contains("direction")) reflect::jsonToVec3(a.get("direction"), dir);
                 float maxDist = std::clamp(a.get("max_distance").asFloat(100.f), 0.f, 100000.f);
                 physics::QueryShape shape;
                 std::string shapeName = a.get("shape").asString("sphere");
                 shape.kind = shapeName == "box"       ? physics::QueryShape::Kind::Box
                              : shapeName == "capsule" ? physics::QueryShape::Kind::Capsule
                                                       : physics::QueryShape::Kind::Sphere;
                 shape.radius = a.get("radius").asFloat(0.5f);
                 shape.height = a.get("height").asFloat(1.8f);
                 if (a.contains("half_extents")) reflect::jsonToVec3(a.get("half_extents"), shape.halfExtents);
                 if (a.contains("rotation")) reflect::jsonToVec3(a.get("rotation"), shape.rotation);
                 physics::PhysicsWorld& w = engine.physics().queryWorld();
                 const std::string type = a.get("type").asString();
                 if ((type == "raycast" || type == "raycast_all" || type == "shapecast") && length(dir) < 1e-6f) {
                     return ToolResult::error(Error::make("invalid_arguments", "direction must not be zero"));
                 }
                 if (type == "overlap") {
                     Json list = Json::array();
                     for (EntityId e : w.overlap(shape, origin, f)) list.push(Json::object({{"id", e}, {"name", s.record(e)->name}}));
                     std::string text = list.size() ? std::to_string(list.size()) + " entities overlap" : "the space is free";
                     return ToolResult::json(Json::object({{"entities", list}, {"free", list.size() == 0}}), text);
                 }
                 if (type == "raycast_all") {
                     Json hits = Json::array();
                     for (const auto& h : w.raycastAll(origin, dir, maxDist, f)) hits.push(hitJson(s, h));
                     return ToolResult::json(Json::object({{"hits", hits}}), std::to_string(hits.size()) + " hits");
                 }
                 std::optional<physics::Hit> hit =
                     type == "shapecast" ? w.shapecast(shape, origin, dir, maxDist, f) : w.raycast(origin, dir, maxDist, f);
                 if (!hit) return ToolResult::json(Json::object({{"hit", Json()}}), "no hit within " + std::to_string(maxDist) + " m");
                 Json h = hitJson(s, *hit);
                 return ToolResult::json(Json::object({{"hit", h}}), "hit " + describe(s, hit->entity) + " at " + h.get("point").dump() +
                                                                         ", " + h.get("distance").dump() + " m");
             }});

    reg.add({"physics_settle", "Settle with physics",
             "Drop objects with a real simulation and keep where they come to rest (simulate, then keep the poses). "
             "Only the listed entities move (as dynamic bodies, even without a body component); everything else "
             "is frozen. The result is ONE undoable edit. Great after scatter/place: rocks, crates, books and debris "
             "end up naturally stacked and resting instead of floating or intersecting. Example: {\"entities\": "
             "[\"Crate 1\", \"Crate 2\", \"Barrel\"], \"seconds\": 4}.",
             "physics",
             object({{"entity", schema::entity("Entity to settle (or use entities)")},
                     {"entities", array(schema::entity(), "Entities to settle (top-level props)")},
                     {"seconds", number("Max simulated seconds (default 4, max 30); stops early when everything sleeps")},
                     {"freeze_others", boolean("Keep other dynamic bodies still (default true)")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 if (engine.playState() != PlayState::Editing) {
                     return ToolResult::error(Error::make("not_editing", "physics_settle works while editing",
                                                          "stop the simulation first (sim_control stop)"));
                 }
                 auto ids = resolveMany(engine, a);
                 if (!ids) return ToolResult::error(ids.error());
                 Scene& s = engine.scene();
                 physics::WorldOptions o;
                 o.forceDynamic.insert(ids->begin(), ids->end());
                 o.freezeOthers = a.get("freeze_others").asBool(true);
                 o.writeBack = false;
                 auto world = engine.physics().makeWorld(std::move(o));
                 world->sync(s, 0.f);
                 const float dt = static_cast<float>(Engine::kFixedDt);
                 int maxTicks = static_cast<int>(std::clamp(a.get("seconds").asFloat(4.f), 0.1f, 30.f) * 60.f);
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
                 Json moved = Json::array();
                 std::vector<std::pair<EntityId, Transform>> finals;
                 for (EntityId id : *ids) {
                     auto t = world->localTransform(s, id);
                     if (!t) continue;
                     Vec3 before = s.worldMatrix(id).translation();
                     finals.emplace_back(id, *t);
                     Mat4 parent = s.record(id)->parent ? s.worldMatrix(s.record(id)->parent) : Mat4{};
                     Vec3 after = parent.transformPoint(t->position);
                     moved.push(Json::object({{"id", id},
                                              {"name", s.record(id)->name},
                                              {"position", r3(after)},
                                              {"moved", std::round(distance(before, after) * 1000.f) / 1000.f},
                                              {"sleeping", world->isSleeping(id)}}));
                 }
                 if (finals.empty()) {
                     return ToolResult::error(Error::make("nothing_to_settle", "none of the entities could be simulated",
                                                          "they may be parts of another body (settle the top-level entity)"));
                 }
                 Status st = engine.edit(ctx.actor, "Settle " + std::to_string(finals.size()) + " objects", [&]() -> Status {
                     for (const auto& [id, t] : finals) {
                         Status ps = s.patchComponent(
                             id, "transform",
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
                                                       {"atRest", asleep}}),
                                         text);
             }});

    reg.add({"physics_debug", "See colliders",
             "Capture the viewport with every collider drawn as a wireframe (color = state) plus physics stats (bodies, "
             "awake/asleep, contacts, triggers, characters, joints) and warnings (bad layers, unsupported shapes, missing "
             "joint targets). Use it to check that colliders match the visuals, triggers sit where you expect, and props "
             "fall asleep. view: editor (default), scene camera, or top (orthographic map).",
             "physics",
             object({{"view", enumeration({"editor", "scene", "top"}, "Camera")},
                     {"focus", schema::entity("Frame this entity")},
                     {"width", integer("Image width (default 768)")},
                     {"height", integer("Image height (default 432)")},
                     {"show_static", boolean("Draw static colliders (default true)")},
                     {"include_image", boolean("Return the image (default true)")}}),
             false, false, [&engine](const Json& a, ToolContext&) { return debugCapture(engine, a); }});

    reg.add({"physics_settings", "Physics settings",
             "Read or change scene-wide physics: gravity (m/s^2), substeps (stability for stacks/fast objects), which "
             "collision layers ignore each other (ignorePairs, e.g. \"debris-player, projectile-projectile\"), sleeping, "
             "on/off. Stored in a `physics_world` component (created on an entity named \"Physics\" when you first change "
             "it). Example: {\"gravity\": [0, -1.62, 0]} for the Moon.",
             "physics",
             object({{"gravity", vec3("Gravity vector in m/s^2")},
                     {"substeps", integer("Collision steps per tick (1..8)")},
                     {"ignorePairs", string("Layer pairs that never collide, comma separated")},
                     {"allowSleep", boolean("Let resting bodies sleep")},
                     {"enabled", boolean("Simulate physics while playing")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 EntityId holder = kNoEntity;
                 for (EntityId e : s.entities()) {
                     if (s.get<PhysicsSettings>(e)) {
                         holder = e;
                         break;
                     }
                 }
                 if (a.size() > 0) {
                     Status st = engine.edit(ctx.actor, "Physics settings", [&]() -> Status {
                         if (!holder) holder = s.create("Physics");
                         return s.patchComponent(holder, "physics_world", a);
                     });
                     if (!st) return fail(st);
                 }
                 if (engine.playState() == PlayState::Editing) (void)engine.physics().queryWorld();  // re-validates layers
                 PhysicsSettings defaults;
                 const PhysicsSettings* cur = holder ? s.get<PhysicsSettings>(holder) : &defaults;
                 Json j = reflect::toJson(cur, PhysicsSettings::type());
                 Json layers = Json::array();
                 for (const auto& l : physics::layerNameList()) layers.push(l);
                 Json result = Json::object({{"settings", j}, {"entity", holder ? Json(holder) : Json()}, {"layers", layers}});
                 Json warnings = Json::array();
                 for (const auto& w : engine.physics().recentWarnings()) warnings.push(w);
                 if (warnings.size()) result["warnings"] = warnings;
                 return ToolResult::json(result, a.size() ? "physics settings updated" : "physics settings");
             }});

    addNavTools(engine, reg);
}

}  // namespace sky::tools
