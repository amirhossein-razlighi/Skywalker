// Shipping: game.json settings, asset-reference collection, the app bundle layout, `skywalker-player --check`,
// the platform builtins (cursor_lock, quit_game) and the game_* tools. No display is needed.

#include <doctest/doctest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/Packager.h"
#include "skywalker/native/NativeModules.h"
#include "skywalker/render/Image.h"
#include "skywalker/wander/Aot.h"

using namespace sky;
using namespace sky::game;
namespace fs = std::filesystem;

#ifndef SKY_PLAYER_PATH
#define SKY_PLAYER_PATH ""
#endif

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("skywalker-game-" + tag + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

/// Runs a command line, returns its exit status and output.
int shell(const std::string& command, std::string& output, bool mergeStderr = false) {
    FILE* pipe = ::popen((command + (mergeStderr ? " 2>&1" : " 2>/dev/null")).c_str(), "r");
    if (!pipe) return -1;
    char buf[4096];
    size_t n;
    output.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) output.append(buf, n);
    int status = ::pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// A tiny but valid 16-bit mono WAV (the player's check really plays sounds).
std::string wavBytes(int samples = 200) {
    std::string d;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) d += static_cast<char>((v >> (8 * i)) & 0xff);
    };
    auto u16 = [&](uint16_t v) {
        d += static_cast<char>(v & 0xff);
        d += static_cast<char>(v >> 8);
    };
    d += "RIFF";
    u32(static_cast<uint32_t>(36 + samples * 2));
    d += "WAVEfmt ";
    u32(16);
    u16(1);
    u16(1);
    u32(22050);
    u32(44100);
    u16(2);
    u16(16);
    d += "data";
    u32(static_cast<uint32_t>(samples * 2));
    for (int i = 0; i < samples; ++i) u16(static_cast<uint16_t>((i % 20) * 800));
    return d;
}

std::string pngBytes() {
    Image img(4, 4);
    for (auto& b : img.pixels) b = 200;
    auto bytes = encodePng(img);
    return std::string(bytes.begin(), bytes.end());
}

std::string q(const std::string& s) { return "'" + s + "'"; }

bool playerAvailable() { return fs::exists(SKY_PLAYER_PATH); }

