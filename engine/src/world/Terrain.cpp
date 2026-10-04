#include "skywalker/world/Terrain.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "skywalker/core/Strings.h"
#include "stb_image.h"

namespace sky::world {

namespace {

constexpr char kMagic[8] = {'S', 'K', 'Y', 'T', 'E', 'R', 'R', '1'};

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

float unit(uint32_t h) { return static_cast<float>(h >> 8) * (1.f / 16777216.f); }

float smooth5(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

/// Gradient noise in [-1, 1] (Perlin-style, hashed gradients).
float gradNoise(float x, float y, uint32_t seed) {
    int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    float xf = x - static_cast<float>(xi), yf = y - static_cast<float>(yi);
    auto grad = [&](int ix, int iy, float dx, float dy) {
        float a = unit(hash3(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), seed)) * 6.2831853f;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    float u = smooth5(xf), v = smooth5(yf);
    float n00 = grad(xi, yi, xf, yf), n10 = grad(xi + 1, yi, xf - 1, yf);
    float n01 = grad(xi, yi + 1, xf, yf - 1), n11 = grad(xi + 1, yi + 1, xf - 1, yf - 1);
    float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.41421356f;
}

float fbm(float x, float y, int octaves, float gain, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < octaves; ++i) {
        sum += gradNoise(x, y, seed + static_cast<uint32_t>(i) * 1013u) * amp;
        norm += amp;
        // Rotate between octaves to hide grid alignment.
        float nx = x * 1.6f - y * 1.2f, ny = x * 1.2f + y * 1.6f;
        x = nx + 17.3f;
        y = ny - 9.1f;
        amp *= gain;
    }
    return sum / norm;
}

/// Ridged multifractal in [0, 1]: sharp crests, smooth valleys.
float ridgedFbm(float x, float y, int octaves, float gain, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0, weight = 1.f;
    for (int i = 0; i < octaves; ++i) {
        float n = 1.f - std::fabs(gradNoise(x, y, seed + static_cast<uint32_t>(i) * 7919u));
        n *= n;
        n *= weight;
        weight = std::clamp(n * 2.f, 0.f, 1.f);
        sum += n * amp;
        norm += amp;
        float nx = x * 1.6f - y * 1.2f, ny = x * 1.2f + y * 1.6f;
        x = nx + 31.7f;
        y = ny + 4.3f;
        amp *= gain;
    }
    return sum / norm;
}

float sstep(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

}  // namespace

// ---------------------------------------------------------------------------
// TerrainData
// ---------------------------------------------------------------------------

TerrainData::TerrainData(int resolution, float size)
    : res_(std::max(resolution, 2)),
      size_(std::max(size, 1.f)),
      heights_(static_cast<size_t>(res_) * res_, 0.f),
      weights_(static_cast<size_t>(res_) * res_ * kMaxLayers, 0) {
    for (size_t i = 0; i < static_cast<size_t>(res_) * res_; ++i) weights_[i * kMaxLayers] = 255;
}

bool TerrainData::heightAt(float x, float z, float& out) const {
    if (res_ < 2) return false;
    float fx = (x / size_ + 0.5f) * static_cast<float>(res_ - 1);
    float fz = (z / size_ + 0.5f) * static_cast<float>(res_ - 1);
    if (fx < 0 || fz < 0 || fx > static_cast<float>(res_ - 1) || fz > static_cast<float>(res_ - 1)) return false;
    int x0 = std::min(static_cast<int>(fx), res_ - 2), z0 = std::min(static_cast<int>(fz), res_ - 2);
    float tx = fx - static_cast<float>(x0), tz = fz - static_cast<float>(z0);
    float a = h(x0, z0) + (h(x0 + 1, z0) - h(x0, z0)) * tx;
    float b = h(x0, z0 + 1) + (h(x0 + 1, z0 + 1) - h(x0, z0 + 1)) * tx;
    out = a + (b - a) * tz;
    return true;
}

Vec3 TerrainData::normalAt(float x, float z) const {
    float e = cell();
    float hl = 0, hr = 0, hd = 0, hu = 0, hc = 0;
    heightAt(x, z, hc);
    if (!heightAt(x - e, z, hl)) hl = hc;
    if (!heightAt(x + e, z, hr)) hr = hc;
    if (!heightAt(x, z - e, hd)) hd = hc;
    if (!heightAt(x, z + e, hu)) hu = hc;
    return normalize(Vec3{hl - hr, 2.f * e, hd - hu});
}

float TerrainData::slopeDegAt(float x, float z) const {
    Vec3 n = normalAt(x, z);
    return std::acos(std::clamp(n.y, -1.f, 1.f)) * 57.2957795f;
}

float TerrainData::minHeight() const { return heights_.empty() ? 0.f : *std::min_element(heights_.begin(), heights_.end()); }
float TerrainData::maxHeight() const { return heights_.empty() ? 0.f : *std::max_element(heights_.begin(), heights_.end()); }

float TerrainData::raycast(Vec3 o, Vec3 d, float maxDist) const {
    if (res_ < 2) return -1.f;
    // Clip to the terrain's bounding box first.
    float half = size_ * 0.5f, lo = minHeight() - 0.01f, hi = maxHeight() + 0.01f;
    float t0 = 0.f, t1 = maxDist;
    const float bmin[3] = {-half, lo, -half}, bmax[3] = {half, hi, half};
    const float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dd[a]) < 1e-9f) {
            if (oo[a] < bmin[a] || oo[a] > bmax[a]) return -1.f;
            continue;
        }
        float ta = (bmin[a] - oo[a]) / dd[a], tb = (bmax[a] - oo[a]) / dd[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return -1.f;
    }
    float step = cell() * 0.5f;
    float prevT = t0;
    float hgt = 0;
    Vec3 p = o + d * t0;
    if (heightAt(p.x, p.z, hgt) && p.y < hgt) return t0;  // starts below the surface
    for (float t = t0 + step; t <= t1 + step; t += step) {
        float tc = std::min(t, t1);
        p = o + d * tc;
        if (heightAt(p.x, p.z, hgt) && p.y <= hgt) {
            float a = prevT, b = tc;
            for (int i = 0; i < 12; ++i) {
                float m = 0.5f * (a + b);
                Vec3 q = o + d * m;
                float hm = 0;
                if (heightAt(q.x, q.z, hm) && q.y <= hm) b = m; else a = m;
            }
            return b;
        }
        prevT = tc;
        if (tc >= t1) break;
    }
    return -1.f;
}

