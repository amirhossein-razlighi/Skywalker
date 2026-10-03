// Spatial & analysis tools: ray casts, surface placement, procedural scattering, simulation
// traces, multi-view captures and performance stats. These give agents "hands and eyes" for
// level building beyond single-entity edits.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"

namespace sky::tools {

void collectSubtree(const Scene& s, EntityId root, std::vector<EntityId>& out) {
    out.push_back(root);
    for (EntityId c : s.children(root)) collectSubtree(s, c, out);
}

std::optional<Aabb> subtreeBounds(const Scene& s, EntityId root) {
    std::vector<EntityId> ids;
    collectSubtree(s, root, ids);
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    bool any = false;
    for (EntityId e : ids) {
        if (!s.get<MeshRenderer>(e)) continue;
        Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
        box.min = vmin(box.min, b.min);
        box.max = vmax(box.max, b.max);
        any = true;
    }
    if (!any) return std::nullopt;
    return box;
}

namespace {

using namespace schema;

/// Moves an entity (in world space) by `delta`, writing through patchComponent so the edit is
/// recorded in the current transaction.
Status translateWorld(Scene& s, EntityId id, Vec3 delta) {
    const Transform* t = s.get<Transform>(id);
    Vec3 local = t ? t->position : Vec3{};
    EntityId parent = s.record(id)->parent;
    if (parent) {
        Mat4 parentWorld = s.worldMatrix(parent);
        Vec3 world = parentWorld.transformPoint(local) + delta;
        local = parentWorld.inverse().transformPoint(world);
    } else {
        local = local + delta;
    }
    return s.patchComponent(id, "transform", Json::object({{"position", reflect::vec3ToJson(local)}}));
}

std::vector<EntityId> allExcept(const Scene& s, const std::vector<EntityId>& keep) {
    std::vector<EntityId> out;
    for (EntityId e : s.entities()) {
        if (std::find(keep.begin(), keep.end(), e) == keep.end()) out.push_back(e);
    }
    return out;
}

Json hitJson(const Scene& s, const Engine::Hit& h) {
    auto r2 = [](float v) { return std::round(v * 1000.f) / 1000.f; };
    return Json::object({{"entity", h.entity},
                         {"name", s.record(h.entity)->name},
                         {"point", Json::array({r2(h.point.x), r2(h.point.y), r2(h.point.z)})},
                         {"normal", Json::array({r2(h.normal.x), r2(h.normal.y), r2(h.normal.z)})},
                         {"distance", r2(h.distance)}});
}

bool rangeArg(const Json& a, const char* key, float& lo, float& hi) {
    const Json& r = a.get(key);
    if (r.isNumber()) {
        lo = hi = r.asFloat();
        return true;
    }
    if (r.isArray() && r.size() == 2) {
        lo = r[0].asFloat();
        hi = r[1].asFloat();
        if (lo > hi) std::swap(lo, hi);
        return true;
    }
    return false;
}

/// Navigates an entity document by a dotted path: "transform.position", "vars.score",
/// "mesh.color", "name", "enabled".
Json readProperty(const Scene& s, EntityId id, const std::string& path) {
    Json doc = s.entityToJson(id);
    auto parts = str::split(path, '.');
    if (parts.empty()) return {};
    const Json* cur = nullptr;
    size_t i = 0;
    if (parts[0] == "vars" || parts[0] == "name" || parts[0] == "enabled" || parts[0] == "tags") {
        cur = doc.find(parts[0]);
        i = 1;
    } else {
        const Json& comps = doc.get("components");
        cur = comps.find(parts[0]);
        i = 1;
    }
    for (; cur && i < parts.size(); ++i) cur = cur->find(parts[i]);
    return cur ? *cur : Json();
}

ViewCamera orthoView(Vec3 center, float radius, Vec3 dir, Vec3 up) {
    ViewCamera v;
    v.orthographic = true;
    v.orthoSize = radius * 1.1f;
    v.target = center;
    v.eye = center + dir * (radius + 1.f);
    v.up = up;
    v.nearPlane = 0.01f;
    v.farPlane = radius * 2.f + 10.f;
    return v;
}

}  // namespace

Status dropToSurface(Engine& engine, EntityId id, float offset) {
    Scene& s = engine.scene();
    auto box = subtreeBounds(s, id);
    Vec3 base;
    if (box) {
        base = {box->center().x, box->min.y, box->center().z};
    } else {
        base = s.worldMatrix(id).transformPoint({0, 0, 0});
    }
    std::vector<EntityId> self;
    collectSubtree(s, id, self);
    float top = box ? box->max.y : base.y;
    auto hit = engine.raycast(Ray{{base.x, top + 0.01f, base.z}, {0, -1, 0}}, self);
    if (!hit) hit = engine.raycast(Ray{{base.x, top + 500.f, base.z}, {0, -1, 0}}, self);
    if (!hit) {
        return Error::make("no_surface", "nothing below " + s.record(id)->name,
                           "add a ground mesh (e.g. a plane) or move the entity above one");
    }
    return translateWorld(s, id, {0, hit->point.y + offset - base.y, 0});
}

void addWorldTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"raycast", "Ray cast",
             "Cast a ray into the scene and get the first mesh hit (triangle-accurate): entity, point, surface "
             "normal and distance. Give origin+direction in world space, or a pixel x,y of an editor-camera "
             "capture (width/height). Use it to find the ground height, check line of sight, or aim placements.",
             "world",
             object({{"origin", vec3("World-space ray origin")},
                     {"direction", vec3("Ray direction (default straight down [0,-1,0])")},
                     {"x", number("Pixel x in an editor-camera capture")},
                     {"y", number("Pixel y in an editor-camera capture")},
                     {"width", integer("Capture width (default 768)")},
                     {"height", integer("Capture height (default 432)")},
                     {"exclude", array(entity(), "Entities to ignore")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Ray ray;
                 if (a.contains("x") && a.contains("y")) {
                     ray = engine.camera().toView().rayAt(a.get("x").asFloat(), a.get("y").asFloat(),
                                                          static_cast<int>(a.get("width").asInt(768)),
                                                          static_cast<int>(a.get("height").asInt(432)));
                 } else if (reflect::jsonToVec3(a.get("origin"), ray.origin)) {
                     ray.dir = {0, -1, 0};
                     (void)reflect::jsonToVec3(a.get("direction"), ray.dir);
                     if (length(ray.dir) < 1e-6f) {
                         return ToolResult::error(Error::make("invalid_arguments", "direction must be non-zero"));
                     }
                 } else {
                     return ToolResult::error(Error::make("invalid_arguments", "give origin (+direction) or pixel x,y"));
                 }
                 std::vector<EntityId> exclude;
                 for (const auto& e : a.get("exclude").elements()) {
                     auto id = resolve(engine, e);
                     if (!id) return ToolResult::error(id.error());
                     exclude.push_back(*id);
                 }
                 auto hit = engine.raycast(ray, exclude);
                 if (!hit) return ToolResult::json(Json::object({{"hit", false}}), "no hit");
                 Json j = hitJson(engine.scene(), *hit);
                 j["hit"] = true;
                 return ToolResult::json(j, "hit " + describe(engine.scene(), hit->entity));
             }});