/// A small project that exercises every kind of reference: a material with textures (relative paths), a prefab, sound
/// effects named in a script, a guid reference, a glTF with a side file and an OBJ with an MTL.
void makeProject(const fs::path& dir) {
    write(dir / "game.json", R"({"id":"orbit_test","title":"Orbit Test","version":"2.1.0","startScene":"scenes/main.sky.json"})");
    write(dir / "textures/wood.png", pngBytes());
    write(dir / "textures/wood_n.png", pngBytes());
    write(dir / "textures/unused.png", pngBytes());
    write(dir / "materials/wood.mat.json", R"({"format":"skywalker.material","texture":"textures/wood.png","normalMap":"textures/wood_n.png"})");
    write(dir / "materials/unused.mat.json", R"({"format":"skywalker.material","color":"#ff0000"})");
    write(dir / "audio/hit.wav", wavBytes());
    write(dir / "audio/unused.wav", wavBytes(300));
    write(dir / "audio/by_guid.wav", wavBytes(400));
    write(dir / "audio/by_guid.wav.meta", R"({"guid":"0123456789abcdef0123456789abcdef","type":"audio"})");
    write(dir / "models/ship.gltf", R"({"asset":{"version":"2.0"},"buffers":[{"uri":"ship.bin","byteLength":4}],"images":[{"uri":"ship_tex.png"}]})");
    write(dir / "models/ship.bin", "bin!");
    write(dir / "models/ship_tex.png", pngBytes());
    write(dir / "models/rock.obj", "mtllib rock.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    write(dir / "models/ship.obj", "mtllib ship.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    write(dir / "models/ship.mtl", "newmtl s\nmap_Kd ship_obj_tex.png\n");
    write(dir / "models/ship_obj_tex.png", pngBytes());
    write(dir / "models/rock.mtl", "newmtl m\nmap_Kd rock_tex.png\n");
    write(dir / "models/rock_tex.png", pngBytes());
    write(dir / "prefabs/coin.prefab.json", R"({"format":"skywalker.prefab","root":{"name":"Coin","components":{"mesh":{"mesh":"asset:models/rock.obj"}}}})");
    write(dir / "scripts/util.wander", "fn double(x)\n  return x * 2\nend\n");
    write(dir / "input.json", R"({"version":1,"actions":{}})");
    write(dir / "CREDITS.md", "credits");
    // Never shipped:
    write(dir / "studio/board.json", "{}");
    write(dir / "agents/nimbus.agent.json", "{}");
    write(dir / ".skywalker/cache/junk.bin", "junk");
    write(dir / "art/ship.blend", "blend");
    write(dir / "Old.app/Contents/Resources/Game/game.json", "{}");

    TempDir scratch("scene");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Off;
    cfg.projectDir = dir.string();
    Engine engine(cfg);
    (void)engine.newScene("Orbit", false);
    Scene& s = engine.scene();
    EntityId ship = s.create("Ship");
    (void)s.patchComponent(ship, "mesh", Json::object({{"mesh", "asset:models/ship.obj"}, {"material", "materials/wood.mat.json"}}));
    EntityId far = s.create("Far");  // a glTF with side files (referenced only; the fixture is not a real model)
    (void)s.patchComponent(far, "mesh", Json::object({{"mesh", "asset:models/ship.gltf"}}));
    EntityId cam = s.create("Camera");
    (void)s.patchComponent(cam, "camera", Json::object());
    ToolResult b = engine.callTool(
        "behavior_set",
        Json::object({{"entity", "Ship"},
                      {"name", "Pew"},
                      {"source", "use \"scripts/util.wander\"\nbehavior Pew\n  on start\n    play_sound(\"audio/hit.wav\", 1)\n"
                                 "    spawn(\"prefab:prefabs/coin.prefab.json\")\n    let a = \"guid:0123456789abcdef0123456789abcdef\"\n"
                                 "    let b = \"audio/missing.wav\"\n  end\nend\n"}}),
        "test");
    REQUIRE_FALSE(b.isError);
    REQUIRE(engine.saveScene("scenes/main.sky.json"));
}

}  // namespace

TEST_CASE("game.json: defaults, parsing, validation") {
    TempDir dir("settings");
    auto none = GameSettings::load(dir.str());
    REQUIRE(none);
    CHECK_FALSE(none->fromFile);
    CHECK(none->window.width == 1280);
    CHECK(none->quality == "high");
    CHECK(none->effectiveBundleId() == "dev.skywalker.games.game");

    // The description-only files of the examples keep working.
    write(dir.path / "game.json", R"j({"id":"sky_dash","title":"Sky Dash","genre":"2D Platformer","mood":"bright","pitch":"Run.","assets":"Poly Haven (CC0)"})j");
    auto basic = GameSettings::load(dir.str());
    REQUIRE(basic);
    CHECK(basic->fromFile);
    CHECK(basic->displayName() == "Sky Dash");
    CHECK(basic->effectiveBundleId() == "dev.skywalker.games.sky-dash");

    auto full = GameSettings::fromJson(*Json::parse(R"({
        "title":"Orbit","startScene":"scenes/a.sky.json","quality":"Medium","renderScale":0.67,
        "window":{"width":1920,"height":1080,"fullscreen":true,"resizable":false,"vsync":false},
        "quitOnEscape":true,"pauseOnFocusLoss":false,"icon":"art/icon.png","bundleId":"com.acme.orbit",
        "version":"1.2.3","include":["data/**"],"exclude":["audio/draft*"]})"));
    REQUIRE(full);
    CHECK(full->window.width == 1920);
    CHECK(full->window.fullscreen);
    CHECK_FALSE(full->window.resizable);
    CHECK_FALSE(full->window.vsync);
    CHECK(full->quality == "medium");
    CHECK(full->renderScale == doctest::Approx(0.67f));
    CHECK(full->quitOnEscape);
    CHECK_FALSE(full->pauseOnFocusLoss);
    CHECK(full->effectiveBundleId() == "com.acme.orbit");
    CHECK(full->include.size() == 1);
    // Round trip.
    auto again = GameSettings::fromJson(full->toJson());
    REQUIRE(again);
    CHECK(again->toJson() == full->toJson());

    auto typo = GameSettings::fromJson(*Json::parse(R"({"startScne":"x"})"));
    REQUIRE_FALSE(typo);
    CHECK(typo.error().hint.find("startScene") != std::string::npos);
    auto badWindow = GameSettings::fromJson(*Json::parse(R"({"window":{"widht":10}})"));
    REQUIRE_FALSE(badWindow);
    CHECK(badWindow.error().hint.find("width") != std::string::npos);
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"quality":"hgih"})")));
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"version":"1.x"})")));
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"bundleId":"nodots"})")));
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"startScene":"../outside.sky.json"})")));
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"renderScale":4})")));
    CHECK_FALSE(GameSettings::fromJson(*Json::parse(R"({"window":{"width":"wide"}})")));
    write(dir.path / "game.json", "{ not json");
    CHECK_FALSE(GameSettings::load(dir.str()));

    // Quality presets adjust the environment; high keeps what the scene authored.
    Environment env;
    GameSettings low;
    low.quality = "low";
    low.applyQuality(env);
    CHECK(env.renderScale == doctest::Approx(0.67f));
    CHECK(env.gi == 0.f);
    Environment hi;
    GameSettings high;
    high.applyQuality(hi);
    CHECK(hi.gi == Environment{}.gi);
    GameSettings scaled;
    scaled.renderScale = 0.5f;
    scaled.applyQuality(hi);
    CHECK(hi.renderScale == doctest::Approx(0.5f));
}

