#include "skywalker/physics2d/TileColliders.h"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "skywalker/core/Strings.h"

namespace sky::physics2d {

namespace {

float signedArea(const std::vector<Vec2>& p) {
    float a = 0.f;
    for (size_t i = 0; i < p.size(); ++i) {
        const Vec2& u = p[i];
        const Vec2& v = p[(i + 1) % p.size()];
        a += u.x * v.y - v.x * u.y;
    }
    return 0.5f * a;
}

void makeCounterClockwise(std::vector<Vec2>& p) {
    if (signedArea(p) < 0.f) std::reverse(p.begin(), p.end());
}

TileShape polygon(std::vector<Vec2> pts, bool oneWay = false) {
    TileShape s;
    s.kind = TileShape::Kind::Polygon;
    s.points = std::move(pts);
    s.oneWay = oneWay;
    return s;
}

bool preset(const std::string& name, TileShape& out) {
    if (name == "full") {
        out = TileShape{};
    } else if (name == "none") {
        out = TileShape{};
        out.kind = TileShape::Kind::None;
    } else if (name == "slope_up") {
        out = polygon({{0, 0}, {1, 0}, {1, 1}});
    } else if (name == "slope_down") {
        out = polygon({{0, 0}, {1, 0}, {0, 1}});
    } else if (name == "slope_up_low") {
        out = polygon({{0, 0}, {1, 0}, {1, 0.5f}});
    } else if (name == "slope_up_high") {
        out = polygon({{0, 0}, {1, 0}, {1, 1}, {0, 0.5f}});
    } else if (name == "slope_down_low") {
        out = polygon({{0, 0}, {1, 0}, {0, 0.5f}});
    } else if (name == "slope_down_high") {
        out = polygon({{0, 0}, {1, 0}, {1, 0.5f}, {0, 1}});
    } else if (name == "half_bottom") {
        out = polygon({{0, 0}, {1, 0}, {1, 0.5f}, {0, 0.5f}});
    } else if (name == "half_top") {
        out = polygon({{0, 0.5f}, {1, 0.5f}, {1, 1}, {0, 1}});
    } else if (name == "top") {
        out = polygon({{0, 0.75f}, {1, 0.75f}, {1, 1}, {0, 1}}, true);
    } else {
        return false;
    }
    return true;
}

/// Applies the tile's flip bits (Tiled's convention: diagonal, then horizontal, then vertical, in image space).
std::vector<Vec2> flipped(const std::vector<Vec2>& pts, uint32_t raw) {
    std::vector<Vec2> out;
    out.reserve(pts.size());
    for (Vec2 p : pts) {
        float a = p.x, b = 1.f - p.y;  // image space: y down
        if (raw & tiles::kFlipDiagonal) std::swap(a, b);
        if (raw & tiles::kFlipX) a = 1.f - a;
        if (raw & tiles::kFlipY) b = 1.f - b;
        out.push_back({a, 1.f - b});
    }
    makeCounterClockwise(out);
    return out;
}

size_t cellIndex(const tiles::Grid& g, int x, int y) {
    return static_cast<size_t>(y) * static_cast<size_t>(g.width) + static_cast<size_t>(x);
}

/// Outline loops of the full cells: directed boundary edges with solid on their left, linked into
/// loops (at a corner shared by two diagonal cells the left turn wins, so regions stay separate),
/// then collinear points are dropped.
std::vector<std::vector<Vec2>> outlineLoops(const tiles::Grid& g, const std::vector<uint8_t>& full, float cs) {
    auto solid = [&](int x, int y) { return g.inside(x, y) && full[cellIndex(g, x, y)]; };
    // Lattice vertices (i, j): i = column 0..width, j = row line 0..height (j grows downward in the grid).
    struct Edge {
        int x0, y0, x1, y1;
        bool used = false;
    };
    std::vector<Edge> edges;
    for (int y = 0; y < g.height; ++y) {
        for (int x = 0; x < g.width; ++x) {
            if (!solid(x, y)) continue;
            // In world space (y up) the cell spans lattice (x, y + 1) bottom-left to (x + 1, y) top-right.
            if (!solid(x, y + 1)) edges.push_back({x, y + 1, x + 1, y + 1});  // bottom, going +x
            if (!solid(x + 1, y)) edges.push_back({x + 1, y + 1, x + 1, y});  // right, going up
            if (!solid(x, y - 1)) edges.push_back({x + 1, y, x, y});          // top, going -x
            if (!solid(x - 1, y)) edges.push_back({x, y, x, y + 1});          // left, going down
        }
    }
    std::map<std::pair<int, int>, std::vector<size_t>> starts;
    for (size_t i = 0; i < edges.size(); ++i) starts[{edges[i].x0, edges[i].y0}].push_back(i);
    // Directions in world space (lattice y is flipped).
    auto dir = [](const Edge& e) { return std::pair<int, int>{e.x1 - e.x0, -(e.y1 - e.y0)}; };
    std::vector<std::vector<Vec2>> loops;
    for (size_t first = 0; first < edges.size(); ++first) {
        if (edges[first].used) continue;
        std::vector<std::pair<int, int>> lattice;
        size_t cur = first;
        while (!edges[cur].used) {
            edges[cur].used = true;
            lattice.push_back({edges[cur].x0, edges[cur].y0});
            const auto& next = starts[{edges[cur].x1, edges[cur].y1}];
            size_t pick = next.empty() ? cur : next.front();
            if (next.size() > 1) {
                auto [dx, dy] = dir(edges[cur]);
                for (size_t n : next) {
                    if (edges[n].used) continue;
                    auto [nx, ny] = dir(edges[n]);
                    if (dx * ny - dy * nx > 0) pick = n;  // left turn
                }
                if (edges[pick].used) {
                    for (size_t n : next) {
                        if (!edges[n].used) pick = n;
                    }
                }
            }
            cur = pick;
        }
        // Drop collinear points.
        std::vector<Vec2> pts;
        const size_t n = lattice.size();
        for (size_t i = 0; i < n; ++i) {
            auto [px, py] = lattice[(i + n - 1) % n];
            auto [cx, cy] = lattice[i];
            auto [qx, qy] = lattice[(i + 1) % n];
            if ((cx - px) * (qy - cy) - (cy - py) * (qx - cx) == 0) continue;
            pts.push_back({static_cast<float>(cx) * cs, -static_cast<float>(cy) * cs});
        }
        if (pts.size() >= 3) loops.push_back(std::move(pts));
    }
    // Deterministic order: by the top-most, then left-most point.
    std::sort(loops.begin(), loops.end(), [](const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
        auto key = [](const std::vector<Vec2>& l) {
            Vec2 best = l.front();
            for (Vec2 p : l) {
                if (p.y > best.y || (p.y == best.y && p.x < best.x)) best = p;
            }
            return std::pair<float, float>{-best.y, best.x};
        };
        return key(a) < key(b);
    });
    return loops;
}

std::vector<TileGeometry::Box> mergedBoxes(const tiles::Grid& g, const std::vector<uint8_t>& full, float cs) {
    struct Open {
        int x0, x1, y0;
    };
    std::vector<Open> open, next;
    std::vector<TileGeometry::Box> out;
    auto emit = [&](const Open& o, int yEnd) {
        out.push_back({{static_cast<float>(o.x0) * cs, -static_cast<float>(yEnd) * cs},
                       {static_cast<float>(o.x1) * cs, -static_cast<float>(o.y0) * cs}});
    };
    for (int y = 0; y <= g.height; ++y) {
        next.clear();
        if (y < g.height) {
            for (int x = 0; x < g.width;) {
                if (!full[cellIndex(g, x, y)]) {
                    ++x;
                    continue;
                }
                int x0 = x;
                while (x < g.width && full[cellIndex(g, x, y)]) ++x;
                auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.x0 == x0 && o.x1 == x; });
                if (it != open.end()) {
                    next.push_back(*it);
                    open.erase(it);
                } else {
                    next.push_back({x0, x, y});
                }
            }
        }
        for (const Open& o : open) emit(o, y);
        open.swap(next);
    }
    std::sort(out.begin(), out.end(), [](const TileGeometry::Box& a, const TileGeometry::Box& b) {
        return a.max.y != b.max.y ? a.max.y > b.max.y : a.min.x < b.min.x;
    });
    return out;
}

