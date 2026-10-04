// Top-down 2D features: y-sorting (sprites and tile rows, tall tiles), animated tiles, palette swaps,
// pixel-art particles (particles2d + tools) and pixel-art UI images (imageFilter, sliceScale).

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/Image.h"
#include "skywalker/render2d/Particles2D.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/render2d/Tilemap.h"
#include "skywalker/scene/Scene.h"
#include "skywalker/ui/UiStyle.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("sky_2dtd_" + std::string(name) + "_" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

Image strip(const std::vector<std::array<uint8_t, 4>>& colors, int tile) {
    Image img(static_cast<int>(colors.size()) * tile, tile);
    for (size_t i = 0; i < colors.size(); ++i) {
        for (int y = 0; y < tile; ++y) {
            for (int x = 0; x < tile; ++x) {
                uint8_t* p = img.at(static_cast<int>(i) * tile + x, y);
                for (int c = 0; c < 4; ++c) p[c] = colors[i][static_cast<size_t>(c)];
            }
        }
    }
    return img;
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p);
    f << text;
}

ViewCamera topDown() {
    ViewCamera cam;
    cam.eye = {0, 0, 10};
    cam.target = {0, 0, 0};
    cam.orthographic = true;
    cam.orthoSize = 8;
    return cam;
}

/// x of the first instance drawn for each entity, in draw order.
std::vector<float> drawXs(const FrameData& f) {
    std::vector<float> xs;
    for (const auto& s : f.render2d.sprites) xs.push_back(s.origin[0]);
    return xs;
}

}  // namespace

TEST_CASE("2d topdown: ySort sprites draw lower-on-screen in front, after plain entries of their order") {
    Scene s;
    auto make = [&](const char* name, Vec3 pos, bool ySort) {
        EntityId e = s.create(name);
        s.get<Transform>(e)->position = pos;
        Sprite& sp = s.add<Sprite>(e);
        sp.size = {1, 1};
        sp.pivot = {0, 0};
        sp.ySort = ySort;
        return e;
    };
    make("Low", {1, -2, 0}, true);
    make("High", {2, 3, 0}, true);
    make("Ground", {3, -5, 0}, false);
    make("Mid", {4, 0.5f, 0}, true);
    render2d::Assets2D assets(".");
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, topDown(), 200, 200, bo);
    render2d::gather2D(s, assets, f, {});
    std::vector<float> xs = drawXs(f);
    REQUIRE(xs.size() == 4);
    CHECK(xs[0] == doctest::Approx(3.f));  // plain (ground) first
    CHECK(xs[1] == doctest::Approx(2.f));  // y = 3 (back)
    CHECK(xs[2] == doctest::Approx(4.f));  // y = 0.5
    CHECK(xs[3] == doctest::Approx(1.f));  // y = -2 (front)
}

