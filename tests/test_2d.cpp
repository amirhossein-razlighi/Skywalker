// 2D: atlases, sprite sheets, tilemap encoding/editing/auto-tiling/collision, sprite gathering and sorting.

#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <random>
#include <set>

#include "skywalker/render/Image.h"
#include "skywalker/render2d/Atlas.h"
#include "skywalker/render2d/ImageIO.h"
#include "skywalker/render2d/Sprites.h"
#include "skywalker/render2d/Tilemap.h"
#include "skywalker/scene/Scene.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const char* name) {
    fs::path p = fs::temp_directory_path() / ("sky_2d_" + std::string(name) + "_" + std::to_string(::getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    Image img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.at(x, y);
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
        }
    }
    return img;
}

}  // namespace

TEST_CASE("2d: PNG round trip through the decoder") {
    fs::path dir = tempDir("png");
    Image img = solid(5, 3, 10, 200, 30);
    img.at(4, 2)[0] = 255;
    REQUIRE(writePng(img, (dir / "a.png").string()).ok());
    auto back = render2d::loadImage((dir / "a.png").string());
    REQUIRE(back.ok());
    CHECK(back->width == 5);
    CHECK(back->height == 3);
    CHECK(back->at(4, 2)[0] == 255);
    CHECK(back->at(0, 0)[1] == 200);
    int w = 0, h = 0;
    CHECK(render2d::imageInfo((dir / "a.png").string(), w, h));
    CHECK(w == 5);
    CHECK_FALSE(render2d::loadImage((dir / "missing.png").string()).ok());
    fs::remove_all(dir);
}

TEST_CASE("2d: atlas packing trims, pads, keeps order and never overlaps") {
    std::vector<render2d::PackInput> inputs;
    for (int i = 0; i < 12; ++i) {
        Image img(20 + i * 3, 16 + (i % 4) * 5);  // transparent border of 2 px around an opaque core
        for (int y = 2; y < img.height - 2; ++y) {
            for (int x = 2; x < img.width - 2; ++x) {
                uint8_t* p = img.at(x, y);
                p[0] = static_cast<uint8_t>(i * 20);
                p[3] = 255;
            }
        }
        inputs.push_back({"f" + std::to_string(i), std::move(img)});
    }
    render2d::PackOptions o;
    o.padding = 2;
    auto r = render2d::packAtlas(inputs, o);
    REQUIRE(r.ok());
    const auto& a = r->atlas;
    REQUIRE(a.frames.size() == 12);
    for (size_t i = 0; i < a.frames.size(); ++i) {
        const auto& f = a.frames[i];
        CHECK(f.name == "f" + std::to_string(i));          // input order
        CHECK(f.w == inputs[i].image.width - 4);         // trimmed
        CHECK(f.sourceW == inputs[i].image.width);
        CHECK(f.offsetX == 2);
        CHECK(f.offsetY == 2);
        CHECK(f.x + f.w <= a.width);
        CHECK(f.y + f.h <= a.height);
        // Pixels were copied.
        CHECK(r->image.at(f.x + f.w / 2, f.y + f.h / 2)[0] == static_cast<uint8_t>(i * 20));
        for (size_t j = i + 1; j < a.frames.size(); ++j) {
            const auto& g = a.frames[j];
            bool apart = f.x + f.w + o.padding <= g.x || g.x + g.w + o.padding <= f.x || f.y + f.h + o.padding <= g.y ||
                         g.y + g.h + o.padding <= f.y;
            CHECK(apart);
        }
    }
    // JSON round trip.
    render2d::Atlas named = a;
    named.image = "atlas.png";
    Json j = named.toJson();
    auto parsed = render2d::parseAtlas(j);
    REQUIRE(parsed.ok());
    CHECK(parsed->frames.size() == 12);
    CHECK(parsed->find("f3")->w == a.frames[3].w);
    // TexturePacker hash format loads too.
    auto tp = render2d::parseAtlas(Json::parse(R"({"frames":{"run_0.png":{"frame":{"x":1,"y":2,"w":8,"h":9},
        "spriteSourceSize":{"x":3,"y":1,"w":8,"h":9},"sourceSize":{"w":16,"h":16}}},"meta":{"image":"s.png","size":{"w":64,"h":32}}})")
                                       .value());
    REQUIRE(tp.ok());
    CHECK(tp->image == "s.png");
    CHECK(tp->width == 64);
    CHECK(tp->frames[0].offsetX == 3);
    CHECK(tp->frames[0].sourceW == 16);
    // Too big to fit.
    render2d::PackOptions tiny;
    tiny.maxSize = 32;
    CHECK_FALSE(render2d::packAtlas(inputs, tiny).ok());
}

