// Save games (docs/SAVE_GAMES.md): round trips, spawned and destroyed entities, exact Wander state,
// migrations, damaged files, atomic writes, deterministic replay after a load, the Wander builtins and
// the agent tools.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "skywalker/assets/AssetDatabase.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/game/SaveGame.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

struct Project {
    fs::path dir;
    explicit Project(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-saves-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~Project() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
    std::string read(const std::string& rel) const {
        std::ifstream f(dir / rel, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    fs::path saves() const { return dir / ".skywalker" / "saves"; }
};

std::unique_ptr<Engine> makeEngine(const Project& p) {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.audio = audio::AudioMode::Null;
    cfg.projectDir = p.dir.string();
    return std::make_unique<Engine>(cfg);
}

EntityId make(Engine& e, const std::string& doc) {
    auto parsed = Json::parse(doc);
    REQUIRE(parsed);
    EntityId id = e.scene().create(parsed.value().get("name").asString());
    Status st = e.scene().applyEntityJson(id, parsed.value());
    INFO((st.ok() ? std::string() : st.error().message));
    REQUIRE(st);
    return id;
}

void behave(Engine& e, EntityId id, const std::string& source) {
    REQUIRE(e.scene().setBehaviors(id, Json::array({Json::object({{"name", "Test"}, {"source", source}})})));
}

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

std::string errorCode(Engine& e, const char* tool, const std::string& args) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    REQUIRE(r.isError);
    return r.structured.get("error").asString();
}

Json var(Engine& e, EntityId id, const char* name) { return e.scene().record(id)->vars.get(name); }

/// Every entity of the scene in a comparable form (order, ids, names, components, vars).
Json worldState(Engine& e) {
    Json out = Json::array();
    const Scene& s = e.scene();
    for (EntityId id : s.entities()) {
        Json j = s.entityToJson(id);
        j.erase("behaviors");
        // Exact transforms: scene JSON rounds to 4 decimals, which would hide a drift.
        const Transform& t = *s.get<Transform>(id);
        for (float v : {t.position.x, t.position.y, t.position.z, t.rotation.x, t.rotation.y, t.rotation.z}) {
            j["exact"].push(static_cast<double>(v));
        }
        out.push(std::move(j));
    }
    return out;
}

// A coin prefab that spawns persisted (spawned) and expires.
const char* kCoinPrefab = R"({"format": "skywalker.prefab", "version": 2, "name": "coin",
  "root": {"pid": 1, "name": "Coin", "enabled": true,
           "components": {"transform": {}, "persist": {"spawned": true}},
           "behaviors": [{"name": "Coin", "source": "var age = 0\non tick\n  age += dt\n  rotate self by (0, 90 * dt, 0)\n  if age > 1.3 then destroy self end\nend"}],
           "children": [{"pid": 2, "name": "Glow", "enabled": true, "components": {"light": {"intensity": 2}}}]}})";

// Random numbers, timers, a waiting coroutine, a state machine and spawns: everything a load must resume exactly.
const char* kSpawner = R"(var spawned = 0
var rolls: list = []
every 0.25 seconds
  if spawned < 6 then
    spawn("prefab:prefabs/coin.prefab.json", (random(-5, 5), 0, random(-5, 5)))
    spawned += 1
  end
end
on tick
  rolls.push(random_int(1, 6))
  if rolls.length > 4 then rolls.remove_at(0) end
end)";

const char* kWalker = R"(var phase = 0
var trail: list = [1, 2, 3]
state Walk
  on tick
    move self by (1 * dt, 0, 0)
    if state_time > 0.5 then go to Rest end
  end
end
state Rest
  on enter
    phase += 1
    trail.push(phase)
  end
  on tick
    wait 0.2
    go to Walk
  end
end)";

/// A small game: a spawner and a walker (both persisted), saved as scenes/main.sky.json.
void buildGame(const Project& p) {
    p.write("prefabs/coin.prefab.json", kCoinPrefab);
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId spawner = make(*e, R"({"name": "Spawner", "components": {"persist": {"id": "spawner"}}})");
    behave(*e, spawner, kSpawner);
    EntityId walker = make(*e, R"({"name": "Walker", "components": {"persist": {"id": "walker"}}})");
    behave(*e, walker, kWalker);
    REQUIRE(e->saveScene("scenes/main.sky.json"));
}

}  // namespace

