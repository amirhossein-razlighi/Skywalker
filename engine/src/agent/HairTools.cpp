// Hair & fur tools (grooms) and GPU effects statistics.
//
//   groom_create   preset + overrides onto an entity with a mesh (a head, an animal); grooms stack: each
//                  further groom (beard, brows on a scalp) lives on its own child entity growing on that mesh
//   groom_update   change fields (or switch preset); regenerates when geometry fields change
//   groom_info     strand / guide / point counts, memory, generation and GPU cost
//   groom_export   rest-pose strands to .hair / .groom.json / .skygroom (DCC round trips)
//   fx_stats       GPU timings, live GPU particle counts and groom costs from the renderer

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/fx/Groom.h"

namespace sky::tools {

namespace {

using namespace schema;

std::vector<std::string> groomPresets() { return Groom::presets(); }

Json groomInfo(Engine& engine, EntityId e) {
    Scene& s = engine.scene();
    const Groom* g = s.get<Groom>(e);
    Json j = Json::object({{"entity", e}, {"name", s.record(e) ? s.record(e)->name : ""}});
    if (!g) {
        j["error"] = "no groom component";
        return j;
    }
    std::string error;
    auto data = engine.grooms().groomFor(
        s, e, [&engine](const std::string& k) { return engine.cpuMesh(k); },
        [&engine](const std::string& p) { return engine.resolvePath(p); }, &error);
    j["preset"] = g->preset;
    if (!data) {
        j["error"] = error.empty() ? "could not generate the groom" : error;
        return j;
    }
    const size_t P = data->points, N = data->strandCount(), G = data->guideCount();
    // GPU memory: guides (rest + 2 simulation states, float4), children (32 B + half4 offsets per
    // point), rendered positions (float4 per point).
    const size_t gpuBytes = G * P * 16 * 3 + N * 48 + N * P * 8 + N * P * 16;
    j["strands"] = N;
    j["guides"] = G;
    j["pointsPerStrand"] = P;
    j["segments"] = P ? P - 1 : 0;
    j["points"] = N * P;
    j["cpuMemoryMB"] = static_cast<double>(data->memoryBytes()) / (1024.0 * 1024.0);
    j["gpuMemoryMB"] = static_cast<double>(gpuBytes) / (1024.0 * 1024.0);
    j["generateMs"] = engine.grooms().lastGenerateMs();
    j["boundsMin"] = reflect::vec3ToJson(data->bounds.min);
    j["boundsMax"] = reflect::vec3ToJson(data->bounds.max);
    j["simulated"] = g->simulate;
    // Skinned characters: roots bound to the mesh's triangles follow the animated skin.
    Json attach = Json::object({{"mode", g->attach}, {"bound", data->bound()}, {"boundRoots", data->childBind.size()}});
    if (data->bindError > 0.f) attach["bindErrorM"] = std::round(data->bindError * 10000.f) / 10000.f;
    if (!g->maskBone.empty()) attach["maskBone"] = g->maskBone;
    j["attach"] = attach;
    Json stats = engine.renderer().stats();
    if (const Json* grooms = stats.find("grooms")) {
        for (const auto& item : grooms->elements()) {
            if (item.get("entity").asInt() == static_cast<int64_t>(e)) j["gpu"] = item;
        }
    }
    if (const Json* ms = stats.find("frameGpuMs")) j["frameGpuMs"] = *ms;
    return j;
}

/// Every entity in `root`'s subtree (root first).
std::vector<EntityId> subtree(const Scene& s, EntityId root) {
    std::vector<EntityId> out{root};
    for (size_t i = 0; i < out.size(); ++i) {
        for (EntityId c : s.children(out[i])) out.push_back(c);
    }
    return out;
}

/// The mesh a groom on `e` grows on when no target is given: e's own mesh, else (a character root)
/// its largest skinned part, preferring one named like a body.
EntityId defaultGroomMesh(Engine& engine, EntityId e) {
    Scene& s = engine.scene();
    if (s.get<MeshRenderer>(e)) return e;
    EntityId best = kNoEntity;
    double bestScore = 0;
    for (EntityId id : subtree(s, e)) {
        const MeshRenderer* m = s.get<MeshRenderer>(id);
        if (!m || s.get<Groom>(id)) continue;
        const MeshData* md = str::startsWith(m->mesh, "asset:") ? engine.cpuMesh(m->mesh) : nullptr;
        if (!md || !md->skinned()) continue;
        const std::string name = str::lower(s.record(id) ? s.record(id)->name : "");
        double score = static_cast<double>(md->vertexCount()) * (name.find("body") != std::string::npos ? 4.0 : 1.0);
        if (score > bestScore) bestScore = score, best = id;
    }
    return best;
}

/// "hair_scalp" -> "Hair Scalp"
std::string presetTitle(const std::string& preset) {
    std::string out;
    bool up = true;
    for (char c : preset) {
        if (c == '_') {
            out += ' ';
            up = true;
        } else {
            out += up ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            up = false;
        }
    }
    return out;
}

std::string summary(const Json& info) {
    if (info.contains("error")) return "groom: " + info.get("error").asString();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%lld strands x %lld points (%lld guides), %.1f MB GPU",
                  static_cast<long long>(info.get("strands").asInt()), static_cast<long long>(info.get("pointsPerStrand").asInt()),
                  static_cast<long long>(info.get("guides").asInt()), info.get("gpuMemoryMB").asNumber());
    return buf;
}

}  // namespace

void addHairTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"groom_create", "Create hair or fur",
             "Grow strand hair or fur on an entity's mesh (a head, a bust, an animal body) from a preset, ready to tweak. "
             "Presets: hair_straight, hair_wavy, hair_curly, hair_ponytail, hair_short (heads: they grow on the upper back "
             "of the mesh; set maskDirection/maskAngle or a vertex-color mask for your model), fur_short, fur_long (cover "
             "the whole mesh); for rigged characters: hair_scalp (dense scalp hair on the Head bone's vertices), beard, eyebrows "
             "(regions in the head's bounds, mirrored brows), fur_dense (a creature's coat). On a rigged mesh the roots ride "
             "the animated skin (attach auto: rooted on triangles, follow, maxSpeed, capsules fitted to the skeleton as "
             "colliders); maskBone / maskCenter / maskRadius / maskSpace limit growth to a region. `overrides` patches groom fields, e.g. {\"melanin\": 0.2, \"length\": 0.5, \"strands\": "
             "100000}. Color is physically based: melanin 0 white .. 0.3 blond .. 0.8 brown .. 1 black, redness for "
             "auburn/ginger, dye for unnatural tints. Hair is simulated (gravity, wind, collisions with the mesh) and "
             "rendered as real strands with Marschner shading and self-shadowing. Example: {\"entity\": \"Head\", "
             "\"preset\": \"hair_wavy\", \"overrides\": {\"melanin\": 0.25}}. Grooms stack: the first groom goes "
             "on the mesh entity itself, every further one (a beard and eyebrows on a scalp) on a new child entity named "
             "after the preset (or `name`) that grows on the same mesh and skin; `replace: true` replaces the existing "
             "groom instead. On a character root without a mesh, hair grows on its largest skinned part (the body). "
             "The result's `entity` is the groom's entity (edit it with groom_update, remove it with entity_delete). "
             "Example: {\"entity\": \"Hero\", \"preset\": \"beard\", \"overrides\": {\"melanin\": 0.6}}.",
             "render",
             object({{"entity", entity("Entity with the mesh to grow on, or a character root (its body mesh)")},
                     {"preset", enumeration(groomPresets(), "Starting style")},
                     {"overrides", Json::object({{"type", "object"}, {"description", "Groom fields to change"}})},
                     {"name", string("Name of the groom's own entity when it is added beside other grooms (default: the "
                                     "preset, e.g. \"Beard\")")},
                     {"replace", boolean("Replace the groom already on `entity` instead of adding another one")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::string preset = a.get("preset").asString("hair_straight");
                 Json patch = fx::groomPreset(preset);
                 if (!patch.size()) {
                     std::string guess = str::closest(preset, groomPresets());
                     return ToolResult::error(Error::make("unknown_preset", "no groom preset '" + preset + "'",
                                                          guess.empty() ? "" : "did you mean '" + guess + "'?"));
                 }
                 for (const auto& [k, v] : a.get("overrides").members()) patch[k] = v;
                 Scene& s = engine.scene();
                 const bool hasTarget = !patch.get("target").isNull() && !(patch.get("target").isString() && patch.get("target").asString().empty());
                 const EntityId mesh = hasTarget ? kNoEntity : defaultGroomMesh(engine, *id);
                 if (!hasTarget && !mesh) {
                     return ToolResult::error(Error::make("no_mesh", "entity has no mesh to grow hair on",
                                                          "add a mesh (e.g. a sphere head), pick a character with a skinned "
                                                          "body or set overrides.target"));
                 }
                 // Where the groom lives: on the mesh entity itself while it has none (or with replace),
                 // otherwise on a new child of the mesh entity growing on the same mesh.
                 const bool replace = a.get("replace").asBool(false);
                 EntityId host = mesh && mesh != *id ? mesh : *id;
                 const bool stack = !replace && (s.get<Groom>(host) || a.contains("name") || (mesh && mesh != *id));
                 EntityId groomEntity = host;
                 Status st = engine.edit(ctx.actor, "Groom: " + preset, [&]() -> Status {
                     if (stack) {
                         std::string base = a.get("name").asString(presetTitle(preset));
                         std::string name = base;
                         auto taken = [&](const std::string& n) {
                             for (EntityId c : s.children(host)) {
                                 if (s.record(c) && s.record(c)->name == n) return true;
                             }
                             return false;
                         };
                         for (int i = 2; taken(name); ++i) name = base + " " + std::to_string(i);
                         groomEntity = s.create(name, host);
                         s.add<Transform>(groomEntity);
                         if (!hasTarget) patch["target"] = Json::object({{"id", host}, {"name", s.record(host)->name}});
                     } else if (s.get<Groom>(groomEntity)) {
                         // Start from defaults so a new preset fully replaces an older groom.
                         if (Status r = s.patchComponent(groomEntity, "groom", Json()); !r) return r;
                     }
                     return s.patchComponent(groomEntity, "groom", patch);
                 });
                 if (!st) return fail(st);
                 Json info = groomInfo(engine, groomEntity);
                 const std::string where = std::string(s.record(groomEntity) ? s.record(groomEntity)->name : "") + " (#" +
                                           std::to_string(groomEntity) + ")";
                 return ToolResult::json(info, "groom " + preset + " on " + where + ": " + summary(info));
             }});

    reg.add({"groom_update", "Update hair or fur",
             "Change a groom's fields (or switch to another preset and then apply `fields`). Geometry fields (strands, "
             "length, curls, clumps, mask...) regenerate the strands; color, shading and motion fields apply instantly. "
             "Example: {\"entity\": \"Head\", \"fields\": {\"curlRadius\": 0.01, \"curlFrequency\": 18, \"melanin\": 0.9}}.",
             "render",
             object({{"entity", entity("Entity with a groom")},
                     {"preset", enumeration(groomPresets(), "Reset to this preset first")},
                     {"fields", Json::object({{"type", "object"}, {"description", "Groom fields to change"}})}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 Scene& s = engine.scene();
                 if (!s.get<Groom>(*id) && !a.contains("preset")) {
                     return ToolResult::error(Error::make("no_groom", "entity has no groom", "use groom_create first"));
                 }
                 Json patch = Json::object();
                 if (a.contains("preset")) {
                     patch = fx::groomPreset(a.get("preset").asString());
                     if (!patch.size()) return ToolResult::error(Error::make("unknown_preset", "no such groom preset"));
                 }
                 for (const auto& [k, v] : a.get("fields").members()) patch[k] = v;
                 Status st = engine.edit(ctx.actor, "Update groom", [&]() -> Status {
                     if (a.contains("preset") && s.get<Groom>(*id)) {
                         if (Status r = s.patchComponent(*id, "groom", Json()); !r) return r;
                     }
                     return s.patchComponent(*id, "groom", patch);
                 });
                 if (!st) return fail(st);
                 Json info = groomInfo(engine, *id);
                 return ToolResult::json(info, summary(info));
             }});

    reg.add({"groom_info", "Hair or fur info",
             "Strand, guide and point counts, memory (CPU and GPU), generation time and the measured GPU cost (simulation "
             "and drawing) of a groom, or of every groom in the scene when `entity` is omitted. Use it to budget hair "
             "(e.g. keep a hero head under ~2 ms at 1080p). On a character (or a mesh with several grooms) it lists every "
             "groom growing on it (`grooms`).",
             "render", object({{"entity", entity("Entity with a groom (omit for all)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     Scene& s = engine.scene();
                     if (!s.get<Groom>(*id)) {  // a character or a head with stacked grooms: all of them
                         std::vector<EntityId> tree = subtree(s, *id);
                         Json list = Json::array();
                         for (EntityId e : s.entities()) {
                             if (!s.get<Groom>(e)) continue;
                             const bool inside = std::find(tree.begin(), tree.end(), e) != tree.end();
                             const EntityId m = fx::groomMeshEntity(s, e);
                             if (inside || std::find(tree.begin(), tree.end(), m) != tree.end()) list.push(groomInfo(engine, e));
                         }
                         if (!list.size()) return ToolResult::json(groomInfo(engine, *id), "no groom on that entity");
                         return ToolResult::json(Json::object({{"entity", *id}, {"grooms", list}}),
                                                 std::to_string(list.size()) + " groom(s) on " + formatEntityRef(*id));
                     }
                     Json info = groomInfo(engine, *id);
                     return ToolResult::json(info, summary(info));
                 }
                 Json all = Json::array();
                 for (EntityId e : engine.scene().entities()) {
                     if (engine.scene().get<Groom>(e)) all.push(groomInfo(engine, e));
                 }
                 return ToolResult::json(Json::object({{"grooms", all}}), std::to_string(all.size()) + " groom(s)");
             }});

    reg.add({"groom_export", "Export groom strands",
             "Write a groom's rest-pose strands to a file for DCC round trips: .hair (Cem Yuksel's format, read by many "
             "hair tools), .groom.json (readable) or .skygroom (compact binary). Paths are project-relative.",
             "asset",
             object({{"entity", entity("Entity with a groom")}, {"path", string("Output file (.hair, .groom.json, .skygroom)")}},
                    {"entity", "path"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::string error;
                 auto data = engine.grooms().groomFor(
                     engine.scene(), *id, [&engine](const std::string& k) { return engine.cpuMesh(k); },
                     [&engine](const std::string& p) { return engine.resolvePath(p); }, &error);
                 if (!data) return ToolResult::error(Error::make("no_groom", error.empty() ? "no groom on that entity" : error));
                 fx::StrandSet strands = fx::restStrands(*data);
                 std::string rel = a.get("path").asString();
                 std::string path = engine.resolvePath(rel);
                 std::string lower = str::lower(rel);
                 std::ofstream out(path, std::ios::binary);
                 if (!out) return ToolResult::error(Error::make("io_error", "cannot write " + rel));
                 if (lower.size() > 5 && lower.substr(lower.size() - 5) == ".hair") {
                     auto bytes = fx::writeHairFile(strands);
                     out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                 } else if (lower.size() > 9 && lower.substr(lower.size() - 9) == ".skygroom") {
                     auto bytes = fx::writeSkyGroom(strands);
                     out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                 } else {
                     out << fx::writeGroomJson(strands).dump();
                 }
                 if (!out) return ToolResult::error(Error::make("io_error", "failed writing " + rel));
                 return ToolResult::json(Json::object({{"path", rel}, {"strands", strands.strandCount()}}),
                                         "exported " + std::to_string(strands.strandCount()) + " strands to " + rel);
             }});

    reg.add({"fx_benchmark", "Benchmark GPU frame time",
             "Render `frames` real-time frames back to back (no readback, effects time advancing 1/60 s per frame, so "
             "GPU particles and hair simulate) and report the GPU time per frame and per effects pass, plus the wall-clock "
             "time. Use it to budget effects: e.g. {\"width\":1920,\"height\":1080,\"frames\":120}. The view is the "
             "editor camera unless eye/target are given.",
             "render",
             object({{"width", integer("Width (default 1920)")},
                     {"height", integer("Height (default 1080)")},
                     {"frames", integer("Frames to render (default 120, max 2000)")},
                     {"eye", vec3("Camera position")},
                     {"target", vec3("Look-at point")},
                     {"serial", boolean("Wait for each frame (default true): exact per-pass GPU times; false = frames "
                                        "overlap like a game loop (throughput)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 CaptureOptions o;
                 o.width = static_cast<int>(std::clamp<int64_t>(a.get("width").asInt(1920), 16, 4096));
                 o.height = static_cast<int>(std::clamp<int64_t>(a.get("height").asInt(1080), 16, 4096));
                 o.samples = 1;
                 o.editorOverlays = false;
                 Vec3 eye, target;
                 if (reflect::jsonToVec3(a.get("eye"), eye)) {
                     o.hasCustomView = true;
                     o.customView = engine.camera().toView();
                     o.customView.eye = eye;
                     if (reflect::jsonToVec3(a.get("target"), target)) o.customView.target = target;
                 }
                 const int frames = static_cast<int>(std::clamp<int64_t>(a.get("frames").asInt(120), 1, 2000));
                 const bool serial = a.get("serial").asBool(true);
                 FrameData base = engine.frame(o);
                 auto start = std::chrono::steady_clock::now();
                 for (int i = 0; i < frames; ++i) {
                     FrameData f = base;
                     f.time = base.time + static_cast<float>(i + 1) / 60.f;
                     if (Status st = engine.renderer().render(f); !st) return fail(st);
                     if (serial) (void)engine.renderer().readback();
                 }
                 (void)engine.renderer().readback();  // waits for the GPU
                 double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                 Json stats = engine.renderer().stats();
                 stats["frames"] = frames;
                 stats["wallMsPerFrame"] = wall / frames;
                 stats["resolution"] = std::to_string(o.width) + "x" + std::to_string(o.height);
                 char buf[160];
                 std::snprintf(buf, sizeof(buf), "%d frames: %.2f ms GPU/frame, %.2f ms wall/frame", frames,
                               stats.get("frameGpuMs").asNumber(), wall / frames);
                 return ToolResult::json(stats, buf);
             }});

    reg.add({"fx_stats", "GPU effects stats",
             "Measured GPU cost of the last rendered frames: whole-frame GPU time, the GPU simulation pass (particles + "
             "hair), per-emitter live particle counts (read back asynchronously, a few frames old) and per-groom sizes. "
             "Render a few frames (viewport_capture) first; values are 0 on renderers without GPU effects.",
             "render", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 Json stats = engine.renderer().stats();
                 return ToolResult::json(stats, "gpu stats");
             }});
}

}  // namespace sky::tools
