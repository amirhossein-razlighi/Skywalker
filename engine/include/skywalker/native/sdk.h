/* Skywalker native module SDK.
 *
 * Native modules are C++ files in a project's `native/` folder, written by people or
 * agents, compiled by the engine (`native_build`) into a library that is loaded at play
 * start. Use them for hot inner loops and for any C++ library (Homebrew packages via
 * pkg-config, see native/module.json). They can:
 *   - read and write entities and components (typed accessors and reflection),
 *   - spawn / destroy entities, emit events, read input, time and seeded randomness,
 *   - register Wander builtins (callable from every behavior) and per-tick systems.
 *
 * The ABI is plain C (a function table passed at load), so modules never link against
 * the engine and keep working across engine builds with the same SKY_SDK_VERSION.
 *
 * SECURITY: native modules are trusted local code running inside the engine process with
 * your user's permissions. Review code (especially agent-written) like any other program
 * before building it. Agents need the mutating "code" tools to build them.
 *
 * Minimal module:
 *
 *   #include "skywalker/native/sdk.h"
 *
 *   static void fib(SkyCall* call, const SkyValue* args, int argc, SkyValue* result, void*) {
 *       double n = sky_arg_number(call, args, 0), a = 0, b = 1;
 *       for (int i = 0; i < (int)n; ++i) { double t = a + b; a = b; b = t; }
 *       *result = sky_number(a);
 *   }
 *
 *   SKY_MODULE_EXPORT int sky_module_init(const SkyApi* api, SkyModule* module) {
 *       SKY_SDK_INIT(api);
 *       SkyBuiltinDesc d = {"fib", "n-th Fibonacci number", "math", "fib(20)", 1, 1, "n: number", "number", fib, 0};
 *       return api->register_builtin(module, &d);
 *   }
 */
#ifndef SKYWALKER_NATIVE_SDK_H
#define SKYWALKER_NATIVE_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "skywalker/native/value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SKY_SDK_VERSION 1

#if defined(_WIN32)
#define SKY_MODULE_EXPORT __declspec(dllexport)
#else
#define SKY_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef uint64_t SkyEntity; /* 0 = no entity */
typedef struct SkyWorld SkyWorld;   /* the running scene (opaque) */
typedef struct SkyModule SkyModule; /* this module (opaque) */
typedef struct SkyCall SkyCall;     /* a builtin call in progress (opaque) */

/* A Wander builtin implemented in C/C++. Write the return value to *result (it starts as
 * none). Arguments are borrowed: do not release them. To fail, call api->call_fail and
 * return immediately. */
typedef void (*SkyBuiltinFn)(SkyCall* call, const SkyValue* args, int argc, SkyValue* result, void* user);
/* A per-tick system: runs every fixed tick (1/60 s) while playing, after Wander behaviors. */
typedef void (*SkySystemFn)(SkyWorld* world, double dt, void* user);

typedef struct SkyBuiltinDesc {
    const char* name;      /* snake_case, e.g. "flock_step" */
    const char* doc;       /* one sentence for wander_reference */
    const char* category;  /* e.g. "math", "ai", "physics" */
    const char* example;   /* e.g. "flock_step(self, 0.5)" */
    int min_args;
    int max_args;          /* -1 = any number */
    const char* params;    /* optional, for docs and type checks: "a: number, b: vec|entity, c?: string" */
    const char* returns;   /* optional type name: "number", "vec", "entity|none", ... */
    SkyBuiltinFn fn;
    void* user;
} SkyBuiltinDesc;

