#include "skywalker/render/Impostor.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#include "skywalker/render/Renderer.h"

namespace sky::impostor {

namespace fs = std::filesystem;

namespace {

float sgn(float v) { return v >= 0.f ? 1.f : -1.f; }

// sRGB <-> linear for 8-bit channels (albedo is stored sRGB; averaging happens in linear).
const std::array<float, 256>& srgbToLinearLut() {
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            float c = static_cast<float>(i) / 255.f;
            t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return lut;
}

uint8_t linearToSrgb8(float c) {
    c = std::clamp(c, 0.f, 1.f);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.f));
}

uint8_t unorm8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

std::string hex64(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

constexpr char kMagic[8] = {'S', 'K', 'Y', 'I', 'M', 'P', '0', '1'};

}  // namespace

// --- Directions ------------------------------------------------------------------------------

Vec2 octEncode(Vec3 d) {
    float s = std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z);
    if (s < 1e-12f) return {0.f, 0.f};
    Vec3 n = d / s;
    Vec2 e{n.x, n.z};
    if (n.y < 0.f) e = Vec2{(1.f - std::fabs(n.z)) * sgn(n.x), (1.f - std::fabs(n.x)) * sgn(n.z)};
    return e;
}

Vec3 octDecode(Vec2 e) {
    Vec3 n{e.x, 1.f - std::fabs(e.x) - std::fabs(e.y), e.y};
    float t = std::max(-n.y, 0.f);
    n.x += n.x >= 0.f ? -t : t;
    n.z += n.z >= 0.f ? -t : t;
    return normalize(n);
}

Vec2 hemiOctEncode(Vec3 d) {
    d.y = std::max(d.y, 0.f);
    float s = std::fabs(d.x) + d.y + std::fabs(d.z);
    if (s < 1e-12f) return {0.f, 0.f};
    Vec3 n = d / s;
    return {n.x + n.z, n.x - n.z};
}

Vec3 hemiOctDecode(Vec2 e) {
    float qx = (e.x + e.y) * 0.5f, qz = (e.x - e.y) * 0.5f;
    return normalize(Vec3{qx, 1.f - std::fabs(qx) - std::fabs(qz), qz});
}

Vec2 gridCoord(Vec3 dir, int frames, bool hemi) {
    Vec2 e = hemi ? hemiOctEncode(dir) : octEncode(dir);
    float n = static_cast<float>(std::max(frames, 2) - 1);
    return {std::clamp(e.x * 0.5f + 0.5f, 0.f, 1.f) * n, std::clamp(e.y * 0.5f + 0.5f, 0.f, 1.f) * n};
}

Vec3 frameDirection(int x, int y, int frames, bool hemi) {
    float n = static_cast<float>(std::max(frames, 2) - 1);
    Vec2 e{static_cast<float>(x) / n * 2.f - 1.f, static_cast<float>(y) / n * 2.f - 1.f};
    return hemi ? hemiOctDecode(e) : octDecode(e);
}

Basis frameBasis(Vec3 forward) {
    Basis b;
    b.forward = normalize(forward);
    Vec3 ref = std::fabs(b.forward.y) > 0.999f ? Vec3{0, 0, -1} : Vec3{0, 1, 0};
    b.right = normalize(cross(ref, b.forward));
    b.up = cross(b.forward, b.right);
    return b;
}

Blend blendFrames(Vec3 viewDir, int frames, bool hemi) {
    frames = std::max(frames, 2);
    Vec2 g = gridCoord(viewDir, frames, hemi);
    int cx = std::clamp(static_cast<int>(std::floor(g.x)), 0, frames - 2);
    int cy = std::clamp(static_cast<int>(std::floor(g.y)), 0, frames - 2);
    float fx = std::clamp(g.x - static_cast<float>(cx), 0.f, 1.f), fy = std::clamp(g.y - static_cast<float>(cy), 0.f, 1.f);
    Blend b;
    if (fx + fy <= 1.f) {
        b.x[0] = cx, b.y[0] = cy, b.w[0] = 1.f - fx - fy;
        b.x[1] = cx + 1, b.y[1] = cy, b.w[1] = fx;
        b.x[2] = cx, b.y[2] = cy + 1, b.w[2] = fy;
    } else {
        b.x[0] = cx + 1, b.y[0] = cy + 1, b.w[0] = fx + fy - 1.f;
        b.x[1] = cx + 1, b.y[1] = cy, b.w[1] = 1.f - fy;
        b.x[2] = cx, b.y[2] = cy + 1, b.w[2] = 1.f - fx;
    }
    for (float& w : b.w) w = std::max(w, 0.f);
    return b;
}

// --- Transition ------------------------------------------------------------------------------

