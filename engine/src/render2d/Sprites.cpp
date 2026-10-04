#include "skywalker/render2d/Sprites.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/render2d/Particles2D.h"
#include "skywalker/text/TextLayout.h"

namespace sky::render2d {

namespace fs = std::filesystem;

Vec4 toLinear(Vec4 c) {
    auto f = [](float v) {
        if (v <= 0.f) return 0.f;
        if (v <= 1.f) return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        return v * v;  // HDR values (> 1) are treated as already-bright linear-ish values
    };
    return {f(c.x), f(c.y), f(c.z), c.w};
}

void Tileset::rect(uint32_t id, int& x, int& y) const {
    uint32_t i = id > 0 ? id - 1 : 0;
    int cols = std::max(1, columns);
    x = margin + static_cast<int>(i % static_cast<uint32_t>(cols)) * (tileSize + spacing);
    y = margin + static_cast<int>(i / static_cast<uint32_t>(cols)) * (tileSize + spacing);
}

uint32_t Tileset::animated(uint32_t id, float time, int x, int y) const {
    if (animations.empty()) return id;
    auto it = animations.find(id);
    if (it == animations.end() || it->second.frames.empty()) return id;
    const TileAnimation& a = it->second;
    const auto n = static_cast<int64_t>(a.frames.size());
    auto step = static_cast<int64_t>(std::floor(std::max(0.f, time) * a.fps));
    if (a.stagger) step += static_cast<int64_t>((static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u)) % n;
    return a.frames[static_cast<size_t>(step % n)];
}

Status parseTileAnimations(const Json& doc, Tileset& out) {
    auto ordered = [](const Json& list, std::vector<uint32_t>& ids) -> Status {
        auto add = [&](const Json& v) -> Status {
            if (v.isNumber() && v.asNumber() >= 1) {
                ids.push_back(static_cast<uint32_t>(v.asNumber()));
                return {};
            }
            if (v.isString()) {
                for (const auto& part : str::split(v.asString(), ',')) {
                    std::string p = str::trim(part);
                    double a = 0, b = 0;
                    size_t dash = p.find('-', 1);
                    if (dash != std::string::npos && str::parseDouble(p.substr(0, dash), a) && str::parseDouble(p.substr(dash + 1), b) &&
                        a >= 1 && b >= 1 && std::fabs(b - a) <= 1024) {
                        const int step = b >= a ? 1 : -1;
                        for (auto i = static_cast<int64_t>(a);; i += step) {
                            ids.push_back(static_cast<uint32_t>(i));
                            if (i == static_cast<int64_t>(b)) break;
                        }
                    } else if (str::parseDouble(p, a) && a >= 1) {
                        ids.push_back(static_cast<uint32_t>(a));
                    } else if (!p.empty()) {
                        return Error::make("invalid_tileset", "animation frame \"" + p + "\" is not a tile id or range",
                                           "frames are 1-based tile ids: [17, 18, \"19-22\"]");
                    }
                }
                return {};
            }
            return Error::make("invalid_tileset", "animation frames are tile ids (numbers or \"a-b\" ranges)");
        };
        if (list.isArray()) {
            for (const auto& v : list.elements()) {
                if (Status s = add(v); !s) return s;
            }
            return {};
        }
        return add(list);
    };
    for (const auto& [key, spec] : doc.get("animations").members()) {
        double id = 0;
        if (!str::parseDouble(key, id) || id < 1) {
            return Error::make("invalid_tileset", "animations: key \"" + key + "\" is not a tile id", "keys are 1-based tile ids: {\"17\": ...}");
        }
        TileAnimation a;
        const Json& frames = spec.isObject() ? spec.get("frames") : spec;
        if (Status s = ordered(frames, a.frames); !s) return s;
        if (spec.isObject()) {
            a.fps = std::clamp(spec.get("fps").asFloat(4.f), 0.f, 120.f);
            a.stagger = spec.get("stagger").asBool(false);
        }
        if (a.frames.empty()) return Error::make("invalid_tileset", "animations: tile " + key + " has no frames");
        out.animations[static_cast<uint32_t>(id)] = std::move(a);
    }
    for (const auto& [key, rows] : doc.get("sortOffset").members()) {
        auto ids = tiles::parseIdList(Json(key));
        if (!ids) return Error::make("invalid_tileset", "sortOffset: key \"" + key + "\" is not a tile id or range");
        for (uint32_t id : ids.value()) out.sortOffset[id] = static_cast<int>(rows.asInt(0));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Assets2D
// ---------------------------------------------------------------------------

Assets2D::Assets2D(std::string projectDir) : projectDir_(std::move(projectDir)), fonts_(projectDir_) {}

void Assets2D::setProjectDir(std::string dir) {
    projectDir_ = std::move(dir);
    fonts_.setProjectDir(projectDir_);
}

std::string Assets2D::resolve(const std::string& path) const {
    if (path.empty()) return path;
    fs::path p(path);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (fs::path(projectDir_) / p).lexically_normal().string();
}

void Assets2D::invalidate(const std::string& absolutePath) {
    images_.invalidate(absolutePath);
    atlases_.erase(absolutePath);
    tilesets_.erase(absolutePath);
    clips_.clear();
    palettes_.clear();
}

namespace {

/// "#rgb", "#rrggbb" or "#rrggbbaa" -> 0xRRGGBBAA.
bool parseHexColor(std::string s, uint32_t& out) {
    s = str::trim(s);
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() == 6) s += "ff";
    if (s.size() != 8) return false;
    uint32_t v = 0;
    for (char ch : s) {
        int d = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0'
                : (ch >= 'a' && ch <= 'f')                    ? ch - 'a' + 10
                : (ch >= 'A' && ch <= 'F')                    ? ch - 'A' + 10
                                                              : -1;
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    out = v;
    return true;
}

}  // namespace

Result<PaletteSwap> loadPalette(const std::string& path) {
    PaletteSwap swap;
    const std::string lowerPath = str::lower(path);
    if (lowerPath.size() >= 5 && lowerPath.compare(lowerPath.size() - 5, 5, ".json") == 0) {
        std::ifstream f(path);
        if (!f) return Error::make("not_found", "cannot read palette " + path);
        std::stringstream ss;
        ss << f.rdbuf();
        auto doc = Json::parse(ss.str());
        if (!doc) return Error::make("invalid_palette", path + ": " + doc.error().message);
        const Json& map = doc.value().get("swap");
        if (!map.isObject()) {
            return Error::make("invalid_palette", path + " has no \"swap\" object",
                               "{\"swap\": {\"#3a7d44\": \"#d8e4ec\", \"#2b5e33\": \"#00000000\"}}");
        }
        for (const auto& [from, to] : map.members()) {
            uint32_t a = 0, b = 0;
            if (!parseHexColor(from, a) || !to.isString() || !parseHexColor(to.asString(), b)) {
                return Error::make("invalid_palette", path + ": bad swap \"" + from + "\"", "use hex colors: \"#rrggbb\": \"#rrggbb[aa]\"");
            }
            swap[a >> 8] = b;
        }
        return swap;
    }
    auto img = loadImage(path);
    if (!img) return img.error();
    if (img->height < 2) return Error::make("invalid_palette", path + " must have 2 rows (sources, then targets)");
    for (int x = 0; x < img->width; ++x) {
        const uint8_t* s = img->at(x, 0);
        const uint8_t* t = img->at(x, 1);
        if (s[3] == 0) continue;  // unused column
        swap[(uint32_t{s[0]} << 16) | (uint32_t{s[1]} << 8) | s[2]] =
            (uint32_t{t[0]} << 24) | (uint32_t{t[1]} << 16) | (uint32_t{t[2]} << 8) | t[3];
    }
    return swap;
}

size_t applyPalette(Image& image, const PaletteSwap& swap) {
    if (swap.empty()) return 0;
    size_t changed = 0;
    uint32_t lastKey = 0xFFFFFFFFu;
    const uint32_t* lastVal = nullptr;
    for (size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        uint8_t* p = image.pixels.data() + i;
        if (p[3] == 0) continue;
        uint32_t key = (uint32_t{p[0]} << 16) | (uint32_t{p[1]} << 8) | p[2];
        if (key != lastKey) {
            auto it = swap.find(key);
            lastKey = key;
            lastVal = it == swap.end() ? nullptr : &it->second;
        }
        if (!lastVal) continue;
        const uint32_t v = *lastVal;
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>((static_cast<uint32_t>(p[3]) * (v & 0xFFu) + 127u) / 255u);
        ++changed;
    }
    return changed;
}

Result<TextureImagePtr> Assets2D::paletted(const std::string& imageAbs, const std::string& palette) {
    const std::string palAbs = resolve(palette);
    CachedPalette& c = palettes_[imageAbs + "|" + palAbs];
    const int64_t mi = fileMTime(imageAbs), mp = fileMTime(palAbs);
    if (mi == c.imageTime && mp == c.paletteTime) return c.image;
    c.imageTime = mi;
    c.paletteTime = mp;
    auto swap = loadPalette(palAbs);
    if (!swap) {
        log::warn("render", swap.error().message);
        c.image = swap.error();
        return c.image;
    }
    auto img = images_.image(imageAbs);
    if (!img) {
        c.image = Error::make("not_found", "cannot open image " + imageAbs);
        return c.image;
    }
    Image copy = *img;
    applyPalette(copy, swap.value());
    auto out = std::make_shared<TextureImage>();
    out->key = "palette:" + imageAbs + "|" + palAbs;
    out->width = copy.width;
    out->height = copy.height;
    out->channels = 4;
    out->pixels = std::move(copy.pixels);
    out->version = ++paletteVersion_;
    c.image = TextureImagePtr(std::move(out));
    return c.image;
}

Result<std::shared_ptr<const Atlas>> Assets2D::atlas(const std::string& path) {
    std::string abs = resolve(path);
    CachedAtlas& c = atlases_[abs];
    int64_t m = fileMTime(abs);
    if (m != c.mtime) {
        c.mtime = m;
        auto a = loadAtlas(abs);
        if (a) c.atlas = std::make_shared<const Atlas>(std::move(a.value()));
        else c.atlas = a.error();
        clips_.clear();
    }
    return c.atlas;
}

Status Assets2D::frameAt(const std::string& texture, int index, int columns, int rows, FrameRef& out) {
    out = FrameRef{};
    if (texture.empty()) return {};
    if (isAtlasPath(texture)) {
        auto a = atlas(texture);
        if (!a) return a.error();
        const Atlas& at = *a.value();
        if (at.frames.empty()) return Error::make("invalid_atlas", texture + " has no frames");
        const AtlasFrame& f = at.frames[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(at.frames.size()) - 1))];
        out.path = (fs::path(resolve(texture)).parent_path() / at.image).lexically_normal().string();
        if (!images_.size(out.path, out.texW, out.texH)) {
            out.texW = at.width;
            out.texH = at.height;
        }
        if (out.texW <= 0 || out.texH <= 0) return Error::make("not_found", "cannot open atlas image " + out.path);
        out.x = static_cast<float>(f.x);
        out.y = static_cast<float>(f.y);
        out.w = static_cast<float>(f.w);
        out.h = static_cast<float>(f.h);
        out.sourceW = static_cast<float>(f.sourceW);
        out.sourceH = static_cast<float>(f.sourceH);
        out.offsetX = static_cast<float>(f.offsetX);
        out.offsetY = static_cast<float>(f.offsetY);
        return {};
    }
    out.path = resolve(texture);
    if (!images_.size(out.path, out.texW, out.texH)) {
        out.path.clear();
        return Error::make("not_found", "cannot open image " + texture);
    }
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    const int count = columns * rows;
    index = std::clamp(index, 0, count - 1);
    const float cw = static_cast<float>(out.texW) / static_cast<float>(columns);
    const float ch = static_cast<float>(out.texH) / static_cast<float>(rows);
    out.x = static_cast<float>(index % columns) * cw;
    out.y = static_cast<float>(index / columns) * ch;
    out.w = out.sourceW = cw;
    out.h = out.sourceH = ch;
    return {};
}

Status Assets2D::frame(const std::string& texture, const std::string& frameSel, int columns, int rows, Vec4 region, FrameRef& out) {
    int index = 0;
    std::string sel = str::trim(frameSel);
    if (!sel.empty()) {
        double v = 0;
        if (str::parseDouble(sel, v)) {
            index = static_cast<int>(v);
        } else if (isAtlasPath(texture)) {
            auto a = atlas(texture);
            if (!a) return a.error();
            index = a.value()->indexOf(sel);
            if (index < 0) {
                std::vector<std::string> names;
                for (const auto& f : a.value()->frames) names.push_back(f.name);
                std::string guess = str::closest(sel, names, 3);
                return Error::make("not_found", texture + " has no frame \"" + sel + "\"", guess.empty() ? "" : "did you mean \"" + guess + "\"?");
            }
        } else {
            return Error::make("invalid_value", "frame \"" + sel + "\" is a name, but " + texture + " is not an atlas",
                               "use a frame index, or pack an atlas with sprite_atlas_pack / sprite_sheet_slice");
        }
    }
    if (Status s = frameAt(texture, index, columns, rows, out); !s) return s;
    if (region.z > 0.f && region.w > 0.f && !out.path.empty()) {
        out.x = region.x;
        out.y = region.y;
        out.w = out.sourceW = region.z;
        out.h = out.sourceH = region.w;
        out.offsetX = out.offsetY = 0;
    }
    return {};
}

int Assets2D::frameCount(const std::string& texture, int columns, int rows) {
    if (texture.empty()) return 1;
    if (isAtlasPath(texture)) {
        auto a = atlas(texture);
        return a ? static_cast<int>(a.value()->frames.size()) : 0;
    }
    return std::max(1, columns) * std::max(1, rows);
}

Result<Tileset> Assets2D::tileset(const std::string& path, int tileSize) {
    std::string abs = resolve(path);
    CachedTileset& c = tilesets_[abs];
    int64_t m = fileMTime(abs);
    if (m == c.mtime && c.tileSize == tileSize) return c.tileset;
    c.mtime = m;
    c.tileSize = tileSize;
    Tileset t;
    t.tileSize = std::max(1, tileSize);
    if (isAtlasPath(path)) {
        std::ifstream f(abs);
        std::stringstream ss;
        ss << f.rdbuf();
        auto doc = Json::parse(ss.str());
        if (!f || !doc) {
            c.tileset = Error::make("not_found", "cannot read tileset " + path);
            return c.tileset;
        }
        const Json& d = doc.value();
        t.image = (fs::path(abs).parent_path() / d.get("image").asString()).lexically_normal().string();
        t.tileSize = static_cast<int>(std::max<int64_t>(1, d.get("tileSize").asInt(t.tileSize)));
        t.spacing = static_cast<int>(std::max<int64_t>(0, d.get("spacing").asInt(0)));
        t.margin = static_cast<int>(std::max<int64_t>(0, d.get("margin").asInt(0)));
        auto solid = tiles::parseIdList(d.get("solid"));
        if (solid) t.solid = solid.value();
        if (d.get("terrains").isObject()) t.terrains = d.get("terrains");
        if (d.get("collision").isObject()) t.collision = d.get("collision");
        for (const auto& [name, id] : d.get("names").members()) t.names[name] = static_cast<uint32_t>(std::max(0.0, id.asNumber()));
        if (Status s = parseTileAnimations(d, t); !s) {
            c.tileset = Error::make(s.error().code, path + ": " + s.error().message, s.error().hint);
            return c.tileset;
        }
    } else {
        t.image = abs;
    }
    if (!images_.size(t.image, t.texW, t.texH)) {
        c.tileset = Error::make("not_found", "cannot open tileset image " + t.image);
        return c.tileset;
    }
    t.columns = std::max(1, (t.texW - 2 * t.margin + t.spacing) / (t.tileSize + t.spacing));
    t.rows = std::max(1, (t.texH - 2 * t.margin + t.spacing) / (t.tileSize + t.spacing));
    t.count = t.columns * t.rows;
    c.tileset = t;
    return c.tileset;
}

Result<std::shared_ptr<const tiles::Grid>> Assets2D::grid(uint64_t entity, const Tilemap& map) {
    CachedGrid& c = grids_[entity];
    if (c.width == map.width && c.height == map.height && c.layers == map.layers) return c.grid;
    c.width = map.width;
    c.height = map.height;
    c.layers = map.layers;
    auto g = tiles::Grid::fromComponent(map);
    if (g) c.grid = std::make_shared<const tiles::Grid>(std::move(g.value()));
    else c.grid = g.error();
    if (grids_.size() > 4096) {  // entities come and go; keep the cache bounded
        CachedGrid keep = std::move(c);
        grids_.clear();
        grids_[entity] = std::move(keep);
        return grids_[entity].grid;
    }
    return c.grid;
}

Result<Clip> Assets2D::clip(const SpriteAnimator& anim, const std::string& name, const std::string& texture, int columns, int rows) {
    const Json& def = anim.clips.get(name);
    if (def.isNull()) {
        std::vector<std::string> names;
        for (const auto& [k, v] : anim.clips.members()) names.push_back(k);
        std::string guess = str::closest(name, names, 3);
        std::string all;
        for (const auto& n : names) all += (all.empty() ? "" : ", ") + n;
        return Error::make("unknown_clip", "no animation clip \"" + name + "\"" + (all.empty() ? " (no clips defined)" : " (clips: " + all + ")"),
                           guess.empty() ? "" : "did you mean \"" + guess + "\"?");
    }
    std::string key = def.dump() + "|" + texture + "|" + std::to_string(columns) + "x" + std::to_string(rows);
    if (auto it = clips_.find(key); it != clips_.end()) return it->second;
    Clip c;
    Result<Clip> result = c;
    if (!def.isObject()) {
        result = Error::make("invalid_clip", "clip \"" + name + "\" must be an object {frames, fps, loop}");
    } else {
        c.fps = std::max(0.01f, def.get("fps").asFloat(10.f));
        c.loop = def.get("loop").asBool(true);
        c.texture = def.get("texture").asString(texture);
        c.columns = static_cast<int>(def.get("columns").asInt(def.contains("texture") ? 1 : columns));
        c.rows = static_cast<int>(def.get("rows").asInt(def.contains("texture") ? 1 : rows));
        std::shared_ptr<const Atlas> at;
        if (isAtlasPath(c.texture)) {
            auto a = atlas(c.texture);
            if (a) at = a.value();
        }
        int count = frameCount(c.texture, c.columns, c.rows);
        auto frames = parseFrameList(def.get("frames"), count, at.get());
        if (!frames) {
            result = Error::make(frames.error().code, "clip \"" + name + "\": " + frames.error().message, frames.error().hint);
        } else {
            c.frames = frames.value();
            for (const auto& [k, v] : def.get("events").members()) {
                double pos = 0;
                if (str::parseDouble(k, pos) && v.isString()) c.events[static_cast<int>(pos)] = v.asString();
            }
            result = c;
        }
    }
    if (clips_.size() > 2048) clips_.clear();
    clips_.emplace(key, result);
    return result;
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

namespace {

int clipIndex(const Clip& c, float time, bool& finished) {
    const int n = static_cast<int>(c.frames.size());
    int idx = static_cast<int>(std::floor(std::max(0.f, time) * c.fps));
    finished = false;
    if (c.loop) return idx % n;
    if (idx >= n) {
        finished = true;
        return n - 1;
    }
    return idx;
}

std::string effectiveClip(const SpriteAnimator& a) {
    if (!a.clip.empty()) return a.clip;
    if (!a.clips.members().empty()) return a.clips.members().front().first;
    return {};
}

}  // namespace

bool animatedFrame(const Scene& scene, Assets2D& assets, EntityId e, std::string& texture, int& columns, int& rows, int& frame) {
    const SpriteAnimator* a = scene.get<SpriteAnimator>(e);
    const Sprite* s = scene.get<Sprite>(e);
    if (!a || !s) return false;
    std::string name = effectiveClip(*a);
    if (name.empty()) return false;
    auto c = assets.clip(*a, name, s->texture, s->columns, s->rows);
    if (!c || c->frames.empty()) return false;
    float t = a->active_ == name ? a->time_ : 0.f;
    bool finished = false;
    frame = c->frames[static_cast<size_t>(clipIndex(c.value(), t, finished))];
    texture = c->texture;
    columns = c->columns;
    rows = c->rows;
    return true;
}

void tickAnimators(Scene& scene, Assets2D& assets, float baseDt, const std::function<void(EntityId, const std::string&)>& emit,
                   const ProcessGate* gate) {
    for (EntityId e : scene.entities()) {
        SpriteAnimator* a = scene.get<SpriteAnimator>(e);
        if (!a || !scene.isActive(e)) continue;
        if (gate && !gate->runs(e)) continue;  // paused (process mode): the sprite holds its frame
        const float dt = baseDt * (gate ? gate->scale(e) : 1.f);
        const Sprite* s = scene.get<Sprite>(e);
        std::string name = effectiveClip(*a);
        if (name.empty()) continue;
        if (a->active_ != name) {
            a->active_ = name;
            a->time_ = 0.f;
            a->lastFrame_ = -1;
            a->finished_ = false;
        }
        auto c = assets.clip(*a, name, s ? s->texture : std::string(), s ? s->columns : 1, s ? s->rows : 1);
        if (!c || c->frames.empty()) continue;
        if (!a->playing || a->finished_) continue;
        a->time_ += dt * a->speed;
        bool finished = false;
        int idx = clipIndex(c.value(), a->time_, finished);
        if (idx != a->lastFrame_ && emit && !c->events.empty()) {
            // Every frame passed since the last tick fires its event (in order, wrapping for loops).
            const int n = static_cast<int>(c->frames.size());
            int from = a->lastFrame_ < 0 ? idx : a->lastFrame_ + 1;
            for (int k = 0, i = from; k < n; ++k, i = (i + 1) % n) {
                if (auto ev = c->events.find(i % n); ev != c->events.end()) emit(e, ev->second);
                if (i % n == idx) break;
            }
        }
        a->lastFrame_ = idx;
        if (finished) {
            a->finished_ = true;
            if (emit) emit(e, "finished");
        }
    }
}

Status playAnimation(Scene& scene, Assets2D& assets, EntityId e, const std::string& clip, bool restart) {
    SpriteAnimator* a = scene.get<SpriteAnimator>(e);
    if (!a) return Error::make("no_animator", "the entity has no sprite_anim component", "add sprite_anim with clips first");
    const Sprite* s = scene.get<Sprite>(e);
    auto c = assets.clip(*a, clip, s ? s->texture : std::string(), s ? s->columns : 1, s ? s->rows : 1);
    if (!c) return c.error();
    if (a->clip != clip || restart) {
        a->clip = clip;
        a->active_ = clip;
        a->time_ = 0.f;
        a->lastFrame_ = -1;
        a->finished_ = false;
    }
    a->playing = true;
    scene.markDirty();
    return {};
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

EntityId activeCamera(const Scene& scene, EntityId preferred) {
    if (preferred && scene.get<Camera>(preferred)) return preferred;
    for (EntityId e : scene.entities()) {
        const Camera* c = scene.get<Camera>(e);
        if (c && c->primary && scene.isActive(e)) return e;
    }
    return kNoEntity;
}

float applyCamera2D(const Scene& scene, EntityId cam, ViewCamera& view, int width, int height) {
    const Camera2D* c2 = cam ? scene.get<Camera2D>(cam) : nullptr;
    if (!c2) return 0.f;
    const float ppu = std::max(0.01f, c2->pixelsPerUnit);
    view.orthographic = true;
    float texel = 0.f;
    if (c2->referenceHeight > 0) {
        float scale = std::max(1.f, std::floor(static_cast<float>(height) / static_cast<float>(c2->referenceHeight)));
        view.orthoSize = static_cast<float>(height) / (2.f * ppu * scale) / std::max(0.01f, c2->zoom);
        texel = 1.f / ppu;  // snap to art texels: true retro look
    } else {
        const Camera* c = scene.get<Camera>(cam);
        view.orthoSize = (c ? c->orthoSize : 5.f) / std::max(0.01f, c2->zoom);
        texel = 2.f * view.orthoSize / static_cast<float>(std::max(1, height));  // one screen pixel
    }
    // Keep the view inside the level bounds.
    const Vec4& b = c2->bounds;
    Vec3 fwd = view.target - view.eye;
    if (b.x != 0.f || b.y != 0.f || b.z != 0.f || b.w != 0.f) {
        float halfH = view.orthoSize, halfW = halfH * static_cast<float>(width) / static_cast<float>(std::max(1, height));
        auto clampAxis = [](float v, float lo, float hi, float half) {
            if (hi - lo <= 2.f * half) return (lo + hi) * 0.5f;
            return std::clamp(v, lo + half, hi - half);
        };
        view.eye.x = clampAxis(view.eye.x, std::min(b.x, b.z), std::max(b.x, b.z), halfW);
        view.eye.y = clampAxis(view.eye.y, std::min(b.y, b.w), std::max(b.y, b.w), halfH);
    }
    if (c2->pixelSnap && texel > 0.f) {
        view.eye.x = std::round(view.eye.x / texel) * texel;
        view.eye.y = std::round(view.eye.y / texel) * texel;
    }
    view.target = view.eye + fwd;
    return c2->pixelSnap ? texel : 0.f;
}

void tickCameras(Scene& scene, float baseDt, const ProcessGate* gate) {
    for (EntityId e : scene.entities()) {
        const Camera2D* c2 = scene.get<Camera2D>(e);
        if (!c2 || c2->follow.empty() || !scene.isActive(e)) continue;
        if (gate && !gate->runs(e)) continue;
        const float dt = baseDt * (gate ? gate->scale(e) : 1.f);
        EntityId target = scene.resolve(c2->follow, e);
        if (!target || target == e) continue;
        Transform* t = scene.get<Transform>(e);
        if (!t) continue;
        Vec3 goal = scene.worldMatrix(target).translation();
        Mat4 parentInv = scene.record(e)->parent ? scene.worldMatrix(scene.record(e)->parent).inverse() : Mat4{};
        Vec3 cur = scene.worldMatrix(e).translation();
        Vec3 desired = cur;
        auto axis = [](float c, float g, float dead) {
            if (g > c + dead) return g - dead;
            if (g < c - dead) return g + dead;
            return c;
        };
        desired.x = axis(cur.x, goal.x + c2->offset.x, c2->deadZone.x);
        desired.y = axis(cur.y, goal.y + c2->offset.y, c2->deadZone.y);
        float k = c2->smoothing > 0.f ? 1.f - std::exp(-dt / c2->smoothing) : 1.f;
        Vec3 next{cur.x + (desired.x - cur.x) * k, cur.y + (desired.y - cur.y) * k, cur.z};
        t->position = parentInv.transformPoint(next);
    }
}

// ---------------------------------------------------------------------------
// Gathering
// ---------------------------------------------------------------------------

namespace {

int layerIndex(const std::string& name) {
    const auto& layers = Sprite::sortingLayers();
    auto it = std::find(layers.begin(), layers.end(), name);
    return it == layers.end() ? 2 : static_cast<int>(it - layers.begin());
}

void set4(float* d, float a, float b, float c, float e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}

SpriteInstance quad(Vec3 origin, Vec3 ax, Vec3 ay, float u0, float v0, float u1, float v1, Vec4 color) {
    SpriteInstance s{};
    set4(s.origin, origin.x, origin.y, origin.z, 1.f);
    set4(s.axisX, ax.x, ax.y, ax.z, 0.f);
    set4(s.axisY, ay.x, ay.y, ay.z, 0.f);
    set4(s.uv, u0, v0, u1, v1);
    set4(s.color, color.x, color.y, color.z, color.w);
    return s;
}

struct Entry {
    int layer = 2;
    int order = 0;
    float depth = 0;
    size_t sceneIndex = 0;
    int sub = 0;  // stable order of several entries of one entity
    size_t first = 0, count = 0;
    TextureRef texture, normalMap;
    bool nearest = false, additive = false;
    bool ySort = false;  // sorted by sortY (higher first) after the plain entries of its layer and order
    float sortY = 0;
    EntityId entity = kNoEntity;
};

struct Ctx {
    const Scene& scene;
    Assets2D& assets;
    FrameData& frame;
    const Gather2DOptions& opts;
    Mat4 vp;
    Vec3 eye, forward, right, up;
    std::vector<SpriteInstance> pool;
    std::vector<Entry> entries;
    std::unordered_map<EntityId, ScreenBox> boxes;
    std::unordered_map<EntityId, int64_t> boxOrder;

    Ctx(const Scene& s, Assets2D& a, FrameData& f, const Gather2DOptions& o) : scene(s), assets(a), frame(f), opts(o) {}

    Entry& begin(EntityId e, int layer, int order, float depth, size_t sceneIndex, int sub) {
        Entry en;
        en.entity = e;
        en.layer = layer;
        en.order = order;
        en.depth = depth;
        en.sceneIndex = sceneIndex;
        en.sub = sub;
        en.first = pool.size();
        entries.push_back(en);
        return entries.back();
    }
    void end(Entry& en) { en.count = pool.size() - en.first; }

    float depthOf(Vec3 p) const { return dot(p - eye, forward); }

    /// Intersects the view frustum's corner rays with the plane through `p0` with normal `n`;
    /// returns the four points (false if the plane is edge-on).
    bool viewOnPlane(Vec3 p0, Vec3 n, Vec3 out[4]) const {
        const float W = static_cast<float>(frame.width), H = static_cast<float>(frame.height);
        const float xs[4] = {0, W, W, 0}, ys[4] = {0, 0, H, H};
        for (int i = 0; i < 4; ++i) {
            Ray r = frame.camera.rayAt(xs[i], ys[i], frame.width, frame.height);
            float d = dot(r.dir, n);
            if (std::fabs(d) < 1e-5f) return false;
            float t = dot(p0 - r.origin, n) / d;
            if (t < 0) t = frame.camera.farPlane;  // ray parallel-ish / looking away: use a far point
            out[i] = r.origin + r.dir * t;
        }
        return true;
    }
};

Vec3 snap(Vec3 p, float texel) {
    if (texel <= 0.f) return p;
    return {std::round(p.x / texel) * texel, std::round(p.y / texel) * texel, p.z};
}

/// Parallax offset for an entity (from the nearest ancestor with a parallax component).
const Parallax* parallaxFor(const Scene& s, EntityId e) {
    for (EntityId cur = e; cur; cur = s.record(cur) ? s.record(cur)->parent : kNoEntity) {
        if (const Parallax* p = s.get<Parallax>(cur)) return p;
    }
    return nullptr;
}

Vec3 parallaxOffset(const Parallax* p, Vec3 eye) {
    if (!p) return {0, 0, 0};
    return {(eye.x - p->origin.x) * (1.f - p->factor.x), (eye.y - p->origin.y) * (1.f - p->factor.y), 0.f};
}

/// The basis a quad uses: the entity's world axes, or camera-facing ones for billboards.
void basis(const Ctx& c, const Mat4& world, const std::string& billboard, Vec3& ax, Vec3& ay, Vec3& az) {
    ax = world.transformDir({1, 0, 0});
    ay = world.transformDir({0, 1, 0});
    az = world.transformDir({0, 0, 1});
    if (billboard == "full") {
        float sx = length(ax), sy = length(ay);
        ax = c.right * sx;
        ay = c.up * sy;
        az = -c.forward;
    } else if (billboard == "y") {
        float sx = length(ax), sy = length(ay);
        Vec3 toCam = c.eye - world.translation();
        toCam.y = 0;
        if (length(toCam) < 1e-5f) toCam = -c.forward;
        toCam = normalize(toCam);
        ax = normalize(cross(Vec3{0, 1, 0}, toCam)) * sx;
        ay = Vec3{0, 1, 0} * sy;
        az = toCam;
    }
}

bool unrotated(Vec3 ax, Vec3 ay) { return std::fabs(ax.y) < 1e-6f && std::fabs(ay.x) < 1e-6f && ax.x > 0 && ay.y > 0; }

void addBox(Ctx& c, EntityId e, const SpriteInstance& s, int64_t order) {
    Vec3 o{s.origin[0], s.origin[1], s.origin[2]};
    Vec3 ax{s.axisX[0], s.axisX[1], s.axisX[2]}, ay{s.axisY[0], s.axisY[1], s.axisY[2]};
    const float W = static_cast<float>(c.frame.width), H = static_cast<float>(c.frame.height);
    ScreenBox& b = c.boxes[e];
    bool fresh = b.entity == 0;
    float x0 = fresh ? 1e30f : b.x, y0 = fresh ? 1e30f : b.y;
    float x1 = fresh ? -1e30f : b.x + b.w, y1 = fresh ? -1e30f : b.y + b.h;
    for (Vec3 p : {o, o + ax, o + ay, o + ax + ay}) {
        Vec4 cl = c.vp * Vec4(p, 1.f);
        if (cl.w <= 1e-5f) return;
        float sx = (cl.x / cl.w * 0.5f + 0.5f) * W, sy = (0.5f - cl.y / cl.w * 0.5f) * H;
        x0 = std::min(x0, sx);
        y0 = std::min(y0, sy);
        x1 = std::max(x1, sx);
        y1 = std::max(y1, sy);
    }
    b.entity = e;
    b.x = x0;
    b.y = y0;
    b.w = x1 - x0;
    b.h = y1 - y0;
    b.depth = std::fabs(c.depthOf(o));
    b.order = std::max(b.order, order);
}

void selectionOutline(Ctx& c, Vec3 o, Vec3 ax, Vec3 ay) {
    // Four thin bars around the quad, in HDR orange (the 3D selection color).
    float px = 2.f * (c.frame.camera.orthographic ? c.frame.camera.orthoSize : std::max(1.f, std::fabs(c.depthOf(o))) *
                                                                                  std::tan(radians(c.frame.camera.fovDeg) * 0.5f)) /
               static_cast<float>(std::max(1, c.frame.height));
    float t = 2.f * px;
    Vec3 nx = normalize(ax) * t, ny = normalize(ay) * t;
    Vec4 col{2.6f, 0.75f, 0.08f, 1.f};
    c.pool.push_back(quad(o - nx - ny, ax + nx * 2.f, ny, 0, 0, 1, 1, col));
    c.pool.push_back(quad(o + ay - nx, ax + nx * 2.f, ny, 0, 0, 1, 1, col));
    c.pool.push_back(quad(o - nx, nx, ay, 0, 0, 1, 1, col));
    c.pool.push_back(quad(o + ax, nx, ay, 0, 0, 1, 1, col));
}

void gatherSprite(Ctx& c, EntityId e, size_t sceneIndex, const Sprite& sp) {
    std::string texture = sp.texture;
    int columns = sp.columns, rows = sp.rows;
    int animFrame = -1;
    FrameRef fr;
    Status st;
    if (animatedFrame(c.scene, c.assets, e, texture, columns, rows, animFrame)) {
        st = c.assets.frameAt(texture, animFrame, columns, rows, fr);
    } else {
        st = c.assets.frame(texture, sp.frame, columns, rows, sp.region, fr);
    }
    if (!st) fr = FrameRef{};  // missing art: draw the tinted quad so the layout is still visible
    const float ppu = std::max(0.01f, sp.pixelsPerUnit);
    float srcW = fr.path.empty() ? ppu : fr.sourceW, srcH = fr.path.empty() ? ppu : fr.sourceH;
    Vec2 size = sp.size;
    if (size.x <= 0 && size.y <= 0) size = {srcW / ppu, srcH / ppu};
    else if (size.x <= 0) size.x = size.y * srcW / std::max(1.f, srcH);
    else if (size.y <= 0) size.y = size.x * srcH / std::max(1.f, srcW);
    const float sx = size.x / std::max(1e-6f, srcW), sy = size.y / std::max(1e-6f, srcH);
    float left = -sp.pivot.x * size.x, top = (1.f - sp.pivot.y) * size.y;
    float w = size.x, h = size.y;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (!fr.path.empty()) {
        left += fr.offsetX * sx;
        top -= fr.offsetY * sy;
        w = fr.w * sx;
        h = fr.h * sy;
        u0 = fr.x / static_cast<float>(fr.texW);
        v0 = fr.y / static_cast<float>(fr.texH);
        u1 = (fr.x + fr.w) / static_cast<float>(fr.texW);
        v1 = (fr.y + fr.h) / static_cast<float>(fr.texH);
    }
    const float fx = sp.flipX ? -1.f : 1.f, fy = sp.flipY ? -1.f : 1.f;
    Mat4 world = c.scene.worldMatrix(e);
    Vec3 wx, wy, wz;
    basis(c, world, sp.billboard, wx, wy, wz);
    Vec3 pos = world.translation();
    const Parallax* par = parallaxFor(c.scene, e);
    pos += parallaxOffset(par, c.eye);
    Vec3 localO{fx * left, fy * top, 0.f};
    Vec3 o = pos + wx * localO.x + wy * localO.y;
    Vec3 ax = wx * (fx * w), ay = wy * (-fy * h);
    if (c.opts.pixelSnap > 0.f && unrotated(wx, wy)) o = snap(o, c.opts.pixelSnap);

    Vec4 color = toLinear(sp.color);
    Vec4 emission = toLinear(Vec4{sp.emissive.x, sp.emissive.y, sp.emissive.z, 1.f}) * sp.emissive.w;
    SpriteInstance base = quad(o, ax, ay, u0, v0, u1, v1, color);
    set4(base.emission, emission.x, emission.y, emission.z, 0.f);
    set4(base.params, static_cast<float>(SpriteMode::Color), sp.lit ? 1.f : 0.f, sp.normalMap.empty() ? 0.f : 1.f, 0.f);
    set4(base.extra, sp.castShadows ? 1.f : 0.f, sp.alphaCutoff, 0.f, 0.f);

    Entry& en = c.begin(e, layerIndex(sp.sortingLayer), sp.order, c.depthOf(pos), sceneIndex, 0);
    en.texture.path = fr.path;
    if (!sp.palette.empty() && !fr.path.empty()) {
        if (auto img = c.assets.paletted(fr.path, sp.palette)) en.texture.image = img.value();
    }
    if (!sp.normalMap.empty()) en.normalMap.path = c.assets.resolve(sp.normalMap);
    en.nearest = sp.filter == "nearest";
    en.ySort = sp.ySort;
    en.sortY = pos.y;
    // Repeats (endless parallax backgrounds) cover the view along the repeated axes.
    int nx0 = 0, nx1 = 0, ny0 = 0, ny1 = 0;
    Vec3 stepX = wx * (par && par->spacing > 0.f ? par->spacing : size.x);
    Vec3 stepY = wy * (par && par->spacing > 0.f ? par->spacing : size.y);
    if (par && (par->repeatX || par->repeatY)) {
        Vec3 corners[4];
        if (c.viewOnPlane(pos, normalize(cross(wx, wy)), corners)) {
            float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
            for (const Vec3& q : corners) {
                Vec3 d = q - pos;
                float ux = dot(d, stepX) / std::max(1e-6f, dot(stepX, stepX));
                float uy = dot(d, stepY) / std::max(1e-6f, dot(stepY, stepY));
                minX = std::min(minX, ux);
                maxX = std::max(maxX, ux);
                minY = std::min(minY, uy);
                maxY = std::max(maxY, uy);
            }
            if (par->repeatX) {
                nx0 = std::max(-200, static_cast<int>(std::floor(minX)) - 1);
                nx1 = std::min(200, static_cast<int>(std::ceil(maxX)) + 1);
            }
            if (par->repeatY) {
                ny0 = std::max(-200, static_cast<int>(std::floor(minY)) - 1);
                ny1 = std::min(200, static_cast<int>(std::ceil(maxY)) + 1);
            }
        }
    }
    for (int iy = ny0; iy <= ny1; ++iy) {
        for (int ix = nx0; ix <= nx1; ++ix) {
            SpriteInstance s = base;
            Vec3 shift = stepX * static_cast<float>(ix) + stepY * static_cast<float>(iy);
            s.origin[0] += shift.x;
            s.origin[1] += shift.y;
            s.origin[2] += shift.z;
            c.pool.push_back(s);
        }
    }
    c.end(en);
    addBox(c, e, base, static_cast<int64_t>(c.entries.size()));
    if (std::find(c.opts.selection.begin(), c.opts.selection.end(), e) != c.opts.selection.end()) {
        Entry& sel = c.begin(e, 4, 1 << 30, 0, sceneIndex, 1);
        selectionOutline(c, o, ax, ay);
        c.end(sel);
    }
}

const Vec4 kTilePalette[] = {
    {0.36f, 0.62f, 0.33f, 1}, {0.55f, 0.43f, 0.29f, 1}, {0.45f, 0.47f, 0.52f, 1}, {0.24f, 0.46f, 0.71f, 1},
    {0.80f, 0.71f, 0.42f, 1}, {0.62f, 0.31f, 0.27f, 1}, {0.37f, 0.33f, 0.52f, 1}, {0.85f, 0.85f, 0.82f, 1},
    {0.19f, 0.39f, 0.25f, 1}, {0.72f, 0.52f, 0.30f, 1}, {0.30f, 0.30f, 0.33f, 1}, {0.50f, 0.70f, 0.78f, 1},
};

void gatherTilemap(Ctx& c, EntityId e, size_t sceneIndex, const Tilemap& map) {
    auto g = c.assets.grid(e, map);
    if (!g) return;
    const tiles::Grid& grid = *g.value();
    Result<Tileset> ts = Error::make("none", "");
    if (!map.tileset.empty()) ts = c.assets.tileset(map.tileset, map.tileSize);
    std::vector<uint32_t> solid = ts ? ts->solid : std::vector<uint32_t>{};
    if (auto extra = tiles::parseIdList(map.solidTiles); extra) solid.insert(solid.end(), extra->begin(), extra->end());
    std::sort(solid.begin(), solid.end());
    std::vector<uint8_t> solidCells;
    if (map.castShadows) solidCells = tiles::solidMask(grid, solid);

    Mat4 world = c.scene.worldMatrix(e);
    Vec3 wx = world.transformDir({1, 0, 0}), wy = world.transformDir({0, 1, 0}), wz = world.transformDir({0, 0, 1});
    const Parallax* par = parallaxFor(c.scene, e);
    Vec3 origin = world.translation() + parallaxOffset(par, c.eye);
    if (c.opts.pixelSnap > 0.f && unrotated(wx, wy)) origin = snap(origin, c.opts.pixelSnap);
    const float cell = std::max(1e-4f, map.cellSize);
    Vec3 cx = wx * cell, cy = wy * -cell;  // +x right, rows go down

    // Visible cell range: the view's footprint on the map plane.
    int x0 = 0, y0 = 0, x1 = grid.width - 1, y1 = grid.height - 1;
    Vec3 corners[4];
    if (c.viewOnPlane(origin, normalize(cross(wx, wy)), corners)) {
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const Vec3& q : corners) {
            Vec3 d = q - origin;
            float u = dot(d, cx) / dot(cx, cx), v = dot(d, cy) / dot(cy, cy);
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
            minV = std::min(minV, v);
            maxV = std::max(maxV, v);
        }
        x0 = std::max(x0, static_cast<int>(std::floor(minU)) - 1);
        x1 = std::min(x1, static_cast<int>(std::ceil(maxU)) + 1);
        y0 = std::max(y0, static_cast<int>(std::floor(minV)) - 1);
        y1 = std::min(y1, static_cast<int>(std::ceil(maxV)) + 1);
    }
    const float invW = ts ? 1.f / static_cast<float>(ts->texW) : 1.f, invH = ts ? 1.f / static_cast<float>(ts->texH) : 1.f;
    // Texels kept away from the tile's edges so filtering never reads the neighbouring tile in the
    // sheet: nearest only needs a hair, bilinear reads half a texel around the sample point.
    const float inset = map.filter == "nearest" ? 0.02f : 0.5f;
    TextureRef tilesTexture;
    if (ts) {
        tilesTexture.path = ts->image;
        if (!map.palette.empty()) {
            if (auto img = c.assets.paletted(ts->image, map.palette)) tilesTexture.image = img.value();
        }
    }
    for (size_t li = 0; li < grid.layers.size(); ++li) {
        const tiles::Layer& layer = grid.layers[li];
        if (!layer.visible) continue;
        Vec3 layerOrigin = origin + wz * layer.z;
        int sortLayer = layerIndex(layer.sortingLayer.empty() ? map.sortingLayer : layer.sortingLayer);
        // Plain layers stack by index; y-sorted layers keep the map/layer order so they interleave with
        // ySort sprites of the same order (characters walking behind fences and tall grass).
        const int layerOrder = map.order + layer.order + (layer.ySort ? 0 : static_cast<int>(li));
        // y-sorted layers emit one entry per row (keyed by the row a tile sorts with), others one entry.
        std::map<int, std::vector<SpriteInstance>> rowsOut;
        const size_t poolStart = c.pool.size();
        Vec4 tint = toLinear(Vec4{map.color.x * layer.tint.x, map.color.y * layer.tint.y, map.color.z * layer.tint.z,
                                  map.color.w * layer.tint.w});
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                uint32_t raw = layer.cells[static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)];
                uint32_t id = raw & tiles::kIdMask;
                if (id == 0) continue;
                int sortRow = y;
                if (ts && layer.ySort && !ts->sortOffset.empty()) {
                    if (auto so = ts->sortOffset.find(id); so != ts->sortOffset.end()) sortRow += so->second;
                }
                if (ts) id = ts->animated(id, c.opts.time, x, y);
                // The quad in cell units: origin (u, v) and edge vectors; flips mirror them (Tiled order:
                // diagonal first, then horizontal, then vertical).
                Vec2 qo{0, 0}, qa{1, 0}, qb{0, 1};
                if (raw & tiles::kFlipDiagonal) std::swap(qa, qb);
                if (raw & tiles::kFlipX) {
                    qo.x = 1.f - qo.x;
                    qa.x = -qa.x;
                    qb.x = -qb.x;
                }
                if (raw & tiles::kFlipY) {
                    qo.y = 1.f - qo.y;
                    qa.y = -qa.y;
                    qb.y = -qb.y;
                }
                Vec3 cellO = layerOrigin + cx * static_cast<float>(x) + cy * static_cast<float>(y);
                Vec3 o = cellO + cx * qo.x + cy * qo.y;
                Vec3 ax = cx * qa.x + cy * qa.y, ay = cx * qb.x + cy * qb.y;
                SpriteInstance s;
                if (ts) {
                    if (id > static_cast<uint32_t>(ts->count)) continue;
                    int px = 0, py = 0;
                    ts->rect(id, px, py);
                    s = quad(o, ax, ay, (static_cast<float>(px) + inset) * invW, (static_cast<float>(py) + inset) * invH,
                             (static_cast<float>(px + ts->tileSize) - inset) * invW,
                             (static_cast<float>(py + ts->tileSize) - inset) * invH, tint);
                } else {
                    // No tileset yet: flat colors per id, so levels can be blocked out before the art exists.
                    Vec4 pc = toLinear(kTilePalette[(id - 1) % (sizeof(kTilePalette) / sizeof(kTilePalette[0]))]);
                    s = quad(o, ax, ay, 0, 0, 1, 1, {pc.x * tint.x, pc.y * tint.y, pc.z * tint.z, tint.w});
                }
                set4(s.params, static_cast<float>(SpriteMode::Color), map.lit ? 1.f : 0.f, 0.f, 0.f);
                bool casts = !solidCells.empty() && solidCells[static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)];
                set4(s.extra, casts ? 1.f : 0.f, 0.f, 0.f, 0.f);
                if (layer.ySort) rowsOut[sortRow].push_back(s);
                else c.pool.push_back(s);
            }
        }
        auto open = [&](int sub) -> Entry& {
            Entry& en = c.begin(e, sortLayer, layerOrder, c.depthOf(layerOrigin), sceneIndex, sub);
            en.texture = tilesTexture;
            en.nearest = map.filter == "nearest";
            return en;
        };
        if (!layer.ySort) {
            Entry& en = open(static_cast<int>(li));
            en.first = poolStart;
            c.end(en);
            continue;
        }
        for (auto& [row, quads] : rowsOut) {
            Entry& en = open(static_cast<int>(li));
            en.ySort = true;
            en.sortY = (layerOrigin + cy * static_cast<float>(row + 1)).y;  // the row's bottom edge (feet line)
            c.pool.insert(c.pool.end(), quads.begin(), quads.end());
            c.end(en);
        }
    }
    // Screen box: the whole map rectangle.
    SpriteInstance whole = quad(origin, cx * static_cast<float>(grid.width), cy * static_cast<float>(grid.height), 0, 0, 1, 1, {});
    addBox(c, e, whole, static_cast<int64_t>(c.entries.size()));
}

