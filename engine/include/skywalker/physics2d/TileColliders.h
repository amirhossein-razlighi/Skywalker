#pragma once
// Collision geometry for tilemaps (collider2d shape "tilemap").
//
// Solid cells come from the tilemap's layers: every tile of a layer with "solid": true, and the tiles
// listed in the tileset's `solid` ids, the map's `solidTiles` or the collision table of a layer with
// "solid": "tiles". Each tile id can have a shape in the collision table (the tileset's "collision"
// object merged with collider2d.tileShapes): full (the default), none, slopes, half tiles, a one-way
// top, or a polygon in tile pixels.
//
// Full cells are merged: into outline loops (counter-clockwise around solid ground, holes clockwise),
// which become Box2D chains with no ghost bumps at tile seams, or into maximal rectangles. Other
// shapes become one polygon per cell; rectangles that span a whole cell's width (half tiles, one-way
// tops) are merged along rows.
//
// Coordinates are the map's local space: x right, y up, the origin at the top-left corner of cell (0, 0)
// (cell (x, y) spans [x, x + 1] * cellSize horizontally and [-(y + 1), -y] * cellSize vertically).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render2d/Tilemap.h"

namespace sky::physics2d {

/// The collision shape of one tile id, in tile units (0..1, x right, y up, origin bottom-left).
struct TileShape {
    enum class Kind { Full, None, Polygon } kind = Kind::Full;
    std::vector<Vec2> points;  // Polygon: counter-clockwise
    bool oneWay = false;
};

/// Preset names: full, none, slope_up, slope_down, slope_up_low, slope_up_high, slope_down_low,
/// slope_down_high, half_bottom, half_top, top (one-way).
const std::vector<std::string>& tileShapePresets();

/// Parses a collision table {"7": "slope_up", "9": {"shape": "top"}, "12": {"points": [[0, 16], [16, 0], [16, 16]], "oneWay": false}}.
/// Points are tile pixels (origin top-left, y down, like the tileset image). Unknown presets are errors with a suggestion.
Result<std::map<uint32_t, TileShape>> parseTileShapes(const Json& table, int tileSize);

struct TileCollision {
    std::vector<uint32_t> solidIds;           // sorted: solid in layers with "solid": "tiles"
    std::map<uint32_t, TileShape> shapes;     // per tile id (missing = full)
};

struct TileGeometry {
    struct Box {
        Vec2 min, max;
    };
    struct Polygon {
        std::vector<Vec2> points;  // counter-clockwise, convex
        bool oneWay = false;
    };
    std::vector<std::vector<Vec2>> loops;  // merged outlines of full cells (counter-clockwise around solid)
    std::vector<Box> boxes;                // merged full cells (tileMerge "boxes")
    std::vector<Polygon> polygons;         // slopes, half tiles, one-way tops (merged along rows when rectangular)
    int solidCells = 0;
    int fullCells = 0;
};

/// Builds the geometry; `chains` picks outline loops (true) or merged boxes (false) for full cells.
TileGeometry buildTileGeometry(const tiles::Grid& grid, const TileCollision& collision, float cellSize, bool chains);

}  // namespace sky::physics2d
