// Hair & fur tools (grooms) and GPU effects statistics.
//
//   groom_create   preset + overrides onto an entity with a mesh (a head, an animal)
//   groom_update   change fields (or switch preset); regenerates when geometry fields change
//   groom_info     strand / guide / point counts, memory, generation and GPU cost
//   groom_export   rest-pose strands to .hair / .groom.json / .skygroom (DCC round trips)
//   fx_stats       GPU timings, live GPU particle counts and groom costs from the renderer

#include <algorithm>
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
    Json stats = engine.renderer().stats();
    if (const Json* grooms = stats.find("grooms")) {
        for (const auto& item : grooms->elements()) {
            if (item.get("entity").asInt() == static_cast<int64_t>(e)) j["gpu"] = item;
        }
    }
    if (const Json* ms = stats.find("frameGpuMs")) j["frameGpuMs"] = *ms;
    return j;
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
             "the whole mesh). `overrides` patches groom fields, e.g. {\"melanin\": 0.2, \"length\": 0.5, \"strands\": "
             "100000}. Color is physically based: melanin 0 white .. 0.3 blond .. 0.8 brown .. 1 black, redness for "
             "auburn/ginger, dye for unnatural tints. Hair is simulated (gravity, wind, collisions with the mesh) and "
             "rendered as real strands with Marschner shading and self-shadowing. Example: {\"entity\": \"Head\", "
             "\"preset\": \"hair_wavy\", \"overrides\": {\"melanin\": 0.25}}.",
             "render",
             object({{"entity", entity("Entity with the mesh to grow on (gets the groom component)")},
                     {"preset", enumeration(groomPresets(), "Starting style")},
                     {"overrides", Json::object({{"type", "object"}, {"description", "Groom fields to change"}})}},
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
                 const std::string target = patch.get("target").asString();
                 if (target.empty() && !s.get<MeshRenderer>(*id)) {
                     return ToolResult::error(Error::make("no_mesh", "entity has no mesh to grow hair on",
                                                          "add a mesh (e.g. a sphere head) or set overrides.target"));
                 }
                 Status st = engine.edit(ctx.actor, "Groom: " + preset, [&]() -> Status {
                     // Start from defaults so a new preset fully replaces an older groom.
                     if (s.get<Groom>(*id)) {
                         if (Status r = s.patchComponent(*id, "groom", Json()); !r) return r;
                     }
                     return s.patchComponent(*id, "groom", patch);
                 });
                 if (!st) return fail(st);
                 Json info = groomInfo(engine, *id);
                 return ToolResult::json(info, "groom " + preset + " on #" + std::to_string(*id) + ": " + summary(info));
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
             "(e.g. keep a hero head under ~2 ms at 1080p).",
             "render", object({{"entity", entity("Entity with a groom (omit for all)")}}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
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
