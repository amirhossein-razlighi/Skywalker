#pragma once
// World-building components: heightfield terrain and instanced foliage.

#include <string>

#include "skywalker/core/Json.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// A large heightfield (mountains, islands, beaches, canyons) with up to 8 blended material
/// layers, erosion-based generation, sculpting and painting. Rendered with continuous LOD
/// from a height texture, so kilometer-scale worlds stay cheap. Create one with
/// terrain_create; edit with terrain_sculpt / terrain_paint / terrain_layers.
struct Terrain {
    std::string data;            // project-relative .terrain file (heights + layer weights)
    float size = 512.f;          // square extent in meters, centered on the entity
    int resolution = 513;        // height samples per side (2^n + 1)
    Json generator = Json::object();  // generation params used for (re)generation (shape, seed, heights, erosion...)
    Json layers = Json::array();      // material layers: [{name, texture, normalMap, ormMap, color, roughness, tiling, rules...}]
    float waterLevel = -100000.f;     // world height of nearby water: sand and soil look wet below it + wetBand
    float wetBand = 1.2f;             // meters above the water line that stay damp (shorelines, wave run-up)
    float detail = 1.f;               // LOD quality multiplier (0.5 faster .. 2 sharper)
    bool castShadows = true;

    static const TypeInfo& type();
};

/// Scatters instanced meshes (grass, flowers, pebbles, shells, ferns, rocks, trees) over the
/// terrain on this entity (or its parent), or over any scene geometry inside `area`. Dense
/// layers are generated in chunks around the camera; everything is GPU-instanced, wind
/// animated and deterministic for a seed. Start from presets with foliage_add.
struct Foliage {
    Json layers = Json::array();  // [{preset?, mesh, color, density (/m²), scale, slope, height, terrainLayer, wind, cullDistance...}]
    int seed = 1;
    float density = 1.f;          // multiplier for every layer (quick quality/perf knob)
    std::string surface = "terrain";  // terrain | scene (raycast onto meshes inside `area`)
    Vec3 area{40.f, 20.f, 40.f};       // scene mode: extent (m) centered on the entity
    bool visible = true;

    static const TypeInfo& type();
};

}  // namespace sky
