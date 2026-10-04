// CPU fallback renderer: sky gradient plus depth-sorted, flat-shaded screen-space boxes.
// It is not meant to be pretty; it guarantees agents always get *a* picture with the
// right layout on platforms that don't have a GPU backend yet (and in CI).

#include <algorithm>
#include <cmath>

#include "skywalker/core/Log.h"
#include "skywalker/render/MeshData.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render/ShadowAtlas.h"
#include "skywalker/render2d/Raster2D.h"

namespace sky {

#if SKY_HAS_METAL
std::unique_ptr<Renderer> createMetalRenderer();  // MetalRenderer.mm
#endif

namespace {

uint8_t toByte(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return static_cast<uint8_t>(std::pow(v, 1.f / 2.2f) * 255.f + 0.5f);
}

class NullRenderer final : public Renderer {
public:
    RendererInfo info() const override { return {"null", "cpu"}; }
    Json localShadowInfo() const override { return shadows_.info(); }
    void invalidateLocalShadows() override { shadows_.invalidate(); }

    Status render(const FrameData& frame) override {
        // Local shadows are planned on the CPU like on the GPU backends (shadow_atlas_info works headless).
        shadows_.plan(frame, shadows::settingsFor(frame.environment, frame.quality, frame.samples > 1 || frame.offline.enabled));
        image_ = Image(frame.width, frame.height);
        const Environment& env = frame.environment;
        for (int y = 0; y < frame.height; ++y) {
            float t = 1.f - static_cast<float>(y) / static_cast<float>(frame.height);
            Vec3 c = lerp(env.skyHorizon.xyz(), env.skyTop.xyz(), t);
            for (int x = 0; x < frame.width; ++x) {
                uint8_t* p = image_.at(x, y);
                p[0] = toByte(c.x);
                p[1] = toByte(c.y);
                p[2] = toByte(c.z);
                p[3] = 255;
            }
        }
        std::vector<const DrawItem*> order;
        for (const auto& d : frame.draws) order.push_back(&d);
        std::sort(order.begin(), order.end(), [&](const DrawItem* a, const DrawItem* b) {
            return distance(frame.camera.eye, a->worldBounds.center()) > distance(frame.camera.eye, b->worldBounds.center());
        });
        const Mat4 vp = frame.viewProjection();
        Vec3 sun = -env.sunDirection();
        for (const DrawItem* d : order) {
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            bool ok = true;
            for (int i = 0; i < 8 && ok; ++i) {
                const Aabb& b = d->worldBounds;
                Vec3 c{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
                Vec4 cl = vp * Vec4(c, 1);
                if (cl.w <= 1e-4f) { ok = false; break; }
                float sx = (cl.x / cl.w * 0.5f + 0.5f) * frame.width;
                float sy = (0.5f - cl.y / cl.w * 0.5f) * frame.height;
                minX = std::min(minX, sx); maxX = std::max(maxX, sx);
                minY = std::min(minY, sy); maxY = std::max(maxY, sy);
            }
            if (!ok) continue;
            float shade = env.ambient + std::max(0.f, sun.y) * 0.6f;
            Vec3 col = d->surface.color.xyz() * shade + d->surface.emissive.xyz() * d->surface.emissive.w;
            int x0 = std::max(0, static_cast<int>(minX)), x1 = std::min(frame.width, static_cast<int>(maxX));
            int y0 = std::max(0, static_cast<int>(minY)), y1 = std::min(frame.height, static_cast<int>(maxY));
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    uint8_t* p = image_.at(x, y);
                    p[0] = toByte(col.x);
                    p[1] = toByte(col.y);
                    p[2] = toByte(col.z);
                }
            }
        }
        // 2D: sprites, tiles, world text and lights, then UI (so headless captures show HUDs and menus).
        raster2d::drawWorld(image_, frame, textures_);
        raster2d::drawUI(image_, frame, textures_);
        if (frame.fade.alpha > 0.f) {  // scene transition: fade the whole picture toward a color
            const float a = std::clamp(frame.fade.alpha, 0.f, 1.f);
            const uint8_t target[3] = {toByte(frame.fade.color.x), toByte(frame.fade.color.y), toByte(frame.fade.color.z)};
            for (int y = 0; y < frame.height; ++y) {
                for (int x = 0; x < frame.width; ++x) {
                    uint8_t* p = image_.at(x, y);
                    for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(p[c] + (target[c] - p[c]) * a + 0.5f);
                }
            }
        }
        return {};
    }

    Result<Image> readback() override {
        if (image_.width == 0) return Error::make("no_frame", "nothing has been rendered yet");
        return image_;
    }
    Status present(void*) override { return {}; }
    Status uploadMesh(const std::string&, const MeshData&) override { return {}; }
    void invalidate(const std::string& key) override { textures_.invalidate(key); }
    Status reloadShaders(const std::string&) override {
        return Error::make("unsupported", "the null renderer has no shaders");
    }
    std::string shaderSource() const override { return {}; }

private:
    Image image_;
    shadows::LocalShadowPlanner shadows_;
    render2d::ImageCache textures_;  // decoded sprite/UI images
};

}  // namespace

std::unique_ptr<Renderer> createNullRenderer() { return std::make_unique<NullRenderer>(); }

std::unique_ptr<Renderer> createRenderer(RendererBackend backend) {
#if SKY_HAS_METAL
    if (backend == RendererBackend::Auto || backend == RendererBackend::Metal) {
        if (auto r = createMetalRenderer()) return r;
        log::warn("render", "Metal unavailable; falling back to the CPU renderer");
    }
#else
    if (backend == RendererBackend::Metal) log::warn("render", "Metal is not available on this platform");
#endif
    return createNullRenderer();
}

}  // namespace sky
