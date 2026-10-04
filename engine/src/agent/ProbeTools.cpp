// Reflection probe tools: add a probe sized to its room, bake (re-capture) probes, inspect the probe
// atlas, and render a probe's cubemap (viewport_capture {probe}). See render/ReflectionProbes.h and
// docs/RENDERING.md ("Reflection probes").

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/render/ReflectionProbes.h"
#include "skywalker/render/RenderLayers.h"

namespace sky::tools {

namespace {

using namespace schema;

/// The view arguments shared with perf_stats and shadow_atlas_info ("editor", "scene" or {eye, target, fov}).
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

/// Renders one frame of `view`: a still (samples >= 2) captures every probe it needs at once.
Result<Capture> renderFrame(Engine& engine, const Json& view, bool still) {
    CaptureOptions o;
    o.width = 320;
    o.height = 180;
    o.samples = still ? 2 : 1;
    o.editorOverlays = false;
    o.listVisible = false;
    if (Status s = viewFrom(engine, view, o); !s) return s.error();
    return engine.capture(o);
}

Result<EntityId> resolveProbe(Engine& engine, const Json& ref) {
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    if (!engine.scene().get<ReflectionProbe>(*id)) {
        return Error::make("not_a_probe", describe(engine.scene(), *id) + " has no reflection_probe component",
                           "list probes with scene_query {\"component\": \"reflection_probe\"} or add one with probe_add");
    }
    return *id;
}

/// The probes named by `ref`: one entity, a list, or "all" / absent.
Result<std::vector<EntityId>> resolveProbes(Engine& engine, const Json& ref) {
    std::vector<EntityId> out;
    const Scene& s = engine.scene();
    if (ref.isNull() || (ref.isString() && ref.asString() == "all")) {
        for (EntityId e : s.entities()) {
            if (s.get<ReflectionProbe>(e)) out.push_back(e);
        }
        return out;
    }
    std::vector<Json> refs;
    if (ref.isArray()) {
        for (const Json& r : ref.elements()) refs.push_back(r);
    } else {
        refs.push_back(ref);
    }
    for (const Json& r : refs) {
        auto id = resolveProbe(engine, r);
        if (!id) return id.error();
        out.push_back(*id);
    }
    return out;
}

/// Adds entity names to the renderer's probe report (and to the warnings, which name probes by id).
Json namedInfo(const Scene& s, Json info, EntityId only) {
    Json probes = Json::array();
    for (const Json& p : info.get("probes").elements()) {
        const auto e = static_cast<EntityId>(p.get("entity").asInt());
        if (only && e != only) continue;
        Json copy = p;
        if (const EntityRecord* rec = s.record(e)) copy["name"] = rec->name;
        probes.push(std::move(copy));
    }
    info["probes"] = probes;
    Json warnings = Json::array();
    for (const Json& w : info.get("warnings").elements()) {
        std::string text = w.asString();
        // "probe #12" -> "probe 'Hall' (#12)"
        for (size_t pos = text.find("#"); pos != std::string::npos; pos = text.find("#", pos + 1)) {
            size_t end = pos + 1;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end == pos + 1) continue;
            const auto id = static_cast<EntityId>(std::stoull(text.substr(pos + 1, end - pos - 1)));
            if (const EntityRecord* rec = s.record(id)) {
                const std::string named = "'" + rec->name + "' (#" + std::to_string(id) + ")";
                text.replace(pos, end - pos, named);
                pos += named.size() - 1;
            }
        }
        warnings.push(text);
    }
    info["warnings"] = warnings;
    return info;
}

std::string summary(const Json& info) {
    std::ostringstream os;
    const Json& atlas = info.get("atlas");
    os << info.get("count").asInt() << " reflection probes: " << info.get("ready").asInt() << " captured, "
       << info.get("shaded").asInt() << " lighting this view; atlas " << atlas.get("slots").asInt() << " x "
       << atlas.get("resolution").asInt() << " px cubes (" << atlas.get("memoryMB").asNumber() << " MB, budget "
       << info.get("budget").get("probes").asInt() << "); " << info.get("facesCaptured").asInt() << " faces captured this frame";
    if (info.get("facesDeferred").asInt() > 0) os << ", " << info.get("facesDeferred").asInt() << " waiting";
    for (const Json& p : info.get("probes").elements()) {
        os << "\n- " << p.get("name").asString() << " (#" << p.get("entity").asInt() << "): " << (p.get("ready").asBool() ? "ready" : "not captured");
        if (p.contains("slot")) os << ", slot " << p.get("slot").asInt() << " " << p.get("debugColor").asString();
        if (p.contains("reason")) os << ", " << p.get("reason").asString();
        if (p.get("stale").asBool(false)) os << ", stale";
        if (p.contains("lastCaptureGpuMs")) os << ", capture " << p.get("lastCaptureGpuMs").asNumber() << " ms GPU";
    }
    for (const Json& w : info.get("warnings").elements()) os << "\nwarning: " << w.asString();
    return os.str();
}

/// Distance from `origin` along `dir` to the nearest visible mesh (bounds, any rotation), or -1.
float castToMeshes(const Scene& s, Vec3 origin, Vec3 dir, float maxDist) {
    float best = -1.f;
    for (EntityId e : s.entities()) {
        if (!s.isActive(e)) continue;
        const MeshRenderer* m = s.get<MeshRenderer>(e);
        if (!m || !m->visible) continue;
        const Mat4 inv = s.worldMatrix(e).inverse();
        Ray r{inv.transformPoint(origin), inv.transformDir(dir)};  // the parameter t is the same in both spaces
        const float t = intersect(r, s.localBounds(e));
        if (t > 0.02f && t < maxDist && (best < 0.f || t < best)) best = t;
    }
    return best;
}

}  // namespace

