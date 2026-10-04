// Movie render queue tool (docs/MOVIE_RENDER.md): movie_render renders sequences, inline camera
// paths or the scene camera to PNG sequences and video files; also reports and cancels renders.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "ToolHelpers.h"
#include "skywalker/core/Strings.h"
#include "skywalker/engine/Movie.h"

namespace sky {

namespace fs = std::filesystem;

namespace movie {

namespace {

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && str::lower(s).compare(s.size() - n, n, suffix) == 0;
}

std::string slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "movie" : out;
}

Result<Output> parseOutput(Engine& engine, const Json& j, const Json& args) {
    Output out;
    std::string path;
    std::string codec;
    if (j.isString()) {
        path = j.asString();
        codec = args.get("codec").asString();
        out.bitrateMbps = args.get("bitrate_mbps").asFloat(0.f);
    } else if (j.isObject()) {
        path = j.get("path").asString();
        codec = j.get("codec").asString();
        out.bitrateMbps = j.get("bitrate_mbps").asFloat(0.f);
    }
    if (path.empty()) return Error::make("invalid_output", "an output needs a path", "e.g. \"renders/intro.mp4\" or \"renders/intro/frame_####.png\"");
    out.path = engine.resolvePath(path);
    out.codec = codecForPath(out.path);
    if (!codec.empty()) {
        auto c = codecFromName(codec);
        if (!c) {
            std::string g = str::closest(codec, codecNames(), 3);
            return Error::make("invalid_codec", "unknown codec \"" + codec + "\"", g.empty() ? "codecs: png, h264, hevc, prores, prores4444" : "did you mean \"" + g + "\"?");
        }
        out.codec = *c;
        if (out.codec != Codec::Png && codecForPath(out.path) == Codec::Png) {
            return Error::make("invalid_output", std::string(codecName(out.codec)) + " needs a .mp4 or .mov path (got " + path + ")");
        }
    }
    if (isVideo(out.codec) && !videoEncodingAvailable()) {
        return Error::make("unsupported_codec", std::string("this build cannot encode ") + codecName(out.codec) + " video",
                           "render a PNG sequence instead: \"output\": \"renders/name/frame_####.png\"");
    }
    out.bitrateMbps = std::max(0.0, out.bitrateMbps);
    return out;
}

}  // namespace