TEST_CASE("2d topdown: ySort tile rows interleave with sprites; tall tiles sort with their base; tiles animate") {
    fs::path dir = tempDir("rows");
    // 4 tiles: 1 red, 2 green, 3 blue, 4 white.
    REQUIRE(writePng(strip({{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}, 16),
                     (dir / "tiles.png").string()).ok());
    writeText(dir / "tiles.tileset.json", R"({"image": "tiles.png", "tileSize": 16,
        "animations": {"3": {"frames": [3, 4], "fps": 2}},
        "sortOffset": {"2": 1}})");
    Scene s;
    EntityId map = s.create("Trees");
    s.get<Transform>(map)->position = {0, 4, 0};  // rows: 0 spans y 4..3, 1 spans 3..2, 2 spans 2..1, 3 spans 1..0
    Tilemap& tm = s.add<Tilemap>(map);
    tm.tileset = "tiles.tileset.json";
    tm.width = 1;
    tm.height = 4;
    tm.sortingLayer = "default";
    // Row 0: a tall tile's top (2) whose base is row 1 (1).
    // An empty plain layer first: a ySort layer keeps the map/layer order whatever its index, so it still
    // interleaves with ySort sprites of order 0.
    tm.layers = Json::array({Json::object({{"name", "ground"}, {"data", "rle:4*0"}}),
                             Json::object({{"name", "objects"}, {"data", "rle:2,1,0,3"}, {"ySort", true}})});
    EntityId hero = s.create("Hero");
    s.get<Transform>(hero)->position = {5, 2.5f, 0};  // between row 1's base (y = 2) and row 0's (y = 3)
    Sprite& sp = s.add<Sprite>(hero);
    sp.size = {1, 1};
    sp.ySort = true;
    render2d::Assets2D assets(dir.string());
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, topDown(), 200, 200, bo);
    render2d::gather2D(s, assets, f, {});
    REQUIRE(f.render2d.sprites.size() == 4);
    // Hero (y 2.5) is in front of nothing with base y >= 3, behind rows whose base is y = 2 and lower. The tall
    // top (row 0, offset 1) sorts with row 1 (base y = 2): both draw after the hero.
    CHECK(f.render2d.sprites[0].origin[0] == doctest::Approx(4.5f));  // hero first: no row has its base above y 2.5
    CHECK(f.render2d.sprites[1].origin[1] == doctest::Approx(4.f));   // tall top (row 0) ...
    CHECK(f.render2d.sprites[2].origin[1] == doctest::Approx(3.f));   // ... with its base (row 1)
    CHECK(f.render2d.sprites[3].origin[1] == doctest::Approx(1.f));   // row 3 (animated tile)
    // Animated tile 3 -> 4 after half a second at 2 fps.
    const float u3 = f.render2d.sprites[3].uv[0];
    render2d::Gather2DOptions later;
    later.time = 0.6f;
    FrameData g = buildFrame(s, topDown(), 200, 200, bo);
    render2d::gather2D(s, assets, g, later);
    CHECK(u3 == doctest::Approx(32.02f / 64.f));
    CHECK(g.render2d.sprites[3].uv[0] == doctest::Approx(48.02f / 64.f));
    // The layer's ySort flag survives the round trip through the editable grid.
    auto grid = tiles::Grid::fromComponent(tm);
    REQUIRE(grid);
    CHECK(grid->layers[1].ySort);
    CHECK_FALSE(grid->layers[0].ySort);
    Tilemap copy;
    grid->writeTo(copy);
    CHECK(copy.layers.elements()[1].get("ySort").asBool());
    // Bad animation specs are explained.
    writeText(dir / "bad.tileset.json", R"({"image": "tiles.png", "animations": {"x": [1]}})");
    auto bad = assets.tileset("bad.tileset.json", 16);
    CHECK_FALSE(bad);
    CHECK(bad.error().code == "invalid_tileset");
    fs::remove_all(dir);
}

TEST_CASE("2d topdown: palette swaps recolor sprites and tilesets from json or png strips") {
    fs::path dir = tempDir("palette");
    REQUIRE(writePng(strip({{58, 125, 68, 255}, {200, 10, 10, 255}}, 2), (dir / "grass.png").string()).ok());
    writeText(dir / "winter.palette.json", R"({"swap": {"#3a7d44": "#e8f0f8", "#c80a0a": "#00000000"}})");
    Image pal(2, 2);
    const uint8_t rows[2][2][4] = {{{58, 125, 68, 255}, {0, 0, 0, 0}}, {{200, 120, 40, 255}, {0, 0, 0, 0}}};
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            for (int c = 0; c < 4; ++c) pal.at(x, y)[c] = rows[y][x][c];
        }
    }
    REQUIRE(writePng(pal, (dir / "autumn.png").string()).ok());

    auto json = render2d::loadPalette((dir / "winter.palette.json").string());
    REQUIRE(json);
    CHECK(json->size() == 2);
    auto png = render2d::loadPalette((dir / "autumn.png").string());
    REQUIRE(png);
    CHECK(png->size() == 1);
    Image img = strip({{58, 125, 68, 255}, {200, 10, 10, 128}}, 1);
    CHECK(render2d::applyPalette(img, json.value()) == 2);
    CHECK(img.at(0, 0)[0] == 0xe8);
    CHECK(img.at(1, 0)[3] == 0);  // swapped to transparent
    writeText(dir / "broken.palette.json", R"({"swap": {"green": "#fff"}})");
    CHECK(render2d::loadPalette((dir / "broken.palette.json").string()).error().code == "invalid_palette");

    Scene s;
    EntityId e = s.create("Grass");
    Sprite& sp = s.add<Sprite>(e);
    sp.texture = "grass.png";
    sp.palette = "winter.palette.json";
    render2d::Assets2D assets(dir.string());
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, topDown(), 64, 64, bo);
    render2d::gather2D(s, assets, f, {});
    REQUIRE(f.render2d.spriteBatches.size() == 1);
    TextureImagePtr swapped = f.render2d.spriteBatches[0].texture.image;
    REQUIRE(swapped);
    CHECK(swapped->pixels[0] == 0xe8);
    // Cached while the files are unchanged.
    FrameData g = buildFrame(s, topDown(), 64, 64, bo);
    render2d::gather2D(s, assets, g, {});
    CHECK(g.render2d.spriteBatches[0].texture.image == swapped);
    fs::remove_all(dir);
}

