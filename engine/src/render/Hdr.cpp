#include "skywalker/render/Hdr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace sky {

namespace {

Error bad(const std::string& why) { return Error::make("invalid_hdr", why); }

void rgbeToFloat(const uint8_t* e, float* out) {
    if (e[3] == 0) {
        out[0] = out[1] = out[2] = 0.f;
        return;
    }
    float f = std::ldexp(1.f, static_cast<int>(e[3]) - (128 + 8));
    out[0] = (e[0] + 0.5f) * f;
    out[1] = (e[1] + 0.5f) * f;
    out[2] = (e[2] + 0.5f) * f;
}

}  // namespace

Result<HdrImage> parseHdr(const std::vector<uint8_t>& bytes) {
    size_t pos = 0;
    auto line = [&]() {
        std::string s;
        while (pos < bytes.size() && bytes[pos] != '\n') s += static_cast<char>(bytes[pos++]);
        if (pos < bytes.size()) ++pos;
        return s;
    };
    std::string magic = line();
    if (magic.rfind("#?", 0) != 0) return bad("not a Radiance .hdr file");
    bool rgbe = false;
    for (;;) {
        if (pos >= bytes.size()) return bad("truncated header");
        std::string l = line();
        if (l.empty()) break;
        if (l.rfind("FORMAT=", 0) == 0) rgbe = l.find("32-bit_rle_rgbe") != std::string::npos;
    }
    if (!rgbe) return bad("only 32-bit_rle_rgbe .hdr files are supported");
    std::string res = line();
    char ya[3] = {}, xa[3] = {};
    int h = 0, w = 0;
    if (std::sscanf(res.c_str(), "%2s %d %2s %d", ya, &h, xa, &w) != 4 || std::strcmp(ya, "-Y") != 0 ||
        std::strcmp(xa, "+X") != 0) {
        return bad("unsupported resolution line '" + res + "' (expected -Y h +X w)");
    }
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return bad("invalid image size");

    HdrImage img;
    img.width = w;
    img.height = h;
    img.rgb.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    std::vector<uint8_t> scan(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h; ++y) {
        if (pos + 4 > bytes.size()) return bad("truncated pixel data");
        const uint8_t* p = bytes.data() + pos;
        bool rle = w >= 8 && w < 32768 && p[0] == 2 && p[1] == 2 && (p[2] & 0x80) == 0;
        if (rle) {
            if (((p[2] << 8) | p[3]) != w) return bad("scanline width mismatch");
            pos += 4;
            for (int c = 0; c < 4; ++c) {  // channels are stored one after another
                int x = 0;
                while (x < w) {
                    if (pos >= bytes.size()) return bad("truncated scanline");
                    int count = bytes[pos++];
                    if (count > 128) {
                        count -= 128;
                        if (x + count > w || pos >= bytes.size()) return bad("bad run");
                        uint8_t v = bytes[pos++];
                        for (int i = 0; i < count; ++i) scan[static_cast<size_t>(x++) * 4 + c] = v;
                    } else {
                        if (count == 0 || x + count > w || pos + static_cast<size_t>(count) > bytes.size()) return bad("bad literal run");
                        for (int i = 0; i < count; ++i) scan[static_cast<size_t>(x++) * 4 + c] = bytes[pos++];
                    }
                }
            }
        } else {  // flat RGBE pixels
            if (pos + static_cast<size_t>(w) * 4 > bytes.size()) return bad("truncated flat scanline");
            std::memcpy(scan.data(), bytes.data() + pos, static_cast<size_t>(w) * 4);
            pos += static_cast<size_t>(w) * 4;
        }
        float* row = img.rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 3;
        for (int x = 0; x < w; ++x) rgbeToFloat(&scan[static_cast<size_t>(x) * 4], row + x * 3);
    }
    return img;
}

bool hdrSunDirection(const HdrImage& img, float& x, float& y, float& z) {
    if (img.width <= 0 || img.height <= 0) return false;
    // Luminance-weighted centroid of the brightest pixels (top ~0.05%), in direction space.
    std::vector<float> lum(static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
    for (size_t i = 0; i < lum.size(); ++i) {
        lum[i] = 0.2126f * img.rgb[i * 3] + 0.7152f * img.rgb[i * 3 + 1] + 0.0722f * img.rgb[i * 3 + 2];
    }
    std::vector<float> sorted = lum;
    size_t k = std::max<size_t>(1, sorted.size() / 2000);
    std::nth_element(sorted.begin(), sorted.end() - static_cast<std::ptrdiff_t>(k), sorted.end());
    float threshold = *(sorted.end() - static_cast<std::ptrdiff_t>(k));
    double sx = 0, sy = 0, sz = 0, total = 0;
    const double pi = 3.14159265358979323846;
    for (int py = 0; py < img.height; ++py) {
        double theta = (py + 0.5) / img.height * pi;
        for (int px = 0; px < img.width; ++px) {
            float l = lum[static_cast<size_t>(py) * static_cast<size_t>(img.width) + static_cast<size_t>(px)];
            if (l < threshold) continue;
            double phi = ((px + 0.5) / img.width - 0.5) * 2.0 * pi;
            sx += l * std::sin(phi) * std::sin(theta);
            sy += l * std::cos(theta);
            sz += -l * std::cos(phi) * std::sin(theta);
            total += l;
        }
    }
    double len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (total <= 0 || len <= 1e-9) return false;
    x = static_cast<float>(sx / len);
    y = static_cast<float>(sy / len);
    z = static_cast<float>(sz / len);
    return true;
}

Result<HdrImage> loadHdr(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("io_error", "cannot read " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseHdr(bytes);
}

}  // namespace sky
