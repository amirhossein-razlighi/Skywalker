// UI: styles, layout (anchors, stacks, grid, fit-content) with exact rects, drawing, hit testing,
// input into Wander, tools, and headless captures that show UI text.

#include <doctest/doctest.h>

#include "skywalker/engine/Engine.h"
#include "skywalker/ui/UiSystem.h"
#include "skywalker/ui/World2D.h"

using namespace sky;

namespace {

std::unique_ptr<Engine> makeEngine() {
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    auto e = std::make_unique<Engine>(cfg);
    (void)e->newScene("UI", false);
    return e;
}

Json call(Engine& e, const char* tool, const Json& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, args, "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

Json parse(const char* s) { return Json::parse(s).value(); }

EntityId canvas(Scene& s, const char* json = R"({"referenceResolution": [1000, 500]})") {
    EntityId c = s.create("Canvas");
    REQUIRE(s.patchComponent(c, "ui_canvas", parse(json)).ok());
    return c;
}

EntityId element(Scene& s, EntityId parent, const char* name, const char* json) {
    EntityId e = s.create(name, parent);
    Status st = s.patchComponent(e, "ui", parse(json));
    if (!st.ok()) FAIL(st.error().message);
    return e;
}

ui::Rect rectOf(const ui::Layout& lay, EntityId e) {
    const ui::Node* n = lay.find(e);
    REQUIRE(n);
    return n->rect;
}

}  // namespace

TEST_CASE("ui: themes and style sheets cascade by kind, class, name, overrides and state") {
    auto dark = ui::StyleSheet::theme("dark");
    ui::Style button = dark->resolve("button", {}, "Play", Json::object(), {}, nullptr);
    CHECK(button.radius == doctest::Approx(8.f));
    CHECK(button.textAlign == "center");
    CHECK(button.font == "Inter");  // inherited from the canvas rule
    ui::Style primary = dark->resolve("button", {"primary"}, "Play", Json::object(), {}, nullptr);
    CHECK_FALSE(primary.background == button.background);
    CHECK(primary.bold);
    ui::StyleState hover;
    hover.hover = true;
    ui::Style hovered = dark->resolve("button", {}, "Play", Json::object(), hover, nullptr);
    CHECK_FALSE(hovered.background == button.background);
    ui::StyleState disabled;
    disabled.disabled = true;
    CHECK(dark->resolve("button", {}, "", Json::object(), disabled, nullptr).opacity < 0.5f);
    ui::Style over = dark->resolve("button", {"primary"}, "Play", parse(R"({"radius": 20, "color": "red"})"), {}, nullptr);
    CHECK(over.radius == doctest::Approx(20.f));
    CHECK(over.color.x == doctest::Approx(1.f));
    // Text properties inherit from the parent element.
    ui::Style parent = dark->resolve("panel", {}, "", parse(R"({"fontSize": 30, "font": "serif"})"), {}, nullptr);
    ui::Style child = dark->resolve("text", {}, "", Json::object(), {}, &parent);
    CHECK(child.fontSize == doctest::Approx(30.f));
    CHECK(child.font == "serif");
    // Themes differ; parchment uses a serif font.
    CHECK(ui::StyleSheet::theme("parchment")->root().font == "EB Garamond");
    CHECK(ui::StyleSheet::theme("pixel")->resolve("button", {}, "", Json::object(), {}, nullptr).radius == doctest::Approx(0.f));
    // Style sheets: extends, vars, validation with did-you-mean.
    auto sheet = ui::StyleSheet::parse(parse(R"({"extends": "light", "vars": {"brand": "#123456"},
        "rules": {"button": {"background": "$brand"}, "#Title": {"fontSize": 64}}})"), nullptr);
    REQUIRE(sheet.ok());
    ui::Style b = sheet.value()->resolve("button", {}, "", Json::object(), {}, nullptr);
    CHECK(reflect::toHexColor(b.background) == "#123456");
    CHECK(sheet.value()->resolve("text", {}, "Title", Json::object(), {}, nullptr).fontSize == doctest::Approx(64.f));
    auto bad = ui::StyleSheet::parse(parse(R"({"rules": {"button": {"backgrund": "#fff"}}})"), nullptr);
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().hint.find("background") != std::string::npos);
    CHECK_FALSE(ui::StyleSheet::parse(parse(R"({"extends": "drak"})"), nullptr).ok());
}

TEST_CASE("ui: anchors, margins and canvas scaling give exact rects") {
    Scene s;
    render2d::Assets2D assets(".");
    ui::UiSystem ui(assets);
    EntityId c = canvas(s);
    EntityId tl = element(s, c, "TL", R"({"anchor": "top_left", "position": [10, 20], "size": [100, 50]})");
    EntityId center = element(s, c, "Center", R"({"anchor": "center", "size": [200, 100]})");
    EntityId br = element(s, c, "BR", R"({"anchor": "bottom_right", "position": [-10, -10], "size": [50, 40]})");
    EntityId fill = element(s, c, "Fill", R"({"anchor": "fill", "margin": [5, 10, 15, 20]})");
    EntityId bar = element(s, c, "Bar", R"({"anchor": "top_stretch", "size": [0, 60], "margin": [0, 30, 0, 30]})");
    EntityId custom = element(s, c, "Custom", R"({"anchor": "custom", "anchorMin": [0.25, 0.5], "anchorMax": [0.25, 0.5],
                                                  "pivot": [0.5, 1], "size": [40, 40]})");
    ui::Layout lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, tl) == ui::Rect{10, 20, 100, 50});
    CHECK(rectOf(lay, center) == ui::Rect{400, 200, 200, 100});
    CHECK(rectOf(lay, br) == ui::Rect{940, 450, 50, 40});
    CHECK(rectOf(lay, fill) == ui::Rect{20, 5, 970, 480});
    CHECK(rectOf(lay, bar) == ui::Rect{30, 0, 940, 60});
    CHECK(rectOf(lay, custom) == ui::Rect{230, 210, 40, 40});
    // At twice the reference resolution, everything scales by 2 in screen pixels.
    ui::Layout big = ui.computeLayout(s, 2000, 1000);
    CHECK(big.canvases[0].scale == doctest::Approx(2.f));
    CHECK(big.screenRect(*big.find(center)) == ui::Rect{800, 400, 400, 200});
    // A wider screen with match = 0.5 scales by the geometric mean and keeps anchors.
    ui::Layout wide = ui.computeLayout(s, 2000, 500);
    CHECK(wide.canvases[0].scale == doctest::Approx(std::sqrt(2.f)).epsilon(0.001));
    ui::Rect wr = wide.screenRect(*wide.find(br));
    CHECK(wr.x + wr.w == doctest::Approx(2000.f - 10.f * std::sqrt(2.f)).epsilon(0.001));
    // constant scale mode: 1 px = 1 px.
    REQUIRE(s.patchComponent(c, "ui_canvas", parse(R"({"scaleMode": "constant"})")).ok());
    CHECK(ui.computeLayout(s, 2000, 1000).canvases[0].scale == doctest::Approx(1.f));
}

