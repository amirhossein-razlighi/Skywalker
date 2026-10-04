// Render layer tools (docs/RENDERING.md "Render layers"): name the 20 layers in game.json and put
// meshes on layers / give cameras and lights cull masks by name, with did-you-mean errors.

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/render/RenderLayers.h"

namespace sky::tools {

namespace fs = std::filesystem;

namespace {

using namespace schema;

Result<render::LayerNames> projectLayers(Engine& engine) {
    auto settings = game::GameSettings::load(engine.config().projectDir);
    if (!settings) return settings.error();
    return settings->renderLayers;
}

Json maskJson(uint32_t mask, const render::LayerNames& names) {
    return Json::object({{"mask", static_cast<int64_t>(mask)}, {"layers", render::describeLayerMask(mask, names)}});
}

/// Everything that takes part in layers: meshes (layers), cameras and lights (cullMask).
Json overview(const Scene& s, const render::LayerNames& names, Json& warnings) {
    Json layers = Json::array();
    std::array<int, render::kLayerCount> meshCount{};
    Json cameras = Json::array(), lights = Json::array();
    uint32_t cameraUnion = 0;
    bool anyCamera = false;
    for (EntityId e : s.entities()) {
        if (const MeshRenderer* m = s.get<MeshRenderer>(e)) {
            for (int i = 0; i < render::kLayerCount; ++i) meshCount[static_cast<size_t>(i)] += (m->layers >> i) & 1;
        }
        if (const Camera* c = s.get<Camera>(e)) {
            anyCamera = true;
            cameraUnion |= static_cast<uint32_t>(c->cullMask);
            Json j = maskJson(static_cast<uint32_t>(c->cullMask) & render::kAllLayers, names);
            j["entity"] = static_cast<int64_t>(e);
            j["name"] = s.record(e)->name;
            j["primary"] = c->primary;
            cameras.push(j);
        }
        if (const Light* l = s.get<Light>(e)) {
            const uint32_t mask = static_cast<uint32_t>(l->cullMask) & render::kAllLayers;
            if (mask == render::kAllLayers) continue;  // only lights with a restricted mask are interesting
            Json j = maskJson(mask, names);
            j["entity"] = static_cast<int64_t>(e);
            j["name"] = s.record(e)->name;
            lights.push(j);
            if (mask == 0) warnings.push("light '" + s.record(e)->name + "' has cullMask 0 and lights nothing");
        }
    }
    for (int i = 0; i < render::kLayerCount; ++i) {
        const std::string& n = names.names[static_cast<size_t>(i)];
        if (n.empty() && meshCount[static_cast<size_t>(i)] == 0) continue;
        Json j = Json::object({{"layer", i + 1}, {"meshes", meshCount[static_cast<size_t>(i)]}});
        if (!n.empty()) j["name"] = n;
        if (anyCamera && meshCount[static_cast<size_t>(i)] > 0 && !(cameraUnion & (1u << i))) {
            warnings.push("layer " + std::to_string(i + 1) + (n.empty() ? "" : " (" + n + ")") + " has " +
                          std::to_string(meshCount[static_cast<size_t>(i)]) + " mesh(es) that no scene camera draws");
        }
        layers.push(j);
    }
    return Json::object({{"layers", layers}, {"cameras", cameras}, {"restrictedLights", lights}});
}

/// Writes game.json render.layers.<n> = name ("" removes), keeping the file minimal.
Status writeLayerName(Engine& engine, int layer, const std::string& name) {
    const fs::path file = fs::path(engine.config().projectDir) / "game.json";
    Json current = Json::object();
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();
        auto doc = Json::parse(ss.str());
        if (!doc) return Error::make("invalid_game_json", "game.json: " + doc.error().message);
        current = *doc;
    }
    Json patch = Json::object({{"render", Json::object({{"layers", Json::object({{std::to_string(layer), name.empty() ? Json() : Json(name)}})}})}});
    current.mergePatch(patch);
    if (auto parsed = game::GameSettings::fromJson(current); !parsed) return parsed.error();
    fs::create_directories(engine.config().projectDir, ec);
    std::ofstream out(file);
    out << current.dump(2) << "\n";
    if (!out) return Error::make("io_error", "cannot write " + file.string());
    return {};
}

}  // namespace

void addRenderLayerTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"render_layers", "Render layers and cull masks",
             "Render layers (20): MeshRenderer.layers says which layers a mesh is on; Camera.cullMask and "
             "Light.cullMask say which layers a camera draws and a light illuminates (a mesh is drawn / lit when they share a "
             "layer). Name layers in game.json and use the names everywhere. action get (default): named layers, meshes per "
             "layer, cameras and lights with restricted masks, plus warnings (meshes no camera sees, lights that light "
             "nothing). action name: {layer: 2, name: \"player\"} (name \"\" clears). action set: {entities, layers} puts meshes "
             "on layers and/or {entities, cull_mask} sets cameras' and lights' masks; masks accept a number (raw bits), a "
             "layer name, \"3\", \"all\", \"none\" or a list; mode add/remove edits the existing mask instead of replacing it. "
             "Uses: a rim light only on the hero ({entities:[\"RimLight\"], cull_mask:\"hero\"}), first-person arms hidden from "
             "a mirror camera, editor-only helpers on layer 20 hidden from the game camera. Example: {\"action\":\"set\","
             "\"entities\":[\"Hero\"],\"layers\":[\"world\",\"hero\"]}",
             "render",
             object({{"action", enumeration({"get", "name", "set"}, "get (default), name a layer, or set masks")},
                     {"layer", integer("name: layer number 1..20")},
                     {"name", string("name: the layer's name (\"\" removes it)")},
                     {"entities", array(entity(), "set: entities to change")},
                     {"layers", any("set: the meshes' layers (number, name, \"all\", or a list of names / layer numbers)")},
                     {"cull_mask", any("set: the cameras' / lights' cull mask (same forms as layers)")},
                     {"mode", enumeration({"replace", "add", "remove"}, "set: replace the mask (default), add layers to it, or remove them")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto names = projectLayers(engine);
                 if (!names) return fail(names.error());
                 const std::string action = a.get("action").asString("get");
                 Json warnings = Json::array();
                 if (action == "name") {
                     const int layer = static_cast<int>(a.get("layer").asInt(0));
                     if (layer < 1 || layer > render::kLayerCount) {
                         return fail(Error::make("invalid_layer", "layer must be a number from 1 to 20", "e.g. {\"layer\": 2, \"name\": \"player\"}"));
                     }
                     if (!a.contains("name") || !a.get("name").isString()) {
                         return fail(Error::make("invalid_arguments", "name needs a \"name\" (a string; \"\" clears it)"));
                     }
                     if (Status s = writeLayerName(engine, layer, a.get("name").asString()); !s) return fail(s);
                     names = projectLayers(engine);
                     if (!names) return fail(names.error());
                     Json j = overview(engine.scene(), *names, warnings);
                     j["named"] = names->toJson();
                     j["warnings"] = warnings;
                     return ToolResult::json(j, "layer " + std::to_string(layer) + " named in game.json");
                 }
                 if (action == "set") {
                     if (!a.get("entities").isArray() || a.get("entities").size() == 0) {
                         return fail(Error::make("invalid_arguments", "set needs \"entities\" (a list of ids or names)"));
                     }
                     if (!a.contains("layers") && !a.contains("cull_mask")) {
                         return fail(Error::make("invalid_arguments", "set needs \"layers\" (meshes) and/or \"cull_mask\" (cameras, lights)",
                                                 "e.g. {\"entities\":[\"Hero\"],\"layers\":\"hero\"}"));
                     }
                     std::optional<uint32_t> meshMask, cullMask;
                     if (a.contains("layers")) {
                         auto m = render::parseLayerMask(a.get("layers"), *names);
                         if (!m) return fail(m.error());
                         meshMask = *m;
                     }
                     if (a.contains("cull_mask")) {
                         auto m = render::parseLayerMask(a.get("cull_mask"), *names);
                         if (!m) return fail(m.error());
                         cullMask = *m;
                     }
                     const std::string mode = a.get("mode").asString("replace");
                     auto combine = [&](int current, uint32_t mask) {
                         uint32_t c = static_cast<uint32_t>(current) & render::kAllLayers;
                         if (mode == "add") return static_cast<int>(c | mask);
                         if (mode == "remove") return static_cast<int>(c & ~mask);
                         return static_cast<int>(mask);
                     };
                     Json changed = Json::array();
                     Status st = engine.edit(ctx.actor, "Render layers", [&]() -> Status {
                         Scene& s = engine.scene();
                         for (const Json& ref : a.get("entities").elements()) {
                             auto id = resolve(engine, ref);
                             if (!id) return id.error();
                             const std::string nm = s.record(*id)->name;
                             bool touched = false;
                             Json entry = Json::object({{"entity", static_cast<int64_t>(*id)}, {"name", nm}});
                             if (meshMask) {
                                 if (const MeshRenderer* m = s.get<MeshRenderer>(*id)) {
                                     const int v = combine(m->layers, *meshMask);
                                     if (Status r = s.patchComponent(*id, "mesh", Json::object({{"layers", v}})); !r) return r;
                                     entry["layers"] = maskJson(static_cast<uint32_t>(v), *names);
                                     if (v == 0) warnings.push("'" + nm + "' is on no layer now: no camera draws it");
                                     touched = true;
                                 } else {
                                     warnings.push("'" + nm + "' has no mesh: layers ignored (cameras and lights use cull_mask)");
                                 }
                             }
                             if (cullMask) {
                                 for (const char* comp : {"camera", "light"}) {
                                     int current = 0;
                                     if (const Camera* c = s.get<Camera>(*id); c && std::string(comp) == "camera") current = c->cullMask;
                                     else if (const Light* l = s.get<Light>(*id); l && std::string(comp) == "light") current = l->cullMask;
                                     else continue;
                                     const int v = combine(current, *cullMask);
                                     if (Status r = s.patchComponent(*id, comp, Json::object({{"cullMask", v}})); !r) return r;
                                     entry[std::string(comp) + "CullMask"] = maskJson(static_cast<uint32_t>(v), *names);
                                     if (v == 0) warnings.push("'" + nm + "' " + comp + " cullMask is 0: it " + (std::string(comp) == "camera" ? "draws" : "lights") + " nothing");
                                     touched = true;
                                 }
                                 if (!s.get<Camera>(*id) && !s.get<Light>(*id)) {
                                     warnings.push("'" + nm + "' is neither a camera nor a light: cull_mask ignored");
                                 }
                             }
                             if (touched) changed.push(entry);
                         }
                         return {};
                     });
                     if (!st) return fail(st);
                     Json j = Json::object({{"changed", changed}});
                     Json ov = overview(engine.scene(), *names, warnings);
                     j["overview"] = ov;
                     j["warnings"] = warnings;
                     return ToolResult::json(j, "updated " + std::to_string(changed.size()) + " entit" + (changed.size() == 1 ? "y" : "ies"));
                 }
                 Json j = overview(engine.scene(), *names, warnings);
                 j["named"] = names->toJson();
                 j["warnings"] = warnings;
                 return ToolResult::json(j, "render layers");
             }});
}

}  // namespace sky::tools
