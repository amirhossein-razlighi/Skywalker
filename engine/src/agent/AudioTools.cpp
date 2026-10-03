// Audio tools: procedural sound and music generation (no external generator needed),
// auditioning, the mixer, and clip analysis.

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "ToolHelpers.h"
#include "skywalker/audio/MusicGen.h"
#include "skywalker/audio/Synth.h"
#include "skywalker/core/Strings.h"

namespace sky::tools {

namespace fs = std::filesystem;

namespace {

using namespace schema;

/// Resolves a project path for an audio output; ensures it is inside the project and ends in .wav.
Result<std::string> outputPath(Engine& engine, std::string path) {
    if (str::lower(path).size() < 4 || str::lower(path).substr(str::lower(path).size() - 4) != ".wav") path += ".wav";
    std::string abs = engine.resolvePath(path);
    std::string rel = engine.assets().relative(abs);
    if (rel.empty()) {
        return Error::make("invalid_arguments", "path '" + path + "' is outside the project", "use a project-relative path like audio/jump.wav");
    }
    return rel;
}

std::string slug(std::string s) {
    for (char& c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return s;
}

void recordAsset(Engine& engine, const std::string& rel, const Json& source, const std::vector<std::string>& tags,
                 const std::string& description) {
    engine.refreshAssets();
    if (auto rec = engine.assets().registerFile(engine.resolvePath(rel))) {
        Json tagsJson = Json::array();
        for (const auto& t : tags) tagsJson.push(t);
        (void)engine.assets().updateMeta((*rec)->path, Json::object({{"source", source}, {"tags", tagsJson}, {"description", description}}));
    }
}

Json fileEntry(const std::string& rel, const audio::Pcm& pcm) {
    audio::Stats st = audio::analyze(pcm);
    return Json::object({{"path", rel}, {"stats", st.toJson()}});
}

}  // namespace

void addAudioTools(Engine& engine, ToolRegistry& reg) {
    {
        std::vector<std::string> presets = audio::sfxPresetNames();
        std::string presetList;
        for (const auto& p : presets) presetList += (presetList.empty() ? "" : ", ") + p;
        std::string moodList;
        for (const auto& m : audio::musicMoods()) moodList += (moodList.empty() ? "" : ", ") + m;
        reg.add(
            {"audio_generate", "Generate sound or music",
             "Create a sound effect, ambience loop or piece of music as a WAV file in the project, procedurally and "
             "deterministically (the same seed and parameters always give the same audio) - no external generator needed. "
             "Presets: " + presetList + ". Names ending in _loop loop seamlessly (use them for ambience; set loop on the "
             "audio component). Use `seed` and `variations` for natural variety (footsteps!). Customize a preset with "
             "`pitch` (multiplier) and `volume`, or fully with `params`: layers of oscillators/noise with pitch slides, "
             "arpeggios, envelopes, filters and echo/reverb (call with list=true for the parameter reference). Music: pass "
             "`music` {mood: " + moodList + "; tempo, key like \"A minor\", progression like \"i VI III VII\", bars, "
             "energy 0..1, reverb, and pad/bass/arp/melody/drums on/off}; it loops seamlessly. Pass `entity` to create "
             "or update that entity's audio component with the clip (loops, bus and spatial settings are chosen from the "
             "kind). The result includes loudness stats; check them with audio_info and audition with audio_play. "
             "Examples: {\"preset\":\"jump\",\"path\":\"audio/jump.wav\"}; {\"preset\":\"footstep_stone\",\"variations\":4}; "
             "{\"music\":{\"mood\":\"epic\",\"tempo\":110,\"bars\":16},\"path\":\"audio/theme.wav\"}.",
             "asset",
             object({{"preset", enumeration(presets, "Sound effect preset")},
                     {"params", Json::object({{"type", "object"}, {"description", "Custom sound parameters (see list=true): {layers:[{wave, freq, slide, "
                                                                                    "attack, decay, lowpass, ...}], echo, reverb, volume, loopSeconds}. With a preset, "
                                                                                    "these replace the preset's matching fields."}})},
                     {"music", Json::object({{"type", "object"}, {"description", "Generate music instead: {mood, tempo, key, progression, bars, energy, "
                                                                                  "reverb, pad, bass, arp, melody, drums}"}})},
                     {"path", string("Output path, e.g. audio/jump.wav (default audio/<name>_<seed>.wav)")},
                     {"seed", integer("Variation seed (default 1)")},
                     {"variations", integer("Generate N variants with seeds seed..seed+N-1 (sfx only, 1..16, default 1); files get _1, _2 ... suffixes")},
                     {"pitch", number("Multiplier on the pitch of the preset's tonal layers (e.g. 0.5 = an octave lower)")},
                     {"volume", number("Output gain 0..4 (default 1; the sound is normalized to 0.9 peak first)")},
                     {"entity", entity("Create/update this entity's audio component with the clip")},
                     {"description", string("What the sound is (stored with the asset)")},
                     {"tags", array(Json::object({{"type", "string"}}), "Asset tags")},
                     {"list", boolean("Only list presets, moods and the parameter reference; generate nothing")}}),
             true, false, [&engine](const Json& a, ToolContext& ctx) {
                 if (a.get("list").asBool()) {
                     Json presetsJson = Json::array();
                     for (const auto& p : audio::sfxPresetNames()) {
                         presetsJson.push(Json::object({{"name", p}, {"loops", audio::sfxPresetLoops(p)}, {"description", audio::sfxPresetDescription(p)}}));
                     }
                     Json moods = Json::array();
                     for (const auto& m : audio::musicMoods()) moods.push(Json::object({{"name", m}, {"description", audio::musicMoodDescription(m)}}));
                     Json out = Json::object({{"presets", presetsJson},
                                              {"musicMoods", moods},
                                              {"layerParameters", audio::sfxParameterDocs()},
                                              {"globalParameters", Json::object({{"layers", "array of layer objects"},
                                                                                 {"echo", "{time s, feedback 0..0.95, mix 0..1}"},
                                                                                 {"reverb", "{mix 0..1, room 0..1, damping 0..1}"},
                                                                                 {"volume", "0..4"},
                                                                                 {"normalize", "peak level 0..1 (default 0.9)"},
                                                                                 {"duration", "force the length in seconds"},
                                                                                 {"loopSeconds", "render a seamless loop of this length"},
                                                                                 {"sampleRate", "8000..96000 (default 44100)"}})},
                                              {"musicParameters", audio::MusicParams{}.toJson()}});
                     return ToolResult::json(out, std::to_string(presetsJson.size()) + " sound presets, " + std::to_string(moods.size()) + " music moods");
                 }
                 const uint32_t seed = static_cast<uint32_t>(a.get("seed").asInt(1));
                 const bool isMusic = a.contains("music");
                 if (!isMusic && !a.contains("preset") && !a.contains("params")) {
                     return ToolResult::error(Error::make("invalid_arguments", "say what to generate: preset, params or music",
                                                          "call audio_generate with list=true to see presets and moods"));
                 }

                 struct Item {
                     audio::Pcm pcm;
                     std::string path;
                     Json provenance;
                 };
                 std::vector<Item> items;
                 std::string kind, name;
                 bool loops = false;
                 std::string describe = a.get("description").asString();

                 if (isMusic) {
                     auto mp = audio::MusicParams::fromJson(a.get("music"));
                     if (!mp) return ToolResult::error(mp.error());
                     mp->seed = seed;
                     auto pcm = audio::renderMusic(*mp);
                     if (!pcm) return ToolResult::error(pcm.error());
                     kind = "music";
                     name = "music_" + slug(mp->mood);
                     loops = true;
                     if (describe.empty()) describe = mp->mood + " music (" + std::to_string(mp->tempo > 0 ? mp->tempo : 0) + " bpm)";
                     items.push_back({std::move(*pcm), "", Json::object({{"generator", "skywalker.music"}, {"kind", "music"}, {"seed", static_cast<int64_t>(seed)}, {"params", mp->toJson()}})});
                 } else {
                     kind = "sfx";
                     int variations = static_cast<int>(std::clamp<int64_t>(a.get("variations").asInt(1), 1, 16));
                     std::string preset = a.get("preset").asString();
                     name = preset.empty() ? "sound" : preset;
                     for (int v = 0; v < variations; ++v) {
                         uint32_t s = seed + static_cast<uint32_t>(v);
                         Json base;
                         if (!preset.empty()) {
                             auto p = audio::sfxPreset(preset, s);
                             if (!p) return ToolResult::error(p.error());
                             base = p->toJson();
                             loops = loops || audio::sfxPresetLoops(preset);
                             if (describe.empty()) describe = audio::sfxPresetDescription(preset);
                         } else {
                             base = Json::object();
                         }
                         if (a.contains("params")) {
                             if (!a.get("params").isObject()) return ToolResult::error(Error::make("invalid_arguments", "params must be an object"));
                             base.mergePatch(a.get("params"));
                         }
                         auto params = audio::SfxParams::fromJson(base);
                         if (!params) return ToolResult::error(params.error());
                         params->seed = s;
                         if (a.contains("pitch")) {
                             float m = a.get("pitch").asFloat(1.f);
                             if (m < 0.1f || m > 10.f) return ToolResult::error(Error::make("invalid_arguments", "pitch must be between 0.1 and 10"));
                             for (auto& l : params->layers) {
                                 if (l.wave != "noise" && l.wave != "pink" && l.wave != "brown") l.freq = std::clamp(l.freq * m, 20.f, 20000.f);
                             }
                         }
                         if (a.contains("volume")) params->volume = std::clamp(a.get("volume").asFloat(1.f), 0.f, 4.f);
                         loops = loops || params->loopSeconds > 0.f;
                         auto pcm = audio::renderSfx(*params);
                         if (!pcm) return ToolResult::error(pcm.error());
                         items.push_back({std::move(*pcm), "", Json::object({{"generator", "skywalker.synth"}, {"kind", "sfx"}, {"preset", preset}, {"seed", static_cast<int64_t>(s)}, {"params", params->toJson()}})});
                     }
                 }

                 // Output paths.
                 std::string base = a.get("path").asString();
                 if (base.empty()) base = "audio/" + name + (seed != 1 ? "_" + std::to_string(seed) : "");
                 {
                     std::string lower = str::lower(base);
                     if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".wav") base = base.substr(0, base.size() - 4);
                 }
                 Json files = Json::array();
                 std::vector<std::string> written;
                 for (size_t i = 0; i < items.size(); ++i) {
                     std::string p = items.size() > 1 ? base + "_" + std::to_string(i + 1) : base;
                     auto rel = outputPath(engine, p);
                     if (!rel) return ToolResult::error(rel.error());
                     if (Status s = audio::writeWav(engine.resolvePath(*rel), items[i].pcm); !s) return fail(s);
                     items[i].provenance["by"] = ctx.actor;
                     recordAsset(engine, *rel, items[i].provenance, {"generated", kind, name, loops ? "loop" : "oneshot"}, describe);
                     engine.audio().invalidate(*rel);
                     files.push(fileEntry(*rel, items[i].pcm));
                     written.push_back(*rel);
                 }

                 Json result = Json::object({{"files", files}, {"loops", loops}});
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     Json patch = Json::object({{"clip", written.front()}, {"loop", loops}});
                     if (kind == "music") {
                         patch["bus"] = "music";
                         patch["spatial"] = false;
                     } else if (loops) {
                         patch["bus"] = "ambience";
                     }
                     Status st = engine.edit(ctx.actor, "Attach audio", [&]() { return engine.scene().patchComponent(*id, "audio", patch); });
                     if (!st) return fail(st);
                     result["attached"] = Json::object({{"entity", *id}, {"clip", written.front()}});
                 }
                 std::string summary = "generated " + std::to_string(written.size()) + " " + kind + " file(s): " + written.front();
                 return ToolResult::json(result, summary);
             }});
    }

