#pragma once
// AnimationSystem: runs Animators, BoneAttachments and SequencePlayers against the Scene.
//
//   * Playing: tick(dt) once per fixed simulation step, in this order: sequencers (keys,
//     camera shots and cuts, events, animation tracks), animators (state machines, events
//     -> Wander, root motion, look-at IK), bone attachments. Deterministic; reset() on
//     play and stop.
//   * Editing: animators show a preview pose (Animator.preview / time, or a tool's
//     preview), sequencers with `preview` show their frame at `time`, attachments follow
//     their bones. These edit-mode effects are applied for one rendered frame only
//     (beginFrame / endFrame), so they never dirty the scene, its history or saved files.
//   * Rendering: skin() gives each skinned draw its joint palette and posed bounds;
//     posedMesh() gives CPU-skinned geometry for raycasts.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/anim/Animation.h"
#include "skywalker/anim/Controller.h"
#include "skywalker/anim/Sequence.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/scene/Scene.h"

namespace sky::anim {

class AnimationSystem {
public:
    struct Hooks {
        std::function<std::string(const std::string&)> resolvePath;            // project-relative -> absolute
        std::function<const MeshData*(const std::string& meshKey)> mesh;        // CPU mesh ("asset:x.glb#2")
        std::function<std::string(const std::string& meshFile)> libraryFor;     // library of an imported model ("" = none)
        std::function<void(const std::string& name, EntityId target)> emit;     // Wander events
        /// Root motion hook (e.g. a physics character controller). Return true when the
        /// delta (world space, meters) was consumed; otherwise the Transform moves.
        std::function<bool(EntityId entity, Vec3 worldDelta)> rootMotion;
    };
    Hooks hooks;

    explicit AnimationSystem(Scene& scene);
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    // --- Lifecycle -----------------------------------------------------------------------
    void reset();                              // forget runtime state (play start / stop)
    void setPlaying(bool playing) { playing_ = playing; }
    void tick(float dt);                       // one fixed simulation step
    void editorUpdate(float dt);               // advance live editor previews
    void invalidate(const std::string& path);  // an asset file changed on disk ("" = all)

    /// Edit-mode effects for one frame (sequence previews, bone attachments): apply before
    /// building a frame, restore right after. No-ops while playing.
    struct FrameOverrides {
        std::vector<std::pair<EntityId, std::pair<std::string, Json>>> components;  // entity, (component, old value)
        bool environment = false;
        Json oldEnvironment;
        bool active = false;
    };
    FrameOverrides beginFrame(bool editing);
    void endFrame(FrameOverrides& overrides);

    // --- Rendering & queries ---------------------------------------------------------------
    const SkinPose* skin(EntityId drawEntity, const std::string& meshKey);
    std::shared_ptr<const MeshData> posedMesh(EntityId drawEntity, const std::string& meshKey);
    /// Nearest entity at or above `e` with an Animator (kNoEntity if none).
    EntityId animatorFor(EntityId e) const;
    /// World transform of a bone (fuzzy name match), using the current pose.
    Result<Mat4> boneWorld(EntityId animatorEntity, std::string_view bone);

    // --- Assets ---------------------------------------------------------------------------
    /// "models/hero.anim", "models/hero.glb" or "asset:models/hero.glb" -> library.
    Result<std::shared_ptr<const Library>> library(const std::string& ref);
    Result<std::shared_ptr<const ControllerDef>> controller(const std::string& path);
    Result<std::shared_ptr<const SequenceDef>> sequence(const std::string& path);
    /// The library an animator uses (its own field, its controller's, or its mesh's).
    Result<std::shared_ptr<const Library>> libraryOf(EntityId animatorEntity);
    /// A clip reference ("Walk" in `lib`, or "anims/dance.anim#Dance") posed for `lib`'s skeleton.
    std::shared_ptr<const Clip> resolveClip(const std::shared_ptr<const Library>& lib, const std::string& ref);

    // --- Control (tools, Wander, sequencer) ------------------------------------------------
    Status setParam(EntityId e, std::string_view name, const Json& value);
    Status trigger(EntityId e, std::string_view name);
    Status play(EntityId e, const std::string& stateOrClip, float fade, std::optional<bool> loop = std::nullopt, int layer = 0);
    std::string stateName(EntityId e);
    /// State, parameters, clips, last events, last root motion.
    Result<Json> describe(EntityId e);
    /// Editor/tool preview: hold an animator at a state or clip, `seconds` in.
    Status setPreview(EntityId animatorEntity, const std::string& stateOrClip, float seconds);
    void clearPreview(EntityId animatorEntity);

    Status playSequence(EntityId e, float from = 0.f);
    Status stopSequence(EntityId e);
    Json sequenceState(EntityId e);
    /// Tool scrubbing: show a sequence at `seconds` in the editor until cleared.
    void setSequenceScrub(EntityId e, float seconds);
    void clearSequenceScrub(EntityId e);
    /// The camera a sequence shows at `seconds` (camera cut, else a shot's camera).
    EntityId sequenceCamera(EntityId e, float seconds);

private:
    struct Instance;
    struct SeqInstance;

    Instance* instance(EntityId animatorEntity);
    void bindMesh(Instance& inst, EntityId animatorEntity);
    void initRuntime(Instance& inst, const Animator& a);
    void poseEditing(Instance& inst, EntityId e, const Animator& a);
    void finishPose(Instance& inst, EntityId e, const Animator& a);
    void applyLookAt(Instance& inst, EntityId e, const Animator& a);
    Mat4 modelToWorld(const Instance& inst, EntityId animatorEntity) const;
    void updateAttachments(FrameOverrides* overrides);
    SeqInstance* seqInstance(EntityId e);
    void applySequence(EntityId e, SeqInstance& s, float t, float prevT, bool playing, FrameOverrides* overrides);
    bool writeComponent(EntityId e, const std::string& component, const Json& patch, FrameOverrides* overrides);
    void warnOnce(const std::string& key, const std::string& message);

    Scene& scene_;
    std::unordered_map<EntityId, std::unique_ptr<Instance>> animators_;
    std::unordered_map<EntityId, std::unique_ptr<SeqInstance>> sequencers_;
    std::unordered_map<std::string, std::pair<std::shared_ptr<const Library>, std::string>> libraries_;  // path -> (lib, error)
    std::unordered_map<std::string, std::pair<std::shared_ptr<const ControllerDef>, std::string>> controllers_;
    std::unordered_map<std::string, std::pair<std::shared_ptr<const SequenceDef>, std::string>> sequences_;
    std::unordered_map<std::string, std::shared_ptr<const Clip>> retargeted_;
    std::unordered_map<EntityId, float> scrubs_;
    std::unordered_map<std::string, bool> warned_;
    uint64_t assetGeneration_ = 1;  // bumps on invalidate(): instances re-bind
    bool playing_ = false;
};

}  // namespace sky::anim
