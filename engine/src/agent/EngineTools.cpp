// The engine's tool catalogue. Descriptions are written for language models: they say
// when to use a tool, what it returns, and how it composes with others.

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/wander/Compiler.h"
#include "ToolHelpers.h"
#include "skywalker/render/Hdr.h"
#include "skywalker/render/DebugViews.h"

namespace sky {

namespace tools {

using namespace schema;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

Result<EntityId> resolve(Engine& engine, const Json& ref) {
    Scene& s = engine.scene();
    EntityId id = kNoEntity;
    if (ref.isNumber()) {
        id = static_cast<EntityId>(ref.asInt());
        if (!s.exists(id)) id = kNoEntity;
    } else if (ref.isString()) {
        id = s.find(ref.asString());
    }
    if (id) return id;
    std::vector<std::string> names;
    for (EntityId e : s.entities()) names.push_back(s.record(e)->name);
    std::string guess = ref.isString() ? str::closest(ref.asString(), names, 3) : "";
    return Error::make("not_found", "no entity " + ref.dump(),
                       guess.empty() ? "call scene_overview or scene_query to list entities"
                                     : "did you mean \"" + guess + "\"?");
}

std::string fmtVec(Vec3 v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "[%g, %g, %g]", std::round(v.x * 100) / 100, std::round(v.y * 100) / 100,
                  std::round(v.z * 100) / 100);
    return buf;
}

/// One-line, token-efficient entity description.
std::string describe(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    std::ostringstream os;
    os << "#" << e << " " << r->name;
    std::vector<std::string> bits;
    if (const auto* m = s.get<MeshRenderer>(e)) bits.push_back(m->mesh + " " + reflect::toHexColor(m->color));
    if (const auto* l = s.get<Light>(e)) bits.push_back(l->kind + " light");
    if (s.get<Camera>(e)) bits.push_back("camera");
    if (const auto* b = s.get<Behavior>(e)) {
        std::string names;
        for (const auto& sc : b->scripts) names += (names.empty() ? "" : ",") + sc.name;
        bits.push_back("behaviors:" + names);
    }
    if (!bits.empty()) {
        os << " (";
        for (size_t i = 0; i < bits.size(); ++i) os << (i ? "; " : "") << bits[i];
        os << ")";
    }
    if (const auto* t = s.get<Transform>(e)) {
        os << " pos " << fmtVec(t->position);
        if (t->rotation != Vec3{0, 0, 0}) os << " rot " << fmtVec(t->rotation);
        if (t->scale != Vec3{1, 1, 1}) os << " scale " << fmtVec(t->scale);
    }
    if (!r->tags.empty()) {
        os << " tags:";
        for (const auto& t : r->tags) os << " " << t;
    }
    if (!r->enabled) os << " [disabled]";
    if (r->unique) os << " [unique]";
    if (r->prefab.linked() && r->prefab.instance == e) os << " [prefab " << r->prefab.source << "]";
    return os.str();
}

Json briefJson(const Scene& s, EntityId e) {
    const EntityRecord* r = s.record(e);
    Json comps = Json::array();
    for (const auto& k : s.componentKinds()) {
        if (k.has(s, e)) comps.push(k.name);
    }
    if (s.get<Behavior>(e)) comps.push("behaviors");
    Json j = Json::object({{"id", e}, {"name", r->name}, {"parent", r->parent}, {"enabled", r->enabled}, {"components", comps}});
    if (const auto* t = s.get<Transform>(e)) j["position"] = reflect::vec3ToJson(t->position);
    if (r->prefab.linked()) j["prefab"] = r->prefab.instance == e ? Json(r->prefab.source) : Json(r->prefab.instance);
    return j;
}

ToolResult fail(const Status& s) { return ToolResult::error(s.error()); }

Json vecArg(const Json& args, const char* key, bool& present, Vec3& out) {
    present = args.contains(key) && reflect::jsonToVec3(args.get(key), out);
    return args.get(key);
}

ToolResult entityResult(Engine& engine, EntityId id, const std::string& verb) {
    Json doc = engine.scene().entityToJson(id);
    return ToolResult::json(doc, verb + " " + describe(engine.scene(), id));
}


Json presetPatch(const std::string& name) {
    if (name == "noon") {
        return Json::parse(R"({"sunElevation":70,"sunAzimuth":30,"sunColor":"#fff6e8","sunIntensity":2.6,
            "skyTop":"#3f7fe0","skyHorizon":"#cfe3ff","ambient":0.4,"fogColor":"#cfe3ff","fogDensity":0.006,"exposure":1})").value();
    }
    if (name == "sunset") {
        return Json::parse(R"({"sunElevation":8,"sunAzimuth":250,"sunColor":"#ffb070","sunIntensity":2.4,
            "skyTop":"#3b4a8a","skyHorizon":"#ff9e6b","ground":"#5a4040","ambient":0.3,"fogColor":"#e8a07a","fogDensity":0.012,"exposure":1.1})").value();
    }
    if (name == "night") {
        return Json::parse(R"({"sunElevation":35,"sunAzimuth":140,"sunColor":"#9fb4ff","sunIntensity":0.35,
            "skyTop":"#050a1a","skyHorizon":"#1a2440","ground":"#101418","ambient":0.12,"fogColor":"#141c30","fogDensity":0.02,"exposure":1.3})").value();
    }
    if (name == "overcast") {
        return Json::parse(R"({"sunElevation":60,"sunAzimuth":0,"sunColor":"#e6ebf0","sunIntensity":0.6,
            "skyTop":"#9aa5b1","skyHorizon":"#c9d0d6","ground":"#6b6f72","ambient":0.85,"fogColor":"#c0c7cd","fogDensity":0.015,"exposure":1})").value();
    }
    if (name == "studio") {
        return Json::parse(R"({"sunElevation":45,"sunAzimuth":45,"sunColor":"#ffffff","sunIntensity":2,
            "skyTop":"#2b2d33","skyHorizon":"#4a4d55","ground":"#30323a","ambient":0.55,"fogDensity":0,"exposure":1})").value();
    }
    return {};
}

void duplicateTree(Scene& s, EntityId src, EntityId newParent, const std::string& name, Vec3 offset, bool root,
                   std::vector<EntityId>& created) {
    // Links inside the copied subtree point at the copies; prefab instances stay linked.
    std::vector<EntityId> roots = s.cloneTrees(s, {src}, newParent);
    if (roots.empty()) return;
    EntityId copy = roots.front();
    if (!name.empty()) (void)s.rename(copy, name);
    if (root) {
        if (auto* t = s.get<Transform>(copy)) t->position += offset;
    }
    collectSubtree(s, copy, created);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

}  // namespace tools

