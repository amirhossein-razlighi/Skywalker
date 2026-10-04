#pragma once
// `physics2d_world`: scene-wide 2D physics settings. Put it on one entity; without one the defaults
// apply. Included at the end of Components.h; table in physics2d/Physics2DComponents.cpp.

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Physics2DSettings {
    Vec2 gravity{0.f, -20.f};  // units/s^2 (1 unit = 1 tile at cellSize 1; games feel better above 9.81)
    int substeps = 4;          // solver sub-steps per 1/60 s tick (tall stacks, chains of joints)
    bool allowSleep = true;
    float impactSpeed = 1.f;   // units/s: approach speed above which `on impact` fires
    bool debugDraw = false;    // shapes, contacts and joints in the frame (editor viewport and captures)
    bool enabled = true;

    static const TypeInfo& type();
};

}  // namespace sky
