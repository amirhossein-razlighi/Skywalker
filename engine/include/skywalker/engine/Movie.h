#pragma once
// Movie render queue (docs/MOVIE_RENDER.md): offline, deterministic rendering of cinematics to
// PNG sequences and video files — the equivalent of Unreal's Movie Render Queue / Unity's Recorder.
//
//   what      a sequence (its whole length or a range), an inline camera path (keyframes or
//             procedural shots), or simply the scene camera for a duration
//   how       every render starts from the scene state (play snapshot, restored at the end); the
//             simulation (simulate=true) or only the cinematic systems (sequences, animators,
//             particles; simulate=false) advance with the engine's fixed 1/60 s ticks
//   sub-frame motion blur accumulates `shutter` sub-samples at fractional times: transforms are
//             interpolated between the two surrounding ticks, sequences/camera paths and the
//             effects clock (water, sky, GPU effects) are evaluated exactly, CPU particles are
//             placed along their velocity
//   output    PNG sequences (resumable), H.264 / 10-bit HEVC .mp4, ProRes .mov (AVFoundation)

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/anim/Sequence.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/MovieWriter.h"
#include "skywalker/render/Renderer.h"

namespace sky {

class Engine;

namespace movie {

// ---------------------------------------------------------------------------
// Frame timing (pure, tested)
// ---------------------------------------------------------------------------

/// Where the open shutter sits relative to a frame's time: centered (film convention, default),
/// opening at it, or closing at it.
enum class ShutterTiming { Center, Open, Close };
const char* toString(ShutterTiming t);
std::optional<ShutterTiming> shutterTimingFromName(const std::string& name);

/// How a frame's sample budget splits into temporal sub-frames (separate renders at different
/// times, accumulated: motion blur) and spatial samples (jittered, accumulated inside each render).
struct SampleSplit {
    int temporal = 1;
    int spatial = 1;
};
/// `samples` = the total budget per frame; with a shutter every sample becomes its own sub-frame
/// unless `shutterSamples` (> 0) fixes the number of sub-frames (each then gets samples/sub-frames).
SampleSplit splitSamples(int samples, double shutter, int shutterSamples);

struct Timing {
    int fps = 24;
    int firstFrame = 0;  // absolute frame number of the first output frame (time = number / fps)
    int frames = 0;      // output frames
    double shutter = 0;  // open fraction of the frame interval (0.5 = 180-degree shutter)
    int temporalSamples = 1;
    ShutterTiming timing = ShutterTiming::Center;

    double frameTime(int k) const { return static_cast<double>(firstFrame + k) / static_cast<double>(fps); }
    /// Movie times of the sub-frames of output frame k: stratified over the open shutter, ascending,
    /// clamped to [lo, hi) — the span of the shot the frame belongs to (no blur across a cut) and >= 0.
    std::vector<double> sampleTimes(int k, double lo = 0.0, double hi = std::numeric_limits<double>::infinity()) const;
};

/// The transform a sub-frame `alpha` (0..1) of the way from tick state `a` to tick state `b` shows:
/// position and scale lerp, rotation slerps. A jump too large for one tick (a teleport, a respawn)
/// is not smeared: it returns `b`.
Transform interpolateTransform(const Transform& a, const Transform& b, float alpha);

/// [lo, hi) of the shot containing `t`, given the sorted cut times of the movie.
std::pair<double, double> shotSpan(const std::vector<double>& cuts, double t);
/// Number of cuts at or before t (changes exactly when a new shot starts).
int shotIndex(const std::vector<double>& cuts, double t);

// ---------------------------------------------------------------------------
// Inline camera path
// ---------------------------------------------------------------------------

/// A camera move given right in movie_render (no sequence asset needed): keyframes of eye /
/// target / fov / roll (splined by default), or a list of procedural shots (orbit, dolly, crane,
/// track, pan, static, path, flyover — the sequence_camera_shot kinds; each shot start is a cut).
struct CameraPath {
    anim::Track eye, target, fov, roll;  // keyframes (property tracks)
    anim::Track shots;                   // procedural shots (a shot track)
    float aperture = 0.f;
    float focusDistance = 0.f;
    float tiltShift = 0.f;
    std::optional<float> nearPlane, farPlane;

