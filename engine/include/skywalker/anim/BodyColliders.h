#pragma once
// Body colliders: capsules fitted to a skinned mesh around the bones of its skeleton (head, neck,
// torso, arms, legs; any bone of a creature). Hair, beards and fur collide with them, so long hair
// rests on the shoulders and fur never sinks into a moving leg.

#include <vector>

#include "skywalker/anim/Animation.h"
#include "skywalker/render/MeshData.h"

namespace sky::anim {

struct BodyCapsule {
    int bone = -1;        // capsule start: this bone's joint
    int child = -1;       // capsule end: this child's joint (-1 = `endLocal` in the bone's frame)
    Vec3 endLocal{0, 0, 0};
    float radius = 0.f;   // model (glTF) units
    size_t vertices = 0;  // vertices it was fitted to (bigger = more important)
};

/// Fits up to `maxCapsules` capsules to the rest pose: each bone that dominates enough vertices gets a
/// segment to its child joint (or along its vertices for end bones) with a radius that hugs them.
std::vector<BodyCapsule> fitBodyCapsules(const Skeleton& skeleton, const MeshData& mesh, const std::vector<int>& slotToBone,
                                         size_t maxCapsules = 24);

}  // namespace sky::anim
