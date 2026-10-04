// Painted 2D: sprite blur / sway / additive blend / flash, sprite_trail afterimages, atlas normal maps,
// particles2d filter and blend, hit-stop, per-entity time scale, camera shake, sprite_sheet_import and game_feel.

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/Image.h"
#include "skywalker/render2d/Atlas.h"
#include "skywalker/render2d/ImageIO.h"
#include "skywalker/render2d/Particles2D.h"
#include "skywalker/render2d/Raster2D.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/scene/Scene.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("sky_p2d_" + std::string(name) + "_" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

/// A w x h image with an opaque `color` disc in the middle (transparent corners: trimming has work to do).
Image disc(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f;
            const float dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 0.5f;
            if (dx * dx + dy * dy > 0.16f) continue;
            uint8_t* p = img.at(x, y);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = 255;
        }
    }
    return img;
}

FrameData frameOf(const Scene& s, render2d::Assets2D& assets, int w = 200, int h = 200) {
    ViewCamera cam;
    cam.eye = {0, 0, 10};
    cam.target = {0, 0, 0};
    cam.orthographic = true;
    cam.orthoSize = 3;
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, cam, w, h, bo);
    render2d::gather2D(s, assets, f, {});
    return f;
}

}  // namespace

TEST_CASE("painted 2d: blur, sway, additive blend and flash reach the sprite instances") {
    Scene s;
    EntityId e = s.create("Curtain");
    Sprite& sp = s.add<Sprite>(e);
    sp.size = {1, 2};
    sp.blur = 6.f;
    sp.sway = {0.05f, 0.4f, 1.5f, 1.f};
    render2d::Assets2D assets(".");
    FrameData f = frameOf(s, assets);
    REQUIRE(f.render2d.sprites.size() == 1);
    const SpriteInstance& q = f.render2d.sprites[0];
    CHECK(q.fx[0] == doctest::Approx(6.f));
    CHECK(q.fx[1] == doctest::Approx(0.05f));
    CHECK(q.fx[2] == doctest::Approx(0.4f));
    CHECK(q.fx[3] == doctest::Approx(1.5f));
    CHECK(q.extra[2] == doctest::Approx(1.f));  // pinned at the top
    CHECK(q.extra[3] == doctest::Approx(0.f));
    CHECK_FALSE(f.render2d.spriteBatches[0].additive);
    CHECK(q.flash[3] == doctest::Approx(0.f));

    // Additive sprites get their own (additive) batch and premultiply in the shader.
    sp.blend = "add";
    render2d::flashSprite(sp, 0.5f, Vec4{1, 0, 0, 1});
    FrameData g = frameOf(s, assets);
    CHECK(g.render2d.spriteBatches[0].additive);
    CHECK(g.render2d.sprites[0].extra[3] == doctest::Approx(1.f));
    CHECK(g.render2d.sprites[0].flash[3] == doctest::Approx(1.f));
    CHECK(g.render2d.sprites[0].flash[0] == doctest::Approx(1.f));
    // The flash fades with ticks.
    for (int i = 0; i < 15; ++i) render2d::tickSpriteFx(s, assets, 1.f / 60.f);
    FrameData h = frameOf(s, assets);
    CHECK(h.render2d.sprites[0].flash[3] == doctest::Approx(0.5f).epsilon(0.02));
    for (int i = 0; i < 30; ++i) render2d::tickSpriteFx(s, assets, 1.f / 60.f);
    CHECK(frameOf(s, assets).render2d.sprites[0].flash[3] == doctest::Approx(0.f));

    // The CPU rasterizer honours the flash (a white-flashed red quad turns white).
    sp.blend = "alpha";
    sp.flash = {1, 1, 1, 1};
    sp.blur = 0;
    FrameData k = frameOf(s, assets, 64, 64);
    Image img(64, 64);
    render2d::ImageCache cache;
    raster2d::drawWorld(img, k, cache);
    const uint8_t* center = img.at(32, 32);
    CHECK(center[0] > 240);
    CHECK(center[1] > 240);
}

