#include "skywalker/world/Foliage.h"

#include <algorithm>
#include <cmath>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"

namespace sky::world {

namespace {

uint32_t mix3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}
float u01(uint32_t h) { return static_cast<float>(h >> 8) * (1.f / 16777216.f); }

float valueNoise(float x, float z, uint32_t seed) {
    int xi = static_cast<int>(std::floor(x)), zi = static_cast<int>(std::floor(z));
    float fx = x - xi, fz = z - zi;
    fx = fx * fx * (3 - 2 * fx);
    fz = fz * fz * (3 - 2 * fz);
    auto v = [&](int a, int b) { return u01(mix3(static_cast<uint32_t>(a), static_cast<uint32_t>(b), seed)); };
    float a = v(xi, zi) + (v(xi + 1, zi) - v(xi, zi)) * fx;
    float b = v(xi, zi + 1) + (v(xi + 1, zi + 1) - v(xi, zi + 1)) * fx;
    return a + (b - a) * fz;
}

float sstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}

}  // namespace

std::vector<FoliageLayer> foliageLayersFromJson(const Json& layers) {
    std::vector<FoliageLayer> out;
    if (!layers.isArray()) return out;
    for (size_t i = 0; i < layers.size(); ++i) {
        // A layer may start from a preset and override any field.
        Json src = layers[i].contains("preset") ? foliagePreset(layers[i].get("preset").asString()) : Json::object();
        for (const auto& [k, v] : layers[i].members()) src[k] = v;
        FoliageLayer l;
        auto rd = [&](const char* k, float& v) {
            if (src.contains(k)) v = src.get(k).asFloat();
        };
        l.name = src.get("name").asString(src.get("preset").asString("layer" + std::to_string(i)));
        l.mesh = src.get("mesh").asString(l.mesh);
        l.material = src.get("material").asString();
        l.prefab = src.get("prefab").asString();
        l.texture = src.get("texture").asString();
        l.normalMap = src.get("normalMap").asString();
        l.ormMap = src.get("ormMap").asString();
        l.triplanar = src.get("triplanar").asBool(false);
        rd("tiling", l.tiling);
        if (src.contains("color")) reflect::jsonToColor(src.get("color"), l.color);
        rd("roughness", l.roughness), rd("subsurface", l.subsurface), rd("density", l.density), rd("scaleMin", l.scaleMin),
            rd("scaleMax", l.scaleMax), rd("slopeMin", l.slopeMin), rd("slopeMax", l.slopeMax), rd("heightMin", l.heightMin),
            rd("heightMax", l.heightMax), rd("layerThreshold", l.layerThreshold), rd("alignToNormal", l.alignToNormal),
            rd("clumping", l.clumping), rd("sink", l.sink), rd("colorVariation", l.colorVariation), rd("wind", l.wind),
            rd("cullDistance", l.cullDistance), rd("randomTilt", l.randomTilt);
        if (src.contains("terrainLayer")) l.terrainLayer = static_cast<int>(src.get("terrainLayer").asInt(-1));
        if (src.contains("castShadows")) l.castShadows = src.get("castShadows").asBool(true);
        if (src.contains("seed")) l.seed = static_cast<uint32_t>(src.get("seed").asInt());
        l.density = std::clamp(l.density, 0.f, 64.f);
        l.cullDistance = std::clamp(l.cullDistance, 4.f, 4000.f);
        if (l.scaleMax < l.scaleMin) std::swap(l.scaleMin, l.scaleMax);
        out.push_back(l);
    }
    return out;
}

const std::vector<std::string>& foliagePresets() {
    static const std::vector<std::string> v = {"meadow_grass", "tall_grass",  "dune_grass",  "beach_pebbles", "shells",
                                               "flowers",      "ferns",       "rocks_small", "boulders",      "custom"};
    return v;
}

