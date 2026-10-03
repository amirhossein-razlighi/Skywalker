// Navigation tools: bake/save the navmesh, find paths, and look at the navmesh from above.
// See docs/PHYSICS.md ("Navigation").

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/nav/NavSystem.h"
#include "skywalker/physics/DebugDraw.h"

namespace sky::tools {

namespace {

using namespace schema;

Json r3(Vec3 v) {
    auto r = [](float x) { return std::round(x * 100.f) / 100.f; };
    return Json::array({r(v.x), r(v.y), r(v.z)});
}

/// A point argument: [x, y, z] or an entity (its position; feet for characters).
Result<Vec3> pointArg(Engine& engine, const Json& v, const char* what) {
    Vec3 p;
    if (v.isArray() && reflect::jsonToVec3(v, p)) return p;
    if (v.isNumber() || v.isString()) {
        auto id = resolve(engine, v);
        if (!id) return id.error();
        Vec3 pos = engine.scene().worldMatrix(*id).translation();
        if (const CharacterController* c = engine.scene().get<CharacterController>(*id)) {
            pos = pos + c->offset - Vec3{0.f, c->height * 0.5f, 0.f};
        }
        return pos;
    }
    return Error::make("invalid_arguments", std::string(what) + " must be [x, y, z] or an entity");
}

EntityId navmeshHolder(const Scene& s) {
    for (EntityId e : s.entities()) {
        if (s.get<NavMeshSurface>(e)) return e;
    }
    return kNoEntity;
}

std::string defaultDataPath(Engine& engine) {
    std::string scene = engine.scenePath();
    if (!scene.empty()) {
        std::filesystem::path p(scene);
        std::string stem = p.filename().string();
        for (std::string ext : {".sky.json", ".json"}) {
            if (stem.size() > ext.size() && stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0) {
                stem = stem.substr(0, stem.size() - ext.size());
                break;
            }
        }
        return (p.parent_path() / (stem + ".navmesh")).string();
    }
    std::string name = engine.scene().name.empty() ? "scene" : engine.scene().name;
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return "navmesh/" + str::lower(name) + ".navmesh";
}

Json reportJson(const nav::BuildReport& r) {
    return Json::object({{"tiles", r.tiles},
                         {"tileGrid", Json::array({r.tileGrid[0], r.tileGrid[1]})},
                         {"polygons", r.polygons},
                         {"inputTriangles", r.inputTriangles},
                         {"walkableArea", std::round(r.walkableArea * 10.f) / 10.f},
                         {"bounds", Json::object({{"min", r3(r.bounds.min)}, {"max", r3(r.bounds.max)}})},
                         {"seconds", std::round(r.seconds * 1000.0) / 1000.0}});
}

}  // namespace

void addNavTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"nav_build", "Bake navmesh",
             "Bake (or rebuild) the navigation mesh from static colliders and static meshes, save it to the project "
             "and record the settings in the scene's `navmesh` component (one undoable edit; creates an entity named "
             "\"Navigation\" if needed). Re-run after changing the level layout. Settings default to the navmesh "
             "component's: agent_radius (walls eroded by it), agent_height, max_climb (steps), max_slope (degrees), "
             "cell_size (precision). Entities tagged \"nav_ignore\" are left out. Then use nav_path / nav_debug, or "
             "nav agents and navigate() in Wander.",
             "physics",
             object({{"agent_radius", number("Agent radius in m (default 0.4)")},
                     {"agent_height", number("Agent height in m (default 1.8)")},
                     {"max_climb", number("Highest step in m (default 0.4)")},
                     {"max_slope", number("Steepest walkable slope in degrees (default 45)")},
                     {"cell_size", number("Voxel size in m (default 0.2; smaller = more precise, slower)")},
                     {"cell_height", number("Voxel height in m (default 0.1)")},
                     {"tile_size", integer("Cells per tile (default 64)")},
                     {"geometry", enumeration({"both", "colliders", "meshes"}, "Input geometry (default both)")},
                     {"save", boolean("Write the navmesh file (default true)")},
                     {"path", string("Project-relative file (default: next to the scene, <scene>.navmesh)")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 EntityId holder = navmeshHolder(s);
                 NavMeshSurface surface = holder ? *s.get<NavMeshSurface>(holder) : NavMeshSurface{};
                 Json patch = Json::object();
                 const std::pair<const char*, const char*> keys[] = {
                     {"agent_radius", "agentRadius"}, {"agent_height", "agentHeight"}, {"max_climb", "maxClimb"},
                     {"max_slope", "maxSlope"},       {"cell_size", "cellSize"},       {"cell_height", "cellHeight"},
                     {"tile_size", "tileSize"},       {"geometry", "geometry"}};
                 for (const auto& [arg, field] : keys) {
                     if (a.contains(arg)) patch[field] = a.get(arg);
                 }
                 if (Status st = reflect::applyJson(&surface, NavMeshSurface::type(), patch); !st) return fail(st);
                 bool save = a.get("save").asBool(true);
                 std::string path = a.get("path").asString(surface.data.empty() ? defaultDataPath(engine) : surface.data);
                 if (save) patch["data"] = path;

                 // Bake first (a failed bake changes nothing), then record the settings in one edit.
                 nav::BuildSettings bs = nav::settingsFrom(&surface);
                 auto report = engine.navigation().build(bs, surface.geometry);
                 if (!report) return ToolResult::error(report.error());
                 Status apply = engine.edit(ctx.actor, "Bake navmesh", [&]() -> Status {
                     if (!holder) holder = s.create("Navigation");
                     return s.patchComponent(holder, "navmesh", patch);
                 });
                 if (!apply) return fail(apply);
                 Json result = Json::object({{"report", reportJson(*report)}, {"entity", holder}});
                 if (save) {
                     if (Status st = engine.navigation().mesh() ? engine.navigation().mesh()->save(engine.resolvePath(path))
                                                                : Status(Error::make("nav_error", "navmesh vanished"));
                         !st) {
                         return fail(st);
                     }
                     result["file"] = path;
                 }
                 std::ostringstream os;
                 os << "navmesh baked: " << report->polygons << " polygons in " << report->tiles << " tiles, "
                    << std::round(report->walkableArea) << " m^2 walkable, " << std::round(report->seconds * 1000.0) << " ms";
                 if (save) os << "; saved to " << path;
                 return ToolResult::json(result, os.str());
             }});