typedef struct SkyApi {
    uint32_t version; /* SKY_SDK_VERSION */
    uint32_t size;    /* sizeof(SkyApi) */

    /* --- registration (only inside sky_module_init) --------------------------------- */
    int (*register_builtin)(SkyModule* module, const SkyBuiltinDesc* desc); /* 0 = ok */
    int (*register_system)(SkyModule* module, const char* name, SkySystemFn fn, void* user);

    /* --- logging & errors -------------------------------------------------------------- */
    void (*log)(SkyWorld* w, const char* text);
    /* Message of the last failed call on this world ("" if none). */
    const char* (*last_error)(SkyWorld* w);

    /* --- builtin calls ---------------------------------------------------------------- */
    SkyWorld* (*call_world)(SkyCall* call);
    SkyEntity (*call_self)(SkyCall* call);
    void (*call_fail)(SkyCall* call, const char* message); /* aborts the calling handler after return */

    /* --- time, randomness, input -------------------------------------------------------- */
    double (*time)(SkyWorld* w);
    double (*dt)(SkyWorld* w);
    uint64_t (*frame)(SkyWorld* w);
    double (*random)(SkyWorld* w); /* seeded 0..1 (replays exactly) */
    int (*key_held)(SkyWorld* w, const char* key);

    /* --- entities ---------------------------------------------------------------------- */
    SkyEntity (*find)(SkyWorld* w, const char* name_or_id);
    int (*exists)(SkyWorld* w, SkyEntity e);
    size_t (*entity_count)(SkyWorld* w);
    SkyEntity (*entity_at)(SkyWorld* w, size_t index); /* scene order */
    size_t (*find_tagged)(SkyWorld* w, const char* tag, SkyEntity* out, size_t capacity); /* returns the total */
    int (*has_tag)(SkyWorld* w, SkyEntity e, const char* tag);
    const char* (*name)(SkyWorld* w, SkyEntity e); /* valid until the next call */
    SkyEntity (*spawn)(SkyWorld* w, const char* mesh_or_prefab, const float position[3], const char* name);
    void (*destroy)(SkyWorld* w, SkyEntity e); /* deferred to the end of the tick */

    /* --- transforms (local to the parent) ------------------------------------------------ */
    int (*get_position)(SkyWorld* w, SkyEntity e, float out[3]);
    int (*set_position)(SkyWorld* w, SkyEntity e, const float v[3]);
    int (*get_rotation)(SkyWorld* w, SkyEntity e, float out[3]); /* Euler degrees */
    int (*set_rotation)(SkyWorld* w, SkyEntity e, const float v[3]);
    int (*get_scale)(SkyWorld* w, SkyEntity e, float out[3]);
    int (*set_scale)(SkyWorld* w, SkyEntity e, const float v[3]);
    int (*world_position)(SkyWorld* w, SkyEntity e, float out[3]);

    /* --- components through reflection (same names as Wander: "light", "intensity") ----- */
    int (*has_component)(SkyWorld* w, SkyEntity e, const char* component);
    int (*get_field)(SkyWorld* w, SkyEntity e, const char* component, const char* field, SkyValue* out); /* out: owned */
    int (*set_field)(SkyWorld* w, SkyEntity e, const char* component, const char* field, const SkyValue* v);
    /* Whole component as JSON. Returns the length needed (excluding NUL); -1 on error. */
    int (*get_component_json)(SkyWorld* w, SkyEntity e, const char* component, char* buffer, size_t capacity);
    int (*patch_component_json)(SkyWorld* w, SkyEntity e, const char* component, const char* json_patch);

    /* --- entity vars (the same vars Wander behaviors use) --------------------------------- */
    int (*get_var)(SkyWorld* w, SkyEntity e, const char* name, SkyValue* out); /* out: owned */
    int (*set_var)(SkyWorld* w, SkyEntity e, const char* name, const SkyValue* v);

    /* --- events ------------------------------------------------------------------------- */
    /* Delivered next tick. target 0 = broadcast. payload: a value (or null for none). */
    void (*emit)(SkyWorld* w, const char* name, SkyEntity target, const SkyValue* payload, SkyEntity other);

    /* --- values --------------------------------------------------------------------------- */
    SkyValue (*string_new)(const char* utf8);
    const char* (*string_chars)(const SkyValue* v); /* NULL if not a string */
    SkyValue (*list_new)(void);
    size_t (*list_length)(const SkyValue* list);
    SkyValue (*list_get)(const SkyValue* list, size_t index); /* owned copy */
    void (*list_push)(SkyValue* list, const SkyValue* item);
    SkyValue (*map_new)(void);
    SkyValue (*map_get)(const SkyValue* map, const char* key); /* owned copy; none if missing */
    void (*map_set)(SkyValue* map, const char* key, const SkyValue* item);
    SkyValue (*copy)(const SkyValue* v); /* owned copy (retains) */
    void (*release)(SkyValue* v);        /* releases an owned value and sets it to none */
} SkyApi;

/* Exported by every module. Return 0 on success (anything else: the load fails). */
typedef int (*SkyModuleInitFn)(const SkyApi* api, SkyModule* module);
/* Optional: called before the library is unloaded (hot reload, engine shutdown). */
typedef void (*SkyModuleShutdownFn)(void);

#ifdef __cplusplus
}
#endif

/* --- convenience helpers ------------------------------------------------------------------ */
#ifdef __cplusplus
inline const SkyApi* sky_sdk_api = nullptr; /* one per module library, shared by all its files */
#else
static const SkyApi* sky_sdk_api = 0; /* C: call SKY_SDK_INIT in each file that uses the helpers */
#endif
#define SKY_SDK_INIT(api_ptr) \
    do { \
        if (!(api_ptr) || (api_ptr)->version != SKY_SDK_VERSION) return -1; \
        sky_sdk_api = (api_ptr); \
    } while (0)

static inline double sky_arg_number(SkyCall* call, const SkyValue* args, int i) {
    if (args[i].type == SKY_NUMBER) return args[i].as.number;
    if (args[i].type == SKY_BOOL) return args[i].as.boolean ? 1.0 : 0.0;
    sky_sdk_api->call_fail(call, "expected a number argument");
    return 0;
}
static inline SkyEntity sky_arg_entity(SkyCall* call, const SkyValue* args, int i) {
    if (args[i].type == SKY_ENTITY) return args[i].as.entity;
    sky_sdk_api->call_fail(call, "expected an entity argument");
    return 0;
}

#endif /* SKYWALKER_NATIVE_SDK_H */
