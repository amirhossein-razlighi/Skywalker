#pragma once
// Internal state of the AnimationSystem shared by its translation units (AnimationSystem.cpp,
// CharacterIk.cpp). Not part of the public API.

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/anim/BodyColliders.h"
#include "skywalker/anim/CharacterIk.h"
#include "skywalker/anim/HumanoidMap.h"

namespace sky::anim {

struct AnimationSystem::Instance {
    std::string signature;
    std::shared_ptr<const Library> lib;
    std::shared_ptr<const ControllerDef> ctl;
    AnimatorRuntime rt;
    std::string error;
    int rootMode = -1;  // pinned root (rootMotion or inPlace) * 2 + root yaw; -1 = not configured yet

    // The first rigged mesh under the animator: where the skeleton sits in the world.
    uint64_t bindRevision = ~0ull, bindGeneration = 0;
    EntityId meshEntity = kNoEntity;
    std::string meshKey, libraryFromMesh;
    Mat4 transform;  // the mesh's glTF -> mesh space transform

    Pose pose;
    std::vector<Mat4> globals;
    std::vector<Mat4> prevGlobals;  // the pose of the tick before (render interpolation)
    uint64_t version = 0;
    bool posed = false;
    std::string editKey;
    uint64_t paramsVersion = 0;
    float editClock = 0.f;

    struct SkinEntry {
        const MeshData* mesh = nullptr;
        std::vector<int> map;
        uint64_t version = ~0ull;
        SkinPose pose;
        std::shared_ptr<MeshData> posed;
        uint64_t posedVersion = ~0ull;
        SkinPose blended;  // between the previous tick's pose and the last one (render interpolation)
        uint64_t blendedVersion = ~0ull;
        float blendedAlpha = -1.f;
    };
    std::unordered_map<std::string, SkinEntry> skins;

    std::vector<int> lookChain;
    std::vector<Mat4> restGlobals;     // skeleton rest pose (look-at reference)
    float lookBlend = 0.f;
    std::optional<Vec3> lookTarget;    // last aim point (world): lets the turn fade out when lookAt clears
    Vec3 lastRootMotion{0, 0, 0};
    float lastRootYaw = 0.f;  // radians, last tick
    std::vector<std::string> recentEvents;
    std::optional<std::pair<std::string, float>> preview;     // tool preview: (state or clip, seconds)
    std::optional<std::pair<std::string, float>> seqPreview;  // sequencer animation track, this frame only

    // Character tech (CharacterIk.cpp): humanoid map, foot / hand IK state, turn in place.
    std::optional<HumanoidMap> humanoid;  // detected once per library
    FootIkState feet;
    float tickDt = 0.f;                   // dt of the tick being posed (0 = editor preview: no smoothing)
    float restAnkleHeight = -1.f, restToeHeight = 0.f;  // model units (rest pose)
    std::optional<float> turnTarget;      // turn_in_place: world yaw (degrees) to reach
    Json ikStatus;                        // last solve, for character_inspect and the debug views
    std::unordered_map<std::string, std::vector<BodyCapsule>> capsules;  // per skinned mesh (fitted once)
};

struct AnimationSystem::SeqInstance {
    std::string signature;
    std::shared_ptr<const SequenceDef> def;
    std::string error;
    bool started = false;
    bool playing = false;
    float time = 0.f;
};

}  // namespace sky::anim
