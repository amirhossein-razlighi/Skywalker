// 2D tools: sprite atlases and sheets, tilemap painting, ASCII levels and inspection.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Image.h"
#include "skywalker/render2d/Atlas.h"
#include "skywalker/render2d/ImageIO.h"
#include "skywalker/render2d/Tilemap.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

/// "run_2" < "run_10": compares digit runs numerically.
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            unsigned long long x = std::stoull(a.substr(i, std::min<size_t>(i2 - i, 18)));
            unsigned long long y = std::stoull(b.substr(j, std::min<size_t>(j2 - j, 18)));
            if (x != y) return x < y;
            i = i2;
            j = j2;
        } else {
            char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))), y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
            if (x != y) return x < y;
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

bool isImage(const fs::path& p) {
    std::string e = str::lower(p.extension().string());
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga";
}

/// Clip definitions inferred from frame names: "run_0".."run_7" -> {"run": {"frames": "run_*"}}.
Json inferClips(const std::vector<std::string>& names, float fps) {
    std::map<std::string, int> groups;
    std::vector<std::string> order;
    for (const auto& n : names) {
        size_t end = n.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(n[end - 1]))) --end;
        if (end == n.size() || end == 0) continue;
        std::string base = n.substr(0, end);
        while (!base.empty() && (base.back() == '_' || base.back() == '-' || base.back() == ' ')) base.pop_back();
        if (base.empty()) continue;
        if (!groups.count(base)) order.push_back(base);
        ++groups[base];
    }
    Json clips = Json::object();
    for (const auto& base : order) {
        if (groups[base] < 2) continue;
        // The frames pattern is the first frame's name without its number, plus '*' (e.g. "run_*").
        std::string first;
        for (const auto& n : names) {
            if (str::startsWith(n, base)) {
                first = n;
                break;
            }
        }
        size_t digits = first.size();
        while (digits > 0 && std::isdigit(static_cast<unsigned char>(first[digits - 1]))) --digits;
        clips[base] = Json::object({{"frames", first.substr(0, digits) + "*"}, {"fps", fps}, {"loop", true}});
    }
    return clips;
}

Result<std::string> projectRelative(Engine& engine, const std::string& path) {
    std::string rel = engine.assets().relative(engine.resolvePath(path));
    if (rel.empty()) return Error::make("invalid_path", "path must be inside the project: " + path);
    return rel;
}

Status writeText(const std::string& full, const std::string& textOut) {
    std::error_code ec;
    fs::create_directories(fs::path(full).parent_path(), ec);
    std::ofstream f(full);
    if (!f) return Error::make("io_error", "cannot write " + full);
    f << textOut;
    return {};
}

/// Terrains of a tilemap: its tileset's plus its own autotile rules.
Result<std::map<std::string, tiles::Terrain>> terrainsFor(Engine& engine, const Tilemap& tm, int& tileCount,
                                                          std::map<std::string, uint32_t>& names) {
    Json merged = Json::object();
    tileCount = 0;
    if (!tm.tileset.empty()) {
        auto ts = engine.world2d().assets().tileset(tm.tileset, tm.tileSize);
        if (ts) {
            tileCount = ts->count;
            names = ts->names;
            for (const auto& [k, v] : ts->terrains.members()) merged[k] = v;
        }
    }
    for (const auto& [k, v] : tm.autotile.members()) merged[k] = v;
    return tiles::parseTerrains(merged, tileCount);
}

Result<uint32_t> tileArg(const Json& tile, const std::map<std::string, tiles::Terrain>& terrains, const std::map<std::string, uint32_t>& names,
                         const tiles::Terrain*& terrain) {
    terrain = nullptr;
    if (tile.isNumber()) return static_cast<uint32_t>(std::max(0.0, tile.asNumber()));
    if (tile.isString()) {
        const std::string& t = tile.asString();
        if (auto it = terrains.find(t); it != terrains.end()) {
            terrain = &it->second;
            return uint32_t{0};
        }
        if (auto it = names.find(t); it != names.end()) return it->second;
        std::vector<std::string> all;
        for (const auto& [k, v] : terrains) all.push_back(k);
        for (const auto& [k, v] : names) all.push_back(k);
        std::string guess = str::closest(t, all, 3);
        return Error::make("invalid_arguments", "unknown tile or terrain \"" + t + "\"",
                           guess.empty() ? "use a tile id, a terrain from tilemap.autotile, or a tileset tile name" : "did you mean \"" + guess + "\"?");
    }
    return Error::make("invalid_arguments", "tile must be an id or a terrain/tile name");
}