Result<Options> parseOptions(Engine& engine, const Json& a) {
    Options o;
    Scene& s = engine.scene();
    // What to render.
    if (a.contains("sequence")) {
        const Json& ref = a.get("sequence");
        if (ref.isString() && endsWith(ref.asString(), ".sequence.json")) {
            std::string path = ref.asString();
            for (EntityId e : s.entities()) {  // prefer the scene's own player (its speed and loop apply)
                const SequencePlayer* sp = s.get<SequencePlayer>(e);
                if (sp && sp->sequence == path) o.sequencePlayer = e;
            }
            if (!o.sequencePlayer) {
                if (!fs::exists(engine.resolvePath(path))) return Error::make("not_found", "no sequence " + path, "list them with asset_list {\"type\": \"sequence\"}");
                o.sequenceAsset = path;
            }
        } else {
            auto id = tools::resolve(engine, ref);
            if (!id) return id.error();
            const SequencePlayer* sp = s.get<SequencePlayer>(*id);
            if (!sp || sp->sequence.empty()) {
                return Error::make("no_sequencer", formatEntityRef(*id) + " has no sequencer with a sequence",
                                   "pass a .sequence.json path or the entity sequence_create made");
            }
            o.sequencePlayer = *id;
        }
    }
    if (a.contains("camera")) {
        auto path = CameraPath::fromJson(a.get("camera"));
        if (!path) return path.error();
        o.camera = std::move(*path);
        for (const auto& k : o.camera.shots.keys) {  // shot targets must exist
            if (k.data.get("target").isString()) {
                if (auto id = tools::resolve(engine, k.data.get("target")); !id) return id.error();
            }
        }
    }
    if (a.contains("camera_entity")) {
        auto id = tools::resolve(engine, a.get("camera_entity"));
        if (!id) return id.error();
        if (!s.get<Camera>(*id)) return Error::make("not_a_camera", formatEntityRef(*id) + " has no camera component");
        o.cameraEntity = *id;
    }
    o.simulate = a.get("simulate").asBool(false);
    // Range.
    o.start = std::max(0.0, a.get("start").asNumber(0.0));
    if (a.contains("end")) o.end = a.get("end").asNumber();
    if (a.contains("duration")) o.end = o.start + std::max(0.0, a.get("duration").asNumber());
    // Image.
    o.fps = static_cast<int>(std::clamp<int64_t>(a.get("fps").asInt(24), 1, 120));
    if (a.contains("resolution")) {
        static const std::vector<std::pair<std::string, std::pair<int, int>>> presets{
            {"720p", {1280, 720}}, {"1080p", {1920, 1080}}, {"1440p", {2560, 1440}}, {"4k", {3840, 2160}}, {"2160p", {3840, 2160}}};
        std::string r = str::lower(a.get("resolution").asString());
        auto it = std::find_if(presets.begin(), presets.end(), [&](const auto& p) { return p.first == r; });
        if (it == presets.end()) return Error::make("invalid_resolution", "unknown resolution \"" + r + "\"", "720p, 1080p, 1440p or 4k");
        o.width = it->second.first;
        o.height = it->second.second;
    }
    o.width = static_cast<int>(a.get("width").asInt(o.width));
    o.height = static_cast<int>(a.get("height").asInt(o.height));
    if (o.width < 16 || o.height < 16 || o.width > 4096 || o.height > 4096 || static_cast<int64_t>(o.width) * o.height > 3840LL * 2160LL) {
        return Error::make("invalid_size", "movie size " + std::to_string(o.width) + "x" + std::to_string(o.height) + " is out of range",
                           "from 16x16 up to 3840x2160 (4K UHD)");
    }
    o.samples = static_cast<int>(std::clamp<int64_t>(a.get("samples").asInt(8), 1, 256));
    if (a.contains("shutter")) o.shutter = std::clamp(a.get("shutter").asNumber(), 0.0, 1.0);
    o.shutterSamples = static_cast<int>(std::clamp<int64_t>(a.get("shutter_samples").asInt(0), 0, 256));
    if (a.contains("shutter_timing")) {
        auto t = shutterTimingFromName(a.get("shutter_timing").asString());
        if (!t) return Error::make("invalid_shutter_timing", "shutter_timing must be center, open or close");
        o.shutterTiming = *t;
    }
    {
        std::string q = a.get("quality").asString("full");
        o.quality = q == "fast" ? 2 : q == "balanced" ? 1 : 0;
        static const char* kViews[] = {"final", "albedo", "normals", "material", "gi", "reflections", "ao", "depth", "lighting", "sketch"};
        std::string dv = a.get("debug_view").asString("final");
        for (int i = 0; i < 10; ++i) {
            if (dv == kViews[i]) o.debugView = i;
        }
        o.clay = a.get("clay").asBool(false);
    }
    o.warmup = static_cast<int>(std::clamp<int64_t>(a.get("warmup").asInt(4), 0, 240));
    o.resume = a.get("resume").asBool(false);
    // Name and outputs.
    o.name = a.get("name").asString();
    if (o.name.empty()) {
        if (o.sequencePlayer) o.name = s.record(o.sequencePlayer)->name;
        else if (!o.sequenceAsset.empty()) o.name = fs::path(o.sequenceAsset).stem().stem().string();
        else o.name = "movie";
    }
    std::vector<Json> outs;
    if (a.contains("output")) outs.push_back(a.get("output"));
    for (const auto& j : a.get("outputs").elements()) outs.push_back(j);
    if (outs.empty()) {
        std::string base = "renders/" + slug(o.name);
        outs.push_back(Json(videoEncodingAvailable() && !o.resume ? base + ".mp4" : base + "/frame_####.png"));
    }
    for (const auto& j : outs) {
        auto out = parseOutput(engine, j, a);
        if (!out) return out.error();
        if (isVideo(out->codec) && ((o.width % 2) || (o.height % 2))) {
            return Error::make("invalid_size", "video needs an even width and height", "e.g. 1920x1080");
        }
        o.outputs.push_back(std::move(*out));
    }
    return o;
}

}  // namespace movie

