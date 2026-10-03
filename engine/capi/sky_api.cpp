#include "sky_api.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "skywalker/core/Log.h"
#include "skywalker/engine/Engine.h"

using namespace sky;

struct SkyEngine {
    std::unique_ptr<Engine> engine;
};

namespace {

char* dup(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

Engine* E(SkyEngine* h) { return h ? h->engine.get() : nullptr; }

}  // namespace

extern "C" {

SkyEngine* sky_engine_create(const char* project_dir) {
    EngineConfig cfg;
    if (project_dir) cfg.projectDir = project_dir;
    auto handle = std::make_unique<SkyEngine>();
    handle->engine = std::make_unique<Engine>(cfg);
    return handle.release();  // ownership passes to the caller; freed in sky_engine_destroy
}

void sky_engine_destroy(SkyEngine* engine) { delete engine; }  // NOLINT(cppcoreguidelines-owning-memory)

const char* sky_version(void) { return SKY_VERSION_STRING; }

void sky_string_free(char* str) { std::free(str); }

char* sky_call_tool(SkyEngine* h, const char* name, const char* args_json, const char* actor) {
    Engine* e = E(h);
    if (!e || !name) return dup(R"({"content":[{"type":"text","text":"invalid engine or tool name"}],"isError":true})");
    Json args = Json::object();
    if (args_json && *args_json) {
        auto parsed = Json::parse(args_json);
        if (!parsed) return dup(ToolResult::error(parsed.error()).toMcp().dump());
        args = std::move(parsed.value());
    }
    return dup(e->callTool(name, args, actor ? actor : "user").toMcp().dump());
}

struct SkyPendingCall {
    Engine::PendingCall call;
};

char* sky_call_tool_begin(SkyEngine* h, const char* name, const char* args_json, const char* actor, SkyPendingCall** pending) {
    if (pending) *pending = nullptr;
    Engine* e = E(h);
    if (!e || !name) return dup(R"({"content":[{"type":"text","text":"invalid engine or tool name"}],"isError":true})");
    Json args = Json::object();
    if (args_json && *args_json) {
        auto parsed = Json::parse(args_json);
        if (!parsed) return dup(ToolResult::error(parsed.error()).toMcp().dump());
        args = std::move(parsed.value());
    }
    Engine::PendingCall call = e->beginTool(name, args, actor ? actor : "user");
    if (!call.result.deferred || !pending) return dup(e->finishTool(call).toMcp().dump());
    *pending = new SkyPendingCall{std::move(call)};  // NOLINT(cppcoreguidelines-owning-memory)
    return nullptr;
}

void sky_pending_run(SkyPendingCall* p) {
    if (!p || !p->call.result.deferred || !p->call.result.deferred->work) return;
    try {
        p->call.result.deferred->work();
    } catch (...) {
        // The failure surfaces in finish(): the tool's own handling decides what to report.
    }
}

char* sky_pending_finish(SkyEngine* h, SkyPendingCall* p) {
    Engine* e = E(h);
    if (!p) return dup(R"({"content":[{"type":"text","text":"no pending call"}],"isError":true})");
    std::unique_ptr<SkyPendingCall> owner(p);
    if (!e) return dup(R"({"content":[{"type":"text","text":"invalid engine"}],"isError":true})");
    return dup(e->finishTool(owner->call).toMcp().dump());
}

char* sky_tools_list(SkyEngine* h) {
    Engine* e = E(h);
    return dup(e ? e->tools().listJson().dump() : "{\"tools\":[]}");
}

void sky_update(SkyEngine* h, double seconds) {
    if (Engine* e = E(h)) e->update(seconds);
}

int sky_render_to_layer(SkyEngine* h, void* layer, int width, int height) {
    Engine* e = E(h);
    if (!e || !layer || width <= 0 || height <= 0) return -1;
    Status s = e->renderToSurface(layer, width, height);
    return s.ok() ? 0 : 1;
}

uint64_t sky_scene_revision(SkyEngine* h) {
    Engine* e = E(h);
    return e ? e->scene().revision() : 0;
}

char* sky_poll_events(SkyEngine* h) {
    Engine* e = E(h);
    Json arr = Json::array();
    if (e) {
        for (auto& ev : e->drainEvents()) arr.push(std::move(ev));
    }
    return dup(arr.dump());
}

const char* sky_play_state(SkyEngine* h) {
    Engine* e = E(h);
    return e ? toString(e->playState()) : "editing";
}

void sky_camera_orbit(SkyEngine* h, float dyaw, float dpitch) {
    if (Engine* e = E(h)) e->camera().orbit(dyaw, dpitch);
}
void sky_camera_pan(SkyEngine* h, float dx, float dy) {
    if (Engine* e = E(h)) e->camera().pan(dx, dy);
}
void sky_camera_zoom(SkyEngine* h, float factor) {
    if (Engine* e = E(h)) e->camera().zoom(factor);
}

uint64_t sky_pick(SkyEngine* h, float x, float y, int width, int height) {
    Engine* e = E(h);
    return e ? e->pickAt(x, y, width, height) : 0;
}

void sky_set_selection(SkyEngine* h, const uint64_t* ids, int count, const char* actor) {
    Engine* e = E(h);
    if (!e) return;
    std::vector<EntityId> v;
    for (int i = 0; i < count && ids; ++i) v.push_back(ids[i]);
    e->setSelection(std::move(v), actor ? actor : "user");
}

int sky_get_selection(SkyEngine* h, uint64_t* out, int capacity) {
    Engine* e = E(h);
    if (!e) return 0;
    const auto& sel = e->selection();
    int n = 0;
    for (EntityId id : sel) {
        if (n >= capacity || !out) break;
        out[n++] = id;
    }
    return static_cast<int>(sel.size());
}

void sky_drag_begin(SkyEngine* h, uint64_t entity, float x, float y, int w, int hgt) {
    if (Engine* e = E(h)) e->beginDrag(entity, x, y, w, hgt);
}
void sky_drag_update(SkyEngine* h, float x, float y, int w, int hgt) {
    if (Engine* e = E(h)) e->dragTo(x, y, w, hgt);
}
void sky_drag_end(SkyEngine* h) {
    if (Engine* e = E(h)) e->endDrag();
}

void sky_gizmo_set_mode(SkyEngine* h, int mode, int local) {
    if (Engine* e = E(h)) {
        e->gizmo().mode = static_cast<GizmoMode>(std::clamp(mode, 0, 3));
        e->gizmo().local = local != 0;
    }
}

int sky_gizmo_mode(SkyEngine* h) {
    Engine* e = E(h);
    return e ? static_cast<int>(e->gizmo().mode) : 0;
}

void sky_gizmo_set_snap(SkyEngine* h, float translate, float rotateDeg) {
    if (Engine* e = E(h)) {
        e->gizmo().snap = translate;
        e->gizmo().rotateSnapDeg = rotateDeg > 0 ? rotateDeg : 15.f;
    }
}

int sky_gizmo_hover(SkyEngine* h, float x, float y, int w, int hgt) {
    Engine* e = E(h);
    return e ? e->gizmoHover(x, y, w, hgt) : -1;
}

int sky_gizmo_begin(SkyEngine* h, float x, float y, int w, int hgt) {
    Engine* e = E(h);
    return e && e->gizmoBegin(x, y, w, hgt) ? 1 : 0;
}

void sky_gizmo_drag(SkyEngine* h, float x, float y, int w, int hgt, int snapping) {
    if (Engine* e = E(h)) e->gizmoDrag(x, y, w, hgt, snapping != 0);
}

void sky_gizmo_end(SkyEngine* h) {
    if (Engine* e = E(h)) e->gizmoEnd();
}

void sky_set_view_scene_camera(SkyEngine* h, int on) {
    if (Engine* e = E(h)) e->setViewThroughSceneCamera(on != 0);
}

void sky_camera_angles(SkyEngine* h, float* yaw, float* pitch) {
    Engine* e = E(h);
    if (!e) return;
    if (yaw) *yaw = e->camera().yaw;
    if (pitch) *pitch = e->camera().pitch;
}

char* sky_frame_stats(SkyEngine* h) {
    Engine* e = E(h);
    if (!e) return dup("{}");
    const auto& s = e->stats();
    RendererInfo ri = e->renderer().info();
    return dup(Json::object({{"cpuMs", s.cpuMs},
                             {"draws", s.draws},
                             {"lights", s.lights},
                             {"entities", s.entities},
                             {"renderer", ri.backend + " · " + ri.device}})
                   .dump());
}

void sky_input_key(SkyEngine* h, const char* key, int down) {
    Engine* e = E(h);
    if (!e || !key) return;
    std::string k = key;
    if (down) {
        if (!e->input().held.count(k)) e->input().pressed.insert(k);
        e->input().held.insert(k);
    } else {
        e->input().held.erase(k);
    }
}

void sky_input_click(SkyEngine* h, uint64_t entity) {
    if (Engine* e = E(h)) e->input().clicked.push_back(entity);
}

int sky_agent_server_start(SkyEngine* h, const char* path) {
    Engine* e = E(h);
    if (!e) return -1;
    Status s = e->startAgentServer(path ? path : Engine::defaultSocketPath());
    if (!s) log::error("agent", s.error().message);
    return s.ok() ? 0 : 1;
}

void sky_agent_server_stop(SkyEngine* h) {
    if (Engine* e = E(h)) e->stopAgentServer();
}

}  // extern "C"