Json gridSummary(const tiles::Grid& g, size_t layer, std::map<uint32_t, char>& symbols) {
    Json legend = Json::object();
    std::map<uint32_t, int> counts;
    for (uint32_t c : g.layers[layer].cells) {
        if (uint32_t id = c & tiles::kIdMask) ++counts[id];
    }
    static const char kSymbols[] = "#abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t next = 0;
    for (const auto& [id, n] : counts) {
        if (next >= sizeof(kSymbols) - 1) break;
        char ch = kSymbols[next++];
        symbols[id] = ch;
        legend[std::string(1, ch)] = Json::object({{"tile", id}, {"count", n}});
    }
    return legend;
}

}  // namespace

void addTools2D(Engine& engine, ToolRegistry& reg) {
    reg.add({"sprite_atlas_pack", "Pack sprite atlas",
             "Pack many images (a folder, a list, or globs like \"sprites/hero/*.png\") into one atlas texture plus a "
             "*.atlas.json describing every frame (trimmed of transparent borders, padded and edge-extruded so nothing bleeds). "
             "Frames are named after their files and keep natural order (run_2 before run_10). Sprites then use "
             "{\"texture\": \"<atlas>.atlas.json\", \"frame\": \"run_3\"}; the result suggests sprite_anim clips from numbered "
             "names (run_0..run_7 -> \"run\"). Example: {\"folder\": \"art/hero\", \"output\": \"art/hero.atlas.json\"}",
             "asset",
             object({{"folder", string("Folder of images (non-recursive)")},
                     {"inputs", array(Json::object({{"type", "string"}}), "Image paths or globs (project-relative)")},
                     {"output", string("Atlas JSON path (default: <folder>.atlas.json)")},
                     {"padding", integer("Pixels between frames (default 2)")},
                     {"trim", boolean("Trim transparent borders (default true)")},
                     {"extrude", integer("Edge pixels repeated into the padding (default 1)")},
                     {"max_size", integer("Maximum atlas size (default 4096)")},
                     {"power_of_two", boolean("Power-of-two atlas size")},
                     {"fps", number("Frame rate for suggested clips (default 10)")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 std::vector<std::string> files;
                 auto addPattern = [&](const std::string& pattern) -> Status {
                     std::string full = engine.resolvePath(pattern);
                     if (pattern.find_first_of("*?") == std::string::npos) {
                         if (!fs::is_regular_file(full)) return Error::make("not_found", "no image " + pattern);
                         files.push_back(full);
                         return {};
                     }
                     fs::path dir = fs::path(full).parent_path();
                     std::string filePattern = fs::path(full).filename().string();
                     std::error_code ec;
                     for (const auto& entry : fs::directory_iterator(dir, ec)) {
                         if (entry.is_regular_file() && isImage(entry.path()) && str::globMatch(filePattern, entry.path().filename().string())) {
                             files.push_back(entry.path().string());
                         }
                     }
                     return {};
                 };
                 std::string defaultOut;
                 if (a.contains("folder")) {
                     std::string folder = a.get("folder").asString();
                     while (!folder.empty() && folder.back() == '/') folder.pop_back();
                     if (Status s = addPattern(folder + "/*"); !s) return fail(s);
                     defaultOut = folder + ".atlas.json";
                 }
                 for (const auto& p : a.get("inputs").elements()) {
                     if (Status s = addPattern(p.asString()); !s) return fail(s);
                 }
                 std::string out = a.get("output").asString(defaultOut);
                 if (out.empty()) return ToolResult::error(Error::make("invalid_arguments", "pass output (e.g. art/hero.atlas.json)"));
                 auto outRel = projectRelative(engine, out);
                 if (!outRel) return ToolResult::error(outRel.error());
                 std::sort(files.begin(), files.end(), [](const std::string& x, const std::string& y) {
                     return naturalLess(fs::path(x).filename().string(), fs::path(y).filename().string());
                 });
                 files.erase(std::unique(files.begin(), files.end()), files.end());
                 // Never pack the atlas's own previous output.
                 std::string imageRel = outRel.value().substr(0, outRel->size() - 5) + ".png";
                 std::string imageFull = engine.resolvePath(imageRel);
                 std::erase_if(files, [&](const std::string& f) { return fs::path(f).lexically_normal() == fs::path(imageFull).lexically_normal(); });
                 if (files.empty()) return ToolResult::error(Error::make("not_found", "no images matched", "check folder/inputs; formats: png, jpg, bmp, tga"));
                 std::vector<render2d::PackInput> inputs;
                 for (const auto& f : files) {
                     auto img = render2d::loadImage(f);
                     if (!img) return ToolResult::error(img.error());
                     inputs.push_back({fs::path(f).stem().string(), std::move(img.value())});
                 }
                 render2d::PackOptions o;
                 o.padding = static_cast<int>(std::clamp<int64_t>(a.get("padding").asInt(2), 0, 64));
                 o.trim = a.get("trim").asBool(true);
                 o.extrude = static_cast<int>(std::clamp<int64_t>(a.get("extrude").asInt(1), 0, 8));
                 o.maxSize = static_cast<int>(std::clamp<int64_t>(a.get("max_size").asInt(4096), 16, 16384));
                 o.powerOfTwo = a.get("power_of_two").asBool(false);
                 auto packed = render2d::packAtlas(inputs, o);
                 if (!packed) return ToolResult::error(packed.error());
                 packed->atlas.image = fs::path(imageRel).filename().string();
                 if (Status s = writePng(packed->image, imageFull); !s) return fail(s);
                 if (Status s = writeText(engine.resolvePath(outRel.value()), packed->atlas.toJson().dump(2) + "\n"); !s) return fail(s);
                 engine.world2d().invalidate(engine.resolvePath(outRel.value()));
                 engine.world2d().invalidate(imageFull);
                 engine.refreshAssets();
                 std::vector<std::string> names;
                 Json frames = Json::array();
                 for (const auto& f : packed->atlas.frames) {
                     names.push_back(f.name);
                     frames.push(Json::object({{"name", f.name}, {"rect", Json::array({f.x, f.y, f.w, f.h})}}));
                 }
                 Json clips = inferClips(names, a.get("fps").asFloat(10.f));
                 Json r = Json::object({{"atlas", outRel.value()}, {"image", imageRel}, {"width", packed->atlas.width},
                                        {"height", packed->atlas.height}, {"frames", frames}, {"suggestedClips", clips}});
                 return ToolResult::json(r, "packed " + std::to_string(frames.size()) + " frames into " + outRel.value() + " (" +
                                                std::to_string(packed->atlas.width) + "x" + std::to_string(packed->atlas.height) + ")");
             }});

    reg.add({"sprite_sheet_slice", "Slice sprite sheet",
             "Describe a grid sprite sheet as an atlas (*.atlas.json) so frames get names: give the cell size (or columns/rows), "
             "optional margin/spacing, and either names or a prefix. `animations` turns rows (or index ranges) into sprite_anim "
             "clips, and `entity` applies the sprite and its animations to an entity in one step. Example: {\"image\": "
             "\"art/knight.png\", \"cell\": [32, 32], \"animations\": {\"idle\": {\"row\": 0, \"fps\": 6}, \"run\": {\"row\": 1, "
             "\"fps\": 12}, \"attack\": {\"frames\": \"16-21\", \"fps\": 14, \"loop\": false}}, \"entity\": \"Knight\"}",
             "asset",
             object({{"image", string("Sheet image (project-relative)")},
                     {"cell", Json::object({{"type", "array"}, {"items", Json::object({{"type", "integer"}})}, {"description", "Cell size [w, h] in pixels"}})},
                     {"columns", integer("Columns (instead of cell)")},
                     {"rows", integer("Rows (instead of cell)")},
                     {"margin", integer("Border around the grid (pixels)")},
                     {"spacing", integer("Gap between cells (pixels)")},
                     {"prefix", string("Frame name prefix (default: image name + \"_\")")},
                     {"names", array(Json::object({{"type", "string"}}), "Frame names in order (left->right, top->bottom)")},
                     {"skip_empty", boolean("Drop fully transparent cells (default true)")},
                     {"output", string("Atlas path (default: <image>.atlas.json)")},
                     {"animations", Json::object({{"type", "object"},
                                                  {"description", "clip -> {row | frames, fps, loop, events}"}})},
                     {"entity", entity("Apply: sprite (atlas) + sprite_anim (clips) on this entity")},
                     {"pixels_per_unit", number("With entity: sprite pixelsPerUnit (default: cell height, i.e. 1 cell = 1 unit)")}},
                    {"image"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto imageRel = projectRelative(engine, a.get("image").asString());
                 if (!imageRel) return ToolResult::error(imageRel.error());
                 auto img = render2d::loadImage(engine.resolvePath(imageRel.value()));
                 if (!img) return ToolResult::error(img.error());
                 int margin = static_cast<int>(std::max<int64_t>(0, a.get("margin").asInt(0)));
                 int spacing = static_cast<int>(std::max<int64_t>(0, a.get("spacing").asInt(0)));
                 int cw = 0, ch = 0;
                 if (a.get("cell").isArray() && a.get("cell").size() == 2) {
                     cw = static_cast<int>(a.get("cell")[size_t{0}].asInt());
                     ch = static_cast<int>(a.get("cell")[1].asInt());
                 } else if (a.contains("columns") || a.contains("rows")) {
                     int cols = static_cast<int>(std::max<int64_t>(1, a.get("columns").asInt(1)));
                     int rows = static_cast<int>(std::max<int64_t>(1, a.get("rows").asInt(1)));
                     cw = (img->width - 2 * margin - (cols - 1) * spacing) / cols;
                     ch = (img->height - 2 * margin - (rows - 1) * spacing) / rows;
                 }
                 if (cw <= 0 || ch <= 0) return ToolResult::error(Error::make("invalid_arguments", "pass cell [w, h] or columns/rows"));
                 std::vector<std::string> names;
                 for (const auto& n : a.get("names").elements()) names.push_back(n.asString());
                 std::string stem = fs::path(imageRel.value()).stem().string();
                 render2d::Atlas atlas = render2d::sliceGrid(img->width, img->height, cw, ch, margin, spacing, a.get("prefix").asString(stem + "_"), names);
                 const int columns = std::max(1, (img->width - 2 * margin + spacing) / (cw + spacing));
                 // Row/index bookkeeping before dropping empty cells.
                 std::vector<int> cellIndex;
                 for (size_t i = 0; i < atlas.frames.size(); ++i) cellIndex.push_back(static_cast<int>(i));
                 if (a.get("skip_empty").asBool(true)) {
                     std::vector<render2d::AtlasFrame> kept;
                     std::vector<int> keptIndex;
                     for (size_t i = 0; i < atlas.frames.size(); ++i) {
                         const auto& f = atlas.frames[i];
                         bool any = false;
                         for (int y = f.y; y < f.y + f.h && !any; ++y)
                             for (int x = f.x; x < f.x + f.w && !any; ++x) any = img->at(x, y)[3] > 0;
                         if (any) {
                             kept.push_back(f);
                             keptIndex.push_back(cellIndex[i]);
                         }
                     }
                     atlas.frames = std::move(kept);
                     cellIndex = std::move(keptIndex);
                 }
                 if (atlas.frames.empty()) return ToolResult::error(Error::make("invalid_arguments", "no frames: the sheet is empty or the cell is too large"));
                 std::string out = a.get("output").asString(imageRel->substr(0, imageRel->size() - fs::path(imageRel.value()).extension().string().size()) + ".atlas.json");
                 auto outRel = projectRelative(engine, out);
                 if (!outRel) return ToolResult::error(outRel.error());
                 atlas.image = fs::relative(engine.resolvePath(imageRel.value()), fs::path(engine.resolvePath(outRel.value())).parent_path()).generic_string();
                 // Clips from rows or explicit ranges (names, so they survive re-slicing).
                 Json clips = Json::object();
                 for (const auto& [clip, def] : a.get("animations").members()) {
                     Json frames = Json::array();
                     if (def.contains("row")) {
                         int row = static_cast<int>(def.get("row").asInt());
                         for (size_t i = 0; i < atlas.frames.size(); ++i) {
                             if (cellIndex[i] / columns == row) frames.push(atlas.frames[i].name);
                         }
                     } else if (def.contains("frames")) {
                         auto list = render2d::parseFrameList(def.get("frames"), static_cast<int>(atlas.frames.size()), &atlas);
                         if (!list) return ToolResult::error(Error::make(list.error().code, "animation \"" + clip + "\": " + list.error().message, list.error().hint));
                         for (int i : list.value()) frames.push(atlas.frames[static_cast<size_t>(i)].name);
                     }
                     if (frames.size() == 0) return ToolResult::error(Error::make("invalid_arguments", "animation \"" + clip + "\" has no frames", "use {\"row\": n} or {\"frames\": \"0-7\"}"));
                     Json c = Json::object({{"frames", frames}, {"fps", def.get("fps").asFloat(10.f)}, {"loop", def.get("loop").asBool(true)}});
                     if (def.contains("events")) c["events"] = def.get("events");
                     clips[clip] = c;
                 }
                 if (Status s = writeText(engine.resolvePath(outRel.value()), atlas.toJson().dump(2) + "\n"); !s) return fail(s);
                 engine.world2d().invalidate(engine.resolvePath(outRel.value()));
                 engine.refreshAssets();
                 Json result = Json::object({{"atlas", outRel.value()}, {"frames", atlas.frames.size()}, {"cell", Json::array({cw, ch})},
                                             {"firstFrame", atlas.frames.front().name}, {"lastFrame", atlas.frames.back().name}});
                 if (clips.members().size()) result["clips"] = clips;
                 if (a.contains("entity")) {
                     auto e = resolve(engine, a.get("entity"));
                     if (!e) return ToolResult::error(e.error());
                     Status st = engine.edit(ctx.actor, "Sprite sheet " + stem, [&]() -> Status {
                         Json sprite = Json::object({{"texture", outRel.value()}, {"frame", atlas.frames.front().name},
                                                     {"pixelsPerUnit", a.get("pixels_per_unit").asFloat(static_cast<float>(ch))},
                                                     {"filter", ch <= 64 ? "nearest" : "linear"}});
                         if (Status r = engine.scene().patchComponent(*e, "sprite", sprite); !r) return r;
                         if (clips.members().size()) {
                             Json anim = Json::object({{"clips", clips}, {"clip", clips.members().front().first}, {"playing", true}});
                             if (Status r = engine.scene().patchComponent(*e, "sprite_anim", anim); !r) return r;
                         }
                         return {};
                     });
                     if (!st) return fail(st);
                     result["entity"] = *e;
                 }
                 return ToolResult::json(result, "sliced " + std::to_string(atlas.frames.size()) + " frames (" + std::to_string(cw) + "x" +
                                                     std::to_string(ch) + ") into " + outRel.value());
             }});

    reg.add({"tilemap_paint", "Paint tiles",
             "Edit a tilemap layer: set individual cells, fill a rectangle, flood-fill a region, or clear. `tile` is a tile id "
             "(1-based in the tileset, 0 = empty), a terrain from tilemap.autotile (auto-tiled: edges and corners pick the right "
             "tiles), or a tileset tile name. Cells are [column, row] from the top-left. One undo step. Example: {\"entity\": "
             "\"Level\", \"action\": \"fill\", \"rect\": [0, 14, 40, 2], \"tile\": \"ground\"}",
             "world",
             object({{"entity", entity("Tilemap entity")},
                     {"layer", string("Layer name (created if missing; default: the first layer)")},
                     {"action", enumeration({"set", "fill", "flood", "clear"}, "set cells, fill rect, flood at, clear rect/layer")},
                     {"tile", Json::object({{"type", Json::array({"integer", "string"})}, {"description", "Tile id, terrain or tile name"}})},
                     {"cells", array(Json::object({{"type", "array"}, {"items", Json::object({{"type", "integer"}})}}), "[[x, y], ...] for set")},
                     {"rect", Json::object({{"type", "array"}, {"items", Json::object({{"type", "integer"}})}, {"description", "[x, y, w, h] for fill/clear"}})},
                     {"at", Json::object({{"type", "array"}, {"items", Json::object({{"type", "integer"}})}, {"description", "[x, y] for flood/set"}})},
                     {"flip_x", boolean("Mirror placed tiles horizontally")},
                     {"flip_y", boolean("Mirror placed tiles vertically")}},
                    {"entity", "action"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 auto e = resolve(engine, a.get("entity"));
                 if (!e) return ToolResult::error(e.error());
                 Tilemap* tm = engine.scene().get<Tilemap>(*e);
                 if (!tm) return ToolResult::error(Error::make("invalid_arguments", "the entity has no tilemap", "create one with tilemap_from_ascii"));
                 auto grid = tiles::Grid::fromComponent(*tm);
                 if (!grid) return ToolResult::error(grid.error());
                 int tileCount = 0;
                 std::map<std::string, uint32_t> names;
                 auto terrains = terrainsFor(engine, *tm, tileCount, names);
                 if (!terrains) return ToolResult::error(terrains.error());
                 std::string layerName = a.get("layer").asString(grid->layers.empty() ? "ground" : grid->layers[0].name);
                 const size_t layer = static_cast<size_t>(grid->layerIndex(layerName, true));
                 const std::string action = a.get("action").asString();
                 const tiles::Terrain* terrain = nullptr;
                 uint32_t id = 0;
                 if (action != "clear") {
                     if (!a.contains("tile")) return ToolResult::error(Error::make("invalid_arguments", action + " needs tile"));
                     auto t = tileArg(a.get("tile"), terrains.value(), names, terrain);
                     if (!t) return ToolResult::error(t.error());
                     id = t.value();
                     if (!terrain && id > 0) {
                         if (tileCount > 0 && id > static_cast<uint32_t>(tileCount)) {
                             return ToolResult::error(Error::make("invalid_arguments", "tile " + std::to_string(id) + " is beyond the tileset (" +
                                                                                           std::to_string(tileCount) + " tiles)"));
                         }
                         if (a.get("flip_x").asBool(false)) id |= tiles::kFlipX;
                         if (a.get("flip_y").asBool(false)) id |= tiles::kFlipY;
                     }
                 }
                 auto put = [&](int x, int y) {
                     if (!grid->inside(x, y)) return false;
                     if (terrain) grid->paintTerrain(layer, *terrain, x, y, static_cast<uint32_t>(engine.scene().seed));
                     else grid->set(layer, x, y, id);
                     return true;
                 };
                 size_t changed = 0;
                 auto rectArg = [&](int& x, int& y, int& w, int& h) {
                     const Json& r = a.get("rect");
                     if (!r.isArray() || r.size() != 4) return false;
                     x = static_cast<int>(r[size_t{0}].asInt());
                     y = static_cast<int>(r[1].asInt());
                     w = static_cast<int>(r[2].asInt());
                     h = static_cast<int>(r[3].asInt());
                     return true;
                 };
                 if (action == "set") {
                     Json cells = a.get("cells");
                     if (a.get("at").isArray()) cells.push(a.get("at"));
                     if (cells.size() == 0) return ToolResult::error(Error::make("invalid_arguments", "set needs cells [[x, y], ...] or at"));
                     for (const auto& c : cells.elements()) changed += put(static_cast<int>(c[size_t{0}].asInt()), static_cast<int>(c[1].asInt()));
                 } else if (action == "fill" || action == "clear") {
                     int x = 0, y = 0, w = grid->width, h = grid->height;
                     if (!rectArg(x, y, w, h) && action == "fill") return ToolResult::error(Error::make("invalid_arguments", "fill needs rect [x, y, w, h]"));
                     for (int yy = y; yy < y + h; ++yy) {
                         for (int xx = x; xx < x + w; ++xx) {
                             if (action == "clear") {
                                 if (grid->inside(xx, yy)) {
                                     grid->set(layer, xx, yy, 0);
                                     ++changed;
                                 }
                             } else {
                                 changed += put(xx, yy);
                             }
                         }
                     }
                     // Clearing next to a terrain re-tiles its edges.
                     if (action == "clear") {
                         for (const auto& [n, t] : terrains.value()) grid->autotile(layer, t, x - 1, y - 1, x + w, y + h, static_cast<uint32_t>(engine.scene().seed));
                     }
                 } else if (action == "flood") {
                     if (!a.get("at").isArray()) return ToolResult::error(Error::make("invalid_arguments", "flood needs at [x, y]"));
                     int x = static_cast<int>(a.get("at")[size_t{0}].asInt()), y = static_cast<int>(a.get("at")[1].asInt());
                     if (terrain) {
                         uint32_t marker = 0x1FFFFFFEu;
                         changed = grid->flood(layer, x, y, marker);
                         for (int yy = 0; yy < grid->height; ++yy)
                             for (int xx = 0; xx < grid->width; ++xx)
                                 if (grid->get(layer, xx, yy) == marker) grid->set(layer, xx, yy, terrain->mode == "blob47" ? terrain->tiles[46] : terrain->mode == "wang16" ? terrain->tiles[15] : terrain->tiles[0]);
                         grid->autotile(layer, *terrain, 0, 0, grid->width - 1, grid->height - 1, static_cast<uint32_t>(engine.scene().seed));
                     } else {
                         changed = grid->flood(layer, x, y, id);
                     }
                 }
                 Status st = engine.edit(ctx.actor, "Paint tiles", [&]() -> Status {
                     Tilemap copy = *tm;
                     grid->writeTo(copy);
                     return engine.scene().patchComponent(*e, "tilemap", Json::object({{"layers", copy.layers}}));
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"entity", *e}, {"layer", layerName}, {"changed", changed}}),
                                         "painted " + std::to_string(changed) + " cell(s) on " + layerName);
             }});

    reg.add({"tilemap_from_ascii", "Tilemap from ASCII",
             "Build or update a tilemap from an ASCII map — the fastest way to lay out a level. Each line is a row (top first); "
             "`legend` maps characters to tile ids, terrain names (auto-tiled) or tileset tile names; ' ' and '.' are empty "
             "unless the legend says otherwise. Creates the entity if it doesn't exist. Without a tileset, tiles render as flat "
             "colors so you can block out levels before the art exists. `solid` marks this layer for collision (true = every "
             "tile, \"tiles\" = only solidTiles). Example: {\"name\": \"Level\", \"tileset\": \"art/tiles.png\", \"tile_size\": 16, "
             "\"autotile\": {\"ground\": {\"mode\": \"blob47\", \"first\": 1}}, \"legend\": {\"#\": \"ground\", \"^\": 60}, "
             "\"solid\": true, \"map\": \"..........\\n...^^.....\\n##########\"}",
             "world",
             object({{"map", string("ASCII rows separated by newlines")},
                     {"legend", Json::object({{"type", "object"}, {"description", "character -> tile id | terrain | tile name"}})},
                     {"entity", entity("Existing tilemap entity to update")},
                     {"name", string("Name for a new tilemap entity (default \"Tilemap\")")},
                     {"layer", string("Layer to write (default \"ground\")")},
                     {"tileset", string("Tileset image or *.tileset.json")},
                     {"tile_size", integer("Tile size in tileset pixels (default 16)")},
                     {"cell_size", number("World units per cell (default 1)")},
                     {"position", vec3("Top-left corner of the map in the world (new maps)")},
                     {"autotile", Json::object({{"type", "object"}, {"description", "Terrains to add: {\"wall\": {\"mode\": \"blob47\", \"first\": 33}}"}})},
                     {"solid", Json::object({{"type", Json::array({"boolean", "string"})}, {"description", "true | \"tiles\" | false"}})},
                     {"solid_tiles", Json::object({{"type", Json::array({"array", "string"})}, {"description", "Tile ids that collide ([3, \"10-20\"])"}})},
                     {"sorting_layer", enumeration(Sprite::sortingLayers(), "Sorting layer of the map")},
                     {"z", number("This layer's z offset (parallax / 2.5D ordering)")}},
                    {"map", "legend"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 EntityId target = kNoEntity;
                 if (a.contains("entity")) {
                     auto e = resolve(engine, a.get("entity"));
                     if (!e) return ToolResult::error(e.error());
                     target = *e;
                 }
                 Json result;
                 Status st = engine.edit(ctx.actor, "Tilemap from ASCII", [&]() -> Status {
                     if (!target) {
                         target = s.create(a.get("name").asString("Tilemap"));
                         if (a.contains("position")) {
                             if (Status r = s.patchComponent(target, "transform", Json::object({{"position", a.get("position")}})); !r) return r;
                         }
                     }
                     Json patch = Json::object();
                     if (a.contains("tileset")) patch["tileset"] = a.get("tileset");
                     if (a.contains("tile_size")) patch["tileSize"] = a.get("tile_size");
                     if (a.contains("cell_size")) patch["cellSize"] = a.get("cell_size");
                     if (a.contains("sorting_layer")) patch["sortingLayer"] = a.get("sorting_layer");
                     if (a.contains("solid_tiles")) patch["solidTiles"] = a.get("solid_tiles");
                     if (a.contains("autotile")) {
                         Json merged = s.get<Tilemap>(target) ? s.get<Tilemap>(target)->autotile : Json::object();
                         if (!merged.isObject()) merged = Json::object();
                         for (const auto& [k, v] : a.get("autotile").members()) merged[k] = v;
                         patch["autotile"] = merged;
                     }
                     if (Status r = s.patchComponent(target, "tilemap", patch); !r) return r;
                     Tilemap& tm = *s.get<Tilemap>(target);
                     int tileCount = 0;
                     std::map<std::string, uint32_t> names;
                     auto terrains = terrainsFor(engine, tm, tileCount, names);
                     if (!terrains) return terrains.error();
                     auto ascii = tiles::parseAscii(a.get("map").asString(), a.get("legend"), terrains.value(), names);
                     if (!ascii) return ascii.error();
                     auto grid = tiles::Grid::fromComponent(tm);
                     if (!grid) return grid.error();
                     const bool fresh = grid->layers.empty();
                     if (fresh) {
                         grid->width = ascii->width;
                         grid->height = ascii->height;
                     } else if (ascii->width > grid->width || ascii->height > grid->height) {
                         grid->resize(std::max(grid->width, ascii->width), std::max(grid->height, ascii->height));
                     }
                     const size_t layer = static_cast<size_t>(grid->layerIndex(a.get("layer").asString("ground"), true));
                     for (int y = 0; y < ascii->height; ++y)
                         for (int x = 0; x < ascii->width; ++x) grid->set(layer, x, y, ascii->cells[static_cast<size_t>(y * ascii->width + x)]);
                     for (const auto& [name, cells] : ascii->terrainCells) {
                         grid->autotile(layer, terrains->at(name), 0, 0, grid->width - 1, grid->height - 1, static_cast<uint32_t>(s.seed));
                         (void)cells;
                     }
                     if (a.contains("solid")) {
                         const Json& sv = a.get("solid");
                         grid->layers[layer].solid = sv.isBool() ? (sv.asBool() ? "all" : "") : sv.asString();
                     }
                     if (a.contains("z")) grid->layers[layer].z = a.get("z").asFloat();
                     Tilemap copy = tm;
                     grid->writeTo(copy);
                     if (Status r = s.patchComponent(target, "tilemap", Json::object({{"width", copy.width}, {"height", copy.height}, {"layers", copy.layers}}));
                         !r) {
                         return r;
                     }
                     std::vector<uint32_t> solid;
                     if (auto ids = tiles::parseIdList(tm.solidTiles); ids) solid = ids.value();
                     auto rects = tiles::solidRects(grid.value(), solid, tm.cellSize);
                     result = Json::object({{"entity", target}, {"name", s.record(target)->name}, {"width", grid->width}, {"height", grid->height},
                                            {"layer", grid->layers[layer].name}, {"solidRects", rects.size()},
                                            {"tileset", tm.tileset.empty() ? Json("(none: flat colors)") : Json(tm.tileset)}});
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(result, "tilemap " + result.get("name").asString() + " " + std::to_string(result.get("width").asInt()) + "x" +
                                                     std::to_string(result.get("height").asInt()));
             }});

    reg.add({"tilemap_inspect", "Inspect tilemap",
             "Read a tilemap as ASCII per layer (with a legend of tile ids and counts), its size, tileset, terrains and the "
             "merged collision rectangles (world units, relative to the map's top-left; x right, y up) — what a physics "
             "body or a pathfinder needs.",
             "world",
             object({{"entity", entity("Tilemap entity")},
                     {"layer", string("Only this layer")},
                     {"max_rects", integer("Collision rectangles to list (default 64)")}},
                    {"entity"}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 auto e = resolve(engine, a.get("entity"));
                 if (!e) return ToolResult::error(e.error());
                 const Tilemap* tm = engine.scene().get<Tilemap>(*e);
                 if (!tm) return ToolResult::error(Error::make("invalid_arguments", "the entity has no tilemap"));
                 auto grid = tiles::Grid::fromComponent(*tm);
                 if (!grid) return ToolResult::error(grid.error());
                 Json layers = Json::array();
                 std::string text;
                 for (size_t l = 0; l < grid->layers.size(); ++l) {
                     if (a.contains("layer") && grid->layers[l].name != a.get("layer").asString()) continue;
                     std::map<uint32_t, char> symbols;
                     Json legend = gridSummary(grid.value(), l, symbols);
                     std::string ascii = tiles::toAscii(grid.value(), l, symbols);
                     layers.push(Json::object({{"name", grid->layers[l].name}, {"solid", grid->layers[l].solid}, {"legend", legend}, {"ascii", ascii}}));
                     text += "layer " + grid->layers[l].name + (grid->layers[l].solid.empty() ? "" : " (solid: " + grid->layers[l].solid + ")") + "\n" + ascii;
                 }
                 std::vector<uint32_t> solid;
                 if (!tm->tileset.empty()) {
                     if (auto ts = engine.world2d().assets().tileset(tm->tileset, tm->tileSize); ts) solid = ts->solid;
                 }
                 if (auto ids = tiles::parseIdList(tm->solidTiles); ids) solid.insert(solid.end(), ids->begin(), ids->end());
                 std::sort(solid.begin(), solid.end());
                 auto rects = tiles::solidRects(grid.value(), solid, tm->cellSize);
                 Json rj = Json::array();
                 size_t maxRects = static_cast<size_t>(std::clamp<int64_t>(a.get("max_rects").asInt(64), 0, 10000));
                 for (size_t i = 0; i < rects.size() && i < maxRects; ++i) {
                     rj.push(Json::array({rects[i].x, rects[i].y, rects[i].w, rects[i].h}));
                 }
                 Json out = Json::object({{"entity", *e}, {"width", grid->width}, {"height", grid->height}, {"cellSize", tm->cellSize},
                                          {"tileset", tm->tileset}, {"layers", layers}, {"solidRects", rj}, {"solidRectCount", rects.size()},
                                          {"terrains", tm->autotile}});
                 return ToolResult::json(out, std::to_string(grid->width) + "x" + std::to_string(grid->height) + " cells, " +
                                                  std::to_string(rects.size()) + " collision rect(s)\n" + text);
             }});
}

}  // namespace sky::tools