TEST_CASE("saves: round trip of whole entities, chosen fields, vars and game vars") {
    Project p("roundtrip");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId hero = make(*e, R"({"name": "Hero", "components": {"transform": {"position": [1, 0, 2]}, "persist": {"id": "hero"}},
                                 "vars": {"hp": 7, "inventory": ["key", "map"]}})");
    EntityId lamp = make(*e, R"({"name": "Lamp", "components": {"light": {"intensity": 3, "range": 9},
                                 "persist": {"mode": "fields", "fields": ["light.intensity"], "vars": false}}})");
    EntityId rock = make(*e, R"({"name": "Rock", "components": {"transform": {"position": [5, 0, 5]}}})");
    REQUIRE(e->saveScene("scenes/main.sky.json"));
    e->play();
    e->step(3);
    REQUIRE(e->saves().setGlobal("chapter", Json(2)));
    REQUIRE(e->saves().setGlobal("flags", Json::array({1, 2, 3})));  // a list, not a vector

    Json saved = call(*e, "save_game", R"({"slot": "slot1", "meta": {"title": "Gate", "chapter": 2}})");
    CHECK(saved.get("entities").asInt() == 2);
    CHECK(saved.get("warnings").isArray());
    CHECK(fs::exists(p.saves() / "slot1.save.json"));
    CHECK(saved.get("meta").get("title").asString() == "Gate");

    // Change everything, then load.
    Scene& s = e->scene();
    s.get<Transform>(hero)->position = {9, 9, 9};
    REQUIRE(s.patchVars(hero, Json::object({{"hp", 1}})));
    s.get<Light>(lamp)->intensity = 0.5f;
    s.get<Light>(lamp)->range = 2.f;
    s.get<Transform>(rock)->position = {0, 0, 0};
    REQUIRE(e->saves().setGlobal("chapter", Json(5)));
    Json loaded = call(*e, "load_game", R"({"slot": "slot1"})");
    CHECK(loaded.get("entities").asInt() == 2);

    CHECK(s.get<Transform>(hero)->position == Vec3{1, 0, 2});
    CHECK(var(*e, hero, "hp").asInt() == 7);
    CHECK(var(*e, hero, "inventory").size() == 2);
    CHECK(s.get<Light>(lamp)->intensity == doctest::Approx(3.f));
    CHECK(s.get<Light>(lamp)->range == doctest::Approx(2.f));       // not a persisted field
    CHECK(s.get<Transform>(rock)->position == Vec3{0, 0, 0});       // not persisted at all
    Json globals = e->saves().globalsJson();
    CHECK(globals.get("chapter").asInt() == 2);
    CHECK(globals.get("flags").isArray());

    // save_list shows the slot with its metadata, size, versions and play time.
    Json list = call(*e, "save_list", "{}");
    REQUIRE(list.get("slots").size() == 1);
    const Json& slot = list.get("slots")[0];
    CHECK(slot.get("slot").asString() == "slot1");
    CHECK(slot.get("meta").get("chapter").asInt() == 2);
    CHECK(slot.get("bytes").asInt() > 0);
    CHECK(slot.get("version").asInt() == 1);
    CHECK(slot.get("formatVersion").asInt() == game::kSaveFormatVersion);
    CHECK(slot.get("playTime").asNumber() == doctest::Approx(3.0 / 60.0));
    CHECK(slot.get("scene").asString() == "scenes/main.sky.json");
    CHECK(list.get("folder").asString() == p.saves().string());

    // save_inspect right after the load: nothing differs. After a change: the field shows up.
    Json same = call(*e, "save_inspect", R"({"slot": "slot1"})");
    CHECK(same.get("summary").get("differs").asInt() == 0);
    s.get<Transform>(hero)->position = {4, 0, 2};
    Json diff = call(*e, "save_inspect", R"({"slot": "slot1"})");
    CHECK(diff.get("summary").get("differs").asInt() == 1);
    bool found = false;
    for (const auto& row : diff.get("entities").elements()) {
        for (const auto& d : row.get("diffs").elements()) found = found || d.get("path").asString() == "components.transform.position";
    }
    CHECK(found);
}

