// UI tools: build whole interfaces from one declarative JSON tree, restyle them, read back the
// computed layout, and drive widgets (click, set values, type) the same way players do.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/text/TextLayout.h"
#include "skywalker/ui/World2D.h"

namespace sky::tools {

namespace {

using namespace schema;
namespace fs = std::filesystem;

const std::vector<std::string>& widgetKinds() {
    static const std::vector<std::string> k{"panel", "image", "text", "button", "toggle", "slider", "progress", "scroll", "input", "spacer"};
    return k;
}

/// Sensible sizes per widget so trees can stay terse.
Json widgetDefaults(const std::string& w, const Json& node) {
    const bool hasSize = node.contains("size");
    Json d = Json::object();
    if (w == "text") {
        if (!hasSize && !node.contains("fit")) d["fit"] = "both";
        else if (hasSize && node.get("size").isArray() && node.get("size").size() == 2 && node.get("size")[1].asNumber() == 0 &&
                 !node.contains("fit"))
            d["fit"] = "height";
    } else if (w == "button") {
        if (!hasSize) d["size"] = Json::array({220, 52});
    } else if (w == "toggle") {
        if (!hasSize) d["size"] = Json::array({260, 36});
    } else if (w == "slider") {
        if (!hasSize) d["size"] = Json::array({260, 32});
    } else if (w == "progress") {
        if (!hasSize) d["size"] = Json::array({260, 14});
    } else if (w == "input") {
        if (!hasSize) d["size"] = Json::array({300, 46});
    } else if (w == "image") {
        if (!hasSize) d["size"] = Json::array({128, 128});
    } else if (w == "scroll") {
        if (!hasSize) d["size"] = Json::array({420, 320});
    } else if (w == "spacer") {
        if (!hasSize) d["size"] = Json::array({0, 0});
        if (!node.contains("flex")) d["flex"] = 1;
    } else if (w == "panel") {
        if (!hasSize && node.contains("children") && !node.contains("fit") && node.get("anchor").asString() != "fill") d["fit"] = "both";
    }
    return d;
}

std::string defaultName(const std::string& widget, const Json& node) {
    std::string t = text::stripTags(node.get("text").asString());
    t = str::trim(t);
    if ((widget == "button" || widget == "toggle") && !t.empty() && t.size() <= 32) return t;
    std::string n = widget;
    if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
    return n;
}

Status createNode(Scene& s, const Json& node, EntityId parent, Json& created, int depth) {
    if (depth > 32) return Error::make("invalid_arguments", "UI tree is nested too deeply (max 32 levels)");
    if (!node.isObject()) return Error::make("invalid_arguments", "each UI node must be an object like {\"type\": \"button\", \"text\": \"Play\"}");
    std::string widget = node.get("type").asString(node.get("widget").asString());
    if (widget.empty()) widget = node.contains("text") && !node.contains("children") ? "text" : "panel";
    if (std::find(widgetKinds().begin(), widgetKinds().end(), widget) == widgetKinds().end()) {
        std::string guess = str::closest(widget, widgetKinds(), 3);
        return Error::make("invalid_arguments", "unknown widget type \"" + widget + "\"",
                           guess.empty() ? "types: panel, image, text, button, toggle, slider, progress, scroll, input, spacer"
                                         : "did you mean \"" + guess + "\"?");
    }
    Json fields = widgetDefaults(widget, node);
    fields["widget"] = widget;
    Json css = Json::object();
    const auto& styleProps = ui::StyleSheet::propertyNames();
    for (const auto& [k, v] : node.members()) {
        if (k == "type" || k == "widget" || k == "name" || k == "children" || k == "tags" || k == "vars" || k == "components") continue;
        if (k == "css" || k == "styleOverrides") {
            if (Status st = ui::StyleSheet::validate(v, "css"); !st) return st;
            css.mergePatch(v);
            continue;
        }
        // Style properties may sit directly on a node ({"type": "text", "fontSize": 32, "color": "#fff"}).
        if (!UIElement::type().field(k) && std::find(styleProps.begin(), styleProps.end(), k) != styleProps.end()) {
            if (Status st = ui::StyleSheet::validate(Json::object({{k, v}}), "css"); !st) return st;
            css[k] = v;
            continue;
        }
        fields[k] = v;
    }
    if (!css.members().empty()) fields["styleOverrides"] = css;
    std::string name = node.get("name").asString(defaultName(widget, node));
    EntityId e = s.create(name, parent);
    if (Status st = s.patchComponent(e, "ui", fields); !st) {
        return Error::make(st.error().code, "\"" + name + "\": " + st.error().message, st.error().hint);
    }
    if (node.get("tags").isArray()) {
        std::vector<std::string> tags;
        for (const auto& t : node.get("tags").elements()) tags.push_back(t.asString());
        (void)s.setTags(e, tags);
    }
    if (node.get("vars").isObject()) (void)s.patchVars(e, node.get("vars"));
    for (const auto& [comp, patch] : node.get("components").members()) {
        if (Status st = s.patchComponent(e, comp, patch); !st) return st;
    }
    created.push(Json::object({{"id", e}, {"name", name}, {"widget", widget}}));
    for (const auto& child : node.get("children").elements()) {
        if (Status st = createNode(s, child, e, created, depth + 1); !st) return st;
    }
    return {};
}

/// Prebuilt UI trees agents can start from (then restyle and edit).
Result<Json> templateTree(const std::string& name) {
    const char* src = nullptr;
    if (name == "main_menu") {
        src = R"([{"type":"panel","name":"Main Menu","anchor":"center","layout":"column","gap":14,"align":"stretch","padding":40,"style":"card",
          "children":[
            {"type":"text","name":"Title","text":"Game Title","style":"title","textAlign":"center"},
            {"type":"text","name":"Subtitle","text":"A tagline for your game","style":"subtitle","css":{"textAlign":"center"}},
            {"type":"spacer","size":[0,18],"flex":0},
            {"type":"button","name":"Play","text":"Play","style":"primary large","size":[360,60]},
            {"type":"button","name":"Settings","text":"Settings","size":[360,52]},
            {"type":"button","name":"Quit","text":"Quit","style":"ghost","size":[360,52]}]}])";
    } else if (name == "hud") {
        src = R"([{"type":"panel","name":"Status","anchor":"top_left","position":[32,28],"layout":"column","gap":8,"style":"hud","padding":[12,16],
          "children":[
            {"type":"text","name":"Health Label","text":"HEALTH","style":"small muted"},
            {"type":"progress","name":"Health","value":0.8,"size":[260,14]},
            {"type":"text","name":"Energy Label","text":"ENERGY","style":"small muted"},
            {"type":"progress","name":"Energy","value":0.55,"size":[260,10],"css":{"accent":"#4fb3ff"}}]},
          {"type":"panel","name":"Score Box","anchor":"top_right","position":[-32,28],"style":"hud","layout":"row","gap":10,"padding":[10,16],
          "children":[{"type":"text","name":"Score","text":"Score  0","style":"large"}]},
          {"type":"text","name":"Prompt","anchor":"bottom","position":[0,-48],"text":"Press <b>E</b> to interact","style":"hud","visible":false}])";
    } else if (name == "pause_menu") {
        src = R"([{"type":"panel","name":"Pause Dim","anchor":"fill","css":{"background":"#00000099"},
          "children":[{"type":"panel","name":"Pause Menu","anchor":"center","layout":"column","gap":12,"align":"stretch","padding":32,"style":"card",
            "children":[
              {"type":"text","name":"Paused","text":"Paused","style":"heading","css":{"textAlign":"center"}},
              {"type":"button","name":"Resume","text":"Resume","style":"primary","size":[300,52]},
              {"type":"button","name":"Options","text":"Options","size":[300,48]},
              {"type":"button","name":"Main Menu","text":"Main Menu","style":"ghost","size":[300,48]}]}]}])";
    } else if (name == "settings") {
        src = R"([{"type":"panel","name":"Settings","anchor":"center","layout":"column","gap":16,"align":"stretch","padding":32,"style":"card","size":[520,0],"fit":"height",
          "children":[
            {"type":"text","name":"Settings Title","text":"Settings","style":"heading"},
            {"type":"text","text":"Music volume","style":"muted small"},
            {"type":"slider","name":"Music Volume","value":0.7},
            {"type":"text","text":"Effects volume","style":"muted small"},
            {"type":"slider","name":"Effects Volume","value":0.9},
            {"type":"toggle","name":"Fullscreen","text":"Fullscreen","value":1},
            {"type":"toggle","name":"Subtitles","text":"Subtitles","value":1},
            {"type":"panel","layout":"row","gap":12,"justify":"end","fit":"height","children":[
              {"type":"button","name":"Back","text":"Back","style":"ghost","size":[140,46]},
              {"type":"button","name":"Apply","text":"Apply","style":"primary","size":[140,46]}]}]}])";
    } else if (name == "inventory") {
        src = R"([{"type":"panel","name":"Inventory","anchor":"center","layout":"column","gap":14,"padding":24,"style":"card","fit":"both",
          "children":[
            {"type":"text","name":"Inventory Title","text":"Inventory","style":"heading"},
            {"type":"panel","name":"Slots","layout":"grid","columns":6,"gap":10,"fit":"both","children":[
              {"type":"button","name":"Slot 1","text":"","size":[72,72]},{"type":"button","name":"Slot 2","text":"","size":[72,72]},
              {"type":"button","name":"Slot 3","text":"","size":[72,72]},{"type":"button","name":"Slot 4","text":"","size":[72,72]},
              {"type":"button","name":"Slot 5","text":"","size":[72,72]},{"type":"button","name":"Slot 6","text":"","size":[72,72]},
              {"type":"button","name":"Slot 7","text":"","size":[72,72]},{"type":"button","name":"Slot 8","text":"","size":[72,72]},
              {"type":"button","name":"Slot 9","text":"","size":[72,72]},{"type":"button","name":"Slot 10","text":"","size":[72,72]},
              {"type":"button","name":"Slot 11","text":"","size":[72,72]},{"type":"button","name":"Slot 12","text":"","size":[72,72]}]}]}])";
    } else if (name == "document") {
        src = R"([{"type":"panel","name":"Document","anchor":"center","size":[760,860],"layout":"column","gap":14,"padding":[44,52],"style":"card","align":"stretch",
          "children":[
            {"type":"text","name":"Document Heading","text":"MINISTRY OF THE INTERIOR","style":"small muted","css":{"letterSpacing":0.2}},
            {"type":"text","name":"Document Title","text":"Decree No. 114","style":"heading"},
            {"type":"scroll","name":"Document Scroll","flex":1,"layout":"column","children":[
              {"type":"text","name":"Document Body","text":"Body text of the document. Rich text works: <i>italics</i>, <b>bold</b>.","size":[640,0],"css":{"lineSpacing":1.35}}]},
            {"type":"panel","layout":"row","gap":12,"justify":"end","fit":"height","children":[
              {"type":"button","name":"Reject","text":"Reject","style":"ghost","size":[160,48]},
              {"type":"button","name":"Sign","text":"Sign","style":"primary","size":[160,48]}]}]}])";
    } else if (name == "dialogue") {
        src = R"([{"type":"panel","name":"Dialogue Box","anchor":"bottom_stretch","margin":[0,120,40,120],"size":[0,236],"layout":"row","gap":24,
                   "padding":22,"align":"stretch","style":"dialogue_box","children":[
            {"type":"image","name":"Dialogue Portrait","size":[192,192],"style":"dialogue_portrait","visible":false},
            {"type":"panel","name":"Dialogue Body","flex":1,"layout":"column","gap":8,"align":"stretch","children":[
              {"type":"text","name":"Dialogue Speaker","fit":"height","style":"dialogue_name","text":"Speaker"},
              {"type":"text","name":"Dialogue Line","flex":1,"style":"dialogue_text","text":"The line being spoken appears here."},
              {"type":"text","name":"Dialogue Hint","fit":"height","style":"dialogue_hint","text":"Click or press Space"}]}]},
          {"type":"panel","name":"Dialogue Choices","anchor":"bottom_right","position":[-140,-300],"size":[560,0],"fit":"height","layout":"column",
           "gap":8,"align":"stretch","visible":false}])";
    }
    if (!src) {
        return Error::make("invalid_arguments", "unknown template \"" + name + "\"",
                           "templates: main_menu, hud, pause_menu, settings, inventory, document, dialogue");
    }
    return Json::parse(src).value();
}

