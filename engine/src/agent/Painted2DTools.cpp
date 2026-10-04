// Tools for painted / pre-rendered 2D action games: importing rendered animation frames (from a DCC
// such as Blender) into packed, normal-mapped sprite atlases, and testing game feel (hit-stop, camera
// shake, sprite flashes) on a live game. Docs: docs/2D_AND_UI.md "Painted 2D: depth, motion and impact".

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render/Image.h"
#include "skywalker/render2d/Atlas.h"
#include "skywalker/render2d/ImageIO.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/text/TextLayout.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

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

Status writeImage(const Image& image, const std::string& full) {
    std::error_code ec;
    fs::create_directories(fs::path(full).parent_path(), ec);
    return writePng(image, full);
}

bool isPng(const fs::path& p) { return str::lower(p.extension().string()) == ".png"; }

/// Natural order ("2.png" < "10.png").
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0, db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            const unsigned long long x = std::stoull(a.substr(i, std::min<size_t>(i2 - i, 18)));
            const unsigned long long y = std::stoull(b.substr(j, std::min<size_t>(j2 - j, 18)));
            if (x != y) return x < y;
            i = i2;
            j = j2;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

std::vector<fs::path> framesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && isPng(entry.path())) out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& x, const fs::path& y) { return naturalLess(x.filename().string(), y.filename().string()); });
    return out;
}

/// Box downsampling by an integer factor. Color averages in premultiplied alpha (no dark fringes);
/// `normals` re-normalizes the averaged vectors instead.
Image downsample(const Image& src, int k, bool normals) {
    if (k <= 1) return src;
    Image out(std::max(1, src.width / k), std::max(1, src.height / k));
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int dy = 0; dy < k; ++dy) {
                for (int dx = 0; dx < k; ++dx) {
                    const uint8_t* p = src.at(std::min(src.width - 1, x * k + dx), std::min(src.height - 1, y * k + dy));
                    const double w = normals ? 1.0 : p[3] / 255.0;
                    r += p[0] * w;
                    g += p[1] * w;
                    b += p[2] * w;
                    a += p[3];
                }
            }
            const double n = static_cast<double>(k * k);
            uint8_t* q = out.at(x, y);
            if (normals) {
                double nx = r / n / 127.5 - 1.0, ny = g / n / 127.5 - 1.0, nz = b / n / 127.5 - 1.0;
                const double len = std::max(1e-6, std::sqrt(nx * nx + ny * ny + nz * nz));
                q[0] = static_cast<uint8_t>(std::clamp((nx / len + 1.0) * 127.5, 0.0, 255.0));
                q[1] = static_cast<uint8_t>(std::clamp((ny / len + 1.0) * 127.5, 0.0, 255.0));
                q[2] = static_cast<uint8_t>(std::clamp((nz / len + 1.0) * 127.5, 0.0, 255.0));
            } else {
                const double wa = a / 255.0;
                q[0] = static_cast<uint8_t>(wa > 1e-6 ? std::clamp(r / wa, 0.0, 255.0) : 0.0);
                q[1] = static_cast<uint8_t>(wa > 1e-6 ? std::clamp(g / wa, 0.0, 255.0) : 0.0);
                q[2] = static_cast<uint8_t>(wa > 1e-6 ? std::clamp(b / wa, 0.0, 255.0) : 0.0);
            }
            q[3] = static_cast<uint8_t>(std::clamp(a / n, 0.0, 255.0));
        }
    }
    return out;
}

struct ClipFrames {
    std::string name;
    std::vector<Image> color, normal;
};

/// The normal-map atlas matching a packed color atlas: each frame's trimmed rect copied from its normal frame.
Image normalAtlas(const render2d::Atlas& atlas, const std::map<std::string, const Image*>& normals) {
    Image out(atlas.width, atlas.height);
    for (size_t i = 0; i < out.pixels.size(); i += 4) {
        out.pixels[i] = 128;
        out.pixels[i + 1] = 128;
        out.pixels[i + 2] = 255;
        out.pixels[i + 3] = 255;
    }
    for (const auto& f : atlas.frames) {
        auto it = normals.find(f.name);
        if (it == normals.end() || !it->second) continue;
        const Image& n = *it->second;
        for (int y = 0; y < f.h; ++y) {
            for (int x = 0; x < f.w; ++x) {
                const int sx = f.offsetX + x, sy = f.offsetY + y;
                if (sx < 0 || sy < 0 || sx >= n.width || sy >= n.height) continue;
                std::copy_n(n.at(sx, sy), 3, out.at(f.x + x, f.y + y));
            }
        }
    }
    return out;
}

}  // namespace