void gatherParticles(Ctx& c, EntityId e, size_t sceneIndex, const Particles2D& p) {
    if (p.particles_.empty()) return;
    const std::vector<int> frames = particleFrames(c.assets, p);
    const int nFrames = static_cast<int>(frames.size());
    const float ppu = std::max(0.01f, p.pixelsPerUnit);
    std::vector<FrameRef> refs(frames.size());
    std::vector<bool> ok(frames.size(), false);
    auto frameRef = [&](int i) -> const FrameRef* {
        if (p.texture.empty()) return nullptr;
        if (!ok[static_cast<size_t>(i)]) {
            if (!c.assets.frameAt(p.texture, frames[static_cast<size_t>(i)], p.columns, p.rows, refs[static_cast<size_t>(i)])) return nullptr;
            ok[static_cast<size_t>(i)] = true;
        }
        return refs[static_cast<size_t>(i)].path.empty() ? nullptr : &refs[static_cast<size_t>(i)];
    };
    Mat4 world = c.scene.worldMatrix(e);
    const Vec3 origin = world.translation();
    Entry& en = c.begin(e, layerIndex(p.sortingLayer), p.order, c.depthOf(origin), sceneIndex, 0);
    en.nearest = true;
    if (!p.texture.empty()) {
        if (const FrameRef* fr = frameRef(0)) en.texture.path = fr->path;
    }
    const Vec4 base = toLinear(p.color);
    const Vec4 glow = toLinear(Vec4{p.emissive.x, p.emissive.y, p.emissive.z, 1.f}) * p.emissive.w;
    const float tau = 6.2831853f;
    for (const Particle2D& q : p.particles_) {
        const float t = std::clamp(q.age / std::max(1e-4f, q.life), 0.f, 1.f);
        float alpha = 1.f;
        if (p.fadeIn > 0.f && t < p.fadeIn) alpha *= t / p.fadeIn;
        if (p.fadeOut > 0.f && t > 1.f - p.fadeOut) alpha *= (1.f - t) / p.fadeOut;
        if (p.pulse > 0.f) alpha *= 1.f - p.pulse * (0.5f + 0.5f * std::sin(q.phase + q.age * p.pulseFrequency * tau));
        if (alpha <= 0.002f) continue;
        int fi = std::clamp(q.frame, 0, nFrames - 1);
        if (p.animate == "life") fi = std::min(nFrames - 1, static_cast<int>(t * static_cast<float>(nFrames)));
        else if (p.animate == "loop") fi = (q.frame + static_cast<int>(q.age * p.fps)) % std::max(1, nFrames);
        const FrameRef* fr = frameRef(fi);
        float w = p.pixelSize.x / ppu, h = p.pixelSize.y / ppu, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        float offX = 0, offY = 0;
        if (fr) {
            w = fr->w / ppu;
            h = fr->h / ppu;
            offX = (fr->offsetX - (fr->sourceW - fr->w) * 0.5f) / ppu;   // trimmed atlas frames keep their place
            offY = (fr->offsetY - (fr->sourceH - fr->h) * 0.5f) / ppu;
            u0 = fr->x / static_cast<float>(fr->texW);
            v0 = fr->y / static_cast<float>(fr->texH);
            u1 = (fr->x + fr->w) / static_cast<float>(fr->texW);
            v1 = (fr->y + fr->h) / static_cast<float>(fr->texH);
        }
        Vec3 pos = particleWorldPosition(p, q, origin, c.eye);
        Vec3 o{pos.x - w * 0.5f + offX, pos.y + h * 0.5f - offY, pos.z};
        if (c.opts.pixelSnap > 0.f) o = snap(o, c.opts.pixelSnap);
        Vec4 col{base.x * q.shade, base.y * q.shade, base.z * q.shade, base.w * alpha};
        SpriteInstance s = quad(o, {w, 0, 0}, {0, -h, 0}, u0, v0, u1, v1, col);
        set4(s.emission, glow.x * alpha, glow.y * alpha, glow.z * alpha, 0.f);
        set4(s.params, static_cast<float>(SpriteMode::Color), p.lit ? 1.f : 0.f, 0.f, 0.f);
        set4(s.extra, 0.f, 0.f, 0.f, 0.f);
        c.pool.push_back(s);
    }
    c.end(en);
}

