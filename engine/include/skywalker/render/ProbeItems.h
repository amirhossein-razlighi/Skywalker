#pragma once
// Renderer-facing description of a reflection probe (the `reflection_probe` component, resolved
// to world space by buildFrame). See render/ReflectionProbes.h and docs/RENDERING.md
// "Reflection probes".

#include <cstdint>

#include "skywalker/math/Math.h"

namespace sky {

using EntityId = uint64_t;

struct ProbeItem {
    EntityId entity = 0;
    Vec3 center;                       // world center of the influence volume
    Mat4 rotation;                     // world orientation of the volume (rotation only)
    Vec3 halfExtents{5.f, 2.5f, 5.f};  // box half size in meters; sphere: x = radius
    bool sphere = false;
    Vec3 capture;                      // world capture point
    float blendDistance = 1.f;         // m
    bool boxProjection = true;
    bool projectionBox = false;        // reflections project onto a box other than the volume:
    Vec3 projectionCenter;             //   its center in the volume's frame (m)
    Vec3 projectionHalf;               //   its half size (m)
    float intensity = 1.f;
    bool interior = false;
    int ambientMode = 0;               // 0 probe, 1 sky, 2 color
    Vec3 ambientColor{0.01f, 0.01f, 0.012f};  // linear rgb x energy
    int update = 0;                    // 0 once, 1 on_change, 2 realtime
    int interval = 1;                  // realtime: frames between captures
    int resolution = 256;              // face size (px), power of two 64..512
    uint32_t cullMask = 0xFFFFFu;      // render layers the capture draws
    int priority = 0;
    float nearPlane = 0.05f;           // capture clip planes (m)
    float farPlane = 20.f;
    uint64_t captureKey = 0;           // hash of everything that changes the captured image itself
};

}  // namespace sky