std::vector<uint8_t> TerrainData::serialize() const {
    std::vector<uint8_t> raw(heights_.size() * sizeof(float) + weights_.size());
    std::memcpy(raw.data(), heights_.data(), heights_.size() * sizeof(float));
    std::memcpy(raw.data() + heights_.size() * sizeof(float), weights_.data(), weights_.size());
    uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> out(8 + 4 + 4 + 4 + packedSize);
    std::memcpy(out.data(), kMagic, 8);
    uint32_t r = static_cast<uint32_t>(res_), layers = kMaxLayers;
    std::memcpy(out.data() + 8, &r, 4);
    std::memcpy(out.data() + 12, &size_, 4);
    std::memcpy(out.data() + 16, &layers, 4);
    compress2(out.data() + 20, &packedSize, raw.data(), static_cast<uLong>(raw.size()), 6);
    out.resize(20 + packedSize);
    return out;
}

Result<TerrainData> TerrainData::deserialize(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 20 || std::memcmp(bytes.data(), kMagic, 8) != 0) {
        return Error::make("invalid_terrain", "not a Skywalker terrain file (bad header)");
    }
    uint32_t r = 0, layers = 0;
    float size = 0;
    std::memcpy(&r, bytes.data() + 8, 4);
    std::memcpy(&size, bytes.data() + 12, 4);
    std::memcpy(&layers, bytes.data() + 16, 4);
    if (r < 2 || r > 8193 || layers != kMaxLayers || !(size > 0)) {
        return Error::make("invalid_terrain", "terrain header out of range");
    }
    TerrainData t(static_cast<int>(r), size);
    std::vector<uint8_t> raw(t.heights_.size() * sizeof(float) + t.weights_.size());
    uLongf rawSize = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawSize, bytes.data() + 20, static_cast<uLong>(bytes.size() - 20)) != Z_OK ||
        rawSize != raw.size()) {
        return Error::make("invalid_terrain", "terrain data is corrupt or truncated");
    }
    std::memcpy(t.heights_.data(), raw.data(), t.heights_.size() * sizeof(float));
    std::memcpy(t.weights_.data(), raw.data() + t.heights_.size() * sizeof(float), t.weights_.size());
    return t;
}

Status TerrainData::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return Error::make("io_error", "cannot write " + path);
    auto bytes = serialize();
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f ? Status{} : Status(Error::make("io_error", "failed writing " + path));
}

