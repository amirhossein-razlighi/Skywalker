#pragma once
// Radiance RGBE (.hdr) panoramas: the format HDRI libraries (Poly Haven, ...) ship for
// image-based lighting. Decodes flat and run-length-encoded scanlines to linear float RGB.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky {

struct HdrImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;  // width * height * 3, linear, top row first
};

Result<HdrImage> parseHdr(const std::vector<uint8_t>& bytes);
/// Direction of the brightest region (the sun) in the renderer's panorama mapping at
/// rotation 0: u = atan2(x, -z) / 2pi + 0.5, v = acos(y) / pi. Returns false if flat.
bool hdrSunDirection(const HdrImage& img, float& x, float& y, float& z);
Result<HdrImage> loadHdr(const std::string& path);

}  // namespace sky
