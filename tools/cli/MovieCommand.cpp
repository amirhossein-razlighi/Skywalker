// `skywalker movie SCENE [JSON] [options]`: render a cinematic to video / PNG frames (the movie_render
// tool from the command line, with live progress and Ctrl-C to stop). See docs/MOVIE_RENDER.md.

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "skywalker/core/Log.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/engine/Movie.h"

namespace {

using namespace sky;

Engine* gEngine = nullptr;

void onInterrupt(int) {
    if (gEngine) gEngine->cancelMovie();  // an atomic store: safe in a signal handler
}

int movieUsage() {
    std::fprintf(stderr,
                 "usage: skywalker movie SCENE [JSON] [options]\n\n"
                 "Renders a cinematic offline: a sequence, an inline camera path (JSON \"camera\") or the scene camera.\n"
                 "JSON takes every movie_render argument; the options below override it.\n\n"
                 "options:\n"
                 "  --project DIR        the project (default: current directory)\n"
                 "  -o, --output PATH    .mp4 (H.264), .mov (ProRes 422 HQ) or a folder / name_####.png (repeatable)\n"
                 "  --codec C            codec of the -o before it: h264 | hevc | prores | prores4444 | png\n"
                 "  --bitrate MBPS       H.264 / HEVC bitrate of the -o before it\n"
                 "  --sequence NAME      sequence entity or .sequence.json\n"
                 "  --camera-entity NAME look through this camera\n"
                 "  --start S --end S --duration S   time range in seconds\n"
                 "  --fps N              24 (default), 30, 60...\n"
                 "  --resolution R       720p | 1080p (default) | 1440p | 4k   (or --width W --height H)\n"
                 "  --samples N          samples per frame (default 8)\n"
                 "  --shutter F          motion blur: open fraction of the frame (0.5 = 180 degrees)\n"
                 "  --shutter-samples N  sub-frames across the shutter\n"
                 "  --simulate           run the game simulation (scripts, physics)\n"
                 "  --clay | --sketch    clay or pencil-sketch look\n"
                 "  --quality Q          full | balanced | fast\n"
                 "  --warmup N           frames rendered before the first one (default 4)\n"
                 "  --resume             continue an interrupted PNG sequence\n"
                 "  --quiet              no progress lines\n"
                 "Ctrl-C stops after the current sub-frame; written frames and the video so far are kept.\n");
    return 2;
}

}  // namespace

