#include "skywalker/ui/World2D.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"

namespace sky {

World2D::World2D(std::string projectDir)
    : assets_(std::make_unique<render2d::Assets2D>(std::move(projectDir))), ui_(std::make_unique<ui::UiSystem>(*assets_)) {}

World2D::~World2D() = default;

void World2D::setProjectDir(const std::string& dir) {
    assets_->setProjectDir(dir);
    scripts_.clear();
}

void World2D::invalidate(const std::string& absolutePath) {
    assets_->invalidate(absolutePath);
    scripts_.erase(absolutePath);
}

void World2D::setViewport(int width, int height) {
    viewW_ = std::max(1, width);
    viewH_ = std::max(1, height);
}

void World2D::reset() {
    conversations_.clear();
    ui_->reset();
    pointerWasDown_ = false;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

float World2D::adjustView(const Scene& scene, EntityId camera, ViewCamera& view, int width, int height) {
    return render2d::applyCamera2D(scene, camera, view, width, height);
}

void World2D::gather(const Scene& scene, FrameData& frame, const std::vector<EntityId>& selection, float time, float pixelSnap) {
    render2d::Gather2DOptions o;
    o.time = time;
    o.selection = selection;
    o.pixelSnap = pixelSnap;
    render2d::gather2D(scene, *assets_, frame, o);
    ui_->setSelection(selection);
    ui_->build(scene, frame, time);
}

EntityId World2D::pick(const Scene& scene, const FrameData& frame, float x, float y, bool ui) const {
    if (ui) return ui_->hitTest(scene, x, y, frame.width, frame.height, &frame.camera, false);
    for (const ScreenBox& b : frame.render2d.boxes) {  // topmost first
        if (!b.ui && x >= b.x && y >= b.y && x < b.x + b.w && y < b.y + b.h) return b.entity;
    }
    return kNoEntity;
}

void World2D::refineVisible(const FrameData& frame, std::vector<VisibleEntity>& visible, const Scene& scene) const {
    const float W = static_cast<float>(frame.width), H = static_cast<float>(frame.height);
    std::unordered_map<EntityId, const ScreenBox*> boxes;
    for (const ScreenBox& b : frame.render2d.boxes) boxes.emplace(b.entity, &b);
    std::unordered_map<EntityId, bool> listed;
    for (VisibleEntity& v : visible) {
        listed[v.id] = true;
        auto it = boxes.find(v.id);
        if (it == boxes.end()) continue;
        const ScreenBox& b = *it->second;
        v.x = b.x;
        v.y = b.y;
        v.w = b.w;
        v.h = b.h;
        v.depth = b.ui ? 0.f : b.depth;
        v.coverage = b.w * b.h / std::max(1.f, W * H);
    }
    for (const ScreenBox& b : frame.render2d.boxes) {
        if (listed.count(b.entity) || !scene.exists(b.entity)) continue;
        listed[b.entity] = true;
        visible.push_back({b.entity, scene.record(b.entity)->name, b.x, b.y, b.w, b.h, b.ui ? 0.f : b.depth,
                           b.w * b.h / std::max(1.f, W * H)});
    }
    std::stable_sort(visible.begin(), visible.end(), [](const VisibleEntity& a, const VisibleEntity& b) { return a.depth < b.depth; });
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

void World2D::preview(Scene& scene, float dt) { render2d::tickAnimators(scene, *assets_, dt, nullptr); }

void World2D::onPlay(Scene& scene, wander::Runtime& runtime) {
    for (EntityId e : scene.entities()) {
        const DialogueRunner* d = scene.get<DialogueRunner>(e);
        if (!d || !d->autoStart || !scene.isActive(e)) continue;
        if (Status s = startDialogue(scene, e, d->startNode, &runtime); !s) log::warn("dialogue", s.error().message);
    }
}

void World2D::postTick(Scene& scene, wander::Runtime& runtime, float dt) {
    render2d::tickAnimators(scene, *assets_, dt, [&](EntityId e, const std::string& ev) { runtime.emit(ev, e); });
    render2d::tickCameras(scene, dt);
}

void World2D::preTick(Scene& scene, wander::InputState& input, wander::Runtime& runtime, float dt) {
    ui::UiInput in;
    in.width = viewW_;
    in.height = viewH_;
    in.x = input.mouseX * static_cast<float>(viewW_);  // normalized 0..1, origin top-left
    in.y = input.mouseY * static_cast<float>(viewH_);
    in.down = input.mouseHeld.count("left") != 0;
    in.wheel = input.scrollY;
    in.keys.assign(input.pressed.begin(), input.pressed.end());
    in.text = input.text;
    // World canvases are hit through the game camera.
    ViewCamera cam;
    const ViewCamera* camPtr = nullptr;
    if (sceneCamera(scene, cam)) {
        int w = in.width > 0 ? in.width : 1920, h = in.height > 0 ? in.height : 1080;
        render2d::applyCamera2D(scene, render2d::activeCamera(scene), cam, w, h);
        camPtr = &cam;
    }
    bool choiceMade = false;
    ui::UiEvents ev;
    ev.emit = [&](const std::string& name, EntityId target) { runtime.emit(name, target); };
    ev.click = [&](EntityId e) { input.clicked.push_back(e); };
    ev.activate = [&](EntityId e) {
        const EntityRecord* rec = scene.record(e);
        if (!rec || !rec->vars.contains("_dialogueChoice")) return false;
        auto runner = static_cast<EntityId>(rec->vars.get("_dialogueRunner").asInt());
        int index = static_cast<int>(rec->vars.get("_dialogueChoice").asInt());
        if (Status s = chooseDialogue(scene, runner, index, &runtime); !s) log::warn("dialogue", s.error().message);
        choiceMade = true;
        return true;
    };
    ui_->tick(scene, in, dt, camPtr, ev);
    input.text.clear();

    // Conversations: waits, typewriter, advancing with a click / space / enter, number keys for choices.
    const bool released = pointerWasDown_ && !in.down;
    pointerWasDown_ = in.down;
    const bool advanceKey = input.pressed.count("space") || input.pressed.count("enter") || input.pressed.count("return");
    std::vector<EntityId> ids;
    for (const auto& [e, c] : conversations_) ids.push_back(e);
    std::sort(ids.begin(), ids.end());  // deterministic order
    for (EntityId e : ids) {
        auto it = conversations_.find(e);
        if (it == conversations_.end()) continue;
        Conversation& c = it->second;
        const DialogueRunner* comp = scene.get<DialogueRunner>(e);
        if (!comp) {
            conversations_.erase(it);
            continue;
        }
        auto store = vars(scene, e);
        auto evs = events(scene, e, &runtime);
        if (c.runner->state() == dialogue::Runner::State::Waiting) (void)c.runner->update(dt, store, evs);
        const int total = text::visibleLength(c.runner->line().text);
        if (c.runner->state() == dialogue::Runner::State::Line || c.runner->state() == dialogue::Runner::State::Choices) {
            c.shown = comp->typewriter <= 0.f ? static_cast<float>(total) : std::min(static_cast<float>(total), c.shown + dt * comp->typewriter);
        }
        const bool overUi = ui_->hovered() != kNoEntity;
        const bool advance = !choiceMade && ((released && !overUi) || advanceKey);
        if (advance) {
            if (typing(scene, e, c)) {
                c.shown = static_cast<float>(total);
            } else if (c.runner->state() == dialogue::Runner::State::Line) {
                if (Status s = advanceDialogue(scene, e, &runtime); !s) log::warn("dialogue", s.error().message);
            }
        }
        if (c.runner->state() == dialogue::Runner::State::Choices && !typing(scene, e, c)) {
            for (int k = 1; k <= 9; ++k) {
                if (input.pressed.count(std::to_string(k))) {
                    (void)chooseDialogue(scene, e, k - 1, &runtime);
                    break;
                }
            }
        }
        if (auto again = conversations_.find(e); again != conversations_.end()) {
            syncState(scene, e, again->second);
            updateDefaultUi(scene, e, again->second, &runtime);
            finishIfEnded(scene, e, again->second, &runtime);
        }
    }
}

// ---------------------------------------------------------------------------
// Dialogue
// ---------------------------------------------------------------------------

EntityId World2D::dialogueRunner(const Scene& scene, EntityId preferred) const {
    if (preferred && scene.get<DialogueRunner>(preferred)) return preferred;
    for (EntityId e : scene.entities()) {
        if (scene.get<DialogueRunner>(e)) return e;
    }
    return kNoEntity;
}

Result<std::shared_ptr<const dialogue::Script>> World2D::script(const DialogueRunner& runner) {
    std::string key, source;
    int64_t mtime = 0;
    if (!runner.script.empty()) {
        key = assets_->resolve(runner.script);
        mtime = render2d::fileMTime(key);
        if (mtime < 0) return Error::make("not_found", "cannot open dialogue script " + runner.script);
        if (auto it = scripts_.find(key); it != scripts_.end() && it->second.mtime == mtime) return it->second.script;
        std::ifstream f(key);
        std::stringstream ss;
        ss << f.rdbuf();
        source = ss.str();
    } else {
        if (runner.source.empty()) return Error::make("no_script", "the dialogue component has neither script nor source");
        key = "inline:" + std::to_string(std::hash<std::string>{}(runner.source));
        source = runner.source;
        if (auto it = scripts_.find(key); it != scripts_.end() && it->second.source == source) return it->second.script;
    }
    auto parsed = dialogue::parse(source);
    if (scripts_.size() > 256) scripts_.clear();
    scripts_[key] = {mtime, runner.script.empty() ? source : std::string(), parsed};
    return parsed;
}

dialogue::VarStore World2D::vars(Scene& scene, EntityId e) {
    dialogue::VarStore v;
    v.get = [&scene, e](const std::string& name) -> Json {
        const EntityRecord* r = scene.record(e);
        return r ? r->vars.get(name) : Json();
    };
    v.set = [&scene, e](const std::string& name, const Json& value) {
        if (EntityRecord* r = scene.record(e)) r->vars[name] = value;
        scene.markDirty();
    };
    return v;
}

dialogue::RunnerEvents World2D::events(Scene& scene, EntityId e, wander::Runtime* runtime) {
    dialogue::RunnerEvents ev;
    ev.command = [&scene, e, runtime](const std::string& cmd, const std::vector<std::string>& args) {
        if (EntityRecord* r = scene.record(e)) {
            std::string joined;
            for (const auto& a : args) joined += (joined.empty() ? "" : " ") + a;
            r->vars["dialogue_command"] = cmd;
            r->vars["dialogue_args"] = joined;
            r->vars["dialogue_arg"] = args.empty() ? std::string() : args[0];
        }
        if (runtime) runtime->emit("dialogue:" + cmd);
    };
    ev.node = [runtime](const std::string&) { (void)runtime; };
    return ev;
}

bool World2D::typing(const Scene& scene, EntityId e, const Conversation& c) const {
    const DialogueRunner* comp = scene.get<DialogueRunner>(e);
    if (!comp || comp->typewriter <= 0.f) return false;
    return c.shown + 0.001f < static_cast<float>(text::visibleLength(c.runner->line().text)) &&
           (c.runner->state() == dialogue::Runner::State::Line || c.runner->state() == dialogue::Runner::State::Choices);
}

Status World2D::startDialogue(Scene& scene, EntityId runnerEntity, const std::string& node, wander::Runtime* runtime) {
    EntityId e = dialogueRunner(scene, runnerEntity);
    if (!e) return Error::make("no_dialogue", "no entity has a dialogue component", "add {\"dialogue\": {\"script\": \"story.dialogue\"}} to an entity");
    const DialogueRunner* comp = scene.get<DialogueRunner>(e);
    auto s = script(*comp);
    if (!s) return s.error();
    if (!s.value()->ok()) {
        for (const auto& d : s.value()->diagnostics) {
            if (d.error) return Error::make("dialogue_error", "line " + std::to_string(d.line) + ": " + d.message, d.hint);
        }
    }
    Conversation c;
    c.runner = std::make_unique<dialogue::Runner>(s.value(), scene.seed * 2654435761u + e);
    std::string start = node.empty() ? comp->startNode : node;
    auto store = vars(scene, e);
    auto evs = events(scene, e, runtime);
    if (runtime) runtime->emit("dialogue:start");
    Status st = c.runner->start(start, store, evs);
    if (!st) return st;
    auto& slot = conversations_[e] = std::move(c);
    if (runtime && slot.runner->state() == dialogue::Runner::State::Line) runtime->emit("dialogue:line");
    syncState(scene, e, slot);
    updateDefaultUi(scene, e, slot, runtime);
    finishIfEnded(scene, e, slot, runtime);
    return {};
}

Status World2D::advanceDialogue(Scene& scene, EntityId runnerEntity, wander::Runtime* runtime) {
    EntityId e = dialogueRunner(scene, runnerEntity);
    auto it = conversations_.find(e);
    if (it == conversations_.end()) return Error::make("not_running", "no conversation is running", "start one with start_dialogue(\"Start\")");
    Conversation& c = it->second;
    if (typing(scene, e, c)) {
        c.shown = static_cast<float>(text::visibleLength(c.runner->line().text));
    } else {
        auto store = vars(scene, e);
        auto evs = events(scene, e, runtime);
        if (Status s = c.runner->advance(store, evs); !s) return s;
        if (c.runner->state() == dialogue::Runner::State::Line) {
            c.shown = 0;  // a new line types in; choices keep showing the line before them
            ++c.lineSerial;
        }
        if (runtime && c.runner->state() == dialogue::Runner::State::Line) runtime->emit("dialogue:line");
    }
    syncState(scene, e, c);
    updateDefaultUi(scene, e, c, runtime);
    finishIfEnded(scene, e, c, runtime);
    return {};
}

Status World2D::chooseDialogue(Scene& scene, EntityId runnerEntity, int index, wander::Runtime* runtime) {
    EntityId e = dialogueRunner(scene, runnerEntity);
    auto it = conversations_.find(e);
    if (it == conversations_.end()) return Error::make("not_running", "no conversation is running");
    Conversation& c = it->second;
    std::string chosen = index >= 0 && index < static_cast<int>(c.runner->choices().size())
                             ? c.runner->choices()[static_cast<size_t>(index)].text
                             : std::string();
    auto store = vars(scene, e);
    auto evs = events(scene, e, runtime);
    if (Status s = c.runner->choose(index, store, evs); !s) return s;
    if (EntityRecord* r = scene.record(e)) {
        r->vars["dialogue_choice"] = chosen;
        r->vars["dialogue_choice_index"] = index;
    }
    if (runtime) runtime->emit("dialogue:choice");
    if (c.runner->state() == dialogue::Runner::State::Line) {
        c.shown = 0;
        ++c.lineSerial;
        if (runtime) runtime->emit("dialogue:line");
    }
    syncState(scene, e, c);
    updateDefaultUi(scene, e, c, runtime);
    finishIfEnded(scene, e, c, runtime);
    return {};
}

Status World2D::stopDialogue(Scene& scene, EntityId runnerEntity, wander::Runtime* runtime) {
    EntityId e = dialogueRunner(scene, runnerEntity);
    auto it = conversations_.find(e);
    if (it == conversations_.end()) return {};
    it->second.runner->stop();
    finishIfEnded(scene, e, it->second, runtime);
    return {};
}

void World2D::syncState(Scene& scene, EntityId e, Conversation& c) {
    DialogueRunner* comp = scene.get<DialogueRunner>(e);
    if (!comp) return;
    using S = dialogue::Runner::State;
    S st = c.runner->state();
    comp->running = st != S::Ended && st != S::Idle;
    comp->node = c.runner->node();
    comp->speaker = st == S::Line || st == S::Choices ? c.runner->line().speaker : std::string();
    comp->line = st == S::Line || st == S::Choices ? c.runner->line().text : std::string();
    comp->tags = st == S::Line || st == S::Choices ? c.runner->line().tags : Json::object();
    Json choices = Json::array();
    if (st == S::Choices) {
        for (const auto& ch : c.runner->choices()) choices.push(ch.text);
    }
    comp->choices = choices;
    scene.markDirty();
}

void World2D::finishIfEnded(Scene& scene, EntityId e, Conversation& c, wander::Runtime* runtime) {
    if (c.runner->state() != dialogue::Runner::State::Ended) return;
    if (DialogueRunner* comp = scene.get<DialogueRunner>(e)) {
        comp->running = false;
        comp->speaker.clear();
        comp->line.clear();
        comp->choices = Json::array();
        comp->tags = Json::object();
        if (comp->ui == "default") hideDefaultUi(scene);
    }
    conversations_.erase(e);
    if (runtime) runtime->emit("dialogue:end");
}

namespace {

EntityId findChild(const Scene& scene, EntityId parent, const std::string& name) {
    for (EntityId c : scene.children(parent)) {
        if (scene.record(c)->name == name) return c;
    }
    return kNoEntity;
}

EntityId ensure(Scene& scene, EntityId parent, const std::string& name, const char* uiJson) {
    if (EntityId e = findChild(scene, parent, name)) return e;
    EntityId e = scene.create(name, parent);
    (void)scene.patchComponent(e, "ui", Json::parse(uiJson).value());
    return e;
}

}  // namespace

void World2D::hideDefaultUi(Scene& scene) {
    EntityId canvas = scene.find("Dialogue UI");
    if (!canvas || !scene.get<UICanvas>(canvas)) return;
    for (const char* part : {"Dialogue Box", "Dialogue Choices"}) {
        if (EntityId p = findChild(scene, canvas, part)) {
            if (UIElement* el = scene.get<UIElement>(p)) el->visible = false;
        }
    }
}

void World2D::updateDefaultUi(Scene& scene, EntityId e, Conversation& c, wander::Runtime*) {
    const DialogueRunner* comp = scene.get<DialogueRunner>(e);
    if (!comp || comp->ui != "default") return;
    using S = dialogue::Runner::State;
    const S st = c.runner->state();
    // Build (or find) the dialogue UI; agents can restyle or move every part (ui_create template "dialogue").
    EntityId canvas = scene.find("Dialogue UI");
    if (!canvas || !scene.get<UICanvas>(canvas)) {
        canvas = scene.create("Dialogue UI");
        (void)scene.patchComponent(canvas, "ui_canvas", Json::object({{"sortOrder", 100}}));
    }
    EntityId box = ensure(scene, canvas, "Dialogue Box",
                          R"({"widget":"panel","anchor":"bottom_stretch","margin":[0,120,40,120],"size":[0,236],
                              "layout":"row","gap":24,"padding":22,"align":"stretch","style":"dialogue_box"})");
    EntityId portrait = ensure(scene, box, "Dialogue Portrait", R"({"widget":"image","size":[192,192],"style":"dialogue_portrait","visible":false})");
    EntityId body = ensure(scene, box, "Dialogue Body", R"({"widget":"panel","flex":1,"layout":"column","gap":8,"align":"stretch"})");
    EntityId speaker = ensure(scene, body, "Dialogue Speaker", R"({"widget":"text","fit":"height","style":"dialogue_name"})");
    EntityId line = ensure(scene, body, "Dialogue Line", R"({"widget":"text","flex":1,"style":"dialogue_text"})");
    EntityId hint = ensure(scene, body, "Dialogue Hint", R"({"widget":"text","fit":"height","style":"dialogue_hint","text":"Click or press Space"})");
    EntityId choices = ensure(scene, canvas, "Dialogue Choices",
                              R"({"widget":"panel","anchor":"bottom_right","position":[-140,-300],"size":[560,0],"fit":"height",
                                  "layout":"column","gap":8,"align":"stretch","visible":false})");
    auto el = [&](EntityId id) { return scene.get<UIElement>(id); };
    const bool showing = st == S::Line || st == S::Choices;
    const bool typingNow = typing(scene, e, c);
    if (UIElement* b = el(box)) b->visible = showing || st == S::Waiting;
    if (UIElement* s = el(speaker)) {
        s->text = c.runner->line().speaker;
        s->visible = !s->text.empty();
    }
    if (UIElement* l = el(line)) l->text = c.runner->line().text;
    if (typingNow) ui_->setReveal(line, static_cast<int>(c.shown));
    else ui_->clearReveal(line);
    if (UIElement* p = el(portrait)) {
        const Json& tag = c.runner->line().tags.get("portrait");
        p->visible = tag.isString() && !tag.asString().empty();
        if (p->visible) p->image = (comp->portraits.empty() ? std::string() : comp->portraits + "/") + tag.asString() + ".png";
    }
    if (UIElement* h = el(hint)) h->visible = st == S::Line && !typingNow;
    const bool pick = st == S::Choices && !typingNow;
    if (UIElement* ch = el(choices)) ch->visible = pick;
    if (pick) {
        const auto& list = c.runner->choices();
        std::vector<EntityId> buttons = scene.children(choices);
        for (size_t i = 0; i < std::max(list.size(), buttons.size()); ++i) {
            EntityId bt = i < buttons.size() ? buttons[i] : kNoEntity;
            if (i >= list.size()) {
                if (UIElement* u = el(bt)) u->visible = false;
                continue;
            }
            if (!bt) {
                bt = scene.create("Choice " + std::to_string(i + 1), choices);
                (void)scene.patchComponent(bt, "ui", Json::parse(R"({"widget":"button","size":[0,44],"fit":"height","style":"dialogue_choice"})").value());
            }
            if (UIElement* u = el(bt)) {
                u->visible = true;
                u->text = std::to_string(i + 1) + ".  " + list[i].text;
            }
            if (EntityRecord* r = scene.record(bt)) {
                r->vars["_dialogueChoice"] = static_cast<int>(i);
                r->vars["_dialogueRunner"] = e;
            }
        }
    }
    scene.markDirty();
}

Json World2D::dialogueState(const Scene& scene, EntityId runnerEntity) const {
    EntityId e = dialogueRunner(scene, runnerEntity);
    Json out = Json::object({{"entity", e}});
    auto it = conversations_.find(e);
    if (it == conversations_.end()) {
        out["running"] = false;
        return out;
    }
    const auto& r = *it->second.runner;
    static const char* names[] = {"idle", "line", "choices", "waiting", "ended"};
    out["running"] = true;
    out["state"] = names[static_cast<int>(r.state())];
    out["node"] = r.node();
    out["speaker"] = r.line().speaker;
    out["line"] = r.line().text;
    out["tags"] = r.line().tags;
    Json ch = Json::array();
    for (const auto& c : r.choices()) ch.push(c.text);
    out["choices"] = ch;
    out["typing"] = typing(scene, e, it->second);
    return out;
}

// ---------------------------------------------------------------------------
// Wander builtins
// ---------------------------------------------------------------------------

namespace {

Result<EntityId> entityArg(const Scene& scene, const Json& v, const char* what) {
    if (const Json* id = v.find("$entity")) {
        auto e = static_cast<EntityId>(id->asInt());
        if (scene.exists(e)) return e;
        return Error::make("invalid_argument", std::string(what) + " refers to an entity that no longer exists");
    }
    if (v.isString()) {
        if (EntityId e = scene.find(v.asString())) return e;
        return Error::make("invalid_argument", std::string(what) + ": no entity named \"" + v.asString() + "\"");
    }
    return Error::make("invalid_argument", std::string(what) + " must be an entity");
}

bool cellAt(const Scene& scene, EntityId map, const Tilemap& tm, const Json& pos, int& cx, int& cy) {
    Vec3 p;
    if (!reflect::jsonToVec3(pos, p)) return false;
    Vec3 local = scene.worldMatrix(map).inverse().transformPoint(p);
    const float cell = std::max(1e-4f, tm.cellSize);
    cx = static_cast<int>(std::floor(local.x / cell));
    cy = static_cast<int>(std::floor(-local.y / cell));
    return true;
}

}  // namespace

Result<Json> World2D::callBuiltin(Scene& scene, wander::Runtime& runtime, const std::string& fn, const std::vector<Json>& a, EntityId self) {
    (void)self;
    auto str = [&](size_t i) { return i < a.size() && a[i].isString() ? a[i].asString() : std::string(); };
    if (fn == "play_anim") {
        auto e = entityArg(scene, a[0], "play_anim() argument 1");
        if (!e) return e.error();
        if (Status s = render2d::playAnimation(scene, *assets_, *e, str(1), a.size() > 2 && a[2].asBool(false)); !s) return s.error();
        return Json();
    }
    if (fn == "start_dialogue") {
        EntityId runner = kNoEntity;
        std::string node = str(0);
        if (a.size() == 2) {
            auto e = entityArg(scene, a[0], "start_dialogue() argument 1");
            if (!e) return e.error();
            runner = *e;
            node = str(1);
        }
        if (Status s = startDialogue(scene, runner, node, &runtime); !s) return s.error();
        return Json();
    }
    if (fn == "dialogue_advance") {
        if (Status s = advanceDialogue(scene, kNoEntity, &runtime); !s) return s.error();
        return Json();
    }
    if (fn == "dialogue_choose") {
        if (Status s = chooseDialogue(scene, kNoEntity, static_cast<int>(a[0].asInt()), &runtime); !s) return s.error();
        return Json();
    }
    if (fn == "dialogue_var") {
        EntityId e = dialogueRunner(scene);
        if (!e) return Error::make("no_dialogue", "no entity has a dialogue component");
        std::string name = str(0);
        if (!name.empty() && name[0] == '$') name.erase(0, 1);
        EntityRecord* r = scene.record(e);
        if (a.size() > 1) {
            r->vars[name] = a[1];
            return a[1];
        }
        return r->vars.get(name);
    }
    if (fn == "tile_at" || fn == "set_tile") {
        auto e = entityArg(scene, a[0], (fn + "() argument 1").c_str());
        if (!e) return e.error();
        Tilemap* tm = scene.get<Tilemap>(*e);
        if (!tm) return Error::make("invalid_argument", fn + "(): the entity has no tilemap");
        int cx = 0, cy = 0;
        if (!cellAt(scene, *e, *tm, a[1], cx, cy)) return Error::make("invalid_argument", fn + "(): argument 2 must be a position");
        auto grid = tiles::Grid::fromComponent(*tm);
        if (!grid) return grid.error();
        const size_t layerArg = fn == "tile_at" ? 2 : 3;
        std::string layerName = str(layerArg);
        int layer = -1;
        if (!layerName.empty()) {
            layer = grid->layerIndex(layerName, fn == "set_tile");
            if (layer < 0) return Error::make("invalid_argument", fn + "(): no layer \"" + layerName + "\"");
        }
        if (fn == "tile_at") {
            if (!grid->inside(cx, cy)) return Json(0);
            if (layer >= 0) return Json(static_cast<double>(grid->get(static_cast<size_t>(layer), cx, cy) & tiles::kIdMask));
            for (size_t l = grid->layers.size(); l-- > 0;) {  // topmost non-empty
                if (uint32_t id = grid->get(l, cx, cy) & tiles::kIdMask) return Json(static_cast<double>(id));
            }
            return Json(0);
        }
        if (!grid->inside(cx, cy)) return Error::make("invalid_argument", "set_tile(): position is outside the map");
        if (layer < 0) layer = grid->layers.empty() ? grid->layerIndex("ground", true) : 0;
        if (a[2].isString()) {
            int count = 0;
            if (!tm->tileset.empty()) {
                if (auto ts = assets_->tileset(tm->tileset, tm->tileSize); ts) count = ts->count;
            }
            auto terrains = tiles::parseTerrains(tm->autotile, count);
            if (!terrains) return terrains.error();
            auto it = terrains->find(a[2].asString());
            if (it == terrains->end()) return Error::make("invalid_argument", "set_tile(): no terrain \"" + a[2].asString() + "\" in tilemap.autotile");
            grid->paintTerrain(static_cast<size_t>(layer), it->second, cx, cy, static_cast<uint32_t>(scene.seed));
        } else {
            grid->set(static_cast<size_t>(layer), cx, cy, static_cast<uint32_t>(std::max(0.0, a[2].asNumber())));
        }
        grid->writeTo(*tm);
        return Json();
    }
    return Error::make("unknown_function", "unknown function " + fn);
}

}  // namespace sky