TEST_CASE("every example's game.json is valid") {
    fs::path examples = fs::path(SKY_SOURCE_DIR) / "examples";
    int checked = 0;
    for (const auto& e : fs::directory_iterator(examples)) {
        if (!fs::exists(e.path() / "game.json")) continue;
        auto s = GameSettings::load(e.path().string());
        INFO(e.path().string() << ": " << (s ? "" : s.error().message));
        CHECK(s.ok());
        ++checked;
    }
    CHECK(checked >= 5);
}

TEST_CASE("start scene resolution") {
    TempDir dir("scene");
    GameSettings s;
    CHECK_FALSE(resolveStartScene(dir.str(), s));  // no scene at all
    write(dir.path / "levels/b.sky.json", "{}");
    write(dir.path / "levels/a.sky.json", "{}");
    CHECK(*resolveStartScene(dir.str(), s) == "levels/a.sky.json");  // first, sorted
    write(dir.path / "scenes/main.sky.json", "{}");
    CHECK(*resolveStartScene(dir.str(), s) == "scenes/main.sky.json");
    s.startScene = "levels/b.sky.json";
    CHECK(*resolveStartScene(dir.str(), s) == "levels/b.sky.json");
    CHECK(*resolveStartScene(dir.str(), s, "levels/a.sky.json") == "levels/a.sky.json");  // override wins
    s.startScene = "levels/c.sky.json";
    auto missing = resolveStartScene(dir.str(), s);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().hint.find("levels/") != std::string::npos);
}