TEST_CASE("2d topdown: particles2d simulate deterministically, cap, wrap around the view and draw as pixels") {
    for (const auto& name : render2d::particles2dPresets()) {
        Scene s;
        EntityId e = s.create(name);
        INFO(name);
        CHECK(s.patchComponent(e, "particles2d", render2d::particles2dPreset(name)).ok());
    }
    CHECK(render2d::particles2dPreset("hail").isNull());

    Particles2D a;
    a.rate = 50;
    a.maxParticles = 40;
    a.lifetime = 2;
    a.area = {10, 6};
    a.wrap = true;
    a.velocity = {3, -9};
    a.seed = 7;
    Particles2D b = a;
    for (int i = 0; i < 120; ++i) {
        render2d::stepParticles2D(a, {100, 50, 0}, 1.f / 60.f, 1, 42);
        render2d::stepParticles2D(b, {100, 50, 0}, 1.f / 60.f, 1, 42);
    }
    REQUIRE(a.particles_.size() == b.particles_.size());
    CHECK(a.particles_.size() == 40);  // capped (prewarmed and saturated)
    for (size_t i = 0; i < a.particles_.size(); ++i) {
        CHECK(a.particles_[i].pos == b.particles_[i].pos);
        CHECK(a.particles_[i].pos.x >= 0.f);
        CHECK(a.particles_[i].pos.x < 10.f);  // wrapped into the box
        Vec3 w = render2d::particleWorldPosition(a, a.particles_[i], {100, 50, 0}, {500, -20, 10});
        CHECK(std::fabs(w.x - 500.f) <= 5.001f);  // the tiling copy nearest the eye, far from the entity
        CHECK(std::fabs(w.y + 20.f) <= 3.001f);
    }
    // burst() from Wander queues particles that appear on the next step, even on a silent emitter.
    Particles2D puff;
    puff.rate = 0;
    puff.maxParticles = 8;
    render2d::stepParticles2D(puff, {0, 0, 0}, 1.f / 60.f, 1, 1);
    CHECK(puff.particles_.empty());
    puff.pendingBurst_ = 12;
    render2d::stepParticles2D(puff, {0, 0, 0}, 1.f / 60.f, 1, 1);
    CHECK(puff.particles_.size() == 8);  // capped
    CHECK(puff.pendingBurst_ == 0);

    Particles2D other = a;
    other.particles_.clear();
    other.started_ = false;
    other.seed = 8;
    render2d::stepParticles2D(other, {100, 50, 0}, 1.f / 60.f, 1, 42);
    CHECK_FALSE(other.particles_[0].pos == a.particles_[0].pos);

    // Drawn as one nearest-sampled batch of untextured pixel quads (pixelSize / pixelsPerUnit).
    Scene s;
    EntityId e = s.create("Rain");
    Particles2D& p = s.add<Particles2D>(e);
    p = a;
    p.pixelSize = {1, 4};
    p.pixelsPerUnit = 16;
    p.fadeIn = p.fadeOut = 0;
    render2d::Assets2D assets(".");
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, topDown(), 64, 64, bo);
    render2d::gather2D(s, assets, f, {});
    CHECK(f.render2d.sprites.size() == p.particles_.size());
    REQUIRE(!f.render2d.spriteBatches.empty());
    CHECK(f.render2d.spriteBatches[0].nearest);
    CHECK(f.render2d.sprites[0].axisY[1] == doctest::Approx(-4.f / 16.f));
}

