#include "skywalker/ecs/CharacterComponents.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<CharacterIk>);

const TypeInfo& CharacterIk::type() {
    static const TypeInfo info{
        "characterIk",
        "Automatic foot planting (stairs, slopes, rocks), pelvis adjustment, feet aligned to the slope and locked while in "
        "contact, hand targets (two-handed grips, ledges, rails) and turn in place for a humanoid character. Put it on the "
        "entity with the animator; bones come from the skeleton's humanoid map (character_inspect shows it).",
        {
            SKY_FIELD(CharacterIk, feet, Bool, "Plant the feet on the ground under them every tick"),
            SKY_FIELD_RANGE(CharacterIk, feetWeight, Float, "0 = animation only .. 1 = fully planted", 0.f, 1.f),
            SKY_FIELD_RANGE(CharacterIk, stepHeight, Float, "Meters a foot may reach up or down to find ground (stairs: 0.3-0.5)",
                            0.01f, 2.f),
            SKY_FIELD_RANGE(CharacterIk, footHeight, Float, "Ankle height above the sole in meters; -1 = measured from the rest pose",
                            -1.f, 0.5f),
            SKY_FIELD(CharacterIk, pelvis, Bool, "Lower the hips so the lower foot reaches its ground (uneven ground, stairs)"),
            SKY_FIELD(CharacterIk, alignFeet, Bool, "Pitch and roll the feet onto the ground's slope"),
            SKY_FIELD_RANGE(CharacterIk, maxSlope, Float, "Steeper ground aligns the feet only this far (degrees)", 0.f, 80.f),
            SKY_FIELD_ENUM(CharacterIk, contact,
                           "When a foot is in contact and locks to its spot (no sliding): auto = low and slow, velocity = slow, "
                           "events = animation events foot_l_down / foot_l_up / foot_r_down / foot_r_up, always, never",
                           "auto", "velocity", "events", "always", "never"),
            SKY_FIELD_RANGE(CharacterIk, lockSpeed, Float, "A foot slower than this (m/s) counts as planted", 0.f, 5.f),
            SKY_FIELD_RANGE(CharacterIk, lockDistance, Float,
                            "A locked foot re-plants with a quick step once the animation pulls it this far (m)", 0.02f, 2.f),
            SKY_FIELD_RANGE(CharacterIk, smoothing, Float, "How fast the pelvis and feet follow the ground (1/s; higher = snappier)",
                            0.5f, 60.f),
            SKY_FIELD_ENTITY(CharacterIk, leftHand,
                             "The left hand reaches this entity: a grip point on a staff held in the right hand, a ledge, a rail"),
            SKY_FIELD_RANGE(CharacterIk, leftHandWeight, Float, "Blend of the left-hand target (animate it for grabs)", 0.f, 1.f),
            SKY_FIELD_ENTITY(CharacterIk, rightHand, "The right hand reaches this entity"),
            SKY_FIELD_RANGE(CharacterIk, rightHandWeight, Float, "Blend of the right-hand target", 0.f, 1.f),
            SKY_FIELD(CharacterIk, handRotation, Bool, "Also orient the hands like their target entities"),
            SKY_FIELD_RANGE(CharacterIk, turnSpeed, Float, "Turn-in-place rate in degrees per second (turn_in_place)", 1.f, 2000.f),
        }};
    return info;
}

}  // namespace sky