TEST_CASE("saves: spawned entities are recreated with their ids; destroyed ones stay destroyed") {
    Project p("spawned");
    p.write("prefabs/coin.prefab.json", kCoinPrefab);
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId chest = make(*e, R"({"name": "Chest", "components": {"persist": {}}})");
    EntityId barrel = make(*e, R"({"name": "Barrel", "components": {"persist": {}}})");
    REQUIRE(e->saveScene("scenes/main.sky.json"));
    e->play();
    e->step(1);
    PrefabPlacement at;
    at.hasPosition = true;
    at.position = {2, 0, 0};
    auto coin = e->instantiatePrefabAsset("prefabs/coin.prefab.json", at);
    REQUIRE(coin);
    const EntityId coinId = *coin;
    const size_t coinChildren = e->scene().children(coinId).size();
    REQUIRE(coinChildren == 1);
    e->scene().destroy(barrel);  // destroyed before the save: a tombstone
    e->step(2);
    call(*e, "save_game", R"({"slot": "s"})");

    // After the save: the coin is collected, another one appears, the chest is destroyed.
    e->scene().destroy(coinId);
    auto later = e->instantiatePrefabAsset("prefabs/coin.prefab.json", at);
    REQUIRE(later);
    e->scene().destroy(chest);
    e->step(1);

    Json r = call(*e, "load_game", R"({"slot": "s"})");
    CHECK(r.get("spawned").asInt() == 1);
    CHECK(r.get("destroyed").asInt() == 1);  // the coin spawned after the save
    CHECK(e->scene().exists(coinId));
    CHECK(e->scene().record(coinId)->name == "Coin");
    CHECK(e->scene().children(coinId).size() == coinChildren);
    CHECK(e->scene().get<Transform>(coinId)->position == Vec3{2, 0, 0});
    CHECK_FALSE(e->scene().exists(*later));
    CHECK_FALSE(e->scene().exists(barrel));
    // The chest was destroyed after the save and is not spawned: a load cannot bring it back, and says so.
    CHECK_FALSE(e->scene().exists(chest));
    bool warned = false;
    for (const auto& w : r.get("warnings").elements()) warned = warned || w.asString().find("Chest") != std::string::npos;
    CHECK(warned);

    // Saving again keeps the barrel's tombstone (it was destroyed in the loaded line).
    call(*e, "save_game", R"({"slot": "s2"})");
    auto doc = game::parseSave(p.read(".skywalker/saves/s2.save.json"));
    REQUIRE(doc);
    bool tomb = false;
    for (const auto& k : doc->get("destroyed").elements()) tomb = tomb || k.asString() == "#" + std::to_string(barrel);
    CHECK(tomb);
}

TEST_CASE("saves: Wander vars, state machines and waiting handlers resume exactly") {
    Project p("wander");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId w = make(*e, R"({"name": "Walker", "components": {"persist": {"id": "walker"}}})");
    behave(*e, w, kWalker);
    e->play();
    e->step(40);  // in Rest, waiting
    REQUIRE(e->runtime().currentState(w) == "Rest");
    call(*e, "save_game", R"({"slot": "w"})");
    const Json phase = var(*e, w, "phase");
    const Vec3 at = e->scene().get<Transform>(w)->position;
    e->step(45);
    CHECK(var(*e, w, "phase") != phase);
    call(*e, "load_game", R"({"slot": "w"})");
    CHECK(var(*e, w, "phase") == phase);
    CHECK(e->runtime().currentState(w) == "Rest");
    CHECK(e->scene().get<Transform>(w)->position == at);
    // The list var stays a list of numbers (JSON [1, 2, 3, ...] would read back as a vector).
    Json state = e->runtime().saveState([&](EntityId id) { return id == w; });
    bool list = false;
    for (const auto& t : state.get("vars").elements()) {
        for (const auto& slot : t.get("slots").elements()) {
            if (slot.get("name").asString() == "trail") list = slot.get("value").isArray();
        }
    }
    CHECK(list);
}