Status TerrainData::saveHeightmap16(const std::string& path, float& lo, float& hi) const {
    lo = minHeight();
    hi = maxHeight();
    const float range = std::max(hi - lo, 0.01f);
    std::vector<uint8_t> raw(heights_.size() * 2);
    for (size_t i = 0; i < heights_.size(); ++i) {
        auto v = static_cast<uint16_t>(std::lround(std::clamp((heights_[i] - lo) / range, 0.f, 1.f) * 65535.f));
        raw[i * 2] = static_cast<uint8_t>(v & 0xff);
        raw[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    return f ? Status{} : Status(Error::make("io_error", "cannot write " + path));
}

Result<TerrainData> TerrainData::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "terrain file not found: " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return deserialize(bytes);
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

void generate(TerrainData& t, const TerrainGenParams& p) {
    const int n = t.resolution();
    const float size = t.size();
    const float freq = 1.f / std::max(p.featureSize, 1.f);
    const float range = p.maxHeight - p.minHeight;
    const uint32_t s = p.seed * 2654435761u + 7u;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            float wx = (static_cast<float>(x) / (n - 1) - 0.5f) * size;
            float wz = (static_cast<float>(z) / (n - 1) - 0.5f) * size;
            // Domain warp: bends features into natural, flowing shapes.
            float qx = wx * freq, qz = wz * freq;
            float w1 = fbm(qx * 0.7f + 3.1f, qz * 0.7f - 1.7f, 4, 0.5f, s + 11);
            float w2 = fbm(qx * 0.7f - 5.3f, qz * 0.7f + 2.9f, 4, 0.5f, s + 23);
            qx += w1 * p.warp * 1.5f;
            qz += w2 * p.warp * 1.5f;
            float smoothN = fbm(qx, qz, p.octaves, p.roughness, s) * 0.5f + 0.5f;
            float ridgeN = ridgedFbm(qx * 0.9f, qz * 0.9f, p.octaves, p.roughness, s + 101);
            float v = smoothN + (ridgeN - smoothN) * p.ridges;  // 0..1
            float u = static_cast<float>(x) / (n - 1) * 2.f - 1.f, w = static_cast<float>(z) / (n - 1) * 2.f - 1.f;
            float height = p.minHeight + v * range;
            if (p.shape == "island" || p.shape == "coast") {
                // Continentality c: > 0 on land, < 0 at sea, with a noisy, natural coastline.
                float coastNoise = fbm(wx * freq * 1.8f + 9.f, wz * freq * 1.8f - 4.f, 5, 0.55f, s + 59) * 0.28f +
                                   fbm(wx * freq * 7.f, wz * freq * 7.f, 3, 0.5f, s + 61) * 0.05f;
                float d = p.shape == "island" ? std::sqrt(u * u + w * w) : (w * 0.5f + 0.5f) * 1.25f;
                float c = (p.shape == "island" ? 0.72f : 0.62f) - d + coastNoise;
                // Profile: sea floor -> gentle beach (beachWidth meters wide) -> rolling land.
                float halfSize = size * 0.5f;
                float beachC = std::max(p.beachWidth, 4.f) / halfSize;  // beach width in c units
                float beachRise = std::max(p.beachWidth * 0.045f, 0.6f);  // ~2.5 degree slope
                if (c < 0.f) {
                    // Underwater: shallow shelf first, then deeper.
                    float k = sstep(0.f, 0.35f, -c);
                    height = p.seaLevel - 0.6f * sstep(0.f, beachC * 0.6f, -c) - (p.seaLevel - p.minHeight) * k * k;
                } else if (c < beachC) {
                    height = p.seaLevel + beachRise * (c / beachC);
                } else {
                    float inland = sstep(beachC, beachC + 0.35f, c);
                    float land = std::pow(v, 1.3f) * (p.maxHeight - p.seaLevel - beachRise);
                    // Dunes / berm behind the beach, then hills.
                    float berm = std::exp(-std::pow((c - beachC) / (beachC * 0.6f + 1e-3f) - 1.f, 2.f)) * beachRise * 0.8f;
                    height = p.seaLevel + beachRise + land * inland + berm * (1.f - inland);
                }
            } else if (p.shape == "canyon") {
                // Plateau cut by winding channels.
                float ch = std::fabs(fbm(qx * 0.8f, qz * 0.8f, 5, 0.5f, s + 77));
                float carve = sstep(0.02f, 0.16f, ch);
                height = p.minHeight + range * (0.25f + 0.75f * carve) * (0.85f + 0.15f * v);
            } else if (p.shape == "dunes") {
                float dir = 0.6f;
                float along = wx * std::cos(dir) + wz * std::sin(dir);
                float dune = std::pow(0.5f + 0.5f * std::sin(along * freq * 6.2831853f * 2.f + w1 * 4.f), 2.2f);
                height = p.minHeight + range * (dune * 0.7f + smoothN * 0.3f);
            } else if (p.shape == "valley") {
                float side = sstep(0.1f, 0.85f, std::fabs(u + w1 * 0.15f));
                height = p.minHeight + range * (0.08f * smoothN + side * (0.4f + 0.6f * ridgeN));
            } else if (p.shape == "plains") {
                height = p.minHeight + range * smoothN * 0.35f;
            } else if (p.shape == "mountains") {
                height = p.minHeight + range * std::pow(0.15f * smoothN + 0.85f * ridgeN, 1.35f);
            } else if (p.shape == "heightmap") {
                float img = p.image ? p.image->sample(static_cast<float>(x) / (n - 1), static_cast<float>(z) / (n - 1)) : 0.f;
                height = p.minHeight + range * img + (v * 2.f - 1.f) * p.detailNoise;
            }
            if (p.terraces > 0.f) {
                float steps = 8.f + 10.f * (1.f - p.terraces);
                float tq = (height - p.minHeight) / std::max(range, 1e-3f) * steps;
                float fl = std::floor(tq), fr = tq - fl;
                float terr = (fl + sstep(0.f, 1.f, std::pow(fr, 4.f))) / steps * range + p.minHeight;
                height = height + (terr - height) * p.terraces;
            }
            t.h(x, z) = height;
        }
    }
    if (p.erosion > 0.f) {
        int droplets = static_cast<int>(static_cast<float>(n) * static_cast<float>(n) * 0.9f * p.erosion);
        erodeHydraulic(t, droplets, p.seed + 1, 0.6f + p.erosion);
    }
    if (p.thermal > 0.f) erodeThermal(t, static_cast<int>(4 + p.thermal * 20.f), 38.f - p.thermal * 8.f);
    t.touch();
}

