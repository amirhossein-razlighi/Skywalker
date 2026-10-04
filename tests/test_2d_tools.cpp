// 2D agent tools (tilemap_from_ascii / paint / inspect, sprite_sheet_slice, sprite_atlas_pack) and
// the Wander 2D builtins (play_anim, tile_at, set_tile).

#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>

#include "skywalker/engine/Engine.h"
#include "skywalker/render/Image.h"
#include "skywalker/render2d/Tilemap.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

Image filled(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    Image img(w, h);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = 255;
    }
    return img;
}

}  // namespace

TEST_CASE("2d tools: tilemaps from ASCII, painting, inspection, sheets, atlases and Wander builtins") {
    fs::path dir = fs::temp_directory_path() / ("sky_2d_tools_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir / "props");
    REQUIRE(writePng(filled(64, 32, 200, 50, 50), (dir / "knight.png").string()).ok());  // 4x2 cells of 16 px
    REQUIRE(writePng(filled(10, 12, 0, 255, 0), (dir / "props" / "coin_1.png").string()).ok());
    REQUIRE(writePng(filled(8, 8, 0, 0, 255), (dir / "props" / "coin_2.png").string()).ok());
    REQUIRE(writePng(filled(64, 16, 90, 90, 90), (dir / "tiles.png").string()).ok());  // 4 tiles of 16 px
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = dir.string();
    {
        Engine e(cfg);
        (void)e.newScene("Tools", false);
        auto call = [&](const char* tool, const char* args, bool ok = true) {
            ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
            INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
            CHECK(r.isError == !ok);
            return r.structured;
        };
        Json level = call("tilemap_from_ascii", R"({"name": "Level", "tileset": "tiles.png", "tile_size": 16,
            "autotile": {"rock": {"mode": "random", "tiles": [3, 4]}}, "legend": {"#": 1, "R": "rock", "=": 2},
            "solid": true, "map": "......\n..==..\n######\nRRRRRR"})");
        CHECK(level.get("width").asInt() == 6);
        CHECK(level.get("height").asInt() == 4);
        CHECK(level.get("solidRects").asInt() == 2);  // the platform and the merged two-row floor
        EntityId map = e.scene().find("Level");
        REQUIRE(map);
        call("tilemap_paint", R"({"entity": "Level", "action": "fill", "rect": [0, 0, 2, 1], "tile": 2})");
        call("tilemap_paint", R"({"entity": "Level", "action": "set", "cells": [[5, 0]], "tile": "rock"})");
        call("tilemap_paint", R"({"entity": "Level", "action": "set", "cells": [[0, 0]], "tile": 99})", false);  // beyond the tileset
        Json inspect = call("tilemap_inspect", R"({"entity": "Level"})");
        std::string ascii = inspect.get("layers")[size_t{0}].get("ascii").asString();
        CHECK(ascii.substr(0, 2) == "aa");
        CHECK(ascii[5] != '.');  // the painted rock
        CHECK(inspect.get("solidRectCount").asInt() >= 2);
        call("history", R"({"action": "undo"})");  // the set
        call("history", R"({"action": "undo"})");  // the fill

        // Sheets: slicing names frames, rows become clips, the entity gets sprite + animator.
        call("sprite_sheet_slice", R"({"image": "knight.png", "cell": [16, 16], "entity": "Knight"})", false);  // no entity yet
        call("entity_create", R"({"name": "Knight"})");
        Json sliced = call("sprite_sheet_slice", R"({"image": "knight.png", "cell": [16, 16], "entity": "Knight",
            "animations": {"idle": {"row": 0, "fps": 4}, "run": {"row": 1, "fps": 8}}})");
        CHECK(sliced.get("frames").asInt() == 8);
        CHECK(fs::exists(dir / "knight.atlas.json"));
        EntityId knight = e.scene().find("Knight");
        const SpriteAnimator* anim = e.scene().get<SpriteAnimator>(knight);
        REQUIRE(anim);
        CHECK(anim->clips.get("run").get("frames").size() == 4);
        CHECK(e.scene().get<Sprite>(knight)->texture == "knight.atlas.json");
        // Atlases from a folder, with clips suggested from numbered names.
        Json packed = call("sprite_atlas_pack", R"({"folder": "props", "output": "props.atlas.json"})");
        CHECK(packed.get("frames").size() == 2);
        CHECK(packed.get("suggestedClips").get("coin").get("frames").asString() == "coin_*");
        CHECK(fs::exists(dir / "props.atlas.png"));

        // Wander builtins.
        REQUIRE(e.scene()
                    .setBehaviors(knight, Json::parse(R"([{"name": "K", "source": "behavior K\n  var below = -1\n  on start\n    play_anim(self, \"run\")\n    below = tile_at(find(\"Level\"), (0.5, -2.5, 0))\n    set_tile(find(\"Level\"), (1.5, -0.5, 0), 4)\n  end\nend"}])")
                                              .value())
                    .ok());
        e.step(2);
        CHECK(e.scene().record(knight)->vars.get("below").asInt() == 1);
        CHECK(e.scene().get<SpriteAnimator>(knight)->clip == "run");
        auto g = tiles::Grid::fromComponent(*e.scene().get<Tilemap>(map));
        REQUIRE(g.ok());
        CHECK(g->get(0, 1, 0) == 4);
        CHECK(e.recentMessages().empty());  // no runtime errors
        e.stop();
    }
    fs::remove_all(dir);
}