TEST_CASE("saves: determinism - save at tick N, load, run M ticks equals running N + M ticks") {
    Project p("determinism");
    buildGame(p);
    constexpr int N = 37, M = 61;

    auto direct = makeEngine(p);
    REQUIRE(direct->loadScene("scenes/main.sky.json"));
    direct->play();
    direct->step(N + M);

    auto replay = makeEngine(p);
    REQUIRE(replay->loadScene("scenes/main.sky.json"));
    replay->play();
    replay->step(N);
    call(*replay, "save_game", R"({"slot": "det"})");
    replay->step(29);  // the timeline moves on...
    Json r = call(*replay, "load_game", R"({"slot": "det"})");
    CHECK(r.get("warnings").size() == 0);
    replay->step(M);  // ...and the loaded one replays the direct run

    CHECK(replay->runtime().frame() == direct->runtime().frame());
    CHECK(replay->scene().size() == direct->scene().size());
    CHECK(worldState(*replay) == worldState(*direct));

    // A fresh session (another engine, as after restarting the game) loads the same state.
    auto fresh = makeEngine(p);
    REQUIRE(fresh->loadScene("scenes/main.sky.json"));
    fresh->play();
    fresh->step(5);
    call(*fresh, "load_game", R"({"slot": "det"})");
    fresh->step(M);
    CHECK(worldState(*fresh) == worldState(*direct));
}

TEST_CASE("saves: loading during play never breaks the editor's stop snapshot") {
    Project p("stop");
    buildGame(p);
    p.write("scenes/other.sky.json", R"({"format": "skywalker.scene", "version": 1, "name": "Other", "seed": 3,
        "entities": [{"id": 50, "name": "Other Hero", "components": {"persist": {"id": "hero2"}}}]})");
    auto e = makeEngine(p);
    // A save made in another scene: loading it switches scene during play.
    REQUIRE(e->loadScene("scenes/other.sky.json"));
    e->play();
    e->step(2);
    call(*e, "save_game", R"({"slot": "elsewhere"})");
    e->stop();

    REQUIRE(e->loadScene("scenes/main.sky.json"));
    const Json edited = e->scene().toJson();
    e->play();
    e->step(10);
    Json r = call(*e, "load_game", R"({"slot": "elsewhere"})");
    CHECK(r.get("sceneChanged").asString() == "scenes/other.sky.json");
    CHECK(e->scene().find("Other Hero") != kNoEntity);
    CHECK(e->saves().currentScene() == "scenes/other.sky.json");
    e->step(3);
    e->stop();
    CHECK(e->scene().toJson() == edited);
    CHECK(e->scenePath() == "scenes/main.sky.json");
    CHECK(e->saves().globalsJson().size() == 0);
}

TEST_CASE("saves: migrations from v1 to v2 (Wander and C++), and saves from a newer game") {
    Project p("migrate");
    p.write("game.json", R"({"id": "migrator", "saves": {"version": 1}})");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    make(*e, R"({"name": "Hero", "components": {"persist": {"id": "hero"}}, "vars": {"gold": 12}})");
    REQUIRE(e->saveScene("scenes/main.sky.json"));
    e->play();
    REQUIRE(e->saves().setGlobal("lvl", Json(3)));
    call(*e, "save_game", R"({"slot": "old"})");
    call(*e, "save_game", R"({"slot": "old2"})");

    // v2 renames the hero's `gold` var to `coins` and the `lvl` global to `level`. Maps and lists are values
    // in Wander: edit copies, then write them back.
    p.write("scripts/save_migrate.wander", R"(fn migrate(from: number, data: map) -> map
  if from == 1 then
    let out = []
    for e in data.entities
      if e.key == "id:hero" then
        let vars = e.entity.vars
        vars.set("coins", vars.get("gold", 0))
        vars.remove("gold")
        let ent = e.entity
        ent.vars = vars
        e.entity = ent
      end
      out.push(e)
    end
    data.entities = out
    let g = data.globals
    g.set("level", g.get("lvl", 1))
    g.remove("lvl")
    data.globals = g
  end
  return data
end)");
    p.write("game.json", R"({"id": "migrator", "saves": {"version": 2, "migrate": "scripts/save_migrate.wander"}})");
    Json r = call(*e, "load_game", R"({"slot": "old"})");
    CHECK(r.get("migratedFrom").asInt() == 1);
    EntityId hero = e->scene().find("Hero");
    CHECK(var(*e, hero, "coins").asInt() == 12);
    CHECK_FALSE(e->scene().record(hero)->vars.contains("gold"));
    CHECK(e->saves().globalsJson().get("level").asInt() == 3);

    // A registered C++ migrator runs instead of the Wander one for its version.
    e->saves().addMigrator(1, [](Json& doc) -> Status {
        doc["globals"]["level"] = 99;
        return {};
    });
    call(*e, "load_game", R"({"slot": "old2"})");
    CHECK(e->saves().globalsJson().get("level").asInt() == 99);

    // A save written by a newer game is refused with a clear error.
    call(*e, "save_game", R"({"slot": "new"})");
    p.write("game.json", R"({"id": "migrator", "saves": {"version": 1}})");
    CHECK(errorCode(*e, "load_game", R"({"slot": "new"})") == "save_too_new");
}

