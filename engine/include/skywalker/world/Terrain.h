#pragma once
// Heightfield terrain: data, procedural generation (noise + hydraulic/thermal erosion),
// sculpt/paint brushes, material-layer weights, queries and (de)serialization.
//
// A terrain is a square grid of `resolution`^2 height samples (meters, relative to the
// entity) covering `size` x `size` meters centered on the entity, plus up to kMaxLayers
// material-layer weights per sample. The renderer displaces a shared grid mesh from the
// height texture (continuous LOD), so a terrain costs a few textures, not a mesh.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky::world {

class TerrainData {
public:
    static constexpr int kMaxLayers = 8;

    TerrainData() = default;
    TerrainData(int resolution, float size);

    int resolution() const { return res_; }
    float size() const { return size_; }
    float cell() const { return size_ / static_cast<float>(res_ - 1); }
    uint64_t version() const { return version_; }
    void touch() { ++version_; }

    float& h(int x, int z) { return heights_[static_cast<size_t>(z) * res_ + x]; }
    float h(int x, int z) const { return heights_[static_cast<size_t>(z) * res_ + x]; }
    const std::vector<float>& heights() const { return heights_; }
    std::vector<float>& mutableHeights() { return heights_; }
    /// Layer weights, kMaxLayers bytes per sample (0..255, normalized on use).
    const std::vector<uint8_t>& weights() const { return weights_; }
    uint8_t* weightsAt(int x, int z) { return weights_.data() + (static_cast<size_t>(z) * res_ + x) * kMaxLayers; }
    const uint8_t* weightsAt(int x, int z) const { return weights_.data() + (static_cast<size_t>(z) * res_ + x) * kMaxLayers; }

    /// Local coordinates: x, z in [-size/2, size/2]. Bilinear height; false outside.
    bool heightAt(float x, float z, float& out) const;
    Vec3 normalAt(float x, float z) const;
    float slopeDegAt(float x, float z) const;
    float minHeight() const;
    float maxHeight() const;

    /// Ray in local space; returns distance along the (normalized) ray or < 0.
    float raycast(Vec3 origin, Vec3 dir, float maxDist) const;

    std::vector<uint8_t> serialize() const;
    static Result<TerrainData> deserialize(const std::vector<uint8_t>& bytes);
    Status save(const std::string& path) const;
    /// 16-bit little-endian heightmap (.r16) normalized to [lo, hi] (for physics heightfields).
    Status saveHeightmap16(const std::string& path, float& lo, float& hi) const;
    static Result<TerrainData> load(const std::string& path);

private:
    int res_ = 0;
    float size_ = 0;
    std::vector<float> heights_;
    std::vector<uint8_t> weights_;
    uint64_t version_ = 1;
};

/// Procedural shapes. Every shape is deterministic for a seed.
struct TerrainGenParams {
    std::string shape = "hills";  // hills | mountains | island | coast | canyon | dunes | plains | valley
    uint32_t seed = 1;
    float minHeight = 0.f;        // meters (negative = below the entity, e.g. sea floor)
    float maxHeight = 60.f;
    float featureSize = 300.f;    // meters between big features
    int octaves = 7;
    float roughness = 0.5f;       // fBm gain
    float ridges = 0.f;           // 0..1 blend toward ridged noise (sharp crests)
    float warp = 0.4f;            // domain warp strength (natural, less "noisy" shapes)
    float erosion = 0.5f;         // 0..1 hydraulic erosion amount (droplets scale with area)
    float thermal = 0.3f;         // 0..1 talus / scree smoothing
    float terraces = 0.f;         // 0..1 stepped strata (mesas, rice terraces)
    float beachWidth = 30.f;      // island/coast: gentle shelf near sea level (meters)
    float seaLevel = 0.f;         // island/coast: height of the shoreline
};

/// Fills heights from the params (erosion included).
void generate(TerrainData& t, const TerrainGenParams& p);
TerrainGenParams genParamsFromJson(const Json& j, TerrainGenParams base = {});
Json toJson(const TerrainGenParams& p);
/// Named starting points: island_beach, tropical_coast, alpine, canyon, desert_dunes,
/// rolling_hills, mountain_valley, flat.
const std::vector<std::string>& terrainPresets();
Result<TerrainGenParams> terrainPreset(const std::string& name);

/// Hydraulic droplet erosion (deterministic). `droplets` ~ 1 per cell gives a light pass.
void erodeHydraulic(TerrainData& t, int droplets, uint32_t seed, float strength = 1.f);
void erodeThermal(TerrainData& t, int iterations, float talusDeg = 35.f);

/// Automatic layer weights from rules (height / slope / noise). `rules` is the terrain's
/// layer list ([{name, heightMin, heightMax, slopeMin, slopeMax, noise, sharpness}...]);
/// later layers paint over earlier ones where their rules match (layer 0 = base).
void autoPaint(TerrainData& t, const Json& layers, uint32_t seed);

/// Brushes (local coordinates, meters). `strength` per call (agents apply strokes).
enum class SculptMode { Raise, Lower, Flatten, Smooth, Noise, Set };
void sculpt(TerrainData& t, Vec2 center, float radius, float strength, SculptMode mode, float target = 0.f,
            float falloff = 0.5f, uint32_t seed = 1);
void paint(TerrainData& t, Vec2 center, float radius, int layer, float strength, float falloff = 0.5f);

}  // namespace sky::world