TEST_CASE("painted 2d: sprite_trail records afterimages while moving and draws them behind the sprite") {
    Scene s;
    EntityId e = s.create("Heroine");
    Sprite& sp = s.add<Sprite>(e);
    sp.size = {1, 1};
    SpriteTrail& t = s.add<SpriteTrail>(e);
    t.count = 4;
    t.interval = 1.f / 30.f;
    t.minSpeed = 1.f;
    render2d::Assets2D assets(".");
    // Standing still: nothing recorded.
    for (int i = 0; i < 10; ++i) render2d::tickSpriteFx(s, assets, 1.f / 60.f);
    CHECK(s.get<SpriteTrail>(e)->snapshots_.empty());
    // Dashing right at 12 units/s.
    for (int i = 0; i < 20; ++i) {
        s.get<Transform>(e)->position.x += 0.2f;
        s.markDirty();
        render2d::tickSpriteFx(s, assets, 1.f / 60.f);
    }
    const auto& snaps = s.get<SpriteTrail>(e)->snapshots_;
    REQUIRE(snaps.size() == 4);
    CHECK(snaps.back().world.translation().x > snaps.front().world.translation().x);
    FrameData f = frameOf(s, assets);
    REQUIRE(f.render2d.sprites.size() == 5);
    // Ghosts first (oldest leftmost, faintest), the sprite itself last at full opacity.
    CHECK(f.render2d.sprites[0].origin[0] < f.render2d.sprites[3].origin[0]);
    CHECK(f.render2d.sprites[0].color[3] < f.render2d.sprites[3].color[3]);
    CHECK(f.render2d.sprites[4].color[3] == doctest::Approx(1.f));
    // Stopping: the afterimages expire.
    for (int i = 0; i < 30; ++i) render2d::tickSpriteFx(s, assets, 1.f / 60.f);
    CHECK(s.get<SpriteTrail>(e)->snapshots_.empty());
    CHECK(frameOf(s, assets).render2d.sprites.size() == 1);
}

TEST_CASE("painted 2d: an atlas's companion normal map lights animated frames; particles2d filter and blend") {
    fs::path dir = tempDir("atlasnormal");
    REQUIRE(writePng(disc(32, 32, 255, 255, 255), (dir / "hero.png").string()).ok());
    REQUIRE(writePng(disc(32, 32, 128, 128, 255), (dir / "hero_n.png").string()).ok());
    {
        std::ofstream(dir / "hero.atlas.json") << R"({"format": "skywalker.atlas", "image": "hero.png", "normalMap": "hero_n.png",
            "width": 32, "height": 32, "frames": {"idle_0": {"rect": [0, 0, 32, 32], "source": [32, 32], "offset": [0, 0]}}})";
    }
    auto atlas = render2d::loadAtlas((dir / "hero.atlas.json").string());
    REQUIRE(atlas.ok());
    CHECK(atlas->normalMap == "hero_n.png");
    CHECK(atlas->toJson().get("normalMap").asString() == "hero_n.png");
    Scene s;
    EntityId e = s.create("Hero");
    Sprite& sp = s.add<Sprite>(e);
    sp.texture = "hero.atlas.json";
    sp.pixelsPerUnit = 32;
    EntityId p = s.create("Motes");
    Particles2D& motes = s.add<Particles2D>(p);
    motes.filter = "linear";
    motes.blend = "add";
    motes.sizeJitter = 0.5f;
    motes.rate = 0;
    motes.burst = 0;
    motes.prewarm = false;
    render2d::Assets2D assets(dir.string());
    motes.started_ = true;
    for (int i = 0; i < 20; ++i) motes.particles_.push_back({});
    render2d::stepParticles2D(motes, {0, 0, 0}, 1.f / 60.f, 1, 1);
    FrameData f = frameOf(s, assets);
    REQUIRE(f.render2d.spriteBatches.size() == 2);
    const SpriteBatch& hero = f.render2d.spriteBatches[0];
    CHECK(fs::path(hero.normalMap.path).filename() == "hero_n.png");
    CHECK(f.render2d.sprites[0].params[2] == doctest::Approx(1.f));
    const SpriteBatch& mb = f.render2d.spriteBatches[1];
    CHECK_FALSE(mb.nearest);
    CHECK(mb.additive);
    fs::remove_all(dir);
}