TerrainGenParams genParamsFromJson(const Json& j, TerrainGenParams p) {
    if (!j.isObject()) return p;
    if (j.contains("shape")) p.shape = j.get("shape").asString();
    if (j.contains("seed")) p.seed = static_cast<uint32_t>(j.get("seed").asInt());
    auto f = [&](const char* k, float& v) {
        if (j.contains(k)) v = j.get(k).asFloat();
    };
    f("minHeight", p.minHeight);
    f("maxHeight", p.maxHeight);
    f("featureSize", p.featureSize);
    f("roughness", p.roughness);
    f("ridges", p.ridges);
    f("warp", p.warp);
    f("erosion", p.erosion);
    f("thermal", p.thermal);
    f("terraces", p.terraces);
    f("beachWidth", p.beachWidth);
    f("seaLevel", p.seaLevel);
    f("detailNoise", p.detailNoise);
    if (j.contains("heightmap") && j.get("heightmap").asString() != p.heightmap) {
        p.heightmap = j.get("heightmap").asString();
        p.image.reset();
    }
    if (j.contains("octaves")) p.octaves = static_cast<int>(std::clamp<int64_t>(j.get("octaves").asInt(), 1, 12));
    p.roughness = std::clamp(p.roughness, 0.1f, 0.9f);
    return p;
}

Json toJson(const TerrainGenParams& p) {
    Json j = Json::object({{"shape", p.shape},           {"seed", static_cast<int64_t>(p.seed)},
                           {"minHeight", p.minHeight},   {"maxHeight", p.maxHeight},
                           {"featureSize", p.featureSize}, {"octaves", p.octaves},
                           {"roughness", p.roughness},   {"ridges", p.ridges},
                           {"warp", p.warp},             {"erosion", p.erosion},
                           {"thermal", p.thermal},       {"terraces", p.terraces},
                           {"beachWidth", p.beachWidth}, {"seaLevel", p.seaLevel}});
    if (!p.heightmap.empty()) j["heightmap"] = p.heightmap;
    if (p.detailNoise != 0.f) j["detailNoise"] = p.detailNoise;
    return j;
}

// ---------------------------------------------------------------------------
// Heightmap images
// ---------------------------------------------------------------------------

float HeightImage::sample(float u, float v) const {
    if (width <= 0 || height <= 0 || values.empty()) return 0.f;
    float x = std::clamp(u, 0.f, 1.f) * static_cast<float>(width - 1);
    float y = std::clamp(v, 0.f, 1.f) * static_cast<float>(height - 1);
    int x0 = std::min(static_cast<int>(x), std::max(width - 2, 0)), y0 = std::min(static_cast<int>(y), std::max(height - 2, 0));
    int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    auto at = [&](int xi, int yi) { return values[static_cast<size_t>(yi) * width + xi]; };
    float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return a + (b - a) * ty;
}