    reg.add({"audio_play", "Play sound",
             "Listen to a clip or an entity's audio component. While editing it auditions the clip on the audio output "
             "(a preview; nothing from the scene plays in edit mode). While the game plays it starts a one-shot (clip) or "
             "(re)starts the entity's source. stop=true stops the preview, or the entity's sound. Reports whether a real "
             "output device is used: on machines without one (CI, servers) sound is mixed silently, so use audio_info "
             "to judge a clip, not your ears. Example: {\"clip\":\"audio/jump.wav\"} or {\"entity\":\"Campfire\"}.",
             "sim",
             object({{"clip", string("Project-relative audio file")},
                     {"entity", entity("Play this entity's audio component")},
                     {"volume", number("Gain 0..4 (default 1)")},
                     {"bus", enumeration(audio::busNames(), "Mixer bus for a clip (default sfx)")},
                     {"stop", boolean("Stop instead of play")}}),
             false, false, [&engine](const Json& a, ToolContext&) {
                 audio::AudioSystem& au = engine.audio();
                 const bool playing = engine.playState() != PlayState::Editing;
                 EntityId entity = kNoEntity;
                 if (a.contains("entity")) {
                     auto id = resolve(engine, a.get("entity"));
                     if (!id) return ToolResult::error(id.error());
                     entity = *id;
                 }
                 if (a.get("stop").asBool()) {
                     if (entity != kNoEntity) au.stopEntity(entity);
                     else au.stopPreview();
                     return ToolResult::text("stopped");
                 }
                 if (au.mode() == audio::AudioMode::Off) {
                     return ToolResult::error(Error::make("audio_disabled", "audio is disabled", "unset SKYWALKER_AUDIO=off"));
                 }
                 std::string err, clip;
                 float volume = a.get("volume").asFloat(1.f);
                 std::string bus = a.get("bus").asString("sfx");
                 if (entity != kNoEntity) {
                     const AudioSource* src = engine.scene().get<AudioSource>(entity);
                     if (!src) return ToolResult::error(Error::make("not_found", "that entity has no audio component", "add one with component_set or audio_generate entity=..."));
                     clip = src->clip;
                     if (playing) {
                         err = au.playEntity(engine.scene(), entity);
                     } else {
                         err = au.preview(src->clip, a.contains("volume") ? volume : src->volume, src->bus);
                     }
                 } else if (a.contains("clip")) {
                     clip = a.get("clip").asString();
                     err = playing ? au.playOneShot(clip, volume, bus, std::nullopt) : au.preview(clip, volume, bus);
                 } else {
                     return ToolResult::error(Error::make("invalid_arguments", "give a clip or an entity"));
                 }
                 if (!err.empty()) return ToolResult::error(Error::make("audio_error", err));
                 Json out = Json::object({{"clip", clip},
                                          {"mode", playing ? "playing" : "preview"},
                                          {"device", au.deviceActive()},
                                          {"voices", au.voiceCount()}});
                 if (!au.statusNote().empty()) out["note"] = au.statusNote();
                 return ToolResult::json(out, (au.deviceActive() ? "playing " : "playing silently (no output device) ") + clip);
             }});

