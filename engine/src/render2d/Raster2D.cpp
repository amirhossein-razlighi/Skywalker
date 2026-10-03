#include "skywalker/render2d/Raster2D.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "skywalker/text/Font.h"

namespace sky::raster2d {

namespace {

// sRGB <-> linear lookup tables (the image is sRGB; sprites shade and blend in linear light).
const std::array<float, 256>& toLinearLut() {
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

uint8_t toSrgbByte(float v) {
    static const std::array<uint8_t, 4096> lut = [] {
        std::array<uint8_t, 4096> t{};
        for (int i = 0; i < 4096; ++i) {
            float c = static_cast<float>(i) / 4095.f;
            float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
            t[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(s, 0.f, 1.f) * 255.f + 0.5f);
        }
        return t;
    }();
    return lut[static_cast<size_t>(std::clamp(v, 0.f, 1.f) * 4095.f + 0.5f)];
}

float sat(float v) { return std::clamp(v, 0.f, 1.f); }
float smoothstep(float a, float b, float x) {
    float t = sat((x - a) / (b - a));
    return t * t * (3.f - 2.f * t);
}

/// A texture to sample: RGBA8 sRGB image or R8 SDF page.
struct Tex {
    const uint8_t* px = nullptr;
    int w = 0, h = 0, channels = 4;
    bool valid() const { return px && w > 0 && h > 0; }

    /// Bilinear (or nearest) sample; returns linear rgba for color textures, (d, d, d, d) for SDF pages.
    Vec4 sample(float u, float v, bool nearest) const {
        float x = u * static_cast<float>(w) - 0.5f, y = v * static_cast<float>(h) - 0.5f;
        if (nearest) {
            int ix = std::clamp(static_cast<int>(std::floor(u * static_cast<float>(w))), 0, w - 1);
            int iy = std::clamp(static_cast<int>(std::floor(v * static_cast<float>(h))), 0, h - 1);
            return texel(ix, iy);
        }
        int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
        Vec4 a = texel(x0, y0), b = texel(x0 + 1, y0), c = texel(x0, y0 + 1), d = texel(x0 + 1, y0 + 1);
        auto lerp4 = [](Vec4 p, Vec4 q, float t) { return Vec4{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t}; };
        return lerp4(lerp4(a, b, fx), lerp4(c, d, fx), fy);
    }
    Vec4 texel(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        const uint8_t* p = px + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * static_cast<size_t>(channels);
        if (channels == 1) {
            float d = static_cast<float>(p[0]) / 255.f;
            return {d, d, d, d};
        }
        const auto& lin = toLinearLut();
        return {lin[p[0]], lin[p[1]], lin[p[2]], static_cast<float>(p[3]) / 255.f};
    }
};

Tex resolve(const TextureRef& ref, render2d::ImageCache& cache, std::shared_ptr<const Image>& keep) {
    Tex t;
    if (ref.image) {
        t.px = ref.image->pixels.data();
        t.w = ref.image->width;
        t.h = ref.image->height;
        t.channels = ref.image->channels;
    } else if (!ref.path.empty()) {
        keep = cache.image(ref.path);
        if (keep) {
            t.px = keep->pixels.data();
            t.w = keep->width;
            t.h = keep->height;
        }
    }
    return t;
}

/// Blends a linear, straight-alpha color over an sRGB pixel.
void blendLinear(uint8_t* p, Vec3 c, float a, bool additive) {
    const auto& lin = toLinearLut();
    float r = lin[p[0]], g = lin[p[1]], b = lin[p[2]];
    if (additive) {
        r += c.x * a;
        g += c.y * a;
        b += c.z * a;
    } else {
        r += (c.x - r) * a;
        g += (c.y - g) * a;
        b += (c.z - b) * a;
    }
    p[0] = toSrgbByte(r);
    p[1] = toSrgbByte(g);
    p[2] = toSrgbByte(b);
}

/// Blends an sRGB straight-alpha color over an sRGB pixel (UI).
void blendSrgb(uint8_t* p, Vec4 c, float a) {
    a = sat(a);
    for (int i = 0; i < 3; ++i) {
        float s = static_cast<float>(p[i]) / 255.f;
        float v = i == 0 ? c.x : i == 1 ? c.y : c.z;
        p[i] = static_cast<uint8_t>(std::clamp(s + (sat(v) - s) * a, 0.f, 1.f) * 255.f + 0.5f);
    }
}

/// Rasterizes a screen-space parallelogram O + s*A + t*B (s, t in [0, 1], `margin` pixels of slack),
/// calling fn(x, y, s, t) for covered pixel centers.
template <typename Fn>
void parallelogram(int W, int H, Vec2 O, Vec2 A, Vec2 B, float margin, const float* clip, Fn&& fn) {
    float det = A.x * B.y - A.y * B.x;
    if (std::fabs(det) < 1e-8f) return;
    float xs[4] = {O.x, O.x + A.x, O.x + B.x, O.x + A.x + B.x}, ys[4] = {O.y, O.y + A.y, O.y + B.y, O.y + A.y + B.y};
    int x0 = static_cast<int>(std::floor(*std::min_element(xs, xs + 4) - margin));
    int x1 = static_cast<int>(std::ceil(*std::max_element(xs, xs + 4) + margin));
    int y0 = static_cast<int>(std::floor(*std::min_element(ys, ys + 4) - margin));
    int y1 = static_cast<int>(std::ceil(*std::max_element(ys, ys + 4) + margin));
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, W);
    y1 = std::min(y1, H);
    if (clip && clip[2] >= clip[0]) {
        x0 = std::max(x0, static_cast<int>(std::floor(clip[0])));
        y0 = std::max(y0, static_cast<int>(std::floor(clip[1])));
        x1 = std::min(x1, static_cast<int>(std::ceil(clip[2])));
        y1 = std::min(y1, static_cast<int>(std::ceil(clip[3])));
    }
    const float lenA = std::sqrt(A.x * A.x + A.y * A.y), lenB = std::sqrt(B.x * B.x + B.y * B.y);
    const float ms = lenA > 0 ? margin / lenA : 0, mt = lenB > 0 ? margin / lenB : 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            float px = static_cast<float>(x) + 0.5f - O.x, py = static_cast<float>(y) + 0.5f - O.y;
            float s = (px * B.y - py * B.x) / det, t = (A.x * py - A.y * px) / det;
            if (s < -ms || t < -mt || s > 1 + ms || t > 1 + mt) continue;
            if (clip && clip[2] >= clip[0]) {
                float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f;
                if (cx < clip[0] || cy < clip[1] || cx > clip[2] || cy > clip[3]) continue;
            }
            fn(x, y, s, t);
        }
    }
}