    bool empty() const { return eye.keys.empty() && shots.keys.empty(); }
    bool hasShots() const { return !shots.keys.empty(); }
    /// End of the last key / shot (seconds).
    float length() const;
    /// Shot start times after the first (cuts).
    std::vector<double> cuts() const;
    /// The camera at time t. `lookup` resolves entity names used as shot targets; `base` supplies
    /// what the path does not set (clip planes).
    ViewCamera evaluate(double t, const anim::PointLookup& lookup, const ViewCamera& base) const;

    /// {"keys": [{"t", "eye", "target", "fov"?, "roll"?, "ease"?}...]} or {"shots": [{"shot", "duration",
    /// "t"?, ...sequence_camera_shot fields}]}, plus optional "aperture", "focus_distance", "near", "far".
    static Result<CameraPath> fromJson(const Json& j);
};

// ---------------------------------------------------------------------------
// Options and the job
// ---------------------------------------------------------------------------

struct Output {
    std::string path;  // absolute
    Codec codec = Codec::Png;
    double bitrateMbps = 0;  // H.264 / HEVC (0 = automatic)
};

struct Options {
    // What to render (at most one of sequencePlayer / sequenceAsset).
    EntityId sequencePlayer = kNoEntity;  // an entity whose sequencer plays the sequence
    std::string sequenceAsset;            // or a *.sequence.json with no player (a temporary one plays it)
    CameraPath camera;                    // inline camera (wins over the scene/sequence camera)
    EntityId cameraEntity = kNoEntity;    // or look through this camera entity
    bool simulate = false;                // full game simulation (scripts, physics) vs cinematic systems only

    // Range: movie time 0 is the start of the simulation (and of the sequence).
    double start = 0;
    double end = -1;  // < 0: the sequence / camera path length

    // Image
    int fps = 24;
    int width = 1920;
    int height = 1080;
    int samples = 8;          // per frame (see splitSamples)
    double shutter = -1;      // 0..1 of the frame interval; < 0 = the camera's motionBlur at the start
    int shutterSamples = 0;   // sub-frames per frame (0 = automatic)
    ShutterTiming shutterTiming = ShutterTiming::Center;
    int quality = 0;          // 0 full, 1 balanced, 2 fast
    bool clay = false;
    int debugView = 0;        // 9 = sketch
    int warmup = 4;           // frames rendered (not written) before the first one

    std::vector<Output> outputs;
    bool resume = false;  // PNG sequences: continue at the first missing frame
    std::string name;     // for reports ("Intro")
};

using ProgressFn = std::function<void(const Json& progress)>;

/// One movie render, advanced one render (a sub-frame or warmup frame) at a time so a host loop
/// (the editor) stays responsive; Engine::renderMovie runs it to completion. Owns the play
/// session: the scene is restored when it finishes, fails, is cancelled or destroyed.
class Job {
public:
    /// Validates, opens the outputs and starts the play session. `cancel` is polled before every render.
    static Result<std::unique_ptr<Job>> start(Engine& engine, Options options, ProgressFn progress,
                                              const std::atomic<bool>* cancel);
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    /// Does the next unit of work. Returns false once the job is over (finished, failed or cancelled).
    bool advance();
    bool done() const;
    /// Progress / final report: state (rendering|done|cancelled|failed), frames, outputs, timings.
    Json status() const;
    /// The final summary, or the error that stopped the render.
    Result<Json> result() const;

private:
    struct Impl;
    explicit Job(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Parses movie_render arguments (docs/MOVIE_RENDER.md): resolves entities, sequence assets and
/// output paths against the engine's project. Shared by the tool and `skywalker movie`.
Result<Options> parseOptions(Engine& engine, const Json& args);

}  // namespace movie
}  // namespace sky