void gatherText(Ctx& c, EntityId e, size_t sceneIndex, const Text& t) {
    text::LayoutParams lp;
    lp.style.font = t.font;
    lp.style.size = t.size;
    lp.style.color = t.color;
    lp.style.outline = t.outline;
    lp.style.outlineColor = t.outlineColor;
    lp.maxWidth = t.maxWidth;
    lp.align = text::alignFromString(t.align);
    lp.lineSpacing = t.lineSpacing;
    text::TextLayout lay = text::layoutText(c.assets.fonts(), t.text, lp);
    if (lay.glyphs.empty() && lay.decorations.empty()) return;
    const float boxW = t.maxWidth > 0.f ? t.maxWidth : lay.width;
    float ox = lp.align == text::Align::Center ? -boxW * 0.5f : lp.align == text::Align::Right ? -boxW : 0.f;
    float oy = t.valign == "middle" ? lay.height * 0.5f : t.valign == "bottom" ? lay.height : 0.f;
    Mat4 world = c.scene.worldMatrix(e);
    Vec3 wx, wy, wz;
    basis(c, world, t.billboard, wx, wy, wz);
    Vec3 pos = world.translation() + parallaxOffset(parallaxFor(c.scene, e), c.eye);
    auto toWorld = [&](float x, float y) { return pos + wx * (ox + x) + wy * (oy - y); };
    const int layer = layerIndex(t.sortingLayer);
    const float depth = c.depthOf(pos);
    int sub = 0;
    auto emitPass = [&](Vec2 shift, Vec4 colorOverride, bool shadow) {
        // One entry per atlas page, glyphs keep their order.
        std::vector<TextureImagePtr> pages;
        for (const auto& g : lay.glyphs) {
            if (std::find(pages.begin(), pages.end(), g.page) == pages.end()) pages.push_back(g.page);
        }
        Vec3 sh = wx * shift.x + wy * shift.y;
        for (const auto& page : pages) {
            Entry& en = c.begin(e, layer, t.order, depth, sceneIndex, sub++);
            en.texture.image = page;
            for (const auto& g : lay.glyphs) {
                if (g.page != page) continue;
                float h = g.y1 - g.y0;
                Vec3 o = toWorld(g.x0 + g.shear * h, g.y0) + sh;
                Vec3 ax = wx * (g.x1 - g.x0), ay = wx * (-g.shear * h) - wy * h;
                Vec4 col = shadow ? colorOverride : g.color;
                Vec4 lin = toLinear(col);
                float glow = shadow ? 1.f : 1.f + t.emissive;
                SpriteInstance s = quad(o, ax, ay, g.u0, g.v0, g.u1, g.v1, {lin.x * glow, lin.y * glow, lin.z * glow, lin.w});
                Vec4 oc = toLinear(shadow ? colorOverride : g.outlineColor);
                set4(s.emission, oc.x, oc.y, oc.z, g.outline);
                set4(s.params, static_cast<float>(SpriteMode::Sdf), 0.f, 0.f, g.dilate);
                set4(s.extra, 0.f, 0.f, 0.f, 0.f);
                c.pool.push_back(s);
                addBox(c, e, s, static_cast<int64_t>(c.entries.size()));
            }
            c.end(en);
        }
        if (!lay.decorations.empty()) {
            Entry& en = c.begin(e, layer, t.order, depth, sceneIndex, sub++);
            for (const auto& d : lay.decorations) {
                Vec4 lin = toLinear(shadow ? colorOverride : d.color);
                SpriteInstance s = quad(toWorld(d.x0, d.y0) + sh, wx * (d.x1 - d.x0), wy * -(d.y1 - d.y0), 0, 0, 1, 1, lin);
                set4(s.params, static_cast<float>(SpriteMode::Color), 0.f, 0.f, 0.f);
                c.pool.push_back(s);
            }
            c.end(en);
        }
    };
    if (t.shadowColor.w > 0.f) emitPass(t.shadowOffset, t.shadowColor, true);
    emitPass({0, 0}, {}, false);
}