TEST_CASE("asset reference collection") {
    TempDir dir("collect");
    makeProject(dir.path);
    auto settings = GameSettings::load(dir.str());
    REQUIRE(settings);

    auto c = collectGameFiles(dir.str(), *settings);
    REQUIRE(c);
    const auto& f = c->files;
    auto has = [&](const char* p) { return f.count(p) > 0; };
    CHECK(c->startScene == "scenes/main.sky.json");
    // Roots and everything they reference, through every kind of link.
    CHECK(has("game.json"));
    CHECK(has("input.json"));
    CHECK(has("CREDITS.md"));
    CHECK(has("scenes/main.sky.json"));
    CHECK(has("materials/wood.mat.json"));    // scene -> material
    CHECK(has("textures/wood.png"));          // material -> texture
    CHECK(has("textures/wood_n.png"));
    CHECK(has("audio/hit.wav"));              // Wander literal in a behavior
    CHECK(has("prefabs/coin.prefab.json"));   // spawn(...)
    CHECK(has("models/rock.obj"));            // prefab -> "asset:" mesh
    CHECK(has("models/rock.mtl"));            // obj -> mtllib
    CHECK(has("models/rock_tex.png"));        // mtl -> map_Kd
    CHECK(has("models/ship.obj"));
    CHECK(has("models/ship.mtl"));
    CHECK(has("models/ship_obj_tex.png"));
    CHECK(has("models/ship.gltf"));
    CHECK(has("models/ship.bin"));            // glTF side files, relative to the model
    CHECK(has("models/ship_tex.png"));
    CHECK(has("audio/by_guid.wav"));          // "guid:..." through the .meta sidecar
    CHECK(has("audio/by_guid.wav.meta"));
    CHECK(has("scripts/util.wander"));
    // Unreferenced and editor-only files stay out.
    CHECK_FALSE(has("textures/unused.png"));
    CHECK_FALSE(has("materials/unused.mat.json"));
    CHECK_FALSE(has("audio/unused.wav"));
    CHECK_FALSE(has("studio/board.json"));
    CHECK_FALSE(has("agents/nimbus.agent.json"));
    CHECK_FALSE(has(".skywalker/cache/junk.bin"));
    CHECK_FALSE(has("art/ship.blend"));
    CHECK_FALSE(has("Old.app/Contents/Resources/Game/game.json"));
    // A reference to a file that does not exist is reported, not fatal.
    REQUIRE(c->missing.count("audio/missing.wav") == 1);
    CHECK(c->missing.at("audio/missing.wav") == "scenes/main.sky.json");
    CHECK(c->referenced > 5);

    // all_assets ships the unreferenced ones too (but never studio/native/hidden folders).
    CollectOptions all;
    all.allAssets = true;
    auto everything = collectGameFiles(dir.str(), *settings, all);
    REQUIRE(everything);
    CHECK(everything->files.count("audio/unused.wav") == 1);
    CHECK(everything->files.count("textures/unused.png") == 1);
    CHECK(everything->files.count("studio/board.json") == 0);
    CHECK(everything->files.count("art/ship.blend") == 0);

    // include globs add files; exclude globs remove them even when referenced (and say so).
    settings->include = {"audio/unused.*", "textures"};
    settings->exclude = {"models/ship_tex.png", "audio/hit*"};
    auto tuned = collectGameFiles(dir.str(), *settings);
    REQUIRE(tuned);
    CHECK(tuned->files.count("audio/unused.wav") == 1);
    CHECK(tuned->files.count("textures/unused.png") == 1);  // a folder name includes everything below it
    CHECK(tuned->files.count("models/ship_tex.png") == 0);
    CHECK(tuned->files.count("audio/hit.wav") == 0);
    CHECK(tuned->excludedReferenced.size() == 2);
}

TEST_CASE("platform builtins: cursor_lock and quit_game") {
    TempDir dir("builtins");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Off;
    cfg.projectDir = dir.str();
    Engine engine(cfg);
    (void)engine.newScene("T", false);
    EntityId e = engine.scene().create("Player");
    ToolResult r = engine.callTool("behavior_set",
                                   Json::object({{"entity", "Player"}, {"name", "Fps"},
                                                 {"source", "behavior Fps\n  on start\n    cursor_lock(true)\n  end\n"
                                                            "  on tick\n    if time > 0.5 then quit_game() end\n  end\nend\n"}}),
                                   "test");
    REQUIRE_FALSE(r.isError);
    (void)e;
    CHECK_FALSE(engine.cursorLocked());
    engine.step(1);
    CHECK(engine.cursorLocked());
    CHECK_FALSE(engine.quitRequested());
    engine.step(40);
    CHECK(engine.quitRequested());
    engine.stop();
    CHECK_FALSE(engine.cursorLocked());  // a stopped game gives the mouse back
    // Documented like every builtin.
    Json ref = engine.callTool("wander_reference", Json::object({{"topic", "cursor_lock"}}), "test").structured;
    CHECK(ref["entries"].size() == 1);
}