TEST_CASE("saves: damaged files fail with a clear error and are listed as damaged") {
    Project p("corrupt");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    make(*e, R"({"name": "Hero", "components": {"persist": {}}})");
    e->play();
    call(*e, "save_game", R"({"slot": "good"})");
    call(*e, "save_game", R"({"slot": "edited"})");
    call(*e, "save_game", R"({"slot": "cut"})");
    // Edited by hand: valid JSON, wrong hash.
    std::string text = p.read(".skywalker/saves/edited.save.json");
    auto pos = text.find("\"Hero\"");
    REQUIRE(pos != std::string::npos);
    text.replace(pos, 6, "\"Zero\"");
    p.write(".skywalker/saves/edited.save.json", text);
    // Truncated (a crash mid-write without atomic rename would leave this).
    std::string cut = p.read(".skywalker/saves/cut.save.json");
    p.write(".skywalker/saves/cut.save.json", cut.substr(0, cut.size() / 2));

    ToolResult edited = e->callTool("load_game", Json::parse(R"({"slot": "edited"})").value(), "agent:test");
    REQUIRE(edited.isError);
    CHECK(edited.content.front().text.find("integrity hash") != std::string::npos);
    CHECK(errorCode(*e, "load_game", R"({"slot": "cut"})") == "save_corrupted");
    CHECK(errorCode(*e, "load_game", R"({"slot": "nope"})") == "save_not_found");
    CHECK(errorCode(*e, "load_game", R"({"slot": "goood"})") == "save_not_found");
    ToolResult guess = e->callTool("load_game", Json::parse(R"({"slot": "goood"})").value(), "agent:test");
    CHECK(guess.content.front().text.find("good") != std::string::npos);  // did-you-mean

    Json list = call(*e, "save_list", "{}");
    CHECK(list.get("slots").size() == 3);
    CHECK(list.get("warnings").size() == 2);
    call(*e, "load_game", R"({"slot": "good"})");
}

TEST_CASE("saves: writes are atomic and compression is optional") {
    Project p("atomic");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId hero = make(*e, R"({"name": "Hero", "components": {"persist": {}}, "vars": {"hp": 3}})");
    e->play();
    call(*e, "save_game", R"({"slot": "a"})");
    const std::string before = p.read(".skywalker/saves/a.save.json");
    // A write that cannot complete (its temp file cannot be created) leaves the old save untouched.
    const fs::path blocker = p.saves() / (".a." + std::to_string(::getpid()) + ".tmp");
    fs::create_directories(blocker);
    REQUIRE(e->scene().patchVars(hero, Json::object({{"hp", 1}})));
    CHECK(errorCode(*e, "save_game", R"({"slot": "a"})") == "io_error");
    CHECK(p.read(".skywalker/saves/a.save.json") == before);
    fs::remove_all(blocker);
    call(*e, "save_game", R"({"slot": "a"})");
    for (const auto& entry : fs::directory_iterator(p.saves())) CHECK(entry.path().extension() != ".tmp");

    // Compressed slots (zlib): smaller, same content, the plain file is replaced.
    p.write("game.json", R"({"saves": {"compress": true}})");
    call(*e, "save_game", R"({"slot": "a"})");
    CHECK(fs::exists(p.saves() / "a.save.json.gz"));
    CHECK_FALSE(fs::exists(p.saves() / "a.save.json"));
    REQUIRE(e->scene().patchVars(hero, Json::object({{"hp", 9}})));
    call(*e, "load_game", R"({"slot": "a"})");
    CHECK(var(*e, hero, "hp").asInt() == 1);

    // Slot limit: named slots count, autosave and quicksave do not.
    p.write("game.json", R"({"saves": {"maxSlots": 1}})");
    CHECK(errorCode(*e, "save_game", R"({"slot": "b"})") == "slot_limit");
    call(*e, "save_game", R"({"slot": "a"})");  // overwriting is fine
    call(*e, "save_game", R"({"slot": "autosave"})");
    call(*e, "save_game", R"({"slot": "quicksave"})");
}