Json nodeJson(const ui::Layout& lay, int index, int depth, std::string& outline) {
    const ui::Node& n = lay.nodes[static_cast<size_t>(index)];
    ui::Rect r = lay.screenRect(n);
    auto round1 = [](float v) { return std::round(v * 10.f) / 10.f; };
    Json j = Json::object({{"id", n.entity},
                           {"name", n.name},
                           {"widget", n.el.widget},
                           {"rect", Json::array({round1(r.x), round1(r.y), round1(r.w), round1(r.h)})},
                           {"visible", n.visible}});
    if (!n.interactable) j["interactable"] = false;
    if (!n.el.text.empty()) j["text"] = n.el.text;
    if (n.el.widget == "slider" || n.el.widget == "progress" || n.el.widget == "toggle") j["value"] = n.el.value;
    if (!n.classes.empty()) {
        Json c = Json::array();
        for (const auto& k : n.classes) c.push(k);
        j["style"] = c;
    }
    if (n.el.widget == "scroll") j["maxScroll"] = round1(n.maxScroll);
    char line[256];
    std::snprintf(line, sizeof(line), "%*s#%llu %s (%s) [%g, %g, %g, %g]%s%s%s\n", depth * 2, "",
                  static_cast<unsigned long long>(n.entity), n.name.c_str(), n.el.widget.c_str(), round1(r.x), round1(r.y),
                  round1(r.w), round1(r.h), n.visible ? "" : " hidden", n.el.text.empty() ? "" : " \"",
                  n.el.text.empty() ? "" : (text::stripTags(n.el.text).substr(0, 40) + "\"").c_str());
    outline += line;
    if (!n.children.empty()) {
        Json kids = Json::array();
        for (int c : n.children) kids.push(nodeJson(lay, c, depth + 1, outline));
        j["children"] = kids;
    }
    return j;
}