int tileSize(int atlasResolution, int frames) {
    frames = std::clamp(frames, kMinFrames, kMaxFrames);
    int t = std::max(atlasResolution, 64) / frames;
    return std::max(16, t / 16 * 16);
}

int autoResolution(float worldDiameter) {
    if (worldDiameter >= 8.f) return 2048;
    if (worldDiameter >= 2.5f) return 1024;
    return 512;
}

float qualityScale(int quality) { return quality >= 2 ? 0.5f : quality == 1 ? 0.75f : 1.f; }

float transitionDistance(const TransitionParams& p) {
    if (p.overrideDistance < 0.f) return 0.f;
    const float diameter = std::max(p.modelRadius, 0.01f) * 2.f;
    float d = p.overrideDistance;
    if (d <= 0.f) {
        // About one atlas texel per screen pixel (the frame tile spans the bounding diameter, which is
        // looser than the silhouette, so a little magnification still reads sharp under TAA).
        const float pxPerMeterAt1m = static_cast<float>(std::max(p.screenHeight, 1)) /
                                     (2.f * std::tan(radians(std::clamp(p.fovDeg, 5.f, 150.f)) * 0.5f));
        d = kTexelMatch * diameter * pxPerMeterAt1m / static_cast<float>(tileSize(p.atlasResolution, p.frames));
    }
    d *= qualityScale(p.quality);
    d = std::max(d, diameter * 1.5f + 2.f);  // never right in front of the camera
    if (d >= p.cullDistance * 0.9f) return 0.f;  // the mesh range covers (almost) everything drawn
    return d;
}

float crossfadeWidth(float transitionDistance) { return std::clamp(transitionDistance * 0.12f, 0.5f, 20.f); }

// --- Cache keys ------------------------------------------------------------------------------

uint64_t hash64(const std::string& s, uint64_t seed) {
    uint64_t h = seed;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string cacheKey(const ImpostorModel& m) {
    std::string in = "impostor-v" + std::to_string(kBakeVersion) + "|" + m.source + "|" + m.stamp + "|f" +
                     std::to_string(std::clamp(m.frames, kMinFrames, kMaxFrames)) + "|t" +
                     std::to_string(tileSize(m.resolution, m.frames)) + (m.hemi ? "|hemi" : "|full");
    return hex64(hash64(in)) + hex64(hash64(in, 0x84222325CBF29CE4ull));
}

// --- Atlas -----------------------------------------------------------------------------------

void finalize(Atlas& a) {
    const int n = a.size, tile = a.tile();
    if (n <= 0 || tile <= 0) return;
    const size_t count = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (a.albedo.size() < count * 4 || a.normal.size() < count * 4) return;
    const auto& lin = srgbToLinearLut();
    // Texels with enough coverage carry reliable (un-premultiplied) values; fainter edge texels
    // are too quantized after the divide and get their values from the nearest solid texel.
    constexpr uint8_t kSolid = 32;
    std::vector<uint8_t> solid(count, 0);
    std::vector<int> queue;
    queue.reserve(count / 2);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t cov = a.albedo[i * 4 + 3];
        if (cov == 0) continue;
        const float inv = 255.f / static_cast<float>(cov);
        for (int c = 0; c < 3; ++c) a.albedo[i * 4 + c] = linearToSrgb8(lin[a.albedo[i * 4 + c]] * inv);
        for (int c = 0; c < 4; ++c) a.normal[i * 4 + c] = unorm8(static_cast<float>(a.normal[i * 4 + c]) * inv / 255.f);
        if (cov >= kSolid) {
            solid[i] = 1;
            queue.push_back(static_cast<int>(i));
        }
    }
    // Dilation: breadth-first flood from solid texels, never crossing a frame tile boundary.
    for (size_t head = 0; head < queue.size(); ++head) {
        const int i = queue[head];
        const int x = i % n, y = i / n;
        const int tx = x / tile, ty = y / tile;
        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (const auto& p : nb) {
            if (p[0] < 0 || p[1] < 0 || p[0] >= n || p[1] >= n || p[0] / tile != tx || p[1] / tile != ty) continue;
            const size_t j = static_cast<size_t>(p[1]) * n + p[0];
            if (solid[j]) continue;
            solid[j] = 1;
            std::memcpy(&a.albedo[j * 4], &a.albedo[static_cast<size_t>(i) * 4], 3);
            std::memcpy(&a.normal[j * 4], &a.normal[static_cast<size_t>(i) * 4], 4);
            queue.push_back(static_cast<int>(j));
        }
    }
    // Frames without any solid texel (nothing visible from there): neutral values.
    for (size_t i = 0; i < count; ++i) {
        if (solid[i]) continue;
        a.albedo[i * 4 + 0] = a.albedo[i * 4 + 1] = a.albedo[i * 4 + 2] = 0;
        a.normal[i * 4 + 0] = a.normal[i * 4 + 1] = a.normal[i * 4 + 2] = 128;
        a.normal[i * 4 + 3] = 0;
    }
}