TEST_CASE("ui: row/column stacks, gaps, padding, align, justify, flex and fit-content") {
    Scene s;
    render2d::Assets2D assets(".");
    ui::UiSystem ui(assets);
    EntityId c = canvas(s);
    EntityId col = element(s, c, "Col", R"({"anchor": "top_left", "size": [300, 400], "layout": "column", "gap": 10,
                                           "padding": [20, 30], "align": "stretch"})");
    EntityId a = element(s, col, "A", R"({"size": [100, 40]})");
    EntityId b = element(s, col, "B", R"({"size": [100, 60]})");
    EntityId hidden = element(s, col, "Hidden", R"({"size": [100, 60], "visible": false})");
    EntityId flexer = element(s, col, "Flex", R"({"size": [100, 0], "flex": 1})");
    ui::Layout lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, a) == ui::Rect{30, 20, 240, 40});   // stretched to content width 300 - 2*30
    CHECK(rectOf(lay, b) == ui::Rect{30, 70, 240, 60});
    CHECK(rectOf(lay, flexer) == ui::Rect{30, 140, 240, 240});  // 400 - 20*2 - 40 - 60 - 2*10
    CHECK_FALSE(lay.find(hidden)->visible);

    EntityId row = element(s, c, "Row", R"({"anchor": "top_left", "position": [0, 420], "size": [500, 60], "layout": "row",
                                           "gap": 20, "justify": "center", "align": "center"})");
    EntityId r1 = element(s, row, "R1", R"({"size": [100, 40]})");
    EntityId r2 = element(s, row, "R2", R"({"size": [60, 20]})");
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, r1) == ui::Rect{160, 430, 100, 40});  // (500 - 180) / 2 = 160
    CHECK(rectOf(lay, r2) == ui::Rect{280, 440, 60, 20});
    REQUIRE(s.patchComponent(row, "ui", parse(R"({"justify": "space_between"})")).ok());
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, r2).x == doctest::Approx(440.f));
    REQUIRE(s.patchComponent(row, "ui", parse(R"({"justify": "end"})")).ok());
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, r2).x == doctest::Approx(440.f));

    // fit: a panel sizes to its children (+ padding), text sizes to its content.
    EntityId fit = element(s, c, "Fit", R"({"anchor": "top_left", "layout": "row", "gap": 4, "padding": 8, "fit": "both"})");
    element(s, fit, "F1", R"({"size": [50, 30]})");
    element(s, fit, "F2", R"({"size": [70, 10]})");
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, fit) == ui::Rect{0, 0, 140, 46});
    EntityId label = element(s, c, "Label", R"({"widget": "text", "text": "Hello", "fit": "both"})");
    lay = ui.computeLayout(s, 1000, 500);
    ui::Rect lr = rectOf(lay, label);
    CHECK(lr.w > 30.f);
    CHECK(lr.w < 80.f);
    CHECK(lr.h == doctest::Approx(18.f * 1.21f).epsilon(0.1));  // Inter line height at 18 px
    // Fixed width + fit height wraps the text.
    EntityId para = element(s, c, "Para", R"({"widget": "text", "size": [120, 0], "fit": "height",
        "text": "A much longer paragraph that has to wrap over several lines"})");
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, para).w == doctest::Approx(120.f));
    CHECK(rectOf(lay, para).h > 60.f);
}