TEST_CASE("platform input feed") {
    input::InputState in;
    in.keyEvent("W", true);
    in.keyEvent("w", true);  // repeat while held: one press
    CHECK(in.held.count("w") == 1);
    CHECK(in.pressed.size() == 1);
    in.keyEvent("Esc", true);
    CHECK(in.held.count("escape") == 1);
    in.keyEvent("w", false);
    CHECK(in.released.count("w") == 1);
    in.mouseMove(0.25f, 0.75f, 3.f, 4.f);
    CHECK(in.mouseX == 0.25f);
    CHECK(in.mouseDY == -4.f);  // screen +y is down, input +y is up
    in.mouseButton(0, true);
    in.mouseButton(7, true);  // ignored
    CHECK(in.mouseHeld.count("left") == 1);
    in.gamepad(0, true, "Pad", 2.f, -0.5f, 0, 0, 0.5f, 0, 1u << 3);
    CHECK(in.pads[0].connected);
    CHECK(in.pads[0].lx == 1.f);  // clamped
    CHECK(in.pads[0].button(input::PadButton::North));
    in.gamepad(0, false, "", 0, 0, 0, 0, 0, 0, 0);
    CHECK_FALSE(in.pads[0].connected);
    CHECK(in.pads[0].buttons == 0);
}

