#pragma once
// Sprite atlases: many frames in one texture, described by a small JSON file.
//
//   {"format": "skywalker.atlas", "image": "hero.png", "width": 256, "height": 128,
//    "frames": {"run_0": {"rect": [0, 0, 32, 32], "source": [32, 32], "offset": [0, 0]}, ...}}
//
// rect = pixels in the atlas image; source = the untrimmed frame size; offset = where the trimmed
// rect sits inside the untrimmed frame (top-left). Frames keep their order (animation ranges index
// into it). TexturePacker / Aseprite "JSON hash" and "JSON array" exports load too.

#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/Image.h"

namespace sky::render2d {

struct AtlasFrame {
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;  // rect in the atlas image
    int sourceW = 0, sourceH = 0;    // untrimmed size
    int offsetX = 0, offsetY = 0;    // trimmed rect position inside the untrimmed frame
};

struct Atlas {
    std::string image;  // as written in the file (relative to the atlas file's folder)
    int width = 0, height = 0;
    std::vector<AtlasFrame> frames;

    const AtlasFrame* find(std::string_view name) const;
    int indexOf(std::string_view name) const;
    Json toJson() const;
};

/// Parses an atlas document (skywalker or TexturePacker format).
Result<Atlas> parseAtlas(const Json& doc);
Result<Atlas> loadAtlas(const std::string& path);
bool isAtlasPath(std::string_view path);  // *.atlas.json or *.json

struct PackInput {
    std::string name;
    Image image;
};

struct PackOptions {
    int padding = 2;       // empty pixels between frames
    bool trim = true;      // drop fully transparent borders
    int extrude = 1;       // repeat edge pixels into the padding (no bleeding with linear filtering)
    int maxSize = 4096;
    bool powerOfTwo = false;
};

struct PackResult {
    Image image;
    Atlas atlas;
};

/// Packs images into one atlas (skyline packing via stb_rect_pack, smallest square-ish size that fits).
Result<PackResult> packAtlas(const std::vector<PackInput>& inputs, const PackOptions& options);

/// Frames of a regular grid sheet. Names are `prefix + index` (or `names[i]` when given).
Atlas sliceGrid(int imageWidth, int imageHeight, int cellWidth, int cellHeight, int margin, int spacing,
                const std::string& prefix, const std::vector<std::string>& names = {});

/// Expands a frame selector into frame indices: "4-11", "0-3,6", "run_*" (atlas name pattern), a number.
/// `frameCount` bounds index ranges; names need an atlas.
Result<std::vector<int>> parseFrameList(const Json& frames, int frameCount, const Atlas* atlas);

}  // namespace sky::render2d