TEST_CASE("2d: grid slicing and frame lists") {
    auto a = render2d::sliceGrid(64, 32, 16, 16, 0, 0, "hero_");
    REQUIRE(a.frames.size() == 8);
    CHECK(a.frames[5].x == 16);
    CHECK(a.frames[5].y == 16);
    CHECK(a.frames[5].name == "hero_5");
    auto spaced = render2d::sliceGrid(70, 18, 16, 16, 1, 2, "t");
    CHECK(spaced.frames.size() == 3);
    CHECK(spaced.frames[1].x == 19);

    auto r = render2d::parseFrameList(Json("0-3,6"), 8, nullptr);
    REQUIRE(r.ok());
    CHECK(r.value() == std::vector<int>{0, 1, 2, 3, 6});
    CHECK(render2d::parseFrameList(Json("3-1"), 8, nullptr).value() == std::vector<int>{3, 2, 1});
    CHECK(render2d::parseFrameList(Json::array({1, 1, 2}), 8, nullptr).value() == std::vector<int>{1, 1, 2});
    CHECK_FALSE(render2d::parseFrameList(Json("0-9"), 8, nullptr).ok());
    auto names = render2d::parseFrameList(Json("hero_*"), 8, &a);
    REQUIRE(names.ok());
    CHECK(names->size() == 8);
    auto bad = render2d::parseFrameList(Json("hero_55"), 8, &a);
    CHECK_FALSE(bad.ok());
    CHECK(bad.error().hint.find("hero_5") != std::string::npos);
}

TEST_CASE("2d: tile data encodes compactly and round-trips") {
    std::vector<uint32_t> cells(40 * 30, 0);
    for (int x = 0; x < 40; ++x) cells[static_cast<size_t>(29 * 40 + x)] = 3;
    cells[5] = 7 | tiles::kFlipX;
    std::string rle = tiles::encodeCells(cells);
    CHECK(rle.rfind("rle:", 0) == 0);
    CHECK(rle.size() < 60);
    auto back = tiles::decodeCells(Json(rle), cells.size());
    REQUIRE(back.ok());
    CHECK(back.value() == cells);

    std::mt19937 rng(7);
    std::vector<uint32_t> noisy(128 * 128);
    for (auto& c : noisy) c = rng() % 6;
    std::string packed = tiles::encodeCells(noisy);
    CHECK(packed.rfind("b64z:", 0) == 0);
    auto noisyBack = tiles::decodeCells(Json(packed), noisy.size());
    REQUIRE(noisyBack.ok());
    CHECK(noisyBack.value() == noisy);

    CHECK(tiles::decodeCells(Json::array({1, 2, 3, 4}), 4).value() == std::vector<uint32_t>{1, 2, 3, 4});
    CHECK(tiles::decodeCells(Json("1,2*0,5"), 4).value() == std::vector<uint32_t>{1, 0, 0, 5});
    CHECK(tiles::decodeCells(Json(), 3).value() == std::vector<uint32_t>{0, 0, 0});
    auto wrong = tiles::decodeCells(Json("rle:5*1"), 6);
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message.find("5 cells") != std::string::npos);
    CHECK_FALSE(tiles::decodeCells(Json("rle:x*1"), 1).ok());

    auto ids = tiles::parseIdList(Json::array({3, "10-12", "1,2"}));
    REQUIRE(ids.ok());
    CHECK(ids.value() == std::vector<uint32_t>{1, 2, 3, 10, 11, 12});
}

