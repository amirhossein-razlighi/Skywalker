// Asset, material and prefab tools: how agents discover, inspect, create and reuse content.

#include <filesystem>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"

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
    if (requirePath) s["required"] = Json::array({"path"});
    s.erase("description");
    return s;
}

}  // namespace

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
             "Register a file placed in the project (e.g. by a generator) as an asset. Meshes (.obj/.glb/.gltf) are "
             "imported — glTF colors/textures become a material — and can be placed as a new entity right away.",
             "asset",
             object({{"path", string("Project-relative file path")},
                     {"create_entity", string("If set, create an entity with this name using the mesh")},
                     {"position", vec3("Position for the created entity")},
                     {"description", string("What the asset is (helps future searches)")},
                     {"tags", array(Json::object({{"type", "string"}}), "Tags")}},
                    {"path"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::string path = a.get("path").asString();
                 Json result = Json::object();
                 if (assetTypeForPath(path) == AssetType::Mesh) {
                     auto r = engine.importMeshAsset(path);
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
                         id = engine.scene().create(a.get("create_entity").asString());
                         Json m = Json::object({{"mesh", result.get("mesh")}});
                         if (result.contains("material")) m["material"] = result.get("material");
                         Status s = engine.scene().patchComponent(id, "mesh", m);
                         if (s && a.contains("position")) {
                             s = engine.scene().patchComponent(id, "transform", Json::object({{"position", a.get("position")}}));
                         }
                         return s;
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
                 auto m = materialFromJson(fields);
                 if (!m) return ToolResult::error(m.error());
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
                 if (Status s = reflect::applyJson(&updated, MaterialAsset::type(), fields); !s) return fail(s);
                 if (Status s = saveMaterial(full, updated); !s) return fail(s);
                 engine.refreshAssets();
                 return ToolResult::json(materialToJson(updated), "updated " + a.get("path").asString());
             }});

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
                     if (a.get("on_surface").asBool()) return dropToSurface(engine, root);
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"entity", root}}), "placed " + describe(engine.scene(), root));
             }});
}

}  // namespace sky::tools