TEST_CASE("building an app bundle") {
    if (!playerAvailable()) {
        MESSAGE("skywalker-player was not built: skipping");
        return;
    }
    TempDir project("bundle-project"), out("bundle-out"), art("bundle-art");
    makeProject(project.path);
    // An icon: a 600x600 gradient PNG.
    Image icon(600, 600);
    for (int y = 0; y < 600; ++y) {
        for (int x = 0; x < 600; ++x) {
            uint8_t* p = icon.at(x, y);
            p[0] = static_cast<uint8_t>(x * 255 / 599);
            p[1] = static_cast<uint8_t>(y * 255 / 599);
            p[2] = 128;
            p[3] = 255;
        }
    }
    REQUIRE(writePng(icon, (art.path / "icon.png").string()));

    PackageOptions o;
    o.projectDir = project.str();
    o.outDir = out.str();
    o.player = SKY_PLAYER_PATH;
    o.icon = (art.path / "icon.png").string();
    o.release = true;
    auto report = buildGame(o);
    INFO((report ? report->toJson().dump(2) : report.error().message));
    REQUIRE(report);
    const fs::path app = out.path / "Orbit Test.app";
    CHECK(report->app == app.string());
    CHECK(report->name == "Orbit Test");
    CHECK(report->version == "2.1.0");
    CHECK(report->bundleId == "dev.skywalker.games.orbit-test");
    CHECK(report->signature == "ad-hoc");
    CHECK(report->appBytes > 1000000);  // the player is in there

    // Layout.
    CHECK(fs::exists(app / "Contents/Info.plist"));
    CHECK(fs::exists(app / "Contents/MacOS/Orbit_Test"));
    CHECK(::access((app / "Contents/MacOS/Orbit_Test").c_str(), X_OK) == 0);
    CHECK(fs::exists(app / "Contents/Resources/AppIcon.icns"));
    CHECK(fs::exists(app / "Contents/Resources/build.json"));
    CHECK(fs::exists(app / "Contents/_CodeSignature/CodeResources"));
    const fs::path game = app / "Contents/Resources/Game";
    CHECK(fs::exists(game / "scenes/main.sky.json"));
    CHECK(fs::exists(game / "textures/wood.png"));
    CHECK(fs::exists(game / "audio/hit.wav"));
    CHECK(fs::exists(game / "models/ship.bin"));
    CHECK_FALSE(fs::exists(game / "audio/unused.wav"));
    CHECK_FALSE(fs::exists(game / "studio"));
    CHECK_FALSE(fs::exists(game / "native"));
    CHECK_FALSE(fs::exists(game / ".skywalker"));
    CHECK_FALSE(fs::exists(app / "Contents/Frameworks"));  // no native module in this project

    // Info.plist is valid and carries the identity.
    std::string plist = slurp(app / "Contents/Info.plist");
    CHECK(plist.find("<string>dev.skywalker.games.orbit-test</string>") != std::string::npos);
    CHECK(plist.find("<string>2.1.0</string>") != std::string::npos);
    CHECK(plist.find("<key>CFBundleIconFile</key>") != std::string::npos);
    CHECK(plist.find("<string>Orbit_Test</string>") != std::string::npos);
    std::string text;
    CHECK(shell("plutil -lint " + q((app / "Contents/Info.plist").string()), text) == 0);
    CHECK(shell("codesign --verify --strict " + q(app.string()), text) == 0);

    // The game.json inside is resolved (start scene filled in, build-only fields gone).
    auto shipped = GameSettings::load(game.string());
    REQUIRE(shipped);
    CHECK(shipped->startScene == "scenes/main.sky.json");
    CHECK(shipped->include.empty());

    // locateGame / verifyBundle agree with the files on disk, and notice tampering.
    auto loc = locateGame(app.string());
    REQUIRE(loc);
    CHECK(loc->bundled);
    CHECK(loc->projectDir == game.string());
    BundleCheck check = verifyBundle(*loc);
    CHECK(check.ok);
    CHECK(check.files == report->files);
    write(game / "audio/hit.wav", "tampered");
    CHECK_FALSE(verifyBundle(*loc).ok);
    write(game / "audio/hit.wav", wavBytes());
    CHECK(verifyBundle(*loc).ok);
    fs::remove(game / "textures/wood.png");
    BundleCheck broken = verifyBundle(*loc);
    CHECK_FALSE(broken.ok);
    CHECK(broken.problems.front().find("textures/wood.png") != std::string::npos);
    write(game / "textures/wood.png", pngBytes());

    // A rebuild replaces the app atomically and leaves no staging folder behind.
    o.release = false;
    auto second = buildGame(o);
    REQUIRE(second);
    CHECK(fs::exists(app / "Contents/Resources/Game/scenes/main.sky.json"));
    for (const auto& entry : fs::directory_iterator(out.path)) CHECK(entry.path().filename().string()[0] != '.');
    // Refuses to overwrite something that is not an app.
    write(out.path / "Other.app/readme.txt", "mine");
    o.name = "Other";
    CHECK_FALSE(buildGame(o));
    CHECK(fs::exists(out.path / "Other.app/readme.txt"));
}

TEST_CASE("the player validates a built app headlessly") {
    if (!playerAvailable()) {
        MESSAGE("skywalker-player was not built: skipping");
        return;
    }
    TempDir project("check-project"), out("check-out");
    makeProject(project.path);
    PackageOptions o;
    o.projectDir = project.str();
    o.outDir = out.str();
    o.player = SKY_PLAYER_PATH;
    REQUIRE(buildGame(o));
    const fs::path app = out.path / "Orbit Test.app";

    // Running the executable inside the bundle needs no arguments, and --check needs no window.
    std::string text;
    int rc = shell(q((app / "Contents/MacOS/Orbit_Test").string()) + " --check 30", text);
    INFO(text);
    CHECK(rc == 0);
    auto report = Json::parse(text);
    REQUIRE(report);
    CHECK(report->get("ok").asBool());
    CHECK(report->get("bundled").asBool());
    CHECK(report->get("name").asString() == "Orbit Test");
    CHECK(report->get("start_scene").asString() == "scenes/main.sky.json");
    CHECK(report->get("ticks").asInt() == 30);
    CHECK(report->get("bundle").get("ok").asBool());
    CHECK(report->get("entities").asInt() == 3);

    // The same through the app path from outside, and for a plain project folder.
    CHECK(shell(q(SKY_PLAYER_PATH) + " " + q(app.string()) + " --check 5", text) == 0);
    CHECK(shell(q(SKY_PLAYER_PATH) + " " + q(project.str()) + " --check 5", text) == 0);

    // A damaged bundle fails with a readable reason.
    fs::remove(app / "Contents/Resources/Game/audio/hit.wav");
    rc = shell(q((app / "Contents/MacOS/Orbit_Test").string()) + " --check 5", text);
    CHECK(rc == 1);
    CHECK(text.find("audio/hit.wav") != std::string::npos);

    // Argument errors and unknown scenes.
    CHECK(shell(q(SKY_PLAYER_PATH) + " --frobnicate", text, true) == 2);
    CHECK(text.find("unknown option") != std::string::npos);
    CHECK(shell(q(SKY_PLAYER_PATH) + " " + q(project.str()) + " --scene scenes/nope.sky.json --check", text) == 1);
    CHECK(text.find("scenes/nope.sky.json") != std::string::npos);
    CHECK(shell(q(SKY_PLAYER_PATH) + " " + q((out.path / "missing").string()) + " --check", text) == 1);
}