TEST_CASE("2d: grid edits, flood fill and component round trip") {
    Tilemap map;
    map.width = 6;
    map.height = 4;
    map.layers = Json::parse(R"([{"name":"ground","data":"rle:24*1","solid":true},{"name":"deco","data":""}])").value();
    auto g = tiles::Grid::fromComponent(map);
    REQUIRE(g.ok());
    CHECK(g->layers.size() == 2);
    CHECK(g->layers[0].solid == "all");
    CHECK(g->get(0, 5, 3) == 1);
    g->set(0, 2, 1, 9);
    CHECK(g->flood(0, 0, 0, 4) == 23);
    CHECK(g->get(0, 2, 1) == 9);
    CHECK(g->get(0, 5, 3) == 4);
    int deco = g->layerIndex("deco", false);
    CHECK(deco == 1);
    CHECK(g->layerIndex("overlay", true) == 2);
    g->resize(8, 4);
    CHECK(g->get(0, 7, 0) == 0);
    CHECK(g->get(0, 5, 0) == 4);
    g->writeTo(map);
    CHECK(map.width == 8);
    auto again = tiles::Grid::fromComponent(map);
    REQUIRE(again.ok());
    CHECK(again->layers.size() == 3);
    CHECK(again->get(0, 2, 1) == 9);
    CHECK(map.layers[size_t{0}].get("solid").asBool());
    // Bad data is reported with the layer name.
    map.layers = Json::parse(R"([{"name":"bad","data":"rle:3*1"}])").value();
    auto err = tiles::Grid::fromComponent(map);
    REQUIRE_FALSE(err.ok());
    CHECK(err.error().message.find("bad") != std::string::npos);
}

TEST_CASE("2d: auto-tiling (wang16, blob47, random) and terrain parsing") {
    CHECK(tiles::blobMasks().size() == 47);
    CHECK(tiles::reduceBlobMask(0xFF) == 0xFF);
    CHECK(tiles::reduceBlobMask(2) == 0);  // a lone corner does not count

    Json autotile = Json::parse(R"({"road": {"mode": "wang16", "first": 1},
                                     "wall": {"mode": "blob47", "first": 20},
                                     "grass": {"mode": "random", "tiles": "70-73"}})")
                        .value();
    auto terrains = tiles::parseTerrains(autotile, 120);
    REQUIRE(terrains.ok());
    const tiles::Terrain& road = terrains->at("road");
    CHECK(road.tiles.size() == 16);

    tiles::Grid g;
    g.width = 5;
    g.height = 5;
    g.layerIndex("ground", true);
    // A plus shape: center connects N, E, S, W.
    for (auto [x, y] : std::vector<std::pair<int, int>>{{2, 1}, {1, 2}, {2, 2}, {3, 2}, {2, 3}}) g.paintTerrain(0, road, x, y);
    CHECK(g.get(0, 2, 2) == road.tiles[15]);      // N|E|S|W
    CHECK(g.get(0, 2, 1) == road.tiles[4]);       // only S
    CHECK(g.get(0, 1, 2) == road.tiles[2]);       // only E
    CHECK(g.get(0, 0, 0) == 0);

    tiles::Grid w;
    w.width = 4;
    w.height = 4;
    w.layerIndex("walls", true);
    const tiles::Terrain& wall = terrains->at("wall");
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 2; ++x) w.paintTerrain(0, wall, x, y);
    // Top-left of a 2x2 block: E, SE, S set -> reduced mask 4|8|16 = 28.
    const auto& masks = tiles::blobMasks();
    auto idx = static_cast<size_t>(std::lower_bound(masks.begin(), masks.end(), uint8_t{28}) - masks.begin());
    CHECK(w.get(0, 1, 1) == wall.tiles[idx]);

    tiles::Grid r;
    r.width = 8;
    r.height = 8;
    r.layerIndex("g", true);
    const tiles::Terrain& grass = terrains->at("grass");
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) r.paintTerrain(0, grass, x, y, 3);
    std::set<uint32_t> seen;
    for (uint32_t c : r.layers[0].cells) seen.insert(c);
    CHECK(seen.size() >= 3);  // variants are mixed
    for (uint32_t c : seen) CHECK((c >= 70 && c <= 73));

    CHECK_FALSE(tiles::parseTerrains(Json::parse(R"({"x": {"mode": "blob46", "first": 1}})").value(), 0).ok());
    CHECK_FALSE(tiles::parseTerrains(Json::parse(R"({"x": {"mode": "wang16", "tiles": [1, 2]}})").value(), 0).ok());
    CHECK_FALSE(tiles::parseTerrains(Json::parse(R"({"x": {"mode": "blob47", "first": 100}})").value(), 120).ok());
}