Json foliagePreset(const std::string& name) {
    if (name == "meadow_grass")
        return Json::object({{"mesh", "grass"}, {"color", "#5a7a2c"}, {"density", 14}, {"scaleMin", 0.7}, {"scaleMax", 1.25},
                             {"slopeMax", 38}, {"clumping", 0.35}, {"cullDistance", 70}, {"subsurface", 0.6}, {"wind", 1.0},
                             {"castShadows", false}, {"alignToNormal", 0.5}});
    if (name == "tall_grass")
        return Json::object({{"mesh", "grass_tall"}, {"color", "#7a8a3a"}, {"density", 4}, {"scaleMin", 0.8}, {"scaleMax", 1.4},
                             {"slopeMax", 30}, {"clumping", 0.7}, {"cullDistance", 90}, {"subsurface", 0.6}, {"wind", 1.3},
                             {"castShadows", true}, {"alignToNormal", 0.3}});
    if (name == "dune_grass")
        return Json::object({{"mesh", "grass_tall"}, {"color", "#b7a96a"}, {"density", 0.9}, {"scaleMin", 0.6}, {"scaleMax", 1.3},
                             {"slopeMax", 32}, {"clumping", 0.85}, {"cullDistance", 120}, {"subsurface", 0.4}, {"wind", 1.6},
                             {"castShadows", true}, {"alignToNormal", 0.2}, {"colorVariation", 0.5}});
    if (name == "beach_pebbles")
        return Json::object({{"mesh", "pebbles"}, {"color", "#8d8577"}, {"density", 0.5}, {"scaleMin", 0.6}, {"scaleMax", 1.6},
                             {"slopeMax", 40}, {"clumping", 0.75}, {"cullDistance", 45}, {"subsurface", 0}, {"wind", 0},
                             {"roughness", 0.55}, {"castShadows", true}, {"alignToNormal", 1.0}, {"sink", 0.01}});
    if (name == "shells")
        return Json::object({{"mesh", "shell"}, {"color", "#e8dccb"}, {"density", 0.25}, {"scaleMin", 0.6}, {"scaleMax", 1.3},
                             {"slopeMax", 30}, {"clumping", 0.6}, {"cullDistance", 30}, {"subsurface", 0.2}, {"wind", 0},
                             {"roughness", 0.35}, {"castShadows", true}, {"alignToNormal", 1.0}, {"sink", 0.005},
                             {"colorVariation", 0.6}, {"randomTilt", 25}});
    if (name == "flowers")
        return Json::object({{"mesh", "flowers"}, {"color", "#ffffff"}, {"density", 1.2}, {"scaleMin", 0.7}, {"scaleMax", 1.2},
                             {"slopeMax", 30}, {"clumping", 0.85}, {"cullDistance", 60}, {"subsurface", 0.5}, {"wind", 1.2},
                             {"castShadows", false}, {"alignToNormal", 0.3}});
    if (name == "ferns")
        return Json::object({{"mesh", "fern"}, {"color", "#4f6e2a"}, {"density", 0.6}, {"scaleMin", 0.7}, {"scaleMax", 1.5},
                             {"slopeMax", 45}, {"clumping", 0.8}, {"cullDistance", 80}, {"subsurface", 0.6}, {"wind", 0.7},
                             {"castShadows", true}, {"alignToNormal", 0.4}});
    if (name == "rocks_small")
        return Json::object({{"mesh", "rock"}, {"color", "#b5afa6"}, {"texgen", "rock"}, {"triplanar", true}, {"tiling", 1.2}, {"density", 0.08}, {"scaleMin", 0.25}, {"scaleMax", 0.9},
                             {"slopeMax", 60}, {"heightMin", 2.5}, {"clumping", 0.7}, {"cullDistance", 160}, {"subsurface", 0}, {"wind", 0},
                             {"roughness", 0.8}, {"castShadows", true}, {"alignToNormal", 0.8}, {"sink", 0.12}, {"randomTilt", 30}});
    if (name == "boulders")
        return Json::object({{"mesh", "rock"}, {"color", "#aaa49b"}, {"texgen", "rock"}, {"triplanar", true}, {"tiling", 0.5}, {"density", 0.0007}, {"scaleMin", 1.5}, {"scaleMax", 4.5},
                             {"slopeMax", 70}, {"clumping", 0.5}, {"cullDistance", 900}, {"subsurface", 0}, {"wind", 0},
                             {"roughness", 0.85}, {"castShadows", true}, {"alignToNormal", 0.6}, {"sink", 0.5}, {"randomTilt", 40}});
    return Json::object({{"mesh", "grass"}});
}

