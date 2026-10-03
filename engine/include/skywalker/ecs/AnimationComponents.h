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
    std::string lookAt;      // entity the head turns towards (look-at IK); "" = off
    float lookAtWeight = 1.f;
    float lookAtLimit = 70.f;  // max head/spine turn in degrees

    static const TypeInfo& type();
};

/// Keeps this entity on a bone of an animated character (weapons in hands, hats, lanterns).
struct BoneAttachment {
    std::string target;  // entity with the Animator; "" = the nearest ancestor that has one
    std::string bone;    // bone name ("RightHand", "mixamorig:Head"...)
    Vec3 offset{0.f};    // position in the bone's space (meters)
    Vec3 rotation{0.f};  // Euler degrees in the bone's space
    bool followScale = false;  // inherit the bone's scale (off: keep this entity's own scale)

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
