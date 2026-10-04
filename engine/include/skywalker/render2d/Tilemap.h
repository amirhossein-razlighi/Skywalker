#pragma once
// Tilemap data: compact layer encoding, editing, auto-tiling and collision rectangles.
//
// Cells hold 32-bit tile ids: 0 = empty, n = the n-th tile of the tileset (1-based, left->right,
// top->bottom); the top 3 bits flip the tile (Tiled's convention). Layer data is stored as text so
// scene files stay small and diffable:
//   "rle:3*0,1,1,5,12*7"   run-length CSV (count*id), used when it is short
//   "b64z:<base64>"        zlib-compressed little-endian uint32s, used for big, noisy maps
// Arrays of numbers are accepted too.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/ecs/Components.h"

namespace sky::tiles {

constexpr uint32_t kFlipX = 0x80000000u;
constexpr uint32_t kFlipY = 0x40000000u;
constexpr uint32_t kFlipDiagonal = 0x20000000u;
constexpr uint32_t kIdMask = 0x1FFFFFFFu;

std::string encodeCells(const std::vector<uint32_t>& cells);
Result<std::vector<uint32_t>> decodeCells(const Json& data, size_t expectedCount);
/// [3, "10-20", "5,7"] -> ids (deduplicated, sorted).
Result<std::vector<uint32_t>> parseIdList(const Json& list);

struct Layer {
    std::string name;
    std::vector<uint32_t> cells;  // width * height, row-major from the top row
    std::string solid;            // "" | "all" | "tiles"
    bool visible = true;
    float z = 0.f;
    Vec4 tint{1, 1, 1, 1};
    std::string sortingLayer;     // empty = the map's
    int order = 0;
    Json extra = Json::object();  // unknown keys, preserved
};

/// Auto-tiling rule for one terrain.
struct Terrain {
    std::string name;
    std::string mode = "blob47";   // blob47 | wang16 | random | single
    std::vector<uint32_t> tiles;   // blob47: 47 ids (ascending reduced-mask order); wang16: 16 (N=1 E=2 S=4 W=8); random: variants
    std::vector<float> weights;    // random
    bool contains(uint32_t id) const;
};

/// The 47 reduced 8-neighbour masks in ascending order (bits: N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128).
const std::vector<uint8_t>& blobMasks();
/// Drops corner bits whose adjacent edges are not both set.
uint8_t reduceBlobMask(uint8_t mask);

Result<std::map<std::string, Terrain>> parseTerrains(const Json& autotile, int tileCount);

/// A tilemap's layers, decoded for editing and queries.
class Grid {
public:
    int width = 0;
    int height = 0;
    std::vector<Layer> layers;

    static Result<Grid> fromComponent(const Tilemap& map);
    /// Writes the layers (re-encoded) and size back into the component.
    void writeTo(Tilemap& map) const;

    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    uint32_t get(size_t layer, int x, int y) const;
    void set(size_t layer, int x, int y, uint32_t id);
    /// Index of a layer by name; creates it when `create` (appended on top).
    int layerIndex(const std::string& name, bool create);
    void resize(int w, int h);  // keeps the top-left content

    /// Re-picks tiles of `terrain` cells in [x0, x1] x [y0, y1] (inclusive, clamped, grown by 1)
    /// from their neighbours. A cell belongs to the terrain when its id is one of its tiles.
    void autotile(size_t layer, const Terrain& terrain, int x0, int y0, int x1, int y1, uint32_t seed = 0);
    /// Fills a cell with a terrain (its full/base tile) and re-tiles around it.
    void paintTerrain(size_t layer, const Terrain& terrain, int x, int y, uint32_t seed = 0);
    /// 4-connected flood fill of the region with the same id as (x, y). Returns the number of cells changed.
    size_t flood(size_t layer, int x, int y, uint32_t id);
};

/// Solid cells merged into rectangles, in the map's local space (world units, x right, y up; the
/// map origin is the top-left corner of cell (0, 0)).
struct SolidRect {
    float x = 0, y = 0, w = 0, h = 0;  // min corner + size
};
std::vector<SolidRect> solidRects(const Grid& grid, const std::vector<uint32_t>& solidTiles, float cellSize);
/// Solid cells as a width*height mask.
std::vector<uint8_t> solidMask(const Grid& grid, const std::vector<uint32_t>& solidTiles);

/// Parses an ASCII map with a legend. Rows are lines (top first); each character maps through
/// `legend` to a tile id, a terrain name (auto-tiled) or "" / 0 (empty). Unknown characters are errors.
struct AsciiResult {
    int width = 0, height = 0;
    std::vector<uint32_t> cells;
    std::vector<std::pair<std::string, std::vector<int>>> terrainCells;  // terrain -> cell indices to auto-tile
};
Result<AsciiResult> parseAscii(const std::string& map, const Json& legend, const std::map<std::string, Terrain>& terrains,
                               const std::map<std::string, uint32_t>& tileNames);

/// Renders a layer as ASCII (for agents): '.' empty, ids mapped through `symbols` (id -> char), else 'a'..'z' by id.
std::string toAscii(const Grid& grid, size_t layer, const std::map<uint32_t, char>& symbols, int maxWidth = 200, int maxHeight = 120);

}  // namespace sky::tiles
