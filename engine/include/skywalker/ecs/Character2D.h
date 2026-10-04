#pragma once
// `character2d`: a kinematic platformer controller on the XY plane. It moves a capsule through the 2D
// world (sliding along walls, walking up and down slopes, standing on one-way platforms) with its own
// gravity, acceleration, coyote time and jump buffering. Drive it from Wander every tick:
// move2d(self, axis), jump2d(self), grounded2d(self). Included at the end of Components.h; table in
// physics2d/Physics2DComponents.cpp.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Character2D {
    float height = 1.f;             // capsule height (units, not scaled by the transform)
    float radius = 0.3f;
    Vec2 offset{0.f, 0.f};          // capsule center relative to the entity origin (y = height/2: feet at the origin)
    float moveSpeed = 6.f;          // units/s at move2d(self, 1)
    float acceleration = 60.f;      // units/s^2 on the ground
    float airControl = 0.6f;        // share of the acceleration in the air (0..1)
    float jumpSpeed = 12.f;         // units/s upward
    float gravity = 35.f;           // units/s^2 downward (snappy platformers use 30..60)
    float fallMultiplier = 1.5f;    // extra gravity while falling (1 = none)
    float maxFallSpeed = 20.f;      // units/s
    float maxSlope = 50.f;          // degrees: steeper ground is a wall
    float coyoteTime = 0.1f;        // seconds after leaving the ground in which a jump still works
    float jumpBuffer = 0.12f;       // seconds a jump pressed in the air is remembered until landing
    float snapDistance = 0.25f;     // units: stays glued to the ground when walking down slopes and steps
    std::string layer = "player";   // default | static | player | enemy | projectile | trigger | debris
    std::string mask = "all";       // layers it collides with
    Vec2 velocity{0.f, 0.f};        // live while playing (read/write: knockback, launch pads)
    bool grounded = false;          // live while playing (read-only)

    static const TypeInfo& type();
};

}  // namespace sky
