#pragma once
// Sequences (`*.sequence.json`): cinematic timelines, the Sequencer / Timeline equivalent.
//
// A sequence is a list of tracks, each with keys at times in seconds:
//   property   animates any reflected field of an entity ("transform.position",
//              "light.intensity", "camera.fov", "mesh.color", "environment.sunElevation")
//              with linear / step / smooth / ease_in / ease_out / auto (spline) / bezier keys
//   camera     camera cuts: which camera is live from each key on
//   shot       procedural camera moves (orbit, dolly, crane, track, pan, static, path)
//              evaluated every tick, so they follow moving targets
//   event      Wander events at times (`on event "name"`)
//   animation  plays a state or clip on an entity's animator at times (with crossfades)
//
//   {"format": "skywalker.sequence", "version": 1, "name": "Intro", "duration": 8,
//    "tracks": [
//      {"type": "property", "entity": "Sun", "property": "light.intensity",
//       "keys": [{"t": 0, "value": 0.2}, {"t": 4, "value": 3, "ease": "smooth"}]},
//      {"type": "shot", "camera": "Cam", "keys": [{"t": 0, "duration": 4, "shot": "orbit",
//       "target": "Hero", "radius": 6, "height": 2, "from": 0, "to": 90}]},
//      {"type": "camera", "keys": [{"t": 0, "camera": "Cam"}, {"t": 4, "camera": "Closeup"}]},
//      {"type": "event", "keys": [{"t": 2.5, "event": "explode", "target": "Barrel"}]},
//      {"type": "animation", "entity": "Hero", "keys": [{"t": 0, "play": "Walk", "fade": 0.2}]}]}
//
// Evaluation here is pure (no scene): the engine's AnimationSystem applies the results.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky::anim {

enum class TrackType { Property, Camera, Shot, Event, Animation };
const char* toString(TrackType t);

struct SeqKey {
    float t = 0.f;
    Json value;               // property: number, [x,y,z], color ([r,g,b,a] or "#hex"), bool, string
    std::string ease = "linear";  // linear | step | smooth | ease_in | ease_out | auto | bezier
    float bezier[4] = {0.25f, 0.1f, 0.25f, 1.f};  // cubic-bezier(x1, y1, x2, y2) when ease == "bezier"
    Json data = Json::object();  // other track types: the key's fields (camera, event, play, shot...)
};

struct Track {
    TrackType type = TrackType::Property;
    std::string entity;    // property / animation tracks
    std::string property;  // "component.field"
    std::string camera;    // shot tracks: the camera entity they move
    std::vector<SeqKey> keys;  // sorted by time
    bool muted = false;
};

struct SequenceDef {
    std::string name;
    float duration = 0.f;  // 0 = up to the last key
    std::vector<Track> tracks;

    static Result<SequenceDef> fromJson(const Json& doc);
    Json toJson() const;
    float length() const;  // duration, or the end of the last key / shot
};

Result<SequenceDef> loadSequence(const std::string& absolutePath);
Status saveSequence(const std::string& absolutePath, const SequenceDef& def);

/// Applies an easing curve to s in [0, 1].
float ease(const SeqKey& key, float s);
/// The property value at time t (null for an empty track). Numbers and vectors interpolate;
/// bools and strings step.
Json evaluateProperty(const Track& track, float t);

/// Camera placement produced by a shot.
struct ShotPose {
    Vec3 position;
    Vec3 rotation;  // engine Euler degrees (camera looks down -Z)
    std::optional<float> fov;
};
/// World-space point an entity reference stands for (bounds center), or nullopt.
using PointLookup = std::function<std::optional<Vec3>(const std::string& entity)>;
/// The shot active at time t on a shot track (the last one started), evaluated.
std::optional<ShotPose> evaluateShot(const Track& track, float t, const PointLookup& lookup);
/// Euler rotation (engine degrees) for a camera at `eye` looking at `target`.
Vec3 lookRotation(Vec3 eye, Vec3 target, float roll = 0.f);

/// Shot kinds understood by evaluateShot().
const std::vector<std::string>& shotKinds();

}  // namespace sky::anim
