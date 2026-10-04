#pragma once
// `reflection_probe`: a captured cubemap of the scene around a point that glossy surfaces inside
// its influence volume reflect (and, by default, take their ambient light from) instead of the sky
// (docs/RENDERING.md "Reflection probes"). Included at the end of Components.h; the renderer side
// is render/ReflectionProbes.h.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct ReflectionProbe {
    std::string shape = "box";          // box | sphere: the influence volume, centered on the entity
    Vec3 size{10.f, 5.f, 10.f};         // box: influence extent in meters (times the entity scale)
    float radius = 5.f;                 // sphere: influence radius in meters (times the entity scale)
    float blendDistance = 1.f;          // m inside the volume over which the probe fades into its surroundings
    bool boxProjection = true;          // parallax-correct reflections against the volume (rooms, corridors)
    Vec3 projectionSize{0.f, 0.f, 0.f}; // box that reflections are projected onto when it differs from the volume (0 = the volume)
    Vec3 projectionOffset{0.f, 0.f, 0.f};  // center of that box relative to the entity (m, local axes)
    float intensity = 1.f;              // reflection and ambient brightness multiplier
    bool interior = false;              // no sky light inside the volume: reflections and ambient come only from the probe
    std::string ambient = "probe";      // probe (diffuse ambient from the capture) | sky (reflections only) | color
    Vec4 ambientColor{0.1f, 0.1f, 0.11f, 1.f};  // interior captures: ambient instead of the sky; ambient "color": the ambient
    float ambientEnergy = 1.f;          // multiplies ambientColor
    std::string update = "once";        // once (cached until moved or rebaked) | on_change | realtime
    int interval = 1;                   // realtime: frames between captures (1 = every frame, within the face budget)
    int resolution = 256;               // cubemap face size in px: 64, 128, 256 or 512
    int cullMask = 0xFFFFF;             // render layers the capture draws (bit i = layer i+1; all 20 by default)
    int priority = 0;                   // higher wins where volumes overlap (equal: the smaller volume wins)
    Vec3 captureOffset{0.f, 0.f, 0.f};  // capture point relative to the entity (m, local axes); keep it inside the room
    float maxDistance = 0.f;            // capture far plane in meters (0 = automatic from the volume)

    static const TypeInfo& type();
};

}  // namespace sky