TEST_CASE("painted 2d: hit-stop freezes the game clock for real ticks; process.timeScale slows one entity") {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    Engine eng(cfg);
    (void)eng.newScene("Feel", false);
    Scene& s = eng.scene();
    EntityId slow = s.create("Slow");
    s.add<Process>(slow).timeScale = 0.25f;
    EntityId kid = s.create("Kid");
    REQUIRE(s.setParent(kid, slow).ok());
    EntityId normal = s.create("Normal");
    REQUIRE(s.setBehaviors(normal, Json::parse(R"([{"name": "Hit", "source": "behavior Hit\n  var hits = 0\n  on start\n    hit_stop(0.1)\n    hits = 1\n  end\nend"}])").value()).ok());
    eng.step(1);  // the start handler requests 6 ticks of hit-stop
    const double t0 = eng.runtime().time();
    CHECK(eng.runtime().hitStopRemaining() == doctest::Approx(0.1).epsilon(0.02));
    eng.step(6);
    CHECK(eng.runtime().time() == doctest::Approx(t0));  // frozen
    CHECK(eng.runtime().hitStopRemaining() == doctest::Approx(0.0));
    eng.step(6);
    CHECK(eng.runtime().time() == doctest::Approx(t0 + 0.1).epsilon(0.01));
    const ProcessGate& gate = eng.runtime().processGate();
    CHECK(gate.scale(slow) == doctest::Approx(0.25f));
    CHECK(gate.scale(kid) == doctest::Approx(0.25f));  // inherited
    CHECK(gate.scale(normal) == doctest::Approx(1.f));
    CHECK(resolveProcess(s, kid).speed == doctest::Approx(0.25f));
    CHECK(eng.recentMessages().empty());
    eng.stop();
}

TEST_CASE("painted 2d: camera shake is deterministic, offsets the 2D view and decays") {
    auto run = [](int ticks) {
        Scene s;
        EntityId cam = s.create("Camera");
        s.add<Camera>(cam).orthoSize = 5;
        Camera2D& c2 = s.add<Camera2D>(cam);
        c2.pixelSnap = false;
        c2.shakeAmplitude = 0.5f;
        render2d::addCameraShake(c2, 0.8f);
        for (int i = 0; i < ticks; ++i) render2d::tickCameras(s, 1.f / 60.f);
        return *s.get<Camera2D>(cam);
    };
    Camera2D a = run(5), b = run(5);
    CHECK(a.shakeOffset_.x == doctest::Approx(b.shakeOffset_.x));
    CHECK(a.shakeOffset_.y == doctest::Approx(b.shakeOffset_.y));
    CHECK(std::fabs(a.shakeOffset_.x) + std::fabs(a.shakeOffset_.y) > 1e-3f);
    CHECK(std::fabs(a.shakeOffset_.x) <= 0.5f);
    CHECK(a.trauma_ < 0.8f);
    Camera2D later = run(120);
    CHECK(later.trauma_ == doctest::Approx(0.f));
    CHECK(later.shakeOffset_.x == doctest::Approx(0.f));
    // The offset moves the view.
    Scene s;
    EntityId cam = s.create("Camera");
    s.add<Camera>(cam).orthoSize = 5;
    Camera2D& c2 = s.add<Camera2D>(cam);
    c2.pixelSnap = false;
    c2.shakeOffset_ = {0.3f, -0.2f};
    ViewCamera view;
    view.eye = {0, 0, 10};
    view.target = {0, 0, 0};
    render2d::applyCamera2D(s, cam, view, 100, 100);
    CHECK(view.eye.x == doctest::Approx(0.3f));
    CHECK(view.eye.y == doctest::Approx(-0.2f));
}

