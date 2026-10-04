#pragma once
// Animation components: skeletal animators, bone attachments and sequence players.
// Plain reflected data like every component; the engine's AnimationSystem runs them.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// Plays skeletal animation on this entity's rigged meshes (and its children's): a state
/// machine from a controller asset, or simply one clip. Runs in the fixed simulation tick
/// while playing; while editing it shows a preview pose (scrub with `time`).
struct Animator {
    std::string library;     // animation library (*.anim) with the skeleton and clips; "" = from the mesh
    std::string controller;  // state machine (*.animctl.json); "" = play `clip`
    std::string clip;        // clip to play without a controller ("" = the first one)
    float speed = 1.f;       // playback rate (0 pauses)
    bool loop = true;        // without a controller: loop the clip
    bool rootMotion = false; // move the entity by the animation's root (hips) movement
    std::string preview = "pose";  // editor: rest (bind pose) | pose (frame at `time`) | play (live)
    float time = 0.f;        // editor preview time in seconds
    EntityLink lookAt;       // entity the head turns towards (look-at IK); empty = off
    float lookAtWeight = 1.f;
    float lookAtLimit = 70.f;  // max head/spine turn in degrees
    // --- character tech (docs/CHARACTERS.md) ---
    bool rootYaw = false;      // root motion also turns the entity by the clip's root rotation (turns, curved walks)
    bool inPlace = false;      // play locomotion in place: the root's horizontal motion (and yaw) is removed, the entity stays
    std::string retargetFrom;  // clip library to take clips from when they are not in `library` (pose-space retargeted)
    std::string retarget = "auto";  // auto | pose | name: how clips from another rig map onto this skeleton

    static const TypeInfo& type();
};

/// Keeps this entity on a bone of an animated character (weapons in hands, hats, lanterns).
struct BoneAttachment {
    EntityLink character;   // entity with the Animator; empty = the nearest ancestor that has one
    std::string bone;       // bone name ("RightHand", "mixamorig:Head"...)
    Vec3 offset{0.f};    // position in the bone's space (meters)
    Vec3 rotation{0.f};  // Euler degrees in the bone's space
    bool followScale = false;  // inherit the bone's scale (off: keep this entity's own scale)

    static const TypeInfo& type();
};

/// Two-bone IK effector: a character's hand or foot (with its elbow or knee) reaches this
/// entity — a hand on a door handle, rail or lever, a foot planted on a step. The end bone's
/// parent and grandparent bend; bone lengths never change.
struct IkTarget {
    EntityLink character;   // entity with the Animator; empty = the nearest ancestor that has one
    std::string bone;       // end bone: LeftHand, RightFoot, ...
    float weight = 1.f;     // 0 = animation only, 1 = fully on the target
    Vec3 pole{0.f};         // bend direction in the character's space (knees: [0,0,-1] forward); 0 = keep the animated bend
    bool matchRotation = false;  // also orient the end bone like this entity

    static const TypeInfo& type();
};

/// Plays a sequence asset (cinematic timeline: camera shots and cuts, property keys,
/// events, animation tracks). Plays during simulation; while editing, `preview` shows
/// the sequence frozen at `time`.
struct SequencePlayer {
    std::string sequence;    // *.sequence.json
    bool playOnStart = true; // start when the simulation starts
    bool loop = false;
    float speed = 1.f;
    bool preview = false;    // editor: apply the sequence at `time` in the viewport
    float time = 0.f;        // editor scrub position (seconds)

    static const TypeInfo& type();
};

}  // namespace sky
