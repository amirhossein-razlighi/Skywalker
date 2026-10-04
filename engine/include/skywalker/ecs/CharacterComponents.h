#pragma once
// `characterIk`: automatic foot planting, pelvis adjustment, hand targets and turn-in-place for a
// humanoid character (docs/CHARACTERS.md). Included at the end of Components.h.
//
// It lives on the entity with the animator. The bones come from the humanoid bone map of the
// animator's skeleton (anim/HumanoidMap.h), so it works on mixamo, UE-style and generic rigs alike.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct CharacterIk {
    // --- Feet ---------------------------------------------------------------------------
    bool feet = true;              // plant the feet on the ground under them (stairs, slopes, rocks)
    float feetWeight = 1.f;        // 0 = animation only .. 1 = fully planted
    float stepHeight = 0.45f;      // meters a foot may reach up or down to find ground
    float footHeight = -1.f;       // ankle height above the sole (m); < 0 = measured from the rest pose
    bool pelvis = true;            // lower the hips so the lower foot can reach its ground
    bool alignFeet = true;         // pitch / roll the feet (and toes) to the ground's slope
    float maxSlope = 40.f;         // degrees: steeper ground aligns only this far
    std::string contact = "auto";  // auto (height + velocity) | velocity | events | always | never: when a foot locks
    float lockSpeed = 0.35f;       // m/s: a foot slower than this near the ground is in contact (auto / velocity)
    float lockDistance = 0.22f;    // m: a locked foot re-plants (a quick step) once the animation pulls it this far away
    float smoothing = 14.f;        // 1/s: how fast pelvis and foot offsets follow the ground (higher = snappier)
    // --- Hands --------------------------------------------------------------------------
    EntityLink leftHand;           // the left hand reaches this entity (a grip point on a held staff, a ledge, a rail)
    float leftHandWeight = 1.f;
    EntityLink rightHand;
    float rightHandWeight = 1.f;
    bool handRotation = false;     // also orient the hands like their targets
    // --- Turning ------------------------------------------------------------------------
    float turnSpeed = 220.f;       // deg/s for turn_in_place (the controller's `turn` parameter gets the angle left)

    static const TypeInfo& type();
};

}  // namespace sky
