#pragma once
// Color grading: built-in "looks" baked into 3D LUTs, and .cube LUT files (the format used
// by DaVinci Resolve, Premiere, Unreal and Unity). LUTs apply in display space, after tonemapping.

#include <string>
#include <vector>

#include "skywalker/core/Result.h"

namespace sky::grading {

struct Lut3D {
    int size = 0;
    std::vector<float> rgb;  // size^3 entries of r, g, b; red varies fastest, then green, then blue
};

/// none, warm, cool, teal_orange, golden_hour, bleach, noir, vivid, moonlight, vintage.
const std::vector<std::string>& looks();
Result<Lut3D> lookLut(const std::string& look, int size = 33);
Result<Lut3D> parseCube(const std::string& text);
Result<Lut3D> loadCube(const std::string& path);

}  // namespace sky::grading
