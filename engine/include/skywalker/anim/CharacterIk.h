#pragma once
// Automatic foot IK (foot planting, pelvis adjustment, slope alignment, contact locking) and hand
// targets for humanoids. Pure functions over poses and ground probes: the AnimationSystem feeds them
// ray hits from the scene every tick; tests feed them synthetic ground.
//
// Per tick and foot:
//   1. the animated ankle and toe are probed downward (heel and toe rays, `stepHeight` above and below);
//   2. the ground offset under the heel (relative to the character's base plane) lifts or lowers the
//      ankle target, keeping the animation's own lift (swing feet stay in the air);
//   3. the pelvis drops just enough for every foot to reach its target with the leg's length (or by the lowest
//      negative offset when the leg length is unknown), smoothed;
//   4. the foot pitches / rolls onto the slope from the heel -> toe probe line and the ground normal,
//      clamped at `maxSlope`; the toe never sinks into a higher step;
//   5. a foot in contact (slow and low, or flagged by animation events) locks to its world spot until the
//      animation pulls it `lockDistance` away; it then re-plants with a short lifted arc (no sliding).

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/anim/HumanoidMap.h"
#include "skywalker/core/Json.h"

namespace sky::anim {

/// A downward probe result (world space).
struct GroundProbe {
    bool hit = false;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 1, 0};
};

struct FootIkSettings {
    float weight = 1.f;
    float stepHeight = 0.45f;
    float footHeight = 0.08f;  // ankle above the sole (resolved: the component's < 0 means "from the rest pose")
    float toeHeight = 0.02f;   // toe joint above the sole
    bool pelvis = true;
    bool alignFeet = true;
    float maxSlope = 40.f;
    std::string contact = "auto";
    float lockSpeed = 0.35f;
    float lockDistance = 0.22f;
    float smoothing = 14.f;
};

/// Runtime state of both feet (kept by the animation system between ticks).
struct FootIkState {
    struct Foot {
        bool contact = false;
        bool locked = false;
        Vec3 lockPoint{0, 0, 0};     // the planted ankle (world)
        float replant = 1.f;         // 0..1 progress of a re-plant step (1 = done)
        Vec3 replantFrom{0, 0, 0};
        Vec3 lastAnimated{0, 0, 0};  // the animated ankle last tick (velocity)
        bool hasLast = false;
        float offset = 0.f;          // smoothed ground offset (m)
        float weight = 0.f;          // smoothed IK weight (fades when no ground is in reach)
        bool eventContact = false;   // contact flagged by animation events (contact = events)
        Vec3 target{0, 0, 0};        // last solved ankle target (world)
        Quat tilt;                   // last slope alignment (world rotation applied to the foot)
        GroundProbe ground;          // last heel probe
    };
    std::array<Foot, 2> feet;  // 0 left, 1 right
    float pelvisOffset = 0.f;  // smoothed (m, along up; <= 0)
    bool initialized = false;
};

/// What the solver needs about one foot this tick (world space).
struct FootInput {
    Vec3 ankle{0, 0, 0};  // animated ankle
    Vec3 toe{0, 0, 0};    // animated toe joint
    GroundProbe heel, toeProbe;
    Vec3 hip{0, 0, 0};       // the leg's hip joint (pelvis drop: only as far as the leg cannot reach)
    float legLength = 0.f;   // hip -> knee -> ankle; 0 = unknown (the pelvis drops by the lowest ground offset)
};

struct FootIkResult {
    std::array<Vec3, 2> ankle{};     // ankle targets
    std::array<Quat, 2> tilt{};      // world rotation to apply to each foot (slope alignment)
    std::array<float, 2> weight{};   // per-foot IK weight
    float pelvisOffset = 0.f;        // along up (<= 0)
};

/// One solver step. `base` is the character's base plane point (entity origin), `up` its up axis.
FootIkResult solveFeet(const FootIkSettings& settings, const std::array<FootInput, 2>& feet, Vec3 base, Vec3 up, float dt,
                       FootIkState& state);

/// Applies the result to a pose: hips moved by the pelvis offset, two-bone IK of each leg onto its ankle
/// target, feet rotated by their tilt. `toModel` maps world -> the skeleton's model space.
void applyFeet(const Skeleton& sk, const HumanoidMap& map, const FootIkResult& result, const Mat4& toModel, Pose& pose,
               std::vector<Mat4>& globals);

/// Two-bone IK of a hand (upper arm, lower arm, hand from the map) onto a world target, optionally
/// matching its rotation. Returns false when the arm chain is missing.
bool applyHand(const Skeleton& sk, const HumanoidMap& map, bool left, const Mat4& targetWorld, const Mat4& toModel, float weight,
               bool matchRotation, Pose& pose, std::vector<Mat4>& globals);

/// Ankle and toe heights above the sole in the rest pose (feet flat on the lowest foot point).
void restFootHeights(const Skeleton& sk, const HumanoidMap& map, float& ankleHeight, float& toeHeight);

}  // namespace sky::anim