float flickerAt(float time, EntityId e, float amount) {
    if (amount <= 0.f) return 1.f;
    float p = static_cast<float>(e % 997) * 1.7f;
    float n = 0.5f * std::sin(time * 9.1f + p) + 0.3f * std::sin(time * 17.3f + p * 2.1f) + 0.2f * std::sin(time * 31.7f + p * 0.7f);
    return std::max(0.f, 1.f - amount * (0.5f + 0.5f * n));
}

void gatherLight(Ctx& c, EntityId e, const Light2D& l) {
    Mat4 world = c.scene.worldMatrix(e);
    Vec4 col = toLinear(l.color);
    float k = l.intensity * flickerAt(c.opts.time, e, l.flicker);
    Vec3 rgb = col.xyz() * k;
    Frame2D& f = c.frame.render2d;
    f.lit = true;
    if (l.kind == "global") {
        f.ambient = f.ambient + rgb;
        return;
    }
    Light2DItem li;
    li.kind = l.kind == "spot" ? Light2DItem::Kind::Spot : Light2DItem::Kind::Point;
    li.position = world.translation() + parallaxOffset(parallaxFor(c.scene, e), c.eye);
    li.direction = normalize(world.transformDir({0, 1, 0}));
    li.color = rgb;
    li.radius = l.radius;
    li.falloff = l.falloff;
    li.cosInner = std::cos(radians(std::min(l.innerAngle, l.outerAngle)));
    li.cosOuter = std::cos(radians(l.outerAngle));
    li.height = l.height;
    li.shadows = l.shadows;
    li.shadowSoftness = l.shadowSoftness;
    li.bands = std::clamp(l.bands, 0, 32);
    f.lights.push_back(li);
    if (l.halo > 0.f) {
        float r = l.radius * 0.75f;
        Vec3 o = li.position - c.right * r + c.up * r;
        SpriteInstance s = quad(o, c.right * (2.f * r), c.up * (-2.f * r), 0, 0, 1, 1,
                                {rgb.x * l.halo * 0.35f, rgb.y * l.halo * 0.35f, rgb.z * l.halo * 0.35f, 1.f});
        set4(s.params, static_cast<float>(SpriteMode::Halo), static_cast<float>(li.bands), 0.f, 0.f);
        Entry& en = c.begin(e, 4, 1 << 29, c.depthOf(li.position), 0, 0);
        en.additive = true;
        c.pool.push_back(s);
        c.end(en);
    }
}

}  // namespace