float frameCoverage(const Atlas& a, int fx, int fy) {
    const int tile = a.tile();
    if (tile <= 0) return 0.f;
    const auto cutoff = static_cast<uint8_t>(std::lround(kAlphaCutoff * 255.f));
    size_t passed = 0;
    for (int y = fy * tile; y < (fy + 1) * tile; ++y) {
        const uint8_t* row = &a.albedo[(static_cast<size_t>(y) * a.size + static_cast<size_t>(fx) * tile) * 4];
        for (int x = 0; x < tile; ++x) passed += row[x * 4 + 3] >= cutoff ? 1 : 0;
    }
    return static_cast<float>(passed) / static_cast<float>(tile * tile);
}

std::vector<Atlas> buildMips(const Atlas& level0) {
    std::vector<Atlas> levels{level0};
    const auto& lin = srgbToLinearLut();
    while (static_cast<int>(levels.size()) < kMipLevels) {
        const Atlas& src = levels.back();
        const int tile = src.tile();
        if (tile < 2 || tile % 2) break;
        Atlas dst;
        dst.size = src.size / 2;
        dst.frames = src.frames;
        const size_t count = static_cast<size_t>(dst.size) * dst.size;
        dst.albedo.resize(count * 4);
        dst.normal.resize(count * 4);
        for (int y = 0; y < dst.size; ++y) {
            for (int x = 0; x < dst.size; ++x) {
                float wsum = 0.f, alpha = 0.f, rgb[3] = {}, nrm[4] = {}, rgbFlat[3] = {}, nrmFlat[4] = {};
                for (int k = 0; k < 4; ++k) {
                    const size_t s = (static_cast<size_t>(y * 2 + k / 2) * src.size + static_cast<size_t>(x * 2 + k % 2)) * 4;
                    const float w = static_cast<float>(src.albedo[s + 3]) / 255.f;
                    alpha += w * 0.25f;
                    wsum += w;
                    for (int c = 0; c < 3; ++c) {
                        rgb[c] += lin[src.albedo[s + c]] * w;
                        rgbFlat[c] += lin[src.albedo[s + c]] * 0.25f;
                    }
                    for (int c = 0; c < 4; ++c) {
                        nrm[c] += static_cast<float>(src.normal[s + c]) * w;
                        nrmFlat[c] += static_cast<float>(src.normal[s + c]) * 0.25f;
                    }
                }
                const size_t d = (static_cast<size_t>(y) * dst.size + x) * 4;
                // Alpha-weighted where anything is covered (edge colors stay those of the
                // surface); dilated values elsewhere.
                const bool covered = wsum > 1e-4f;
                for (int c = 0; c < 3; ++c) dst.albedo[d + c] = linearToSrgb8(covered ? rgb[c] / wsum : rgbFlat[c]);
                dst.albedo[d + 3] = unorm8(alpha);
                for (int c = 0; c < 4; ++c) dst.normal[d + c] = unorm8((covered ? nrm[c] / wsum : nrmFlat[c]) / 255.f);
            }
        }
        // Coverage preservation (per frame): scale alpha so as many texels pass the alpha test as
        // in level 0 (Castaño, "Computing Alpha Mipmaps").
        const int dtile = dst.tile();
        std::vector<uint8_t> alphas(static_cast<size_t>(dtile) * dtile);
        for (int fy = 0; fy < dst.frames; ++fy) {
            for (int fx = 0; fx < dst.frames; ++fx) {
                const float target = frameCoverage(level0, fx, fy);
                if (target <= 0.f) continue;
                size_t k = 0;
                for (int y = fy * dtile; y < (fy + 1) * dtile; ++y) {
                    for (int x = fx * dtile; x < (fx + 1) * dtile; ++x) alphas[k++] = dst.albedo[(static_cast<size_t>(y) * dst.size + x) * 4 + 3];
                }
                const size_t want = std::clamp<size_t>(static_cast<size_t>(std::lround(target * static_cast<float>(alphas.size()))), 1, alphas.size());
                std::nth_element(alphas.begin(), alphas.begin() + static_cast<std::ptrdiff_t>(want - 1), alphas.end(), std::greater<>());
                const float edge = static_cast<float>(alphas[want - 1]) / 255.f;
                if (edge <= 1e-3f) continue;
                // Map the want-th largest alpha to just above the cutoff.
                const float scale = std::clamp((kAlphaCutoff + 0.5f / 255.f) / edge, 0.25f, 8.f);
                for (int y = fy * dtile; y < (fy + 1) * dtile; ++y) {
                    for (int x = fx * dtile; x < (fx + 1) * dtile; ++x) {
                        uint8_t& av = dst.albedo[(static_cast<size_t>(y) * dst.size + x) * 4 + 3];
                        av = unorm8(static_cast<float>(av) / 255.f * scale);
                    }
                }
            }
        }
        levels.push_back(std::move(dst));
    }
    return levels;
}