TEST_CASE("ui: grids and scroll views") {
    Scene s;
    render2d::Assets2D assets(".");
    ui::UiSystem ui(assets);
    EntityId c = canvas(s);
    EntityId grid = element(s, c, "Grid", R"({"size": [330, 0], "fit": "height", "layout": "grid", "columns": 3, "gap": 15})");
    std::vector<EntityId> cells;
    for (int i = 0; i < 5; ++i) cells.push_back(element(s, grid, ("C" + std::to_string(i)).c_str(), R"({"size": [10, 50]})"));
    ui::Layout lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, cells[0]) == ui::Rect{0, 0, 100, 50});
    CHECK(rectOf(lay, cells[2]) == ui::Rect{230, 0, 100, 50});
    CHECK(rectOf(lay, cells[4]) == ui::Rect{115, 65, 100, 50});
    CHECK(rectOf(lay, grid).h == doctest::Approx(115.f));

    EntityId scroll = element(s, c, "Scroll", R"({"widget": "scroll", "position": [500, 0], "size": [200, 100], "align": "stretch"})");
    std::vector<EntityId> items;
    for (int i = 0; i < 6; ++i) items.push_back(element(s, scroll, ("I" + std::to_string(i)).c_str(), R"({"size": [0, 40]})"));
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(lay.find(scroll)->maxScroll == doctest::Approx(140.f));
    CHECK(rectOf(lay, items[1]).y == doctest::Approx(40.f));
    CHECK(lay.find(items[1])->clip == ui::Rect{500, 0, 200, 100});
    REQUIRE(s.patchComponent(scroll, "ui", parse(R"({"scroll": 30})")).ok());
    lay = ui.computeLayout(s, 1000, 500);
    CHECK(rectOf(lay, items[1]).y == doctest::Approx(10.f));
}