Result<Image> probeCubemapImage(Engine& engine, const Json& probe, int mip) {
    auto id = resolveProbe(engine, probe);
    if (!id) return id.error();
    return engine.renderer().reflectionProbeImage(*id, std::clamp(mip, 0, probes::kMips - 1));
}

void addProbeTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"probe_add", "Add a reflection probe",
             "Add a reflection probe: a captured cubemap that glossy floors, metal and glass inside its volume reflect "
             "instead of the sky (box-projected, so reflections line up with the walls), and that gives them their ambient "
             "light. Put one per room, corridor or street section, with the capture point in open space at about eye "
             "height. size \"auto\" (default) casts rays from `position` to the nearest meshes in the six axis "
             "directions and fits the box to the room (open directions get 8 m): the volume reaches blend_distance past "
             "the walls and the reflections project onto the room (projectionSize); or give [x, y, z] in meters. "
             "interior=true for closed rooms: no sky light leaks in (the probe replaces it; windows still show the sky). "
             "update: once (default, cached until it moves or probe_bake), on_change (re-captures when something in "
             "range changes), realtime (every `interval` frames, within Environment.probeUpdates faces per frame). The "
             "new probe is captured right away and its state returned (see probe_info). Example: {\"name\": \"Hall "
             "Probe\", \"position\": [0, 1.6, 0], \"interior\": true}.",
             "render",
             object({{"name", string("Entity name (default \"Reflection Probe\")")},
                     {"position", vec3("Capture point in meters (the room's open space, ~1.5 m up)")},
                     {"parent", schema::entity("Parent entity")},
                     {"size", any("Box size [x, y, z] in meters, or \"auto\" (default): fit the room around `position`")},
                     {"shape", enumeration({"box", "sphere"}, "Influence volume (default box)")},
                     {"radius", number("Sphere radius in meters")},
                     {"interior", boolean("Closed room: no sky light inside (default false)")},
                     {"update", enumeration({"once", "on_change", "realtime"}, "When to capture (default once)")},
                     {"interval", integer("realtime: frames between captures (default 1)")},
                     {"resolution", integer("Face size in px: 64, 128, 256 (default), 512")},
                     {"priority", integer("Higher wins where volumes overlap (default 0)")},
                     {"blend_distance", number("Meters over which the probe fades at its edges (default 1)")},
                     {"box_projection", boolean("Parallax-correct reflections against the box (default true)")},
                     {"intensity", number("Brightness multiplier (default 1)")},
                     {"ambient", enumeration({"probe", "sky", "color"}, "Diffuse ambient inside the volume (default probe)")},
                     {"ambient_color", string("Interior / color ambient \"#rrggbb\"")},
                     {"cull_mask", any("Render layers the capture draws: names, numbers, \"all\" (default) or a list")},
                     {"render", boolean("Capture it now and return its state (default true)")}},
                    {"position"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Vec3 pos;
                 if (!reflect::jsonToVec3(a.get("position"), pos)) {
                     return ToolResult::error(Error::make("invalid_argument", "position must be [x, y, z]", "e.g. {\"position\": [0, 1.6, 0]}"));
                 }
                 Json probe = Json::object();
                 Vec3 center = pos;
                 Json open = Json::array();
                 const Json& size = a.get("size");
                 const bool sphere = a.get("shape").asString() == "sphere";
                 if (sphere) {
                     probe["shape"] = "sphere";
                     if (a.contains("radius")) probe["radius"] = std::max(0.1f, a.get("radius").asFloat());
                 } else if (size.isArray()) {
                     Vec3 sz;
                     if (!reflect::jsonToVec3(size, sz) || sz.x <= 0.f || sz.y <= 0.f || sz.z <= 0.f) {
                         return ToolResult::error(Error::make("invalid_argument", "size must be [x, y, z] with positive meters or \"auto\"",
                                                              "e.g. {\"size\": [8, 3, 6]} or {\"size\": \"auto\"}"));
                     }
                     probe["size"] = reflect::vec3ToJson(sz);
                 } else if (size.isNull() || (size.isString() && size.asString() == "auto")) {
                     // Fit the room: the nearest mesh in each axis direction (open sides get 16 m).
                     const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                     const char* names[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};
                     float d[6];
                     for (int i = 0; i < 6; ++i) {
                         d[i] = castToMeshes(engine.scene(), pos, dirs[i], 64.f);
                         if (d[i] < 0.f) {
                             d[i] = 8.f;
                             open.push(names[i]);
                         }
                     }
                     const Vec3 room{d[0] + d[1], d[2] + d[3], d[4] + d[5]};
                     center = pos + Vec3{(d[0] - d[1]) * 0.5f, (d[2] - d[3]) * 0.5f, (d[4] - d[5]) * 0.5f};
                     // The volume reaches blendDistance past the walls, floor and ceiling so they get the probe at full
                     // strength; reflections project onto the room itself, so they stay aligned with the walls.
                     const float blend = a.contains("blend_distance") ? std::max(0.f, a.get("blend_distance").asFloat()) : 1.f;
                     auto r2 = [](float v) { return std::round(v * 100.f) / 100.f; };
                     const Vec3 sz = room + Vec3(2.f * blend);
                     probe["size"] = reflect::vec3ToJson({r2(sz.x), r2(sz.y), r2(sz.z)});
                     probe["projectionSize"] = reflect::vec3ToJson({r2(room.x), r2(room.y), r2(room.z)});
                     probe["captureOffset"] = reflect::vec3ToJson(pos - center);
                 } else {
                     return ToolResult::error(Error::make("invalid_argument", "size must be [x, y, z] or \"auto\"",
                                                          "e.g. {\"size\": [8, 3, 6]}"));
                 }
                 if (a.contains("interior")) probe["interior"] = a.get("interior").asBool();
                 if (a.contains("update")) probe["update"] = a.get("update").asString();
                 if (a.contains("interval")) probe["interval"] = std::clamp<int64_t>(a.get("interval").asInt(1), 1, 600);
                 if (a.contains("resolution")) probe["resolution"] = probes::sanitizeResolution(static_cast<int>(a.get("resolution").asInt(256)));
                 if (a.contains("priority")) probe["priority"] = a.get("priority").asInt();
                 if (a.contains("blend_distance")) probe["blendDistance"] = std::max(0.f, a.get("blend_distance").asFloat());
                 if (a.contains("box_projection")) probe["boxProjection"] = a.get("box_projection").asBool();
                 if (a.contains("intensity")) probe["intensity"] = std::max(0.f, a.get("intensity").asFloat());
                 if (a.contains("ambient")) probe["ambient"] = a.get("ambient").asString();
                 if (a.contains("ambient_color")) probe["ambientColor"] = a.get("ambient_color");
                 if (a.contains("cull_mask")) {
                     render::LayerNames names;
                     if (auto settings = game::GameSettings::load(engine.config().projectDir)) names = settings->renderLayers;
                     auto mask = render::parseLayerMask(a.get("cull_mask"), names);
                     if (!mask) return ToolResult::error(mask.error());
                     probe["cullMask"] = static_cast<int64_t>(*mask);
                 }
                 const std::string name = a.get("name").asString().empty() ? "Reflection Probe" : a.get("name").asString();
                 EntityId created = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Add reflection probe " + name, [&]() -> Status {
                     Scene& s = engine.scene();
                     EntityId parent = kNoEntity;
                     if (a.contains("parent")) {
                         auto p = resolve(engine, a.get("parent"));
                         if (!p) return p.error();
                         parent = *p;
                     }
                     created = s.create(name, parent);
                     Json doc = Json::object({{"components", Json::object({{"transform", Json::object({{"position", reflect::vec3ToJson(center)}})},
                                                                           {"reflection_probe", probe}})}});
                     return s.applyEntityJson(created, doc);
                 });
                 if (!st) return fail(st);
                 Json result = Json::object({{"entity", briefJson(engine.scene(), created)}, {"probe", probe}});
                 if (open.size() > 0) result["openSides"] = open;
                 std::ostringstream os;
                 os << "added " << describe(engine.scene(), created);
                 if (probe.contains("size")) os << " size " << probe.get("size").dump();
                 if (open.size() > 0) os << " (nothing hit toward " << open.dump() << ": 8 m used)";
                 if (a.get("render").asBool(true)) {
                     auto cap = renderFrame(engine, "editor", true);
                     if (!cap) return ToolResult::error(cap.error());
                     Json info = namedInfo(engine.scene(), engine.renderer().reflectionProbeInfo(&cap->frame), created);
                     if (info.isObject()) {
                         if (info.get("probes").size() > 0) result["state"] = info.get("probes").elements().front();
                         result["warnings"] = info.get("warnings");
                         for (const Json& w : info.get("warnings").elements()) os << "\nwarning: " << w.asString();
                     }
                 }
                 return ToolResult::json(result, os.str());
             }});

    reg.add({"probe_bake", "Re-capture reflection probes",
             "Re-capture reflection probes now (after moving furniture, changing lights or the sky): renders one still "
             "frame of `view`, which captures every invalidated probe in full regardless of the per-frame face budget, "
             "and returns their state (slot, captures, GPU time) plus warnings. probes: one entity, a list or \"all\" "
             "(default). render=false only marks them; they re-capture over the next frames (Wander: probe_bake()). "
             "Example: {\"probes\": [\"Hall Probe\"]}.",
             "render",
             object({{"probes", any("A probe entity, a list of them, or \"all\" (default)")},
                     {"view", any("Camera of the frame: \"editor\" (default), \"scene\", or {eye, target, fov}")},
                     {"render", boolean("Render the frame that captures them now (default true)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto ids = resolveProbes(engine, a.get("probes"));
                 if (!ids) return ToolResult::error(ids.error());
                 if (ids->empty()) {
                     return ToolResult::error(Error::make("no_probes", "the scene has no reflection probes",
                                                          "add one with probe_add {\"position\": [x, y, z]}"));
                 }
                 const bool all = a.get("probes").isNull() || (a.get("probes").isString() && a.get("probes").asString() == "all");
                 if (all) {
                     engine.renderer().invalidateReflectionProbes(0);
                 } else {
                     for (EntityId e : *ids) engine.renderer().invalidateReflectionProbes(e);
                 }
                 if (!a.get("render").asBool(true)) {
                     return ToolResult::json(Json::object({{"invalidated", static_cast<int64_t>(ids->size())}}),
                                             std::to_string(ids->size()) + " probes will re-capture over the next frames");
                 }
                 auto cap = renderFrame(engine, a.get("view"), true);
                 if (!cap) return ToolResult::error(cap.error());
                 Json info = engine.renderer().reflectionProbeInfo(&cap->frame);
                 if (!info.isObject()) {
                     return ToolResult::error(Error::make("unsupported", "this renderer backend has no reflection probes",
                                                          "run with the Metal or null renderer"));
                 }
                 info = namedInfo(engine.scene(), info, 0);
                 Json baked = Json::array();
                 for (const Json& p : info.get("probes").elements()) {
                     const auto e = static_cast<EntityId>(p.get("entity").asInt());
                     if (std::find(ids->begin(), ids->end(), e) != ids->end()) baked.push(p);
                 }
                 info["probes"] = baked;
                 return ToolResult::json(info, summary(info));
             }});

    reg.add({"probe_info", "Inspect reflection probes",
             "Reflection probes and their atlas: the budget (Environment.probeBudget probes, probeUpdates faces per "
             "frame), atlas resolution / slots / memory, and per probe: slot and debug color (the reflection_probes "
             "debug view draws it), whether it is captured, lighting this view, waiting for the face budget, over budget "
             "or out of view, captures so far, last capture GPU time, `stale` (something in range changed since a once "
             "probe was captured: run probe_bake), size, update mode. Warnings: overlapping volumes with equal "
             "priority, probes with no geometry in them, capture points inside a mesh, atlas exhaustion. Renders one "
             "real-time frame of `view` first. See the volumes with viewport_capture {debug_view: \"reflection_probes\"} "
             "and a probe's cubemap with viewport_capture {probe: \"Hall Probe\"}. Example: {\"entity\": \"Hall Probe\"}.",
             "render",
             object({{"view", any("Camera of the frame: \"editor\" (default), \"scene\", or {eye, target, fov}")},
                     {"entity", schema::entity("Only report this probe")},
                     {"render", boolean("Render a frame of `view` first (default true; false = the last frame)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 EntityId only = kNoEntity;
                 if (a.contains("entity")) {
                     auto id = resolveProbe(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     only = *id;
                 }
                 std::optional<FrameData> frame;
                 if (a.get("render").asBool(true)) {
                     auto cap = renderFrame(engine, a.get("view"), false);
                     if (!cap) return ToolResult::error(cap.error());
                     frame = std::move(cap->frame);
                 }
                 Json info = engine.renderer().reflectionProbeInfo(frame ? &*frame : nullptr);
                 if (!info.isObject()) {
                     return ToolResult::error(Error::make("unsupported", "this renderer backend has no reflection probes",
                                                          "run with the Metal or null renderer"));
                 }
                 info = namedInfo(engine.scene(), info, only);
                 return ToolResult::json(info, summary(info));
             }});
}

}  // namespace sky::tools