Result<HeightImage> loadHeightImage(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "heightmap not found: " + path, "paths are project-relative, e.g. maps/height.png");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    HeightImage img;
    std::string ext = path.size() >= 4 ? str::lower(path.substr(path.size() - 4)) : "";
    if (ext == ".r16" || ext == ".raw") {
        size_t count = bytes.size() / 2;
        int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(count))));
        if (side < 2 || static_cast<size_t>(side) * side * 2 != bytes.size()) {
            return Error::make("invalid_heightmap", "raw heightmap " + path + " is not a square 16-bit grid",
                               "write width*width little-endian uint16 values");
        }
        img.width = img.height = side;
        img.values.resize(count);
        for (size_t i = 0; i < count; ++i) img.values[i] = static_cast<float>(bytes[i * 2] | (bytes[i * 2 + 1] << 8)) / 65535.f;
        return img;
    }
    int w = 0, h = 0, n = 0;
    const int len = static_cast<int>(std::min<size_t>(bytes.size(), INT32_MAX));
    if (stbi_is_16_bit_from_memory(bytes.data(), len)) {
        stbi_us* px = stbi_load_16_from_memory(bytes.data(), len, &w, &h, &n, 1);
        if (px) {
            img.width = w, img.height = h;
            img.values.resize(static_cast<size_t>(w) * h);
            for (size_t i = 0; i < img.values.size(); ++i) img.values[i] = static_cast<float>(px[i]) / 65535.f;
            stbi_image_free(px);
        }
    } else if (stbi_uc* px = stbi_load_from_memory(bytes.data(), len, &w, &h, &n, 1)) {
        img.width = w, img.height = h;
        img.values.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < img.values.size(); ++i) img.values[i] = static_cast<float>(px[i]) / 255.f;
        stbi_image_free(px);
    }
    if (img.values.empty() || img.width < 2 || img.height < 2) {
        const char* why = stbi_failure_reason();
        return Error::make("invalid_heightmap", "cannot decode heightmap " + path + (why ? std::string(": ") + why : ""),
                           "use a grayscale PNG (16-bit for smooth slopes) or a square .r16 file");
    }
    return img;
}

Status resolveHeightmap(TerrainGenParams& p, const std::function<std::string(const std::string&)>& resolve) {
    if (p.shape != "heightmap" || p.image) return {};
    if (p.heightmap.empty()) {
        return Error::make("missing_heightmap", "shape \"heightmap\" needs generator.heightmap",
                           "pass a project-relative grayscale PNG, e.g. {\"heightmap\": \"maps/height.png\"}");
    }
    auto img = loadHeightImage(resolve ? resolve(p.heightmap) : p.heightmap);
    if (!img) return img.error();
    p.image = std::make_shared<const HeightImage>(std::move(img.value()));
    return {};
}

const std::vector<std::string>& terrainPresets() {
    static const std::vector<std::string> v = {"island_beach", "tropical_coast", "alpine",        "canyon",
                                               "desert_dunes", "rolling_hills",  "mountain_valley", "flat"};
    return v;
}

Result<TerrainGenParams> terrainPreset(const std::string& name) {
    TerrainGenParams p;
    if (name == "island_beach") {
        p.shape = "island", p.minHeight = -18, p.maxHeight = 48, p.featureSize = 260, p.ridges = 0.35f, p.erosion = 0.6f,
        p.beachWidth = 40;
    } else if (name == "tropical_coast") {
        p.shape = "coast", p.minHeight = -16, p.maxHeight = 70, p.featureSize = 320, p.ridges = 0.5f, p.erosion = 0.7f,
        p.beachWidth = 55;
    } else if (name == "alpine") {
        p.shape = "mountains", p.minHeight = 0, p.maxHeight = 420, p.featureSize = 900, p.ridges = 0.85f, p.erosion = 0.9f,
        p.thermal = 0.5f, p.octaves = 8;
    } else if (name == "canyon") {
        p.shape = "canyon", p.minHeight = 0, p.maxHeight = 140, p.featureSize = 600, p.terraces = 0.55f, p.erosion = 0.5f;
    } else if (name == "desert_dunes") {
        p.shape = "dunes", p.minHeight = 0, p.maxHeight = 26, p.featureSize = 180, p.erosion = 0.f, p.thermal = 0.6f, p.warp = 0.8f;
    } else if (name == "rolling_hills") {
        p.shape = "hills", p.minHeight = 0, p.maxHeight = 45, p.featureSize = 380, p.ridges = 0.15f, p.erosion = 0.4f;
    } else if (name == "mountain_valley") {
        p.shape = "valley", p.minHeight = 0, p.maxHeight = 260, p.featureSize = 700, p.ridges = 0.7f, p.erosion = 0.8f, p.thermal = 0.4f;
    } else if (name == "flat") {
        p.shape = "plains", p.minHeight = 0, p.maxHeight = 2, p.featureSize = 200, p.erosion = 0.f, p.thermal = 0.f;
    } else {
        std::string guess = str::closest(name, terrainPresets());
        return Error::make("unknown_preset", "no terrain preset '" + name + "'", guess.empty() ? "" : "did you mean '" + guess + "'?");
    }
    return p;
}

