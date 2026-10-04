// Spatial & analysis tools: ray casts, surface placement, procedural scattering, simulation
// traces, multi-view captures and performance stats. These give agents "hands and eyes" for
// level building beyond single-entity edits.

#include <algorithm>
#include <optional>
#include <map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Profiler.h"
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

Json readPropertyPath(const Scene& s, EntityId id, const std::string& path) { return readProperty(s, id, path); }

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
             "afterwards if it was in edit mode. display_hz (e.g. 120) simulates a real display instead: real time "
             "advances 1/display_hz per frame (fixed 60 Hz ticks accumulate), `every` counts display frames, and each "
             "sample shows the interpolation alpha plus `shown` = what that frame displays (render interpolation) next to "
             "the tick values; the result's `smoothness` compares how evenly vector properties move per frame with and "
             "without interpolation (stepJitter: 0 = perfectly even, about 2 = every other frame repeats a tick) and `pacing` gives the frame-time jitter.",
             "sim",
             object({{"entities", array(entity(), "Entities to watch")},
                     {"properties", array(Json::object({{"type", "string"}}), "Property paths, e.g. [\"transform.position\", \"vars.score\"]")},
                     {"ticks", integer("Total ticks to simulate (default 120 = 2 s, max 7200)")},
                     {"every", integer("Sample every N ticks (default 10)")},
                     {"press", array(Json::object({{"type", "string"}}), "Keys pressed at the start")},
                     {"hold", array(Json::object({{"type", "string"}}), "Keys held during the trace")},
                     {"restore", boolean("Restore the edit-mode scene afterwards (default true)")},
                     {"display_hz", number("Simulate a display at this refresh rate (30..240, e.g. 120 for ProMotion): "
                                           "samples per displayed frame with interpolation alpha and shown values")},
                     {"interpolation", boolean("With display_hz: render interpolation on (default: the engine setting)")}},
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
                 size_t before = engine.recentMessages(100000).size();
                 Json displayInfo;
                 if (a.contains("display_hz")) {
                     // A simulated display: real time advances per frame, ticks accumulate (Engine::advance), and each
                     // sample reads what that frame shows (the in-between transforms) next to the tick state.
                     const double hz = std::clamp(a.get("display_hz").asFloat(120.f), 30.f, 240.f);
                     const bool wasInterp = engine.interpolation();
                     if (a.contains("interpolation")) engine.setInterpolation(a.get("interpolation").asBool(true));
                     if (engine.playState() != PlayState::Playing) engine.play();
                     const int frames = static_cast<int>(std::ceil(ticks * hz / 60.0));
                     const int everyFrame = static_cast<int>(std::clamp<int64_t>(a.get("every").asInt(1), 1, frames));
                     // Per entity/property: per-frame displacement of vector values, shown and raw (tick state).
                     struct Track {
                         std::vector<double> shownSteps, rawSteps;
                         std::optional<Vec3> lastShown, lastRaw;
                     };
                     std::map<std::string, Track> tracks;
                     // How uneven per-frame motion is: RMS of each frame's step minus the mean of its two
                     // neighbours, relative to the mean step. Real acceleration and turns barely count (they
                     // change steps smoothly); a frame that repeats the last tick and one that jumps two do.
                     auto stepJitter = [](const std::vector<double>& v) {
                         if (v.size() < 3) return 0.0;
                         double mean = 0, sq = 0;
                         for (double x : v) mean += x;
                         mean /= static_cast<double>(v.size());
                         if (mean < 1e-9) return 0.0;
                         for (size_t i = 1; i + 1 < v.size(); ++i) {
                             const double d = v[i] - 0.5 * (v[i - 1] + v[i + 1]);
                             sq += d * d;
                         }
                         return std::sqrt(sq / static_cast<double>(v.size() - 2)) / mean;
                     };
                     for (int f = 1; f <= frames; ++f) {
                         const int ran = engine.advance(1.0 / hz);
                         const float alpha = engine.interpolationAlpha();
                         engine.notePresentedFrame(engine.interpolation() ? alpha : 1.f);
                         const bool record = f % everyFrame == 0;
                         const bool warm = engine.transformHistory().valid();
                         Json row = Json::object({{"frame", f}, {"time", std::round(f / hz * 1000.) / 1000.},
                                                  {"alpha", std::round(alpha * 1000.) / 1000.}, {"ticks", ran}});
                         // Tick state first, then the displayed state (the scope restores the tick state).
                         std::map<std::string, Json> raw;
                         for (EntityId id : ids) {
                             if (!engine.scene().exists(id)) continue;
                             for (const auto& p : props) raw[std::to_string(id) + "." + p] = readProperty(engine.scene(), id, p);
                         }
                         {
                             ScopedInterpolation shown(engine.scene(), engine.transformHistory(),
                                                       engine.interpolation() ? alpha : 1.f, &engine.runtime().processGate());
                             for (EntityId id : ids) {
                                 if (!engine.scene().exists(id)) continue;
                                 Json vals = Json::object();
                                 for (const auto& p : props) {
                                     const std::string key = std::to_string(id) + "." + p;
                                     Json v = readProperty(engine.scene(), id, p);
                                     Track& tr = tracks[key];
                                     Vec3 sv, rv;
                                     // Steps count once a tick history exists (the first frames have nothing to blend).
                                     if (warm && reflect::jsonToVec3(v, sv) && reflect::jsonToVec3(raw[key], rv)) {
                                         if (tr.lastShown) tr.shownSteps.push_back(length(sv - *tr.lastShown));
                                         if (tr.lastRaw) tr.rawSteps.push_back(length(rv - *tr.lastRaw));
                                         tr.lastShown = sv;
                                         tr.lastRaw = rv;
                                     }
                                     if (record) vals[p] = Json::object({{"shown", v}, {"tick", raw[key]}});
                                 }
                                 if (record) row[std::to_string(id)] = vals;
                             }
                         }
                         if (record && samples.size() < 2000) samples.push(std::move(row));
                     }
                     Json smooth = Json::object();
                     for (const auto& [key, tr] : tracks) {
                         if (tr.shownSteps.empty()) continue;
                         smooth[key] = Json::object({{"stepJitter", std::round(stepJitter(tr.shownSteps) * 1000.) / 1000.},
                                                     {"stepJitterWithoutInterpolation", std::round(stepJitter(tr.rawSteps) * 1000.) / 1000.}});
                         os << key << ": step jitter " << stepJitter(tr.shownSteps) << " (without interpolation " << stepJitter(tr.rawSteps) << ")\n";
                     }
                     displayInfo = Json::object({{"hz", hz}, {"frames", frames}, {"interpolation", engine.interpolation()},
                                                 {"pacing", engine.pacing().toJson()}, {"smoothness", smooth}});
                     os << "display " << hz << " Hz, " << frames << " frames: jitter " << engine.pacing().jitterMs << " ms (without interpolation "
                        << engine.pacing().jitterMsRaw << " ms)\n";
                     engine.setInterpolation(wasInterp);
                 } else {
                     sample(0);
                     for (int t = every; t <= ticks; t += every) {
                         engine.step(every);
                         sample(t);
                     }
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
                 if (!displayInfo.isNull()) r.structured["display"] = displayInfo;
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
             "after big scatters, generators or look changes; 16.6 ms = 60 fps. view is \"editor\", \"scene\" or a "
             "custom camera {eye, target, fov}; quality benchmarks an editor viewport tier. The gpu section includes "
             "foliage impostor stats (impostorInstances, meshInstances, impostorsBaked, impostorBakeMs). passes=true adds "
             "where the frame time goes: profile.passes = GPU time per render pass (Main, Shadow cascades, SSGI, SSR, Lighting "
             "resolve, TAA, Bloom, Composite, Foliage cull, GPU particles, Hair ...; rolling 60-frame avg/min/max), "
             "profile.groups = the same summed by area (shadows, main, ao, ssgi, ssr, resolve, effects, volumetrics, clouds, "
             "temporal, post, foliage, particles, hair, ui ...) and profile.cpu = CPU scopes (frame.build, scene.buildFrame, "
             "world.gather, render.encode ...). With frames > 0 the profile covers exactly the benchmark frames. Example: "
             "{\"frames\":30,\"passes\":true,\"view\":{\"eye\":[0,300,-600],\"target\":[0,80,0]}}.",
             "render",
             object({{"frames", integer("Benchmark this many real-time frames first (default 0 = just report)")},
                     {"width", integer("Benchmark width (default 1920)")},
                     {"height", integer("Benchmark height (default 1080)")},
                     {"view", any("Camera for the benchmark: \"editor\" (default), \"scene\", or {eye: [x,y,z], target: "
                                  "[x,y,z], fov: degrees}")},
                     {"quality", enumeration({"full", "balanced", "fast"},
                                             "Viewport quality tier to benchmark (default full, as in play mode and captures)")},
                     {"passes", boolean("Add the per-pass GPU timeline and CPU scopes (profile: passes, groups, cpu)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Json bench = Json::object();
                 int frames = static_cast<int>(std::clamp<int64_t>(a.get("frames").asInt(0), 0, 600));
                 if (frames > 0) {
                     CaptureOptions o;
                     o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(1920), 64, 4096));
                     o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(1080), 64, 4096));
                     o.samples = 1;
                     const Json& view = a.get("view");
                     if (view.isObject()) {
                         Vec3 eye, target;
                         if (!reflect::jsonToVec3(view.get("eye"), eye)) {
                             return ToolResult::error(Error::make("invalid_argument", "view.eye must be [x, y, z]",
                                                                  "e.g. {\"view\": {\"eye\": [0, 50, -120], \"target\": [0, 20, 0]}}"));
                         }
                         o.hasCustomView = true;
                         o.customView = engine.camera().toView();
                         o.customView.eye = eye;
                         if (reflect::jsonToVec3(view.get("target"), target)) o.customView.target = target;
                         if (view.contains("fov")) o.customView.fovDeg = std::clamp(view.get("fov").asFloat(), 5.f, 150.f);
                     } else {
                         o.useSceneCamera = view.asString() == "scene";
                     }
                     const std::string q = a.get("quality").asString("full");
                     o.quality = q == "fast" ? 2 : q == "balanced" ? 1 : 0;
                     o.editorOverlays = false;
                     if (a.get("passes").asBool(false)) {  // the profile covers exactly the benchmark frames
                         engine.renderer().resetPassProfile();
                         prof::CpuProfiler::instance().reset();
                     }
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
                 j["vehicles"] = [&] {  // wheeled vehicles (physics/Vehicles.cpp): counts and per-tick CPU cost
                     physics::PhysicsWorld* pw = engine.physics().playWorld();
                     physics::VehicleStats vs = pw ? pw->vehicleStats() : physics::VehicleStats{};
                     if (!pw) vs.vehicles = static_cast<int>(s.registry().count<Vehicle>());
                     return Json::object({{"vehicles", vs.vehicles}, {"wheels", vs.wheels}, {"simulated", pw != nullptr},
                                          {"stepMs", std::round(vs.lastStepMs * 1000.0) / 1000.0}});
                 }();
                 j["frameFlow"] = Json::object({{"interpolation", engine.interpolation()},
                                                {"alpha", std::round(engine.interpolationAlpha() * 1000.f) / 1000.f},
                                                {"interpolatedLastFrame", engine.frameFlowStats().interpolated},
                                                {"frameHandlerRunsLastFrame", engine.frameFlowStats().frameHandlerRuns},
                                                {"gamePaused", engine.gamePaused()},
                                                {"timeScale", engine.timeScale()},
                                                {"pacing", engine.pacing().toJson()}});
                 Json warnings = Json::array();
                 if (a.get("passes").asBool(false)) {
                     Json profile = engine.renderer().passProfile();
                     profile["cpu"] = prof::CpuProfiler::instance().toJson();
                     if (!profile.get("supported").asBool(false)) {
                         warnings.push("per-pass GPU timing is unavailable on this renderer/GPU (" +
                                       profile.get("mode").asString("unsupported") + "); CPU scopes still apply");
                     } else if (profile.get("frames").asInt(0) == 0) {
                         warnings.push("no profiled frames yet: pass frames (e.g. 30) to benchmark, or let the editor viewport run");
                     }
                     if (profile.get("droppedPasses").asInt(0) > 0) {
                         warnings.push("some passes were not timed (timestamp buffer full); totals are a lower bound");
                     }
                     j["profile"] = profile;
                 }
                 if (warnings.size() > 0) j["warnings"] = warnings;
                 std::ostringstream os;
                 os << "frame build " << j.get("cpuFrameMs").dump() << " ms, " << st.draws << " draws, " << st.lights
                    << " lights; " << s.size() << " entities (" << meshes << " meshes, " << behaviors
                    << " with behaviors); " << engine.assets().size() << " assets";
                 return ToolResult::json(j, os.str());
             }});
}

}  // namespace sky::tools
