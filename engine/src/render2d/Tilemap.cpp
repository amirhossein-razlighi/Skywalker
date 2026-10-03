#include "skywalker/render2d/Tilemap.h"

#include <zlib.h>

#include <algorithm>
#include <set>

#include "skywalker/core/Strings.h"
#include "skywalker/text/Font.h"

namespace sky::tiles {

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::string encodeCells(const std::vector<uint32_t>& cells) {
    std::string rle = "rle:";
    for (size_t i = 0; i < cells.size();) {
        size_t j = i + 1;
        while (j < cells.size() && cells[j] == cells[i]) ++j;
        if (i > 0) rle += ',';
        if (j - i > 1) rle += std::to_string(j - i) + "*";
        rle += std::to_string(cells[i]);
        i = j;
    }
    if (rle.size() <= 2048) return rle;
    std::vector<uint8_t> raw(cells.size() * 4);
    for (size_t i = 0; i < cells.size(); ++i) {
        for (int b = 0; b < 4; ++b) raw[i * 4 + static_cast<size_t>(b)] = static_cast<uint8_t>(cells[i] >> (8 * b));
    }
    uLongf len = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> packed(len);
    if (compress2(packed.data(), &len, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK) return rle;
    std::string b64 = "b64z:" + str::base64Encode(packed.data(), len);
    return b64.size() < rle.size() ? b64 : rle;
}

Result<std::vector<uint32_t>> decodeCells(const Json& data, size_t expected) {
    std::vector<uint32_t> out;
    if (data.isNull() || (data.isString() && data.asString().empty())) {
        out.assign(expected, 0);
        return out;
    }
    if (data.isArray()) {
        for (const auto& v : data.elements()) {
            if (!v.isNumber() || v.asNumber() < 0) return Error::make("invalid_tiles", "tile data arrays hold non-negative ids");
            out.push_back(static_cast<uint32_t>(v.asNumber()));
        }
    } else if (data.isString()) {
        std::string_view s = data.asString();
        if (str::startsWith(s, "b64z:")) {
            std::vector<uint8_t> packed;
            if (!str::base64Decode(s.substr(5), packed)) return Error::make("invalid_tiles", "tile data: invalid base64");
            std::vector<uint8_t> raw(expected * 4);
            uLongf len = static_cast<uLongf>(raw.size());
            if (expected == 0 || uncompress(raw.data(), &len, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
                len != raw.size()) {
                return Error::make("invalid_tiles", "tile data: corrupt or the wrong size for the map");
            }
            out.resize(expected);
            for (size_t i = 0; i < expected; ++i) {
                out[i] = static_cast<uint32_t>(raw[i * 4]) | (static_cast<uint32_t>(raw[i * 4 + 1]) << 8) |
                         (static_cast<uint32_t>(raw[i * 4 + 2]) << 16) | (static_cast<uint32_t>(raw[i * 4 + 3]) << 24);
            }
        } else {
            if (str::startsWith(s, "rle:")) s.remove_prefix(4);
            else if (str::startsWith(s, "csv:")) s.remove_prefix(4);
            size_t start = 0;
            while (start <= s.size()) {
                size_t comma = s.find(',', start);
                std::string tok = str::trim(s.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
                if (!tok.empty()) {
                    double count = 1, id = 0;
                    size_t star = tok.find('*');
                    bool ok = star == std::string::npos
                                  ? str::parseDouble(tok, id)
                                  : str::parseDouble(tok.substr(0, star), count) && str::parseDouble(tok.substr(star + 1), id);
                    if (!ok || count < 1 || id < 0 || id > 4294967295.0) {
                        return Error::make("invalid_tiles", "tile data: bad token \"" + tok + "\"",
                                           "use comma-separated ids with optional count*id runs, e.g. \"rle:3*0,1,1,5\"");
                    }
                    if (out.size() + static_cast<size_t>(count) > expected + 1024 * 1024) {
                        return Error::make("invalid_tiles", "tile data is far larger than the map");
                    }
                    out.insert(out.end(), static_cast<size_t>(count), static_cast<uint32_t>(id));
                }
                if (comma == std::string_view::npos) break;
                start = comma + 1;
            }
        }
    } else {
        return Error::make("invalid_tiles", "tile data must be a string (\"rle:...\" / \"b64z:...\") or an array of ids");
    }
    if (out.size() != expected) {
        return Error::make("invalid_tiles", "tile data has " + std::to_string(out.size()) + " cells but the map has " +
                                                std::to_string(expected),
                           "data is row-major from the top row: width*height ids");
    }
    return out;
}

Result<std::vector<uint32_t>> parseIdList(const Json& list) {
    std::set<uint32_t> ids;
    auto addSpec = [&](const std::string& spec) -> Status {
        for (const auto& raw : str::split(spec, ',')) {
            std::string part = str::trim(raw);
            if (part.empty()) continue;
            double a = 0, b = 0;
            size_t dash = part.find('-', 1);
            if (dash != std::string::npos && str::parseDouble(part.substr(0, dash), a) && str::parseDouble(part.substr(dash + 1), b)) {
                if (a < 0 || b < a || b - a > 1 << 20) return Error::make("invalid_tiles", "bad id range \"" + part + "\"");
                for (auto i = static_cast<uint32_t>(a); i <= static_cast<uint32_t>(b); ++i) ids.insert(i);
            } else if (str::parseDouble(part, a) && a >= 0) {
                ids.insert(static_cast<uint32_t>(a));
            } else {
                return Error::make("invalid_tiles", "bad tile id \"" + part + "\"");
            }
        }
        return {};
    };
    if (list.isNull()) return std::vector<uint32_t>{};
    if (list.isNumber()) ids.insert(static_cast<uint32_t>(std::max(0.0, list.asNumber())));
    else if (list.isString()) {
        if (Status s = addSpec(list.asString()); !s) return s.error();
    } else if (list.isArray()) {
        for (const auto& v : list.elements()) {
            if (v.isNumber() && v.asNumber() >= 0) ids.insert(static_cast<uint32_t>(v.asNumber()));
            else if (v.isString()) {
                if (Status s = addSpec(v.asString()); !s) return s.error();
            } else {
                return Error::make("invalid_tiles", "tile id lists hold numbers or \"a-b\" ranges");
            }
        }
    } else {
        return Error::make("invalid_tiles", "tile id lists hold numbers or \"a-b\" ranges");
    }
    return std::vector<uint32_t>(ids.begin(), ids.end());
}

// ---------------------------------------------------------------------------
// Terrains
// ---------------------------------------------------------------------------

bool Terrain::contains(uint32_t id) const {
    id &= kIdMask;
    return id != 0 && std::find(tiles.begin(), tiles.end(), id) != tiles.end();
}

uint8_t reduceBlobMask(uint8_t m) {
    const bool n = m & 1, e = m & 4, s = m & 16, w = m & 64;
    uint8_t r = m & (1 | 4 | 16 | 64);
    if (n && e && (m & 2)) r |= 2;
    if (s && e && (m & 8)) r |= 8;
    if (s && w && (m & 32)) r |= 32;
    if (n && w && (m & 128)) r |= 128;
    return r;
}

const std::vector<uint8_t>& blobMasks() {
    static const std::vector<uint8_t> masks = [] {
        std::set<uint8_t> set;
        for (int m = 0; m < 256; ++m) set.insert(reduceBlobMask(static_cast<uint8_t>(m)));
        return std::vector<uint8_t>(set.begin(), set.end());
    }();
    return masks;
}

Result<std::map<std::string, Terrain>> parseTerrains(const Json& autotile, int tileCount) {
    std::map<std::string, Terrain> out;
    if (autotile.isNull()) return out;
    if (!autotile.isObject()) return Error::make("invalid_autotile", "autotile must be an object: {\"wall\": {\"mode\": \"blob47\", \"first\": 33}}");
    for (const auto& [name, def] : autotile.members()) {
        Terrain t;
        t.name = name;
        t.mode = def.get("mode").asString("blob47");
        size_t need = t.mode == "blob47" ? 47 : t.mode == "wang16" ? 16 : 0;
        if (t.mode != "blob47" && t.mode != "wang16" && t.mode != "random" && t.mode != "single") {
            std::string guess = str::closest(t.mode, {"blob47", "wang16", "random", "single"}, 3);
            return Error::make("invalid_autotile", "terrain \"" + name + "\": unknown mode \"" + t.mode + "\"",
                               guess.empty() ? "modes: blob47, wang16, random, single" : "did you mean \"" + guess + "\"?");
        }
        if (def.contains("tiles")) {
            const Json& tl = def.get("tiles");
            if (t.mode == "blob47" || t.mode == "wang16") {  // order matters: no range expansion/dedup
                for (const auto& v : tl.elements()) t.tiles.push_back(static_cast<uint32_t>(std::max(0.0, v.asNumber())));
            } else {
                auto ids = parseIdList(tl);
                if (!ids) return ids.error();
                t.tiles = ids.value();
            }
        } else if (def.contains("first")) {
            auto first = static_cast<uint32_t>(std::max<int64_t>(1, def.get("first").asInt(1)));
            size_t n = need ? need : static_cast<size_t>(std::max<int64_t>(1, def.get("count").asInt(1)));
            for (size_t i = 0; i < n; ++i) t.tiles.push_back(first + static_cast<uint32_t>(i));
        } else if (def.isNumber()) {
            t.mode = "single";
            t.tiles.push_back(static_cast<uint32_t>(std::max(1.0, def.asNumber())));
        }
        if (t.tiles.empty()) return Error::make("invalid_autotile", "terrain \"" + name + "\" needs \"first\" or \"tiles\"");
        if (need && t.tiles.size() != need) {
            return Error::make("invalid_autotile", "terrain \"" + name + "\" (" + t.mode + ") needs " + std::to_string(need) +
                                                       " tiles, got " + std::to_string(t.tiles.size()));
        }
        for (uint32_t id : t.tiles) {
            if (id == 0 || (tileCount > 0 && id > static_cast<uint32_t>(tileCount))) {
                return Error::make("invalid_autotile", "terrain \"" + name + "\" uses tile " + std::to_string(id) +
                                                           " but the tileset has " + std::to_string(tileCount) + " tiles");
            }
        }
        for (const auto& w : def.get("weights").elements()) t.weights.push_back(std::max(0.f, w.asFloat()));
        out.emplace(name, std::move(t));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

Result<Grid> Grid::fromComponent(const Tilemap& map) {
    Grid g;
    g.width = std::max(1, map.width);
    g.height = std::max(1, map.height);
    if (static_cast<int64_t>(g.width) * g.height > 16 * 1024 * 1024) return Error::make("invalid_tiles", "tilemap is too large");
    const size_t n = static_cast<size_t>(g.width) * static_cast<size_t>(g.height);
    if (!map.layers.isNull() && !map.layers.isArray()) {
        return Error::make("invalid_tiles", "tilemap.layers must be an array of {name, data, ...}");
    }
    for (const auto& lj : map.layers.elements()) {
        if (!lj.isObject()) return Error::make("invalid_tiles", "each tilemap layer must be an object");
        Layer l;
        l.name = lj.get("name").asString("layer" + std::to_string(g.layers.size()));
        auto cells = decodeCells(lj.get("data"), n);
        if (!cells) return Error::make(cells.error().code, "layer \"" + l.name + "\": " + cells.error().message, cells.error().hint);
        l.cells = std::move(cells.value());
        const Json& solid = lj.get("solid");
        l.solid = solid.isBool() ? (solid.asBool() ? "all" : "") : solid.asString();
        if (!l.solid.empty() && l.solid != "all" && l.solid != "tiles") {
            return Error::make("invalid_tiles", "layer \"" + l.name + "\": solid must be true, false or \"tiles\"");
        }
        l.visible = lj.get("visible").asBool(true);
        l.z = lj.get("z").asFloat(0.f);
        if (lj.contains("tint")) (void)reflect::jsonToColor(lj.get("tint"), l.tint);
        l.sortingLayer = lj.get("sortingLayer").asString();
        l.order = static_cast<int>(lj.get("order").asInt(0));
        for (const auto& [k, v] : lj.members()) {
            if (k != "name" && k != "data" && k != "solid" && k != "visible" && k != "z" && k != "tint" && k != "sortingLayer" &&
                k != "order") {
                l.extra[k] = v;
            }
        }
        g.layers.push_back(std::move(l));
    }
    return g;
}

void Grid::writeTo(Tilemap& map) const {
    map.width = width;
    map.height = height;
    Json arr = Json::array();
    for (const auto& l : layers) {
        Json j = Json::object({{"name", l.name}, {"data", encodeCells(l.cells)}});
        if (l.solid == "all") j["solid"] = true;
        else if (l.solid == "tiles") j["solid"] = "tiles";
        if (!l.visible) j["visible"] = false;
        if (l.z != 0.f) j["z"] = l.z;
        if (!(l.tint == Vec4{1, 1, 1, 1})) j["tint"] = reflect::colorToJson(l.tint);
        if (!l.sortingLayer.empty()) j["sortingLayer"] = l.sortingLayer;
        if (l.order != 0) j["order"] = l.order;
        for (const auto& [k, v] : l.extra.members()) j[k] = v;
        arr.push(std::move(j));
    }
    map.layers = std::move(arr);
}

uint32_t Grid::get(size_t layer, int x, int y) const {
    if (layer >= layers.size() || !inside(x, y)) return 0;
    return layers[layer].cells[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

void Grid::set(size_t layer, int x, int y, uint32_t id) {
    if (layer >= layers.size() || !inside(x, y)) return;
    layers[layer].cells[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = id;
}

int Grid::layerIndex(const std::string& name, bool create) {
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].name == name) return static_cast<int>(i);
    }
    if (!create) return -1;
    Layer l;
    l.name = name.empty() ? "layer" + std::to_string(layers.size()) : name;
    l.cells.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    layers.push_back(std::move(l));
    return static_cast<int>(layers.size() - 1);
}

void Grid::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    for (auto& l : layers) {
        std::vector<uint32_t> cells(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
        for (int y = 0; y < std::min(h, height); ++y) {
            for (int x = 0; x < std::min(w, width); ++x) {
                cells[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                    l.cells[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            }
        }
        l.cells = std::move(cells);
    }
    width = w;
    height = h;
}

namespace {

uint32_t hashCell(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

uint32_t pickVariant(const Terrain& t, int x, int y, uint32_t seed) {
    if (t.tiles.size() == 1) return t.tiles[0];
    float total = 0;
    for (size_t i = 0; i < t.tiles.size(); ++i) total += i < t.weights.size() ? t.weights[i] : 1.f;
    float r = static_cast<float>(hashCell(x, y, seed) % 100000u) / 100000.f * total;
    for (size_t i = 0; i < t.tiles.size(); ++i) {
        r -= i < t.weights.size() ? t.weights[i] : 1.f;
        if (r < 0) return t.tiles[i];
    }
    return t.tiles.back();
}

}  // namespace

void Grid::autotile(size_t layer, const Terrain& t, int x0, int y0, int x1, int y1, uint32_t seed) {
    if (layer >= layers.size() || t.tiles.empty()) return;
    x0 = std::max(0, std::min(x0, x1) - 1);
    y0 = std::max(0, std::min(y0, y1) - 1);
    x1 = std::min(width - 1, std::max(x0, x1) + 1);
    y1 = std::min(height - 1, std::max(y0, y1) + 1);
    // Membership is decided before any rewrite so the result does not depend on scan order.
    auto member = [&](int x, int y) { return !inside(x, y) || t.contains(get(layer, x, y)); };
    std::vector<std::pair<int, uint32_t>> writes;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (!t.contains(get(layer, x, y))) continue;
            uint32_t id = t.tiles[0];
            if (t.mode == "blob47") {
                uint8_t m = 0;
                if (member(x, y - 1)) m |= 1;
                if (member(x + 1, y - 1)) m |= 2;
                if (member(x + 1, y)) m |= 4;
                if (member(x + 1, y + 1)) m |= 8;
                if (member(x, y + 1)) m |= 16;
                if (member(x - 1, y + 1)) m |= 32;
                if (member(x - 1, y)) m |= 64;
                if (member(x - 1, y - 1)) m |= 128;
                const auto& masks = blobMasks();
                auto it = std::lower_bound(masks.begin(), masks.end(), reduceBlobMask(m));
                id = t.tiles[static_cast<size_t>(it - masks.begin())];
            } else if (t.mode == "wang16") {
                int m = (member(x, y - 1) ? 1 : 0) | (member(x + 1, y) ? 2 : 0) | (member(x, y + 1) ? 4 : 0) | (member(x - 1, y) ? 8 : 0);
                id = t.tiles[static_cast<size_t>(m)];
            } else if (t.mode == "random") {
                id = pickVariant(t, x, y, seed);
            }
            writes.push_back({y * width + x, id});
        }
    }
    for (const auto& [i, id] : writes) layers[layer].cells[static_cast<size_t>(i)] = id;
}

void Grid::paintTerrain(size_t layer, const Terrain& t, int x, int y, uint32_t seed) {
    if (layer >= layers.size() || !inside(x, y) || t.tiles.empty()) return;
    uint32_t full = t.mode == "blob47" ? t.tiles[46] : t.mode == "wang16" ? t.tiles[15] : t.mode == "random" ? pickVariant(t, x, y, seed) : t.tiles[0];
    set(layer, x, y, full);
    autotile(layer, t, x, y, x, y, seed);
}

size_t Grid::flood(size_t layer, int x, int y, uint32_t id) {
    if (layer >= layers.size() || !inside(x, y)) return 0;
    uint32_t target = get(layer, x, y);
    if (target == id) return 0;
    size_t changed = 0;
    std::vector<std::pair<int, int>> stack{{x, y}};
    while (!stack.empty()) {
        auto [cx, cy] = stack.back();
        stack.pop_back();
        if (!inside(cx, cy) || get(layer, cx, cy) != target) continue;
        set(layer, cx, cy, id);
        ++changed;
        stack.push_back({cx + 1, cy});
        stack.push_back({cx - 1, cy});
        stack.push_back({cx, cy + 1});
        stack.push_back({cx, cy - 1});
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------

std::vector<uint8_t> solidMask(const Grid& grid, const std::vector<uint32_t>& solidTiles) {
    std::vector<uint8_t> mask(static_cast<size_t>(grid.width) * static_cast<size_t>(grid.height), 0);
    for (const auto& l : grid.layers) {
        if (l.solid.empty()) continue;
        for (size_t i = 0; i < l.cells.size(); ++i) {
            uint32_t id = l.cells[i] & kIdMask;
            if (id == 0) continue;
            if (l.solid == "all" || std::binary_search(solidTiles.begin(), solidTiles.end(), id)) mask[i] = 1;
        }
    }
    return mask;
}

std::vector<SolidRect> solidRects(const Grid& grid, const std::vector<uint32_t>& solidTiles, float cellSize) {
    std::vector<uint8_t> mask = solidMask(grid, solidTiles);
    struct Open {
        int x0, x1, y0;  // run [x0, x1) open since row y0
    };
    std::vector<Open> open, next;
    std::vector<SolidRect> out;
    auto emit = [&](const Open& o, int yEnd) {
        out.push_back({static_cast<float>(o.x0) * cellSize, -static_cast<float>(yEnd) * cellSize,
                       static_cast<float>(o.x1 - o.x0) * cellSize, static_cast<float>(yEnd - o.y0) * cellSize});
    };
    for (int y = 0; y <= grid.height; ++y) {
        next.clear();
        if (y < grid.height) {
            for (int x = 0; x < grid.width;) {
                if (!mask[static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)]) {
                    ++x;
                    continue;
                }
                int x0 = x;
                while (x < grid.width && mask[static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)]) ++x;
                // Continue a rect from the row above with exactly this span, else start a new one.
                auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.x0 == x0 && o.x1 == x; });
                if (it != open.end()) {
                    next.push_back(*it);
                    open.erase(it);
                } else {
                    next.push_back({x0, x, y});
                }
            }
        }
        for (const Open& o : open) emit(o, y);  // spans that did not continue end here
        open.swap(next);
    }
    std::sort(out.begin(), out.end(), [](const SolidRect& a, const SolidRect& b) {
        return a.y != b.y ? a.y > b.y : a.x < b.x;
    });
    return out;
}

// ---------------------------------------------------------------------------
// ASCII
// ---------------------------------------------------------------------------

Result<AsciiResult> parseAscii(const std::string& map, const Json& legend, const std::map<std::string, Terrain>& terrains,
                               const std::map<std::string, uint32_t>& tileNames) {
    if (!legend.isObject()) return Error::make("invalid_arguments", "legend must map characters to tiles: {\"#\": \"wall\", \".\": 0}");
    // Legend: one character (UTF-8) -> tile id | terrain name | tile name | 0 / "" (empty).
    struct Entry {
        uint32_t id = 0;
        std::string terrain;
    };
    std::map<uint32_t, Entry> entries;
    for (const auto& [key, value] : legend.members()) {
        size_t at = 0;
        if (key.empty()) return Error::make("invalid_arguments", "legend keys are single characters");
        uint32_t cp = text::decodeUtf8(key, at);
        if (at != key.size()) return Error::make("invalid_arguments", "legend key \"" + key + "\" must be a single character");
        Entry e;
        if (value.isNumber()) {
            e.id = static_cast<uint32_t>(std::max(0.0, value.asNumber()));
        } else if (value.isString()) {
            const std::string& v = value.asString();
            if (v.empty()) {
            } else if (terrains.count(v)) {
                e.terrain = v;
            } else if (auto it = tileNames.find(v); it != tileNames.end()) {
                e.id = it->second;
            } else {
                std::vector<std::string> names;
                for (const auto& [n, t] : terrains) names.push_back(n);
                for (const auto& [n, id] : tileNames) names.push_back(n);
                std::string guess = str::closest(v, names, 3);
                return Error::make("invalid_arguments", "legend \"" + key + "\": unknown tile or terrain \"" + v + "\"",
                                   guess.empty() ? "use tile ids (numbers), terrain names from tilemap.autotile, or tileset tile names"
                                                 : "did you mean \"" + guess + "\"?");
            }
        } else if (!value.isNull()) {
            return Error::make("invalid_arguments", "legend \"" + key + "\" must be a tile id or a name");
        }
        entries[cp] = e;
    }
    for (uint32_t blank : {static_cast<uint32_t>(' '), static_cast<uint32_t>('.')}) {
        if (!entries.count(blank)) entries[blank] = Entry{};
    }

    std::vector<std::vector<uint32_t>> rows;
    for (auto line : str::split(map, '\n')) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<uint32_t> cps;
        for (size_t i = 0; i < line.size();) cps.push_back(text::decodeUtf8(line, i));
        rows.push_back(std::move(cps));
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    while (!rows.empty() && rows.front().empty()) rows.erase(rows.begin());
    if (rows.empty()) return Error::make("invalid_arguments", "the ASCII map is empty");
    AsciiResult r;
    r.height = static_cast<int>(rows.size());
    for (const auto& row : rows) r.width = std::max(r.width, static_cast<int>(row.size()));
    if (static_cast<int64_t>(r.width) * r.height > 4 * 1024 * 1024) return Error::make("invalid_arguments", "the ASCII map is too large");
    r.cells.assign(static_cast<size_t>(r.width) * static_cast<size_t>(r.height), 0);
    std::map<std::string, std::vector<int>> terrainCells;
    for (int y = 0; y < r.height; ++y) {
        const auto& row = rows[static_cast<size_t>(y)];
        for (int x = 0; x < static_cast<int>(row.size()); ++x) {
            auto it = entries.find(row[static_cast<size_t>(x)]);
            if (it == entries.end()) {
                std::string ch;
                text::appendUtf8(ch, row[static_cast<size_t>(x)]);
                return Error::make("invalid_arguments",
                                   "character '" + ch + "' at row " + std::to_string(y + 1) + ", column " + std::to_string(x + 1) +
                                       " is not in the legend",
                                   "add it to the legend (spaces and '.' are empty by default)");
            }
            int idx = y * r.width + x;
            if (!it->second.terrain.empty()) {
                const Terrain& t = terrains.at(it->second.terrain);
                r.cells[static_cast<size_t>(idx)] = t.mode == "blob47" ? t.tiles[46] : t.mode == "wang16" ? t.tiles[15] : t.tiles[0];
                terrainCells[it->second.terrain].push_back(idx);
            } else {
                r.cells[static_cast<size_t>(idx)] = it->second.id;
            }
        }
    }
    for (auto& [name, cells] : terrainCells) r.terrainCells.push_back({name, std::move(cells)});
    return r;
}

std::string toAscii(const Grid& grid, size_t layer, const std::map<uint32_t, char>& symbols, int maxWidth, int maxHeight) {
    std::string out;
    if (layer >= grid.layers.size()) return out;
    const int w = std::min(grid.width, maxWidth), h = std::min(grid.height, maxHeight);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t id = grid.get(layer, x, y) & kIdMask;
            if (id == 0) {
                out += '.';
            } else if (auto it = symbols.find(id); it != symbols.end()) {
                out += it->second;
            } else {
                static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
                out += kDigits[id % 62];
            }
        }
        if (grid.width > w) out += "…";
        out += '\n';
    }
    if (grid.height > h) out += "… (" + std::to_string(grid.height - h) + " more rows)\n";
    return out;
}

}  // namespace sky::tiles