// ---------------------------------------------------------------------------
// Erosion
// ---------------------------------------------------------------------------

void erodeHydraulic(TerrainData& t, int droplets, uint32_t seed, float strength) {
    const int n = t.resolution();
    if (n < 8 || droplets <= 0) return;
    // Brush: erosion is spread over a small radius so channels are smooth, not pitted.
    const int radius = 3;
    std::vector<std::pair<int, float>> brush;  // offsets (dz * n + dx) and weights
    std::vector<std::pair<int, int>> brushXY;
    float wsum = 0;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            float d = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            if (d > radius) continue;
            float w = 1.f - d / radius;
            brushXY.push_back({dx, dz});
            brush.push_back({dz * n + dx, w});
            wsum += w;
        }
    }
    for (auto& b : brush) b.second /= wsum;
    const float cellSize = t.cell();
    // Parameters are in "height per cell" units; scale by the cell size so results do not
    // depend on resolution.
    const float inertia = 0.05f, capacityK = 4.f, minCapacity = 0.01f, depositK = 0.3f, erodeK = 0.3f * strength,
                evaporate = 0.015f, gravity = 4.f;
    const int lifetime = 40;
    std::vector<float>& H = t.mutableHeights();
    auto heightGrad = [&](float px, float pz, float& gx, float& gz) {
        int x = static_cast<int>(px), z = static_cast<int>(pz);
        float u = px - x, v = pz - z;
        size_t i = static_cast<size_t>(z) * n + x;
        float h00 = H[i], h10 = H[i + 1], h01 = H[i + n], h11 = H[i + n + 1];
        gx = ((h10 - h00) * (1 - v) + (h11 - h01) * v) / cellSize;
        gz = ((h01 - h00) * (1 - u) + (h11 - h10) * u) / cellSize;
        return h00 * (1 - u) * (1 - v) + h10 * u * (1 - v) + h01 * (1 - u) * v + h11 * u * v;
    };
    for (int d = 0; d < droplets; ++d) {
        uint32_t hs = hash3(static_cast<uint32_t>(d), seed, 0xD2051u);
        float px = 1.f + unit(hs) * (n - 3), pz = 1.f + unit(hash3(hs, seed, 3)) * (n - 3);
        float dx = 0, dz = 0, speed = 1, water = 1, sediment = 0;
        for (int life = 0; life < lifetime; ++life) {
            int x = static_cast<int>(px), z = static_cast<int>(pz);
            float u = px - x, v = pz - z;
            float gx, gz;
            float hOld = heightGrad(px, pz, gx, gz);
            dx = dx * inertia - gx * (1 - inertia);
            dz = dz * inertia - gz * (1 - inertia);
            float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6f) break;
            dx /= len;
            dz /= len;
            px += dx;
            pz += dz;
            if (px < 1 || pz < 1 || px >= n - 2 || pz >= n - 2) break;
            float g2x, g2z;
            float hNew = heightGrad(px, pz, g2x, g2z);
            float dh = hNew - hOld;
            float capacity = std::max(-dh / cellSize * speed * water * capacityK, minCapacity) * cellSize;
            size_t i = static_cast<size_t>(z) * n + x;
            if (sediment > capacity || dh > 0) {
                float amount = dh > 0 ? std::min(dh, sediment) : (sediment - capacity) * depositK;
                sediment -= amount;
                H[i] += amount * (1 - u) * (1 - v);
                H[i + 1] += amount * u * (1 - v);
                H[i + n] += amount * (1 - u) * v;
                H[i + n + 1] += amount * u * v;
            } else {
                float amount = std::min((capacity - sediment) * erodeK, -dh);
                for (size_t b = 0; b < brush.size(); ++b) {
                    int bx = x + brushXY[b].first, bz = z + brushXY[b].second;
                    if (bx < 0 || bz < 0 || bx >= n || bz >= n) continue;
                    size_t bi = static_cast<size_t>(bz) * n + bx;
                    float take = std::min(amount * brush[b].second, std::max(H[bi] - (H[i] - 2.f * cellSize), 0.f));
                    H[bi] -= take;
                    sediment += take;
                }
            }
            speed = std::sqrt(std::max(speed * speed + dh / cellSize * gravity * -1.f, 0.f));
            water *= 1 - evaporate;
        }
    }
    t.touch();
}

