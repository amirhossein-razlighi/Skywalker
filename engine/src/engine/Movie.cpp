// Movie render queue: frame timing, inline camera paths and the render job (docs/MOVIE_RENDER.md).
//
// Sub-frame time. The simulation keeps its fixed 1/60 s tick (changing it would make a movie
// render diverge from the game it records). A sub-frame at movie time τ runs the simulation to
// the first tick at or after τ and shows the state between that tick and the previous one:
//   * entity transforms are interpolated (lerp / slerp) between the two tick states,
//   * sequences (property, camera-cut and shot tracks) and inline camera paths are evaluated
//     exactly at τ on top (so cuts are exact and camera moves are smooth curves, not polylines),
//   * the effects clock (water, sky, clouds, GPU particles, fluids, hair) is set to τ exactly,
//   * CPU particles are placed along their velocity from the tick to τ.
// Skeletal poses step at 60 Hz (documented limitation).

#include "skywalker/engine/Movie.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#include "skywalker/anim/AnimMath.h"
#include "skywalker/anim/AnimationSystem.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Engine.h"

namespace sky {
namespace movie {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

const char* toString(ShutterTiming t) {
    switch (t) {
        case ShutterTiming::Center: return "center";
        case ShutterTiming::Open: return "open";
        case ShutterTiming::Close: return "close";
    }
    return "center";
}

std::optional<ShutterTiming> shutterTimingFromName(const std::string& name) {
    if (name == "center") return ShutterTiming::Center;
    if (name == "open") return ShutterTiming::Open;
    if (name == "close") return ShutterTiming::Close;
    return std::nullopt;
}

SampleSplit splitSamples(int samples, double shutter, int shutterSamples) {
    samples = std::clamp(samples, 1, 1024);
    if (shutter <= 0.0) return {1, samples};
    const int temporal = shutterSamples > 0 ? std::clamp(shutterSamples, 2, 256) : std::clamp(samples, 2, 256);
    return {temporal, std::max(1, (samples + temporal - 1) / temporal)};
}

std::vector<double> Timing::sampleTimes(int k, double lo, double hi) const {
    const double t = frameTime(k);
    const double len = std::clamp(shutter, 0.0, 1.0) / static_cast<double>(fps);
    const int n = len > 0.0 ? std::max(1, temporalSamples) : 1;
    const double open = timing == ShutterTiming::Open ? t : timing == ShutterTiming::Close ? t - len : t - len * 0.5;
    lo = std::max(lo, 0.0);
    // The shot ends at `hi` (a cut): stay a hair before it, beyond float time resolution of the sequence clock.
    const double last = std::max(lo, hi - 1e-4);
    std::vector<double> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double s = len > 0.0 ? open + (static_cast<double>(i) + 0.5) / static_cast<double>(n) * len : t;
        out[static_cast<size_t>(i)] = std::clamp(s, lo, last);
    }
    return out;
}

std::pair<double, double> shotSpan(const std::vector<double>& cuts, double t) {
    double lo = 0.0, hi = std::numeric_limits<double>::infinity();
    for (double c : cuts) {
        if (c <= t) lo = std::max(lo, c);
        else hi = std::min(hi, c);
    }
    return {lo, hi};
}

int shotIndex(const std::vector<double>& cuts, double t) {
    return static_cast<int>(std::count_if(cuts.begin(), cuts.end(), [&](double c) { return c <= t; }));
}

Transform interpolateTransform(const Transform& a, const Transform& b, float alpha) {
    alpha = std::clamp(alpha, 0.f, 1.f);
    constexpr float kTeleport = 25.f;  // meters in one 1/60 s tick (1500 m/s): a jump, not motion
    if (alpha >= 1.f || distance(a.position, b.position) > kTeleport) return b;
    Transform out = b;
    out.position = lerp(a.position, b.position, alpha);
    out.scale = lerp(a.scale, b.scale, alpha);
    if (a.rotation.x != b.rotation.x || a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z) {
        out.rotation = anim::eulerDegFromQuat(anim::slerp(anim::Quat::fromEulerDeg(a.rotation), anim::Quat::fromEulerDeg(b.rotation), alpha));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Inline camera path
// ---------------------------------------------------------------------------

float CameraPath::length() const {
    float end = 0.f;
    for (const auto& k : eye.keys) end = std::max(end, k.t);
    for (const auto& k : shots.keys) end = std::max(end, k.t + k.data.get("duration").asFloat());
    return end;
}

std::vector<double> CameraPath::cuts() const {
    std::vector<double> out;
    for (size_t i = 1; i < shots.keys.size(); ++i) out.push_back(shots.keys[i].t);
    return out;
}

ViewCamera CameraPath::evaluate(double t, const anim::PointLookup& lookup, const ViewCamera& base) const {
    ViewCamera v = base;
    const float tf = static_cast<float>(t);
    if (hasShots()) {
        if (auto pose = anim::evaluateShot(shots, tf, lookup)) {
            Mat4 r = Mat4::rotateEulerDeg(pose->rotation);
            v.eye = pose->position;
            v.target = v.eye + normalize(r.transformDir({0, 0, -1}));
            v.up = normalize(r.transformDir({0, 1, 0}));
            if (pose->fov) v.fovDeg = *pose->fov;
        }
    } else if (!eye.keys.empty()) {
        Vec3 e = v.eye, tg = v.target;
        reflect::jsonToVec3(anim::evaluateProperty(eye, tf), e);
        if (!target.keys.empty()) reflect::jsonToVec3(anim::evaluateProperty(target, tf), tg);
        v.eye = e;
        v.target = tg;
        v.up = {0, 1, 0};
        if (!fov.keys.empty()) v.fovDeg = std::clamp(anim::evaluateProperty(fov, tf).asFloat(v.fovDeg), 1.f, 170.f);
        if (!roll.keys.empty()) {  // rotate "up" about the view direction (Rodrigues)
            float r = radians(anim::evaluateProperty(roll, tf).asFloat(0.f));
            Vec3 f = normalize(v.target - v.eye);
            Vec3 u = v.up;
            v.up = normalize(u * std::cos(r) + cross(f, u) * std::sin(r) + f * (dot(f, u) * (1.f - std::cos(r))));
        }
    }
    v.aperture = aperture;
    v.focusDistance = focusDistance;
    v.tiltShift = tiltShift;
    v.motionBlur = 0.f;
    if (nearPlane) v.nearPlane = *nearPlane;
    if (farPlane) v.farPlane = *farPlane;
    v.orthographic = false;
    return v;
}

Result<CameraPath> CameraPath::fromJson(const Json& j) {
    CameraPath p;
    if (!j.isObject()) return Error::make("invalid_camera", "camera must be an object with \"keys\" or \"shots\"");
    static const std::vector<std::string> top{"keys", "shots", "aperture", "focus_distance", "tilt_shift", "near", "far"};
    for (const auto& [name, v] : j.members()) {
        if (std::find(top.begin(), top.end(), name) == top.end()) {
            std::string g = str::closest(name, top, 3);
            return Error::make("invalid_camera", "unknown camera field \"" + name + "\"", g.empty() ? "fields: keys, shots, aperture, focus_distance, tilt_shift, near, far" : "did you mean \"" + g + "\"?");
        }
    }
    const bool hasKeys = j.get("keys").isArray() && j.get("keys").size() > 0;
    const bool hasShots = j.get("shots").isArray() && j.get("shots").size() > 0;
    if (hasKeys == hasShots) {
        return Error::make("invalid_camera", "camera needs either \"keys\" (eye/target keyframes) or \"shots\" (procedural moves)",
                           "e.g. {\"keys\": [{\"t\": 0, \"eye\": [0, 5, 20], \"target\": [0, 1, 0]}, {\"t\": 4, \"eye\": [15, 4, 10], \"target\": [0, 1, 0]}]}");
    }
    p.aperture = std::max(0.f, j.get("aperture").asFloat(0.f));
    p.focusDistance = std::max(0.f, j.get("focus_distance").asFloat(0.f));
    p.tiltShift = std::clamp(j.get("tilt_shift").asFloat(0.f), 0.f, 1.f);
    if (j.get("near").isNumber()) p.nearPlane = std::max(0.001f, j.get("near").asFloat());
    if (j.get("far").isNumber()) p.farPlane = std::max(1.f, j.get("far").asFloat());
    // Both forms reuse the sequence format (and its validation): keyframes become property tracks,
    // shots a shot track.
    Json tracks = Json::array();
    if (hasKeys) {
        Json eyeKeys = Json::array(), targetKeys = Json::array(), fovKeys = Json::array(), rollKeys = Json::array();
        static const std::vector<std::string> keyFields{"t", "eye", "target", "fov", "roll", "ease"};
        size_t i = 0;
        for (const auto& k : j.get("keys").elements()) {
            std::string where = "camera key " + std::to_string(i++);
            if (!k.isObject()) return Error::make("invalid_camera", where + " must be an object {t, eye, target}");
            for (const auto& [name, v] : k.members()) {
                if (std::find(keyFields.begin(), keyFields.end(), name) == keyFields.end()) {
                    std::string g = str::closest(name, keyFields, 3);
                    return Error::make("invalid_camera", where + ": unknown field \"" + name + "\"", g.empty() ? "fields: t, eye, target, fov, roll, ease" : "did you mean \"" + g + "\"?");
                }
            }
            Vec3 e, tg;
            if (!k.get("t").isNumber() || !reflect::jsonToVec3(k.get("eye"), e) || !reflect::jsonToVec3(k.get("target"), tg)) {
                return Error::make("invalid_camera", where + " needs \"t\" (seconds), \"eye\" and \"target\" ([x, y, z])");
            }
            Json ease = k.contains("ease") ? k.get("ease") : Json("auto");  // splined through the keys by default
            auto key = [&](const Json& value) { return Json::object({{"t", k.get("t")}, {"value", value}, {"ease", ease}}); };
            eyeKeys.push(key(k.get("eye")));
            targetKeys.push(key(k.get("target")));
            if (k.get("fov").isNumber()) fovKeys.push(key(k.get("fov")));
            if (k.get("roll").isNumber()) rollKeys.push(key(k.get("roll")));
        }
        auto track = [](const char* prop, Json keys) {
            return Json::object({{"type", "property"}, {"entity", "camera"}, {"property", prop}, {"keys", std::move(keys)}});
        };
        tracks.push(track("camera.eye", eyeKeys));
        tracks.push(track("camera.target", targetKeys));
        tracks.push(track("camera.fov", fovKeys));
        tracks.push(track("camera.roll", rollKeys));
    } else {
        Json keys = Json::array();
        float next = 0.f;
        for (const auto& s : j.get("shots").elements()) {
            if (!s.isObject()) return Error::make("invalid_camera", "each shot must be an object {shot, duration, ...}");
            Json k = s;
            if (!k.get("t").isNumber()) k["t"] = next;  // shots play back to back by default
            next = k.get("t").asFloat() + std::max(0.f, k.get("duration").asFloat());
            keys.push(std::move(k));
        }
        tracks.push(Json::object({{"type", "shot"}, {"camera", "camera"}, {"keys", std::move(keys)}}));
    }
    auto def = anim::SequenceDef::fromJson(Json::object({{"tracks", tracks}}));
    if (!def) {
        Error e = def.error();
        e.code = "invalid_camera";
        for (const char* where : {"track 0 key", "track 1 key", "track 2 key", "track 3 key"}) {
            if (size_t at = e.message.find(where); at != std::string::npos) e.message.replace(at, std::strlen(where), hasKeys ? "camera key" : "camera shot");
        }
        return e;
    }
    if (hasKeys) {
        p.eye = def->tracks[0];
        p.target = def->tracks[1];
        p.fov = def->tracks[2];
        p.roll = def->tracks[3];
    } else {
        p.shots = def->tracks[0];
    }
    return p;
}

// ---------------------------------------------------------------------------
// The render job
// ---------------------------------------------------------------------------

namespace {

constexpr double kTick = 1.0 / 60.0;  // Engine::kFixedDt, in double for time arithmetic

double seconds(std::chrono::steady_clock::duration d) { return std::chrono::duration<double>(d).count(); }
double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

anim::PointLookup pointLookup(const Scene& s) {
    return [&s](const std::string& ref) -> std::optional<Vec3> {
        EntityId id = s.find(ref);
        if (!id) return std::nullopt;
        if (s.get<MeshRenderer>(id)) return s.localBounds(id).transformed(s.worldMatrix(id)).center();
        return s.worldMatrix(id).translation();
    };
}

EntityId primaryCamera(const Scene& s) {
    for (EntityId e : s.entities()) {
        const Camera* c = s.get<Camera>(e);
        if (c && c->primary && s.isActive(e)) return e;
    }
    return kNoEntity;
}

}  // namespace

struct Job::Impl {
    enum class State { Rendering, Done, Cancelled, Failed };

    Engine& engine;
    Options o;
    ProgressFn progress;
    const std::atomic<bool>* cancel = nullptr;

    Timing timing;
    SampleSplit split;
    std::vector<double> cuts;  // movie times where a new shot starts (sorted)
    EntityId seq = kNoEntity;  // the sequencer rendered (possibly a temporary one)
    std::shared_ptr<const anim::SequenceDef> seqDef;
    float seqSpeed = 1.f;
    float seqLength = 0.f;
    bool seqLoop = false;
    std::vector<PngSequence> pngs;
    std::vector<std::unique_ptr<VideoWriter>> videos;
    bool session = false;

    // Simulation state
    long tick = 0;
    std::unordered_map<EntityId, Transform> prev;  // transforms at tick - 1
    bool prevValid = false;

    // Progress
    int firstK = 0;  // first output frame of this run (> 0 when resuming)
    int warmupLeft = 0;
    int k = 0;
    std::vector<double> subTimes;
    size_t sub = 0;
    FrameBuffer acc;
    struct View {
        int shot = -1;
        EntityId camera = kNoEntity;
        bool operator==(const View&) const = default;
    };
    std::optional<View> lastView;
    int written = 0;
    State state = State::Rendering;
    std::optional<Error> error;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    double frameSeconds = 0;  // sum over written frames
    std::string cameraLabel;

    explicit Impl(Engine& e) : engine(e) {}

    double seqTime(double tau) const {
        double t = tau * static_cast<double>(seqSpeed);
        if (seqLoop && seqLength > 0.f) return std::fmod(t, static_cast<double>(seqLength));
        return std::min(t, static_cast<double>(seqLength));
    }

    // --- Setup -----------------------------------------------------------------------

    Status setup() {
        Scene& scene = engine.scene();
        if (engine.playState() != PlayState::Editing) {
            return Error::make("busy", "the simulation is running", "stop it (sim_control {\"action\": \"stop\"}) first: movies start from the scene state");
        }
        if (o.width < 16 || o.height < 16 || o.width > 7680 || o.height > 4320) {
            return Error::make("invalid_size", "movie size must be between 16x16 and 7680x4320");
        }
        if (o.fps < 1 || o.fps > 240) return Error::make("invalid_fps", "fps must be between 1 and 240");
        if (o.outputs.empty()) return Error::make("no_output", "no output given", "e.g. \"output\": \"renders/intro.mp4\"");
        // The sequence: its definition gives the length and the cuts.
        std::string seqPath = o.sequenceAsset;
        if (o.sequencePlayer) {
            const SequencePlayer* sp = scene.get<SequencePlayer>(o.sequencePlayer);
            if (!sp || sp->sequence.empty()) return Error::make("no_sequencer", formatEntityRef(o.sequencePlayer) + " has no sequencer with a sequence");
            seqPath = sp->sequence;
            seqSpeed = sp->speed > 0.f ? sp->speed : 1.f;
            seqLoop = sp->loop;
        }
        if (!seqPath.empty()) {
            auto def = engine.animation().sequence(seqPath);
            if (!def) return def.error();
            seqDef = *def;
            seqLength = seqDef->length();
            if (o.name.empty()) o.name = seqDef->name;
        }
        // Range.
        double end = o.end;
        if (end < 0) {
            if (seqDef) end = seqLength / seqSpeed;
            else if (!o.camera.empty()) end = o.camera.length();
        }
        if (end <= o.start) {
            return Error::make("invalid_range", "nothing to render: the range is empty",
                               "give \"duration\" (seconds) or \"end\", or render a sequence / camera path (their length is the default)");
        }
        timing.fps = o.fps;
        timing.firstFrame = static_cast<int>(std::llround(std::max(0.0, o.start) * o.fps));
        timing.frames = std::max(1, static_cast<int>(std::llround((end - std::max(0.0, o.start)) * o.fps)));
        timing.timing = o.shutterTiming;
        // Outputs.
        bool anyVideo = false;
        for (const Output& out : o.outputs) {
            if (out.codec == Codec::Png) pngs.emplace_back(out.path);
            else anyVideo = true;
        }
        if (o.resume) {
            if (anyVideo) return Error::make("invalid_resume", "resume works with PNG sequences only (a video file cannot be appended to)",
                                             "render the PNG sequence with resume, then encode it, or re-render the video");
            int first = timing.firstFrame + timing.frames;
            for (const auto& p : pngs) first = std::min(first, p.firstMissing(timing.firstFrame, timing.firstFrame + timing.frames - 1));
            firstK = first - timing.firstFrame;
        }
        for (const Output& out : o.outputs) {
            if (out.codec == Codec::Png) continue;
            VideoSettings vs{out.path, out.codec, o.width, o.height, o.fps, out.bitrateMbps};
            auto w = openVideoWriter(vs);
            if (!w) return w.error();
            videos.push_back(std::move(*w));
        }
        // Canonical start: the scene exactly as saved (a JSON round trip, what every play session restores
        // to on stop), so the first render after an edit matches every later one bit for bit.
        {
            ChangeObserver* observer = scene.observer();
            Json saved = scene.toJson();
            if (Status s = scene.loadJson(saved); !s) return s;
            scene.setObserver(observer);
        }
        // The play session: everything below is undone by stop().
        engine.play();
        engine.pause();  // the job advances the simulation itself
        session = true;
        engine.audio().setBusVolume("master", 0.f);  // ticks run far from real time: keep the speakers quiet
        if (seqDef) {
            seq = o.sequencePlayer;
            if (!seq) {  // an asset with no player in the scene: a temporary one (gone when the session ends)
                seq = scene.create("Movie Sequencer");
                if (Status s = scene.patchComponent(seq, "sequencer", Json::object({{"sequence", seqPath}, {"playOnStart", false}})); !s) return s;
            }
            if (Status s = engine.animation().playSequence(seq, 0.f); !s) return s;
            collectSequenceCuts(end);
        } else if (!o.camera.empty()) {
            cuts = o.camera.cuts();
        }
        std::sort(cuts.begin(), cuts.end());
        // Shutter: explicit, or the camera's own motion blur setting.
        double shutter = o.shutter;
        EntityId cam = o.cameraEntity;
        if (!cam && o.camera.empty()) cam = seq ? engine.animation().sequenceCamera(seq, static_cast<float>(seqTime(timing.frameTime(0)))) : kNoEntity;
        if (!cam && o.camera.empty()) cam = primaryCamera(scene);
        if (shutter < 0) {
            const Camera* c = cam ? scene.get<Camera>(cam) : nullptr;
            shutter = c ? std::clamp(static_cast<double>(c->motionBlur), 0.0, 1.0) : 0.0;
        }
        timing.shutter = std::clamp(shutter, 0.0, 1.0);
        split = splitSamples(o.samples, timing.shutter, o.shutterSamples);
        timing.temporalSamples = split.temporal;
        cameraLabel = !o.camera.empty() ? "inline path" : cam ? scene.record(cam)->name : "editor camera";
        k = firstK;
        warmupLeft = firstK < timing.frames ? std::clamp(o.warmup, 0, 240) : 0;
        if (firstK >= timing.frames) finish(State::Done);  // resumed a complete sequence
        return {};
    }

    /// Times where the shown camera (or its shot) changes, in movie time.
    void collectSequenceCuts(double end) {
        std::vector<float> candidates;
        for (const auto& t : seqDef->tracks) {
            if (t.muted) continue;
            if (t.type == anim::TrackType::Camera || t.type == anim::TrackType::Shot) {
                for (const auto& key : t.keys) {
                    if (key.t > 0.f) candidates.push_back(key.t);
                }
            }
        }
        auto identity = [&](float t) {
            EntityId cam = engine.animation().sequenceCamera(seq, t);
            int shot = -1;
            for (const auto& tr : seqDef->tracks) {
                if (tr.muted || tr.type != anim::TrackType::Shot || engine.scene().find(tr.camera) != cam) continue;
                for (size_t i = 0; i < tr.keys.size(); ++i) {
                    if (tr.keys[i].t <= t) shot = static_cast<int>(i);
                }
            }
            return std::make_pair(cam, shot);
        };
        std::vector<double> seqCuts;
        for (float c : candidates) {
            if (identity(c - 1e-3f) != identity(c + 1e-3f)) seqCuts.push_back(c);
        }
        std::sort(seqCuts.begin(), seqCuts.end());
        seqCuts.erase(std::unique(seqCuts.begin(), seqCuts.end()), seqCuts.end());
        const double period = seqLength / seqSpeed;
        for (int loop = 0;; ++loop) {
            const double base = loop * period;
            if (base > end || (!seqLoop && loop > 0) || period <= 0) break;
            if (loop > 0) cuts.push_back(base);  // wrapping around is a cut too
            for (double c : seqCuts) cuts.push_back(base + c / seqSpeed);
        }
    }

    // --- Simulation ------------------------------------------------------------------

    void tickOnce() {
        if (o.simulate) {
            engine.step(1);
        } else {  // cinematic systems only: sequences, animators, particles
            engine.animation().tick(Engine::kFixedDt);
            engine.particles().update(engine.scene(), Engine::kFixedDt);
        }
    }

    /// Simulates up to the first tick at or after `tau`, remembering the state one tick before it.
    void advanceTo(double tau) {
        const long target = std::max(0L, static_cast<long>(std::ceil(tau / kTick - 1e-6)));
        while (tick < target) {
            if (tick == target - 1) {
                prev.clear();
                Scene& s = engine.scene();
                for (EntityId e : s.entities()) {
                    if (const Transform* t = s.get<Transform>(e)) prev.emplace(e, *t);
                }
                prevValid = true;
            }
            tickOnce();
            ++tick;
        }
    }

    // --- Rendering ---------------------------------------------------------------------

    /// Renders the scene at movie time `tau`. `frameStart`: the first render of an output (or warmup)
    /// frame, where cuts reset temporal state.
    Result<Image> render(double tau, int samples, int sampleOffset, float exposureDt, bool frameStart) {
        advanceTo(tau);
        Scene& scene = engine.scene();
        const double tickTime = static_cast<double>(tick) * kTick;
        const float alpha = tick == 0 || !prevValid ? 1.f : static_cast<float>(std::clamp((tau - (tickTime - kTick)) / kTick, 0.0, 1.0));

        // Everything below is restored when this scope ends (also on errors).
        std::vector<std::pair<EntityId, Transform>> restore;
        anim::AnimationSystem::FrameOverrides overrides;
        struct Restore {
            Impl& job;
            std::vector<std::pair<EntityId, Transform>>& transforms;
            anim::AnimationSystem::FrameOverrides& ov;
            ~Restore() {
                job.engine.particles().setRenderTimeOffset(0.f);
                job.engine.setEffectsTimeOverride(std::nullopt);
                job.engine.animation().endFrame(ov);
                for (auto it = transforms.rbegin(); it != transforms.rend(); ++it) {
                    if (Transform* t = job.engine.scene().get<Transform>(it->first)) *t = it->second;
                }
            }
        } guard{*this, restore, overrides};

        // 1. Transforms between the two ticks around tau.
        if (alpha < 1.f) {
            for (EntityId e : scene.entities()) {
                Transform* t = scene.get<Transform>(e);
                auto it = t ? prev.find(e) : prev.end();
                if (it == prev.end()) continue;
                const Transform& a = it->second;
                if (a.position == t->position && a.rotation == t->rotation && a.scale == t->scale) continue;
                restore.emplace_back(e, *t);
                *t = interpolateTransform(a, *t, alpha);
            }
        }
        // 2. The sequence exactly at tau (cameras, shots, keyed properties).
        if (seq) overrides = engine.animation().overrideSequenceAt(seq, static_cast<float>(seqTime(tau)));
        // 3. Clocks: effects at tau; CPU particles back along their velocity from the tick.
        engine.setEffectsTimeOverride(tau);
        engine.particles().setRenderTimeOffset(static_cast<float>(tau - tickTime));
        // 4. The view, rendered through Engine::capture: it holds the machine-wide GPU job lock for
        // this one sub-frame only (other processes' renders interleave between them) and the
        // renderer commits a command buffer per sample, so no clip ever becomes one long GPU job.
        CaptureOptions co;
        co.width = o.width;
        co.height = o.height;
        co.editorOverlays = false;
        co.annotate = false;
        co.listVisible = false;
        co.samples = samples;
        co.debugView = o.debugView;
        co.clay = o.clay;
        co.quality = o.quality;
        co.offline.enabled = true;
        co.offline.sampleOffset = sampleOffset;
        co.offline.exposureDt = exposureDt;
        View view;
        view.shot = shotIndex(cuts, tau);
        if (!o.camera.empty()) {
            ViewCamera base;
            ViewCamera sc;
            if (sceneCamera(scene, sc)) {
                base.nearPlane = sc.nearPlane;
                base.farPlane = sc.farPlane;
            }
            co.hasCustomView = true;
            co.customView = o.camera.evaluate(tau, pointLookup(scene), base);
        } else if (o.cameraEntity) {
            co.cameraEntity = o.cameraEntity;
            view.camera = o.cameraEntity;
        } else {
            co.useSceneCamera = true;
            view.camera = primaryCamera(scene);
        }
        if (frameStart) {
            co.resetHistory = !lastView || !(*lastView == view);  // first frame, or a cut: re-meter, drop history
            lastView = view;
        }
        auto cap = engine.capture(co);
        if (!cap) return cap.error();
        return std::move(cap->image);
    }

    // --- Work units ----------------------------------------------------------------------

    Status step() {
        const float frameDt = 1.f / static_cast<float>(o.fps);
        if (warmupLeft > 0) {  // the frames before the first one, not written: temporal state settles
            const double t = std::max(0.0, timing.frameTime(firstK - warmupLeft));
            --warmupLeft;
            auto r = render(t, 1, 0, frameDt, true);
            if (!r && r.error().code == "gpu_error") {  // nothing is written from warmup frames: carry on
                log::warn("movie", "warmup frame hit a GPU error (" + r.error().message + "), continuing");
                return {};
            }
            return r ? Status{} : Status(r.error());
        }
        if (subTimes.empty()) {
            auto [lo, hi] = shotSpan(cuts, timing.frameTime(k));
            subTimes = timing.sampleTimes(k, lo, hi);
            sub = 0;
            acc = FrameBuffer(o.width, o.height);
            frameStart_ = std::chrono::steady_clock::now();
            retried = false;
        }
        const float weight = 1.f / static_cast<float>(subTimes.size());
        const int spatial = retried ? std::max(1, split.spatial / 2) : split.spatial;
        auto img = render(subTimes[sub], spatial, static_cast<int>(sub) * spatial, frameDt * weight, sub == 0);
        if (!img) {
            // A faulted GPU frame (watchdog timeout, page fault) never reaches the outputs: the whole
            // frame is rendered again once with half the samples (the renderer is in safe mode by then),
            // and the render stops if that fails too.
            if (img.error().code != "gpu_error" || retried) return img.error();
            log::warn("movie", "frame " + std::to_string(timing.firstFrame + k) + ": " + img.error().message + "; retrying it at lower samples");
            retried = true;
            ++gpuRetries;
            sub = 0;
            acc = FrameBuffer(o.width, o.height);
            lastView.reset();  // re-meter after the fault
            return {};
        }
        acc.accumulate(*img, weight);
        if (++sub < subTimes.size()) return {};
        // The frame is complete.
        const int number = timing.firstFrame + k;
        for (const auto& p : pngs) {
            if (Status s = p.write(number, acc); !s) return s;
        }
        for (auto& v : videos) {
            if (Status s = v->append(acc); !s) return s;
        }
        frameSeconds += seconds(std::chrono::steady_clock::now() - frameStart_);
        ++written;
        ++k;
        subTimes.clear();
        if (k >= timing.frames) {
            finish(State::Done);
        } else {
            report();
        }
        return {};
    }
    std::chrono::steady_clock::time_point frameStart_;
    bool retried = false;  // this frame already failed once on the GPU
    int gpuRetries = 0;

    void report() {
        Json p = status();
        p["type"] = "movie_progress";
        engine.emitEvent(p);
        if (progress) progress(p);
    }

    void finish(State st, std::optional<Error> err = std::nullopt) {
        if (state != State::Rendering) return;
        for (auto& v : videos) {
            Status s = v->finish();
            if (!s && st == State::Done) {
                st = State::Failed;
                err = s.error();
            }
        }
        videos.clear();
        if (session) {
            engine.stop();  // restores the scene (and the audio mix) from the play snapshot
            session = false;
        }
        state = st;
        error = std::move(err);
        Json p = status();
        p["type"] = "movie_finished";
        engine.emitEvent(p);
        if (progress) progress(p);
        if (st == State::Failed) log::warn("movie", "render failed: " + (error ? error->message : std::string("?")));
    }

    Json status() const {
        static const char* kStates[] = {"rendering", "done", "cancelled", "failed"};
        const int total = timing.frames - firstK;  // frames this run renders
        const double elapsed = seconds(std::chrono::steady_clock::now() - started);
        const double perFrame = written > 0 ? frameSeconds / written : 0.0;
        Json outs = Json::array();
        for (const Output& out : o.outputs) {
            outs.push(Json::object({{"path", out.codec == Codec::Png ? PngSequence(out.path).pattern() : out.path},
                                    {"codec", codecName(out.codec)}}));
        }
        Json j = Json::object({{"state", kStates[static_cast<int>(state)]},
                               {"name", o.name},
                               {"frame", written},
                               {"frames", std::max(total, 0)},
                               {"percent", total > 0 ? std::round(1000.0 * written / total) / 10.0 : 100.0},
                               {"first_frame", timing.firstFrame},
                               {"last_frame", timing.firstFrame + timing.frames - 1},
                               {"fps", o.fps},
                               {"width", o.width},
                               {"height", o.height},
                               {"duration", round3(static_cast<double>(timing.frames) / o.fps)},
                               {"camera", cameraLabel},
                               {"simulate", o.simulate},
                               {"shutter", round3(timing.shutter)},
                               {"samples", Json::object({{"temporal", split.temporal}, {"spatial", split.spatial}})},
                               {"elapsed_s", round3(elapsed)},
                               {"ms_per_frame", std::round(perFrame * 10000.0) / 10.0},
                               {"outputs", outs}});
        if (state == State::Rendering && written > 0) j["eta_s"] = std::round(perFrame * (total - written));
        if (firstK > 0) j["resumed_at"] = timing.firstFrame + firstK;
        if (gpuRetries > 0) j["gpu_retries"] = gpuRetries;
        if (error) {
            j["error"] = error->message;
            if (!error->hint.empty()) j["hint"] = error->hint;
        }
        return j;
    }
};

Job::Job(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Job::~Job() {
    if (impl_) impl_->finish(Impl::State::Cancelled);
}

Result<std::unique_ptr<Job>> Job::start(Engine& engine, Options options, ProgressFn progress, const std::atomic<bool>* cancel) {
    auto impl = std::make_unique<Impl>(engine);
    impl->o = std::move(options);
    impl->progress = std::move(progress);
    impl->cancel = cancel;
    if (Status s = impl->setup(); !s) {
        impl->videos.clear();  // unfinished video files are discarded
        if (impl->session) engine.stop();
        impl->session = false;
        impl->state = Impl::State::Failed;
        return s.error();
    }
    return std::unique_ptr<Job>(new Job(std::move(impl)));
}

bool Job::advance() {
    Impl& j = *impl_;
    if (j.state != Impl::State::Rendering) return false;
    if (j.cancel && j.cancel->load()) {
        j.finish(Impl::State::Cancelled);
        return false;
    }
    if (j.engine.playState() == PlayState::Editing) {  // someone stopped the play session under us
        j.session = false;
        j.finish(Impl::State::Failed, Error::make("interrupted", "the simulation was stopped during the movie render"));
        return false;
    }
    if (Status s = j.step(); !s) {
        j.finish(Impl::State::Failed, s.error());
        return false;
    }
    return j.state == Impl::State::Rendering;
}

bool Job::done() const { return impl_->state != Impl::State::Rendering; }

Json Job::status() const { return impl_->status(); }

Result<Json> Job::result() const {
    if (impl_->state == Impl::State::Failed) return impl_->error.value_or(Error::make("render_failed", "the movie render failed"));
    return impl_->status();
}

}  // namespace movie

// ---------------------------------------------------------------------------
// Engine entry points
// ---------------------------------------------------------------------------

Result<Json> Engine::renderMovie(const movie::Options& options, const std::function<void(const Json&)>& progress) {
    if (movie_) return Error::make("busy", "a movie is already rendering", "wait for it, or cancel it: movie_render {\"action\": \"cancel\"}");
    movieCancel_ = false;
    auto job = movie::Job::start(*this, options, progress, &movieCancel_);
    if (!job) return job.error();
    movie_ = std::move(*job);  // visible to movieStatus() / cancelMovie() while it runs
    while (movie_->advance()) {
    }
    Result<Json> result = movie_->result();
    lastMovie_ = movie_->status();
    movie_.reset();
    return result;
}

Status Engine::startMovie(const movie::Options& options) {
    if (movie_) return Error::make("busy", "a movie is already rendering", "wait for it, or cancel it: movie_render {\"action\": \"cancel\"}");
    movieCancel_ = false;
    auto job = movie::Job::start(*this, options, {}, &movieCancel_);
    if (!job) return job.error();
    movie_ = std::move(*job);
    if (movie_->done()) {  // nothing left to render (a resumed, complete sequence)
        lastMovie_ = movie_->status();
        movie_.reset();
    }
    return {};
}

Json Engine::movieStatus() const {
    if (movie_) return movie_->status();
    if (!lastMovie_.isNull()) return lastMovie_;
    return Json::object({{"state", "idle"}});
}

}  // namespace sky