namespace {

using namespace schema;
using namespace tools;

void addSceneTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"engine_info", "Engine info",
             "Version, renderer, play state, component types and tool categories. Call once at the start of a session. "
             "shaders reports how the shader library was loaded (library \"metallib\" precompiled or \"source\" compiled at "
             "startup), shaderCompileMs, renderer startupMs and the pipeline cache (hits/misses, archive path).",
             "scene", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 RendererInfo ri = engine.renderer().info();
                 Json cats = Json::object();
                 for (const auto& t : engine.tools().all()) cats[t.category].push(t.name);
                 for (const auto& t : engine.tools().dynamicTools()) cats[t.category].push(t.name);  // custom and hosted tools
                 Json j = Json::object({{"engine", "Skywalker"},
                                        {"version", SKY_VERSION_STRING},
                                        {"renderer", ri.backend + " (" + ri.device + ")"},
                                        {"playState", toString(engine.playState())},
                                        {"components", Json::array()},
                                        {"primitives", Json::array()},
                                        {"tools", cats},
                                        {"conventions", "meters, +Y up, entities face -Z, rotations in Euler degrees "
                                                        "[pitch, yaw, roll], colors as \"#rrggbb\""}});
                 j["shaders"] = ri.shaders;  // [shader cache] library origin, compile/startup ms, pipeline archive
                 if (ri.shaders.contains("shaderCompileMs")) {
                     j["shaderCompileMs"] = ri.shaders.get("shaderCompileMs");
                     j["rendererStartupMs"] = ri.shaders.get("startupMs");
                 }
                 for (const auto& n : engine.scene().componentNames()) j["components"].push(n);
                 for (const auto& p : MeshRenderer::primitives()) j["primitives"].push(p);
                 return ToolResult::json(j);
             }});

    reg.add({"scene_overview", "Scene overview",
             "Compact outline of the whole scene: every entity as one line (id, name, mesh/color, position, tags) in "
             "hierarchy order, plus environment, selection and the scene file (`path`; empty until saved). Start here "
             "before editing.",
             "scene", object({{"max_entities", integer("Limit lines (default 300)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 const Environment& env = s.environment();
                 size_t limit = static_cast<size_t>(a.get("max_entities").asInt(300));
                 std::ostringstream os;
                 os << "Scene \"" << s.name << "\" ("
                    << (engine.scenePath().empty() ? std::string("not saved yet") : engine.scenePath()) << "): " << s.size()
                    << " entities, state " << toString(engine.playState());
                 if (!engine.selection().empty()) {
                     os << ", selected:";
                     for (EntityId id : engine.selection()) os << " #" << id;
                 }
                 os << "\nEnvironment: sun elev " << env.sunElevation << " az " << env.sunAzimuth << " intensity "
                    << env.sunIntensity << ", ambient " << env.ambient << ", fog " << env.fogDensity << ", exposure "
                    << env.exposure << "\n";
                 size_t lines = 0;
                 std::function<void(EntityId, int)> walk = [&](EntityId e, int depth) {
                     if (lines++ >= limit) return;
                     os << std::string(static_cast<size_t>(depth) * 2, ' ') << describe(s, e) << "\n";
                     for (EntityId c : s.children(e)) walk(c, depth + 1);
                 };
                 Json list = Json::array();
                 for (EntityId e : s.entities()) {
                     if (s.record(e)->parent == kNoEntity) walk(e, 0);
                     if (list.size() < limit) list.push(briefJson(s, e));
                 }
                 if (lines > limit) os << "... (" << s.size() - limit << " more; use scene_query)\n";
                 Json sel = Json::array();
                 for (EntityId id : engine.selection()) sel.push(id);
                 ToolResult r = ToolResult::text(os.str());
                 r.structured = Json::object({{"name", s.name}, {"path", engine.scenePath()}, {"count", s.size()},
                                              {"selection", sel}, {"entities", list}});
                 return r;
             }});

    reg.add({"scene_query", "Find entities",
             "Find entities by name glob (e.g. \"tree*\"), tag, component, or proximity. Returns one line per match.",
             "scene",
             object({{"name", string("Name glob, case-insensitive (* and ?)")},
                     {"tag", string("Required tag")},
                     {"component", string("Required component (transform, mesh, light, camera, behaviors)")},
                     {"near", vec3("Only entities within `radius` of this point")},
                     {"radius", number("Radius for `near` (default 5)")},
                     {"limit", integer("Max results (default 50)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 Vec3 nearP;
                 bool hasNear = reflect::jsonToVec3(a.get("near"), nearP);
                 float radius = a.get("radius").asFloat(5.f);
                 size_t limit = static_cast<size_t>(a.get("limit").asInt(50));
                 std::string out;
                 Json list = Json::array();
                 for (EntityId e : s.entities()) {
                     const EntityRecord* r = s.record(e);
                     if (a.contains("name") && !str::globMatch(a.get("name").asString(), r->name)) continue;
                     if (a.contains("tag") &&
                         std::find(r->tags.begin(), r->tags.end(), a.get("tag").asString()) == r->tags.end()) continue;
                     if (a.contains("component")) {
                         const std::string& c = a.get("component").asString();
                         if (c == "behaviors" ? !s.get<Behavior>(e)
                                              : (!s.componentKind(c) || !s.componentKind(c)->has(s, e))) continue;
                     }
                     if (hasNear && distance(s.worldMatrix(e).translation(), nearP) > radius) continue;
                     if (list.size() >= limit) break;
                     out += describe(s, e) + "\n";
                     list.push(briefJson(s, e));
                 }
                 ToolResult r = ToolResult::text(out.empty() ? "no matches" : out);
                 r.structured = Json::object({{"matches", list}});
                 return r;
             }});

    reg.add({"entity_get", "Get entity",
             "Full data of one entity: components (all fields), tags, vars, behaviors (intent + Wander source).",
             "entity", object({{"entity", schema::entity()}}, {"entity"}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 return ToolResult::json(engine.scene().entityToJson(*id));
             }});

    reg.add({"entity_create", "Create entity",
             "Create an entity. Shorthands: mesh (primitive name), color, position, rotation (degrees), scale. "
             "`components` takes full component objects, e.g. {\"light\": {\"kind\": \"point\", \"intensity\": 3}}. "
             "Returns the new id.",
             "entity",
             object({{"name", string("Display name")},
                     {"parent", schema::entity("Parent entity (id or name)")},
                     {"mesh", string("Primitive: cube, sphere, plane, cylinder, cone, quad, capsule, torus")},
                     {"color", string("Base color \"#rrggbb\"")},
                     {"position", vec3("Position [x, y, z] in meters")},
                     {"rotation", vec3("Rotation [pitch, yaw, roll] in degrees")},
                     {"scale", vec3("Scale [x, y, z]")},
                     {"tags", array(Json::object({{"type", "string"}}), "Tags")},
                     {"vars", Json::object({{"type", "object"}, {"description", "Free-form entity variables"}})},
                     {"components", Json::object({{"type", "object"}, {"description", "Component name -> fields"}})}},
                    {"name"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 EntityId created = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Create " + a.get("name").asString(), [&]() -> Status {
                     Scene& s = engine.scene();
                     EntityId parent = kNoEntity;
                     if (a.contains("parent")) {
                         auto p = resolve(engine, a.get("parent"));
                         if (!p) return p.error();
                         parent = *p;
                     }
                     created = s.create(a.get("name").asString(), parent);
                     Json doc = Json::object();
                     Json comps = a.get("components").isObject() ? a.get("components") : Json::object();
                     Json t = comps.get("transform").isObject() ? comps.get("transform") : Json::object();
                     for (const char* k : {"position", "rotation", "scale"}) {
                         if (a.contains(k)) t[k] = a.get(k);
                     }
                     if (t.size()) comps["transform"] = t;
                     if (a.contains("mesh") || a.contains("color")) {
                         Json m = comps.get("mesh").isObject() ? comps.get("mesh") : Json::object();
                         if (a.contains("mesh")) m["mesh"] = a.get("mesh");
                         if (a.contains("color")) m["color"] = a.get("color");
                         comps["mesh"] = m;
                     }
                     doc["components"] = comps;
                     if (a.contains("tags")) doc["tags"] = a.get("tags");
                     if (a.contains("vars")) doc["vars"] = a.get("vars");
                     return s.applyEntityJson(created, doc);
                 });
                 if (!st) return fail(st);
                 return entityResult(engine, created, "created");
             }});

    reg.add({"entity_update", "Update entity",
             "Change an entity. Only given keys change; component objects are merged field-by-field. Set a "
             "component to null to remove it. Example: {\"entity\": \"Lamp\", \"components\": {\"light\": "
             "{\"intensity\": 5}}}.",
             "entity",
             object({{"entity", schema::entity()},
                     {"name", string("New name")},
                     {"parent", schema::entity("New parent (0 for root)")},
                     {"enabled", boolean("Enable/disable")},
                     {"tags", array(Json::object({{"type", "string"}}), "Replace tags")},
                     {"vars", Json::object({{"type", "object"}, {"description", "Merge into vars (null deletes)"}})},
                     {"unique", boolean("Unique name in its prefab instance / scene: Wander find(\"%Name\") finds it")},
                     {"components", Json::object({{"type", "object"}, {"description", "Component name -> partial fields"}})}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Json doc = a;
                 doc.erase("entity");
                 std::string oldName = engine.scene().record(*id)->name;
                 Status st = engine.edit(ctx.actor, "Edit " + oldName,
                                         [&] { return engine.scene().applyEntityJson(*id, doc); });
                 if (!st) return fail(st);
                 ToolResult r = entityResult(engine, *id, "updated");
                 if (engine.scene().record(*id)->name != oldName) {
                     // Links store ids: they follow the rename. Name-only links (and Wander find("Old")) do not.
                     size_t byId = 0, byName = 0;
                     for (EntityId e : engine.scene().entities()) {
                         for (const auto& l : engine.scene().linksFrom(e)) {
                             if (l.target == *id) ++byId;
                             if (!l.target && !l.link.id && l.link.name == oldName) ++byName;
                         }
                     }
                     std::string note = "renamed \"" + oldName + "\": " + std::to_string(byId) + " link(s) follow it";
                     if (byName) note += "; " + std::to_string(byName) + " name-only link(s) still say \"" + oldName + "\" (entity_refs lists them)";
                     if (!r.content.empty()) r.content.front().text = note + "\n" + r.content.front().text;
                     r.structured["renamed"] = Json::object({{"from", oldName}, {"links_following", byId}, {"links_broken", byName}});
                 }
                 return r;
             }});

    reg.add({"transform", "Transform entity",
             "Move/rotate/scale an entity. Absolute: position, rotation (degrees), scale. Relative: translate, rotate. "
             "space=world interprets position in world space even if the entity has a parent.",
             "entity",
             object({{"entity", schema::entity()},
                     {"position", vec3("Absolute position")},
                     {"rotation", vec3("Absolute rotation, degrees")},
                     {"scale", vec3("Absolute scale")},
                     {"translate", vec3("Relative offset")},
                     {"rotate", vec3("Relative rotation, degrees")},
                     {"space", enumeration({"local", "world"}, "Space for `position` (default local)")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Scene& s = engine.scene();
                 Status st = engine.edit(ctx.actor, "Transform " + s.record(*id)->name, [&]() -> Status {
                     Transform t = *s.get<Transform>(*id);
                     Vec3 v;
                     bool has = false;
                     vecArg(a, "position", has, v);
                     if (has) {
                         const EntityRecord* r = s.record(*id);
                         if (a.get("space").asString() == "world" && r->parent) {
                             v = s.worldMatrix(r->parent).inverse().transformPoint(v);
                         }
                         t.position = v;
                     }
                     vecArg(a, "rotation", has, v);
                     if (has) t.rotation = v;
                     vecArg(a, "scale", has, v);
                     if (has) t.scale = v;
                     vecArg(a, "translate", has, v);
                     if (has) t.position += v;
                     vecArg(a, "rotate", has, v);
                     if (has) t.rotation += v;
                     return s.patchComponent(*id, "transform", reflect::toJson(&t, Transform::type()));
                 });
                 if (!st) return fail(st);
                 return ToolResult::text("ok: " + describe(s, *id));
             }});

    reg.add({"entity_delete", "Delete entity", "Delete an entity and all its children (undoable).", "entity",
             object({{"entity", schema::entity()}}, {"entity"}), true, true, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::string name = engine.scene().record(*id)->name;
                 size_t n = 0;
                 Status st = engine.edit(ctx.actor, "Delete " + name, [&]() -> Status {
                     n = engine.scene().destroy(*id);
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::text("deleted " + name + " (" + std::to_string(n) + " entities)");
             }});

    reg.add({"entity_duplicate", "Duplicate entity",
             "Copy an entity (with children, components and behaviors). Optional new name and position offset. "
             "`entities` duplicates several together: links between them (joint targets, look-at, follow...) point at "
             "the copies, so a duplicated rig stays wired to itself. Prefab instances stay linked.",
             "entity",
             object({{"entity", schema::entity()},
                     {"entities", array(schema::entity(), "Several entities duplicated as one group")},
                     {"name", string("Name of the copy (single entity)")},
                     {"offset", vec3("Offset added to the copy's position")},
                     {"count", integer("Number of copies (default 1, max 100); offsets accumulate")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::vector<EntityId> sources;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     sources.push_back(*id);
                 }
                 for (const auto& e : a.get("entities").elements()) {
                     auto id = resolve(engine, e);
                     if (!id) return ToolResult::error(id.error());
                     sources.push_back(*id);
                 }
                 if (sources.empty()) {
                     return ToolResult::error(Error::make("invalid_arguments", "give `entity` or `entities`"));
                 }
                 Scene& s = engine.scene();
                 // Children of another source come along with it.
                 std::vector<EntityId> tops;
                 for (EntityId e : sources) {
                     bool inside = std::find(tops.begin(), tops.end(), e) != tops.end();
                     for (EntityId p = s.record(e)->parent; p && !inside; p = s.record(p)->parent) {
                         inside = std::find(sources.begin(), sources.end(), p) != sources.end();
                     }
                     if (!inside) tops.push_back(e);
                 }
                 sources = std::move(tops);
                 Vec3 offset{0, 0, 0};
                 reflect::jsonToVec3(a.get("offset"), offset);
                 int count = static_cast<int>(std::clamp<int64_t>(a.get("count").asInt(1), 1, 100));
                 std::vector<EntityId> roots;
                 Status st = engine.edit(ctx.actor, "Duplicate " + s.record(sources.front())->name, [&]() -> Status {
                     for (int i = 0; i < count; ++i) {
                         // One clone call per copy: links among the sources are remapped onto the copies.
                         std::vector<EntityId> copies = s.cloneTrees(s, sources, kNoEntity);
                         for (size_t k = 0; k < copies.size() && k < sources.size(); ++k) {
                             EntityId c = copies[k];
                             if (EntityId p = s.record(sources[k])->parent) (void)s.setParent(c, p);
                             std::string name = sources.size() == 1 ? a.get("name").asString(s.record(sources[k])->name + " copy")
                                                                    : s.record(sources[k])->name + " copy";
                             if (count > 1) name += " " + std::to_string(i + 1);
                             (void)s.rename(c, name);
                             if (auto* t = s.get<Transform>(c)) t->position += offset * static_cast<float>(i + 1);
                             roots.push_back(c);
                         }
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string out;
                 Json ids = Json::array();
                 for (EntityId r : roots) {
                     out += describe(s, r) + "\n";
                     ids.push(r);
                 }
                 ToolResult r = ToolResult::text(out);
                 r.structured = Json::object({{"created", ids}});
                 return r;
             }});

    reg.add({"batch", "Batch operations",
             "Run many tool calls atomically as ONE undo step. If any operation fails, everything is rolled back "
             "and the failing index is reported. Use for building scenes efficiently.",
             "scene",
             object({{"operations", array(object({{"tool", string("Tool name")},
                                                  {"args", Json::object({{"type", "object"}, {"description", "Tool arguments"}})}},
                                                 {"tool"}),
                                          "Operations in order")},
                     {"label", string("History label, e.g. \"Build village\"")}},
                    {"operations"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Json results = Json::array();
                 size_t failedAt = SIZE_MAX;
                 ToolResult failure;
                 std::string label = a.get("label").asString("Batch (" + std::to_string(a.get("operations").size()) + " ops)");
                 Status st = engine.edit(ctx.actor, label, [&]() -> Status {
                     for (size_t i = 0; i < a.get("operations").size(); ++i) {
                         const Json& op = a.get("operations")[i];
                         const std::string& tool = op.get("tool").asString();
                         if (tool == "batch" || tool == "scene_load" || tool == "scene_new" || tool == "sim_control" ||
                             tool == "history") {
                             failedAt = i;
                             failure = ToolResult::error(Error::make("invalid_arguments", tool + " cannot run inside a batch"));
                             return Error::make("batch_failed", "operation failed");
                         }
                         ToolResult r = engine.tools().call(tool, op.get("args"), ctx);
                         if (r.isError) {
                             failedAt = i;
                             failure = r;
                             return Error::make("batch_failed", "operation failed");
                         }
                         results.push(r.structured.isNull() ? Json(r.content.empty() ? "" : r.content.front().text)
                                                            : r.structured);
                     }
                     return {};
                 });
                 if (!st) {
                     std::string why = failure.content.empty() ? st.error().message : failure.content.front().text;
                     return ToolResult::error(Error::make("batch_failed",
                                                          "operation " + std::to_string(failedAt) + " failed; nothing was "
                                                          "applied. " + why));
                 }
                 return ToolResult::json(Json::object({{"ok", true}, {"results", results}}),
                                         "applied " + std::to_string(results.size()) + " operations as one undo step");
             }});

    reg.add({"component_schema", "Component schema",
             "JSON schema of components (transform, mesh, light, camera) and the environment, with field docs, "
             "ranges and enums.",
             "entity", object({{"component", string("One component name; omit for all")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 Json out = Json::object();
                 for (const auto& k : engine.scene().componentKinds()) {
                     if (!a.contains("component") || a.get("component").asString() == k.name) {
                         out[k.name] = reflect::schema(*k.info);
                     }
                 }
                 if (!a.contains("component") || a.get("component").asString() == "environment") {
                     out["environment"] = reflect::schema(Environment::type());
                 }
                 if (out.size() == 0) return ToolResult::error(Error::make("not_found", "unknown component"));
                 return ToolResult::json(out);
             }});

    reg.add({"environment_get", "Get lighting & environment",
             "Current sun, sky, ambient, fog, exposure and grid settings.", "render", object({}), false, false,
             [&engine](const Json&, ToolContext&) {
                 return ToolResult::json(reflect::toJson(&engine.scene().environment(), Environment::type()));
             }});

    reg.add({"environment_update", "Lighting & environment",
             "Edit scene lighting/atmosphere: sun (azimuth, elevation, color, intensity), sky colors, ambient, fog, "
             "exposure, grid. Optional preset: noon, sunset, night, overcast, studio (applied first).",
             "render",
             [] {
                 Json s = reflect::schema(Environment::type());
                 s["properties"]["preset"] = enumeration({"noon", "sunset", "night", "overcast", "studio"}, "Lighting preset");
                 s["properties"]["align_sun_to_hdri"] =
                     boolean("Point the sun (direction of light and shadows) at the brightest spot of the hdri panorama");
                 s["description"] = Json();
                 s.erase("description");
                 return s;
             }(),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Json patch = a;
                 Status st = engine.edit(ctx.actor, "Lighting", [&]() -> Status {
                     if (a.contains("preset")) {
                         if (Status s = engine.scene().patchEnvironment(presetPatch(a.get("preset").asString())); !s) return s;
                         patch.erase("preset");
                     }
                     patch.erase("align_sun_to_hdri");
                     if (Status s = patch.size() ? engine.scene().patchEnvironment(patch) : Status{}; !s) return s;
                     if (!a.get("align_sun_to_hdri").asBool()) return {};
                     Environment& env = engine.scene().environment();
                     if (env.hdri.empty()) return Error::make("invalid_arguments", "align_sun_to_hdri needs `hdri` set");
                     auto img = loadHdr(engine.resolvePath(env.hdri));
                     if (!img) return img.error();
                     float x, y, z;
                     if (!hdrSunDirection(*img, x, y, z)) return Error::make("no_sun", "the panorama has no bright spot");
                     // Panorama rotation turns the image around +Y; apply it to the found direction.
                     float r = radians(env.hdriRotation), c = std::cos(r), sn = std::sin(r);
                     // u shifts by +rot: the sun appears at phi - rot.
                     float rx = x * c - (-z) * sn, rz = -(x * sn + (-z) * c);
                     return engine.scene().patchEnvironment(Json::object(
                         {{"sunAzimuth", degrees(std::atan2(rx, rz))}, {"sunElevation", degrees(std::asin(std::clamp(y, -1.f, 1.f)))}}));
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(reflect::toJson(&engine.scene().environment(), Environment::type()), "environment:");
             }});
}

void addSimTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"sim_control", "Simulation control",
             "play / pause / stop the game, or `step` N fixed ticks (1/60 s each) deterministically and get the "
             "resulting logs and errors. stop restores the scene to its pre-play state. Use step + "
             "viewport_capture to test behaviors. Two different pauses: `pause` is the EDITOR pause (nothing ticks; "
             "step still works), `pause_game` is the GAME's pause, like a pause menu (pause_game() in Wander): pausable "
             "entities freeze while UI canvases and `process` mode always/when_paused entities keep running, and "
             "`on pause` fires. `time_scale` with scale (0..10) is slow motion / fast forward for the game clock. Game "
             "pause and time scale apply from the next tick. `interpolation` turns render smoothing between ticks on/off "
             "(real-time frames only). Example: {\"action\":\"pause_game\"} then {\"action\":\"step\",\"ticks\":30}.",
             "sim",
             object({{"action", enumeration({"play", "pause", "stop", "step", "status", "pause_game", "resume_game", "time_scale"},
                                            "What to do")},
                     {"ticks", integer("Ticks for step (default 60 = 1 second, max 36000)")},
                     {"scale", number("time_scale: game clock speed (0..10; 1 = normal, 0.25 = bullet time)")},
                     {"interpolation", boolean("Render interpolation between ticks for real-time frames (default on)")}},
                    {"action"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 const std::string& action = a.get("action").asString();
                 size_t before = engine.recentMessages(100000).size();
                 std::vector<std::string> notes;
                 auto ensurePlaying = [&] {
                     if (engine.playState() == PlayState::Editing) {
                         engine.play();
                         notes.push_back("started play (the game pause and time scale exist only while playing)");
                     }
                 };
                 if (a.contains("interpolation")) engine.setInterpolation(a.get("interpolation").asBool(true));
                 if (action == "play") engine.play();
                 else if (action == "pause") engine.pause();
                 else if (action == "stop") engine.stop();
                 else if (action == "step") engine.step(static_cast<int>(std::clamp<int64_t>(a.get("ticks").asInt(60), 1, 36000)));
                 else if (action == "pause_game" || action == "resume_game") {
                     ensurePlaying();
                     engine.setGamePaused(action == "pause_game");
                     notes.push_back(std::string(action == "pause_game" ? "game paused" : "game resumed") + " from the next tick");
                 } else if (action == "time_scale") {
                     if (!a.get("scale").isNumber()) {
                         return ToolResult::error(Error::make("invalid_argument", "time_scale needs `scale`",
                                                              "e.g. {\"action\": \"time_scale\", \"scale\": 0.25}"));
                     }
                     ensurePlaying();
                     double scale = a.get("scale").asFloat(1.f);
                     if (scale < 0.0 || scale > wander::Runtime::kMaxTimeScale) {
                         notes.push_back("scale clamped to 0..10");
                     }
                     engine.setTimeScale(scale);
                     notes.push_back("time scale applies from the next tick");
                 }
                 auto all = engine.recentMessages(100000);
                 Json fresh = Json::array();
                 for (size_t i = std::min(before, all.size()); i < all.size(); ++i) fresh.push(all[i]);
                 Json j = Json::object({{"state", toString(engine.playState())},
                                        {"time", engine.runtime().time()},
                                        {"unscaledTime", engine.runtime().unscaledTime()},
                                        {"frame", engine.runtime().frame()},
                                        {"game", Json::object({{"paused", engine.gamePaused()},
                                                               {"timeScale", engine.timeScale()},
                                                               {"pausedThisTick", engine.runtime().gamePaused()},
                                                               {"timeScaleThisTick", engine.runtime().timeScale()}})},
                                        {"interpolation", Json::object({{"enabled", engine.interpolation()},
                                                                        {"alpha", engine.interpolationAlpha()}})},
                                        {"messages", fresh}});
                 if (!notes.empty()) {
                     Json n = Json::array();
                     for (const auto& s : notes) n.push(s);
                     j["notes"] = n;
                 }
                 return ToolResult::json(j);
             }});

    reg.add({"sim_input", "Simulate input",
             "Inject player input for the next ticks, like a player (or a playtest bot) would: press keys (fires `on key`), "
             "hold/release keys (for key()), press or hold input ACTIONS (jump, fire, ... see input_map; fires `on action`) "
             "for N ticks (60 ticks = 1 s), hold an axis at a value (move forward: axes=[{name:\"move\", x:0, y:1, "
             "ticks:120}]), set a simulated gamepad (sticks, triggers, buttons) or mouse (position, movement, buttons), "
             "click an entity (fires `on click`), or emit a named event. Applies on the next tick: follow with "
             "sim_control step.",
             "sim",
             object({{"press", array(Json::object({{"type", "string"}}), "Keys pressed once, e.g. [\"space\"]")},
                     {"hold", array(Json::object({{"type", "string"}}), "Keys to start holding, e.g. [\"w\"]")},
                     {"release", array(Json::object({{"type", "string"}}), "Keys to release")},
                     {"actions", array(Json::object({{"description", "Action name, or {name, ticks (default 1 = a tap), x (value)}"}}),
                                       "Input actions to hold for ticks, e.g. [\"jump\"] or [{\"name\":\"fire\",\"ticks\":30}]")},
                     {"axes", array(Json::object({{"type", "object"}}), "Axis actions to hold: [{name, x, y, ticks}] with y = forward/up")},
                     {"release_actions", array(Json::object({{"type", "string"}}), "Actions to stop holding")},
                     {"gamepad", Json::object({{"type", "object"}, {"description", "Simulated controller: {index?, leftStick:[x,y], rightStick:[x,y], leftTrigger, rightTrigger, buttons:[\"south\",...]} (buttons = the full held set; persists until changed)"}})},
                     {"mouse", Json::object({{"type", "object"}, {"description", "Simulated mouse: {x, y (0..1), dx, dy (pixels, y up), scroll, press:[\"left\"], hold, release}"}})},
                     {"click", schema::entity("Entity to click")},
                     {"event", string("Event name to emit")},
                     {"data", any("Event payload, `data` in the handler, e.g. {\"amount\": 2}")},
                     {"target", schema::entity("Event receiver (default: broadcast)")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto& in = engine.input();
                 for (const auto& k : a.get("press").elements()) in.pressed.insert(input::canonicalKey(k.asString()));
                 for (const auto& k : a.get("hold").elements()) in.held.insert(input::canonicalKey(k.asString()));
                 for (const auto& k : a.get("release").elements()) {
                     in.held.erase(input::canonicalKey(k.asString()));
                     in.released.insert(input::canonicalKey(k.asString()));
                 }
                 if (Status s = tools::applySimInput(engine, a); !s) return tools::fail(s);
                 if (a.contains("click")) {
                     auto id = resolve(engine, a.get("click"));
                     if (!id) return ToolResult::error(id.error());
                     in.clicked.push_back(*id);
                 }
                 if (a.contains("event")) {
                     EntityId target = kNoEntity;
                     if (a.contains("target")) {
                         auto id = resolve(engine, a.get("target"));
                         if (!id) return ToolResult::error(id.error());
                         target = *id;
                     }
                     engine.runtime().emitJson(a.get("event").asString(), target, a.get("data"));
                 }
                 return ToolResult::text("input queued (applies on the next tick)");
             }});

    reg.add({"logs", "Runtime logs", "Recent Wander log output and runtime/compile errors.", "sim",
             object({{"limit", integer("Max messages (default 30)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 Json arr = Json::array();
                 for (auto& m : engine.recentMessages(static_cast<size_t>(a.get("limit").asInt(30)))) arr.push(m);
                 return ToolResult::json(Json::object({{"messages", arr}}));
             }});
}

void addViewTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"viewport_capture", "Look at the scene",
             "Render the scene and return a PNG plus every visible entity with its on-screen box [x, y, w, h]. "
             "annotate=true (default) draws each entity's #id on the image so you can match what you see to ids. "
             "Choose the view: the editor camera (default), the game camera (view=\"scene\"), or any eye/target.",
             "view",
             object({{"width", integer("Image width (default 768, max 2048)")},
                     {"height", integer("Image height (default 432, max 2048)")},
                     {"view", enumeration({"editor", "scene"}, "Editor orbit camera or the scene's primary camera")},
                     {"camera_entity", schema::entity("Render from this camera entity")},
                     {"eye", vec3("Custom camera position")},
                     {"target", vec3("Custom look-at point (with eye)")},
                     {"fov", number("Vertical field of view in degrees for the custom view (lens: 25 tele .. 90 wide)")},
                     {"aperture", number("Custom view depth of field f-stop (1.4 shallow .. 16 deep; default off)")},
                     {"focus_distance", number("Custom view focus distance in meters (default: autofocus on the center)")},
                     {"tilt_shift", number("Custom view tilt-shift miniature look: 0..1 blur above and below a sharp band across the "
                                           "middle of the frame (aerial 'toy town' shots; combines with aperture)")},
                     {"annotate", boolean("Draw entity id labels (default true)")},
                     {"overlays", boolean("Editor grid & selection highlight (default true)")},
                     {"samples", integer("Supersampling: jittered sub-frames accumulated (default 4; 1 = fastest preview, "
                                         "16-32 = final-quality stills with noise-free GI and reflections)")},
                     {"clay", boolean("Render every surface as matte white clay (judge form and light; film 'sketch to fill' beats)")},
                     {"debug_view", enumeration(debugViewNames(),
                                                "Diagnostic view instead of the final image. Buffers: albedo, normals, material (roughness red / "
                                                "metallic green), gi, reflections, ao, depth, lighting. Shading: unshaded, lighting_only (white "
                                                "material), emission, specular. Geometry: wireframe, overdraw (heat map), lod (green 0 .. red 3), "
                                                "uv_checker, texel_density (green = 512 texels/m). Lights: shadow_cascades (red/green/blue/yellow), "
                                                "light_complexity (lights per pixel heat map), shadow_atlas (point/spot shadow maps, outlined per light: green "
                                                "re-rendered, blue cached, orange waiting; see shadow_atlas_info). Motion: the velocity buffer (hue = "
                                                "direction, strength = speed). Also sketch, impostors. Full legend: "
                                                "viewport_debug_view {\"list\": true}")},
                     {"quality", enumeration({"full", "balanced", "fast"}, "Viewport quality tier (default full; fast/balanced preview what the editor shows while editing)")},
                     {"include_image", boolean("Return the image (default true); false = only the entity list")},
                     {"save_path", string("Also write the PNG to this project-relative path")},
                     {"alpha", number("While playing: render the in-between frame a real-time display shows this far between "
                                      "the last two ticks (0..1; render interpolation). Default 1 = the exact tick state")},
                     {"frame_handlers", boolean("While playing: also run the cosmetic `on frame` Wander handlers for this "
                                                "image (camera shake, UI tweens); their writes are undone afterwards")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 CaptureOptions o;
                 o.interpolationAlpha = std::clamp(a.get("alpha").asFloat(1.f), 0.f, 1.f);
                 o.frameHandlers = a.get("frame_handlers").asBool(false);
                 o.frameDt = Engine::kFixedDt;
                 o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(768), 16, 2048));
                 o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(432), 16, 2048));
                 o.useSceneCamera = a.get("view").asString() == "scene";
                 if (a.contains("camera_entity")) {
                     auto id = resolve(engine, a.get("camera_entity"));
                     if (!id) return ToolResult::error(id.error());
                     o.cameraEntity = *id;
                 }
                 Vec3 eye, target;
                 if (reflect::jsonToVec3(a.get("eye"), eye)) {
                     o.hasCustomView = true;
                     o.customView = engine.camera().toView();
                     if (!reflect::jsonToVec3(a.get("target"), target)) target = o.customView.target;
                     o.customView.lookFrom(eye, target);
                     if (a.contains("fov")) o.customView.fovDeg = std::clamp(a.get("fov").asFloat(), 5.f, 150.f);
                     o.customView.aperture = std::max(0.f, a.get("aperture").asFloat(0.f));
                     o.customView.focusDistance = std::max(0.f, a.get("focus_distance").asFloat(0.f));
                     o.customView.tiltShift = std::clamp(a.get("tilt_shift").asFloat(0.f), 0.f, 1.f);
                 }
                 o.annotate = a.get("annotate").asBool(true);
                 o.editorOverlays = a.get("overlays").asBool(true);
                 o.samples = static_cast<int>(std::clamp<int64_t>(a.get("samples").asInt(4), 1, 64));
                 {
                     auto dv = debugViewFromName(a.get("debug_view").asString());
                     if (!dv) return ToolResult::error(dv.error());
                     o.debugView = *dv;
                     o.clay = a.get("clay").asBool(false);
                     std::string q = a.get("quality").asString();
                     o.quality = q == "fast" ? 2 : q == "balanced" ? 1 : 0;
                 }
                 auto cap = engine.capture(o);
                 if (!cap) return ToolResult::error(cap.error());
                 Json visible = Json::array();
                 std::ostringstream os;
                 os << "Rendered " << o.width << "x" << o.height << ". Visible entities (nearest first):\n";
                 for (const auto& v : cap->visible) {
                     visible.push(Json::object({{"id", v.id},
                                                {"name", v.name},
                                                {"box", Json::array({std::round(v.x), std::round(v.y), std::round(v.w), std::round(v.h)})},
                                                {"distance", std::round(v.depth * 100) / 100},
                                                {"coverage", std::round(v.coverage * 1000) / 1000}}));
                     char line[200];
                     std::snprintf(line, sizeof(line), "#%llu %s box [%d,%d,%d,%d] dist %.1f\n",
                                   static_cast<unsigned long long>(v.id), v.name.c_str(), static_cast<int>(v.x),
                                   static_cast<int>(v.y), static_cast<int>(v.w), static_cast<int>(v.h), v.depth);
                     os << line;
                 }
                 if (a.contains("save_path")) {
                     if (Status s = writePng(cap->image, engine.resolvePath(a.get("save_path").asString())); !s) {
                         return fail(s);
                     }
                 }
                 ToolResult r = ToolResult::text(os.str());
                 r.structured = Json::object({{"width", o.width},
                                              {"height", o.height},
                                              {"camera", Json::object({{"eye", reflect::vec3ToJson(cap->frame.camera.eye)},
                                                                       {"target", reflect::vec3ToJson(cap->frame.camera.target)}})},
                                              {"visible", visible}});
                 if (a.get("include_image").asBool(true)) {
                     std::vector<uint8_t> png = encodePng(cap->image);
                     r.image(str::base64Encode(png.data(), png.size()));
                 }
                 return r;
             }});

    reg.add({"viewport_pick", "Pick at pixel",
             "Which entity is at pixel (x, y) of a capture with the given size (editor camera). Use with the "
             "boxes/annotations from viewport_capture.",
             "view",
             object({{"x", number("Pixel x")},
                     {"y", number("Pixel y")},
                     {"width", integer("Capture width (default 768)")},
                     {"height", integer("Capture height (default 432)")}},
                    {"x", "y"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 EntityId id = engine.pickAt(a.get("x").asFloat(), a.get("y").asFloat(),
                                             static_cast<int>(a.get("width").asInt(768)),
                                             static_cast<int>(a.get("height").asInt(432)));
                 if (!id) return ToolResult::json(Json::object({{"entity", Json()}}), "nothing there (sky/background)");
                 return ToolResult::json(Json::object({{"entity", id}}), describe(engine.scene(), id));
             }});

    reg.add({"camera_set", "Move editor camera",
             "Point the editor camera (what the human sees and the default capture view). Use frame to fit an entity "
             "(or \"all\"), or set eye/target, or orbit with yaw/pitch/distance.",
             "view",
             object({{"frame", schema::entity("Entity to frame, or \"all\"")},
                     {"eye", vec3("Camera position")},
                     {"target", vec3("Look-at point")},
                     {"yaw", number("Orbit yaw degrees")},
                     {"pitch", number("Orbit pitch degrees")},
                     {"distance", number("Orbit distance")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 OrbitCamera& cam = engine.camera();
                 Scene& s = engine.scene();
                 if (a.contains("frame")) {
                     Aabb box{Vec3(1e30f), Vec3(-1e30f)};
                     bool any = false;
                     auto include = [&](EntityId e) {
                         Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
                         box.min = vmin(box.min, b.min);
                         box.max = vmax(box.max, b.max);
                         any = true;
                     };
                     if (a.get("frame").asString() == "all") {
                         for (EntityId e : s.entities()) {
                             const auto* m = s.get<MeshRenderer>(e);
                             const auto* t = s.get<Transform>(e);
                             // skip huge ground planes so "all" frames the interesting content
                             if (m && t && !(m->mesh == "plane" && t->scale.x > 10)) include(e);
                         }
                     } else {
                         auto id = resolve(engine, a.get("frame"));
                         if (!id) return ToolResult::error(id.error());
                         include(*id);
                     }
                     if (any) cam.frame(box);
                 }
                 Vec3 eye, target;
                 bool hasEye = reflect::jsonToVec3(a.get("eye"), eye);
                 bool hasTarget = reflect::jsonToVec3(a.get("target"), target);
                 if (hasEye) cam.lookAt(eye, hasTarget ? target : cam.target);
                 else if (hasTarget) cam.target = target;
                 if (a.contains("yaw")) cam.yaw = a.get("yaw").asFloat();
                 if (a.contains("pitch")) cam.pitch = std::clamp(a.get("pitch").asFloat(), -89.f, 89.f);
                 if (a.contains("distance")) cam.distance = std::max(0.2f, a.get("distance").asFloat());
                 return ToolResult::json(cam.toJson(), "camera:");
             }});

    reg.add({"viewport_quality", "Viewport quality",
             "How the live editor viewport renders while editing: fast (default; lower internal resolution, no "
             "screen-space GI/reflections or light shafts, near foliage shadows, coarser LODs) keeps heavy worlds "
             "responsive; balanced; full (what the game and captures show). Play mode always renders full.",
             "view", object({{"quality", enumeration({"fast", "balanced", "full"}, "Editing quality (omit to read)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 static const char* names[] = {"full", "balanced", "fast"};
                 if (a.contains("quality")) {
                     std::string q = a.get("quality").asString();
                     engine.setViewportQuality(q == "full" ? ViewportQuality::Full
                                               : q == "balanced" ? ViewportQuality::Balanced
                                                                 : ViewportQuality::Fast);
                 }
                 std::string cur = names[static_cast<int>(engine.viewportQuality())];
                 ToolResult r = ToolResult::text("viewport quality: " + cur);
                 r.structured = Json::object({{"quality", cur}});
                 return r;
             }});

    reg.add({"viewport_debug_view", "Viewport debug view",
             "Show a debug visualization in the live editor viewport (the human sees it too), or list every view with its "
             "color legend (list=true). Views: wireframe, overdraw, unshaded, lighting_only, shadow_cascades, light_complexity, "
             "lod, emission, specular, uv_checker, texel_density, plus the buffers albedo, normals, material, gi, reflections, "
             "ao, depth, lighting and sketch/impostors. \"final\" turns it off. For a one-off image use viewport_capture "
             "{debug_view}. Example: {\"view\": \"overdraw\"}.",
             "view",
             object({{"view", enumeration(debugViewNames(), "Debug view to show (final = normal image); omit to read the current one")},
                     {"list", boolean("Return every view with its kind and color legend")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 if (a.contains("view")) {
                     auto dv = debugViewFromName(a.get("view").asString());
                     if (!dv) return ToolResult::error(dv.error());
                     engine.setViewportDebugView(*dv);
                 }
                 const int cur = engine.viewportDebugView();
                 Json j = Json::object({{"view", debugViewName(cur)}});
                 for (const auto& v : debugViews()) {
                     if (v.id == cur) j["legend"] = v.description;
                 }
                 if (a.get("list").asBool(false)) {
                     Json list = Json::array();
                     for (const auto& v : debugViews()) {
                         list.push(Json::object({{"view", v.name}, {"kind", v.kind}, {"legend", v.description}}));
                     }
                     j["views"] = list;
                 }
                 return ToolResult::json(j, std::string("viewport debug view: ") + debugViewName(cur));
             }});

    reg.add({"selection_get", "Get selection",
             "Entities the human currently has selected in the editor (\"this\", \"these\" usually means them).",
             "view", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 std::string out;
                 Json ids = Json::array();
                 for (EntityId id : engine.selection()) {
                     out += describe(engine.scene(), id) + "\n";
                     ids.push(id);
                 }
                 ToolResult r = ToolResult::text(out.empty() ? "nothing selected" : out);
                 r.structured = Json::object({{"selection", ids}});
                 return r;
             }});

    reg.add({"selection_set", "Select entities", "Select entities in the editor to show the human what you mean.",
             "view", object({{"entities", array(schema::entity(), "Entities to select (empty clears)")}}, {"entities"}),
             false, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::vector<EntityId> ids;
                 for (const auto& e : a.get("entities").elements()) {
                     auto id = resolve(engine, e);
                     if (!id) return ToolResult::error(id.error());
                     ids.push_back(*id);
                 }
                 engine.setSelection(ids, ctx.actor);
                 return ToolResult::text("selected " + std::to_string(ids.size()));
             }});
}

void addHistoryAndFileTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"history", "Undo / redo / log",
             "Undo or redo edits (by anyone), or list recent history entries with who made them.", "history",
             object({{"action", enumeration({"undo", "redo", "list"}, "What to do")},
                     {"steps", integer("How many steps for undo/redo (default 1)")},
                     {"limit", integer("Entries for list (default 20)")}},
                    {"action"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 History& h = engine.history();
                 const std::string& action = a.get("action").asString();
                 if (engine.playState() != PlayState::Editing && action != "list") {
                     return ToolResult::error(Error::make("invalid_state", "stop the simulation before undo/redo"));
                 }
                 if (action == "list") {
                     Json arr = Json::array();
                     std::string out;
                     size_t limit = static_cast<size_t>(a.get("limit").asInt(20));
                     const auto& es = h.entries();
                     for (size_t i = es.size() > limit ? es.size() - limit : 0; i < es.size(); ++i) {
                         Json j = es[i].summary();
                         j["undone"] = i >= h.cursor();
                         out += (i >= h.cursor() ? "(undone) " : "") + es[i].actor + ": " + es[i].label + "\n";
                         arr.push(j);
                     }
                     ToolResult r = ToolResult::text(out.empty() ? "history is empty" : out);
                     r.structured = Json::object({{"entries", arr}});
                     return r;
                 }
                 int steps = static_cast<int>(std::clamp<int64_t>(a.get("steps").asInt(1), 1, 100));
                 std::string out;
                 for (int i = 0; i < steps; ++i) {
                     const HistoryEntry* e = action == "undo" ? h.undo() : h.redo();
                     if (!e) break;
                     out += action + ": " + e->label + " (by " + e->actor + ")\n";
                     engine.emitEvent(Json::object({{"type", action}, {"actor", ctx.actor}, {"label", e->label}}));
                 }
                 return ToolResult::text(out.empty() ? "nothing to " + action : out);
             }});

    reg.add({"scene_save", "Save scene", "Save the scene as JSON (.sky.json). Path is relative to the project.",
             "scene", object({{"path", string("e.g. scenes/level1.sky.json (default: current file)")}}), true, false,
             [&engine](const Json& a, ToolContext&) {
                 Status s = engine.saveScene(a.get("path").asString());
                 if (!s) return fail(s);
                 return ToolResult::text("saved " + engine.scenePath());
             }});

    reg.add({"scene_load", "Load scene", "Load a scene file (replaces the current scene and clears history).",
             "scene", object({{"path", string("Scene path")}}, {"path"}), true, true,
             [&engine](const Json& a, ToolContext&) {
                 Status s = engine.loadScene(a.get("path").asString());
                 if (!s) return fail(s);
                 std::string text = "loaded \"" + engine.scene().name + "\" (" + std::to_string(engine.scene().size()) + " entities)";
                 Json warnings = Json::array();
                 for (const auto& w : engine.scene().loadWarnings()) {
                     warnings.push(w);
                     text += "\nwarning: " + w;
                 }
                 return ToolResult::json(Json::object({{"name", engine.scene().name},
                                                       {"entities", engine.scene().size()},
                                                       {"warnings", warnings}}),
                                         text);
             }});

    reg.add({"scene_new", "New scene",
             "Start a new scene. By default it contains a ground plane, a cube and a camera; empty=true for nothing.",
             "scene", object({{"name", string("Scene name")}, {"empty", boolean("Start completely empty")}}), true, true,
             [&engine](const Json& a, ToolContext&) {
                 Status s = engine.newScene(a.get("name").asString("Untitled"), !a.get("empty").asBool(false));
                 if (!s) return fail(s);
                 return ToolResult::text("new scene \"" + engine.scene().name + "\"");
             }});
}

void addAssetAndRenderTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"asset_import_mesh", "Import mesh",
             "Import a mesh (.obj, .glb, .gltf — e.g. output of a 3D-generation model) and optionally assign it to an "
             "entity (its mesh becomes \"asset:<path>\"). Meshes are normalized to fit a 1m cube; glTF materials "
             "become a material asset. See also asset_import.",
             "asset",
             object({{"path", string("Project-relative .obj/.glb/.gltf path")},
                     {"entity", schema::entity("Entity to assign the mesh to")}},
                    {"path"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto key = engine.importMesh(a.get("path").asString());
                 if (!key) return ToolResult::error(key.error());
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     Status st = engine.edit(ctx.actor, "Assign mesh", [&] {
                         return engine.scene().patchComponent(*id, "mesh", Json::object({{"mesh", *key}}));
                     });
                     if (!st) return fail(st);
                 }
                 return ToolResult::json(Json::object({{"mesh", *key}}), "imported");
             }});

    reg.add({"asset_request", "Request generated asset",
             "Queue a request for generated content — kind: mesh (3D model), texture, sprite (2D), audio (sfx), music, "
             "video — with a prompt. A connected generator (image/3D/audio model) or an agent fulfills it with "
             "asset_complete; results are applied to `target` automatically.",
             "asset",
             object({{"kind", enumeration({"mesh", "texture", "sprite", "audio", "music", "video"}, "Asset kind")},
                     {"prompt", string("What to generate, in detail")},
                     {"style", string("Optional style guide, e.g. \"low-poly pastel\"")},
                     {"target", schema::entity("Entity that should receive the result")}},
                    {"kind", "prompt"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 AssetRequest req;
                 req.kind = a.get("kind").asString();
                 req.prompt = a.get("prompt").asString();
                 req.style = a.get("style").asString();
                 req.requestedBy = ctx.actor;
                 if (a.contains("target")) {
                     auto id = resolve(engine, a.get("target"));
                     if (!id) return ToolResult::error(id.error());
                     req.target = *id;
                 }
                 AssetRequest& r = engine.addAssetRequest(std::move(req));
                 return ToolResult::json(r.toJson(), "queued asset request #" + std::to_string(r.id));
             }});

    reg.add({"asset_requests", "List asset requests", "List generated-asset requests and their status.", "asset",
             object({{"status", enumeration({"pending", "done", "failed"}, "Filter by status")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 Json arr = Json::array();
                 for (const auto& r : engine.assetRequests()) {
                     if (!a.contains("status") || r.status == a.get("status").asString()) arr.push(r.toJson());
                 }
                 return ToolResult::json(Json::object({{"requests", arr}}));
             }});

    reg.add({"asset_complete", "Complete asset request",
             "Mark a request done with the generated file (project-relative). mesh -> imported and assigned to the "
             "target; texture -> set as the target's texture; sprite -> target becomes a camera-facing textured quad.",
             "asset",
             object({{"id", integer("Request id")},
                     {"path", string("Generated file path")},
                     {"failed", boolean("Mark as failed instead")}},
                    {"id"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 AssetRequest* req = nullptr;
                 for (auto& r : engine.assetRequests()) {
                     if (r.id == static_cast<uint64_t>(a.get("id").asInt())) req = &r;
                 }
                 if (!req) return ToolResult::error(Error::make("not_found", "no such asset request"));
                 if (a.get("failed").asBool()) {
                     req->status = "failed";
                     return ToolResult::json(req->toJson(), "marked failed");
                 }
                 req->path = a.get("path").asString();
                 if (req->path.empty()) return ToolResult::error(Error::make("invalid_arguments", "path is required"));
                 if (req->target && engine.scene().exists(req->target)) {
                     Json patch;
                     if (req->kind == "mesh") {
                         auto key = engine.importMesh(req->path);
                         if (!key) return ToolResult::error(key.error());
                         patch = Json::object({{"mesh", *key}});
                     } else if (req->kind == "texture") {
                         patch = Json::object({{"texture", req->path}});
                     } else if (req->kind == "sprite") {
                         // The target becomes a sprite (camera-facing in 3D scenes; flat when it already was a 2D sprite).
                         EntityId target = req->target;
                         Scene& sc = engine.scene();
                         Json sprite = Json::object({{"texture", req->path}, {"frame", ""}, {"color", "#ffffff"}});
                         if (!sc.get<Sprite>(target)) sprite["billboard"] = "y";
                         Status st = engine.edit(ctx.actor, "Apply generated sprite", [&]() -> Status {
                             if (Status r = sc.patchComponent(target, "sprite", sprite); !r) return r;
                             return sc.get<MeshRenderer>(target) ? sc.patchComponent(target, "mesh", Json()) : Status{};
                         });
                         if (!st) return fail(st);
                     }
                     if (patch.isObject()) {
                         EntityId target = req->target;
                         Status st = engine.edit(ctx.actor, "Apply generated " + req->kind,
                                                 [&] { return engine.scene().patchComponent(target, "mesh", patch); });
                         if (!st) return fail(st);
                     }
                 }
                 // Record provenance so agents can later find and regenerate this asset.
                 if (auto registered = engine.assets().registerFile(engine.resolvePath(req->path))) {
                     (void)engine.assets().updateMeta(
                         (*registered)->path, Json::object({{"source", Json::object({{"kind", req->kind},
                                                                              {"prompt", req->prompt},
                                                                              {"style", req->style},
                                                                              {"by", req->requestedBy},
                                                                              {"completedBy", ctx.actor}})},
                                                     {"description", (*registered)->description.empty() ? Json(req->prompt) : Json()}}));
                 }
                 req->status = "done";
                 engine.emitEvent(Json::object({{"type", "asset_done"}, {"request", req->toJson()}}));
                 return ToolResult::json(req->toJson(), "completed");
             }});

    reg.add({"shader_get", "Get shader source",
             "The renderer's current shader source (Metal Shading Language). Edit it with shader_set.", "render",
             object({}), false, false, [&engine](const Json&, ToolContext&) {
                 std::string src = engine.renderer().shaderSource();
                 if (src.empty()) return ToolResult::error(Error::make("unsupported", "this renderer has no shader source"));
                 return ToolResult::text(src);
             }});

    reg.add({"shader_set", "Hot-reload shaders",
             "Replace the renderer's shader source at runtime. On a compile error nothing changes and the compiler "
             "diagnostics are returned. Function names and struct layouts must be kept.",
             "render", object({{"source", string("Complete shader source")}}, {"source"}), true, false,
             [&engine](const Json& a, ToolContext&) {
                 Status s = engine.renderer().reloadShaders(a.get("source").asString());
                 if (!s) return fail(s);
                 return ToolResult::text("shaders reloaded");
             }});
}

}  // namespace

void registerEngineTools(Engine& engine) {
    ToolRegistry& reg = engine.tools();
    addSceneTools(engine, reg);
    tools::addWanderTools(engine, reg);  // engine/src/agent/WanderTools.cpp
    tools::addNativeTools(engine, reg);  // engine/src/agent/NativeTools.cpp
    addSimTools(engine, reg);
    addViewTools(engine, reg);
    addHistoryAndFileTools(engine, reg);
    addAssetAndRenderTools(engine, reg);
    tools::addAssetTools(engine, reg);
    tools::addWorldTools(engine, reg);
    tools::addNetworkTools(engine, reg);
    tools::addFxTools(engine, reg);
    tools::addTools2D(engine, reg);
    tools::addUiTools(engine, reg);
    tools::addDialogueTools(engine, reg);
    tools::addWorldBuildTools(engine, reg);
    tools::addAudioTools(engine, reg);
    tools::addInputTools(engine, reg);
    tools::addDccTools(engine, reg);
    tools::addStudioTools(engine, reg);
    tools::addPhysicsTools(engine, reg);
    tools::addAnimationTools(engine, reg);
    tools::addHairTools(engine, reg);
    tools::addImpostorTools(engine, reg);
    tools::addGameTools(engine, reg);  // engine/src/agent/GameTools.cpp
    tools::addMovieTools(engine, reg);  // engine/src/agent/MovieTools.cpp (movie renderer)
    tools::addRenderLayerTools(engine, reg);  // engine/src/agent/RenderLayerTools.cpp (render layers, cull masks)
    tools::addProcessTools(engine, reg);  // ProcessTools.cpp: process_info, sim_teleport, sim_display
    tools::addShadowTools(engine, reg);  // engine/src/agent/ShadowTools.cpp (point / spot light shadows)
    tools::addPrefabTools(engine, reg);  // engine/src/agent/PrefabTools.cpp (entity links, linked prefabs)
    tools::addLegalTools(engine, reg);  // engine/src/agent/LegalTools.cpp (terms, privacy, acceptance state)
    tools::addCustomToolTools(engine, reg);  // engine/src/agent/CustomToolTools.cpp (agent-defined tools)
    tools::addAgentLinkTools(engine, reg);  // engine/src/agent/AgentLinkTools.cpp: events_poll, tool_host_*
    tools::addAuditTools(engine, reg);   // engine/src/agent/AuditTools.cpp (scene_audit quality gate)
    tools::addLocaleTools(engine, reg);  // engine/src/agent/LocaleTools.cpp (localization, docs/LOCALIZATION.md)
}

}  // namespace sky