void erodeThermal(TerrainData& t, int iterations, float talusDeg) {
    const int n = t.resolution();
    std::vector<float>& H = t.mutableHeights();
    const float maxDiff = std::tan(talusDeg * 0.0174533f) * t.cell();
    std::vector<float> delta(H.size());
    for (int it = 0; it < iterations; ++it) {
        std::fill(delta.begin(), delta.end(), 0.f);
        for (int z = 1; z < n - 1; ++z) {
            for (int x = 1; x < n - 1; ++x) {
                size_t i = static_cast<size_t>(z) * n + x;
                const int nb[4] = {-1, 1, -n, n};
                for (int k : nb) {
                    float diff = H[i] - H[i + k];
                    if (diff > maxDiff) {
                        float move = (diff - maxDiff) * 0.25f;
                        delta[i] -= move;
                        delta[i + k] += move;
                    }
                }
            }
        }
        for (size_t i = 0; i < H.size(); ++i) H[i] += delta[i];
    }
    t.touch();
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

void autoPaint(TerrainData& t, const Json& layers, uint32_t seed) {
    const int n = t.resolution();
    const size_t count = std::min<size_t>(layers.isArray() ? layers.size() : 0, TerrainData::kMaxLayers);
    if (count == 0) return;
    struct Rule {
        float hMin, hMax, sMin, sMax, noise, sharp;
        bool painted;  // rules-free layer: keeps hand-painted weights
    };
    std::vector<Rule> rules;
    for (size_t i = 0; i < count; ++i) {
        const Json& l = layers[i];
        Rule r{l.get("heightMin").asFloat(-1e9f), l.get("heightMax").asFloat(1e9f), l.get("slopeMin").asFloat(0.f),
               l.get("slopeMax").asFloat(90.f), l.get("noise").asFloat(0.3f), l.get("sharpness").asFloat(0.5f),
               false};
        rules.push_back(r);
    }
    const float half = t.size() * 0.5f;
    for (int z = 0; z < n; ++z) {
        for (int x = 0; x < n; ++x) {
            float wx = static_cast<float>(x) / (n - 1) * t.size() - half;
            float wz = static_cast<float>(z) / (n - 1) * t.size() - half;
            float hgt = t.h(x, z);
            float slope = t.slopeDegAt(wx, wz);
            float w[TerrainData::kMaxLayers] = {};
            w[0] = 1.f;
            for (size_t i = 1; i < count; ++i) {
                const Rule& r = rules[i];
                float nz = fbm(wx * 0.045f, wz * 0.045f, 4, 0.5f, seed + static_cast<uint32_t>(i) * 31u) * r.noise;
                float soft = 0.5f + (1.f - r.sharp) * 6.f;  // meters / degrees of blend
                float m = 1.f;
                if (r.hMin > -1e8f) m *= sstep(r.hMin - soft, r.hMin + soft, hgt + nz * 6.f);
                if (r.hMax < 1e8f) m *= 1.f - sstep(r.hMax - soft, r.hMax + soft, hgt + nz * 6.f);
                float sSoft = 1.f + (1.f - r.sharp) * 10.f;
                if (r.sMin > 0.f) m *= sstep(r.sMin - sSoft, r.sMin + sSoft, slope + nz * 12.f);
                if (r.sMax < 90.f) m *= 1.f - sstep(r.sMax - sSoft, r.sMax + sSoft, slope + nz * 12.f);
                m = std::clamp(m, 0.f, 1.f);
                for (size_t j = 0; j < i; ++j) w[j] *= 1.f - m;
                w[i] = m;
            }
            uint8_t* out = t.weightsAt(x, z);
            for (int i = 0; i < TerrainData::kMaxLayers; ++i) out[i] = static_cast<uint8_t>(std::lround(std::clamp(w[i], 0.f, 1.f) * 255.f));
        }
    }
    t.touch();
}

// ---------------------------------------------------------------------------
// Brushes
// ---------------------------------------------------------------------------

namespace {
template <class F>
void forBrush(TerrainData& t, Vec2 c, float radius, float falloff, F&& fn) {
    const int n = t.resolution();
    const float cell = t.cell(), half = t.size() * 0.5f;
    int x0 = std::max(0, static_cast<int>(std::floor((c.x - radius + half) / cell)));
    int x1 = std::min(n - 1, static_cast<int>(std::ceil((c.x + radius + half) / cell)));
    int z0 = std::max(0, static_cast<int>(std::floor((c.y - radius + half) / cell)));
    int z1 = std::min(n - 1, static_cast<int>(std::ceil((c.y + radius + half) / cell)));
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            float wx = x * cell - half, wz = z * cell - half;
            float d = std::sqrt((wx - c.x) * (wx - c.x) + (wz - c.y) * (wz - c.y)) / std::max(radius, 1e-3f);
            if (d > 1.f) continue;
            float inner = 1.f - std::clamp(falloff, 0.f, 1.f);
            float w = d <= inner ? 1.f : 1.f - sstep(inner, 1.f, d);
            fn(x, z, w, wx, wz);
        }
    }
}
}  // namespace