/// A polygon spanning the whole cell width as an axis-aligned rectangle: its [v0, v1] band.
bool rowBand(const std::vector<Vec2>& p, float& v0, float& v1) {
    if (p.size() != 4) return false;
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (Vec2 q : p) {
        minX = std::min(minX, q.x);
        maxX = std::max(maxX, q.x);
        minY = std::min(minY, q.y);
        maxY = std::max(maxY, q.y);
    }
    for (Vec2 q : p) {
        bool onX = std::abs(q.x - minX) < 1e-5f || std::abs(q.x - maxX) < 1e-5f;
        bool onY = std::abs(q.y - minY) < 1e-5f || std::abs(q.y - maxY) < 1e-5f;
        if (!onX || !onY) return false;
    }
    if (minX > 1e-5f || maxX < 1.f - 1e-5f) return false;
    v0 = minY;
    v1 = maxY;
    return true;
}

}  // namespace

const std::vector<std::string>& tileShapePresets() {
    static const std::vector<std::string> names{"full",          "none",           "slope_up",        "slope_down",
                                                "slope_up_low",  "slope_up_high",  "slope_down_low",  "slope_down_high",
                                                "half_bottom",   "half_top",       "top"};
    return names;
}

Result<std::map<uint32_t, TileShape>> parseTileShapes(const Json& table, int tileSize) {
    std::map<uint32_t, TileShape> out;
    if (table.isNull()) return out;
    if (!table.isObject()) {
        return Error::make("invalid_tile_shapes", "tile shapes must be an object keyed by tile id",
                           "e.g. {\"7\": \"slope_up\", \"9\": \"top\"}");
    }
    const float ts = static_cast<float>(std::max(1, tileSize));
    for (const auto& [key, value] : table.members()) {
        char* end = nullptr;
        unsigned long id = std::strtoul(key.c_str(), &end, 10);
        if (key.empty() || *end != '\0' || id == 0) {
            return Error::make("invalid_tile_shapes", "tile shape key '" + key + "' is not a tile id",
                               "keys are 1-based tile ids as strings: {\"7\": \"slope_up\"}");
        }
        TileShape s;
        std::string name = value.isString() ? value.asString() : value.get("shape").asString();
        const Json& pts = value.isObject() ? value.get("points") : Json();
        if (pts.isArray() && pts.size() > 0) {
            if (pts.size() < 3 || pts.size() > 8) {
                return Error::make("invalid_tile_shapes", "tile " + key + ": a polygon needs 3 to 8 points",
                                   "points are tile pixels, origin top-left: [[0, 16], [16, 0], [16, 16]]");
            }
            std::vector<Vec2> p;
            for (const Json& q : pts.elements()) {
                if (!q.isArray() || q.size() != 2) {
                    return Error::make("invalid_tile_shapes", "tile " + key + ": each point is [x, y] in tile pixels", "");
                }
                p.push_back({static_cast<float>(q[0].asNumber()) / ts, 1.f - static_cast<float>(q[1].asNumber()) / ts});
            }
            makeCounterClockwise(p);
            if (std::abs(signedArea(p)) < 1e-4f) {
                return Error::make("invalid_tile_shapes", "tile " + key + ": the polygon has no area", "");
            }
            s = polygon(std::move(p));
        } else if (!preset(name.empty() ? "full" : name, s)) {
            std::string guess = str::closest(name, tileShapePresets(), 4);
            std::string list;
            for (const auto& n : tileShapePresets()) list += (list.empty() ? "" : ", ") + n;
            return Error::make("invalid_tile_shapes",
                               "tile " + key + ": unknown shape '" + name + "'" + (guess.empty() ? "" : " - did you mean '" + guess + "'?"),
                               "shapes: " + list + ", or {\"points\": [[x, y], ...]}");
        }
        if (value.isObject() && value.contains("oneWay")) s.oneWay = value.get("oneWay").asBool();
        out[static_cast<uint32_t>(id)] = std::move(s);
    }
    return out;
}

