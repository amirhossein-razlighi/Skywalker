#include "skywalker/ecs/AnimationComponents.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<Animator>);
static_assert(std::is_standard_layout_v<BoneAttachment>);
static_assert(std::is_standard_layout_v<SequencePlayer>);

const TypeInfo& Animator::type() {
    static const TypeInfo info{
        "animator",
        "Skeletal animation for this entity's rigged meshes (and its children's): a state machine controller or one clip. "
        "Set parameters and triggers with animator_set or Wander set_param/trigger/play_animation.",
        {
            SKY_FIELD(Animator, library, String, "Animation library (*.anim: skeleton + clips); empty = the one imported with the mesh"),
            SKY_FIELD(Animator, controller, String, "State machine (*.animctl.json); empty = play `clip`"),
            SKY_FIELD(Animator, clip, String, "Clip to play when there is no controller (empty = the first clip)"),
            SKY_FIELD_RANGE(Animator, speed, Float, "Playback rate (0 pauses, 2 = double speed)", 0.f, 10.f),
            SKY_FIELD(Animator, loop, Bool, "Loop the clip (without a controller)"),
            SKY_FIELD(Animator, rootMotion, Bool, "Move the entity with the animation's root (hips) movement instead of in place"),
            SKY_FIELD_ENUM(Animator, preview, "Editor preview: rest = bind pose, pose = frame at `time`, play = animate live",
                           "rest", "pose", "play"),
            SKY_FIELD_RANGE(Animator, time, Float, "Editor preview time (seconds into the default state / clip)", 0.f, 600.f),
            SKY_FIELD(Animator, lookAt, String, "Entity the head and spine turn towards (look-at IK); empty = off"),
            SKY_FIELD_RANGE(Animator, lookAtWeight, Float, "How strongly the head follows lookAt", 0.f, 1.f),
            SKY_FIELD_RANGE(Animator, lookAtLimit, Float, "Maximum head/spine turn (degrees)", 0.f, 180.f),
        }};
    return info;
}

const TypeInfo& BoneAttachment::type() {
    static const TypeInfo info{
        "attach",
        "Keeps this entity on a bone of an animated character (weapon in a hand, hat on a head, lantern on a belt).",
        {
            SKY_FIELD(BoneAttachment, target, String, "Entity with the animator; empty = nearest ancestor that has one"),
            SKY_FIELD(BoneAttachment, bone, String, "Bone name, e.g. RightHand or mixamorig:Head (animation_list shows them)"),
            SKY_FIELD(BoneAttachment, offset, Vec3, "Position offset in the bone's space (meters)"),
            SKY_FIELD(BoneAttachment, rotation, Vec3, "Rotation offset in the bone's space (Euler degrees)"),
            SKY_FIELD(BoneAttachment, followScale, Bool, "Inherit the bone's scale (off keeps this entity's scale)"),
        }};
    return info;
}

const TypeInfo& SequencePlayer::type() {
    static const TypeInfo info{
        "sequencer",
        "Plays a cinematic sequence (*.sequence.json): camera shots and cuts, keyed properties, events and animations. "
        "Build sequences with the sequence_* tools.",
        {
            SKY_FIELD(SequencePlayer, sequence, String, "Sequence asset (*.sequence.json)"),
            SKY_FIELD(SequencePlayer, playOnStart, Bool, "Start playing when the simulation starts"),
            SKY_FIELD(SequencePlayer, loop, Bool, "Loop at the end"),
            SKY_FIELD_RANGE(SequencePlayer, speed, Float, "Playback rate", 0.f, 10.f),
            SKY_FIELD(SequencePlayer, preview, Bool, "Editor: show the sequence frozen at `time` in the viewport"),
            SKY_FIELD_RANGE(SequencePlayer, time, Float, "Editor scrub position (seconds)", 0.f, 3600.f),
        }};
    return info;
}

}  // namespace sky
