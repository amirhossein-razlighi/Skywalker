#pragma once
// Animator controllers (`*.animctl.json`) and the state machine that runs them.
//
// A controller has typed parameters (float, int, bool, trigger) and one or more layers.
// Each layer is a state machine: states play a clip or a 1D / 2D blend space, with a
// speed, looping, and events at normalized times; transitions have conditions, a
// crossfade duration, an optional exit time, a start offset and can be interruptible.
// Extra layers override (or add to) the base pose, optionally only for a bone mask
// (e.g. an upper-body "wave" over locomotion).
//
//   {
//     "format": "skywalker.animctl", "version": 1,
//     "library": "models/hero.anim",
//     "parameters": {"speed": "float", "jump": "trigger", "grounded": {"type": "bool", "default": true}},
//     "layers": [{
//       "name": "Base", "default": "Locomotion",
//       "states": {
//         "Locomotion": {"blend": {"parameter": "speed",
//                        "motions": [{"clip": "Idle", "at": 0}, {"clip": "Walk", "at": 1.6}, {"clip": "Run", "at": 4.5}]}},
//         "Jump": {"clip": "Jump", "loop": false, "events": [{"time": 0.1, "name": "jump_start"}]}
//       },
//       "transitions": [
//         {"from": "Locomotion", "to": "Jump", "when": "jump", "duration": 0.1},
//         {"from": "Jump", "to": "Locomotion", "exit": 0.85, "duration": 0.2}
//       ]
//     }]
//   }
//
// Conditions ("when") are a string ("speed > 0.1 and grounded"), a list of such strings,
// or objects {"param", "op", "value"}. A bare trigger name fires (and consumes) the trigger.
//
// The runtime (AnimatorRuntime) is pure: no scene, no engine. It is stepped with a fixed
// dt while playing (deterministic) and can be seeked for editor previews.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/anim/Animation.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::anim {

enum class ParamType { Float, Int, Bool, Trigger };
const char* toString(ParamType t);

struct ParamDef {
    std::string name;
    ParamType type = ParamType::Float;
    float value = 0.f;  // default
};

struct MotionDef {
    std::string clip;      // clip name in the library, or "<library path>#<clip>" from another file
    float threshold = 0.f; // 1D blend position ("at")
    Vec2 position;         // 2D blend position ("pos")
    float speed = 1.f;
};

struct EventDef {
    float time = 0.f;  // normalized 0..1 within the state's cycle
    std::string name;
};

enum class StateKind { Clip, Blend1D, Blend2D };

struct StateDef {
    std::string name;
    StateKind kind = StateKind::Clip;
    std::vector<MotionDef> motions;  // Clip: exactly one
    std::string paramX, paramY;      // blend parameters
    float speed = 1.f;
    std::string speedParam;          // optional multiplier parameter
    bool loop = true;
    std::vector<EventDef> events;
};

enum class CondOp { Greater, Less, GreaterEq, LessEq, Equal, NotEqual, IsTrue, IsFalse, Trigger };

struct ConditionDef {
    std::string param;
    CondOp op = CondOp::IsTrue;
    float value = 0.f;
};

struct TransitionDef {
    std::string from;  // state name, or "any"
    std::string to;
    std::vector<ConditionDef> conditions;
    float duration = 0.2f;   // crossfade seconds
    float exitTime = -1.f;   // normalized; < 0 = none (with no conditions it defaults to 1)
    float offset = 0.f;      // normalized start time in the destination
    bool interruptible = false;
};

struct LayerDef {
    std::string name = "Base";
    float weight = 1.f;
    std::string weightParam;  // optional parameter driving the weight (0..1)
    bool additive = false;
    std::vector<std::string> mask;  // bones (with their descendants); empty = whole body
    std::string defaultState;
    std::vector<StateDef> states;
    std::vector<TransitionDef> transitions;

    int stateIndex(std::string_view name) const;
};

struct ControllerDef {
    std::string library;
    std::vector<ParamDef> params;
    std::vector<LayerDef> layers;

    static Result<ControllerDef> fromJson(const Json& doc);
    Json toJson() const;
    int paramIndex(std::string_view name) const;
    /// Checks every reference (states, parameters, clips via `hasClip`, mask bones) and
    /// returns precise, did-you-mean errors.
    Status validate(const std::function<bool(const std::string& clipRef)>& hasClip, const std::vector<std::string>& clipNames,
                    const Skeleton* skeleton) const;
};

Result<ControllerDef> loadController(const std::string& absolutePath);
Status saveController(const std::string& absolutePath, const ControllerDef& def);

