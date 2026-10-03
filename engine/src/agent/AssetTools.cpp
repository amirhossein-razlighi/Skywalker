// Asset, material and prefab tools: how agents discover, inspect, create and reuse content.

#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/TextureGen.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

std::string line(const AssetRecord& r) {
    std::ostringstream os;
    os << r.path << "  [" << toString(r.type) << "]";
    if (!r.tags.empty()) {
        os << " tags:";
        for (const auto& t : r.tags) os << " " << t;
    }
    if (!r.description.empty()) os << " — " << r.description.substr(0, 120);
    if (r.source.contains("prompt")) os << "  (generated: \"" << r.source.get("prompt").asString().substr(0, 60) << "\")";
    return os.str();
}

Result<const AssetRecord*> findAsset(Engine& engine, const Json& ref) {
    if (const AssetRecord* r = engine.assets().find(ref.asString())) return r;
    engine.refreshAssets();
    if (const AssetRecord* r = engine.assets().find(ref.asString())) return r;
    std::vector<std::string> paths;
    for (const auto* r : engine.assets().query({})) paths.push_back(r->path);
    std::string guess = str::closest(ref.asString(), paths, 6);
    return Error::make("not_found", "no asset " + ref.dump(),
                       guess.empty() ? "use asset_list to see project assets" : "did you mean \"" + guess + "\"?");
}

Json materialSchema(bool requirePath) {
    Json s = reflect::schema(MaterialAsset::type());
    s["properties"]["path"] = string("Project-relative path ending in .mat.json, e.g. materials/stone.mat.json");
    Json presets = Json::array();
    for (const auto& p : materialPresets()) presets.push(p);
    s["properties"]["preset"] = Json::object({{"type", "string"}, {"enum", presets},
                                              {"description", "Start from a built-in material (other fields override it)"}});
    if (requirePath) s["required"] = Json::array({"path"});
    s.erase("description");
    return s;
}

}  // namespace

Status placeImportedMesh(Engine& engine, const Json& imported, const std::string& name, const Json& position, EntityId& out) {
    if (imported.contains("prefab")) {
        PrefabPlacement pl;
        pl.name = name;
        if (position.isArray() && position.size() == 3) {
            pl.hasPosition = true;
            pl.position = {position[0].asFloat(), position[1].asFloat(), position[2].asFloat()};
        }
        auto id = engine.instantiatePrefabAsset(imported.get("prefab").asString(), pl);
        if (!id) return id.error();
        out = *id;
        return {};
    }
    out = engine.scene().create(name);
    Json m = Json::object({{"mesh", imported.get("mesh")}});
    if (imported.contains("material")) m["material"] = imported.get("material");
    if (imported.get("vertexColors").asBool()) m["color"] = "#ffffff";
    Status s = engine.scene().patchComponent(out, "mesh", m);
    if (s && position.isArray()) s = engine.scene().patchComponent(out, "transform", Json::object({{"position", position}}));
    return s;
}

void addAssetTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"asset_list", "List assets",
             "Search the project's assets (meshes, textures, materials, prefabs, scenes, audio...) by type, tag or "
             "text (matches path, description and tags; supports * globs). Each line shows path, type, tags, "
             "description and, for generated assets, the prompt that made them.",
             "asset",
             object({{"type", enumeration({"mesh", "texture", "material", "prefab", "scene", "audio", "video", "script", "agent"},
                                          "Filter by asset type")},
                     {"tag", string("Required tag")},
                     {"query", string("Text or glob, e.g. \"tree\" or \"props/*.glb\"")},
                     {"limit", integer("Max results (default 60)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 AssetDatabase::Query q;
                 q.type = assetTypeFromString(a.get("type").asString());
                 q.tag = a.get("tag").asString();
                 q.text = a.get("query").asString();
                 q.limit = static_cast<size_t>(a.get("limit").asInt(60));
                 auto found = engine.assets().query(q);
                 std::string out;
                 Json list = Json::array();
                 for (const auto* r : found) {
                     out += line(*r) + "\n";
                     Json item = r->toJson();
                     item["rev"] = static_cast<double>(r->mtime);  // changes when the file does (thumbnail caches)
                     list.push(std::move(item));
                 }
                 ToolResult res = ToolResult::text(out.empty() ? "no matching assets" : out);
                 res.structured = Json::object({{"assets", list}, {"total", engine.assets().size()}});
                 return res;
             }});

    reg.add({"asset_info", "Asset details",
             "Everything about one asset: GUID, type, tags, description, provenance, import settings, which "
             "entities use it, and type-specific data (material fields, prefab contents).",
             "asset", object({{"asset", string("Path, \"asset:path\" or \"guid:...\"")}}, {"asset"}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 auto r = findAsset(engine, a.get("asset"));
                 if (!r) return ToolResult::error(r.error());
                 Json j = (*r)->toJson();
                 Json usage = Json::array();
                 for (EntityId e : engine.assetUsage((*r)->path)) usage.push(briefJson(engine.scene(), e));
                 j["usedBy"] = usage;
                 if ((*r)->type == AssetType::Material) {
                     if (auto m = loadMaterial(engine.resolvePath((*r)->path))) j["material"] = materialToJson(*m);
                 } else if ((*r)->type == AssetType::Prefab) {
                     if (auto p = engine.loadPrefabAsset((*r)->path)) {
                         size_t count = 0;
                         std::function<void(const Json&)> walk = [&](const Json& n) {
                             ++count;
                             for (const auto& c : n.get("children").elements()) walk(c);
                         };
                         walk(p->get("root"));
                         j["prefab"] = Json::object({{"root", p->get("root").get("name")}, {"entities", count}});
                     }
                 }
                 return ToolResult::json(j);
             }});

    reg.add({"asset_preview", "Look at an asset",
             "Render an isolated preview image of a mesh, material (on a sphere), texture or prefab — look before "
             "you place it.",
             "asset", object({{"asset", string("Asset path or guid")}, {"size", integer("Image size in pixels (default 384)")}}, {"asset"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto r = findAsset(engine, a.get("asset"));
                 if (!r) return ToolResult::error(r.error());
                 auto img = engine.assetPreview((*r)->path, static_cast<int>(std::clamp<int64_t>(a.get("size").asInt(384), 64, 1024)));
                 if (!img) return ToolResult::error(img.error());
                 std::vector<uint8_t> png = encodePng(*img);
                 ToolResult res = ToolResult::text("preview of " + (*r)->path);
                 res.structured = (*r)->toJson();
                 res.image(str::base64Encode(png.data(), png.size()));
                 return res;
             }});

    reg.add({"asset_import", "Import asset",
             "Register a file placed in the project (e.g. by a generator) as an asset. Meshes (.glb/.gltf/.obj/.ply/.stl) "
             "are imported — glTF / OBJ+MTL materials become material assets, PLY/OBJ vertex colors are kept — and "
             "can be placed as a new entity right away. To fetch models from the web use asset_download.",
             "asset",
             object({{"path", string("Project-relative file path")},
                     {"create_entity", string("If set, create an entity with this name using the mesh")},
                     {"position", vec3("Position for the created entity")},
                     {"description", string("What the asset is (helps future searches)")},
                     {"tags", array(Json::object({{"type", "string"}}), "Tags")},
                     {"normalize", boolean("Scale meshes to fit 1 m (default true); false keeps real-world units")},
                     {"z_up", boolean("The mesh is Z-up (CAD, scans, some exporters) — rotate to Y-up")}},
                    {"path"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::string path = a.get("path").asString();
                 Json result = Json::object();
                 if (assetTypeForPath(path) == AssetType::Mesh) {
                     Engine::MeshImportOptions opts;
                     opts.normalize = a.get("normalize").asBool(true);
                     opts.zUp = a.get("z_up").asBool(false);
                     auto r = engine.importMeshAsset(path, opts);
                     if (!r) return ToolResult::error(r.error());
                     result = r.value();
                 } else {
                     auto r = engine.assets().registerFile(engine.resolvePath(path));
                     if (!r) return ToolResult::error(r.error());
                 }
                 auto rec = engine.assets().find(engine.assets().relative(engine.resolvePath(path)));
                 if (!rec) return ToolResult::error(Error::make("internal", "asset was not registered"));
                 Json meta = Json::object();
                 if (a.contains("description")) meta["description"] = a.get("description");
                 if (a.contains("tags")) meta["tags"] = a.get("tags");
                 if (meta.size()) (void)engine.assets().updateMeta(rec->path, meta);
                 if (a.contains("create_entity") && rec->type == AssetType::Mesh) {
                     EntityId id = kNoEntity;
                     Status st = engine.edit(ctx.actor, "Place " + a.get("create_entity").asString(), [&]() -> Status {
                         return placeImportedMesh(engine, result, a.get("create_entity").asString(), a.get("position"), id);
                     });
                     if (!st) return fail(st);
                     result["entity"] = id;
                 }
                 result["asset"] = engine.assets().find(rec->path)->toJson();
                 return ToolResult::json(result, "imported " + rec->path);
             }});

    reg.add({"asset_tag", "Describe & tag asset",
             "Set an asset's tags and/or description (and provenance). Good descriptions make assets findable by "
             "other agents later.",
             "asset",
             object({{"asset", string("Asset path or guid")},
                     {"tags", array(Json::object({{"type", "string"}}), "Replace tags")},
                     {"description", string("One-line description")},
                     {"source", Json::object({{"type", "object"}, {"description", "Provenance fields to merge, e.g. {\"generator\":\"...\",\"prompt\":\"...\"}"}})}},
                    {"asset"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 auto r = findAsset(engine, a.get("asset"));
                 if (!r) return ToolResult::error(r.error());
                 Json patch = a;
                 patch.erase("asset");
                 if (Status s = engine.assets().updateMeta((*r)->path, patch); !s) return fail(s);
                 return ToolResult::json(engine.assets().find((*r)->path)->toJson(), "updated");
             }});

    reg.add({"asset_move", "Move / rename asset",
             "Rename or move an asset file (its .meta and GUID move with it) and update every reference in the open "
             "scene. The file move itself is not undoable — move it back with another asset_move.",
             "asset", object({{"asset", string("Asset path or guid")}, {"to", string("New project-relative path (same extension)")}}, {"asset", "to"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto r = findAsset(engine, a.get("asset"));
                 if (!r) return ToolResult::error(r.error());
                 std::string from = (*r)->path;
                 std::string to = engine.assets().relative(engine.resolvePath(a.get("to").asString()));
                 if (Status s = engine.assets().move(from, to); !s) return fail(s);
                 size_t n = 0;
                 Status st = engine.edit(ctx.actor, "Move asset " + from, [&]() -> Status {
                     n = engine.rewriteAssetReferences(from, to);
                     return {};
                 });
                 engine.refreshAssets();
                 if (!st) return fail(st);
                 return ToolResult::text("moved " + from + " -> " + to + " (" + std::to_string(n) + " references updated)");
             }});

    reg.add({"asset_refresh", "Rescan assets",
             "Rescan the project folder for new or changed files (the editor also does this automatically).",
             "asset", object({}), false, false, [&engine](const Json&, ToolContext&) {
                 auto changed = engine.refreshAssets();
                 Json arr = Json::array();
                 for (const auto& c : changed) arr.push(c);
                 return ToolResult::json(Json::object({{"changed", arr}, {"total", engine.assets().size()}}),
                                         std::to_string(changed.size()) + " new/changed assets");
             }});

    reg.add({"material_create", "Create material",
             "Create a reusable material asset (*.mat.json): color, metallic, roughness, emissive (glows with bloom), "
             "albedo texture with tiling, unlit. Assign it with material_assign.",
             "asset", materialSchema(true), true, false, [&engine](const Json& a, ToolContext&) {
                 std::string path = a.get("path").asString();
                 if (assetTypeForPath(path) != AssetType::Material) {
                     return ToolResult::error(Error::make("invalid_arguments", "material path must end with .mat.json"));
                 }
                 Json fields = a;
                 fields.erase("path");
                 fields.erase("preset");
                 MaterialAsset base;
                 if (a.contains("preset")) {
                     auto p = materialPreset(a.get("preset").asString());
                     if (!p) return ToolResult::error(p.error());
                     base = *p;
                 }
                 Result<MaterialAsset> m = base;
                 if (Status st = reflect::applyJson(&m.value(), MaterialAsset::type(), fields); !st) return fail(st);
                 std::string full = engine.resolvePath(path);
                 std::error_code ec;
                 if (fs::exists(full, ec)) {
                     return ToolResult::error(Error::make("exists", path + " already exists", "use material_update"));
                 }
                 fs::create_directories(fs::path(full).parent_path(), ec);
                 if (Status s = saveMaterial(full, *m); !s) return fail(s);
                 engine.refreshAssets();
                 return ToolResult::json(materialToJson(*m), "created " + path);
             }});

    reg.add({"material_update", "Update material",
             "Change fields of an existing material; every entity using it updates instantly.", "asset",
             materialSchema(true), true, false, [&engine](const Json& a, ToolContext&) {
                 std::string full = engine.resolvePath(a.get("path").asString());
                 auto m = loadMaterial(full);
                 if (!m) return ToolResult::error(m.error());
                 Json fields = a;
                 fields.erase("path");
                 MaterialAsset updated = *m;
                 if (a.contains("preset")) {
                     auto p = materialPreset(a.get("preset").asString());
                     if (!p) return ToolResult::error(p.error());
                     updated = *p;
                     fields.erase("preset");
                 }
                 if (Status s = reflect::applyJson(&updated, MaterialAsset::type(), fields); !s) return fail(s);
                 if (Status s = saveMaterial(full, updated); !s) return fail(s);
                 engine.refreshAssets();
                 return ToolResult::json(materialToJson(updated), "updated " + a.get("path").asString());
             }});

    {
        Json kinds = Json::array();
        for (const auto& k : texgen::kinds()) kinds.push(k);
        reg.add({"texture_generate", "Generate PBR texture",
                 "Procedurally generate a seamless PBR texture set — albedo, normal map and ORM (occlusion/roughness/"
                 "metallic) — for realistic or stylized surfaces: bricks, planks, cobblestone, rock, rust, marble, "
                 "fabric, scales, ... With create_material it also writes a ready material (triplanar by default, so it "
                 "works on scaled primitives without stretching). Colors override the kind's defaults. The soft kinds "
                 "`glow` (radial) and `curtain` (vertical rays fading upward) have transparent edges and make self-lit "
                 "effect materials: particles, light pools, fake reflections, auroras, light shafts, steam.",
                 "asset",
                 object({{"kind", Json::object({{"type", "string"}, {"enum", kinds}, {"description", "Pattern"}})},
                         {"name", string("Base path without extension, e.g. textures/old_bricks")},
                         {"size", integer("Pixels, power of two 64..2048 (default 512)")},
                         {"seed", integer("Variation seed (default 1)")},
                         {"scale", number("Feature count across the tile (kind default if omitted)")},
                         {"color1", string("Primary color #rrggbb")},
                         {"color2", string("Secondary color #rrggbb")},
                         {"color3", string("Accent color (mortar, grout, veins, rust) #rrggbb")},
                         {"roughness", number("Base roughness 0..1")},
                         {"metallic", number("Metallic 0..1")},
                         {"variation", number("Color / height variation 0..1")},
                         {"bump", number("Normal strength (default 1)")},
                         {"create_material", boolean("Also create materials/<name>.mat.json using the maps (default true)")},
                         {"tiling", number("Material repeats per meter (triplanar) — default 0.5")},
                         {"glow", number("Emission strength for the soft effect kinds glow / curtain (default 1.5)")}},
                        {"kind", "name"}),
                 true, false, [&engine](const Json& a, ToolContext& ctx) {
                     texgen::Params p = texgen::defaults(a.get("kind").asString());
                     p.kind = a.get("kind").asString();
                     p.size = static_cast<int>(a.get("size").asInt(512));
                     p.seed = static_cast<uint32_t>(a.get("seed").asInt(1));
                     if (a.contains("scale")) p.scale = a.get("scale").asFloat();
                     for (auto [key, dst] : {std::pair{"color1", &p.color1}, std::pair{"color2", &p.color2}, std::pair{"color3", &p.color3}}) {
                         if (a.contains(key) && !reflect::jsonToColor(a.get(key), *dst)) {
                             return ToolResult::error(Error::make("invalid_arguments", std::string(key) + " must be a color like #aa7744"));
                         }
                     }
                     if (a.contains("roughness")) p.roughness = a.get("roughness").asFloat();
                     if (a.contains("metallic")) p.metallic = a.get("metallic").asFloat();
                     if (a.contains("variation")) p.variation = a.get("variation").asFloat();
                     if (a.contains("bump")) p.bump = a.get("bump").asFloat();
                     auto set = texgen::generate(p);
                     if (!set) return ToolResult::error(set.error());
                     std::string base = a.get("name").asString();
                     for (const char* ext : {".png", ".jpg"}) {
                         if (str::lower(base).size() > 4 && str::lower(base).rfind(ext) == base.size() - 4) base = base.substr(0, base.size() - 4);
                     }
                     std::string albedo = base + "_albedo.png", normal = base + "_normal.png", orm = base + "_orm.png";
                     std::error_code ec;
                     fs::create_directories(fs::path(engine.resolvePath(albedo)).parent_path(), ec);
                     for (auto [path, img] : {std::pair{albedo, &set->albedo}, std::pair{normal, &set->normal}, std::pair{orm, &set->orm}}) {
                         if (Status st = writePng(*img, engine.resolvePath(path)); !st) return fail(st);
                     }
                     engine.refreshAssets();
                     Json source = Json::object({{"generator", "texgen"}, {"kind", p.kind}, {"seed", static_cast<int64_t>(p.seed)}, {"by", ctx.actor}});
                     Json result = Json::object({{"albedo", albedo}, {"normal", normal}, {"orm", orm}});
                     for (const auto& path : {albedo, normal, orm}) {
                         if (auto rec = engine.assets().registerFile(engine.resolvePath(path))) {
                             (void)engine.assets().updateMeta((*rec)->path, Json::object({{"source", source}, {"tags", Json::array({"generated", p.kind})}}));
                         }
                     }
                     if (a.get("create_material").asBool(true)) {
                         MaterialAsset m;
                         m.color = {1, 1, 1, 1};
                         m.texture = albedo;
                         m.normalMap = normal;
                         m.ormMap = orm;
                         m.roughness = 1.f;
                         m.metallic = 1.f;  // the ORM map carries the real values
                         m.triplanar = true;
                         m.tilingU = m.tilingV = a.get("tiling").asFloat(0.5f);
                         if (p.kind == "glow" || p.kind == "curtain") {
                             // Effect material: soft alpha, self-lit, visible from both sides.
                             m = MaterialAsset{};
                             m.texture = albedo;
                             m.emissiveMap = albedo;
                             m.color = {1, 1, 1, 0.99f};
                             m.emissive = {1, 1, 1, a.get("glow").asFloat(1.5f)};
                             m.unlit = true;
                             m.doubleSided = true;
                         }
                         std::string stem = fs::path(base).filename().string();
                         std::string matPath = "materials/" + stem + ".mat.json";
                         fs::create_directories(fs::path(engine.resolvePath(matPath)).parent_path(), ec);
                         if (Status st = saveMaterial(engine.resolvePath(matPath), m); !st) return fail(st);
                         engine.refreshAssets();
                         (void)engine.assets().updateMeta(matPath, Json::object({{"source", source}, {"description", p.kind + " (procedural PBR)"}}));
                         result["material"] = matPath;
                     }
                     return ToolResult::json(result, "generated " + p.kind + " texture set" +
                                                         (result.contains("material") ? " and " + result.get("material").asString() : ""));
                 }});
    }

    reg.add({"material_assign", "Assign material",
             "Use a material asset on one or more entities (sets mesh.material; empty string clears it).", "asset",
             object({{"entities", array(entity(), "Entities")}, {"material", string("Material path (*.mat.json) or \"\"")}},
                    {"entities", "material"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::string mat = a.get("material").asString();
                 if (!mat.empty() && !engine.resolveMaterial(mat)) {
                     return ToolResult::error(Error::make("not_found", "material " + mat + " not found or invalid",
                                                          "list materials with asset_list type=material"));
                 }
                 size_t n = 0;
                 Status st = engine.edit(ctx.actor, "Assign material", [&]() -> Status {
                     for (const auto& e : a.get("entities").elements()) {
                         auto id = resolve(engine, e);
                         if (!id) return id.error();
                         if (Status s = engine.scene().patchComponent(*id, "mesh", Json::object({{"material", mat}})); !s) return s;
                         ++n;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::text("assigned " + (mat.empty() ? "no material" : mat) + " to " + std::to_string(n) + " entities");
             }});

    reg.add({"prefab_create", "Save prefab",
             "Save an entity and all its children (components, behaviors, vars, tags) as a reusable prefab asset. "
             "Instantiate it with prefab_instantiate, scatter, or Wander spawn(\"prefab:path\").",
             "asset",
             object({{"entity", entity()},
                     {"path", string("Project-relative path ending in .prefab.json, e.g. prefabs/cottage.prefab.json")},
                     {"description", string("What it is")},
                     {"tags", array(Json::object({{"type", "string"}}), "Tags")}},
                    {"entity", "path"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto id = resolve(engine, a.get("entity"));
                 if (!id) return ToolResult::error(id.error());
                 std::string path = a.get("path").asString();
                 if (assetTypeForPath(path) != AssetType::Prefab) {
                     return ToolResult::error(Error::make("invalid_arguments", "prefab path must end with .prefab.json"));
                 }
                 Json prefab = prefabFromEntity(engine.scene(), *id);
                 std::string full = engine.resolvePath(path);
                 std::error_code ec;
                 fs::create_directories(fs::path(full).parent_path(), ec);
                 if (Status s = savePrefab(full, prefab); !s) return fail(s);
                 engine.refreshAssets();
                 Json meta = Json::object({{"source", Json::object({{"by", ctx.actor}, {"fromEntity", engine.scene().record(*id)->name}})}});
                 if (a.contains("description")) meta["description"] = a.get("description");
                 if (a.contains("tags")) meta["tags"] = a.get("tags");
                 (void)engine.assets().updateMeta(engine.assets().relative(full), meta);
                 return ToolResult::text("saved prefab " + path);
             }});

    reg.add({"prefab_instantiate", "Place prefab",
             "Create an instance of a prefab. Optionally drop it onto the surface below its position.", "asset",
             object({{"prefab", string("Prefab path")},
                     {"position", vec3("World position (default origin)")},
                     {"yaw", number("Rotation around Y in degrees")},
                     {"rotation", vec3("Full rotation in degrees (x, y, z); overrides yaw")},
                     {"scale", number("Uniform scale multiplier (default 1)")},
                     {"parent", entity("Parent entity")},
                     {"name", string("Name for the instance root")},
                     {"on_surface", boolean("Drop onto the geometry below (default false)")}},
                    {"prefab"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 PrefabPlacement p;
                 p.hasPosition = reflect::jsonToVec3(a.get("position"), p.position);
                 p.hasYaw = a.contains("yaw");
                 p.yaw = a.get("yaw").asFloat();
                 p.scale = a.get("scale").asFloat(1.f);
                 p.name = a.get("name").asString();
                 if (a.contains("parent")) {
                     auto parent = resolve(engine, a.get("parent"));
                     if (!parent) return ToolResult::error(parent.error());
                     p.parent = *parent;
                 }
                 EntityId root = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Place prefab", [&]() -> Status {
                     auto r = engine.instantiatePrefabAsset(a.get("prefab").asString(), p);
                     if (!r) return r.error();
                     root = *r;
                     if (a.contains("rotation")) {
                         if (Status s = engine.scene().patchComponent(root, "transform", Json::object({{"rotation", a.get("rotation")}})); !s) {
                             return s;
                         }
                     }
                     if (a.get("on_surface").asBool()) return dropToSurface(engine, root);
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"entity", root}}), "placed " + describe(engine.scene(), root));
             }});
}

}  // namespace sky::tools