TEST_CASE("script errors fail the player check") {
    if (!playerAvailable()) return;
    TempDir project("check-bad");
    write(project.path / "game.json", R"({"title":"Bad"})");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Off;
    cfg.projectDir = project.str();
    {
        Engine engine(cfg);
        (void)engine.newScene("Bad", false);
        engine.scene().create("Thing");
        // Plant a behavior that fails at run time (the tool would refuse invalid code).
        ToolResult r = engine.callTool("behavior_set",
                                       Json::object({{"entity", "Thing"}, {"name", "Boom"},
                                                     {"source", "behavior Boom\n  on tick\n    let l = [1]\n    log l[4]\n  end\nend\n"}}),
                                       "test");
        REQUIRE_FALSE(r.isError);
        REQUIRE(engine.saveScene("scenes/main.sky.json"));
    }
    std::string text;
    CHECK(shell(q(SKY_PLAYER_PATH) + " " + q(project.str()) + " --check 10", text) == 1);
    auto report = Json::parse(text);
    REQUIRE(report);
    CHECK_FALSE(report->get("ok").asBool());
    CHECK(report->get("problems").size() >= 1);
}

TEST_CASE("native modules ship precompiled") {
    if (!playerAvailable() || wander::findCxxCompiler().empty()) {
        MESSAGE("no player or no C++ compiler: skipping");
        return;
    }
    TempDir project("native-project"), out("native-out");
    write(project.path / "game.json", R"({"title":"Native Test"})");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Off;
    cfg.projectDir = project.str();
    {
        Engine engine(cfg);
        (void)engine.newScene("N", false);
        engine.scene().create("Thing");
        REQUIRE_FALSE(engine.callTool("native_template", Json::object({{"name", "gameplay"}}), "test").isError);
        // `wave` only exists when the module is loaded: the script fails to compile without it.
        ToolResult r = engine.callTool("behavior_set",
                                       Json::object({{"entity", "Thing"}, {"name", "Uses"},
                                                     {"source", "behavior Uses\n  on tick\n    log wave(time, 2)\n  end\nend\n"}}),
                                       "test");
        INFO(r.structured.dump(2));
        // The tool validates against the loaded builtins, so load them first.
        ToolResult built = engine.callTool("native_build", Json::object(), "test");
        REQUIRE_FALSE(built.isError);
        r = engine.callTool("behavior_set",
                            Json::object({{"entity", "Thing"}, {"name", "Uses"},
                                          {"source", "behavior Uses\n  on tick\n    log wave(time, 2)\n  end\nend\n"}}),
                            "test");
        REQUIRE_FALSE(r.isError);
        REQUIRE(engine.saveScene("scenes/main.sky.json"));
    }
    PackageOptions o;
    o.projectDir = project.str();
    o.outDir = out.str();
    o.player = SKY_PLAYER_PATH;
    auto report = buildGame(o);
    INFO((report ? report->toJson().dump(2) : report.error().message));
    REQUIRE(report);
    const fs::path app = out.path / "Native Test.app";
    auto manifest = Json::parse(slurp(app / "Contents/Resources/build.json"));
    REQUIRE(manifest);
    const std::string lib = manifest->get("native").asString();
    REQUIRE_FALSE(lib.empty());
    CHECK(fs::exists(app / "Contents" / lib));
    CHECK_FALSE(fs::exists(app / "Contents/Resources/Game/native"));
    std::string text;
    CHECK(shell("codesign --verify --strict " + q(app.string()), text) == 0);
    // The shipped app loads the prebuilt library: the behavior using `wave` runs without any compiler.
    int rc = shell(q((app / "Contents/MacOS/Native_Test").string()) + " --check 20", text);
    INFO(text);
    CHECK(rc == 0);
    auto check = Json::parse(text);
    REQUIRE(check);
    CHECK(check->get("ok").asBool());
    CHECK(check->get("logs").size() > 0);
}

