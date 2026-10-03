#include "skywalker/render/ColorGrading.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"

namespace sky::grading {

namespace {

struct C {
    float r, g, b;
};
C operator+(C a, C b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
C operator*(C a, float s) { return {a.r * s, a.g * s, a.b * s}; }
C operator*(C a, C b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
C mix(C a, C b, float t) { return a + (b + a * -1.f) * t; }
float luma(C c) { return c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f; }
C clamp01(C c) { return {std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f)}; }
C saturation(C c, float s) {
    float l = luma(c);
    return mix(C{l, l, l}, c, s);
}
C contrast(C c, float k) { return {(c.r - 0.5f) * k + 0.5f, (c.g - 0.5f) * k + 0.5f, (c.b - 0.5f) * k + 0.5f}; }
/// Lift (shadows), gamma (mid-tones), gain (highlights), per channel.
C lgg(C c, C lift, C gamma, C gain) {
    auto f = [](float v, float l, float g, float gn) {
        v = v * gn + l * (1.f - v);
        return std::pow(std::max(v, 0.f), 1.f / std::max(g, 0.05f));
    };
    return {f(c.r, lift.r, gamma.r, gain.r), f(c.g, lift.g, gamma.g, gain.g), f(c.b, lift.b, gamma.b, gain.b)};
}
/// Split toning: tint shadows and highlights with different hues.
C splitTone(C c, C shadow, C highlight, float amount) {
    float l = luma(c);
    float hs = std::clamp((l - 0.15f) / 0.7f, 0.f, 1.f);
    hs = hs * hs * (3 - 2 * hs);
    C tint = mix(shadow, highlight, hs);
    return mix(c, c * tint * 2.f, amount);
}
C sCurve(C c, float k) {
    auto f = [k](float v) {
        float s = v * v * (3 - 2 * v);
        return v + (s - v) * k;
    };
    return {f(c.r), f(c.g), f(c.b)};
}

C applyLook(const std::string& look, C c) {
    if (look == "warm") return clamp01(saturation(lgg(c, {0.01f, 0.005f, 0.f}, {1.03f, 1.f, 0.96f}, {1.04f, 1.f, 0.92f}), 1.08f));
    if (look == "cool") return clamp01(saturation(lgg(c, {0.f, 0.008f, 0.02f}, {0.97f, 1.f, 1.04f}, {0.94f, 0.99f, 1.05f}), 0.95f));
    if (look == "teal_orange")
        return clamp01(saturation(sCurve(splitTone(c, {0.33f, 0.52f, 0.56f}, {0.62f, 0.5f, 0.38f}, 0.55f), 0.35f), 1.15f));
    if (look == "golden_hour")
        return clamp01(saturation(sCurve(lgg(c, {0.035f, 0.02f, 0.f}, {1.05f, 1.f, 0.9f}, {1.08f, 0.98f, 0.82f}), 0.2f), 1.12f));
    if (look == "bleach") {  // bleach bypass: silver retained, low saturation, hard contrast
        C desat = saturation(c, 0.45f);
        return clamp01(sCurve(contrast(desat, 1.18f), 0.4f));
    }
    if (look == "noir") {
        float l = luma(sCurve(contrast(c, 1.25f), 0.5f));
        return clamp01(C{l * 0.97f, l * 0.99f, l * 1.04f});
    }
    if (look == "vivid") return clamp01(saturation(sCurve(c, 0.35f), 1.35f));
    if (look == "moonlight")
        return clamp01(saturation(lgg(c, {0.01f, 0.02f, 0.05f}, {0.9f, 0.95f, 1.08f}, {0.78f, 0.88f, 1.05f}), 0.6f));
    if (look == "vintage") {
        C faded = lgg(c, {0.06f, 0.055f, 0.04f}, {1.05f, 1.02f, 0.95f}, {0.95f, 0.94f, 0.86f});
        return clamp01(saturation(splitTone(faded, {0.45f, 0.52f, 0.48f}, {0.56f, 0.5f, 0.42f}, 0.3f), 0.75f));
    }
    return c;
}

}  // namespace

const std::vector<std::string>& looks() {
    static const std::vector<std::string> v = {"none", "warm", "cool", "teal_orange", "golden_hour", "bleach", "noir", "vivid", "moonlight", "vintage"};
    return v;
}

Result<Lut3D> lookLut(const std::string& look, int size) {
    const auto& all = looks();
    if (std::find(all.begin(), all.end(), look) == all.end()) {
        std::string guess = str::closest(look, all);
        return Error::make("unknown_look", "no look '" + look + "'", guess.empty() ? "" : "did you mean '" + guess + "'?");
    }
    Lut3D lut;
    lut.size = std::clamp(size, 2, 65);
    lut.rgb.resize(static_cast<size_t>(lut.size) * lut.size * lut.size * 3);
    size_t i = 0;
    const float inv = 1.f / static_cast<float>(lut.size - 1);
    for (int b = 0; b < lut.size; ++b) {
        for (int g = 0; g < lut.size; ++g) {
            for (int r = 0; r < lut.size; ++r) {
                C c = applyLook(look, C{r * inv, g * inv, b * inv});
                lut.rgb[i++] = c.r;
                lut.rgb[i++] = c.g;
                lut.rgb[i++] = c.b;
            }
        }
    }
    return lut;
}

Result<Lut3D> parseCube(const std::string& text) {
    Lut3D lut;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    while (std::getline(in, line)) {
        ++lineNo;
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first)) continue;
        if (first == "TITLE") continue;
        if (first == "LUT_3D_SIZE") {
            ls >> lut.size;
            if (lut.size < 2 || lut.size > 256) return Error::make("invalid_lut", "LUT_3D_SIZE out of range (line " + std::to_string(lineNo) + ")");
            lut.rgb.reserve(static_cast<size_t>(lut.size) * lut.size * lut.size * 3);
            continue;
        }
        if (first == "LUT_1D_SIZE") return Error::make("unsupported_lut", "1D LUTs are not supported; export a 3D .cube");
        if (first == "DOMAIN_MIN") {
            ls >> dmin[0] >> dmin[1] >> dmin[2];
            continue;
        }
        if (first == "DOMAIN_MAX") {
            ls >> dmax[0] >> dmax[1] >> dmax[2];
            continue;
        }
        float r = 0, g = 0, b = 0;
        try {
            r = std::stof(first);
        } catch (...) {
            continue;  // unknown keyword
        }
        if (!(ls >> g >> b)) return Error::make("invalid_lut", "expected three numbers on line " + std::to_string(lineNo));
        lut.rgb.push_back((r - dmin[0]) / std::max(dmax[0] - dmin[0], 1e-6f));
        lut.rgb.push_back((g - dmin[1]) / std::max(dmax[1] - dmin[1], 1e-6f));
        lut.rgb.push_back((b - dmin[2]) / std::max(dmax[2] - dmin[2], 1e-6f));
    }
    if (lut.size == 0) return Error::make("invalid_lut", "missing LUT_3D_SIZE");
    if (lut.rgb.size() != static_cast<size_t>(lut.size) * lut.size * lut.size * 3) {
        return Error::make("invalid_lut", "expected " + std::to_string(lut.size * lut.size * lut.size) + " entries, got " +
                                              std::to_string(lut.rgb.size() / 3));
    }
    return lut;
}

Result<Lut3D> loadCube(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("not_found", "LUT file not found: " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parseCube(ss.str());
}

}  // namespace sky::grading