std::vector<FoliageInstance> scatterChunk(const FoliageLayer& l, int layerIndex, uint32_t seed, int cx, int cz,
                                          float chunkSize, const SurfaceFn& surface, float meshHeight) {
    std::vector<FoliageInstance> out;
    if (l.density <= 0.f) return out;
    const float cell = 1.f / std::sqrt(l.density);
    const int cells = std::max(1, static_cast<int>(std::ceil(chunkSize / cell)));
    const float step = chunkSize / static_cast<float>(cells);
    const uint32_t lseed = mix3(seed, l.seed + static_cast<uint32_t>(layerIndex) * 977u, 0xF011A6Eu);
    out.reserve(static_cast<size_t>(cells) * cells / 2);
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            int gx = cx * cells + i, gz = cz * cells + j;
            uint32_t h = mix3(static_cast<uint32_t>(gx), static_cast<uint32_t>(gz), lseed);
            float x = (static_cast<float>(cx) * chunkSize) + (static_cast<float>(i) + u01(h)) * step;
            float z = (static_cast<float>(cz) * chunkSize) + (static_cast<float>(j) + u01(mix3(h, 1, 2))) * step;
            // Clumps: patches of high density separated by sparse ground.
            if (l.clumping > 0.f) {
                float n = valueNoise(x * 0.12f, z * 0.12f, lseed) * 0.65f + valueNoise(x * 0.5f, z * 0.5f, lseed + 5) * 0.35f;
                float keep = 1.f + (sstep(0.35f, 0.75f, n) * 1.4f - 1.f) * l.clumping;
                if (u01(mix3(h, 3, 4)) > keep) continue;
            }
            SurfaceSample s;
            if (!surface(x, z, s)) continue;
            if (s.y < l.heightMin || s.y > l.heightMax) continue;
            float slope = std::acos(std::clamp(s.normal.y, -1.f, 1.f)) * 57.2957795f;
            if (slope < l.slopeMin || slope > l.slopeMax) continue;
            if (l.terrainLayer >= 0 && l.terrainLayer < 8) {
                float w = s.layerWeights[l.terrainLayer];
                if (w < l.layerThreshold * u01(mix3(h, 5, 6)) * 1.5f || w < 0.05f) continue;
            }
            // Orientation: up blends from world up to the ground normal, plus a random tilt.
            float tiltA = u01(mix3(h, 7, 8)) * 6.2831853f, tiltR = l.randomTilt * 0.0174533f * u01(mix3(h, 9, 10));
            Vec3 up = normalize(Vec3{0, 1, 0} + (s.normal - Vec3{0, 1, 0}) * l.alignToNormal);
            up = normalize(up + Vec3{std::cos(tiltA), 0, std::sin(tiltA)} * std::tan(tiltR));
            float yaw = u01(mix3(h, 11, 12)) * 6.2831853f;
            Vec3 ref = std::fabs(up.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            Vec3 t0 = normalize(cross(ref, up));
            Vec3 b0 = cross(up, t0);
            Vec3 right = t0 * std::cos(yaw) + b0 * std::sin(yaw);
            Vec3 fwd = cross(right, up);
            float sc = l.scaleMin + (l.scaleMax - l.scaleMin) * u01(mix3(h, 13, 14));
            Vec3 pos{x, s.y - l.sink * sc, z};
            FoliageInstance inst{};
            // Columns: right * sc, up * sc, fwd * sc, pos; stored as rows.
            inst.row0[0] = right.x * sc, inst.row0[1] = up.x * sc, inst.row0[2] = fwd.x * sc, inst.row0[3] = pos.x;
            inst.row1[0] = right.y * sc, inst.row1[1] = up.y * sc, inst.row1[2] = fwd.y * sc, inst.row1[3] = pos.y;
            inst.row2[0] = right.z * sc, inst.row2[1] = up.z * sc, inst.row2[2] = fwd.z * sc, inst.row2[3] = pos.z;
            inst.tint = (u01(mix3(h, 15, 16)) * 2.f - 1.f) * l.colorVariation;
            inst.phase = u01(mix3(h, 17, 18));
            inst.height = std::max(meshHeight * sc, 0.05f);
            inst.fade = u01(mix3(h, 19, 20));
            out.push_back(inst);
        }
    }
    return out;
}

