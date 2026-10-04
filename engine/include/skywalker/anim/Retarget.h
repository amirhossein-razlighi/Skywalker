#pragma once
// Pose-space retargeting between different humanoid rigs.
//
// Name-based retargeting (anim::retarget) only works when two rigs share bone names, rest poses
// and bone axes. Pose-space retargeting works between any two humanoids:
//
//   * both skeletons are mapped to humanoid slots (HumanoidMap: mixamo / suffixed (thigh_l) / generic names, or
//     overrides);
//   * every mapped bone of the target takes the source bone's rotation *relative to its rest pose*
//     in model space (G_t = A * (G_s * R_s^-1) * A^-1 * R'_t), so differing bone axes do not matter;
//   * rest-pose differences (T-pose vs A-pose) are removed by aligning each target limb's rest
//     direction to the source's (R'_t = align * R_t) before the motion is applied;
//   * the hips (and a root bone above them) translate by the source motion scaled by the leg
//     length ratio, so a short character keeps its feet on the ground and strides proportionally;
//   * bones keep the target's own lengths (rotations only below the hips): no stretching.
//
// A is the rig alignment (source model frame -> target model frame) from each rig's up (hips -> head)
// and left (right arm -> left arm) axes, snapped to the nearest axis-aligned rotation when close.

#include <array>
#include <string>
#include <vector>

#include "skywalker/anim/HumanoidMap.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::anim {

struct RetargetOptions {
    float fps = 30.f;            // resampling rate of the output clip
    bool translation = true;     // move the hips / root (else the hips stay at the target's rest height)
    Json sourceMap;              // humanoid slot overrides for the source ({"leftHand": "hand_l"})
    Json targetMap;              // same for the target
};

/// Everything that does not depend on the clip: maps, rest data, alignments. Reuse it for many clips.
struct RetargetSetup {
    HumanoidMap source, target;
    int sourceRoot = -1, targetRoot = -1;  // a root bone above the hips (root motion), -1 if none
    Quat align;                           // A: source model frame -> target model frame
    std::array<Quat, kHumanBones> restAlign{};  // per slot: target rest -> source rest direction (model space)
    float scale = 1.f;                    // target / source leg length (hips and root translation)
    std::vector<Mat4> sourceRest, targetRest;   // rest globals
    std::vector<std::string> warnings;

    Json toJson(const Skeleton& source, const Skeleton& target) const;
};

/// Maps both skeletons and precomputes the alignment. Fails (with the missing slots) when either
/// is not a complete humanoid.
Result<RetargetSetup> prepareRetarget(const Skeleton& source, int sourceRootBone, const Skeleton& target, int targetRootBone,
                                      const RetargetOptions& options = {});

/// The source pose (local transforms) retargeted to target local transforms.
void retargetPose(const RetargetSetup& setup, const Skeleton& source, const Pose& sourcePose, const Skeleton& target,
                  Pose& targetPose, bool translation = true);

/// Resamples `clip` (on `source`) at options.fps into a clip on `target`: rotation channels for the
/// mapped bones (and the root), translation for the hips (and the root).
Clip retargetClipPose(const RetargetSetup& setup, const Skeleton& source, const Clip& clip, const Skeleton& target,
                      const RetargetOptions& options = {});

/// Max over the clip's frames of |bone length change| / rest length for the target's mapped limbs:
/// 0 for a correct retarget (rotations only). Tests and the retarget tool report it.
float retargetStretch(const Skeleton& target, const Clip& clip, int samples = 16);

}  // namespace sky::anim