float sdRoundRect(Vec2 p, Vec2 half, float r) {
    r = std::min(r, std::min(half.x, half.y));
    float qx = std::fabs(p.x) - half.x + r, qy = std::fabs(p.y) - half.y + r;
    float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

}  // namespace

void drawWorld(Image& image, const FrameData& frame, render2d::ImageCache& cache) {
    const Frame2D& f = frame.render2d;
    if (f.sprites.empty()) return;
    const Mat4 vp = frame.viewProjection();
    const float W = static_cast<float>(image.width), H = static_cast<float>(image.height);
    auto project = [&](Vec3 p, Vec2& out) {
        Vec4 c = vp * Vec4(p, 1.f);
        if (c.w <= 1e-5f) return false;
        out = {(c.x / c.w * 0.5f + 0.5f) * W, (0.5f - c.y / c.w * 0.5f) * H};
        return true;
    };
    for (const SpriteBatch& batch : f.spriteBatches) {
        std::shared_ptr<const Image> keep;
        Tex tex = resolve(batch.texture, cache, keep);
        for (uint32_t i = batch.first; i < batch.first + batch.count && i < f.sprites.size(); ++i) {
            const SpriteInstance& s = f.sprites[i];
            Vec3 o{s.origin[0], s.origin[1], s.origin[2]};
            Vec3 ax{s.axisX[0], s.axisX[1], s.axisX[2]}, ay{s.axisY[0], s.axisY[1], s.axisY[2]};
            Vec2 so, sa, sb;
            if (!project(o, so) || !project(o + ax, sa) || !project(o + ay, sb)) continue;
            const int mode = static_cast<int>(s.params[0]);
            const bool lit = f.lit && s.params[1] > 0.5f;
            // 2D lighting at the quad center (per-sprite on the CPU).
            Vec3 light{1, 1, 1};
            if (lit && mode != static_cast<int>(SpriteMode::Halo)) {
                Vec3 center = o + ax * 0.5f + ay * 0.5f;
                light = f.ambient;
                for (const auto& l : f.lights) {
                    float d = std::sqrt((center.x - l.position.x) * (center.x - l.position.x) +
                                        (center.y - l.position.y) * (center.y - l.position.y));
                    float att = std::pow(sat(1.f - d / std::max(1e-3f, l.radius)), l.falloff);
                    if (l.kind == Light2DItem::Kind::Spot && d > 1e-4f) {
                        float c = ((center.x - l.position.x) * l.direction.x + (center.y - l.position.y) * l.direction.y) / d;
                        att *= smoothstep(l.cosOuter, l.cosInner, c);
                    }
                    light = light + l.color * att;
                }
            }
            const Vec4 tint{s.color[0], s.color[1], s.color[2], s.color[3]};
            const float texelsPerPixel =
                tex.valid() ? std::fabs(s.uv[2] - s.uv[0]) * static_cast<float>(tex.w) /
                                  std::max(1.f, std::sqrt((sa.x - so.x) * (sa.x - so.x) + (sa.y - so.y) * (sa.y - so.y)))
                            : 1.f;
            parallelogram(image.width, image.height, so, {sa.x - so.x, sa.y - so.y}, {sb.x - so.x, sb.y - so.y}, 0.f, nullptr,
                          [&](int x, int y, float u, float v) {
                              uint8_t* p = image.at(x, y);
                              float tu = s.uv[0] + (s.uv[2] - s.uv[0]) * u, tv = s.uv[1] + (s.uv[3] - s.uv[1]) * v;
                              if (mode == static_cast<int>(SpriteMode::Halo)) {
                                  float dx = u * 2.f - 1.f, dy = v * 2.f - 1.f;
                                  float r = sat(1.f - std::sqrt(dx * dx + dy * dy));
                                  blendLinear(p, tint.xyz(), r * r, true);
                                  return;
                              }
                              if (mode == static_cast<int>(SpriteMode::Sdf)) {
                                  if (!tex.valid()) return;
                                  float d = tex.sample(tu, tv, false).x + s.params[3];
                                  float w = std::clamp(0.7f * texelsPerPixel / (2.f * static_cast<float>(text::Font::kSpread)), 0.02f, 0.5f);
                                  float fill = smoothstep(0.5f - w, 0.5f + w, d);
                                  float outline = s.emission[3];
                                  Vec3 c = tint.xyz();
                                  float a = fill;
                                  if (outline > 0.f) {
                                      float outer = smoothstep(0.5f - outline - w, 0.5f - outline + w, d);
                                      Vec3 oc{s.emission[0], s.emission[1], s.emission[2]};
                                      c = oc + (c - oc) * fill;
                                      a = outer;
                                  }
                                  blendLinear(p, c, a * tint.w, false);
                                  return;
                              }
                              Vec4 t = tex.valid() ? tex.sample(tu, tv, batch.nearest) : Vec4{1, 1, 1, 1};
                              float a = t.w * tint.w;
                              if (s.extra[1] > 0.f) a = t.w >= s.extra[1] ? tint.w : 0.f;
                              if (a <= 0.001f) return;
                              Vec3 c{t.x * tint.x * light.x + s.emission[0], t.y * tint.y * light.y + s.emission[1],
                                     t.z * tint.z * light.z + s.emission[2]};
                              blendLinear(p, c, a, batch.additive);
                          });
        }
    }
}