TEST_CASE("game tools") {
    TempDir project("tools");
    makeProject(project.path);
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Off;
    cfg.projectDir = project.str();
    Engine engine(cfg);

    const ToolDef* build = engine.tools().find("game_build");
    const ToolDef* run = engine.tools().find("game_run");
    const ToolDef* settings = engine.tools().find("game_settings");
    REQUIRE(build);
    REQUIRE(run);
    REQUIRE(settings);
    CHECK(build->category == "files");
    CHECK(build->mutates);
    CHECK(build->openWorld);  // the human is asked before an agent builds or runs a process
    CHECK(run->openWorld);
    CHECK(run->category == "files");

    ToolResult get = engine.callTool("game_settings", Json::object(), "test");
    REQUIRE_FALSE(get.isError);
    CHECK(get.structured["settings"]["title"].asString() == "Orbit Test");
    CHECK(get.structured["start_scene"].asString() == "scenes/main.sky.json");

    ToolResult set = engine.callTool(
        "game_settings",
        Json::object({{"operation", "set"}, {"settings", Json::parse(R"({"window":{"width":800,"height":600},"quality":"low","pitch":null})").value()}}), "test");
    REQUIRE_FALSE(set.isError);
    CHECK(set.structured["settings"]["window"]["width"].asInt() == 800);
    CHECK(set.structured["settings"]["quality"].asString() == "low");
    CHECK(Json::parse(slurp(project.path / "game.json"))->get("window").get("width").asInt() == 800);
    ToolResult bad = engine.callTool("game_settings", Json::object({{"operation", "set"}, {"settings", Json::parse(R"({"qualty":"low"})").value()}}), "test");
    CHECK(bad.isError);
    CHECK(Json::parse(slurp(project.path / "game.json"))->get("quality").asString() == "low");  // a rejected change writes nothing

    ToolResult dry = engine.callTool("game_build", Json::object({{"dry_run", true}}), "test");
    REQUIRE_FALSE(dry.isError);
    CHECK(dry.structured["files"].asInt() > 10);
    CHECK(dry.structured["missing"].size() == 1);
    CHECK(engine.callTool("game_build", Json::object(), "test").isError);  // "out" is required

    // game_run: nothing to stop or ask about before anything started.
    ToolResult status = engine.callTool("game_run", Json::object({{"action", "status"}}), "test");
    CHECK(status.isError);
    CHECK(engine.callTool("game_run", Json::object({{"action", "dance"}}), "test").isError);

    if (playerAvailable()) {
        TempDir out("tools-out");
        ::setenv("SKYWALKER_PLAYER", SKY_PLAYER_PATH, 1);  // the tool finds the player like the CLI does
        ToolResult built = engine.callTool("game_build", Json::object({{"out", out.str()}, {"name", "Tool Built"}, {"version", "3.0.0"}}), "test");
        INFO(built.structured.dump(2));
        REQUIRE_FALSE(built.isError);
        CHECK(fs::exists(out.path / "Tool Built.app/Contents/Info.plist"));
        CHECK(built.structured["version"].asString() == "3.0.0");
        CHECK(built.structured["warnings"].size() >= 1);  // no icon, one missing sound
    }
}