TEST_CASE("2d: solid tiles merge into collision rectangles") {
    tiles::Grid g;
    g.width = 6;
    g.height = 4;
    int l = g.layerIndex("ground", true);
    g.layers[static_cast<size_t>(l)].solid = "all";
    // A 3x2 block at (1,1) and a single tile at (5,3).
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 3; ++x) g.set(0, x, y, 1);
    g.set(0, 5, 3, 2);
    auto rects = tiles::solidRects(g, {}, 2.f);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0].x == doctest::Approx(2.f));
    CHECK(rects[0].y == doctest::Approx(-6.f));  // rows 1..2 -> y from -6 to -2 (cell 2 m)
    CHECK(rects[0].w == doctest::Approx(6.f));
    CHECK(rects[0].h == doctest::Approx(4.f));
    CHECK(rects[1].x == doctest::Approx(10.f));
    CHECK(rects[1].y == doctest::Approx(-8.f));

    // Only listed tiles count in "tiles" layers.
    g.layers[0].solid = "tiles";
    auto only2 = tiles::solidRects(g, {2}, 1.f);
    REQUIRE(only2.size() == 1);
    CHECK(only2[0].x == doctest::Approx(5.f));
}

TEST_CASE("2d: ASCII maps with legends and terrains") {
    auto terrains = tiles::parseTerrains(Json::parse(R"({"wall": {"mode": "wang16", "first": 1}})").value(), 0);
    REQUIRE(terrains.ok());
    std::map<std::string, uint32_t> names{{"water", 40}};
    std::string map =
        "#####\n"
        "#..~#\n"
        "#####\n";
    auto r = tiles::parseAscii(map, Json::parse(R"({"#": "wall", "~": "water", ".": 0})").value(), terrains.value(), names);
    REQUIRE(r.ok());
    CHECK(r->width == 5);
    CHECK(r->height == 3);
    CHECK(r->cells[1 * 5 + 3] == 40);
    CHECK(r->cells[1 * 5 + 1] == 0);
    REQUIRE(r->terrainCells.size() == 1);
    CHECK(r->terrainCells[0].second.size() == 12);
    auto bad = tiles::parseAscii("#?#", Json::parse(R"({"#": 1})").value(), {}, {});
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("'?'") != std::string::npos);
    auto unknown = tiles::parseAscii("#", Json::parse(R"({"#": "wal"})").value(), terrains.value(), names);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().hint.find("wall") != std::string::npos);

    tiles::Grid g;
    g.width = 3;
    g.height = 2;
    g.layerIndex("x", true);
    g.set(0, 1, 1, 5);
    CHECK(tiles::toAscii(g, 0, {{5, '#'}}) == "...\n.#.\n");
}

TEST_CASE("2d: sprites gather into sorted instances with screen boxes") {
    Scene s;
    auto make = [&](const char* name, Vec3 pos, const char* layer, int order) {
        EntityId e = s.create(name);
        s.get<Transform>(e)->position = pos;
        Sprite& sp = s.add<Sprite>(e);
        sp.sortingLayer = layer;
        sp.order = order;
        sp.size = {1, 1};
        return e;
    };
    EntityId fg = make("Front", {0, 0, 0}, "foreground", 0);
    EntityId bg = make("Back", {0.2f, 0, 0}, "background", 5);
    EntityId a = make("A", {0.4f, 0, 0}, "default", 2);
    EntityId b = make("B", {0.6f, 0, 0}, "default", 1);
    EntityId far = make("Far", {0.8f, 0, -5}, "default", 1);  // same order as B, but farther: drawn first
    render2d::Assets2D assets(".");
    ViewCamera cam;
    cam.eye = {0, 0, 10};
    cam.target = {0, 0, 0};
    cam.orthographic = true;
    cam.orthoSize = 3;
    BuildOptions bo;
    bo.editorOverlays = false;
    FrameData f = buildFrame(s, cam, 300, 200, bo);
    render2d::gather2D(s, assets, f, {});
    REQUIRE(f.render2d.sprites.size() == 5);
    // Instances are in painter's order: background, default (far, B, A), foreground.
    std::vector<float> xs;
    for (const auto& inst : f.render2d.sprites) xs.push_back(inst.origin[0] + 0.5f);
    CHECK(xs[0] == doctest::Approx(0.2f));
    CHECK(xs[1] == doctest::Approx(0.8f));
    CHECK(xs[2] == doctest::Approx(0.6f));
    CHECK(xs[3] == doctest::Approx(0.4f));
    CHECK(xs[4] == doctest::Approx(0.f));
    // Untextured sprites batch together.
    CHECK(f.render2d.spriteBatches.size() == 1);
    // Boxes: the front sprite is on top; its box is centered and 1 unit = 200/6 pixels.
    REQUIRE(f.render2d.boxes.size() == 5);
    CHECK(f.render2d.boxes[0].entity == fg);
    CHECK(f.render2d.boxes[0].w == doctest::Approx(200.f / 6.f).epsilon(0.01));
    CHECK(f.render2d.boxes[0].x + f.render2d.boxes[0].w * 0.5f == doctest::Approx(150.f).epsilon(0.01));
    (void)bg;
    (void)a;
    (void)b;
    (void)far;
    // No 2D lights: sprites are full bright.
    CHECK_FALSE(f.render2d.lit);
    CHECK(f.render2d.ambient == Vec3{1, 1, 1});
}

TEST_CASE("2d: sprite frames from sheets, pivot, flip and pixels per unit") {
    fs::path dir = tempDir("sheet");
    REQUIRE(writePng(solid(64, 32, 255, 255, 255), (dir / "sheet.png").string()).ok());
    Scene s;
    EntityId e = s.create("Hero");
    Sprite& sp = s.add<Sprite>(e);
    sp.texture = "sheet.png";
    sp.columns = 4;
    sp.rows = 2;
    sp.frame = "5";
    sp.pixelsPerUnit = 16;
    sp.pivot = {0.5f, 0.f};
    render2d::Assets2D assets(dir.string());
    ViewCamera cam;
    cam.eye = {0, 0, 10};
    cam.orthographic = true;
    BuildOptions bo;
    FrameData f = buildFrame(s, cam, 100, 100, bo);
    render2d::gather2D(s, assets, f, {});
    REQUIRE(f.render2d.sprites.size() == 1);
    const SpriteInstance& q = f.render2d.sprites[0];
    // Frame 5 = column 1, row 1 of 16x16 cells.
    CHECK(q.uv[0] == doctest::Approx(16.f / 64.f));
    CHECK(q.uv[1] == doctest::Approx(16.f / 32.f));
    CHECK(q.uv[2] == doctest::Approx(32.f / 64.f));
    // 16 px at 16 ppu = 1 world unit; pivot at the feet: top-left corner at (-0.5, 1).
    CHECK(q.origin[0] == doctest::Approx(-0.5f));
    CHECK(q.origin[1] == doctest::Approx(1.f));
    CHECK(q.axisX[0] == doctest::Approx(1.f));
    CHECK(q.axisY[1] == doctest::Approx(-1.f));
    REQUIRE(f.render2d.spriteBatches.size() == 1);
    CHECK(f.render2d.spriteBatches[0].texture.path == (dir / "sheet.png").lexically_normal().string());
    sp.flipX = true;
    FrameData g = buildFrame(s, cam, 100, 100, bo);
    render2d::gather2D(s, assets, g, {});
    CHECK(g.render2d.sprites[0].origin[0] == doctest::Approx(0.5f));
    CHECK(g.render2d.sprites[0].axisX[0] == doctest::Approx(-1.f));
    fs::remove_all(dir);
}

TEST_CASE("2d: animation clips advance deterministically and fire events") {
    fs::path dir = tempDir("anim");
    REQUIRE(writePng(solid(128, 16, 200, 200, 200), (dir / "run.png").string()).ok());
    Scene s;
    EntityId e = s.create("Runner");
    Sprite& sp = s.add<Sprite>(e);
    sp.texture = "run.png";
    sp.columns = 8;
    SpriteAnimator& an = s.add<SpriteAnimator>(e);
    an.clips = Json::parse(R"({"run": {"frames": "0-3", "fps": 10, "loop": true, "events": {"2": "step"}},
                               "die": {"frames": [4, 5, 6], "fps": 10, "loop": false}})")
                   .value();
    an.clip = "run";
    render2d::Assets2D assets(dir.string());
    std::vector<std::string> events;
    auto emit = [&](EntityId, const std::string& ev) { events.push_back(ev); };
    std::string tex;
    int cols = 0, rows = 0, frame = -1;
    for (int i = 0; i < 6; ++i) render2d::tickAnimators(s, assets, 1.f / 60.f, emit);  // 0.1 s
    REQUIRE(render2d::animatedFrame(s, assets, e, tex, cols, rows, frame));
    CHECK(frame == 1);
    for (int i = 0; i < 12; ++i) render2d::tickAnimators(s, assets, 1.f / 60.f, emit);  // 0.3 s
    CHECK(events == std::vector<std::string>{"step"});
    for (int i = 0; i < 24; ++i) render2d::tickAnimators(s, assets, 1.f / 60.f, emit);  // 0.7 s: looped once more
    CHECK(events.size() == 2);
    REQUIRE(render2d::playAnimation(s, assets, e, "die", false).ok());
    for (int i = 0; i < 60; ++i) render2d::tickAnimators(s, assets, 1.f / 60.f, emit);
    REQUIRE(render2d::animatedFrame(s, assets, e, tex, cols, rows, frame));
    CHECK(frame == 6);  // held on the last frame
    CHECK(events.back() == "finished");
    auto bad = render2d::playAnimation(s, assets, e, "jum", false);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "unknown_clip");
    fs::remove_all(dir);
}

TEST_CASE("2d: tilemaps cull to the view and lights gather") {
    Scene s;
    EntityId m = s.create("Map");
    Tilemap& map = s.add<Tilemap>(m);
    map.width = 200;
    map.height = 100;
    map.layers = Json::array({Json::object({{"name", "ground"}, {"data", "rle:20000*3"}})});
    EntityId lamp = s.create("Lamp");
    Light2D& l = s.add<Light2D>(lamp);
    l.kind = "point";
    l.halo = 1;
    EntityId sky = s.create("Ambient");
    s.add<Light2D>(sky).kind = "global";
    render2d::Assets2D assets(".");
    ViewCamera cam;
    cam.eye = {10, -10, 10};
    cam.target = {10, -10, 0};
    cam.orthographic = true;
    cam.orthoSize = 4;
    BuildOptions bo;
    FrameData f = buildFrame(s, cam, 160, 90, bo);
    render2d::gather2D(s, assets, f, {});
    // ~14 x 8 cells visible (+ margins), never the whole 20000-cell map.
    size_t tiles = f.render2d.sprites.size() - 1;  // minus the halo
    CHECK(tiles > 100);
    CHECK(tiles < 400);
    CHECK(f.render2d.lit);
    REQUIRE(f.render2d.lights.size() == 1);
    CHECK(f.render2d.ambient.x > 0.5f);
    // The halo is the last, additive batch.
    CHECK(f.render2d.spriteBatches.back().additive);
}

TEST_CASE("2d: linear-filtered tiles stay half a texel inside their cell of the sheet") {
    fs::path dir = tempDir("tileinset");
    REQUIRE(writePng(solid(64, 16, 90, 90, 90), (dir / "tiles.png").string()).ok());  // 4 tiles of 16 px
    Scene s;
    EntityId m = s.create("Map");
    Tilemap& map = s.add<Tilemap>(m);
    map.tileset = "tiles.png";
    map.width = 1;
    map.height = 1;
    map.layers = Json::array({Json::object({{"name", "ground"}, {"data", "rle:1*2"}})});
    render2d::Assets2D assets(dir.string());
    ViewCamera cam;
    cam.eye = {0.5f, -0.5f, 10};
    cam.target = {0.5f, -0.5f, 0};
    cam.orthographic = true;
    cam.orthoSize = 2;
    BuildOptions bo;
    for (const char* filter : {"linear", "nearest"}) {
        map.filter = filter;
        FrameData f = buildFrame(s, cam, 64, 64, bo);
        render2d::gather2D(s, assets, f, {});
        REQUIRE(f.render2d.sprites.size() == 1);
        const SpriteInstance& q = f.render2d.sprites[0];
        float inset = std::string(filter) == "linear" ? 0.5f : 0.02f;
        CHECK(q.uv[0] == doctest::Approx((16.f + inset) / 64.f));   // tile 2 starts at x = 16
        CHECK(q.uv[2] == doctest::Approx((32.f - inset) / 64.f));
        CHECK(q.uv[1] == doctest::Approx(inset / 16.f));
    }
    fs::remove_all(dir);
}
