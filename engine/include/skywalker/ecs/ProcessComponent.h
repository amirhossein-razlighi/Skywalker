#pragma once
// `process`: how an entity and its children run while the game is paused or slowed down, in
// which order their behaviors run, and whether their motion is smoothed between ticks on
// screen (docs/ARCHITECTURE.md "Game pause, process modes and time scale" and "Render
// interpolation"). Included at the end of Components.h.
//
// Every field can say `inherit`: the value then comes from the nearest ancestor that sets it.
// Without any setting an entity is `pausable`, on the `game` clock and interpolated; a UI
// canvas (and everything under it) defaults to `always` on the `real` clock, so menus keep
// working while the game is paused or in slow motion.

#include <string>

#include "skywalker/ecs/Reflection.h"

namespace sky {

struct Process {
    std::string mode = "inherit";           // inherit | pausable | when_paused | always | disabled
    int priority = 0;                       // behaviors run in (priority, scene order) order: lower first (not inherited)
    std::string clock = "inherit";          // inherit | game (follows time_scale) | real (ignores it: menus, HUD)
    std::string interpolation = "inherit";  // inherit | on | off: smooth motion between 60 Hz ticks on fast displays
    float timeScale = 1.f;                  // this entity's own clock speed, multiplied down the hierarchy (slow one enemy)

    static const TypeInfo& type();
};

}  // namespace sky