void gather2D(const Scene& scene, Assets2D& assets, FrameData& frame, const Gather2DOptions& opts) {
    Ctx c(scene, assets, frame, opts);
    c.vp = frame.viewProjection();
    c.eye = frame.camera.eye;
    c.forward = normalize(frame.camera.target - frame.camera.eye);
    Mat4 inv = frame.view.inverse();
    c.right = normalize(inv.transformDir({1, 0, 0}));
    c.up = normalize(inv.transformDir({0, 1, 0}));
    Frame2D& f = frame.render2d;
    f.ambient = {0, 0, 0};
    f.texel = opts.pixelSnap;

    const auto& order = scene.entities();
    for (size_t i = 0; i < order.size(); ++i) {
        EntityId e = order[i];
        if (!scene.isActive(e)) continue;
        if (const Light2D* l = scene.get<Light2D>(e)) gatherLight(c, e, *l);
        if (const Sprite* s = scene.get<Sprite>(e); s && s->visible) gatherSprite(c, e, i, *s);
        if (const Tilemap* t = scene.get<Tilemap>(e)) gatherTilemap(c, e, i, *t);
        if (const Particles2D* p = scene.get<Particles2D>(e)) gatherParticles(c, e, i, *p);
        if (const Text* t = scene.get<Text>(e); t && t->visible && !t->text.empty()) gatherText(c, e, i, *t);
    }
    if (!f.lit) f.ambient = {1, 1, 1};
    // Keep the lights that matter most (nearest to the view center), stable.
    if (f.lights.size() > Frame2D::kMaxLights) {
        Vec3 center = frame.camera.target;
        std::stable_sort(f.lights.begin(), f.lights.end(), [&](const Light2DItem& a, const Light2DItem& b) {
            return distance(a.position, center) - a.radius < distance(b.position, center) - b.radius;
        });
        f.lights.resize(Frame2D::kMaxLights);
    }

    std::stable_sort(c.entries.begin(), c.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.order != b.order) return a.order < b.order;
        if (a.ySort != b.ySort) return b.ySort;  // plain entries (ground) first, then the y-sorted ones
        if (a.ySort && std::fabs(a.sortY - b.sortY) > 1e-5f) return a.sortY > b.sortY;  // higher on screen first
        if (std::fabs(a.depth - b.depth) > 1e-5f) return a.depth > b.depth;  // far first
        if (a.sceneIndex != b.sceneIndex) return a.sceneIndex < b.sceneIndex;
        return a.sub < b.sub;
    });
    f.sprites.reserve(f.sprites.size() + c.pool.size());
    int64_t drawIndex = 0;
    std::unordered_map<EntityId, int64_t> lastDraw;
    for (const Entry& en : c.entries) {
        if (en.count == 0) continue;
        const auto first = static_cast<uint32_t>(f.sprites.size());
        f.sprites.insert(f.sprites.end(), c.pool.begin() + static_cast<std::ptrdiff_t>(en.first),
                         c.pool.begin() + static_cast<std::ptrdiff_t>(en.first + en.count));
        SpriteBatch* last = f.spriteBatches.empty() ? nullptr : &f.spriteBatches.back();
        if (last && last->texture == en.texture && last->normalMap == en.normalMap && last->nearest == en.nearest &&
            last->additive == en.additive) {
            last->count += static_cast<uint32_t>(en.count);
        } else {
            SpriteBatch b;
            b.texture = en.texture;
            b.normalMap = en.normalMap;
            b.nearest = en.nearest;
            b.additive = en.additive;
            b.first = first;
            b.count = static_cast<uint32_t>(en.count);
            f.spriteBatches.push_back(std::move(b));
        }
        lastDraw[en.entity] = ++drawIndex;
    }
    const float W = static_cast<float>(frame.width), H = static_cast<float>(frame.height);
    for (auto& [e, b] : c.boxes) {
        float x0 = std::clamp(b.x, 0.f, W), y0 = std::clamp(b.y, 0.f, H);
        float x1 = std::clamp(b.x + b.w, 0.f, W), y1 = std::clamp(b.y + b.h, 0.f, H);
        if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) continue;
        ScreenBox out = b;
        out.x = x0;
        out.y = y0;
        out.w = x1 - x0;
        out.h = y1 - y0;
        out.order = lastDraw.count(e) ? lastDraw[e] : 0;
        f.boxes.push_back(out);
    }
    std::sort(f.boxes.begin(), f.boxes.end(), [](const ScreenBox& a, const ScreenBox& b) {
        return a.order != b.order ? a.order > b.order : a.entity < b.entity;
    });
}

}  // namespace sky::render2d
