/* Skywalker native ABI: the value type shared by Wander, AOT-compiled behaviors and native
 * modules.
 *
 * This header is C (and C++) compatible and has no dependencies, so generated code and
 * project modules compile with nothing but a system compiler. The layout is part of the
 * stable ABI (SKY_ABI_VERSION): the engine's own `wander::Value` is layout-identical and
 * static_asserts it.
 *
 * Ownership: strings, lists and maps are reference-counted heap objects. A SkyValue that
 * holds an object owns one reference. Never copy such a value with plain assignment; use
 * the host API (`retain`/`release`, or the C++ helpers in sdk.h) instead. Numbers, bools,
 * vectors, colors and entity ids are plain data and may be copied freely.
 */
#ifndef SKYWALKER_NATIVE_VALUE_H
#define SKYWALKER_NATIVE_VALUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKY_ABI_VERSION 1

typedef enum SkyType {
    SKY_NONE = 0,
    SKY_BOOL = 1,
    SKY_NUMBER = 2,
    SKY_VEC = 3,    /* 3 floats: x, y, z */
    SKY_COLOR = 4,  /* 4 floats: r, g, b, a (linear-ish 0..1) */
    SKY_ENTITY = 5, /* stable 64-bit entity id (0 = no entity) */
    SKY_STRING = 6, /* heap object (refcounted) */
    SKY_LIST = 7,   /* heap object (refcounted) */
    SKY_MAP = 8     /* heap object (refcounted) */
} SkyType;

/* Header of every heap object. */
typedef struct SkyObject {
    int32_t refcount;
    uint32_t kind; /* SkyType of the object */
} SkyObject;

typedef struct SkyValue {
    uint32_t type; /* SkyType */
    uint32_t reserved;
    union {
        double number;
        uint32_t boolean;
        float vec[4];
        uint64_t entity;
        SkyObject* object;
    } as;
} SkyValue;

static inline SkyValue sky_none(void) {
    SkyValue v;
    v.type = SKY_NONE;
    v.reserved = 0;
    v.as.number = 0;
    return v;
}
static inline SkyValue sky_number(double n) {
    SkyValue v;
    v.type = SKY_NUMBER;
    v.reserved = 0;
    v.as.number = n;
    return v;
}
static inline SkyValue sky_bool(int b) {
    SkyValue v;
    v.type = SKY_BOOL;
    v.reserved = 0;
    v.as.number = 0;
    v.as.boolean = b ? 1u : 0u;
    return v;
}
static inline SkyValue sky_vec(float x, float y, float z) {
    SkyValue v;
    v.type = SKY_VEC;
    v.reserved = 0;
    v.as.vec[0] = x;
    v.as.vec[1] = y;
    v.as.vec[2] = z;
    v.as.vec[3] = 0;
    return v;
}
static inline SkyValue sky_color(float r, float g, float b, float a) {
    SkyValue v;
    v.type = SKY_COLOR;
    v.reserved = 0;
    v.as.vec[0] = r;
    v.as.vec[1] = g;
    v.as.vec[2] = b;
    v.as.vec[3] = a;
    return v;
}
static inline SkyValue sky_entity(uint64_t id) {
    SkyValue v;
    v.type = SKY_ENTITY;
    v.reserved = 0;
    v.as.entity = id;
    return v;
}
static inline int sky_is_object(const SkyValue* v) { return v->type >= SKY_STRING; }

#ifdef __cplusplus
}
#endif

#endif /* SKYWALKER_NATIVE_VALUE_H */