    reg.add({"audio_mix", "Audio mixer",
             "Read or set the project's mixer (audio.json): per-bus volume and mute for master, music, sfx, ambience, voice "
             "and ui, and the default music crossfade. Every audio component and Wander sound plays through a bus, so "
             "this is how you balance a game (music quieter than effects, mute ambience for a cutscene). With no arguments "
             "it returns the mix plus what is playing right now (voices with clip, bus and distance gain), whether a real "
             "output device is used, and the measured output peak in silent (null) mode. Wander can change volumes at runtime "
             "with set_volume(\"music\", 0.4). Example: {\"buses\":{\"music\":0.5,\"ambience\":{\"volume\":0.8}}}.",
             "sim",
             object({{"buses", Json::object({{"type", "object"}, {"description", "bus -> volume (0..4) or {volume, mute}. Buses: master, music, sfx, ambience, voice, ui"}})},
                     {"musicFade", number("Default music crossfade in seconds")},
                     {"reset", boolean("Restore default levels first")}}),
             true, false, [&engine](const Json& a, ToolContext&) {
                 audio::AudioSystem& au = engine.audio();
                 if (a.contains("buses") || a.contains("musicFade") || a.get("reset").asBool()) {
                     audio::MixSettings mix = a.get("reset").asBool() ? audio::MixSettings{} : au.mix();
                     if (a.contains("buses")) {
                         if (!a.get("buses").isObject()) return ToolResult::error(Error::make("invalid_arguments", "buses must be an object"));
                         for (const auto& [bus, value] : a.get("buses").members()) {
                             auto it = mix.buses.find(bus);
                             if (it == mix.buses.end()) {
                                 std::string guess = str::closest(bus, audio::busNames(), 3);
                                 return ToolResult::error(Error::make("not_found", "unknown audio bus '" + bus + "'",
                                                                      guess.empty() ? "buses: master, music, sfx, ambience, voice, ui" : "did you mean '" + guess + "'?"));
                             }
                             if (value.isNumber()) {
                                 it->second.volume = value.asFloat();
                             } else if (value.isObject()) {
                                 for (const auto& [k, v] : value.members()) {
                                     if (k == "volume") it->second.volume = v.asFloat(1.f);
                                     else if (k == "mute") it->second.mute = v.asBool();
                                     else return ToolResult::error(Error::make("invalid_arguments", "unknown bus field '" + k + "'", "use volume and mute"));
                                 }
                             } else {
                                 return ToolResult::error(Error::make("invalid_arguments", "bus '" + bus + "' must be a volume number or {volume, mute}"));
                             }
                             if (it->second.volume < 0.f || it->second.volume > 4.f) {
                                 return ToolResult::error(Error::make("invalid_arguments", "bus '" + bus + "' volume must be between 0 and 4"));
                             }
                         }
                     }
                     if (a.contains("musicFade")) mix.musicFade = std::clamp(a.get("musicFade").asFloat(1.5f), 0.f, 30.f);
                     if (Status s = engine.setAudioMix(mix); !s) return fail(s);
                 }
                 Json voices = Json::array();
                 for (const auto& v : au.voices()) voices.push(v.toJson());
                 Json out = Json::object({{"mix", au.mix().toJson()},
                                          {"mode", audio::toString(au.mode())},
                                          {"device", au.deviceActive()},
                                          {"voices", voices}});
                 if (!au.statusNote().empty()) out["note"] = au.statusNote();
                 if (!au.deviceActive()) out["outputPeak"] = std::round(au.takePeak() * 1000.f) / 1000.f;
                 return ToolResult::json(out, "audio mixer");
             }});