void sculpt(TerrainData& t, Vec2 c, float radius, float strength, SculptMode mode, float target, float falloff, uint32_t seed) {
    std::vector<float> before;
    if (mode == SculptMode::Smooth) before = t.heights();
    const int n = t.resolution();
    forBrush(t, c, radius, falloff, [&](int x, int z, float w, float wx, float wz) {
        float& h = t.h(x, z);
        switch (mode) {
            case SculptMode::Raise: h += strength * w; break;
            case SculptMode::Lower: h -= strength * w; break;
            case SculptMode::Set: h = target; break;
            case SculptMode::Flatten: h += (target - h) * std::clamp(strength, 0.f, 1.f) * w; break;
            case SculptMode::Noise: h += gradNoise(wx * 0.15f, wz * 0.15f, seed) * strength * w; break;
            case SculptMode::Smooth: {
                float sum = 0;
                int cnt = 0;
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int xx = std::clamp(x + dx, 0, n - 1), zz = std::clamp(z + dz, 0, n - 1);
                        sum += before[static_cast<size_t>(zz) * n + xx];
                        ++cnt;
                    }
                }
                h += (sum / cnt - h) * std::clamp(strength, 0.f, 1.f) * w;
                break;
            }
        }
    });
    t.touch();
}

void paint(TerrainData& t, Vec2 c, float radius, int layer, float strength, float falloff) {
    if (layer < 0 || layer >= TerrainData::kMaxLayers) return;
    forBrush(t, c, radius, falloff, [&](int x, int z, float w, float, float) {
        uint8_t* ws = t.weightsAt(x, z);
        float k = std::clamp(strength * w, 0.f, 1.f);
        float sum = 0;
        float v[TerrainData::kMaxLayers];
        for (int i = 0; i < TerrainData::kMaxLayers; ++i) {
            v[i] = ws[i] / 255.f * (1.f - k) + (i == layer ? k : 0.f);
            sum += v[i];
        }
        for (int i = 0; i < TerrainData::kMaxLayers; ++i) ws[i] = static_cast<uint8_t>(std::lround(v[i] / std::max(sum, 1e-6f) * 255.f));
    });
    t.touch();
}

SculptMode sculptModeFromString(const std::string& m) {
    return m == "lower"     ? SculptMode::Lower
           : m == "flatten" ? SculptMode::Flatten
           : m == "smooth"  ? SculptMode::Smooth
           : m == "noise"   ? SculptMode::Noise
           : m == "set"     ? SculptMode::Set
                            : SculptMode::Raise;
}

void applyEdit(TerrainData& t, const Json& edit, const Json& layers, uint32_t seed) {
    std::string op = edit.get("op").asString();
    if (op == "autopaint") {
        autoPaint(t, layers, seed);
        return;
    }
    uint32_t i = 0;
    for (const auto& st : edit.get("strokes").elements()) {
        Vec2 c{st.get("x").asFloat(), st.get("z").asFloat()};
        if (op == "sculpt") {
            sculpt(t, c, st.get("radius").asFloat(10.f), st.get("strength").asFloat(1.f),
                   sculptModeFromString(st.get("mode").asString("raise")), st.get("target").asFloat(0.f), st.get("falloff").asFloat(0.5f),
                   ++i);
        } else if (op == "paint") {
            paint(t, c, st.get("radius").asFloat(8.f), static_cast<int>(edit.get("layer").asInt(0)), st.get("strength").asFloat(0.8f),
                  st.get("falloff").asFloat(0.5f));
        }
    }
    t.touch();
}

void applyEdits(TerrainData& t, const Json& edits, const Json& layers, uint32_t seed) {
    if (!edits.isArray()) return;
    for (const auto& e : edits.elements()) applyEdit(t, e, layers, seed);
}

}  // namespace sky::world
