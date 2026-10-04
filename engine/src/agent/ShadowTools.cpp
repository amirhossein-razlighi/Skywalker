// Point / spot light shadow tools: inspect the local shadow atlas (which lights have shadows,
// where, what was re-rendered, budgets) and configure light shadows in bulk.
// See render/ShadowAtlas.h and docs/RENDERING.md ("Point and spot light shadows").

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/ShadowAtlas.h"

namespace sky::tools {

namespace {

using namespace schema;

/// Applies the view arguments shared with perf_stats ("editor", "scene" or {eye, target, fov}).
Status viewFrom(Engine& engine, const Json& view, CaptureOptions& o) {
    if (view.isObject()) {
        Vec3 eye, target;
        if (!reflect::jsonToVec3(view.get("eye"), eye)) {
            return Error::make("invalid_argument", "view.eye must be [x, y, z]", "e.g. {\"view\": {\"eye\": [0, 5, 12], \"target\": [0, 1, 0]}}");
        }
        o.hasCustomView = true;
        o.customView = engine.camera().toView();
        if (!reflect::jsonToVec3(view.get("target"), target)) target = o.customView.target;
        o.customView.lookFrom(eye, target);
        if (view.contains("fov")) o.customView.fovDeg = std::clamp(view.get("fov").asFloat(), 5.f, 150.f);
    } else if (view.isString()) {
        const std::string v = view.asString();
        if (v != "editor" && v != "scene") {
            return Error::make("invalid_argument", "unknown view '" + v + "'", "use \"editor\", \"scene\" or {eye, target, fov}");
        }
        o.useSceneCamera = v == "scene";
    }
    return {};
}

}  // namespace

void addShadowTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"shadow_atlas_info", "Inspect point/spot light shadows",
             "Which point and spot lights cast shadows this frame and how: projection (spot view, cube = 6 views, "
             "dual_paraboloid = 2 views), atlas slot and resolution, whether it was re-rendered this frame or reused from "
             "the static cache, and why a light has no shadow (disabled, out_of_view, beyond_max_distance, "
             "over_light_budget, atlas_full, pending). Also the atlas size and memory, slots used per quadrant, the "
             "per-frame budgets (Environment.localShadowLights / localShadowUpdates) and warnings. Renders one real-time "
             "frame of `view` first (default: the editor camera). Use it when light leaks through walls, a shadow is "
             "missing or blocky, or to check the cost of many lamps; see the atlas itself with viewport_capture "
             "debug_view \"shadow_atlas\". With `entity`, also lists the meshes that block that light (casters, first 40) "
             "and the small fixtures right at the light that are ignored (lamp heads, bulbs). invalidate=true re-renders every "
             "cached shadow on the next frame. Example: "
             "{\"view\": \"scene\"} or {\"entity\": \"Street Lamp 3\"}.",
             "render",
             object({{"view", any("Camera of the frame to inspect: \"editor\" (default), \"scene\", or {eye: [x,y,z], target: "
                                  "[x,y,z], fov: degrees}")},
                     {"entity", entity("Only report this light entity")},
                     {"render", boolean("Render a frame of `view` first (default true; false = the last frame rendered)")},
                     {"invalidate", boolean("Force every cached shadow to re-render (after editing meshes outside the "
                                            "engine, or to measure the full cost)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 EntityId only = kNoEntity;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     only = *id;
                     if (!engine.scene().get<Light>(only)) {
                         return ToolResult::error(Error::make("not_a_light", describe(engine.scene(), only) + " has no light component",
                                                              "pass a light entity; list them with scene_query {\"component\": \"light\"}"));
                     }
                 }
                 if (a.get("invalidate").asBool(false)) engine.renderer().invalidateLocalShadows();
                 std::optional<FrameData> frame;
                 if (a.get("render").asBool(true)) {
                     CaptureOptions o;
                     o.width = 640;
                     o.height = 360;
                     o.samples = 1;  // a real-time frame: budgets and the static cache apply as in play
                     o.editorOverlays = false;
                     o.listVisible = false;
                     if (Status s = viewFrom(engine, a.get("view"), o); !s) return ToolResult::error(s.error());
                     auto cap = engine.capture(o);
                     if (!cap) return ToolResult::error(cap.error());
                     frame = std::move(cap->frame);
                 }
                 Json info = engine.renderer().localShadowInfo();
                 if (!info.isObject()) {
                     return ToolResult::error(Error::make("unsupported", "this renderer backend has no local shadow atlas",
                                                          "run with the Metal or null renderer"));
                 }
                 // Names for the entities, and the optional filter.
                 const Scene& s = engine.scene();
                 Json lights = Json::array();
                 for (const Json& l : info.get("lights").elements()) {
                     auto e = static_cast<EntityId>(l.get("entity").asInt());
                     if (only && e != only) continue;
                     Json copy = l;
                     if (const EntityRecord* rec = s.record(e)) copy["name"] = rec->name;
                     lights.push(std::move(copy));
                 }
                 info["lights"] = lights;
                 // One light: which meshes block it, and which fixtures next to it are ignored.
                 if (only && frame) {
                     for (const LightItem& l : frame->lights) {
                         if (l.id != lightId(only, 0)) continue;
                         Json casters = Json::array(), fixtures = Json::array();
                         for (const DrawItem& d : frame->draws) {
                             if (!shadows::sphereTouches(l.position, l.range, d.worldBounds) || !d.castShadows) continue;
                             const EntityRecord* rec = s.record(d.entity);
                             Json item = Json::object({{"id", static_cast<int64_t>(d.entity)}, {"name", rec ? rec->name : ""}});
                             if (shadows::castsLocalShadow(d, l.position, l.range)) {
                                 if (casters.size() < 40) casters.push(std::move(item));
                             } else if (d.surface.color.w >= 0.5f && d.surface.shading != Shading::Unlit) {
                                 fixtures.push(std::move(item));
                             }
                         }
                         info["casters"] = casters;
                         info["ignoredFixtures"] = fixtures;
                     }
                 }
                 std::ostringstream os;
                 os << info.get("shadowed").asInt() << " point/spot lights with shadows (" << info.get("cachedLights").asInt()
                    << " from the cache, " << info.get("facesRendered").asInt() << " views rendered this frame); "
                    << info.get("requested").asInt() << " lights cast shadows, " << info.get("candidates").asInt()
                    << " affect the view; atlas " << info.get("atlasSize").asInt() << " px";
                 if (info.get("overBudget").asInt() > 0) os << "; " << info.get("overBudget").asInt() << " over the light budget";
                 for (const Json& w : info.get("warnings").elements()) os << "\nwarning: " << w.asString();
                 return ToolResult::json(info, os.str());
             }});