TEST_CASE("painted 2d tools: sprite_sheet_import packs rendered clips with normals; game_feel drives impacts") {
    fs::path dir = tempDir("import");
    for (const char* clip : {"idle", "run"}) {
        fs::create_directories(dir / "renders" / clip);
        fs::create_directories(dir / "normals" / clip);
        for (int i = 1; i <= 3; ++i) {
            char name[16];
            std::snprintf(name, sizeof name, "%04d.png", i);
            REQUIRE(writePng(disc(40, 60, 200, 60, 60), (dir / "renders" / clip / name).string()).ok());
            REQUIRE(writePng(disc(40, 60, 128, 128, 255), (dir / "normals" / clip / name).string()).ok());
        }
    }
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = dir.string();
    {
        Engine e(cfg);
        (void)e.newScene("Import", false);
        auto call = [&](const char* tool, const char* args, bool ok = true) {
            ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
            INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
            CHECK(r.isError == !ok);
            return r;
        };
        (void)call("entity_create", R"({"name": "Heroine"})");
        ToolResult r = call("sprite_sheet_import", R"({"folder": "renders", "normals": "normals", "output": "art/heroine",
            "downsample": 2, "fps": 30, "clips": {"run": {"events": {"1": "step"}}}, "entity": "Heroine",
            "pivot": [0.5, 0.1], "pixels_per_unit": 20, "clip": "run"})");
        const Json& j = r.structured;
        CHECK(j.get("frames").asInt() == 6);
        CHECK(j.get("clips").get("run").get("fps").asFloat() == doctest::Approx(30.f));
        CHECK(j.get("clips").get("run").get("events").get("1").asString() == "step");
        CHECK(fs::exists(dir / "art" / "heroine.png"));
        CHECK(fs::exists(dir / "art" / "heroine_n.png"));
        auto atlas = render2d::loadAtlas((dir / "art" / "heroine.atlas.json").string());
        REQUIRE(atlas.ok());
        CHECK(atlas->normalMap == "heroine_n.png");
        REQUIRE(atlas->frames.size() == 6);
        CHECK(atlas->frames[0].sourceW == 20);  // downsampled 2x
        CHECK(atlas->frames[0].w < 20);         // trimmed
        auto normalImg = render2d::loadImage((dir / "art" / "heroine_n.png").string());
        REQUIRE(normalImg.ok());
        const auto& f0 = atlas->frames[0];
        const uint8_t* n = normalImg->at(f0.x + f0.w / 2, f0.y + f0.h / 2);
        CHECK(n[2] > 240);  // the copied normal (facing the viewer)
        EntityId h = e.scene().find("Heroine");
        CHECK(e.scene().get<SpriteAnimator>(h)->clip == "run");
        CHECK(e.scene().get<Sprite>(h)->pivot.y == doctest::Approx(0.1f));
        // Did-you-mean on clip names.
        ToolResult bad = call("sprite_sheet_import", R"({"folder": "renders", "output": "art/x", "clips": {"rnu": {}}})", false);
        CHECK(bad.content.front().text.find("run") != std::string::npos);
        // game_feel while playing.
        (void)call("entity_create", R"({"name": "Cam", "components": {"camera": {"orthographic": true}, "camera2d": {"pixelSnap": false}}})");
        e.play();
        (void)call("game_feel", R"({"action": "shake", "trauma": 0.6, "camera": "Cam"})");
        (void)call("game_feel", R"({"action": "flash", "entity": "Heroine", "color": "#ff0000"})");
        (void)call("game_feel", R"({"action": "hit_stop", "seconds": 0.05})");
        e.step(1);
        ToolResult info = call("game_feel", R"({"action": "info"})");
        CHECK(info.structured.get("hitStopLeft").asFloat() > 0.f);
        CHECK(info.structured.get("flashes").size() == 1);
        CHECK(info.structured.get("cameras")[size_t{0}].get("trauma").asFloat() > 0.f);
        e.stop();
    }
    fs::remove_all(dir);
}
