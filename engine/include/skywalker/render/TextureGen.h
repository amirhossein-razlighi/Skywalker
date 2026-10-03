#pragma once
// Procedural, seamlessly tiling PBR texture generator. Lets agents create realistic or
// stylized surfaces (bricks, wood, rust, scales, ...) without external image assets.
// Output is an albedo + tangent-space normal + ORM (glTF packing) texture set.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"
#include "skywalker/render/Image.h"

namespace sky::texgen {

struct Params {
    std::string kind = "noise";
    int size = 512;        // power of two, 32..2048
    uint32_t seed = 1;
    float scale = 4.f;     // feature frequency (repeats across the tile; rounded so the tile stays seamless)
    Vec4 color1{0.25f, 0.27f, 0.30f, 1.f};  // primary color (sRGB 0..1, written directly to 8-bit)
    Vec4 color2{0.70f, 0.72f, 0.75f, 1.f};  // secondary color
    Vec4 color3{0.10f, 0.10f, 0.12f, 1.f};  // accent (mortar / grout / veins / rust ...)
    float roughness = 0.6f;  // base roughness written into ORM.g
    float metallic = 0.f;    // ORM.b
    float variation = 0.5f;  // 0..1 amount of color / height variation
    float bump = 1.f;        // normal map strength
};

struct TextureSet {
    Image albedo;  // sRGB color, alpha = 255
    Image normal;  // tangent space, OpenGL (+Y up), encoded n * 0.5 + 0.5; flat = (128, 128, 255)
    Image orm;     // R = ambient occlusion, G = roughness, B = metallic (glTF convention)
};

/// All supported kinds: noise, marble, wood, planks, bricks, tiles, cobblestone, grass, dirt,
/// sand, rock, metal_brushed, rust, fabric, checker, stripes, hexagons, scales, stylized.
const std::vector<std::string>& kinds();

/// Sensible colors / roughness / scale for a kind (unknown kinds yield the plain `noise` defaults).
Params defaults(const std::string& kind);

/// Generates a tileable texture set. Deterministic for a given Params. Fails with
/// `invalid_argument` for an unknown kind, non power-of-two or out-of-range size, or bad scale.
Result<TextureSet> generate(const Params& p);

}  // namespace sky::texgen