std::vector<FoliageChunk> FoliageCache::visibleChunks(const FoliageLayer& layer, int layerIndex, uint32_t seed, Vec3 eye,
                                                      Vec2 areaMin, Vec2 areaMax, const SurfaceFn& surface,
                                                      float meshHeight, int& budget) {
    ++tick_;
    std::vector<FoliageChunk> out;
    const float cs = kChunkSize, r = layer.cullDistance;
    int cx0 = static_cast<int>(std::floor(std::max(eye.x - r, areaMin.x) / cs));
    int cx1 = static_cast<int>(std::floor(std::min(eye.x + r, areaMax.x - 1e-3f) / cs));
    int cz0 = static_cast<int>(std::floor(std::max(eye.z - r, areaMin.y) / cs));
    int cz1 = static_cast<int>(std::floor(std::min(eye.z + r, areaMax.y - 1e-3f) / cs));
    // Nearest chunks first, so a limited generation budget fills in around the camera.
    std::vector<std::pair<float, std::pair<int, int>>> order;
    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            float mx = (cx + 0.5f) * cs - eye.x, mz = (cz + 0.5f) * cs - eye.z;
            float d = std::sqrt(mx * mx + mz * mz) - cs * 0.7071f;
            if (d > r) continue;
            order.push_back({d, {cx, cz}});
        }
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [d, c] : order) {
        FoliageChunkKey key{layerIndex, c.first, c.second};
        auto it = chunks_.find(key);
        if (it == chunks_.end()) {
            if (budget <= 0) continue;
            --budget;
            Entry e;
            auto inst = std::make_shared<std::vector<FoliageInstance>>(
                scatterChunk(layer, layerIndex, seed, c.first, c.second, cs, surface, meshHeight));
            Aabb b;
            b.min = {1e30f, 1e30f, 1e30f};
            b.max = {-1e30f, -1e30f, -1e30f};
            for (const auto& i : *inst) {
                Vec3 p{i.row0[3], i.row1[3], i.row2[3]};
                b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
                b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y + i.height), std::max(b.max.z, p.z)};
            }
            float pad = meshHeight * layer.scaleMax;
            b.min = b.min - Vec3{pad, pad * 0.2f, pad};
            b.max = b.max + Vec3{pad, pad * 0.2f, pad};
            e.chunk.instances = std::move(inst);
            e.chunk.bounds = b;
            e.chunk.id = nextId_++;
            it = chunks_.emplace(key, std::move(e)).first;
        }
        it->second.lastUse = tick_;
        if (!it->second.chunk.instances->empty()) out.push_back(it->second.chunk);
    }
    // Evict chunks unused for a while (camera moved away).
    if (chunks_.size() > 4096) {
        for (auto i = chunks_.begin(); i != chunks_.end();) i = tick_ - i->second.lastUse > 120 ? chunks_.erase(i) : std::next(i);
    }
    return out;
}

size_t FoliageCache::instanceCount() const {
    size_t n = 0;
    for (const auto& [k, e] : chunks_) n += e.chunk.instances->size();
    return n;
}

}  // namespace sky::world
