#pragma once
// Humanoid bone map: which bone of a skeleton plays which body part (hips, spine, arms, legs...).
//
// Retargeting between rigs, foot / hand IK and body colliders all need to know the anatomy of a
// skeleton without relying on one naming convention. detectHumanoid() finds it:
//
//   1. by name: common conventions are recognized after normalizing case, namespaces
//      ("mixamorig:", "Armature|") and separators: "LeftUpLeg" / "thigh_l" / "UpperLeg.L" /
//      "Bip01 L Thigh" / "upper_arm.R" all map to the same slot;
//   2. by topology: missing links are inferred from the hierarchy (a hand's parent is the lower
//      arm, the hips are the common ancestor of both legs, the chest the common ancestor of both
//      arms), and left / right come from the rest pose (glTF characters face +Z: their left is +X)
//      when the names do not say.
//
// The result names a convention ("mixamo", "suffixed" for side-suffixed names like pelvis / thigh_l /
// upperarm_r, "generic" or "custom" after overrides), a confidence and warnings, so tools can show
// agents exactly what was understood.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/anim/Animation.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::anim {

enum class HumanBone : uint8_t {
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    LeftShoulder,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightShoulder,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftUpperLeg,
    LeftLowerLeg,
    LeftFoot,
    LeftToes,
    RightUpperLeg,
    RightLowerLeg,
    RightFoot,
    RightToes,
    Count
};
constexpr size_t kHumanBones = static_cast<size_t>(HumanBone::Count);

/// Stable slot names used in tools and JSON: "hips", "spine", "leftUpperArm", ...
const char* humanBoneName(HumanBone b);
std::optional<HumanBone> humanBoneFromName(std::string_view name);
std::vector<std::string> humanBoneNames();
/// The slot this one hangs from in the canonical hierarchy (Hips for the root slots; Hips has none).
std::optional<HumanBone> humanParent(HumanBone b);
/// The slot a bone points at (upper arm -> lower arm), used to measure limb directions; none for ends.
std::optional<HumanBone> humanChild(HumanBone b);

struct HumanoidMap {
    std::array<int, kHumanBones> bones;  // skeleton bone index per slot, -1 = not present
    std::string convention = "generic";  // mixamo | suffixed | generic | custom
    float confidence = 0.f;              // 0..1: required slots found, names agreeing with topology
    std::vector<std::string> warnings;
    /// Foot bones that are IK controls outside the leg (left, right; -1 = none): retargeting keeps
    /// them at the end of the retargeted lower leg so the feet the mesh is skinned to follow the legs.
    std::array<int, 2> footControls{-1, -1};

    HumanoidMap() { bones.fill(-1); }
    int operator[](HumanBone b) const { return bones[static_cast<size_t>(b)]; }
    int& at(HumanBone b) { return bones[static_cast<size_t>(b)]; }
    /// Hips, a spine bone, the head, both arms (upper, lower, hand) and both legs (upper, lower, foot).
    bool complete() const;
    /// Enough to retarget: hips, a spine bone, the head or neck, both upper and lower arms and legs
    /// (hands and feet are mapped when both rigs have them; some rigs deform feet with the shin).
    bool retargetable() const;
    /// Slots that are required but missing (`forRetarget`: only those retargetable() needs).
    std::vector<std::string> missing(bool forRetarget = false) const;
    size_t mapped() const;
    /// {convention, confidence, complete, bones: {slot: bone name}, missing, warnings}.
    Json toJson(const Skeleton& skeleton) const;
};

/// Finds the humanoid slots of a skeleton (see the header comment). Never fails: an animal or a
/// prop rig returns an incomplete map with low confidence.
HumanoidMap detectHumanoid(const Skeleton& skeleton);

/// Applies {"leftHand": "hand_l", "spine": null, ...} on top of a detected map (null / "" clears a
/// slot). Unknown slots or bones fail with did-you-mean hints. Sets convention to "custom".
Status applyHumanoidOverrides(HumanoidMap& map, const Skeleton& skeleton, const Json& overrides);

/// Normalized bone name used for matching: lower case, no namespace, no separators
/// ("mixamorig:Left_Up-Leg" -> "leftupleg").
std::string normalizeBoneName(std::string_view name);

}  // namespace sky::anim
