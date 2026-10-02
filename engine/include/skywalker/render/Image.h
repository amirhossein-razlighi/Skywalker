#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky {

/// Tightly packed 8-bit RGBA image, row 0 at the top.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;  // width * height * 4

    Image() = default;
    Image(int w, int h) : width(w), height(h), pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0) {}
    uint8_t* at(int x, int y) { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
    const uint8_t* at(int x, int y) const { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
};

/// Encodes a PNG (zlib-compressed). Used for agent screenshots and asset export.
std::vector<uint8_t> encodePng(const Image& image);
Status writePng(const Image& image, const std::string& path);

}  // namespace sky