/// A controller with one looping state per clip (used by animators without a controller).
ControllerDef simpleController(const std::vector<std::string>& clips, const std::string& defaultClip, bool loop);

/// Resolves a motion's clip reference to a clip posed for the animator's skeleton.
using ClipResolver = std::function<std::shared_ptr<const Clip>(const std::string& ref)>;

class AnimatorRuntime {
public:
    struct Event {
        std::string name;
        std::string state;
        int layer = 0;
    };

    /// Binds a controller to a skeleton. Unresolvable clips play as the rest pose.
    Status init(std::shared_ptr<const Library> library, std::shared_ptr<const ControllerDef> controller,
                const ClipResolver& resolve);
    bool ready() const { return library_ != nullptr; }
    const Library& library() const { return *library_; }
    const ControllerDef& controller() const { return *controller_; }

    // Parameters ---------------------------------------------------------------------
    bool setParam(std::string_view name, float value);  // bool: 0/1; trigger: value != 0 arms it
    bool trigger(std::string_view name);
    std::optional<float> param(std::string_view name) const;
    Json paramsJson() const;

    /// Crossfades to a state (or, if no state has that name, to a clip of the library as a
    /// one-shot state). `loop` overrides looping; a non-looping state entered this way
    /// returns to the layer's default state when it ends (unless it has its own exits).
    Status play(const std::string& stateOrClip, float fade, int layer = 0, std::optional<bool> loop = std::nullopt);
    /// Jumps straight to a state at a normalized time (editor previews, sequencer scrubbing).
    Status seek(const std::string& stateOrClip, float normalizedTime, int layer = 0);

    /// Root motion: the root bone's horizontal movement is taken out of the pose and
    /// reported by update() as a delta in model (glTF) space. `up` is the model's up axis.
    void setRootMotion(bool enabled, Vec3 up = {0, 1, 0});

    /// Advances by dt seconds (already scaled by the animator's speed). Collects events
    /// crossed this step and the root motion delta.
    void update(float dt, std::vector<Event>* events = nullptr, Vec3* rootDelta = nullptr);
    /// The current pose (local transforms).
    void evaluate(Pose& out) const;

    std::string stateName(int layer = 0) const;
    float normalizedTime(int layer = 0) const;
    bool inTransition(int layer = 0) const;
    Json stateJson() const;
    /// Seconds one cycle of a state takes at its current blend weights.
    float stateDuration(int layer, int state) const;

private:
    struct Motion {
        std::shared_ptr<const Clip> clip;
        MotionDef def;
    };
    struct State {
        const StateDef* def = nullptr;
        StateDef owned;  // one-shot clip states created by play()
        std::vector<Motion> motions;
    };
    struct Playing {
        int state = -1;
        float nt = 0.f;  // normalized time (cycles since entering)
        std::optional<bool> loop;
        bool returnToDefault = false;
        bool fresh = true;  // just entered: events at its start time still fire
    };
    struct Layer {
        const LayerDef* def = nullptr;
        std::vector<State> states;
        std::vector<bool> mask;
        Playing current;
        Playing previous;            // crossfade source (state), unless frozen
        bool frozenSource = false;   // source is a snapshot pose (interrupted transition)
        Pose frozen;
        float fadeElapsed = 0.f, fadeDuration = 0.f;
        bool fading = false;
        bool interruptible = false;
    };

    int resolveState(Layer& layer, const std::string& name, bool allowClips);
    void enter(Layer& layer, int state, float fade, float offset, bool interruptible);
    std::vector<float> weights(const Layer& layer, const Playing& p) const;
    bool looping(const Layer& layer, const Playing& p) const;
    void advance(Layer& layer, Playing& p, float dt) const;
    void samplePlaying(const Layer& layer, const Playing& p, Pose& out) const;
    void layerPose(const Layer& layer, Pose& out) const;
    bool conditionsHold(const TransitionDef& t) const;
    void consumeTriggers(const TransitionDef& t);
    Vec3 rootMotion(const Layer& layer, const Playing& p, float ntFrom, float ntTo) const;
    Vec3 horizontal(Vec3 rootLocal) const;

    std::shared_ptr<const Library> library_;
    std::shared_ptr<const ControllerDef> controller_;
    ClipResolver resolve_;
    std::vector<float> params_;
    std::vector<float> triggerAge_;  // seconds a trigger has been armed (-1 = not armed)
    std::vector<Layer> layers_;
    bool rootMotion_ = false;
    Vec3 up_{0, 1, 0};
    Mat4 rootParent_;  // rest global of the root bone's parent (model space)
    Mat4 rootParentInv_;
};

}  // namespace sky::anim