    reg.add({"nav_path", "Find path",
             "Walking path on the navmesh between two points or entities (bakes the navmesh first if needed). Returns "
             "the corner points, the walking length, and whether the goal is reachable (partial = it is not; the path "
             "ends at the closest reachable point). Use to check that a level is traversable or to plan patrols. "
             "Example: {\"from\": \"Player\", \"to\": [12, 0, -4]}.",
             "physics",
             object({{"from", schema::any("Start: [x, y, z] or an entity")}, {"to", schema::any("Goal: [x, y, z] or an entity")}},
                    {"from", "to"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto from = pointArg(engine, a.get("from"), "from");
                 if (!from) return ToolResult::error(from.error());
                 auto to = pointArg(engine, a.get("to"), "to");
                 if (!to) return ToolResult::error(to.error());
                 nav::NavMesh* mesh = engine.navigation().mesh();
                 if (!mesh) {
                     return ToolResult::error(Error::make("no_navmesh", "no navmesh: " + engine.navigation().lastError(),
                                                          "add floors (static colliders or meshes) and run nav_build"));
                 }
                 nav::PathResult p = mesh->findPath(*from, *to);
                 Json pts = Json::array();
                 for (const Vec3& v : p.points) pts.push(r3(v));
                 Json j = Json::object({{"found", p.found},
                                        {"reachable", p.found && !p.partial},
                                        {"partial", p.partial},
                                        {"length", std::round(p.length * 100.f) / 100.f},
                                        {"straightDistance", std::round(distance(*from, *to) * 100.f) / 100.f},
                                        {"points", pts}});
                 std::string text = !p.found      ? "no path (start or goal is off the navmesh)"
                                    : p.partial   ? "goal unreachable; closest approach path is " + j.get("length").dump() + " m"
                                                  : "path " + j.get("length").dump() + " m through " + std::to_string(p.points.size()) +
                                                      " points";
                 return ToolResult::json(j, text);
             }});

    reg.add({"nav_debug", "See navmesh",
             "Top-down map of the level with the navmesh drawn over it (teal = walkable), nav agents (yellow dots) with "
             "their current paths, and optionally a test path between two points (orange). The fastest way to spot "
             "unreachable rooms, doorways too narrow for agents, and gaps. Bakes the navmesh first if needed.",
             "physics",
             object({{"focus", schema::entity("Frame this entity instead of the whole navmesh")},
                     {"from", schema::any("Optional test path start: [x, y, z] or an entity")},
                     {"to", schema::any("Optional test path goal")},
                     {"size", integer("Image size in pixels (default 640)")},
                     {"include_image", boolean("Return the image (default true)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 nav::NavMesh* mesh = engine.navigation().mesh();
                 if (!mesh) {
                     return ToolResult::error(Error::make("no_navmesh", "no navmesh: " + engine.navigation().lastError(),
                                                          "add floors (static colliders or meshes) and run nav_build"));
                 }
                 Scene& s = engine.scene();
                 Aabb bounds = mesh->report().bounds;
                 auto polys = mesh->polygons();
                 if (bounds.min.x > bounds.max.x || (bounds.min.x == 0 && bounds.max.x == 0)) {
                     bounds = {Vec3(1e30f), Vec3(-1e30f)};
                     for (const auto& poly : polys) {
                         for (const Vec3& v : poly) {
                             bounds.min = vmin(bounds.min, v);
                             bounds.max = vmax(bounds.max, v);
                         }
                     }
                 }
                 if (a.contains("focus")) {
                     auto id = resolve(engine, a.get("focus"));
                     if (!id) return ToolResult::error(id.error());
                     if (auto b = subtreeBounds(s, *id)) bounds = *b;
                 }
                 int size = static_cast<int>(std::clamp<int64_t>(a.get("size").asInt(640), 128, 2048));
                 CaptureOptions o;
                 o.width = o.height = size;
                 o.hasCustomView = true;
                 o.customView = debugdraw::topDown(bounds, 1.f);
                 o.editorOverlays = false;
                 o.annotate = false;
                 o.fog = false;
                 auto cap = engine.capture(o);
                 if (!cap) return ToolResult::error(cap.error());
                 debugdraw::Canvas canvas(cap->image, cap->frame.camera);
                 for (const auto& poly : polys) canvas.polygon(poly, {40, 200, 190, 90});
                 for (const auto& poly : polys) {
                     for (size_t i = 0; i < poly.size(); ++i) canvas.line(poly[i], poly[(i + 1) % poly.size()], {30, 120, 115, 200});
                 }
                 Json agents = Json::array();
                 for (EntityId e : s.entities()) {
                     const NavAgent* na = s.get<NavAgent>(e);
                     if (!na) continue;
                     Vec3 p = s.worldMatrix(e).translation();
                     auto path = engine.navigation().agentPath(e);
                     for (size_t i = 1; i < path.size(); ++i) canvas.line(path[i - 1], path[i], {255, 220, 60, 255}, 2);
                     canvas.dot(p, 5.f, {255, 230, 40, 255});
                     agents.push(Json::object({{"id", e},
                                               {"name", s.record(e)->name},
                                               {"position", r3(p)},
                                               {"navigating", na->navigating},
                                               {"destination", r3(na->destination)}}));
                 }
                 Json result = Json::object({{"report", reportJson(mesh->report())}, {"agents", agents}});
                 if (a.contains("from") && a.contains("to")) {
                     auto from = pointArg(engine, a.get("from"), "from");
                     if (!from) return ToolResult::error(from.error());
                     auto to = pointArg(engine, a.get("to"), "to");
                     if (!to) return ToolResult::error(to.error());
                     nav::PathResult p = mesh->findPath(*from, *to);
                     for (size_t i = 1; i < p.points.size(); ++i) canvas.line(p.points[i - 1], p.points[i], {255, 120, 30, 255}, 3);
                     canvas.dot(*from, 6.f, {60, 255, 90, 255});
                     canvas.dot(*to, 6.f, {255, 60, 60, 255});
                     result["path"] = Json::object({{"reachable", p.found && !p.partial}, {"length", std::round(p.length * 100.f) / 100.f}});
                 }
                 std::ostringstream os;
                 os << "Top-down navmesh (+X right, -Z up): " << mesh->report().polygons << " polygons, " << agents.size()
                    << " agents. Teal = walkable; yellow = agents and their paths";
                 if (result.contains("path")) os << "; orange = test path (green start, red goal)";
                 os << ".";
                 ToolResult r = ToolResult::text(os.str());
                 r.structured = result;
                 if (a.get("include_image").asBool(true)) {
                     std::vector<uint8_t> png = encodePng(cap->image);
                     r.image(str::base64Encode(png.data(), png.size()));
                 }
                 return r;
             }});
}

}  // namespace sky::tools
