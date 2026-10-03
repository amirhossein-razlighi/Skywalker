/* Skywalker AOT ABI: the contract between the engine and Wander behaviors compiled to
 * native code by `wander_compile_native`.
 *
 * Generated code is a direct translation of a program's register bytecode: arithmetic,
 * comparisons, loops and branches run inline on SkyValues; every other instruction
 * (property access, builtins, calls, waits, ...) calls back into the host's `exec`, which
 * executes it with the VM's own implementation. Native and interpreted runs are
 * therefore identical by construction, including errors and the execution budget.
 *
 * The engine embeds this header in each generated file, so compiling needs nothing but
 * a system C++ compiler. Bump SKY_AOT_VERSION on any change.
 */
#ifndef SKYWALKER_NATIVE_AOT_H
#define SKYWALKER_NATIVE_AOT_H

#include "skywalker/native/value.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SKY_AOT_VERSION 1

typedef struct SkyAotFrame {
    void* exec;            /* host execution state (opaque) */
    SkyValue* R;           /* registers of this call */
    const SkyValue* K;     /* program constants */
    int64_t* budget;       /* remaining execution budget (shared with the VM) */
    void* outcome;         /* host outcome (opaque) */
    void* error;           /* set by the host when an instruction failed */
    int32_t proto;         /* proto index */
    uint32_t pc;           /* start pc (0, or a resume point after a wait) */
} SkyAotFrame;

typedef struct SkyAotApi {
    uint32_t version; /* SKY_AOT_VERSION */
    uint32_t size;    /* sizeof(SkyAotApi) */
    /* Executes instruction `pc` with VM semantics. Returns the next pc, or -1 when the run
     * ended (return, wait, go to, stop) or failed; the native function must then return. */
    int64_t (*exec)(SkyAotFrame* f, uint32_t pc);
    /* Truthiness of any value (entities: whether they still exist). */
    int (*truthy)(SkyAotFrame* f, const SkyValue* v);
    /* Records "execution budget exceeded" at pc; always returns -1. */
    int64_t (*budget_exceeded)(SkyAotFrame* f, uint32_t pc);
    /* Releases a heap value (refcount reached zero). */
    void (*free_object)(SkyObject* o);
} SkyAotApi;

/* One compiled proto. Returns when the run ended (always 0). */
typedef int (*SkyAotFn)(SkyAotFrame* f);

/* Exported by every generated library:
 *   int sky_aot_init(const SkyAotApi* api, uint64_t* programHash, uint32_t* protoCount,
 *                    const SkyAotFn** table);
 * returns SKY_AOT_VERSION. table[i] may be null (that proto stays interpreted). */
typedef int (*SkyAotInitFn)(const SkyAotApi* api, uint64_t* programHash, uint32_t* protoCount, const SkyAotFn** table);

#ifdef __cplusplus
}
#endif

#ifdef SKY_AOT_GENERATED
/* Helpers for generated code. */
static const SkyAotApi* sky_api;

static inline void sky_release(SkyValue* v) {
    if (v->type >= SKY_STRING) {
        SkyObject* o = v->as.object;
        if (--o->refcount == 0) sky_api->free_object(o);
    }
}
static inline void sky_set_number(SkyValue* r, double x) {
    sky_release(r);
    r->type = SKY_NUMBER;
    r->as.number = x;
}
static inline void sky_set_bool(SkyValue* r, int b) {
    sky_release(r);
    r->type = SKY_BOOL;
    r->as.number = 0;
    r->as.boolean = b ? 1u : 0u;
}
static inline void sky_set_none(SkyValue* r) {
    sky_release(r);
    r->type = SKY_NONE;
    r->as.number = 0;
}
static inline int sky_truthy(SkyAotFrame* f, const SkyValue* v) {
    switch (v->type) {
        case SKY_NONE: return 0;
        case SKY_BOOL: return v->as.boolean != 0;
        case SKY_NUMBER: return v->as.number != 0;
        default: return sky_api->truthy(f, v);
    }
}
#endif /* SKY_AOT_GENERATED */

#endif /* SKYWALKER_NATIVE_AOT_H */