Json styleJson(const ui::Style& s) {
    return Json::object({{"background", reflect::toHexColor(s.background)},
                         {"color", reflect::toHexColor(s.color)},
                         {"font", s.font.empty() ? "Inter" : s.font},
                         {"fontSize", s.fontSize},
                         {"radius", s.radius},
                         {"borderWidth", s.borderWidth},
                         {"borderColor", reflect::toHexColor(s.borderColor)},
                         {"textAlign", s.textAlign},
                         {"opacity", s.opacity}});
}

/// The UI layout for an inspect/create result: the canvas's reference resolution unless given.
ui::Layout layoutFor(Engine& engine, const Json& a, EntityId canvas) {
    int w = static_cast<int>(a.get("width").asInt(0)), h = static_cast<int>(a.get("height").asInt(0));
    if (w <= 0 || h <= 0) {
        w = 1920;
        h = 1080;
        if (const UICanvas* c = canvas ? engine.scene().get<UICanvas>(canvas) : nullptr) {
            w = static_cast<int>(c->referenceResolution.x);
            h = static_cast<int>(c->referenceResolution.y);
        }
    }
    return engine.world2d().ui().computeLayout(engine.scene(), std::clamp(w, 16, 8192), std::clamp(h, 16, 8192));
}

EntityId canvasOf(const Scene& s, EntityId e) {
    for (EntityId cur = e; cur; cur = s.record(cur) ? s.record(cur)->parent : kNoEntity) {
        if (s.get<UICanvas>(cur)) return cur;
    }
    return kNoEntity;
}

}  // namespace

void addUiTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"ui_create", "Build UI",
             "Build a whole user interface (HUD, menu, dialog, inventory, document) from one declarative JSON tree in a single "
             "undoable step. Each node: {type: panel|image|text|button|toggle|slider|progress|scroll|input|spacer, name, text, "
             "image, style (classes, e.g. \"primary large\"), css (inline style), anchor (top_left, top, top_right, left, center, "
             "right, bottom_left, bottom, bottom_right, fill, top_stretch, bottom_stretch, left_stretch, right_stretch...), "
             "position [x,y] (offset, y down), size [w,h], margin, layout (row|column|grid), gap, padding (CSS order), align "
             "(start|center|end|stretch), justify (start|center|end|space_between), columns, fit (none|width|height|both), flex, "
             "value, event, children: [...]}. Text nodes size to their content by default; button names default to their text, "
             "so `on ui \"Play\"` works. Without `parent` a new screen canvas is made (configure it with `canvas`). Templates: "
             "main_menu, hud, pause_menu, settings, inventory, document, dialogue (the dialogue box the dialogue system uses). "
             "Returns created ids and the computed layout (rects in pixels at the canvas's reference resolution). Example: "
             "{\"root\": {\"type\": \"panel\", \"anchor\": \"center\", \"layout\": \"column\", \"gap\": 12, \"style\": \"card\", "
             "\"children\": [{\"type\": \"text\", \"text\": \"Paused\", \"style\": \"heading\"}, {\"type\": \"button\", \"text\": \"Resume\", "
             "\"style\": \"primary\"}]}}",
             "ui",
             object({{"root", Json::object({{"type", "object"}, {"description", "One UI tree (node with children)"}})},
                     {"elements", array(Json::object({{"type", "object"}}), "Several sibling trees")},
                     {"template", enumeration({"main_menu", "hud", "pause_menu", "settings", "inventory", "document", "dialogue"},
                                              "Start from a prebuilt tree (added after root/elements)")},
                     {"parent", entity("Canvas or UI element to add under (default: a new canvas)")},
                     {"canvas", Json::object({{"type", "object"},
                                              {"description", "New canvas: {name, mode: screen|world, theme: dark|light|parchment|glass|pixel, "
                                                              "referenceResolution, styleSheet, sortOrder, worldScale}"}})},
                     {"width", integer("Viewport width for the returned layout (default: reference resolution)")},
                     {"height", integer("Viewport height for the returned layout")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 std::vector<Json> trees;
                 if (a.contains("root")) trees.push_back(a.get("root"));
                 for (const auto& t : a.get("elements").elements()) trees.push_back(t);
                 if (a.contains("template")) {
                     auto t = templateTree(a.get("template").asString());
                     if (!t) return ToolResult::error(t.error());
                     for (const auto& n : t->elements()) trees.push_back(n);
                 }
                 if (trees.empty() && !a.contains("canvas")) {
                     return ToolResult::error(Error::make("invalid_arguments", "nothing to create", "pass root, elements or template"));
                 }
                 Json created = Json::array();
                 EntityId canvas = kNoEntity;
                 Status st = engine.edit(ctx.actor, "Create UI", [&]() -> Status {
                     Scene& s = engine.scene();
                     EntityId parent = kNoEntity;
                     if (a.contains("parent")) {
                         auto p = resolve(engine, a.get("parent"));
                         if (!p) return p.error();
                         parent = *p;
                         if (!s.get<UICanvas>(parent) && !s.get<UIElement>(parent)) {
                             return Error::make("invalid_arguments", "the parent must be a ui_canvas or a ui element",
                                                "omit parent to create a new canvas, or pass the canvas name");
                         }
                         canvas = canvasOf(s, parent);
                     } else {
                         Json cfg = a.get("canvas").isObject() ? a.get("canvas") : Json::object();
                         std::string name = cfg.get("name").asString("UI");
                         cfg.erase("name");
                         canvas = s.create(name);
                         if (Status r = s.patchComponent(canvas, "ui_canvas", cfg); !r) return r;
                         if (!cfg.get("mode").asString().empty() && cfg.get("mode").asString() == "world") {
                             (void)s.patchComponent(canvas, "transform", Json::object({{"position", Json::array({0, 2, 0})}}));
                         }
                         created.push(Json::object({{"id", canvas}, {"name", name}, {"widget", "canvas"}}));
                         parent = canvas;
                     }
                     for (const auto& t : trees) {
                         if (Status r = createNode(s, t, parent, created, 0); !r) return r;
                     }
                     return {};
                 });
                 if (!st) return fail(st);
                 ui::Layout lay = layoutFor(engine, a, canvas);
                 std::string outline;
                 Json layout = Json::array();
                 std::vector<EntityId> ids;
                 for (const auto& c : created.elements()) ids.push_back(static_cast<EntityId>(c.get("id").asInt()));
                 for (size_t i = 0; i < lay.nodes.size(); ++i) {
                     const ui::Node& n = lay.nodes[i];
                     bool isNew = std::find(ids.begin(), ids.end(), n.entity) != ids.end();
                     bool parentNew = n.parent >= 0 && std::find(ids.begin(), ids.end(), lay.nodes[static_cast<size_t>(n.parent)].entity) != ids.end();
                     if (isNew && !parentNew) layout.push(nodeJson(lay, static_cast<int>(i), 0, outline));
                 }
                 return ToolResult::json(Json::object({{"canvas", canvas}, {"created", created}, {"layout", layout},
                                                       {"viewport", Json::array({lay.width, lay.height})}}),
                                         "created " + std::to_string(created.size()) + " UI entities\n" + outline);
             }});

    reg.add({"ui_style", "Style UI",
             "Restyle a UI. Switch a canvas's built-in theme (dark, light, parchment, glass, pixel), write style rules to a "
             "style sheet (*.uistyle.json) the canvas uses — rules map selectors (widget kind \"button\", class \".primary\", "
             "entity \"#Title\", \"canvas\" for text defaults) to properties, with hover/pressed/focus/checked/disabled blocks "
             "and $vars — or set one element's classes / inline css. Properties: background, background2 (gradient), "
             "backgroundImage, slice (9-slice), radius, borderWidth, borderColor, shadowColor, shadowOffset, shadowBlur, opacity, "
             "padding, font, fontSize, color, textAlign, verticalAlign, bold, italic, lineSpacing, letterSpacing, textTransform, "
             "textOutline, textOutlineColor, textShadowColor, textShadowOffset, accent, track, knob, trackHeight, knobSize, "
             "placeholderColor, imageFit, imageTint, transition. Example: {\"canvas\": \"HUD\", \"path\": \"ui/game.uistyle.json\", "
             "\"vars\": {\"accent\": \"#e8a33d\"}, \"rules\": {\"button\": {\"radius\": 4, \"hover\": {\"background\": \"$accent\"}}}}",
             "ui",
             object({{"canvas", entity("Canvas to restyle")},
                     {"theme", enumeration(ui::StyleSheet::themeNames(), "Built-in theme for the canvas")},
                     {"path", string("Style sheet file to write/merge (project-relative, *.uistyle.json)")},
                     {"rules", Json::object({{"type", "object"}, {"description", "selector -> properties"}})},
                     {"vars", Json::object({{"type", "object"}, {"description", "variables referenced as $name"}})},
                     {"element", entity("Element to restyle")},
                     {"style", string("Element classes (replaces), e.g. \"primary large\"")},
                     {"css", Json::object({{"type", "object"}, {"description", "Element inline style (merged; null deletes a key)"}})},
                     {"list", boolean("Only list themes, properties and the canvas's selectors")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 if (a.get("list").asBool(false)) {
                     Json themes = Json::array(), props = Json::array();
                     for (const auto& t : ui::StyleSheet::themeNames()) themes.push(t);
                     for (const auto& p : ui::StyleSheet::propertyNames()) props.push(p);
                     Json out = Json::object({{"themes", themes}, {"properties", props}});
                     if (a.contains("canvas")) {
                         auto c = resolve(engine, a.get("canvas"));
                         if (!c) return ToolResult::error(c.error());
                         if (const UICanvas* uc = s.get<UICanvas>(*c)) {
                             auto sheet = ui::StyleSheet::theme(uc->theme);
                             Json sel = Json::array();
                             for (const auto& [k, v] : sheet->rules().members()) sel.push(k);
                             out["selectors"] = sel;
                             out["vars"] = sheet->vars();
                         }
                     }
                     return ToolResult::json(out, "UI styling reference");
                 }
                 Json changed = Json::array();
                 Status st = engine.edit(ctx.actor, "Style UI", [&]() -> Status {
                     if (a.contains("canvas")) {
                         auto c = resolve(engine, a.get("canvas"));
                         if (!c) return c.error();
                         if (!s.get<UICanvas>(*c)) return Error::make("invalid_arguments", "\"" + s.record(*c)->name + "\" is not a ui_canvas");
                         Json patch = Json::object();
                         if (a.contains("theme")) patch["theme"] = a.get("theme");
                         if (a.contains("rules") || a.contains("vars")) {
                             std::string path = a.get("path").asString(s.get<UICanvas>(*c)->styleSheet);
                             if (path.empty()) path = "ui/" + s.record(*c)->name + ".uistyle.json";
                             if (fs::path(path).extension() != ".json") {
                                 return Error::make("invalid_arguments", "style sheets are .json files (e.g. ui/game.uistyle.json)");
                             }
                             std::string full = engine.resolvePath(path);
                             Json doc = Json::object({{"format", "skywalker.uistyle"}, {"rules", Json::object()}, {"vars", Json::object()}});
                             if (std::ifstream in(full); in) {
                                 std::stringstream ss;
                                 ss << in.rdbuf();
                                 if (auto parsed = Json::parse(ss.str()); parsed && parsed->isObject()) doc = parsed.value();
                             }
                             Json rules = doc.get("rules").isObject() ? doc.get("rules") : Json::object();
                             for (const auto& [sel, props] : a.get("rules").members()) {
                                 if (Status v = ui::StyleSheet::validate(props, "rules." + sel); !v) return v;
                                 Json merged = rules.get(sel).isObject() ? rules.get(sel) : Json::object();
                                 merged.mergePatch(props);
                                 rules[sel] = merged;
                             }
                             doc["rules"] = rules;
                             Json vars = doc.get("vars").isObject() ? doc.get("vars") : Json::object();
                             vars.mergePatch(a.get("vars"));
                             doc["vars"] = vars;
                             auto check = ui::StyleSheet::parse(doc, ui::StyleSheet::theme(s.get<UICanvas>(*c)->theme));
                             if (!check) return check.error();
                             std::error_code ec;
                             fs::create_directories(fs::path(full).parent_path(), ec);
                             std::ofstream out(full);
                             if (!out) return Error::make("io_error", "cannot write " + path);
                             out << doc.dump(2) << "\n";
                             out.close();
                             engine.world2d().invalidate(full);
                             patch["styleSheet"] = path;
                             changed.push(path);
                         }
                         if (!patch.members().empty()) {
                             if (Status r = s.patchComponent(*c, "ui_canvas", patch); !r) return r;
                             changed.push(s.record(*c)->name);
                         }
                     }
                     if (a.contains("element")) {
                         auto e = resolve(engine, a.get("element"));
                         if (!e) return e.error();
                         UIElement* el = s.get<UIElement>(*e);
                         if (!el) return Error::make("invalid_arguments", "\"" + s.record(*e)->name + "\" is not a ui element");
                         Json patch = Json::object();
                         if (a.contains("style")) patch["style"] = a.get("style");
                         if (a.contains("css")) {
                             if (Status v = ui::StyleSheet::validate(a.get("css"), "css"); !v) return v;
                             Json css = el->styleOverrides.isObject() ? el->styleOverrides : Json::object();
                             css.mergePatch(a.get("css"));
                             patch["styleOverrides"] = css;
                         }
                         if (Status r = s.patchComponent(*e, "ui", patch); !r) return r;
                         changed.push(s.record(*e)->name);
                     }
                     if (changed.size() == 0) return Error::make("invalid_arguments", "nothing to change", "pass canvas+theme/rules or element+style/css");
                     return {};
                 });
                 if (!st) return fail(st);
                 return ToolResult::json(Json::object({{"changed", changed}}), "restyled");
             }});

    reg.add({"ui_inspect", "Inspect UI",
             "Read the computed UI layout: every element's rect [x, y, w, h] in pixels for a viewport (default: the canvas's "
             "reference resolution), visibility, text, values and style classes, as a tree. Use it to verify a layout without a "
             "screenshot, find what to click, or debug overlap. With `element`, also returns its resolved style.",
             "ui",
             object({{"canvas", entity("Only this canvas")},
                     {"element", entity("Only this element (and its children); includes its resolved style")},
                     {"width", integer("Viewport width (default: reference resolution)")},
                     {"height", integer("Viewport height")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 Scene& s = engine.scene();
                 EntityId only = kNoEntity, canvas = kNoEntity;
                 if (a.contains("element")) {
                     auto e = resolve(engine, a.get("element"));
                     if (!e) return ToolResult::error(e.error());
                     only = *e;
                     canvas = canvasOf(s, only);
                 }
                 if (a.contains("canvas")) {
                     auto c = resolve(engine, a.get("canvas"));
                     if (!c) return ToolResult::error(c.error());
                     canvas = *c;
                 }
                 ui::Layout lay = layoutFor(engine, a, canvas);
                 Json canvases = Json::array();
                 std::string outline;
                 for (size_t ci = 0; ci < lay.canvases.size(); ++ci) {
                     const ui::CanvasInfo& c = lay.canvases[ci];
                     if (canvas && c.entity != canvas) continue;
                     Json roots = Json::array();
                     char head[200];
                     std::snprintf(head, sizeof(head), "canvas #%llu %s (%s, theme %s, scale %.3g)\n", static_cast<unsigned long long>(c.entity),
                                   s.record(c.entity)->name.c_str(), c.canvas.mode.c_str(), c.canvas.theme.c_str(), c.scale);
                     outline += head;
                     for (size_t i = 0; i < lay.nodes.size(); ++i) {
                         const ui::Node& n = lay.nodes[i];
                         if (n.canvas != static_cast<int>(ci)) continue;
                         bool start = only ? n.entity == only : n.parent < 0;
                         if (start) roots.push(nodeJson(lay, static_cast<int>(i), 1, outline));
                     }
                     canvases.push(Json::object({{"id", c.entity}, {"name", s.record(c.entity)->name}, {"mode", c.canvas.mode},
                                                 {"theme", c.canvas.theme}, {"scale", c.scale}, {"elements", roots}}));
                 }
                 Json out = Json::object({{"viewport", Json::array({lay.width, lay.height})}, {"canvases", canvases}});
                 if (only) {
                     if (const ui::Node* n = lay.find(only)) out["style"] = styleJson(n->style);
                 }
                 if (canvases.size() == 0) outline = "no UI canvases (create one with ui_create)\n";
                 return ToolResult::json(out, outline);
             }});

    reg.add({"ui_interact", "Use UI",
             "Use a widget the way a player would: click a button (sends Wander `on ui \"<name>\"` and `on click` on the next "
             "tick while playing), toggle, set a slider/progress value, type into an input, scroll a scroll view, or click at a "
             "pixel (hit-tested like a mouse). While editing, value changes are recorded edits and clicks only report which "
             "events would fire. Example: {\"element\": \"Play\", \"action\": \"click\"} then sim_control step.",
             "ui",
             object({{"element", entity("Element to use")},
                     {"at", Json::object({{"type", "array"}, {"items", Json::object({{"type", "number"}})},
                                          {"description", "Pixel [x, y] to click instead of an element (hit test)"}})},
                     {"width", integer("Viewport width for `at` (default 1920)")},
                     {"height", integer("Viewport height for `at` (default 1080)")},
                     {"action", enumeration({"click", "set_value", "type", "scroll", "focus"}, "What to do (default click)")},
                     {"value", number("set_value: new value; scroll: offset in pixels")},
                     {"text", string("type: text to put in an input (replaces unless append)")},
                     {"append", boolean("type: append instead of replace")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 Scene& s = engine.scene();
                 EntityId target = kNoEntity;
                 if (a.contains("element")) {
                     auto e = resolve(engine, a.get("element"));
                     if (!e) return ToolResult::error(e.error());
                     target = *e;
                 } else if (a.get("at").isArray() && a.get("at").size() == 2) {
                     int w = static_cast<int>(a.get("width").asInt(1920)), h = static_cast<int>(a.get("height").asInt(1080));
                     ViewCamera cam;
                     bool hasCam = sceneCamera(s, cam);
                     target = engine.world2d().ui().hitTest(s, a.get("at")[size_t{0}].asFloat(), a.get("at")[1].asFloat(), w, h,
                                                            hasCam ? &cam : nullptr, true);
                     if (!target) {
                         return ToolResult::json(Json::object({{"hit", Json()}}), "nothing interactive at that point");
                     }
                 } else {
                     return ToolResult::error(Error::make("invalid_arguments", "pass element or at: [x, y]"));
                 }
                 UIElement* el = s.get<UIElement>(target);
                 if (!el) return ToolResult::error(Error::make("invalid_arguments", "\"" + s.record(target)->name + "\" is not a ui element"));
                 const std::string action = a.get("action").asString("click");
                 const std::string name = s.record(target)->name;
                 const bool playing = engine.playState() != PlayState::Editing;
                 Json events = Json::array();
                 auto apply = [&](const Json& patch) -> Status {
                     if (playing) return s.patchComponent(target, "ui", patch);
                     return engine.edit(ctx.actor, "UI " + action + " " + name, [&] { return s.patchComponent(target, "ui", patch); });
                 };
                 if (action == "click") {
                     if (!el->interactable) return ToolResult::error(Error::make("not_interactable", "\"" + name + "\" is disabled (interactable: false)"));
                     if (playing) {
                         ui::UiEvents ev;
                         ev.emit = [&](const std::string& n, EntityId t) {
                             engine.runtime().emit(n, t);
                             events.push(n);
                         };
                         ev.click = [&](EntityId e) { engine.input().clicked.push_back(e); };
                         ev.activate = [&](EntityId e) {
                             const EntityRecord* rec = s.record(e);
                             if (!rec || !rec->vars.contains("_dialogueChoice")) return false;
                             auto runner = static_cast<EntityId>(rec->vars.get("_dialogueRunner").asInt());
                             (void)engine.world2d().chooseDialogue(s, runner, static_cast<int>(rec->vars.get("_dialogueChoice").asInt()),
                                                                   &engine.runtime());
                             events.push("dialogue:choice");
                             return true;
                         };
                         engine.world2d().ui().activate(s, target, ev);
                     } else {
                         events.push("ui:" + name);
                         if (!el->event.empty()) events.push(el->event);
                         if (el->widget == "toggle") {
                             if (Status st = apply(Json::object({{"value", el->value > 0.5f ? 0 : 1}})); !st) return fail(st);
                         }
                     }
                 } else if (action == "set_value") {
                     if (!a.contains("value")) return ToolResult::error(Error::make("invalid_arguments", "set_value needs value"));
                     if (Status st = apply(Json::object({{"value", a.get("value")}})); !st) return fail(st);
                     if (playing) {
                         engine.runtime().emit("ui:" + name);
                         events.push("ui:" + name);
                     }
                 } else if (action == "type") {
                     std::string t = a.get("append").asBool(false) ? el->text + a.get("text").asString() : a.get("text").asString();
                     if (Status st = apply(Json::object({{"text", t}})); !st) return fail(st);
                 } else if (action == "scroll") {
                     if (Status st = apply(Json::object({{"scroll", std::max(0.f, a.get("value").asFloat())}})); !st) return fail(st);
                 } else if (action == "focus") {
                     engine.world2d().ui().setFocus(target);
                 }
                 const UIElement* now = s.get<UIElement>(target);
                 Json out = Json::object({{"element", target}, {"name", name}, {"action", action}, {"events", events},
                                          {"value", now ? now->value : 0.f}, {"text", now ? now->text : std::string()},
                                          {"playing", playing}});
                 std::string summary = action + " " + name;
                 if (events.size()) summary += playing ? " -> events delivered next tick" : " (editing: events fire only while playing)";
                 return ToolResult::json(out, summary);
             }});
}

}  // namespace sky::tools