int runMovie(const std::vector<std::string>& raw) {
    if (raw.size() < 2 || raw[1].rfind("-", 0) == 0) return movieUsage();
    const std::string scenePath = raw[1];
    std::string project = ".";
    Json args = Json::object();
    Json outputs = Json::array();
    bool quiet = false;
    for (size_t i = 2; i < raw.size(); ++i) {
        const std::string& a = raw[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= raw.size()) {
                std::fprintf(stderr, "error: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return raw[++i];
        };
        auto number = [&](const char* key) {
            std::string v = value();
            char* end = nullptr;
            double d = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end) {
                std::fprintf(stderr, "error: %s expects a number (got \"%s\")\n", a.c_str(), v.c_str());
                std::exit(2);
            }
            args[key] = d;
        };
        if (a == "--project") project = value();
        else if (a == "-o" || a == "--output") outputs.push(Json(value()));
        else if (a == "--codec" || a == "--bitrate") {  // applies to the -o before it
            if (outputs.size() == 0) {
                std::fprintf(stderr, "error: %s goes after the -o it applies to\n", a.c_str());
                return 2;
            }
            Json& last = outputs.elements().back();
            if (last.isString()) last = Json::object({{"path", last.asString()}});
            if (a == "--codec") last["codec"] = value();
            else last["bitrate_mbps"] = std::strtod(value().c_str(), nullptr);
        }
        else if (a == "--sequence") args["sequence"] = value();
        else if (a == "--camera-entity") args["camera_entity"] = value();
        else if (a == "--start") number("start");
        else if (a == "--end") number("end");
        else if (a == "--duration") number("duration");
        else if (a == "--fps") number("fps");
        else if (a == "--resolution") args["resolution"] = value();
        else if (a == "--width") number("width");
        else if (a == "--height") number("height");
        else if (a == "--samples") number("samples");
        else if (a == "--shutter") number("shutter");
        else if (a == "--shutter-samples") number("shutter_samples");
        else if (a == "--warmup") number("warmup");
        else if (a == "--quality") args["quality"] = value();
        else if (a == "--simulate") args["simulate"] = true;
        else if (a == "--clay") args["clay"] = true;
        else if (a == "--sketch") args["debug_view"] = "sketch";
        else if (a == "--resume") args["resume"] = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") return movieUsage();
        else if (!a.empty() && a[0] == '{') {
            auto parsed = Json::parse(a);
            if (!parsed || !parsed->isObject()) {
                std::fprintf(stderr, "error: invalid JSON arguments: %s\n", parsed ? "expected an object" : parsed.error().message.c_str());
                return 2;
            }
            for (const auto& [k, v] : parsed->members()) {
                if (!args.contains(k)) args[k] = v;
            }
        } else {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return movieUsage();
        }
    }
    if (outputs.size() > 0) {
        args.erase("output");
        Json all = outputs;
        for (const auto& o : args.get("outputs").elements()) all.push(o);
        args["outputs"] = all;
    }

    EngineConfig cfg;
    cfg.projectDir = project;
    cfg.audio = audio::AudioMode::Null;  // renders run far from real time
    Engine engine(cfg);
    if (Status s = engine.loadScene(scenePath); !s) {
        std::fprintf(stderr, "error: %s\n", s.error().message.c_str());
        return 1;
    }
    if (Status s = engine.tools().find("movie_render") ? validateSchema(engine.tools().find("movie_render")->inputSchema, args) : Status{}; !s) {
        std::fprintf(stderr, "error: %s%s%s\n", s.error().message.c_str(), s.error().hint.empty() ? "" : "\n  hint: ", s.error().hint.c_str());
        return 2;
    }
    auto opts = movie::parseOptions(engine, args);
    if (!opts) {
        std::fprintf(stderr, "error: %s%s%s\n", opts.error().message.c_str(), opts.error().hint.empty() ? "" : "\n  hint: ",
                     opts.error().hint.c_str());
        return 1;
    }
    gEngine = &engine;
    std::signal(SIGINT, onInterrupt);
    auto r = engine.renderMovie(*opts, [&](const Json& p) {
        if (quiet || p.get("type").asString() != "movie_progress") return;
        std::fprintf(stderr, "\rframe %4lld/%lld  %5.1f%%  %6.0f ms/frame  eta %4.0f s ", static_cast<long long>(p.get("frame").asInt()),
                     static_cast<long long>(p.get("frames").asInt()), p.get("percent").asNumber(), p.get("ms_per_frame").asNumber(),
                     p.get("eta_s").asNumber());
        std::fflush(stderr);
    });
    std::signal(SIGINT, SIG_DFL);
    gEngine = nullptr;
    if (!quiet) std::fprintf(stderr, "\n");
    if (!r) {
        std::fprintf(stderr, "error: %s%s%s\n", r.error().message.c_str(), r.error().hint.empty() ? "" : "\n  hint: ", r.error().hint.c_str());
        return 1;
    }
    const Json& j = *r;
    std::printf("%s: %lld frames, %dx%d @ %d fps (%.2f s), %.0f ms/frame, %.1f s total, renderer %s\n", j.get("state").asString().c_str(),
                static_cast<long long>(j.get("frame").asInt()), static_cast<int>(j.get("width").asInt()),
                static_cast<int>(j.get("height").asInt()), static_cast<int>(j.get("fps").asInt()), j.get("duration").asNumber(),
                j.get("ms_per_frame").asNumber(), j.get("elapsed_s").asNumber(), engine.renderer().info().backend.c_str());
    for (const auto& o : j.get("outputs").elements()) std::printf("  %s (%s)\n", o.get("path").asString().c_str(), o.get("codec").asString().c_str());
    return j.get("state").asString() == "done" ? 0 : 130;
}
