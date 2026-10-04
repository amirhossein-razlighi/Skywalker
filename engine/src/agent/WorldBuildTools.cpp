// World-building tools: terrains (generate, sculpt, paint, layers, queries) and foliage.
//
// Terrain heights and layer weights live in a .terrain file next to the scene (they are
// far too big for scene JSON). Every terrain edit keeps an in-memory undo snapshot so
// terrain_undo can step back.

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/fx/Particles.h"
#include "skywalker/render/TextureGen.h"

namespace sky::tools {

namespace {

namespace fs = std::filesystem;
using namespace schema;

/// A terrain undo step: the data plus the recorded edits and generator it was made from.
struct TerrainUndo {
    world::TerrainData data;
    Json edits;
    Json generator;
};

std::unordered_map<EntityId, std::deque<TerrainUndo>>& undoStacks() {
    static std::unordered_map<EntityId, std::deque<TerrainUndo>> stacks;
    return stacks;
}

void pushUndo(Engine& engine, EntityId e, const world::TerrainData& d) {
    const Terrain* t = engine.scene().get<Terrain>(e);
    auto& st = undoStacks()[e];
    st.push_back({d, t ? t->edits : Json::array(), t ? t->generator : Json::object()});
    while (st.size() > 12) st.pop_front();
}

/// Records a hand edit on the component so a rebuilt cache replays it (see world::applyEdits).
Status recordEdit(Engine& engine, EntityId e, Json edit, const std::string& actor) {
    Json edits = engine.scene().get<Terrain>(e)->edits;
    if (!edits.isArray()) edits = Json::array();
    edits.push(std::move(edit));
    return engine.edit(actor, "Terrain edit", [&]() -> Status {
        return engine.scene().patchComponent(e, "terrain", Json::object({{"edits", edits}}));
    });
}

std::string slug(const std::string& name) {
    std::string out;
    for (char c : name) out += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(c))) : '_';
    return out.empty() ? "terrain" : out;
}

/// Natural, muted palettes for terrain-scale textures (the generic texture_generate
/// defaults are tuned for props and read too saturated across a landscape).
texgen::Params terrainTextureParams(const std::string& kind) {
    texgen::Params p = texgen::defaults(kind);
    p.size = 1024;
    p.seed = 7;
    if (kind == "grass") {
        p.color1 = {0.16f, 0.22f, 0.08f, 1}, p.color2 = {0.30f, 0.34f, 0.13f, 1}, p.color3 = {0.42f, 0.38f, 0.20f, 1};
        p.roughness = 0.9f;
    } else if (kind == "sand") {
        p.color1 = {0.74f, 0.64f, 0.48f, 1}, p.color2 = {0.86f, 0.77f, 0.60f, 1}, p.color3 = {0.55f, 0.47f, 0.36f, 1};
        p.roughness = 0.92f, p.scale = 10.f;
    } else if (kind == "dirt") {
        p.color1 = {0.27f, 0.20f, 0.14f, 1}, p.color2 = {0.38f, 0.30f, 0.21f, 1}, p.color3 = {0.50f, 0.45f, 0.38f, 1};
    } else if (kind == "rock") {
        p.color1 = {0.30f, 0.28f, 0.26f, 1}, p.color2 = {0.52f, 0.49f, 0.45f, 1}, p.color3 = {0.14f, 0.13f, 0.12f, 1};
        p.scale = 2.f;
    }
    return p;
}

/// Writes procedural textures for layers that ask for one ("texgen": "sand") and points the
/// layer at them. Shared files per kind, generated once per project.
Json materializeLayerTextures(Engine& engine, Json layers) {
    if (!layers.isArray()) return layers;
    for (Json& l : layers.elements()) {
        std::string kind = l.get("texgen").asString();
        if (kind.empty() || l.contains("texture")) continue;
        std::string base = "textures/terrain/" + kind;
        std::string albedo = base + ".png", normal = base + "_normal.png", orm = base + "_orm.png";
        if (!fs::exists(engine.resolvePath(albedo))) {
            texgen::Params p = terrainTextureParams(kind);
            auto set = texgen::generate(p);
            if (!set) continue;
            std::error_code ec;
            fs::create_directories(engine.resolvePath("textures/terrain"), ec);
            if (!writePng(set->albedo, engine.resolvePath(albedo)) || !writePng(set->normal, engine.resolvePath(normal)) ||
                !writePng(set->orm, engine.resolvePath(orm))) {
                continue;
            }
        }
        l["texture"] = albedo;
        l["normalMap"] = normal;
        l["ormMap"] = orm;
        if (!l.contains("tint")) l["color"] = "#ffffff";
    }
    return layers;
}