TEST_CASE("2d topdown: particles2d tools and play/stop reset") {
    fs::path dir = tempDir("ptools");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = dir.string();
    {
        Engine eng(cfg);
        (void)eng.newScene("Weather", false);
        auto call = [&](const char* tool, const char* args, bool ok = true) {
            ToolResult r = eng.callTool(tool, Json::parse(args).value(), "agent:test");
            INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
            CHECK(r.isError == !ok);
            return r;
        };
        call("particles2d_create", R"({"preset": "snow", "name": "Snowfall", "overrides": {"rate": 200}})");
        call("particles2d_create", R"({"preset": "snw"})", false);
        ToolResult typo = call("particles2d_create", R"({"preset": "firefly"})", false);
        CHECK(typo.content.front().text.find("fireflies") != std::string::npos);
        EntityId snow = eng.scene().find("Snowfall");
        REQUIRE(snow);
        CHECK(eng.scene().get<Particles2D>(snow)->rate == doctest::Approx(200.f));
        CHECK(eng.scene().get<Particles2D>(snow)->wrap);
        call("sim_control", R"({"action": "play"})");
        call("sim_control", R"({"action": "step", "ticks": 30})");
        Json info = call("particles2d_info", R"({"entity": "Snowfall"})").structured;
        CHECK(info.get("alive").asInt() > 100);
        call("sim_control", R"({"action": "stop"})");
        snow = eng.scene().find("Snowfall");
        REQUIRE(snow);
        CHECK(eng.scene().get<Particles2D>(snow)->particles_.empty());
        call("particles2d_info", R"({"entity": "Snowfall"})");
        call("particles2d_info", R"({})");
    }
    fs::remove_all(dir);
}

TEST_CASE("2d topdown: pixel UI images sample nearest and 9-slices scale their borders") {
    auto pixel = ui::StyleSheet::theme("pixel");
    ui::Style img = pixel->resolve("image", {}, "", Json::object(), {}, nullptr);
    CHECK(img.imageFilter == "nearest");
    auto dark = ui::StyleSheet::theme("dark");
    ui::Style plain = dark->resolve("image", {}, "", Json::object(), {}, nullptr);
    CHECK(plain.imageFilter == "linear");
    ui::Style framed = dark->resolve("panel", {}, "", Json::object({{"sliceScale", 5}, {"imageFilter", "nearest"}}), {}, nullptr);
    CHECK(framed.sliceScale == doctest::Approx(5.f));
    CHECK(framed.imageFilter == "nearest");
}

TEST_CASE("2d topdown: banded lights carry their steps to the frame (light and halo) with the texel grid") {
    Scene s;
    EntityId e = s.create("Lamp");
    Light2D& l = s.add<Light2D>(e);
    l.bands = 6;
    l.halo = 0.5f;
    EntityId plain = s.create("Plain");
    s.add<Light2D>(plain);
    render2d::Assets2D assets(".");
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, topDown(), 64, 64, bo);
    render2d::Gather2DOptions o;
    o.pixelSnap = 1.f / 16.f;
    render2d::gather2D(s, assets, f, o);
    REQUIRE(f.render2d.lights.size() == 2);
    CHECK(f.render2d.lights[0].bands == 6);
    CHECK(f.render2d.lights[1].bands == 0);
    CHECK(f.render2d.texel == doctest::Approx(1.f / 16.f));
    bool halo = false;
    for (const auto& q : f.render2d.sprites) {
        if (static_cast<int>(q.params[0]) == static_cast<int>(SpriteMode::Halo)) {
            halo = true;
            CHECK(q.params[1] == doctest::Approx(6.f));
        }
    }
    CHECK(halo);
}