TEST_CASE("saves: Wander builtins - save_game, load_game, has_save, list_saves, game_var, on saved / loaded") {
    Project p("builtins");
    auto e = makeEngine(p);
    REQUIRE(e->newScene("Main", false));
    EntityId gm = make(*e, R"({"name": "Game", "components": {"persist": {"id": "game"}}})");
    behave(*e, gm, R"(var saves_heard = 0
var loads_heard = 0
var listed = 0
var had = false
on start
  had = has_save("cp")
  game_var("chapter", 1)
end
on key "s"
  game_var("chapter", game_var("chapter") + 1)
  save_game("cp", {title: "Checkpoint"})
end
on key "l"
  load_game("cp")
end
on saved
  saves_heard += 1
  listed = list_saves().length
end
on loaded
  loads_heard += 1
end)");
    e->play();
    e->step(1);
    CHECK(var(*e, gm, "had").asBool() == false);
    e->input().pressed.insert("s");
    e->step(1);
    CHECK(fs::exists(p.saves() / "cp.save.json"));
    e->step(1);
    CHECK(var(*e, gm, "saves_heard").asInt() == 1);
    CHECK(var(*e, gm, "listed").asInt() == 1);
    CHECK(e->saves().globalsJson().get("chapter").asInt() == 2);
    REQUIRE(e->saves().setGlobal("chapter", Json(7)));
    e->input().pressed.insert("l");
    e->step(2);
    CHECK(e->saves().globalsJson().get("chapter").asInt() == 2);
    CHECK(var(*e, gm, "loads_heard").asInt() == 1);

    // Outside play the tools refuse; bad slot names are rejected with a hint.
    e->stop();
    CHECK(errorCode(*e, "save_game", R"({"slot": "x"})") == "not_playing");
    CHECK(errorCode(*e, "save_delete", R"({"slot": "Bad Name"})") == "invalid_slot");
    call(*e, "save_delete", R"({"slot": "cp"})");
    CHECK_FALSE(e->saves().has("cp"));
    CHECK(errorCode(*e, "save_delete", R"({"slot": "cp"})") == "save_not_found");
}

TEST_CASE("saves: settings, slot names and the platform save folder") {
    CHECK(game::validateSlotName("slot1").ok());
    CHECK(game::validateSlotName("chapter-2_b").ok());
    CHECK_FALSE(game::validateSlotName("").ok());
    CHECK_FALSE(game::validateSlotName("../x").ok());
    CHECK_FALSE(game::validateSlotName("Slot").ok());
    CHECK(game::isReservedSlot("autosave"));
    auto bad = game::SaveSettings::fromJson(Json::parse(R"({"verison": 2})").value());
    REQUIRE_FALSE(bad);
    CHECK(bad.error().hint.find("version") != std::string::npos);
    auto good = game::SaveSettings::fromJson(Json::parse(R"({"version": 3, "maxSlots": 5, "compress": true})").value());
    REQUIRE(good);
    CHECK(good->version == 3);
    CHECK(good->maxSlots == 5);
    // game.json accepts the block and validates it.
    CHECK(game::GameSettings::fromJson(Json::parse(R"({"id": "x", "saves": {"version": 2}})").value()));
    CHECK_FALSE(game::GameSettings::fromJson(Json::parse(R"({"id": "x", "saves": {"maxSlot": 2}})").value()));
    const std::string dir = game::userSaveDir("sky-dash");
    CHECK(dir.find("sky-dash") != std::string::npos);
    CHECK(fs::path(dir).filename() == "saves");
#if defined(__APPLE__)
    CHECK(dir.find("Application Support") != std::string::npos);
#endif
}