void addPainted2DTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"sprite_sheet_import", "Import rendered sprite animations",
             "Turn rendered animation frames (a Blender/DCC render: one subfolder of numbered PNGs per clip, e.g. "
             "renders/heroine/run/0001.png; folders starting with _ or . are skipped) into packed sprite atlases with sprite_anim clips, plus a matching normal-map "
             "atlas (from a parallel folder of normal-pass frames) so 2D lights shade the animation. Frames are trimmed and "
             "packed; clips that do not fit one atlas spill into more (each clip then names its texture). `downsample` "
             "averages supersampled renders (2 = render at 2x for smooth edges). `entity` applies sprite (pivot, size), "
             "sprite_anim (clips) and lighting in one undoable step. Example: {\"folder\": \"renders/heroine\", \"normals\": "
             "\"renders/heroine_normals\", \"output\": \"art/heroine/heroine\", \"downsample\": 2, \"fps\": 30, \"clips\": "
             "{\"attack1\": {\"loop\": false, \"events\": {\"4\": \"hit\"}}}, \"entity\": \"Heroine\", \"pivot\": [0.5, 0.08], "
             "\"pixels_per_unit\": 150}",
             "asset",
             object({{"folder", string("Folder with one subfolder of numbered PNG frames per clip (project-relative)")},
                     {"normals", string("Folder with the same clip subfolders holding normal-pass frames (optional)")},
                     {"output", string("Output path without extension: writes <output>.png, <output>_n.png, <output>.atlas.json "
                                       "(and _2, _3 ... when clips spill over)")},
                     {"clips", Json::object({{"type", "object"},
                                             {"description", "Per clip: {fps, loop, events: {\"frame\": \"name\"}, name (rename)}; "
                                                             "clips not listed use fps and loop true"}})},
                     {"only", array(Json::object({{"type", "string"}}), "Import only these clip folders")},
                     {"fps", number("Default frames per second (default 24)")},
                     {"downsample", integer("Average NxN source pixels into one (1-4, default 1)")},
                     {"max_size", integer("Maximum atlas size in pixels (default 4096)")},
                     {"padding", integer("Pixels between frames (default 2)")},
                     {"entity", entity("Apply the sprite, animations and normal map to this entity")},
                     {"pivot", Json::object({{"type", "array"}, {"items", Json::object({{"type", "number"}})},
                                             {"description", "With entity: pivot in the untrimmed frame (0..1, [0.5, 0] = feet)"}})},
                     {"pixels_per_unit", number("With entity: sprite pixelsPerUnit (after downsampling)")},
                     {"clip", string("With entity: clip to start (default: the first)")}},
                    {"folder", "output"}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 const std::string folderArg = a.get("folder").asString();
                 const fs::path folder = engine.resolvePath(folderArg);
                 std::error_code ec;
                 if (!fs::is_directory(folder, ec)) {
                     return ToolResult::error(Error::make("not_found", "no folder " + folderArg, "render one subfolder of PNG frames per clip"));
                 }
                 const std::string normalsArg = a.get("normals").asString();
                 const fs::path normals = normalsArg.empty() ? fs::path() : fs::path(engine.resolvePath(normalsArg));
                 if (!normalsArg.empty() && !fs::is_directory(normals, ec)) return ToolResult::error(Error::make("not_found", "no folder " + normalsArg));
                 std::vector<std::string> only;
                 for (const auto& o : a.get("only").elements()) only.push_back(o.asString());
                 const int k = static_cast<int>(std::clamp<int64_t>(a.get("downsample").asInt(1), 1, 4));
                 const float defaultFps = a.get("fps").asFloat(24.f);
                 const Json& clipDefs = a.get("clips");

                 // Read every clip folder.
                 std::vector<std::string> dirs;
                 for (const auto& entry : fs::directory_iterator(folder, ec)) {
                     const std::string dn = entry.path().filename().string();
                     if (entry.is_directory() && !dn.empty() && dn[0] != '_' && dn[0] != '.') dirs.push_back(dn);  // _textures etc. are not clips
                 }
                 std::sort(dirs.begin(), dirs.end(), naturalLess);
                 for (const auto& o : only) {
                     if (std::find(dirs.begin(), dirs.end(), o) == dirs.end()) {
                         return ToolResult::error(Error::make("not_found", "no clip folder \"" + o + "\" in " + folderArg,
                                                              str::closest(o, dirs, 3).empty() ? "" : "did you mean \"" + str::closest(o, dirs, 3) + "\"?"));
                     }
                 }
                 for (const auto& [name, def] : clipDefs.members()) {
                     if (std::find(dirs.begin(), dirs.end(), name) == dirs.end()) {
                         std::string guess = str::closest(name, dirs, 3);
                         return ToolResult::error(Error::make("not_found", "clips names \"" + name + "\" but " + folderArg + " has no such folder",
                                                              guess.empty() ? "" : "did you mean \"" + guess + "\"?"));
                     }
                 }
                 std::vector<ClipFrames> clips;
                 size_t frameTotal = 0;
                 for (const auto& d : dirs) {
                     if (!only.empty() && std::find(only.begin(), only.end(), d) == only.end()) continue;
                     auto files = framesIn(folder / d);
                     if (files.empty()) continue;
                     ClipFrames cf;
                     cf.name = clipDefs.get(d).get("name").asString(d);
                     auto nfiles = normals.empty() ? std::vector<fs::path>{} : framesIn(normals / d);
                     if (!normals.empty() && nfiles.size() != files.size()) {
                         return ToolResult::error(Error::make("invalid_arguments", "clip \"" + d + "\" has " + std::to_string(files.size()) +
                                                                                       " frames but " + std::to_string(nfiles.size()) + " normal frames",
                                                              "render the normal pass for every frame"));
                     }
                     for (size_t i = 0; i < files.size(); ++i) {
                         auto img = render2d::loadImage(files[i].string());
                         if (!img) return ToolResult::error(img.error());
                         cf.color.push_back(downsample(img.value(), k, false));
                         if (!nfiles.empty()) {
                             auto n = render2d::loadImage(nfiles[i].string());
                             if (!n) return ToolResult::error(n.error());
                             if (n->width != img->width || n->height != img->height) {
                                 return ToolResult::error(Error::make("invalid_arguments", "normal frame " + nfiles[i].filename().string() + " of \"" + d +
                                                                                               "\" differs in size from its color frame"));
                             }
                             cf.normal.push_back(downsample(n.value(), k, true));
                         }
                     }
                     frameTotal += files.size();
                     clips.push_back(std::move(cf));
                 }
                 if (clips.empty()) return ToolResult::error(Error::make("not_found", "no PNG frames in the clip folders of " + folderArg));

                 std::string out = a.get("output").asString();
                 for (const std::string ext : {".atlas.json", ".png"}) {
                     if (out.size() > ext.size() && out.compare(out.size() - ext.size(), ext.size(), ext) == 0) out.resize(out.size() - ext.size());
                 }
                 auto outRel = projectRelative(engine, out + ".atlas.json");
                 if (!outRel) return ToolResult::error(outRel.error());
                 const std::string base = outRel->substr(0, outRel->size() - 11);

                 render2d::PackOptions o;
                 o.padding = static_cast<int>(std::clamp<int64_t>(a.get("padding").asInt(2), 0, 64));
                 o.maxSize = static_cast<int>(std::clamp<int64_t>(a.get("max_size").asInt(4096), 64, 16384));
                 auto inputsOf = [&](size_t from, size_t to) {
                     std::vector<render2d::PackInput> in;
                     for (size_t c = from; c < to; ++c) {
                         for (size_t i = 0; i < clips[c].color.size(); ++i) {
                             char idx[16];
                             std::snprintf(idx, sizeof idx, "%04zu", i);
                             in.push_back({clips[c].name + "_" + idx, clips[c].color[i]});
                         }
                     }
                     return in;
                 };
                 // Greedy: as many whole clips per atlas as fit.
                 std::vector<std::pair<size_t, size_t>> groups;
                 size_t start = 0;
                 while (start < clips.size()) {
                     size_t end = start + 1;
                     if (!render2d::packAtlas(inputsOf(start, end), o)) {
                         return ToolResult::error(Error::make("too_large", "clip \"" + clips[start].name + "\" does not fit a " +
                                                                                std::to_string(o.maxSize) + " px atlas",
                                                              "raise max_size, downsample, or render smaller frames"));
                     }
                     while (end < clips.size() && render2d::packAtlas(inputsOf(start, end + 1), o)) ++end;
                     groups.emplace_back(start, end);
                     start = end;
                 }
                 Json clipJson = Json::object();
                 Json sheets = Json::array();
                 std::string firstAtlas;
                 for (size_t g = 0; g < groups.size(); ++g) {
                     auto packed = render2d::packAtlas(inputsOf(groups[g].first, groups[g].second), o);
                     if (!packed) return ToolResult::error(packed.error());
                     const std::string stem = groups.size() == 1 || g == 0 ? base : base + "_" + std::to_string(g + 1);
                     const std::string atlasRel = stem + ".atlas.json", imageRel = stem + ".png", normalRel = stem + "_n.png";
                     packed->atlas.image = fs::path(imageRel).filename().string();
                     bool haveNormals = false;
                     std::map<std::string, const Image*> nmap;
                     for (size_t c = groups[g].first; c < groups[g].second; ++c) {
                         for (size_t i = 0; i < clips[c].normal.size(); ++i) {
                             char idx[16];
                             std::snprintf(idx, sizeof idx, "%04zu", i);
                             nmap[clips[c].name + "_" + idx] = &clips[c].normal[i];
                             haveNormals = true;
                         }
                     }
                     if (haveNormals) {
                         packed->atlas.normalMap = fs::path(normalRel).filename().string();
                         if (Status s = writeImage(normalAtlas(packed->atlas, nmap), engine.resolvePath(normalRel)); !s) return fail(s);
                         engine.world2d().invalidate(engine.resolvePath(normalRel));
                     }
                     if (Status s = writeImage(packed->image, engine.resolvePath(imageRel)); !s) return fail(s);
                     if (Status s = writeText(engine.resolvePath(atlasRel), packed->atlas.toJson().dump(2) + "\n"); !s) return fail(s);
                     engine.world2d().invalidate(engine.resolvePath(atlasRel));
                     engine.world2d().invalidate(engine.resolvePath(imageRel));
                     if (firstAtlas.empty()) firstAtlas = atlasRel;
                     Json names = Json::array();
                     for (size_t c = groups[g].first; c < groups[g].second; ++c) {
                         Json def = Json::object();
                         for (const auto& [folderName, d] : clipDefs.members()) {
                             if (d.get("name").asString(folderName) == clips[c].name) def = d;
                         }
                         Json clip = Json::object({{"frames", clips[c].name + "_*"}, {"fps", def.get("fps").asFloat(defaultFps)},
                                                   {"loop", def.get("loop").asBool(true)}});
                         if (def.contains("events")) clip["events"] = def.get("events");
                         if (atlasRel != firstAtlas) clip["texture"] = atlasRel;
                         clipJson[clips[c].name] = clip;
                         names.push(clips[c].name);
                     }
                     sheets.push(Json::object({{"atlas", atlasRel}, {"image", imageRel}, {"normalMap", haveNormals ? Json(normalRel) : Json()},
                                               {"width", packed->atlas.width}, {"height", packed->atlas.height}, {"clips", names}}));
                 }
                 engine.refreshAssets();
                 Json result = Json::object({{"atlas", firstAtlas}, {"sheets", sheets}, {"clips", clipJson},
                                             {"frames", static_cast<int64_t>(frameTotal)}, {"downsample", k}});
                 if (a.contains("entity")) {
                     auto e = resolve(engine, a.get("entity"));
                     if (!e) return ToolResult::error(e.error());
                     std::string startClip = a.get("clip").asString(clips.front().name);
                     if (!clipJson.contains(startClip)) {
                         std::vector<std::string> names;
                         for (const auto& [n, v] : clipJson.members()) names.push_back(n);
                         std::string guess = str::closest(startClip, names, 3);
                         return ToolResult::error(Error::make("not_found", "no clip \"" + startClip + "\"", guess.empty() ? "" : "did you mean \"" + guess + "\"?"));
                     }
                     Status st = engine.edit(ctx.actor, "Import sprite animations", [&]() -> Status {
                         Json sprite = Json::object({{"texture", firstAtlas}, {"frame", ""}, {"filter", "linear"}, {"lit", true}});
                         if (a.contains("pivot")) sprite["pivot"] = a.get("pivot");
                         if (a.contains("pixels_per_unit")) sprite["pixelsPerUnit"] = a.get("pixels_per_unit");
                         if (Status r = engine.scene().patchComponent(*e, "sprite", sprite); !r) return r;
                         Json anim = Json::object({{"clips", clipJson}, {"clip", startClip}, {"playing", true}});
                         return engine.scene().patchComponent(*e, "sprite_anim", anim);
                     });
                     if (!st) return fail(st);
                     result["entity"] = *e;
                 }
                 return ToolResult::json(result, "imported " + std::to_string(frameTotal) + " frames in " + std::to_string(clips.size()) +
                                                     " clips into " + std::to_string(groups.size()) + " atlas(es), first " + firstAtlas);
             }});

    reg.add({"game_feel", "Game feel (hit-stop, shake, flash)",
             "Try impact feedback on the running game exactly as Wander does: `hit_stop` freezes the game clock for `seconds` "
             "(or slows it to `scale`), `shake` adds trauma (0..1) to the 2D camera (`camera`, default the active one), `flash` "
             "flashes an entity's sprite `color` fading over `seconds`; `info` reads the hit-stop left, each camera2d's trauma "
             "and offset, and active flashes. Use while playing (sim_control play), then step and capture. Example: "
             "{\"action\": \"shake\", \"trauma\": 0.5}",
             "sim",
             object({{"action", enumeration({"hit_stop", "shake", "flash", "info"}, "What to do")},
                     {"seconds", number("hit_stop / flash duration (default 0.08 / 0.12)")},
                     {"scale", number("hit_stop: clock speed during the stop (0 = frozen)")},
                     {"trauma", number("shake: trauma added (0..1, default 0.4)")},
                     {"camera", entity("shake: camera entity with camera2d")},
                     {"entity", entity("flash: sprite entity")},
                     {"color", string("flash: color (default #ffffff)")}},
                    {"action"}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 const std::string action = a.get("action").asString();
                 Scene& scene = engine.scene();
                 if (action == "hit_stop") {
                     engine.runtime().requestHitStop(a.get("seconds").asFloat(0.08f), a.get("scale").asFloat(0.f));
                     return ToolResult::json(Json::object({{"hitStopLeft", engine.runtime().hitStopRemaining()}}), "hit-stop queued");
                 }
                 if (action == "shake") {
                     EntityId cam = kNoEntity;
                     if (a.contains("camera")) {
                         auto e = resolve(engine, a.get("camera"));
                         if (!e) return ToolResult::error(e.error());
                         cam = *e;
                     } else {
                         cam = render2d::activeCamera(scene);
                     }
                     Camera2D* c2 = cam ? scene.get<Camera2D>(cam) : nullptr;
                     if (!c2) return ToolResult::error(Error::make("not_found", "no camera2d to shake", "pass camera, or add camera2d to the camera"));
                     render2d::addCameraShake(*c2, a.get("trauma").asFloat(0.4f));
                     return ToolResult::json(Json::object({{"camera", cam}, {"trauma", c2->trauma_}}), "shaking");
                 }
                 if (action == "flash") {
                     auto e = resolve(engine, a.get("entity"));
                     if (!e) return ToolResult::error(e.error());
                     Sprite* sp = scene.get<Sprite>(*e);
                     if (!sp) return ToolResult::error(Error::make("invalid_arguments", "the entity has no sprite"));
                     Vec4 color{1, 1, 1, 1};
                     if (a.contains("color") && !text::parseColor(a.get("color").asString(), color)) {
                         return ToolResult::error(Error::make("invalid_value", "color must look like #ffffff"));
                     }
                     render2d::flashSprite(*sp, std::clamp(a.get("seconds").asFloat(0.12f), 0.01f, 10.f), color);
                     return ToolResult::json(Json::object({{"entity", *e}}), "flashing");
                 }
                 if (action == "info") {
                     Json cams = Json::array(), flashes = Json::array();
                     for (EntityId e : scene.entities()) {
                         if (const Camera2D* c2 = scene.get<Camera2D>(e)) {
                             cams.push(Json::object({{"entity", e}, {"trauma", c2->trauma_},
                                                     {"offset", Json::array({c2->shakeOffset_.x, c2->shakeOffset_.y})}}));
                         }
                         if (const Sprite* sp = scene.get<Sprite>(e); sp && sp->flashTimer_ > 0.f) {
                             flashes.push(Json::object({{"entity", e}, {"left", sp->flashTimer_}}));
                         }
                     }
                     return ToolResult::json(Json::object({{"hitStopLeft", engine.runtime().hitStopRemaining()},
                                                           {"timeScale", engine.runtime().timeScale()}, {"cameras", cams}, {"flashes", flashes}}),
                                             "game feel state");
                 }
                 return ToolResult::error(Error::make("invalid_value", "unknown action " + action, "hit_stop, shake, flash or info"));
             }});
}

}  // namespace sky::tools
