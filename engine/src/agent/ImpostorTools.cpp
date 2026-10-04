// Foliage impostor tools: bake (or rebake) the octahedral impostors that draw distant
// instances of heavy foliage layers, and inspect their atlases.

#include <algorithm>
#include <cmath>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Impostor.h"

namespace sky::tools {

namespace {

using namespace schema;

}  // namespace

void addImpostorTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"impostor_bake", "Bake foliage impostors",
             "Bake the octahedral impostors of foliage layers now: each heavy model (an imported tree, bush, rock or "
             "grass clump; one mesh or every part of a prefab) is captured from up to 32x32 directions into an atlas "
             "of albedo, normal and depth, and instances beyond the layer's transition distance render as impostors "
             "(lit, shadowed and depth-correct like real geometry). The renderer bakes on first use and caches in "
             ".skywalker/cache/impostors/, so this is for baking ahead of time, rebaking after editing a mesh or "
             "material (rebake:true), or checking the atlases (preview:true returns the first atlas as an image). "
             "Tune per layer with impostorDistance (m; 0 auto from on-screen size, -1 off), impostorResolution "
             "(atlas px) and impostorFrames (views per side); see the transition distance in the result. "
             "Example: {\"entity\":\"Forest\",\"layer\":0,\"rebake\":true,\"preview\":true}.",
             "render",
             object({{"entity", entity("Foliage entity (default: every foliage entity in the scene)")},
                     {"layer", any("Layer index or name (default: every layer)")},
                     {"rebake", boolean("Bake again even when a cached atlas exists (default false)")},
                     {"preview", boolean("Return the first atlas as an image (default false)")},
                     {"save_preview", string("Also write that preview PNG to this project-relative path")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& scene = engine.scene();
                 std::vector<EntityId> targets;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     if (!scene.get<Foliage>(*id)) {
                         return ToolResult::error(Error::make("not_foliage", "entity #" + std::to_string(*id) + " has no foliage component",
                                                              "pass the entity that foliage_add created (e.g. \"Forest\")"));
                     }
                     targets.push_back(*id);
                 } else {
                     for (EntityId e : scene.entities()) {
                         if (scene.get<Foliage>(e)) targets.push_back(e);
                     }
                 }
                 if (targets.empty()) return ToolResult::error(Error::make("not_found", "the scene has no foliage", "use foliage_add first"));
                 const Json& layerArg = a.get("layer");
                 std::vector<world::WorldRuntime::LayerImpostor> layers;
                 std::vector<EntityId> owners;
                 for (EntityId e : targets) {
                     int layer = -1;
                     if (layerArg.isNumber()) {
                         layer = static_cast<int>(layerArg.asInt());
                     } else if (layerArg.isString()) {
                         auto parsed = world::foliageLayersFromJson(scene.get<Foliage>(e)->layers);
                         std::vector<std::string> names;
                         for (size_t i = 0; i < parsed.size(); ++i) {
                             names.push_back(parsed[i].name);
                             if (parsed[i].name == layerArg.asString()) layer = static_cast<int>(i);
                         }
                         if (layer < 0) {
                             std::string guess = str::closest(layerArg.asString(), names);
                             return ToolResult::error(Error::make("unknown_layer", "no foliage layer named '" + layerArg.asString() + "'",
                                                                  guess.empty() ? "pass the layer index" : "did you mean '" + guess + "'?"));
                         }
                     }
                     for (auto& li : engine.world().impostorModels(scene, e, layer)) {
                         layers.push_back(std::move(li));
                         owners.push_back(e);
                     }
                 }
                 if (layers.empty()) {
                     return ToolResult::error(Error::make("no_impostors", "no layer uses impostors",
                                                          "impostors need a real mesh (300+ triangles) and impostors not set to false; "
                                                          "meshes still loading are skipped"));
                 }
                 std::vector<ImpostorModel> models;
                 for (const auto& l : layers) models.push_back(l.model);
                 auto baked = engine.renderer().bakeImpostors(models, a.get("rebake").asBool(false));
                 if (!baked) return ToolResult::error(baked.error());
                 Json out = Json::array();
                 std::ostringstream text;
                 for (size_t i = 0; i < layers.size(); ++i) {
                     Json r = baked.value()[i];
                     r["entity"] = owners[i];
                     r["layer"] = layers[i].layer;
                     r["name"] = layers[i].name;
                     r["transitionDistance"] = std::round(layers[i].transitionDistance * 10.f) / 10.f;
                     r["cullDistance"] = layers[i].cullDistance;
                     char line[320];
                     std::snprintf(line, sizeof(line), "%s: %s, %lld px atlas, %lldx%lld views, impostors beyond %.0f m (cull %.0f m), %.0f ms%s\n",
                                   layers[i].name.c_str(), r.get("ok").asBool() ? "ok" : "FAILED",
                                   static_cast<long long>(r.get("atlas").asInt()), static_cast<long long>(r.get("frames").asInt()),
                                   static_cast<long long>(r.get("frames").asInt()), layers[i].transitionDistance, layers[i].cullDistance,
                                   r.get("ms").asFloat(), r.get("fromCache").asBool() ? " (cache)" : "");
                     text << line;
                     out.push(r);
                 }
                 ToolResult result = ToolResult::json(Json::object({{"impostors", out}}), text.str());
                 if (a.get("preview").asBool(false) || a.contains("save_preview")) {
                     auto loaded = impostor::load(models.front().cachePath);
                     if (!loaded) return ToolResult::error(loaded.error());
                     Image img = impostor::preview(loaded->atlas);
                     if (a.contains("save_preview")) {
                         if (Status s = writePng(img, engine.resolvePath(a.get("save_preview").asString())); !s) return fail(s);
                     }
                     if (a.get("preview").asBool(false)) {
                         std::vector<uint8_t> png = encodePng(img);
                         result.image(str::base64Encode(png.data(), png.size()));
                     }
                 }
                 return result;
             }});
}

}  // namespace sky::tools