    reg.add({"light_shadows", "Configure light shadows",
             "Turn shadows of point/spot lights on or off and tune them, for one light, a list, or every light "
             "(\"all\", optionally only one kind). Shadows stop light leaking through walls and floors; they cost an "
             "atlas slot (cube point light = 6 views, spot = 1) but are cached while nothing in range moves. Tuning: "
             "resolution (slot px hint: 256 small, 1024 hero light; 0 = automatic), mode (cube | dual_paraboloid for "
             "cheaper point lights), bias (m; raise against speckled self-shadowing, lower if shadows float), "
             "normal_bias (texels), max_distance (m from the camera beyond which the shadow fades out). One undo step. "
             "Example: {\"lights\": \"all\", \"kind\": \"point\", \"enabled\": true, \"max_distance\": 40}.",
             "render",
             object({{"lights", any("A light entity (id or name), a list of them, or \"all\"")},
                     {"kind", enumeration({"point", "spot"}, "With \"all\": only lights of this kind")},
                     {"enabled", boolean("Cast shadows (Light.castShadows)")},
                     {"resolution", integer("Atlas slot hint in px (0 = automatic, 128..2048)")},
                     {"mode", enumeration({"cube", "dual_paraboloid"}, "Point light projection")},
                     {"bias", number("Depth bias in meters (default 0.02)")},
                     {"normal_bias", number("Normal offset in shadow-map texels (default 1)")},
                     {"max_distance", number("Camera distance (m) where the shadow fades out (0 = no limit)")}},
                    {"lights"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 std::vector<EntityId> targets;
                 const Json& sel = a.get("lights");
                 const std::string kind = a.get("kind").asString();
                 if (sel.isString() && sel.asString() == "all") {
                     for (EntityId e : s.entities()) {
                         const Light* l = s.get<Light>(e);
                         if (l && l->kind != "directional" && (kind.empty() || l->kind == kind)) targets.push_back(e);
                     }
                 } else {
                     std::vector<Json> refs;
                     if (sel.isArray()) {
                         for (const Json& r : sel.elements()) refs.push_back(r);
                     } else {
                         refs.push_back(sel);
                     }
                     for (const Json& r : refs) {
                         auto id = resolve(engine, r);
                         if (!id) return ToolResult::error(id.error());
                         if (!s.get<Light>(*id)) {
                             return ToolResult::error(Error::make("not_a_light", describe(s, *id) + " has no light component",
                                                                  "add one with entity_update {\"entity\": ..., \"components\": "
                                                                  "{\"light\": {\"kind\": \"point\"}}} or pick a light entity"));
                         }
                         targets.push_back(*id);
                     }
                 }
                 if (targets.empty()) {
                     return ToolResult::error(Error::make("no_lights", "no point/spot lights matched",
                                                          "create one with entity_create and a light component"));
                 }
                 Json patch = Json::object();
                 if (a.contains("enabled")) patch["castShadows"] = a.get("enabled").asBool();
                 if (a.contains("resolution")) patch["shadowResolution"] = std::clamp<int64_t>(a.get("resolution").asInt(), 0, 4096);
                 if (a.contains("mode")) patch["shadowMode"] = a.get("mode").asString();
                 if (a.contains("bias")) patch["shadowBias"] = std::clamp(a.get("bias").asFloat(), 0.f, 1.f);
                 if (a.contains("normal_bias")) patch["shadowNormalBias"] = std::clamp(a.get("normal_bias").asFloat(), 0.f, 8.f);
                 if (a.contains("max_distance")) patch["shadowMaxDistance"] = std::max(0.f, a.get("max_distance").asFloat());
                 if (patch.members().empty()) {
                     return ToolResult::error(Error::make("invalid_argument", "nothing to change",
                                                          "pass enabled, resolution, mode, bias, normal_bias or max_distance"));
                 }
                 Status st = engine.edit(ctx.actor, "Light shadows (" + std::to_string(targets.size()) + ")", [&]() -> Status {
                     for (EntityId e : targets) {
                         if (Status r = s.patchComponent(e, "light", patch); !r) return r;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 Json changed = Json::array();
                 for (EntityId e : targets) changed.push(briefJson(s, e));
                 std::ostringstream os;
                 os << "updated " << targets.size() << " light" << (targets.size() == 1 ? "" : "s") << ": " << patch.dump();
                 return ToolResult::json(Json::object({{"changed", changed}, {"patch", patch}}), os.str());
             }});
}

}  // namespace sky::tools