    reg.add({"place_on_surface", "Drop onto surface",
             "Move entities straight down (or up) so the bottom of their bounds rests on the geometry below them. "
             "Great after placing props roughly on uneven terrain. One undo step.",
             "world",
             object({{"entities", array(entity(), "Entities to drop")}, {"offset", number("Extra height above the surface (default 0)")}},
                    {"entities"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::vector<std::string> notes;
                 size_t placed = 0;
                 Status st = engine.edit(ctx.actor, "Place on surface", [&]() -> Status {
                     for (const auto& e : a.get("entities").elements()) {
                         auto id = resolve(engine, e);
                         if (!id) return id.error();
                         if (Status s = dropToSurface(engine, *id, a.get("offset").asFloat()); !s) {
                             notes.push_back(s.error().message);
                             continue;
                         }
                         ++placed;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string msg = "placed " + std::to_string(placed) + " entities";
                 for (const auto& n : notes) msg += "\n  skipped: " + n;
                 return ToolResult::text(msg);
             }});

    reg.add({"scatter", "Scatter copies",
             "Procedurally place many copies of an entity or prefab in an area — forests, rocks, coins, crowds. "
             "Deterministic for a given seed. Random yaw/scale ranges, minimum spacing, and optional snapping to "
             "the surface below. All copies go under a new group entity; the whole operation is one undo step.",
             "world",
             object({{"source", entity("Entity to copy (with its children)")},
                     {"prefab", string("…or a prefab asset path")},
                     {"count", integer("How many copies (1-2000)")},
                     {"center", vec3("Area center (default origin)")},
                     {"size", Json::object({{"type", "array"}, {"items", Json::object({{"type", "number"}})},
                                            {"description", "Area [width, depth] in meters (default [20, 20])"}})},
                     {"radius", number("Use a circular area with this radius instead of size")},
                     {"min_distance", number("Minimum spacing between copies (default 0)")},
                     {"yaw", any("Yaw degrees: number or [min, max] (default [0, 360])")},
                     {"scale", any("Uniform scale multiplier: number or [min, max] (default 1)")},
                     {"on_surface", boolean("Drop copies onto the geometry below (default true)")},
                     {"surface", entity("Only land on this entity (e.g. the terrain)")},
                     {"seed", integer("Random seed (default 1)")},
                     {"group", string("Name of the group entity (default \"<source> scatter\")")}},
                    {"count"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 EntityId source = kNoEntity;
                 std::string prefab = a.get("prefab").asString();
                 if (a.contains("source")) {
                     auto id = resolve(engine, a.get("source"));
                     if (!id) return ToolResult::error(id.error());
                     source = *id;
                 } else if (prefab.empty()) {
                     return ToolResult::error(Error::make("invalid_arguments", "give source (entity) or prefab"));
                 } else if (auto p = engine.loadPrefabAsset(prefab); !p) {
                     return ToolResult::error(p.error());
                 }
                 int count = static_cast<int>(std::clamp<int64_t>(a.get("count").asInt(), 1, 2000));
                 Vec3 center;
                 (void)reflect::jsonToVec3(a.get("center"), center);
                 float w = 20, d = 20;
                 if (a.get("size").isArray() && a.get("size").size() >= 2) {
                     w = std::max(0.f, a.get("size")[0].asFloat());
                     d = std::max(0.f, a.get("size")[1].asFloat());
                 }
                 float radius = a.get("radius").asFloat(0);
                 float minDist = std::max(0.f, a.get("min_distance").asFloat(0));
                 float yawLo = 0, yawHi = 360, scaleLo = 1, scaleHi = 1;
                 rangeArg(a, "yaw", yawLo, yawHi);
                 rangeArg(a, "scale", scaleLo, scaleHi);
                 scaleLo = std::max(scaleLo, 0.001f);
                 scaleHi = std::max(scaleHi, scaleLo);
                 bool onSurface = a.get("on_surface").asBool(true);
                 std::vector<EntityId> surfaceKeep;
                 if (a.contains("surface")) {
                     auto id = resolve(engine, a.get("surface"));
                     if (!id) return ToolResult::error(id.error());
                     collectSubtree(s, *id, surfaceKeep);
                 }
                 Random rng(static_cast<uint64_t>(a.get("seed").asInt(1)));

                 // 1) Choose positions (rejection sampling for spacing), deterministic.
                 std::vector<Vec3> points;
                 int attempts = count * 40;
                 while (static_cast<int>(points.size()) < count && attempts-- > 0) {
                     Vec3 p;
                     if (radius > 0) {
                         float ang = rng.range(0, 2.f * kPi), r = radius * std::sqrt(rng.nextFloat());
                         p = center + Vec3{std::cos(ang) * r, 0, std::sin(ang) * r};
                     } else {
                         p = center + Vec3{rng.range(-w * 0.5f, w * 0.5f), 0, rng.range(-d * 0.5f, d * 0.5f)};
                     }
                     bool ok = true;
                     for (const auto& q : points) {
                         float dx = q.x - p.x, dz = q.z - p.z;
                         if (dx * dx + dz * dz < minDist * minDist) {
                             ok = false;
                             break;
                         }
                     }
                     if (ok) points.push_back(p);
                 }
                 std::vector<std::pair<float, float>> yawScale;
                 for (size_t i = 0; i < points.size(); ++i) yawScale.emplace_back(rng.range(yawLo, yawHi), rng.range(scaleLo, scaleHi));

                 // 2) Surface heights are sampled before creating copies so copies never land on each other.
                 std::vector<bool> grounded(points.size(), false);
                 if (onSurface) {
                     std::vector<EntityId> exclude = surfaceKeep.empty() ? std::vector<EntityId>{} : allExcept(s, surfaceKeep);
                     if (source) collectSubtree(s, source, exclude);
                     for (size_t i = 0; i < points.size(); ++i) {
                         auto hit = engine.raycast(Ray{points[i] + Vec3{0, 1000.f, 0}, {0, -1, 0}}, exclude);
                         if (hit) {
                             points[i].y = hit->point.y;
                             grounded[i] = true;
                         }
                     }
                 }

                 std::string baseName = source ? s.record(source)->name : prefab.substr(prefab.find_last_of('/') + 1);
                 if (auto dot = baseName.find('.'); !source && dot != std::string::npos) baseName = baseName.substr(0, dot);
                 std::string groupName = a.get("group").asString(baseName + " scatter");
                 EntityId group = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Scatter " + std::to_string(points.size()) + " " + baseName, [&]() -> Status {
                     group = s.create(groupName);
                     for (size_t i = 0; i < points.size(); ++i) {
                         EntityId copy = kNoEntity;
                         std::string name = baseName + " " + std::to_string(i + 1);
                         if (source) {
                             std::vector<EntityId> created;
                             duplicateTree(s, source, group, name, {}, true, created);
                             copy = created.front();
                         } else {
                             PrefabPlacement pl;
                             pl.parent = group;
                             pl.name = name;
                             auto r = engine.instantiatePrefabAsset(prefab, pl);
                             if (!r) return r.error();
                             copy = *r;
                         }
                         const Transform* t = s.get<Transform>(copy);
                         Vec3 rot = t ? t->rotation : Vec3{};
                         Vec3 scl = t ? t->scale : Vec3{1, 1, 1};
                         rot.y += yawScale[i].first;
                         scl = scl * yawScale[i].second;
                         Json patch = Json::object({{"position", reflect::vec3ToJson(points[i])},
                                                    {"rotation", reflect::vec3ToJson(rot)},
                                                    {"scale", reflect::vec3ToJson(scl)}});
                         if (Status ps = s.patchComponent(copy, "transform", patch); !ps) return ps;
                         if (grounded[i]) {
                             if (auto box = subtreeBounds(s, copy)) {
                                 if (Status ts = translateWorld(s, copy, {0, points[i].y - box->min.y, 0}); !ts) return ts;
                             }
                         }
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 std::string msg = "scattered " + std::to_string(points.size()) + " copies of " + baseName + " under #" +
                                   std::to_string(group) + " " + groupName;
                 if (static_cast<int>(points.size()) < count) {
                     msg += " (only " + std::to_string(points.size()) + " fit with min_distance " + std::to_string(minDist) + ")";
                 }
                 return ToolResult::json(Json::object({{"group", group}, {"count", points.size()}}), msg);
             }});

    reg.add({"sim_trace", "Trace simulation",
             "Run the game for N ticks and sample properties over time — e.g. transform.position of the player, "
             "vars.score, mesh.color — to verify behaviors numerically (did it jump? does the score increase?). "
             "Paths: <component>.<field>[.<sub>], vars.<name>, name, enabled. By default the scene is restored "
             "afterwards if it was in edit mode.",
             "sim",
             object({{"entities", array(entity(), "Entities to watch")},
                     {"properties", array(Json::object({{"type", "string"}}), "Property paths, e.g. [\"transform.position\", \"vars.score\"]")},
                     {"ticks", integer("Total ticks to simulate (default 120 = 2 s, max 7200)")},
                     {"every", integer("Sample every N ticks (default 10)")},
                     {"press", array(Json::object({{"type", "string"}}), "Keys pressed at the start")},
                     {"hold", array(Json::object({{"type", "string"}}), "Keys held during the trace")},
                     {"restore", boolean("Restore the edit-mode scene afterwards (default true)")}},
                    {"entities", "properties"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 std::vector<EntityId> ids;
                 for (const auto& e : a.get("entities").elements()) {
                     auto id = resolve(engine, e);
                     if (!id) return ToolResult::error(id.error());
                     ids.push_back(*id);
                 }
                 std::vector<std::string> props;
                 for (const auto& p : a.get("properties").elements()) props.push_back(p.asString());
                 int ticks = static_cast<int>(std::clamp<int64_t>(a.get("ticks").asInt(120), 1, 7200));
                 int every = static_cast<int>(std::clamp<int64_t>(a.get("every").asInt(10), 1, ticks));
                 bool wasEditing = engine.playState() == PlayState::Editing;
                 bool restore = a.get("restore").asBool(true);
                 auto& in = engine.input();
                 for (const auto& k : a.get("press").elements()) in.pressed.insert(str::lower(k.asString()));
                 for (const auto& k : a.get("hold").elements()) in.held.insert(str::lower(k.asString()));

                 Json samples = Json::array();
                 std::ostringstream os;
                 auto sample = [&](int tick) {
                     Json row = Json::object({{"tick", tick}, {"time", std::round(tick * Engine::kFixedDt * 1000.) / 1000.}});
                     os << "t=" << row.get("time").dump() << "s";
                     for (EntityId id : ids) {
                         if (!engine.scene().exists(id)) {
                             row[std::to_string(id)] = "destroyed";
                             os << "  #" << id << " destroyed";
                             continue;
                         }
                         Json vals = Json::object();
                         for (const auto& p : props) {
                             Json v = readProperty(engine.scene(), id, p);
                             vals[p] = v;
                             os << "  #" << id << "." << p << "=" << v.dump();
                         }
                         row[std::to_string(id)] = vals;
                     }
                     os << "\n";
                     samples.push(std::move(row));
                 };
                 sample(0);
                 size_t before = engine.recentMessages(100000).size();
                 for (int t = every; t <= ticks; t += every) {
                     engine.step(every);
                     sample(t);
                 }
                 for (const auto& k : a.get("hold").elements()) in.held.erase(str::lower(k.asString()));
                 auto all = engine.recentMessages(100000);
                 Json logs = Json::array();
                 for (size_t i = std::min(before, all.size()); i < all.size(); ++i) {
                     logs.push(all[i]);
                     os << "log: " << all[i].dump() << "\n";
                 }
                 if (wasEditing && restore) {
                     engine.stop();
                     os << "(scene restored to edit mode)\n";
                 }
                 ToolResult r = ToolResult::text(os.str());
                 r.structured = Json::object({{"samples", samples}, {"messages", logs}});
                 return r;
             }});

    reg.add({"viewport_multi", "Four-view capture",
             "One image with four views of the scene (or an entity): perspective (top-left), top (top-right), "
             "front (bottom-left) and side (bottom-right). Orthographic views make layout, alignment and spacing "
             "problems obvious — use it to check level layouts.",
             "view",
             object({{"focus", entity("Entity to frame (default: whole scene)")},
                     {"size", integer("Size of each view in pixels (default 384)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 std::optional<Aabb> box;
                 if (a.contains("focus")) {
                     auto id = resolve(engine, a.get("focus"));
                     if (!id) return ToolResult::error(id.error());
                     box = subtreeBounds(s, *id);
                 } else {
                     Aabb all{Vec3(1e30f), Vec3(-1e30f)};
                     bool any = false;
                     for (EntityId e : s.entities()) {
                         const MeshRenderer* m = s.get<MeshRenderer>(e);
                         if (!m || !m->visible) continue;
                         Aabb b = s.localBounds(e).transformed(s.worldMatrix(e));
                         // Ignore huge ground planes when framing: they'd make everything tiny.
                         if (length(b.extents()) > 200.f) continue;
                         all.min = vmin(all.min, b.min);
                         all.max = vmax(all.max, b.max);
                         any = true;
                     }
                     if (any) box = all;
                 }
                 if (!box) box = Aabb{Vec3(-5.f), Vec3(5.f)};
                 Vec3 c = box->center();
                 float radius = std::max(length(box->extents()), 0.5f);
                 int size = static_cast<int>(std::clamp<int64_t>(a.get("size").asInt(384), 96, 1024));

                 ViewCamera persp = engine.camera().toView();
                 persp.target = c;
                 persp.eye = c + normalize(Vec3{1.f, 0.8f, 1.3f}) * (radius / std::sin(radians(persp.fovDeg * 0.5f)));
                 ViewCamera views[4] = {persp, orthoView(c, radius, {0, 1, 0}, {0, 0, -1}),
                                        orthoView(c, radius, {0, 0, 1}, {0, 1, 0}), orthoView(c, radius, {1, 0, 0}, {0, 1, 0})};
                 Image out(size * 2 + 2, size * 2 + 2);
                 for (auto& px : out.pixels) px = 40;  // gutter color
                 for (int v = 0; v < 4; ++v) {
                     CaptureOptions o;
                     o.width = o.height = size;
                     o.hasCustomView = true;
                     o.customView = views[v];
                     o.editorOverlays = v == 0;
                     o.annotate = false;
                     o.fog = v == 0;
                     auto cap = engine.capture(o);
                     if (!cap) return ToolResult::error(cap.error());
                     int ox = (v % 2) * (size + 2), oy = (v / 2) * (size + 2);
                     for (int y = 0; y < size; ++y) {
                         std::copy_n(cap->image.at(0, y), static_cast<size_t>(size) * 4, out.at(ox, oy + y));
                     }
                 }
                 char buf[256];
                 std::snprintf(buf, sizeof(buf),
                               "2x2 views (each %dpx): perspective | top (+X right, -Z up)\n"
                               "front (+X right, +Y up) | side (-Z right, +Y up). Bounds center [%.2f, %.2f, %.2f], "
                               "extent %.2f m.",
                               size, c.x, c.y, c.z, radius * 2.f);
                 ToolResult r = ToolResult::text(buf);
                 std::vector<uint8_t> png = encodePng(out);
                 r.image(str::base64Encode(png.data(), png.size()));
                 return r;
             }});

    reg.add({"perf_stats", "Performance stats",
             "Frame cost and scene complexity: GPU and CPU frame time, draw calls, lights, terrain nodes, foliage "
             "instances, entities, behaviors and assets. Pass frames (e.g. 30) to benchmark real-time rendering of the "
             "current view at width x height (temporal AA, no supersampling) and get average/min/max GPU ms. Check "
             "after big scatters, generators or look changes; 16.6 ms = 60 fps.",
             "render",
             object({{"frames", integer("Benchmark this many real-time frames first (default 0 = just report)")},
                     {"width", integer("Benchmark width (default 1920)")},
                     {"height", integer("Benchmark height (default 1080)")},
                     {"view", enumeration({"editor", "scene"}, "Camera for the benchmark (default editor)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Json bench = Json::object();
                 int frames = static_cast<int>(std::clamp<int64_t>(a.get("frames").asInt(0), 0, 600));
                 if (frames > 0) {
                     CaptureOptions o;
                     o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(1920), 64, 4096));
                     o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(1080), 64, 4096));
                     o.samples = 1;
                     o.useSceneCamera = a.get("view").asString() == "scene";
                     o.editorOverlays = false;
                     double sum = 0, lo = 1e9, hi = 0, cpu = 0;
                     for (int i = 0; i < frames + 3; ++i) {  // 3 warm-up frames
                         auto t0 = std::chrono::steady_clock::now();
                         auto cap = engine.capture(o);
                         if (!cap) return ToolResult::error(cap.error());
                         double ms = engine.renderer().stats().get("gpuMs").asFloat(0.f);
                         if (i < 3) continue;
                         sum += ms, lo = std::min(lo, ms), hi = std::max(hi, ms);
                         cpu += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                     }
                     bench = Json::object({{"frames", frames}, {"width", o.width}, {"height", o.height},
                                           {"gpuMsAvg", std::round(sum / frames * 100) / 100}, {"gpuMsMin", std::round(lo * 100) / 100},
                                           {"gpuMsMax", std::round(hi * 100) / 100},
                                           {"wallMsAvg", std::round(cpu / frames * 100) / 100}});
                 }
                 const auto& st = engine.stats();
                 Scene& s = engine.scene();
                 size_t meshes = 0, behaviors = 0, lights = 0;
                 for (EntityId e : s.entities()) {
                     meshes += s.get<MeshRenderer>(e) ? 1 : 0;
                     behaviors += s.get<Behavior>(e) ? 1 : 0;
                     lights += s.get<Light>(e) ? 1 : 0;
                 }
                 Json j = Json::object({{"cpuFrameMs", std::round(st.cpuMs * 1000) / 1000},
                                        {"drawCalls", st.draws},
                                        {"activeLights", st.lights},
                                        {"entities", s.size()},
                                        {"meshEntities", meshes},
                                        {"lightEntities", lights},
                                        {"behaviorEntities", behaviors},
                                        {"assets", engine.assets().size()},
                                        {"renderer", engine.renderer().info().backend},
                                        {"gpu", engine.renderer().stats()},
                                        {"world", Json::object({{"terrains", static_cast<int64_t>(engine.world().stats().terrains)},
                                                                 {"foliageChunks", static_cast<int64_t>(engine.world().stats().foliageChunks)},
                                                                 {"foliageInstances", static_cast<int64_t>(engine.world().stats().foliageInstances)}})}});
                 if (frames > 0) j["benchmark"] = bench;
                 std::ostringstream os;
                 os << "frame build " << j.get("cpuFrameMs").dump() << " ms, " << st.draws << " draws, " << st.lights
                    << " lights; " << s.size() << " entities (" << meshes << " meshes, " << behaviors
                    << " with behaviors); " << engine.assets().size() << " assets";
                 return ToolResult::json(j, os.str());
             }});
}

}  // namespace sky::tools
