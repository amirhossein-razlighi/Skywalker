#include <doctest/doctest.h>

#include <unistd.h>

#include <filesystem>
#include <string>

#include "sky_api.h"
#include "skywalker/core/Json.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

Json callTool(SkyEngine* h, const char* tool, const char* args) {
    char* raw = sky_call_tool(h, tool, args, "test");
    REQUIRE(raw != nullptr);
    std::string text = raw;
    sky_string_free(raw);
    auto j = Json::parse(text);
    REQUIRE(j);
    return j->get("structuredContent");
}

}  // namespace

TEST_CASE("capi: platform input (keys, mouse, gamepad) feeds the action map") {
    std::string dir = (fs::temp_directory_path() / ("skywalker-capi-" + std::to_string(::getpid()))).string();
    fs::create_directories(dir);
    SkyEngine* h = sky_engine_create(dir.c_str());
    REQUIRE(h != nullptr);

    sky_input_key(h, "Space", 1);                  // aliases and case are normalized
    sky_input_mouse_move(h, 0.4f, 0.6f, 10.f, 5.f);  // 5 pixels DOWN on screen
    sky_input_mouse_button(h, 0, 1);
    SkyGamepad pad{};
    pad.left_x = 3.f;  // out of range values are clamped
    pad.right_trigger = 0.9f;
    pad.buttons = (1u << 3) | (1u << 12);  // north + dpad up
    sky_input_gamepad(h, 0, 1, "Test pad", &pad);

    callTool(h, "sim_control", R"({"action":"step","ticks":1})");
    Json state = callTool(h, "input_map", "{}").get("state");
    CHECK(state.get("jump").get("held").asBool());
    CHECK(state.get("jump").get("pressed").asBool());
    CHECK(state.get("fire").get("held").asBool());                       // left mouse button and right trigger
    CHECK(state.get("move").get("value")[0].asNumber() == doctest::Approx(1.0));  // stick clamped to 1
    CHECK(state.get("look").get("value")[0].asNumber() == doctest::Approx(1.0));   // 10 px * 0.1
    CHECK(state.get("look").get("value")[1].asNumber() == doctest::Approx(-0.5));  // moved down: negative y
    CHECK(state.get("cursor").get("value")[0].asNumber() == doctest::Approx(0.4));
    CHECK(state.get("cursor").get("value")[1].asNumber() == doctest::Approx(0.6));

    // Releases, a second tick: movement was consumed, held states persist, jump is no longer new.
    sky_input_key(h, "space", 0);
    sky_input_mouse_button(h, 0, 0);
    pad.right_trigger = 0.f;
    sky_input_gamepad(h, 0, 1, "Test pad", &pad);
    callTool(h, "sim_control", R"({"action":"step","ticks":1})");
    state = callTool(h, "input_map", "{}").get("state");
    CHECK_FALSE(state.get("jump").get("held").asBool());
    CHECK(state.get("jump").get("released").asBool());
    CHECK_FALSE(state.get("fire").get("held").asBool());
    CHECK(state.get("look").get("value")[0].asNumber() == doctest::Approx(0.0));

    // Unplugging clears the pad.
    sky_input_gamepad(h, 0, 0, nullptr, nullptr);
    callTool(h, "sim_control", R"({"action":"step","ticks":1})");
    state = callTool(h, "input_map", "{}").get("state");
    CHECK(state.get("move").get("value")[0].asNumber() == doctest::Approx(0.0));

    // Bad arguments never crash.
    sky_input_gamepad(h, 99, 1, "x", &pad);
    sky_input_mouse_button(h, 7, 1);
    sky_input_key(h, nullptr, 1);
    sky_input_gamepad(nullptr, 0, 1, "x", &pad);

    sky_engine_destroy(h);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