void drawUI(Image& image, const FrameData& frame, render2d::ImageCache& cache) {
    const Frame2D& f = frame.render2d;
    if (f.ui.empty()) return;
    const Mat4 vp = frame.viewProjection();
    const float W = static_cast<float>(image.width), H = static_cast<float>(image.height);
    for (const UIBatch& batch : f.uiBatches) {
        std::shared_ptr<const Image> keep;
        Tex tex = resolve(batch.texture, cache, keep);
        const UICanvasItem* canvas = batch.canvas >= 0 && static_cast<size_t>(batch.canvas) < f.uiCanvases.size()
                                         ? &f.uiCanvases[static_cast<size_t>(batch.canvas)]
                                         : nullptr;
        for (uint32_t i = batch.first; i < batch.first + batch.count && i < f.ui.size(); ++i) {
            const UIQuad& q = f.ui[i];
            const auto kind = static_cast<UIQuadKind>(static_cast<int>(q.params[0]));
            const float rw = q.rect[2], rh = q.rect[3];
            if (rw <= 0.f || rh <= 0.f) continue;
            const float shear = kind == UIQuadKind::Glyph ? q.params2[2] : 0.f;
            // The quad in screen space: O + s*A + t*B (s, t over the rect, plus the italic shear).
            Vec2 O{q.rect[0], q.rect[1]}, A{rw, 0}, B{0, rh};
            const float* clip = q.clip;
            float pixelsPerUnit = 1.f;
            if (canvas && canvas->world) {
                auto proj = [&](float cx, float cy, Vec2& out) {
                    Vec4 c = vp * Vec4(canvas->model.transformPoint({cx, cy, 0.f}), 1.f);
                    if (c.w <= 1e-5f) return false;
                    out = {(c.x / c.w * 0.5f + 0.5f) * W, (0.5f - c.y / c.w * 0.5f) * H};
                    return true;
                };
                Vec2 a, b, c;
                if (!proj(q.rect[0], q.rect[1], a) || !proj(q.rect[0] + rw, q.rect[1], b) || !proj(q.rect[0], q.rect[1] + rh, c)) continue;
                O = a;
                A = {b.x - a.x, b.y - a.y};
                B = {c.x - a.x, c.y - a.y};
                pixelsPerUnit = std::sqrt(A.x * A.x + A.y * A.y) / rw;
                clip = nullptr;  // world canvases clip in canvas space below
            }
            const Vec4 color{q.color[0], q.color[1], q.color[2], q.color[3]};
            const Vec4 color2{q.color2[0], q.color2[1], q.color2[2], q.color2[3]};
            const Vec4 border{q.borderColor[0], q.borderColor[1], q.borderColor[2], q.borderColor[3]};
            const float opacity = q.params[3];
            const float radius = q.params[1], bw = q.params[2];
            const Vec2 half{rw * 0.5f, rh * 0.5f};
            const float aa = 0.5f / std::max(1e-3f, pixelsPerUnit);
            const bool worldClip = canvas && canvas->world && q.clip[2] >= q.clip[0];
            const float margin = (kind == UIQuadKind::Glyph ? std::fabs(shear) * pixelsPerUnit : 0.f) + 1.f;
            Vec2 Ae = A, Oe = O;
            if (shear != 0.f) {  // widen the parallelogram so the slanted glyph fits
                Ae = {A.x + std::fabs(shear) * pixelsPerUnit, A.y};
                if (shear < 0.f) Oe = {O.x + shear * pixelsPerUnit, O.y};
            }
            parallelogram(image.width, image.height, Oe, Ae, B, margin, clip, [&](int x, int y, float s, float t) {
                // Canvas-space position inside the rect.
                float lx = s * (Ae.x / std::max(1e-6f, A.x)) * rw + (Oe.x - O.x) / std::max(1e-6f, pixelsPerUnit);
                float ly = t * rh;
                if (worldClip) {
                    float cx = q.rect[0] + lx, cy = q.rect[1] + ly;
                    if (cx < q.clip[0] || cy < q.clip[1] || cx > q.clip[2] || cy > q.clip[3]) return;
                }
                uint8_t* p = image.at(x, y);
                Vec2 local{lx - half.x, ly - half.y};
                switch (kind) {
                    case UIQuadKind::Rect: {
                        float d = sdRoundRect(local, half, radius);
                        float cover = sat(0.5f - d / (2.f * aa));
                        if (cover <= 0.f) return;
                        float v = sat(ly / rh);
                        Vec4 fill{color.x + (color2.x - color.x) * v, color.y + (color2.y - color.y) * v, color.z + (color2.z - color.z) * v,
                                  color.w + (color2.w - color.w) * v};
                        if (bw > 0.f && border.w > 0.f) {
                            float inner = sat(0.5f - (d + bw) / (2.f * aa));
                            float a = border.w * (1.f - inner) + fill.w * inner;
                            if (a <= 0.f) return;
                            Vec4 c{(border.x * border.w * (1.f - inner) + fill.x * fill.w * inner) / a,
                                   (border.y * border.w * (1.f - inner) + fill.y * fill.w * inner) / a,
                                   (border.z * border.w * (1.f - inner) + fill.z * fill.w * inner) / a, a};
                            blendSrgb(p, c, a * cover * opacity);
                        } else {
                            blendSrgb(p, fill, fill.w * cover * opacity);
                        }
                        break;
                    }
                    case UIQuadKind::Shadow: {
                        float blur = std::max(0.5f, q.params2[0]);
                        Vec2 inner{std::max(0.f, half.x - blur), std::max(0.f, half.y - blur)};
                        float d = sdRoundRect(local, inner, radius);
                        float a = 1.f - smoothstep(-blur, blur, d);
                        blendSrgb(p, color, color.w * a * opacity);
                        break;
                    }
                    case UIQuadKind::Image: {
                        if (!tex.valid()) return;
                        if (s < 0.f || t < 0.f || s > 1.f || t > 1.f) return;
                        float mask = radius > 0.f ? sat(0.5f - sdRoundRect(local, half, radius) / (2.f * aa)) : 1.f;
                        Vec4 c = tex.sample(q.uv[0] + (q.uv[2] - q.uv[0]) * s, q.uv[1] + (q.uv[3] - q.uv[1]) * t, batch.nearest);
                        // The texture sample is linear; UI blends in sRGB.
                        auto enc = [](float v) { return static_cast<float>(toSrgbByte(v)) / 255.f; };
                        Vec4 srgb{enc(c.x) * color.x, enc(c.y) * color.y, enc(c.z) * color.z, c.w * color.w};
                        blendSrgb(p, srgb, srgb.w * mask * opacity);
                        break;
                    }
                    case UIQuadKind::Glyph: {
                        if (!tex.valid()) return;
                        float gx = lx - shear * (1.f - ly / rh);  // undo the italic slant
                        float us = gx / rw;
                        if (us < -0.05f || us > 1.05f || t < 0.f || t > 1.f) return;
                        float d = tex.sample(q.uv[0] + (q.uv[2] - q.uv[0]) * us, q.uv[1] + (q.uv[3] - q.uv[1]) * t, false).x + q.params2[0];
                        float texelsPerPixel = std::fabs(q.uv[2] - q.uv[0]) * static_cast<float>(tex.w) / std::max(1e-3f, rw * pixelsPerUnit);
                        float w = std::clamp(0.7f * texelsPerPixel / (2.f * static_cast<float>(text::Font::kSpread)), 0.02f, 0.5f);
                        float fill = smoothstep(0.5f - w, 0.5f + w, d);
                        float outline = q.params2[1];
                        if (outline > 0.f) {
                            float outer = smoothstep(0.5f - outline - w, 0.5f - outline + w, d);
                            Vec4 c{color2.x + (color.x - color2.x) * fill, color2.y + (color.y - color2.y) * fill,
                                   color2.z + (color.z - color2.z) * fill, 1};
                            blendSrgb(p, c, outer * (color2.w + (color.w - color2.w) * fill) * opacity);
                        } else {
                            blendSrgb(p, color, fill * color.w * opacity);
                        }
                        break;
                    }
                }
            });
        }
    }
}

}  // namespace sky::raster2d