TEST_CASE("ui: drawing emits quads and batches, hit testing finds the topmost interactive element") {
    Scene s;
    render2d::Assets2D assets(".");
    ui::UiSystem ui(assets);
    EntityId c = canvas(s);
    EntityId panel = element(s, c, "Panel", R"({"anchor": "center", "size": [400, 300], "style": "card", "layout": "column",
                                              "padding": 20, "gap": 10, "align": "stretch"})");
    EntityId play = element(s, panel, "Play", R"({"widget": "button", "text": "Play", "size": [0, 50]})");
    EntityId label = element(s, panel, "Label", R"({"widget": "text", "text": "Hello", "fit": "height"})");
    FrameData f;
    f.width = 1000;
    f.height = 500;
    ui.build(s, f, 0.f);
    CHECK(f.render2d.ui.size() > 8);  // shadows, backgrounds, glyphs
    CHECK(f.render2d.uiCanvases.size() == 1);
    size_t glyphs = 0;
    for (const auto& q : f.render2d.ui) glyphs += static_cast<int>(q.params[0]) == static_cast<int>(UIQuadKind::Glyph);
    CHECK(glyphs == 9);  // "Play" + "Hello"
    for (const auto& b : f.render2d.uiBatches) CHECK(b.count > 0);
    // Hit tests: the button over its panel; text inside the button resolves to the button; the panel blocks clicks.
    ui::Layout lay = ui.computeLayout(s, 1000, 500);
    ui::Rect pr = rectOf(lay, play);
    CHECK(ui.hitTest(s, pr.x + 5, pr.y + 5, 1000, 500, nullptr, true) == play);
    ui::Rect lr = rectOf(lay, label);
    CHECK(ui.hitTest(s, lr.x + 2, lr.y + 2, 1000, 500, nullptr, true) == kNoEntity);   // over the panel: consumed
    CHECK(ui.hitTest(s, lr.x + 2, lr.y + 2, 1000, 500, nullptr, false) == panel);
    CHECK(ui.hitTest(s, 5, 5, 1000, 500, nullptr, false) == kNoEntity);
    // Disabled buttons are not targets.
    REQUIRE(s.patchComponent(play, "ui", parse(R"({"interactable": false})")).ok());
    CHECK(ui.hitTest(s, pr.x + 5, pr.y + 5, 1000, 500, nullptr, true) == kNoEntity);
}