struct TerrainRef {
    EntityId id;
    std::shared_ptr<world::TerrainData> data;
};

Result<TerrainRef> terrainOf(Engine& engine, const Json& ref) {
    auto id = resolve(engine, ref);
    if (!id) return id.error();
    if (!engine.scene().get<Terrain>(*id)) {
        return Error::make("not_a_terrain", "entity #" + std::to_string(*id) + " has no terrain component",
                           "create one with terrain_create");
    }
    auto data = engine.world().terrain(engine.scene(), *id);
    if (!data) return Error::make("terrain_unavailable", "terrain data could not be loaded or generated");
    return TerrainRef{*id, data};
}

/// Saves edited data to the terrain's file (creating a path if needed) and adopts it.
Status saveTerrain(Engine& engine, EntityId id, const std::shared_ptr<world::TerrainData>& data, const std::string& actor) {
    Scene& s = engine.scene();
    std::string path = s.get<Terrain>(id)->data;
    if (path.empty()) {
        path = "terrain/" + slug(s.record(id)->name) + "_" + std::to_string(id) + ".terrain";
        Status st = engine.edit(actor, "Terrain data file", [&]() -> Status {
            return s.patchComponent(id, "terrain", Json::object({{"data", path}}));
        });
        if (!st) return st;
    }
    std::error_code ec;
    fs::create_directories(fs::path(engine.resolvePath(path)).parent_path(), ec);
    if (Status st = data->save(engine.resolvePath(path)); !st) return st;
    engine.world().adopt(id, data, engine.world().sourceKey(s, id));
    // Physics: a matching heightfield collider (16-bit heights next to the terrain file).
    std::string r16 = fs::path(path).replace_extension(".r16").string();
    float lo = 0, hi = 0;
    if (Status st = data->saveHeightmap16(engine.resolvePath(r16), lo, hi); !st) return st;
    const float range = std::max(hi - lo, 0.01f);
    int res = 8;
    while (res < data->resolution() - 1 && res < 1024) res *= 2;
    Json collider = Json::object({{"shape", "heightfield"},
                                  {"heightmap", r16},
                                  {"size", Json::array({data->size(), range, data->size()})},
                                  {"offset", Json::array({0.0, static_cast<double>(lo), 0.0})},
                                  {"resolution", res}});
    return engine.edit(actor, "Terrain collider", [&]() -> Status { return s.patchComponent(id, "collider", collider); });
}

Json terrainStats(const world::TerrainData& d) {
    return Json::object({{"resolution", d.resolution()},
                         {"size", d.size()},
                         {"cell", std::round(d.cell() * 1000) / 1000},
                         {"minHeight", std::round(d.minHeight() * 100) / 100},
                         {"maxHeight", std::round(d.maxHeight() * 100) / 100}});
}

}  // namespace

void addWorldBuildTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"terrain_create", "Create terrain",
             "Create a large heightfield terrain: an island with beaches, a tropical coast, alpine peaks, a canyon, desert "
             "dunes, rolling hills or a mountain valley. Natural shapes come from warped noise plus hydraulic and thermal "
             "erosion; material layers (sand, grass, soil, rock, snow...) are auto-painted by height and slope, with "
             "procedural textures unless you pass your own layer textures (e.g. downloaded photoscans). `water: true` adds "
             "an FFT sea at sea level and wet sand along the shore. Then add foliage_add (grass, pebbles, shells, rocks). "
             "Your own relief: pass `heightmap` (a grayscale PNG). Examples: {\"preset\":\"island_beach\",\"size\":600,\"water\":true}, "
             "{\"heightmap\":\"maps/continent.png\",\"size\":4000,\"generator\":{\"minHeight\":-40,\"maxHeight\":160}}.",
             "world",
             object({{"name", string("Entity name (default \"Terrain\")")},
                     {"preset", enumeration(world::terrainPresets(), "Starting shape and layers")},
                     {"size", number("Square extent in meters (default 512)")},
                     {"resolution", integer("Height samples per side: 257, 513 (default), 1025, 2049")},
                     {"position", vec3("World position of the terrain center")},
                     {"seed", integer("Random seed (each seed is a different landscape)")},
                     {"generator", Json::object({{"type", "object"},
                                                 {"description", "Overrides: shape, minHeight, maxHeight, featureSize (m), ridges, "
                                                                 "warp, erosion, thermal, terraces, beachWidth, seaLevel, heightmap, detailNoise"}})},
                     {"layers", Json::object({{"type", "array"}, {"description", "Material layers (see the terrain component)"}})},
                     {"heightmap", string("Grayscale image (16-bit PNG, 8-bit PNG or square .r16) whose values 0..1 map to "
                                          "generator minHeight..maxHeight: hand-made or scripted continents, real-world-style "
                                          "relief. Row 0 is the -Z edge. Erosion/thermal from the generator still apply.")},
                     {"water", boolean("Add an ocean at sea level with a wet shoreline (island/coast presets)")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 const bool fromImage = a.contains("heightmap");
                 if (!fromImage && !a.contains("preset")) {
                     return ToolResult::error(Error::make("invalid_arguments", "terrain_create needs a preset or a heightmap",
                                                          "e.g. {\"preset\": \"island_beach\"} or {\"heightmap\": \"maps/height.png\"}"));
                 }
                 std::string preset = a.get("preset").asString(fromImage ? "rolling_hills" : "");
                 auto gp = world::terrainPreset(preset);
                 if (!gp) return ToolResult::error(gp.error());
                 if (fromImage) {
                     gp->shape = "heightmap";
                     gp->heightmap = a.get("heightmap").asString();
                     if (!a.contains("preset")) gp->minHeight = 0, gp->maxHeight = 100, gp->erosion = 0.2f, gp->thermal = 0.1f;
                 }
                 world::TerrainGenParams params = world::genParamsFromJson(a.get("generator"), *gp);
                 if (a.contains("seed")) params.seed = static_cast<uint32_t>(a.get("seed").asInt());
                 if (Status hs = world::resolveHeightmap(params, [&engine](const std::string& p) { return engine.resolvePath(p); }); !hs) {
                     return fail(hs);
                 }
                 Json layers = a.contains("layers") ? a.get("layers") : world::defaultTerrainLayers(preset);
                 layers = materializeLayerTextures(engine, layers);
                 int res = static_cast<int>(std::clamp<int64_t>(a.get("resolution").asInt(513), 65, 4097));
                 float size = std::clamp(a.get("size").asFloat(512.f), 16.f, 32768.f);
                 std::string name = a.get("name").asString("Terrain");
                 EntityId id = kNoEntity, sea = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Create terrain", [&]() -> Status {
                     Scene& s = engine.scene();
                     id = s.create(name);
                     Json t = Json::object({{"size", size}, {"resolution", res}, {"generator", world::toJson(params)}, {"layers", layers}});
                     Vec3 pos{0, 0, 0};
                     if (a.contains("position")) reflect::jsonToVec3(a.get("position"), pos);
                     bool water = a.get("water").asBool(false);
                     if (water) {
                         t["waterLevel"] = pos.y + params.seaLevel;
                         t["wetBand"] = 1.4;
                     }
                     if (Status r = s.patchComponent(id, "transform", Json::object({{"position", reflect::vec3ToJson(pos)}})); !r) return r;
                     if (Status r = s.patchComponent(id, "terrain", t); !r) return r;
                     if (water) {
                         sea = s.create("Sea");
                         if (Status r = s.patchComponent(sea, "transform",
                                                         Json::object({{"position", Json::array({pos.x, pos.y + params.seaLevel, pos.z})}}));
                             !r) {
                             return r;
                         }
                         Json wp = fx::waterPreset("calm_sea");
                         wp["depth"] = std::max(4.f, params.seaLevel - params.minHeight);
                         if (Status r = s.patchComponent(sea, "water", wp); !r) return r;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 auto data = engine.world().terrain(engine.scene(), id);
                 if (!data) return ToolResult::error(Error::make("terrain_failed", "terrain generation failed"));
                 if (Status s = saveTerrain(engine, id, data, ctx.actor); !s) return fail(s);
                 Json r = Json::object({{"entity", id}, {"name", name}, {"preset", preset}, {"stats", terrainStats(*data)},
                                        {"layers", static_cast<int64_t>(layers.size())}, {"data", engine.scene().get<Terrain>(id)->data}});
                 if (sea != kNoEntity) r["sea"] = sea;
                 return ToolResult::json(r, "created terrain #" + std::to_string(id) + " (" + preset + ")");
             }});

    reg.add({"terrain_generate", "Regenerate terrain",
             "Regenerate a terrain's heights from new generator parameters (shape, seed, heights, featureSize, ridges, warp, "
             "erosion, thermal, terraces, beachWidth, seaLevel), then re-paint its layers. Undo with terrain_undo.",
             "world",
             object({{"entity", entity("Terrain entity")},
                     {"generator", Json::object({{"type", "object"}, {"description", "Parameters to change"}})},
                     {"seed", integer("New seed")}},
                    {"entity"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto t = terrainOf(engine, a.get("entity"));
                 if (!t) return ToolResult::error(t.error());
                 const Terrain* comp = engine.scene().get<Terrain>(t->id);
                 world::TerrainGenParams p = world::genParamsFromJson(a.get("generator"), world::genParamsFromJson(comp->generator));
                 if (a.contains("seed")) p.seed = static_cast<uint32_t>(a.get("seed").asInt());
                 if (Status hs = world::resolveHeightmap(p, [&engine](const std::string& path) { return engine.resolvePath(path); }); !hs) {
                     return fail(hs);
                 }
                 pushUndo(engine, t->id, *t->data);
                 auto data = std::make_shared<world::TerrainData>(t->data->resolution(), t->data->size());
                 world::generate(*data, p);
                 world::autoPaint(*data, comp->layers, p.seed);
                 Status st = engine.edit(ctx.actor, "Terrain generator", [&]() -> Status {
                     return engine.scene().patchComponent(t->id, "terrain", Json::object({{"generator", world::toJson(p)}, {"edits", Json::array()}}));
                 });
                 if (!st) return fail(st);
                 if (Status s = saveTerrain(engine, t->id, data, ctx.actor); !s) return fail(s);
                 return ToolResult::json(Json::object({{"entity", t->id}, {"stats", terrainStats(*data)}}), "terrain regenerated");
             }});

    reg.add({"terrain_sculpt", "Sculpt terrain",
             "Sculpt a terrain with brush strokes at world (x, z) positions: raise / lower (strength in meters), flatten "
             "(toward `target` world height; plateaus, roads, building pads), smooth, noise (rougher ground), set. Great for "
             "carving a beach cove, a path, a harbor basin or a flat spot for a village. Undo with terrain_undo.",
             "world",
             object({{"entity", entity("Terrain entity")},
                     {"strokes",
                      array(Json::object({{"type", "object"},
                                          {"description", "{x, z, radius, strength, mode: raise|lower|flatten|smooth|noise|set, "
                                                          "target (world y, flatten/set), falloff 0..1}"}}),
                            "Brush strokes, applied in order")}},
                    {"entity", "strokes"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto t = terrainOf(engine, a.get("entity"));
                 if (!t) return ToolResult::error(t.error());
                 Vec3 o = engine.scene().worldMatrix(t->id).translation();
                 pushUndo(engine, t->id, *t->data);
                 auto data = std::make_shared<world::TerrainData>(*t->data);
                 // Strokes in terrain-local meters: applied now and recorded for cache rebuilds.
                 Json strokes = Json::array();
                 for (const auto& st : a.get("strokes").elements()) {
                     strokes.push(Json::object({{"x", st.get("x").asFloat() - o.x},
                                                {"z", st.get("z").asFloat() - o.z},
                                                {"radius", st.get("radius").asFloat(10.f)},
                                                {"strength", st.get("strength").asFloat(1.f)},
                                                {"mode", st.get("mode").asString("raise")},
                                                {"target", st.get("target").asFloat(o.y) - o.y},
                                                {"falloff", st.get("falloff").asFloat(0.5f)}}));
                 }
                 const int applied = static_cast<int>(strokes.size());
                 Json edit = Json::object({{"op", "sculpt"}, {"strokes", strokes}});
                 const Terrain* comp = engine.scene().get<Terrain>(t->id);
                 world::applyEdit(*data, edit, comp->layers, world::genParamsFromJson(comp->generator).seed);
                 if (Status s = recordEdit(engine, t->id, std::move(edit), ctx.actor); !s) return fail(s);
                 if (Status s = saveTerrain(engine, t->id, data, ctx.actor); !s) return fail(s);
                 return ToolResult::json(Json::object({{"entity", t->id}, {"strokes", applied}, {"stats", terrainStats(*data)}}),
                                         "sculpted " + std::to_string(applied) + " stroke(s)");
             }});

    reg.add({"terrain_paint", "Paint terrain layer",
             "Paint a material layer (by index or name) onto a terrain with brush strokes at world (x, z): sand paths, dirt "
             "patches, grass clearings. Foliage restricted to a layer follows the paint. Undo with terrain_undo.",
             "world",
             object({{"entity", entity("Terrain entity")},
                     {"layer", Json::object({{"description", "Layer index (0 = base) or name"}})},
                     {"strokes", array(Json::object({{"type", "object"}, {"description", "{x, z, radius, strength 0..1, falloff}"}}),
                                       "Brush strokes")}},
                    {"entity", "layer", "strokes"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto t = terrainOf(engine, a.get("entity"));
                 if (!t) return ToolResult::error(t.error());
                 const Terrain* comp = engine.scene().get<Terrain>(t->id);
                 int layer = -1;
                 if (a.get("layer").isNumber()) {
                     layer = static_cast<int>(a.get("layer").asInt());
                 } else {
                     std::vector<std::string> names;
                     for (size_t i = 0; i < comp->layers.size(); ++i) {
                         names.push_back(comp->layers[i].get("name").asString());
                         if (names.back() == a.get("layer").asString()) layer = static_cast<int>(i);
                     }
                     if (layer < 0) {
                         std::string guess = str::closest(a.get("layer").asString(), names);
                         return ToolResult::error(Error::make("unknown_layer", "no layer '" + a.get("layer").asString() + "'",
                                                              guess.empty() ? "" : "did you mean '" + guess + "'?"));
                     }
                 }
                 if (layer < 0 || layer >= static_cast<int>(std::max<size_t>(comp->layers.size(), 1))) {
                     return ToolResult::error(Error::make("invalid_layer", "layer index out of range"));
                 }
                 Vec3 o = engine.scene().worldMatrix(t->id).translation();
                 pushUndo(engine, t->id, *t->data);
                 auto data = std::make_shared<world::TerrainData>(*t->data);
                 Json strokes = Json::array();
                 for (const auto& st : a.get("strokes").elements()) {
                     strokes.push(Json::object({{"x", st.get("x").asFloat() - o.x},
                                                {"z", st.get("z").asFloat() - o.z},
                                                {"radius", st.get("radius").asFloat(8.f)},
                                                {"strength", st.get("strength").asFloat(0.8f)},
                                                {"falloff", st.get("falloff").asFloat(0.5f)}}));
                 }
                 const int n = static_cast<int>(strokes.size());
                 Json edit = Json::object({{"op", "paint"}, {"layer", layer}, {"strokes", strokes}});
                 world::applyEdit(*data, edit, comp->layers, 0);
                 if (Status s = recordEdit(engine, t->id, std::move(edit), ctx.actor); !s) return fail(s);
                 if (Status s = saveTerrain(engine, t->id, data, ctx.actor); !s) return fail(s);
                 return ToolResult::json(Json::object({{"entity", t->id}, {"layer", layer}, {"strokes", n}}), "painted");
             }});

    reg.add({"terrain_layers", "Terrain layers",
             "Replace a terrain's material layers and re-paint them from their rules (height/slope/noise). Each layer: {name, "
             "texture, normalMap, ormMap (project paths, e.g. downloaded photoscans) or texgen (sand, grass, dirt, rock, noise), "
             "color/tint, roughness, tiling (meters per repeat), triplanar (cliffs), heightMin, heightMax, slopeMin, slopeMax, "
             "noise, sharpness}. Base layer first; later layers paint over earlier ones where their rules match. Hand-painted "
             "weights are replaced unless auto_paint is false.",
             "world",
             object({{"entity", entity("Terrain entity")},
                     {"layers", Json::object({{"type", "array"}, {"description", "Up to 8 layers"}})},
                     {"auto_paint", boolean("Recompute weights from the rules (default true)")}},
                    {"entity", "layers"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto t = terrainOf(engine, a.get("entity"));
                 if (!t) return ToolResult::error(t.error());
                 Json layers = materializeLayerTextures(engine, a.get("layers"));
                 if (layers.size() > world::TerrainData::kMaxLayers) {
                     return ToolResult::error(Error::make("too_many_layers", "a terrain has at most 8 layers"));
                 }
                 Status st = engine.edit(ctx.actor, "Terrain layers", [&]() -> Status {
                     return engine.scene().patchComponent(t->id, "terrain", Json::object({{"layers", layers}}));
                 });
                 if (!st) return fail(st);
                 auto data = std::make_shared<world::TerrainData>(*t->data);
                 if (a.get("auto_paint").asBool(true)) {
                     pushUndo(engine, t->id, *t->data);
                     world::autoPaint(*data, layers, world::genParamsFromJson(engine.scene().get<Terrain>(t->id)->generator).seed);
                     // Repainting replaces every earlier paint: keep only the sculpting, then mark the repaint.
                     Json kept = Json::array();
                     for (const auto& e : engine.scene().get<Terrain>(t->id)->edits.elements()) {
                         if (e.get("op").asString() == "sculpt") kept.push(e);
                     }
                     kept.push(Json::object({{"op", "autopaint"}}));
                     Status rs = engine.edit(ctx.actor, "Terrain edit", [&]() -> Status {
                         return engine.scene().patchComponent(t->id, "terrain", Json::object({{"edits", kept}}));
                     });
                     if (!rs) return fail(rs);
                 }
                 if (Status s = saveTerrain(engine, t->id, data, ctx.actor); !s) return fail(s);
                 return ToolResult::json(Json::object({{"entity", t->id}, {"layers", static_cast<int64_t>(layers.size())}}), "layers updated");
             }});

    reg.add({"terrain_undo", "Undo terrain edit",
             "Step back the last terrain_sculpt / terrain_paint / terrain_generate / terrain_layers on a terrain (heights and "
             "weights; component fields use the regular undo).",
             "world", object({{"entity", entity("Terrain entity")}}, {"entity"}), true, false,
             [&engine](const Json& a, ToolContext& ctx) {
                 auto t = terrainOf(engine, a.get("entity"));
                 if (!t) return ToolResult::error(t.error());
                 auto& st = undoStacks()[t->id];
                 if (st.empty()) return ToolResult::error(Error::make("nothing_to_undo", "no terrain edits to undo"));
                 TerrainUndo step = std::move(st.back());
                 st.pop_back();
                 auto data = std::make_shared<world::TerrainData>(std::move(step.data));
                 data->touch();
                 Status rs = engine.edit(ctx.actor, "Terrain edit", [&]() -> Status {
                     return engine.scene().patchComponent(t->id, "terrain",
                                                          Json::object({{"edits", step.edits}, {"generator", step.generator}}));
                 });
                 if (!rs) return fail(rs);
                 if (Status s = saveTerrain(engine, t->id, data, ctx.actor); !s) return fail(s);
                 return ToolResult::json(Json::object({{"entity", t->id}, {"remaining", static_cast<int64_t>(st.size())}}), "terrain edit undone");
             }});

    reg.add({"terrain_query", "Query terrain",
             "Terrain height, normal, slope (degrees) and dominant layer at world (x, z) points, plus terrain stats. Use it to "
             "place villages on flat ground, find the shoreline, or check where cliffs are.",
             "world",
             object({{"entity", entity("Terrain entity (default: any terrain under each point)")},
                     {"points", array(Json::object({{"type", "array"}, {"items", Json::object({{"type", "number"}})}}), "[x, z] pairs")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Json out = Json::array();
                 Json stats = Json::object();
                 for (const auto& p : a.get("points").elements()) {
                     float x = p[size_t{0}].asFloat(), z = p[size_t{1}].asFloat(), y = 0;
                     Vec3 n;
                     EntityId which = kNoEntity;
                     if (!engine.world().terrainHeight(engine.scene(), x, z, y, &n, &which)) {
                         out.push(Json::object({{"x", x}, {"z", z}, {"terrain", false}}));
                         continue;
                     }
                     auto data = engine.world().terrain(engine.scene(), which);
                     Vec3 o = engine.scene().worldMatrix(which).translation();
                     int res = data->resolution();
                     int ix = std::clamp(static_cast<int>(std::lround(((x - o.x) / data->size() + 0.5f) * (res - 1))), 0, res - 1);
                     int iz = std::clamp(static_cast<int>(std::lround(((z - o.z) / data->size() + 0.5f) * (res - 1))), 0, res - 1);
                     const uint8_t* w = data->weightsAt(ix, iz);
                     int dom = static_cast<int>(std::max_element(w, w + 8) - w);
                     const Terrain* comp = engine.scene().get<Terrain>(which);
                     std::string layerName = dom < static_cast<int>(comp->layers.size()) ? comp->layers[static_cast<size_t>(dom)].get("name").asString() : "";
                     out.push(Json::object({{"x", x},
                                            {"z", z},
                                            {"height", std::round(y * 100) / 100},
                                            {"slope", std::round(std::acos(std::clamp(n.y, -1.f, 1.f)) * 57.29578f * 10) / 10},
                                            {"normal", reflect::vec3ToJson(n)},
                                            {"layer", layerName},
                                            {"terrain", which}}));
                     stats[std::to_string(which)] = terrainStats(*data);
                 }
                 return ToolResult::json(Json::object({{"points", out}, {"terrains", stats}}), "terrain query");
             }});

    reg.add({"foliage_add", "Add foliage",
             "Grow GPU-instanced foliage and ground detail over a terrain (or over scene meshes in an area): grass, tall "
             "grass, dune grass, flowers, ferns, beach pebbles, shells, small rocks, boulders — or any mesh/asset as a "
             "custom layer (trees, bushes from downloads). Presets: " +
                 [] {
                     std::string out;
                     for (const auto& p : world::foliagePresets()) out += (out.empty() ? "" : ", ") + p;
                     return out;
                 }() +
                 ". Layer fields: mesh, color, density (/m²), scaleMin/Max, slopeMin/Max, heightMin/Max (world y), "
                 "terrainLayer (a terrain layer's index or name, e.g. \"grass\"; only where that layer is painted), wind, cullDistance, castShadows, clumping. Dense "
                 "layers stream in around the camera. Heavy meshes (trees, bushes, rocks) switch to octahedral impostors "
                 "in the distance automatically; tune with impostorDistance (m; 0 auto, -1 off), impostorResolution and "
                 "impostorFrames, or impostors:false. Example: {\"entity\":\"Island\",\"layers\":[{\"preset\":\"dune_grass\","
                 "\"heightMin\":1.5},{\"preset\":\"shells\",\"heightMax\":1.2}]}. Hand-placed instances: a layer with "
                 "`points` draws exactly those (buildings of a distant city, a row of cypresses, lamp posts along a quay) "
                 "with the same instancing, LODs and impostors: [x, y, z, yaw?, scale?] or {position, rotation [pitch, yaw, "
                 "roll], scale (number or [x, y, z]), tint}; `snapToSurface: true` makes y an offset above the ground. "
                 "Example: {\"entity\":\"Terrain\",\"name\":\"Far City\",\"layers\":[{\"prefab\":\"models/house_a.prefab.json\","
                 "\"wind\":0,\"cullDistance\":3000,\"snapToSurface\":true,\"points\":[[40,0,-120,90],[52,0,-118,0,1.2]]}]}.",
             "world",
             object({{"entity", entity("Terrain entity to grow on (foliage becomes its child), or any entity for scene mode")},
                     {"layers", Json::object({{"type", "array"}, {"description", "Layers: {preset, ...overrides} or full custom layers"}})},
                     {"name", string("Foliage entity name (default \"Foliage\")")},
                     {"area", vec3("Scene mode: extent in meters around the entity")},
                     {"seed", integer("Placement seed")}},
                    {"entity", "layers"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto target = resolve(engine, a.get("entity"));
                 if (!target) return ToolResult::error(target.error());
                 for (const auto& l : a.get("layers").elements()) {
                     if (l.contains("preset")) {
                         const auto& ps = world::foliagePresets();
                         std::string p = l.get("preset").asString();
                         if (std::find(ps.begin(), ps.end(), p) == ps.end()) {
                             std::string guess = str::closest(p, ps);
                             return ToolResult::error(Error::make("unknown_preset", "no foliage preset '" + p + "'",
                                                                  guess.empty() ? "" : "did you mean '" + guess + "'?"));
                         }
                     }
                 }
                 {
                     size_t index = 0;
                     for (const auto& l : a.get("layers").elements()) {
                         if (l.contains("points")) {
                             auto pts = world::foliagePointsFromJson(l.get("points"));
                             if (!pts) {
                                 Error err = pts.error();
                                 err.message = "layers[" + std::to_string(index) + "]: " + err.message;
                                 return ToolResult::error(err);
                             }
                         }
                         ++index;
                     }
                 }
                 // terrainLayer: an index or a layer name of the terrain it grows on (did-you-mean on typos).
                 {
                     const Terrain* terrain = engine.scene().get<Terrain>(*target);
                     const std::vector<std::string> names =
                         terrain ? world::terrainLayerNames(terrain->layers) : std::vector<std::string>{};
                     size_t index = 0;
                     for (const auto& l : a.get("layers").elements()) {
                         if (l.contains("terrainLayer") && !l.get("terrainLayer").isNull()) {
                             if (!terrain && !l.get("terrainLayer").isNumber()) {
                                 return ToolResult::error(Error::make(
                                     "invalid_terrain_layer", "layers[" + std::to_string(index) +
                                                                  "].terrainLayer names a terrain layer, but the entity is not a terrain",
                                     "grow the foliage on the terrain entity, or drop terrainLayer"));
                             }
                             auto r = world::resolveTerrainLayer(l.get("terrainLayer"), names);
                             if (!r) {
                                 Error err = r.error();
                                 err.message = "layers[" + std::to_string(index) + "]: " + err.message;
                                 return ToolResult::error(err);
                             }
                         }
                         ++index;
                     }
                 }
                 // Presets that ask for procedural textures get them materialized in the project.
                 Json layers = Json::array();
                 for (const auto& l : a.get("layers").elements()) {
                     Json full = l.contains("preset") ? world::foliagePreset(l.get("preset").asString()) : Json::object();
                     for (const auto& [k, v] : l.members()) full[k] = v;
                     layers.push(full);
                 }
                 layers = materializeLayerTextures(engine, layers);
                 EntityId id = kNoEntity;
                 bool onTerrain = engine.scene().get<Terrain>(*target) != nullptr;
                 Status st = engine.edit(ctx.actor, "Add foliage", [&]() -> Status {
                     Scene& s = engine.scene();
                     id = s.create(a.get("name").asString("Foliage"), *target);
                     Json f = Json::object({{"layers", layers}, {"seed", a.get("seed").asInt(1)},
                                            {"surface", onTerrain ? "terrain" : "scene"}});
                     if (a.contains("area")) f["area"] = a.get("area");
                     return s.patchComponent(id, "foliage", f);
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"entity", id}, {"layers", static_cast<int64_t>(a.get("layers").size())},
                                                       {"surface", onTerrain ? "terrain" : "scene"}}),
                                         "foliage #" + std::to_string(id) + " added");
             }});
}

}  // namespace sky::tools