// --- Cache file --------------------------------------------------------------------------------

Status save(const std::string& path, const Atlas& a, const Json& meta) {
    const size_t bytes = static_cast<size_t>(a.size) * a.size * 4;
    if (a.size <= 0 || a.albedo.size() != bytes || a.normal.size() != bytes) {
        return Error::make("invalid_atlas", "impostor atlas has no data");
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return Error::make("io_error", "cannot write " + tmp);
        const std::string metaText = meta.dump();
        auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        out.write(kMagic, 8);
        put32(static_cast<uint32_t>(a.size));
        put32(static_cast<uint32_t>(a.frames));
        put32(static_cast<uint32_t>(metaText.size()));
        out.write(metaText.data(), static_cast<std::streamsize>(metaText.size()));
        for (const auto* plane : {&a.albedo, &a.normal}) {
            uLongf len = compressBound(static_cast<uLong>(plane->size()));
            std::vector<uint8_t> z(len);
            if (compress2(z.data(), &len, plane->data(), static_cast<uLong>(plane->size()), 3) != Z_OK) {
                return Error::make("io_error", "could not compress the impostor atlas");
            }
            put32(static_cast<uint32_t>(len));
            out.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(len));
        }
        if (!out) return Error::make("io_error", "cannot write " + tmp);
    }
    fs::rename(tmp, path, ec);
    if (ec) return Error::make("io_error", "cannot move " + tmp + " into place: " + ec.message());
    return {};
}

Result<Loaded> load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error::make("not_found", "no impostor cache at " + path);
    char magic[8];
    in.read(magic, 8);
    if (!in || std::memcmp(magic, kMagic, 8) != 0) return Error::make("invalid_cache", path + " is not an impostor cache");
    auto get32 = [&]() {
        uint32_t v = 0;
        in.read(reinterpret_cast<char*>(&v), 4);
        return v;
    };
    Loaded l;
    l.atlas.size = static_cast<int>(get32());
    l.atlas.frames = static_cast<int>(get32());
    const uint32_t metaLen = get32();
    if (!in || l.atlas.size <= 0 || l.atlas.size > 16384 || l.atlas.frames < kMinFrames || l.atlas.frames > kMaxFrames ||
        metaLen > (1u << 20)) {
        return Error::make("invalid_cache", path + " has a corrupt header");
    }
    std::string metaText(metaLen, '\0');
    in.read(metaText.data(), metaLen);
    auto meta = Json::parse(metaText);
    if (!in || !meta) return Error::make("invalid_cache", path + " has corrupt metadata");
    l.meta = std::move(meta.value());
    const size_t bytes = static_cast<size_t>(l.atlas.size) * l.atlas.size * 4;
    for (auto* plane : {&l.atlas.albedo, &l.atlas.normal}) {
        const uint32_t len = get32();
        if (!in || len == 0 || len > compressBound(static_cast<uLong>(bytes))) return Error::make("invalid_cache", path + " is truncated");
        std::vector<uint8_t> z(len);
        in.read(reinterpret_cast<char*>(z.data()), len);
        if (!in) return Error::make("invalid_cache", path + " is truncated");
        plane->resize(bytes);
        uLongf outLen = static_cast<uLongf>(bytes);
        if (uncompress(plane->data(), &outLen, z.data(), len) != Z_OK || outLen != bytes) {
            return Error::make("invalid_cache", path + " has corrupt atlas data");
        }
    }
    return l;
}

Image preview(const Atlas& a, int maxSize) {
    int step = 1;
    while (a.size / step > std::max(maxSize, 16)) step *= 2;
    Image img(std::max(a.size / step, 1), std::max(a.size / step, 1));
    const auto& lin = srgbToLinearLut();
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            const size_t s = (static_cast<size_t>(y * step) * a.size + static_cast<size_t>(x * step)) * 4;
            const float alpha = static_cast<float>(a.albedo[s + 3]) / 255.f;
            const float check = ((x / 8 + y / 8) % 2) ? 0.32f : 0.22f;
            uint8_t* d = img.at(x, y);
            for (int c = 0; c < 3; ++c) d[c] = linearToSrgb8(lin[a.albedo[s + c]] * alpha + check * check * (1.f - alpha));
            d[3] = 255;
        }
    }
    return img;
}

}  // namespace sky::impostor