    reg.add({"audio_info", "Analyze audio",
             "Measure an audio file (wav, mp3, flac): duration, sample rate, channels, peak and RMS in dBFS, integrated "
             "loudness (LUFS), clipping, DC offset, silence ratio, whether it starts or ends abruptly (clicks), and for "
             "loops how big the jump is at the loop point. Returns warnings (clipping, too quiet/loud, click risks, "
             "loop seam) and the asset's provenance. Use it to verify generated audio since you cannot listen: "
             "sound effects around -23 to -14 LUFS, music around -20 to -16, ambience quieter. Example: {\"path\":\"audio/jump.wav\"}.",
             "asset", object({{"path", string("Project-relative audio file")}}, {"path"}), false, false,
             [&engine](const Json& a, ToolContext&) {
                 std::string path = a.get("path").asString();
                 std::string abs = engine.resolvePath(path);
                 std::error_code ec;
                 if (!fs::is_regular_file(abs, ec)) {
                     std::vector<std::string> names;
                     AssetDatabase::Query q;
                     q.type = AssetType::Audio;
                     q.limit = 500;
                     for (const auto* r : engine.assets().query(q)) names.push_back(r->path);
                     std::string guess = str::closest(path, names, 6);
                     return ToolResult::error(Error::make("not_found", "no audio file " + path,
                                                          guess.empty() ? "list audio with asset_list type=audio" : "did you mean '" + guess + "'?"));
                 }
                 auto pcm = audio::readAudioFile(abs);
                 if (!pcm) return ToolResult::error(pcm.error());
                 audio::Stats st = audio::analyze(*pcm);
                 Json warnings = Json::array();
                 if (st.clipped > 4) warnings.push("clipping: " + std::to_string(st.clipped) + " samples at full scale; lower the volume");
                 if (st.peak < 0.01) warnings.push("the file is silent");
                 else if (st.loudnessLufs > -8) warnings.push("very loud (" + std::to_string(static_cast<int>(st.loudnessLufs)) + " LUFS); lower it or the bus volume");
                 else if (st.loudnessLufs < -40 && st.loudnessLufs > -119) warnings.push("very quiet (" + std::to_string(static_cast<int>(st.loudnessLufs)) + " LUFS)");
                 if (std::fabs(st.dcOffset) > 0.02) warnings.push("DC offset " + std::to_string(st.dcOffset) + " (wastes headroom, can click)");
                 // Loop seam: the step from the last to the first frame compared with the typical step.
                 Json loop = Json::object();
                 const size_t ch = static_cast<size_t>(pcm->channels), n = pcm->frames();
                 if (n > 64) {
                     double seam = 0, typical = 0;
                     for (size_t c = 0; c < ch; ++c) {
                         seam = std::max(seam, std::fabs(static_cast<double>(pcm->samples[c]) - pcm->samples[(n - 1) * ch + c]));
                     }
                     size_t counted = 0;
                     for (size_t i = 1; i < n; i += std::max<size_t>(1, n / 4000)) {
                         for (size_t c = 0; c < ch; ++c) {
                             typical += std::fabs(static_cast<double>(pcm->samples[i * ch + c]) - pcm->samples[(i - 1) * ch + c]);
                         }
                         ++counted;
                     }
                     typical /= static_cast<double>(std::max<size_t>(1, counted * ch));
                     double ratio = typical > 1e-6 ? seam / typical : 0.0;
                     loop["seamJump"] = std::round(seam * 10000.0) / 10000.0;
                     loop["seamVsTypicalStep"] = std::round(ratio * 100.0) / 100.0;
                     loop["seamless"] = ratio < 6.0 || seam < 0.01;
                     if (ratio >= 6.0 && seam >= 0.01 && st.duration > 2.0) {
                         warnings.push("loop point jumps (" + std::to_string(seam).substr(0, 5) + "): a looping clip would click; use *_loop presets or crossfade");
                     }
                 }
                 // A non-zero first/last sample only matters when the wrap is not smooth (a smooth wrap is a loop).
                 const bool smoothWrap = loop.contains("seamVsTypicalStep") && loop.get("seamVsTypicalStep").asNumber() < 3.0;
                 if (!smoothWrap) {
                    if (st.firstSample > 0.05 * std::max(st.peak, 1e-3)) warnings.push("first sample is not near zero (" + std::to_string(st.firstSample).substr(0, 5) + "): playback starts with a click unless it is a seamless loop");
                    if (st.lastSample > 0.05 * std::max(st.peak, 1e-3) && st.duration < 30) warnings.push("last sample is not near zero (" + std::to_string(st.lastSample).substr(0, 5) + "): ends with a click unless it is a seamless loop");
                 }
                 Json out = Json::object({{"path", path}, {"stats", st.toJson()}, {"loop", loop}, {"warnings", warnings}});
                 if (const AssetRecord* rec = engine.assets().find(path)) {
                     if (rec->source.size()) out["source"] = rec->source;
                     if (!rec->description.empty()) out["description"] = rec->description;
                 }
                 char buf[160];
                 std::snprintf(buf, sizeof(buf), "%s: %.2f s, %d ch, %d Hz, peak %.1f dB, %.1f LUFS%s", path.c_str(), st.duration, st.channels,
                               st.sampleRate, st.peakDb, st.loudnessLufs, warnings.size() ? (" - " + std::to_string(warnings.size()) + " warning(s)").c_str() : "");
                 return ToolResult::json(out, buf);
             }});
}

}  // namespace sky::tools