TEST_CASE("ui: pointer and keyboard drive buttons, toggles, sliders and inputs into Wander") {
    auto e = makeEngine();
    Scene& s = e->scene();
    call(*e, "ui_create", parse(R"({"canvas": {"name": "HUD", "referenceResolution": [1000, 500]}, "root":
        {"type": "panel", "anchor": "center", "layout": "column", "gap": 10, "align": "stretch", "size": [300, 0], "fit": "height",
         "children": [{"type": "button", "text": "Play"},
                      {"type": "toggle", "name": "Music", "text": "Music"},
                      {"type": "slider", "name": "Volume", "value": 0.5},
                      {"type": "input", "name": "Player Name", "placeholder": "Your name"}]}})"));
    EntityId game = s.create("Game");
    call(*e, "behavior_set", Json::object({{"entity", "Game"}, {"name", "Menu"}, {"source", R"(behavior Menu
  var plays = 0
  var volume = 0
  on ui "Play"
    plays = plays + 1
  end
  on ui "Volume"
    volume = find("Volume").ui.value
  end
end)"}}));
    ui::Layout lay = e->world2d().ui().computeLayout(s, 1000, 500);
    auto center = [&](const char* name) {
        const ui::Node* n = lay.find(s.find(name));
        REQUIRE(n);
        return Vec2{n->rect.x + n->rect.w * 0.5f, n->rect.y + n->rect.h * 0.5f};
    };
    auto& in = e->input();
    e->world2d().setViewport(1000, 500);
    auto clickAt = [&](Vec2 p) {
        in.mouseX = p.x / 1000.f;
        in.mouseY = p.y / 500.f;
        in.mouseHeld.insert("left");
        e->step(1);
        in.mouseHeld.erase("left");
        e->step(2);  // release, then the event is delivered
    };
    clickAt(center("Play"));
    CHECK(s.record(game)->vars.get("plays").asInt() == 1);
    // Toggle flips its value.
    clickAt(center("Music"));
    CHECK(s.get<UIElement>(s.find("Music"))->value == doctest::Approx(1.f));
    // Dragging the slider to its right end sets the max value and sends the event.
    const ui::Node* slider = lay.find(s.find("Volume"));
    in.mouseX = (slider->rect.x + slider->rect.w - 1) / 1000.f;
    in.mouseY = (slider->rect.y + slider->rect.h * 0.5f) / 500.f;
    in.mouseHeld.insert("left");
    e->step(2);
    in.mouseHeld.erase("left");
    e->step(2);
    CHECK(s.get<UIElement>(s.find("Volume"))->value == doctest::Approx(1.f));
    CHECK(s.record(game)->vars.get("volume").asNumber() == doctest::Approx(1.0));
    // Typing into an input after focusing it.
    clickAt(center("Player Name"));
    in.text = "Ada";
    in.pressed.insert("backspace");
    e->step(1);
    CHECK(s.get<UIElement>(s.find("Player Name"))->text == "Ad");
    // Keyboard: tab cycles focus, enter activates the focused button.
    in.mouseX = in.mouseY = 0;  // away from the panel
    in.pressed.insert("escape");
    e->step(1);
    in.pressed.insert("tab");
    e->step(1);
    CHECK(e->world2d().ui().focused() == s.find("Play"));
    in.pressed.insert("enter");
    e->step(2);
    CHECK(s.record(game)->vars.get("plays").asInt() == 2);
    // ui_interact clicks without a pointer.
    Json r = call(*e, "ui_interact", parse(R"({"element": "Play"})"));
    CHECK(r.get("events")[size_t{0}].asString() == "ui:Play");
    e->step(1);
    CHECK(s.record(game)->vars.get("plays").asInt() == 3);
    e->stop();
    CHECK(s.record(game)->vars.get("plays").isNull());  // play state restored
}

TEST_CASE("ui: clicks on UI do not reach the world while playing; editor picks UI elements") {
    auto e = makeEngine();
    call(*e, "ui_create", parse(R"({"canvas": {"referenceResolution": [768, 432]}, "root":
        {"type": "button", "name": "Big", "anchor": "fill", "text": "x"}})"));
    EntityId cube = e->scene().create("Cube");
    e->scene().add<MeshRenderer>(cube);
    EntityId big = e->scene().find("Big");
    CHECK(e->pickAt(384, 216, 768, 432) == big);  // editing: select the element
    e->play();
    CHECK(e->pickAt(384, 216, 768, 432) == kNoEntity);  // playing: the UI consumes the click
    e->stop();
}

TEST_CASE("ui: ui_create templates, ui_inspect rects and ui_style") {
    auto e = makeEngine();
    Json created = call(*e, "ui_create", parse(R"({"template": "main_menu", "canvas": {"name": "Menu"}})"));
    CHECK(created.get("created").size() >= 7);
    CHECK(created.get("layout").size() == 1);
    EntityId play = e->scene().find("Play");
    REQUIRE(play);
    Json inspect = call(*e, "ui_inspect", parse(R"({"element": "Play"})"));
    Json rect = inspect.get("canvases")[size_t{0}].get("elements")[size_t{0}].get("rect");
    CHECK(rect[2].asNumber() == doctest::Approx(360.0));
    CHECK(rect[3].asNumber() == doctest::Approx(60.0));
    // Centered horizontally on a 1920-wide canvas.
    CHECK(rect[size_t{0}].asNumber() + rect[2].asNumber() * 0.5 == doctest::Approx(960.0));
    CHECK(inspect.get("style").get("fontSize").asNumber() > 18.0);  // "large"
    call(*e, "ui_style", parse(R"({"canvas": "Menu", "theme": "parchment"})"));
    CHECK(e->scene().get<UICanvas>(e->scene().find("Menu"))->theme == "parchment");
    call(*e, "ui_style", parse(R"({"element": "Play", "css": {"radius": 2}})"));
    CHECK(e->scene().get<UIElement>(play)->styleOverrides.get("radius").asInt() == 2);
    Json bad = call(*e, "ui_style", parse(R"({"element": "Play", "css": {"colour": "#fff"}})"), false);
    CHECK(bad.get("hint").asString().find("color") != std::string::npos);
    call(*e, "history", parse(R"({"action": "undo"})"));
    // Unknown widget types and fields are rejected with hints.
    Json badType = call(*e, "ui_create", parse(R"({"root": {"type": "buton"}})"), false);
    CHECK(badType.get("hint").asString().find("button") != std::string::npos);
    Json badField = call(*e, "ui_create", parse(R"({"root": {"type": "button", "colr": "#fff"}})"), false);
    CHECK(badField.get("error").asString() == "unknown_field");
}

TEST_CASE("ui: headless captures show UI panels and text") {
    auto e = makeEngine();
    e->scene().environment().skyTop = {0, 0, 0, 1};
    e->scene().environment().skyHorizon = {0, 0, 0, 1};
    call(*e, "ui_create", parse(R"({"canvas": {"referenceResolution": [400, 200]}, "root":
        {"type": "text", "name": "Title", "anchor": "center", "text": "HELLO", "css": {"fontSize": 48, "color": "#ffffff"}}})"));
    call(*e, "ui_create", parse(R"({"parent": "UI", "root": {"type": "panel", "name": "Box", "anchor": "top_left",
        "size": [60, 40], "css": {"background": "#ff0000"}}})"));
    CaptureOptions o;
    o.width = 400;
    o.height = 200;
    o.editorOverlays = false;
    auto cap = e->capture(o);
    REQUIRE(cap.ok());
    // The red panel.
    const uint8_t* p = cap->image.at(30, 20);
    CHECK(p[0] > 200);
    CHECK(p[1] < 40);
    // White glyph pixels inside the text's box, black around it.
    ui::Layout lay = e->world2d().ui().computeLayout(e->scene(), 400, 200);
    ui::Rect r = lay.screenRect(*lay.find(e->scene().find("Title")));
    int bright = 0, total = 0;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(r.y + r.h); ++y) {
        for (int x = static_cast<int>(r.x); x < static_cast<int>(r.x + r.w); ++x) {
            ++total;
            bright += cap->image.at(x, y)[0] > 200;
        }
    }
    CHECK(bright > total / 10);
    CHECK(bright < total * 3 / 4);
    CHECK(cap->image.at(380, 180)[0] < 10);
    // The capture's visible list carries the UI element's real box.
    bool found = false;
    for (const auto& v : cap->visible) {
        if (v.name == "Title") {
            found = true;
            CHECK(v.w == doctest::Approx(r.w).epsilon(0.01));
        }
    }
    CHECK(found);
}

TEST_CASE("ui: headless captures show sprites and world text") {
    auto e = makeEngine();
    e->scene().environment().skyTop = {0, 0, 0, 1};
    e->scene().environment().skyHorizon = {0, 0, 0, 1};
    call(*e, "entity_create", parse(R"({"name": "Cam", "position": [0, 0, 10],
        "components": {"camera": {"orthographic": true, "orthoSize": 5}}})"));
    call(*e, "entity_create", parse(R"({"name": "Block", "components": {"sprite": {"size": [2, 2], "color": "#00ff00"}}})"));
    call(*e, "entity_create", parse(R"({"name": "Sign", "position": [0, 3, 0],
        "components": {"text": {"text": "SHOP", "size": 1.2, "color": "#ffffff"}}})"));
    CaptureOptions o;
    o.width = 200;
    o.height = 200;
    o.useSceneCamera = true;
    o.editorOverlays = false;
    auto cap = e->capture(o);
    REQUIRE(cap.ok());
    const uint8_t* center = cap->image.at(100, 100);
    CHECK(center[1] > 200);
    CHECK(center[0] < 40);
    int bright = 0;
    for (int y = 30; y < 50; ++y)
        for (int x = 60; x < 140; ++x) bright += cap->image.at(x, y)[0] > 200 ? 1 : 0;
    CHECK(bright > 40);
    bool sprite = false;
    for (const auto& v : cap->visible) {
        if (v.name == "Block") {
            sprite = true;
            CHECK(v.w == doctest::Approx(40.f).epsilon(0.05));  // 2 units of a 10-unit-tall view at 200 px
        }
    }
    CHECK(sprite);
}