namespace tools {

using namespace schema;

void addMovieTools(Engine& engine, ToolRegistry& reg) {
    reg.add({"movie_render", "Render movie",
             "Render a cinematic to video or a PNG sequence, offline and deterministically (the Movie Render Queue). "
             "Renders a sequence (`sequence`: its entity or .sequence.json, whole length or start/end), an inline camera "
             "move (`camera`: {keys: [{t, eye, target, fov?, roll?}]} splined, or {shots: [{shot: orbit|dolly|crane|track|"
             "pan|static|path|flyover, duration, target, ...sequence_camera_shot fields}]} where each shot is a cut), or the "
             "scene camera for `duration` seconds. Always starts from the scene state and restores it after. simulate=true "
             "runs the game (scripts, physics); false (default) runs only sequences, animators and particles. Motion blur: "
             "`shutter` 0..1 of the frame (0.5 = 180 degrees; default the camera's motionBlur) accumulates sub-frames at "
             "fractional times; `samples` (default 8) is the per-frame budget. Outputs by extension: .mp4 = H.264 (codec "
             "hevc = 10-bit HEVC), .mov = ProRes 422 HQ, a folder or name_####.png = PNG sequence (resume=true continues an "
             "interrupted one). clay=true / debug_view=\"sketch\" render the same move as clay or pencil sketch. Blocks until "
             "done and returns frames, timings and paths; background=true returns at once (progress: movie_progress events, "
             "action=\"status\"; action=\"cancel\" stops). Example: {\"sequence\": \"Intro\", \"output\": \"renders/intro.mp4\", "
             "\"resolution\": \"1080p\", \"fps\": 24, \"shutter\": 0.5, \"samples\": 16}.",
             "render",
             object({{"action", enumeration({"render", "status", "cancel"}, "render (default), status of the current/last render, or cancel it")},
                     {"sequence", any("Sequence to render: entity with a sequencer, or a .sequence.json path")},
                     {"camera", any("Inline camera move: {keys: [{t, eye, target, fov?, roll?, ease?}]} or {shots: [{shot, duration, "
                                    "target, t?, ...}]}, plus aperture, focus_distance")},
                     {"camera_entity", schema::entity("Look through this camera instead of the scene's primary one")},
                     {"simulate", boolean("Run the full game simulation (scripts, physics) while rendering (default false)")},
                     {"start", number("Start time in seconds (default 0; the simulation pre-rolls to it)")},
                     {"end", number("End time in seconds (default: the sequence / camera path length)")},
                     {"duration", number("Seconds to render from start (instead of end)")},
                     {"fps", integer("Frames per second: 24 (default), 25, 30, 48, 50, 60")},
                     {"resolution", enumeration({"720p", "1080p", "1440p", "4k"}, "Size preset (default 1080p)")},
                     {"width", integer("Width in pixels (up to 3840)")},
                     {"height", integer("Height in pixels (up to 2160)")},
                     {"samples", integer("Samples per frame (default 8; 16-32 for finals): anti-aliasing, noise-free GI, motion blur")},
                     {"shutter", number("Motion blur: open shutter as a fraction of the frame (0.5 = 180-degree film look, 0 = off)")},
                     {"shutter_samples", integer("Sub-frames across the shutter (default: one per sample)")},
                     {"shutter_timing", enumeration({"center", "open", "close"}, "Shutter interval relative to the frame time (default center)")},
                     {"quality", enumeration({"full", "balanced", "fast"}, "Render quality tier (default full; fast for previews)")},
                     {"clay", boolean("Matte clay look (same move, for sketch -> clay -> final transitions)")},
                     {"debug_view", enumeration({"final", "albedo", "normals", "material", "gi", "reflections", "ao", "depth", "lighting", "sketch"},
                                                "Buffer visualization or the pencil sketch look")},
                     {"warmup", integer("Frames rendered before the first one so temporal effects settle (default 4)")},
                     {"output", string("Output path: .mp4 (H.264), .mov (ProRes 422 HQ), or a folder / name_####.png (PNG sequence); "
                                       "default renders/<name>.mp4")},
                     {"outputs", array(Json::object({{"description", "path, or {path, codec, bitrate_mbps}"}}),
                                       "Several outputs from one render, e.g. [\"renders/a.mp4\", {\"path\": \"renders/a.mov\", \"codec\": \"prores\"}]")},
                     {"codec", enumeration(movie::codecNames(), "Codec for `output`: h264, hevc (10-bit), prores (422 HQ), prores4444, png")},
                     {"bitrate_mbps", number("H.264 / HEVC average bitrate (default: chosen from size and fps)")},
                     {"resume", boolean("PNG sequences: continue from the first missing frame of an interrupted render")},
                     {"name", string("Name for reports and the default output path")},
                     {"background", boolean("Return immediately; the editor keeps rendering frame by frame (default false)")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 const std::string action = a.get("action").asString("render");
                 if (action == "status") return ToolResult::json(engine.movieStatus());
                 if (action == "cancel") {
                     if (!engine.movieRendering()) return ToolResult::json(engine.movieStatus(), "no movie is rendering");
                     engine.cancelMovie();
                     return ToolResult::json(engine.movieStatus(), "cancel requested: the render stops after the current sub-frame");
                 }
                 auto opts = movie::parseOptions(engine, a);
                 if (!opts) return ToolResult::error(opts.error());
                 if (a.get("background").asBool(false)) {
                     if (Status s = engine.startMovie(*opts); !s) return fail(s);
                     Json st = engine.movieStatus();
                     return ToolResult::json(st, "rendering " + std::to_string(st.get("frames").asInt()) +
                                                     " frames in the background (movie_progress events; movie_render {\"action\": \"status\"})");
                 }
                 auto r = engine.renderMovie(*opts);
                 if (!r) return ToolResult::error(r.error());
                 const Json& j = *r;
                 std::string text = "Movie " + j.get("state").asString() + ": " + std::to_string(j.get("frame").asInt()) + " frames (" +
                                    std::to_string(j.get("width").asInt()) + "x" + std::to_string(j.get("height").asInt()) + " @ " +
                                    std::to_string(j.get("fps").asInt()) + " fps), " + std::to_string(static_cast<long long>(std::llround(j.get("ms_per_frame").asNumber()))) +
                                    " ms/frame";
                 for (const auto& out : j.get("outputs").elements()) text += "\n  " + out.get("path").asString() + " (" + out.get("codec").asString() + ")";
                 return ToolResult::json(j, text);
             }});
}

}  // namespace tools
}  // namespace sky