TileGeometry buildTileGeometry(const tiles::Grid& grid, const TileCollision& collision, float cellSize, bool chains) {
    TileGeometry out;
    const float cs = cellSize > 0.f ? cellSize : 1.f;
    const size_t count = static_cast<size_t>(std::max(0, grid.width)) * static_cast<size_t>(std::max(0, grid.height));
    std::vector<uint8_t> full(count, 0);
    std::vector<uint8_t> solidAny(count, 0);
    // Non-full shapes per cell: (cell, tile shape points after flips, one-way).
    struct CellPoly {
        int x, y;
        std::vector<Vec2> points;
        bool oneWay;
    };
    std::vector<CellPoly> partial;
    for (const auto& layer : grid.layers) {
        if (layer.solid.empty()) continue;
        for (int y = 0; y < grid.height; ++y) {
            for (int x = 0; x < grid.width; ++x) {
                const size_t i = cellIndex(grid, x, y);
                if (i >= layer.cells.size()) continue;
                const uint32_t raw = layer.cells[i];
                const uint32_t id = raw & tiles::kIdMask;
                if (id == 0) continue;
                auto it = collision.shapes.find(id);
                const bool listed = std::binary_search(collision.solidIds.begin(), collision.solidIds.end(), id);
                if (layer.solid == "tiles" && !listed && it == collision.shapes.end()) continue;
                if (it == collision.shapes.end() || it->second.kind == TileShape::Kind::Full) {
                    full[i] = 1;
                    solidAny[i] = 1;
                } else if (it->second.kind == TileShape::Kind::Polygon) {
                    solidAny[i] = 1;
                    partial.push_back({x, y, flipped(it->second.points, raw), it->second.oneWay});
                }
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        out.fullCells += full[i];
        out.solidCells += solidAny[i];
    }
    if (chains) out.loops = outlineLoops(grid, full, cs);
    else out.boxes = mergedBoxes(grid, full, cs);

    // Partial shapes: skip those inside full cells; merge full-width bands along rows.
    std::sort(partial.begin(), partial.end(), [](const CellPoly& a, const CellPoly& b) { return std::tie(a.y, a.x) < std::tie(b.y, b.x); });
    for (size_t i = 0; i < partial.size();) {
        const CellPoly& p = partial[i];
        if (full[cellIndex(grid, p.x, p.y)]) {
            ++i;
            continue;
        }
        float v0 = 0, v1 = 0;
        size_t j = i + 1;
        if (rowBand(p.points, v0, v1)) {
            while (j < partial.size()) {
                const CellPoly& q = partial[j];
                float w0 = 0, w1 = 0;
                if (q.y != p.y || q.x != partial[j - 1].x + 1 || q.oneWay != p.oneWay || full[cellIndex(grid, q.x, q.y)] ||
                    !rowBand(q.points, w0, w1) || std::abs(w0 - v0) > 1e-5f || std::abs(w1 - v1) > 1e-5f) {
                    break;
                }
                ++j;
            }
            const float x0 = static_cast<float>(p.x) * cs, x1 = static_cast<float>(partial[j - 1].x + 1) * cs;
            const float base = -static_cast<float>(p.y + 1) * cs;
            out.polygons.push_back({{{x0, base + v0 * cs}, {x1, base + v0 * cs}, {x1, base + v1 * cs}, {x0, base + v1 * cs}}, p.oneWay});
        } else {
            TileGeometry::Polygon poly;
            poly.oneWay = p.oneWay;
            const Vec2 origin{static_cast<float>(p.x) * cs, -static_cast<float>(p.y + 1) * cs};
            for (Vec2 q : p.points) poly.points.push_back(origin + q * cs);
            out.polygons.push_back(std::move(poly));
        }
        i = j;
    }
    return out;
}

}  // namespace sky::physics2d
