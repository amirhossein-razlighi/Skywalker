#pragma once
// CPU rasterizer for Frame2D: sprites, tiles, SDF text, 2D lights and UI quads. The software
// fallback renderer uses it so headless captures (CI, agents without a GPU) show 2D scenes and
// readable UI; it follows the GPU shaders closely but skips normal maps and 2D shadows.

#include "skywalker/render/Image.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render2d/ImageIO.h"

namespace sky::raster2d {

/// World-space quads (painter's order) composited over `image` (sRGB, row 0 at the top).
void drawWorld(Image& image, const FrameData& frame, render2d::ImageCache& textures);
/// Screen-space UI quads. World-space canvases are projected through the frame's camera.
void drawUI(Image& image, const FrameData& frame, render2d::ImageCache& textures);

}  // namespace sky::raster2d
