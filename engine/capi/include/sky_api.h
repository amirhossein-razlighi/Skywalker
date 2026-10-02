/*
 * Skywalker C API — the stable ABI boundary of the engine.
 *
 * Why a C API? It is the one interface every language can call without a custom
 * toolchain: Swift (the macOS editor), Python, Rust, C#, Zig, JavaScript (via FFI)...
 * It is intentionally narrow: most functionality is reached through `sky_call_tool`,
 * the exact same tool surface that agents use over MCP. Only latency-critical paths
 * (rendering, camera, picking, dragging, input) get dedicated functions.
 *
 * Ownership: strings returned as `char*` are heap-allocated and MUST be released with
 * sky_string_free(). Input strings are borrowed for the duration of the call.
 * Threading: all functions must be called from the thread that created the engine,
 * except where noted.
 */
#ifndef SKYWALKER_API_H
#define SKYWALKER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SkyEngine SkyEngine;

/* Lifetime ---------------------------------------------------------------- */
SkyEngine* sky_engine_create(const char* project_dir);
void sky_engine_destroy(SkyEngine* engine);
const char* sky_version(void); /* static string, do not free */
void sky_string_free(char* str);

/* Tools (same surface as MCP) --------------------------------------------- */
/* Returns an MCP CallToolResult JSON: {"content":[...],"isError":bool,"structuredContent":{...}} */
char* sky_call_tool(SkyEngine* engine, const char* name, const char* args_json, const char* actor);
/* Returns the MCP tools/list payload: {"tools":[{name,title,description,inputSchema,annotations}]} */
char* sky_tools_list(SkyEngine* engine);

/* Frame loop ---------------------------------------------------------------- */
/* Pumps cross-thread jobs (agent requests) and advances the simulation when playing. */
void sky_update(SkyEngine* engine, double seconds);
/* Renders the editor view into a CAMetalLayer (passed as void*). Returns 0 on success. */
int sky_render_to_layer(SkyEngine* engine, void* metal_layer, int width, int height);
/* Monotonic counter; changes whenever scene data changes (use to refresh UI lazily). */
uint64_t sky_scene_revision(SkyEngine* engine);
/* JSON array of activity events since the last call (edits, tool calls, logs, play state). */
char* sky_poll_events(SkyEngine* engine);
/* "editing" | "playing" | "paused" (static string) */
const char* sky_play_state(SkyEngine* engine);

/* Editor camera & interaction ----------------------------------------------- */
void sky_camera_orbit(SkyEngine* engine, float dyaw_degrees, float dpitch_degrees);
void sky_camera_pan(SkyEngine* engine, float dx, float dy); /* fractions of viewport height */
void sky_camera_zoom(SkyEngine* engine, float factor);      /* <1 zooms in */
uint64_t sky_pick(SkyEngine* engine, float x, float y, int width, int height); /* 0 = nothing */
void sky_set_selection(SkyEngine* engine, const uint64_t* ids, int count, const char* actor);
int sky_get_selection(SkyEngine* engine, uint64_t* out_ids, int capacity);   /* returns count */
void sky_drag_begin(SkyEngine* engine, uint64_t entity, float x, float y, int width, int height);
void sky_drag_update(SkyEngine* engine, float x, float y, int width, int height);
void sky_drag_end(SkyEngine* engine);

/* Transform gizmo: mode 0 = none, 1 = move, 2 = rotate, 3 = scale. ---------- */
void sky_gizmo_set_mode(SkyEngine* engine, int mode, int local_space);
int sky_gizmo_mode(SkyEngine* engine);
void sky_gizmo_set_snap(SkyEngine* engine, float translate_step, float rotate_step_degrees);
int sky_gizmo_hover(SkyEngine* engine, float x, float y, int width, int height); /* axis or -1 */
int sky_gizmo_begin(SkyEngine* engine, float x, float y, int width, int height); /* 1 if a handle was grabbed */
void sky_gizmo_drag(SkyEngine* engine, float x, float y, int width, int height, int snapping);
void sky_gizmo_end(SkyEngine* engine);

/* 1 = viewport looks through the scene's primary camera, 0 = editor orbit camera. */
void sky_set_view_scene_camera(SkyEngine* engine, int on);

/* Editor camera state for overlays: yaw, pitch (degrees). */
void sky_camera_angles(SkyEngine* engine, float* yaw, float* pitch);

/* Frame statistics: {"cpuMs":..,"draws":..,"lights":..,"entities":..,"renderer":".."} */
char* sky_frame_stats(SkyEngine* engine);

/* Game input (forwarded to Wander while playing) ---------------------------- */
void sky_input_key(SkyEngine* engine, const char* key, int down);
void sky_input_click(SkyEngine* engine, uint64_t entity);

/* Agent server: MCP over a Unix socket so external agents can attach. ------- */
/* socket_path may be NULL for the default (~/.skywalker/editor.sock). Returns 0 on success. */
int sky_agent_server_start(SkyEngine* engine, const char* socket_path);
void sky_agent_server_stop(SkyEngine* engine);

#ifdef __cplusplus
}
#endif

#endif /* SKYWALKER_API_H */
