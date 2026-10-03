#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "skywalker/audio/AudioSystem.h"
#include "skywalker/audio/MusicGen.h"
#include "skywalker/audio/Synth.h"
#include "skywalker/audio/Wav.h"
#include "skywalker/engine/Engine.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

audio::Pcm sine(float freq, float amp, float seconds, int rate = 44100) {
    audio::Pcm p;
    p.sampleRate = rate;
    p.channels = 1;
    size_t n = static_cast<size_t>(seconds * static_cast<float>(rate));
    p.samples.resize(n);
    for (size_t i = 0; i < n; ++i) p.samples[i] = amp * std::sin(6.2831853f * freq * static_cast<float>(i) / static_cast<float>(rate));
    return p;
}

struct AudioProject {
    std::string dir;
    std::unique_ptr<Engine> engine;

    explicit AudioProject(audio::AudioMode mode = audio::AudioMode::Null) {
        dir = (fs::temp_directory_path() / ("skywalker-audio-" + AssetDatabase::newGuid().substr(0, 8))).string();
        fs::create_directories(dir + "/audio");
        // A steady 330 Hz tone, long enough to loop through a whole test.
        REQUIRE(audio::writeWav(dir + "/audio/tone.wav", sine(330.f, 0.5f, 2.f)));
        REQUIRE(audio::writeWav(dir + "/audio/tone2.wav", sine(440.f, 0.5f, 2.f)));
        engine = open(mode);
    }
    ~AudioProject() {
        engine.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::unique_ptr<Engine> open(audio::AudioMode mode = audio::AudioMode::Null) const {
        EngineConfig cfg;
        cfg.renderer = RendererBackend::Null;
        cfg.projectDir = dir;
        cfg.audio = mode;
        auto e = std::make_unique<Engine>(cfg);
        (void)e->newScene("Audio", false);
        return e;
    }
    EntityId source(const char* name, Vec3 pos, const char* patch = "{}") {
        Scene& s = engine->scene();
        EntityId id = s.create(name);
        Json j = Json::parse(patch).value();
        if (!j.contains("clip")) j["clip"] = "audio/tone.wav";
        REQUIRE(s.patchComponent(id, "audio", j));
        s.get<Transform>(id)->position = pos;
        return id;
    }
    EntityId listener() {
        EntityId id = engine->scene().create("Ears");
        REQUIRE(engine->scene().patchComponent(id, "listener", Json::object()));
        return id;
    }
};

Json call(Engine& e, const char* tool, const std::string& args, bool expectOk = true) {
    ToolResult r = e.callTool(tool, Json::parse(args).value(), "agent:test");
    INFO(tool, " -> ", (r.content.empty() ? "" : r.content.front().text));
    CHECK(r.isError == !expectOk);
    return r.structured;
}

}  // namespace

// ---------------------------------------------------------------------------
// WAV and analysis
// ---------------------------------------------------------------------------

TEST_CASE("audio: WAV round trip keeps format and samples (16-bit quantization)") {
    audio::Pcm src = sine(440.f, 0.8f, 0.25f, 22050);
    src.channels = 2;
    src.samples.resize(src.samples.size() / 2 * 2);
    auto bytes = audio::encodeWav(src);
    CHECK(bytes.size() == 44 + src.samples.size() * 2);
    CHECK(std::string(bytes.begin(), bytes.begin() + 4) == "RIFF");
    auto back = audio::decodeWav(bytes.data(), bytes.size());
    REQUIRE(back);
    CHECK(back->channels == 2);
    CHECK(back->sampleRate == 22050);
    REQUIRE(back->samples.size() == src.samples.size());
    for (size_t i = 0; i < src.samples.size(); i += 17) CHECK(std::fabs(back->samples[i] - src.samples[i]) <= 2.0f / 32768.f);

    // Encoding is deterministic, and out-of-range / NaN samples are clamped instead of corrupting the file.
    CHECK(audio::encodeWav(src) == bytes);
    audio::Pcm wild;
    wild.samples = {5.f, -5.f, std::nanf(""), 0.5f};
    auto w = audio::decodeWav(audio::encodeWav(wild).data(), audio::encodeWav(wild).size());
    REQUIRE(w);
    CHECK(w->samples[0] == doctest::Approx(1.0).epsilon(0.001));
    CHECK(w->samples[1] == doctest::Approx(-1.0).epsilon(0.001));
    CHECK(w->samples[2] == doctest::Approx(0.0));
}

TEST_CASE("audio: WAV reader rejects garbage and reads 32-bit float files") {
    std::vector<uint8_t> junk{'n', 'o', 'p', 'e'};
    CHECK_FALSE(audio::decodeWav(junk.data(), junk.size()));

    // Hand-built 32-bit float mono WAV with two samples.
    std::vector<uint8_t> w;
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto put16 = [&](uint16_t v) { w.push_back(static_cast<uint8_t>(v)); w.push_back(static_cast<uint8_t>(v >> 8)); };
    auto tag = [&](const char* t) { w.insert(w.end(), t, t + 4); };
    tag("RIFF"); put32(36 + 8); tag("WAVE"); tag("fmt "); put32(16); put16(3); put16(1); put32(8000); put32(32000); put16(4); put16(32);
    tag("data"); put32(8);
    float s[2] = {0.25f, -0.5f};
    for (float f : s) { uint32_t u; std::memcpy(&u, &f, 4); put32(u); }
    auto r = audio::decodeWav(w.data(), w.size());
    REQUIRE(r);
    CHECK(r->sampleRate == 8000);
    CHECK(r->samples[0] == doctest::Approx(0.25));
    CHECK(r->samples[1] == doctest::Approx(-0.5));
}

TEST_CASE("audio: analysis measures peak, RMS and BS.1770 loudness") {
    // A 997 Hz sine at -20 dBFS peak reads about -23 LUFS (the BS.1770 calibration point).
    audio::Stats st = audio::analyze(sine(997.f, 0.1f, 3.f, 48000));
    CHECK(st.peakDb == doctest::Approx(-20.0).epsilon(0.01));
    CHECK(st.rmsDb == doctest::Approx(-23.0).epsilon(0.02));
    CHECK(st.loudnessLufs == doctest::Approx(-23.0).epsilon(0.02));
    CHECK(st.clipped == 0);
    CHECK(std::fabs(st.dcOffset) < 1e-3);

    audio::Pcm silent;
    silent.samples.assign(44100, 0.f);
    CHECK(audio::analyze(silent).loudnessLufs < -100);
    CHECK(audio::analyze(silent).silenceRatio == doctest::Approx(1.0));

    audio::Pcm hot = sine(200.f, 1.f, 0.5f);
    for (float& v : hot.samples) v = std::clamp(v * 3.f, -1.f, 1.f);
    CHECK(audio::analyze(hot).clipped > 1000);
}

TEST_CASE("audio: seamless loop construction has no jump at the wrap") {
    // A continuous noisy signal longer than the loop; the wrap must continue the signal.
    audio::Pcm src = sine(180.f, 0.5f, 3.f);
    for (size_t i = 0; i < src.samples.size(); ++i) src.samples[i] += 0.2f * std::sin(0.0371f * static_cast<float>(i * i % 1000));
    const size_t frames = 100000;
    audio::Pcm loop = audio::makeSeamlessLoop(src, frames);
    REQUIRE(loop.frames() == frames);
    // Last sample -> first sample is the natural continuation of the source: out[0] == src[frames].
    CHECK(loop.samples[0] == doctest::Approx(src.samples[frames]).epsilon(1e-4));
    CHECK(std::fabs(loop.samples[frames - 1] - loop.samples[0]) < 0.35);
}

// ---------------------------------------------------------------------------
// Synth
// ---------------------------------------------------------------------------

TEST_CASE("synth: the same seed renders bit-identical samples; other seeds differ") {
    for (const char* name : {"jump", "explosion", "footstep_grass", "rain_loop"}) {
        INFO(name);
        auto a = audio::sfxPreset(name, 7);
        auto b = audio::sfxPreset(name, 7);
        REQUIRE(a);
        REQUIRE(b);
        auto pa = audio::renderSfx(*a), pb = audio::renderSfx(*b);
        REQUIRE(pa);
        REQUIRE(pb);
        REQUIRE(pa->samples.size() == pb->samples.size());
        CHECK(pa->samples == pb->samples);
        CHECK(audio::encodeWav(*pa) == audio::encodeWav(*pb));
    }
    // Seeds change the details (pitch variation, noise) - footsteps are never copies.
    auto s1 = audio::renderSfx(*audio::sfxPreset("footstep_stone", 1));
    auto s2 = audio::renderSfx(*audio::sfxPreset("footstep_stone", 2));
    REQUIRE(s1);
    REQUIRE(s2);
    CHECK(s1->samples != s2->samples);
}

TEST_CASE("synth: every preset renders finite, audible, bounded audio") {
    for (const auto& name : audio::sfxPresetNames()) {
        INFO(name);
        auto params = audio::sfxPreset(name, 3);
        REQUIRE(params);
        auto pcm = audio::renderSfx(*params);
        REQUIRE(pcm);
        CHECK(pcm->channels == 1);
        CHECK(pcm->duration() > 0.02);
        CHECK(pcm->duration() < 25.0);
        audio::Stats st = audio::analyze(*pcm);
        CHECK(st.peak > 0.05);
        CHECK(st.peak <= 1.0);
        CHECK(std::fabs(st.dcOffset) < 0.02);
        for (float s : pcm->samples) REQUIRE(std::isfinite(s));
        if (audio::sfxPresetLoops(name) && params->loopSeconds > 0.f) {
            // Seamless: the jump at the wrap is within the signal's usual sample-to-sample motion.
            const auto& d = pcm->samples;
            double typical = 0;
            for (size_t i = 1; i < d.size(); i += 7) typical += std::fabs(d[i] - d[i - 1]);
            typical /= static_cast<double>(d.size() / 7);
            CHECK(std::fabs(d.front() - d.back()) < 8 * typical + 0.01);
            CHECK(pcm->duration() == doctest::Approx(params->loopSeconds).epsilon(0.001));
        } else {
            CHECK(st.firstSample < 0.02);
            CHECK(st.lastSample < 0.02);  // one-shots start and end on silence: no clicks
        }
    }
}

TEST_CASE("synth: parameters round trip through JSON and bad input gets helpful errors") {
    auto p = audio::sfxPreset("laser", 5);
    REQUIRE(p);
    Json j = p->toJson();
    auto q = audio::SfxParams::fromJson(j);
    REQUIRE(q);
    CHECK(q->toJson() == j);
    auto a = audio::renderSfx(*p), b = audio::renderSfx(*q);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->samples == b->samples);

    auto typo = audio::SfxParams::fromJson(Json::parse(R"({"layers":[{"wave":"saw","freqq":300}]})").value());
    REQUIRE_FALSE(typo);
    CHECK(typo.error().hint.find("freq") != std::string::npos);
    auto wave = audio::SfxParams::fromJson(Json::parse(R"({"wave":"sqaure"})").value());
    REQUIRE_FALSE(wave);
    CHECK(wave.error().hint.find("square") != std::string::npos);
    auto range = audio::SfxParams::fromJson(Json::parse(R"({"freq":5})").value());
    REQUIRE_FALSE(range);
    CHECK(range.error().message.find("out of range") != std::string::npos);
    auto flat = audio::SfxParams::fromJson(Json::parse(R"({"wave":"sine","freq":880,"decay":0.2,"reverb":{"mix":0.3}})").value());
    REQUIRE(flat);  // a single layer can be given flat
    CHECK(flat->layers.size() == 1);
    auto unknown = audio::sfxPreset("jmup");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().hint.find("jump") != std::string::npos);
}

TEST_CASE("synth: pitch slides and arpeggios move the frequency as specified") {
    auto dominant = [](const audio::Pcm& p, double t) {
        const size_t n = 4096, start = static_cast<size_t>(t * p.sampleRate);
        double best = 0, bestF = 0;
        for (double f = 100; f < 2000; f += 4) {  // Goertzel-free brute force DFT bin scan
            double re = 0, im = 0;
            for (size_t i = 0; i < n; ++i) {
                double w = 0.5 - 0.5 * std::cos(6.2831853 * static_cast<double>(i) / static_cast<double>(n));
                double ph = 6.2831853 * f * static_cast<double>(start + i) / p.sampleRate;
                re += p.samples[start + i] * w * std::cos(ph);
                im += p.samples[start + i] * w * std::sin(ph);
            }
            double m = re * re + im * im;
            if (m > best) {
                best = m;
                bestF = f;
            }
        }
        return bestF;
    };
    auto up = audio::renderSfx(*audio::SfxParams::fromJson(Json::parse(
        R"({"wave":"sine","freq":300,"slide":1.0,"attack":0.001,"decay":0.1,"sustain":1,"hold":0.8,"release":0.05})").value()));
    REQUIRE(up);
    CHECK(dominant(*up, 0.02) < dominant(*up, 0.5));
    CHECK(dominant(*up, 0.5) == doctest::Approx(300.0 * std::pow(2.0, 0.55)).epsilon(0.1));

    auto arp = audio::renderSfx(*audio::SfxParams::fromJson(Json::parse(
        R"({"wave":"sine","freq":400,"arp":[0,12],"arpRate":4,"attack":0.001,"decay":0.1,"sustain":1,"hold":0.8,"release":0.05})").value()));
    REQUIRE(arp);
    CHECK(dominant(*arp, 0.02) == doctest::Approx(400.0).epsilon(0.03));
    CHECK(dominant(*arp, 0.4) == doctest::Approx(800.0).epsilon(0.03));
}

// ---------------------------------------------------------------------------
// Music
// ---------------------------------------------------------------------------

TEST_CASE("music: key and progression parsing builds the right chords") {
    auto key = audio::parseKey("A minor", "major");
    REQUIRE(key);
    CHECK(key->first == 9);
    CHECK(key->second == std::vector<int>{0, 2, 3, 5, 7, 8, 10});
    auto sharp = audio::parseKey("F# dorian", "major");
    REQUIRE(sharp);
    CHECK(sharp->first == 6);
    CHECK_FALSE(audio::parseKey("H minor", "major"));
    auto bad = audio::parseKey("A minr", "major");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().hint.find("minor") != std::string::npos);

    auto chords = audio::parseProgression("i VI III VII", key->first, key->second, 48);
    REQUIRE(chords);
    REQUIRE(chords->size() == 4);
    auto pcs = [](const audio::Chord& c) {
        std::vector<int> v;
        for (int n : c.notes) v.push_back(n % 12);
        return v;
    };
    CHECK(pcs((*chords)[0]) == std::vector<int>{9, 0, 4});   // Am: A C E
    CHECK(pcs((*chords)[1]) == std::vector<int>{5, 9, 0});   // F: F A C
    CHECK(pcs((*chords)[2]) == std::vector<int>{0, 4, 7});   // C: C E G
    CHECK(pcs((*chords)[3]) == std::vector<int>{7, 11, 2});  // G: G B D
    // Borrowed chords are measured from the major scale: bVII in D mixolydian... or any mode: a minor 7th above the tonic.
    auto borrowed = audio::parseProgression("I bVII", 2, {0, 2, 4, 5, 7, 9, 11}, 48);
    REQUIRE(borrowed);
    CHECK((*borrowed)[1].notes[0] % 12 == 0);  // C in key of D
    auto seventh = audio::parseProgression("V7", 0, {0, 2, 4, 5, 7, 9, 11}, 48);
    REQUIRE(seventh);
    CHECK((*seventh)[0].notes.size() == 4);
    auto junk = audio::parseProgression("I X IV", 0, {0, 2, 4, 5, 7, 9, 11}, 48);
    REQUIRE_FALSE(junk);
    CHECK(junk.error().message.find("X") != std::string::npos);
}

TEST_CASE("music: deterministic, exactly bars long, stereo, and loops without a jump") {
    audio::MusicParams p;
    p.mood = "adventure";
    p.bars = 4;
    p.tempo = 120;
    p.seed = 11;
    auto a = audio::renderMusic(p), b = audio::renderMusic(p);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->samples == b->samples);
    CHECK(a->channels == 2);
    // 4 bars at 120 bpm in 4/4 = 8 seconds.
    CHECK(a->duration() == doctest::Approx(8.0).epsilon(0.001));
    audio::Stats st = audio::analyze(*a);
    CHECK(st.loudnessLufs == doctest::Approx(-18.0).epsilon(0.05));
    CHECK(st.peak <= 0.9);
    CHECK(st.clipped == 0);
    CHECK(std::fabs(st.dcOffset) < 0.005);
    p.seed = 12;
    auto c = audio::renderMusic(p);
    REQUIRE(c);
    CHECK(c->samples != a->samples);

    for (const auto& mood : audio::musicMoods()) {
        INFO(mood);
        audio::MusicParams mp;
        mp.mood = mood;
        mp.bars = 2;
        auto pcm = audio::renderMusic(mp);
        REQUIRE(pcm);
        CHECK(audio::analyze(*pcm).peak > 0.05);
        for (float s : pcm->samples) REQUIRE(std::isfinite(s));
    }
    audio::MusicParams bad;
    bad.mood = "happpy";
    auto err = audio::renderMusic(bad);
    REQUIRE_FALSE(err);
    CHECK(err.error().hint.find("happy") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Spatial math and the audio system
// ---------------------------------------------------------------------------

TEST_CASE("audio: distance attenuation math matches the mixer's models") {
    using audio::Attenuation;
    using audio::distanceGain;
    CHECK(distanceGain(Attenuation::Inverse, 1.f, 1.f, 50.f, 1.f) == doctest::Approx(1.f));
    CHECK(distanceGain(Attenuation::Inverse, 0.2f, 1.f, 50.f, 1.f) == doctest::Approx(1.f));  // inside min distance
    CHECK(distanceGain(Attenuation::Inverse, 2.f, 1.f, 50.f, 1.f) == doctest::Approx(0.5f));
    CHECK(distanceGain(Attenuation::Inverse, 11.f, 1.f, 50.f, 1.f) == doctest::Approx(1.f / 11.f));
    CHECK(distanceGain(Attenuation::Inverse, 11.f, 1.f, 50.f, 2.f) == doctest::Approx(1.f / 21.f));
    CHECK(distanceGain(Attenuation::Inverse, 500.f, 1.f, 50.f, 1.f) == doctest::Approx(1.f / 50.f));  // clamps at max
    CHECK(distanceGain(Attenuation::Linear, 1.f, 1.f, 11.f, 1.f) == doctest::Approx(1.f));
    CHECK(distanceGain(Attenuation::Linear, 6.f, 1.f, 11.f, 1.f) == doctest::Approx(0.5f));
    CHECK(distanceGain(Attenuation::Linear, 99.f, 1.f, 11.f, 1.f) == doctest::Approx(0.f));
    CHECK(distanceGain(Attenuation::Exponential, 4.f, 1.f, 50.f, 1.f) == doctest::Approx(0.25f));
    CHECK(distanceGain(Attenuation::Exponential, 4.f, 1.f, 50.f, 2.f) == doctest::Approx(1.f / 16.f));
    CHECK(distanceGain(Attenuation::None, 40.f, 1.f, 50.f, 1.f) == doctest::Approx(1.f));
    CHECK(distanceGain(Attenuation::Inverse, 5.f, 3.f, 3.f, 1.f) == doctest::Approx(1.f));  // degenerate range: no attenuation
    CHECK(audio::parseAttenuation("linear") == Attenuation::Linear);
    CHECK(audio::parseAttenuation("anything-else") == Attenuation::Inverse);
}

TEST_CASE("audio: mix settings parse, validate and round trip") {
    audio::MixSettings m;
    CHECK(m.buses.size() == 6);
    auto parsed = audio::MixSettings::fromJson(Json::parse(R"({"buses":{"music":0.4,"sfx":{"volume":0.8,"mute":true}},"musicFade":3})").value());
    REQUIRE(parsed);
    CHECK(parsed->buses["music"].volume == doctest::Approx(0.4));
    CHECK(parsed->buses["sfx"].mute);
    CHECK(parsed->buses["master"].volume == doctest::Approx(1.0));
    CHECK(parsed->musicFade == doctest::Approx(3.0));
    auto again = audio::MixSettings::fromJson(parsed->toJson());
    REQUIRE(again);
    CHECK(again->toJson() == parsed->toJson());
    auto typo = audio::MixSettings::fromJson(Json::parse(R"({"buses":{"musik":0.4}})").value());
    REQUIRE_FALSE(typo);
    CHECK(typo.error().hint.find("music") != std::string::npos);
    CHECK_FALSE(audio::MixSettings::fromJson(Json::parse(R"({"buses":{"music":9}})").value()));
}

TEST_CASE("audio system: plays only while the game runs, and stop silences everything") {
    AudioProject proj;
    Engine& e = *proj.engine;
    proj.listener();
    EntityId src = proj.source("Source", {0, 0, -3}, R"({"loop":true})");

    // Editing: nothing from the scene is audible.
    e.update(0.1);
    CHECK(e.audio().voiceCount() == 0);

    e.step(10);
    CHECK(e.audio().voiceCount() == 1);
    CHECK(e.audio().entityPlaying(src));
    CHECK(e.audio().takePeak() > 0.01f);
    auto voices = e.audio().voices();
    REQUIRE(voices.size() == 1);
    CHECK(voices[0].kind == "entity");
    CHECK(voices[0].clip == "audio/tone.wav");
    CHECK(voices[0].spatial);
    CHECK(voices[0].distance == doctest::Approx(3.0).epsilon(0.01));
    CHECK(voices[0].distanceGain == doctest::Approx(1.0 / 3.0).epsilon(0.01));

    e.stop();
    CHECK(e.audio().voiceCount() == 0);
    (void)e.audio().takePeak();
    e.update(0.2);
    CHECK(e.audio().takePeak() == 0.f);  // silence after stop
}

TEST_CASE("audio system: spatial sources get quieter with distance (inverse model)") {
    AudioProject proj;
    Engine& e = *proj.engine;
    proj.listener();
    EntityId src = proj.source("Source", {0, 0, -2}, R"({"loop":true,"minDistance":1,"maxDistance":100})");
    auto measure = [&](float distance) {
        e.scene().get<Transform>(src)->position = {0, 0, -distance};
        e.step(8);  // let the gain smoothing settle
        (void)e.audio().takePeak();
        e.step(12);
        return e.audio().takePeak();
    };
    float near = measure(2.f), far = measure(10.f), farther = measure(40.f);
    CHECK(near > 0.05f);
    // Inverse falloff: gain 1/2 at 2 m, 1/10 at 10 m, 1/40 at 40 m.
    CHECK(near / far == doctest::Approx(5.0).epsilon(0.12));
    CHECK(far / farther == doctest::Approx(4.0).epsilon(0.12));

    // Linear rolloff reaches silence at maxDistance.
    REQUIRE(e.scene().patchComponent(src, "audio", Json::parse(R"({"rolloff":"linear","maxDistance":20})").value()));
    CHECK(measure(30.f) < 0.002f);
    // Linear gain at 11 m with maxDistance 20 is 1 - 10/19 = 0.474; near (2 m, inverse) was 0.5.
    CHECK(measure(11.f) == doctest::Approx(near * 0.474f / 0.5f).epsilon(0.1));
}

TEST_CASE("audio system: panning follows the listener's orientation") {
    AudioProject proj;
    Engine& e = *proj.engine;
    EntityId ears = proj.listener();
    proj.source("Right", {5, 0, 0}, R"({"loop":true})");
    e.step(10);
    auto stereo = e.audio().mixdown(0.2);
    REQUIRE(stereo);
    double l = 0, r = 0;
    for (size_t i = 0; i + 1 < stereo->samples.size(); i += 2) {
        l += std::fabs(stereo->samples[i]);
        r += std::fabs(stereo->samples[i + 1]);
    }
    CHECK(r > l * 3.0);  // the listener faces -Z, so +X is on the right

    // Turn the listener 180 degrees: the same source is now on the left.
    e.scene().get<Transform>(ears)->rotation = {0, 180, 0};
    e.step(10);
    stereo = e.audio().mixdown(0.2);
    REQUIRE(stereo);
    l = r = 0;
    for (size_t i = 0; i + 1 < stereo->samples.size(); i += 2) {
        l += std::fabs(stereo->samples[i]);
        r += std::fabs(stereo->samples[i + 1]);
    }
    CHECK(l > r * 3.0);
}

TEST_CASE("audio system: buses mute and scale, and pause freezes sounds") {
    AudioProject proj;
    Engine& e = *proj.engine;
    proj.listener();
    proj.source("Hum", {0, 0, -1}, R"({"loop":true,"spatial":false,"bus":"ambience"})");
    e.step(10);
    (void)e.audio().takePeak();
    e.step(10);
    float full = e.audio().takePeak();
    CHECK(full > 0.05f);

    audio::MixSettings mix;
    mix.buses["ambience"].volume = 0.5f;
    e.audio().setMix(mix);
    e.step(10);
    (void)e.audio().takePeak();
    e.step(10);
    CHECK(e.audio().takePeak() == doctest::Approx(full * 0.5f).epsilon(0.05));

    mix.buses["ambience"].mute = true;
    e.audio().setMix(mix);
    e.step(10);
    (void)e.audio().takePeak();
    e.step(10);
    CHECK(e.audio().takePeak() < 0.001f);

    e.audio().setMix(audio::MixSettings{});
    // Pause (real-time update path): sounds stop; resume restarts them.
    e.pause();
    e.update(0.05);
    CHECK_FALSE(e.audio().entityPlaying(e.scene().find("Hum")));
    e.play();
    e.update(0.05);
    CHECK(e.audio().entityPlaying(e.scene().find("Hum")));
}

TEST_CASE("audio system: playOnStart=false waits for play(), non-looping clips finish and free their voice") {
    AudioProject proj;
    Engine& e = *proj.engine;
    proj.listener();
    EntityId quiet = proj.source("Manual", {0, 0, -1}, R"({"playOnStart":false})");
    // A short clip.
    REQUIRE(audio::writeWav(proj.dir + "/audio/blip.wav", sine(880.f, 0.4f, 0.1f)));
    EntityId blip = proj.source("Blip", {0, 0, -1}, R"({"clip":"audio/blip.wav"})");
    e.step(3);
    CHECK_FALSE(e.audio().entityPlaying(quiet));
    CHECK(e.audio().entityPlaying(blip));
    e.step(31);  // 0.5 s: the 0.1 s clip is over
    CHECK_FALSE(e.audio().entityPlaying(blip));
    CHECK(e.audio().voiceCount() == 0);

    // Wander play(e) starts the manual source (scripts are edited outside play so the change sticks).
    e.stop();
    call(e, "behavior_set", R"({"entity":"Blip","name":"P","source":"on start\n play(find(\"Manual\"))\nend"})");
    e.step(3);
    CHECK(e.audio().entityPlaying(e.scene().find("Manual")));

    // stop_sound(e) silences a playing source, and it does not restart by itself.
    e.stop();
    e.scene().get<AudioSource>(e.scene().find("Manual"))->playOnStart = true;
    call(e, "behavior_set", R"({"entity":"Blip","name":"P","source":"on event \"quiet\"\n stop_sound(find(\"Manual\"))\nend"})");
    e.step(3);
    CHECK(e.audio().entityPlaying(e.scene().find("Manual")));
    e.runtime().emit("quiet");
    e.step(3);
    CHECK_FALSE(e.audio().entityPlaying(e.scene().find("Manual")));
    e.step(20);
    CHECK_FALSE(e.audio().entityPlaying(e.scene().find("Manual")));
}

TEST_CASE("audio system: Wander one-shots, music crossfade and bus volume") {
    AudioProject proj;
    Engine& e = *proj.engine;
    proj.listener();
    EntityId emitter = e.scene().create("Emitter");
    (void)emitter;
    call(e, "behavior_set", R"({"entity":"Emitter","name":"Sfx","source":"on start\n play_sound(\"audio/tone.wav\", 0.5)\n music(\"audio/tone2.wav\", 0.5)\n set_volume(\"music\", 0.25)\nend"})");
    e.step(3);
    auto voices = e.audio().voices();
    REQUIRE(voices.size() == 2);
    bool oneShot = false, music = false;
    for (const auto& v : voices) {
        oneShot = oneShot || (v.kind == "oneshot" && v.spatial && v.bus == "sfx");
        music = music || (v.kind == "music" && !v.spatial && v.bus == "music");
    }
    CHECK(oneShot);
    CHECK(music);
    CHECK(e.audio().busVolume("music") == doctest::Approx(0.25));

    // Crossfade to another track: the old one fades out and is released after the fade.
    CHECK(e.audio().playMusic("audio/tone.wav", 0.5).empty());
    CHECK(e.audio().voiceCount() == 3);
    // Asking for the track that is already playing does nothing.
    CHECK(e.audio().playMusic("audio/tone.wav", 0.5).empty());
    CHECK(e.audio().voiceCount() == 3);
    e.step(60);
    int musicVoices = 0;
    for (const auto& v : e.audio().voices()) musicVoices += v.kind == "music";
    CHECK(musicVoices == 1);
    // An empty path fades the music out entirely.
    CHECK(e.audio().playMusic("", 0.2).empty());
    e.step(30);
    musicVoices = 0;
    for (const auto& v : e.audio().voices()) musicVoices += v.kind == "music";
    CHECK(musicVoices == 0);

    // Script volume overrides end with the game.
    e.stop();
    CHECK(e.audio().busVolume("music") == doctest::Approx(1.0));
    CHECK(e.audio().voiceCount() == 0);
}

TEST_CASE("audio system: missing files report errors without breaking the engine") {
    AudioProject proj;
    Engine& e = *proj.engine;
    std::string err = e.audio().playOneShot("audio/nope.wav", 1.f, "sfx", std::nullopt);
    CHECK(err.find("not found") != std::string::npos);
    CHECK(e.audio().playMusic("audio/nope.wav", 1.f).find("not found") != std::string::npos);
    // A garbage file with a .wav name is an error, not a crash.
    {
        std::ofstream f(proj.dir + "/audio/broken.wav", std::ios::binary);
        f << "this is not audio";
    }
    CHECK_FALSE(e.audio().playOneShot("audio/broken.wav", 1.f, "sfx", std::nullopt).empty());

    call(e, "behavior_set", R"({"entity":"Missing","name":"x","source":"on start\n log \"hi\"\nend"})", false);
    EntityId id = e.scene().create("Speaker");
    call(e, "behavior_set", R"({"entity":"Speaker","name":"S","source":"on start\n play_sound(\"audio/nope.wav\")\nend"})");
    e.step(2);
    bool reported = false;
    for (const auto& m : e.recentMessages(20)) reported = reported || m.get("text").asString().find("audio file not found") != std::string::npos;
    CHECK(reported);
    (void)id;

    // Off mode: everything is a silent no-op that never errors.
    auto off = proj.open(audio::AudioMode::Off);
    CHECK(off->audio().playOneShot("audio/tone.wav", 1.f, "sfx", std::nullopt).empty());
    CHECK(off->audio().voiceCount() == 0);
    off->step(5);
    CHECK_FALSE(off->audio().deviceActive());
    ToolResult r = off->callTool("audio_play", Json::parse(R"({"clip":"audio/tone.wav"})").value(), "t");
    CHECK(r.isError);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

TEST_CASE("tools: audio_generate creates deterministic, registered, attachable audio") {
    AudioProject proj;
    Engine& e = *proj.engine;
    Json list = call(e, "audio_generate", R"({"list":true})");
    bool hasJump = false;
    for (const auto& p : list.get("presets").elements()) hasJump = hasJump || p.get("name").asString() == "jump";
    CHECK(hasJump);
    CHECK(list.get("musicMoods").size() >= 8);
    CHECK(list.get("layerParameters").contains("lowpass"));

    Json out = call(e, "audio_generate", R"({"preset":"coin","path":"audio/coin.wav","seed":4,"description":"pickup chime"})");
    REQUIRE(out.get("files").size() == 1);
    CHECK(out.get("files")[0].get("path").asString() == "audio/coin.wav");
    CHECK(out.get("files")[0].get("stats").get("duration").asNumber() > 0.3);
    CHECK(fs::exists(proj.dir + "/audio/coin.wav"));
    const AssetRecord* rec = e.assets().find("audio/coin.wav");
    REQUIRE(rec);
    CHECK((rec->type == AssetType::Audio));
    CHECK(rec->source.get("generator").asString() == "skywalker.synth");
    CHECK(rec->source.get("preset").asString() == "coin");
    CHECK(rec->source.get("seed").asInt() == 4);
    CHECK(rec->source.get("by").asString() == "agent:test");
    CHECK(rec->description == "pickup chime");

    // The same call yields byte-identical files.
    auto bytes = [&](const std::string& rel) {
        std::ifstream f(proj.dir + "/" + rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    std::string first = bytes("audio/coin.wav");
    call(e, "audio_generate", R"({"preset":"coin","path":"audio/coin2.wav","seed":4})");
    CHECK(bytes("audio/coin2.wav") == first);

    // Variations: N files with consecutive seeds, all different.
    Json vars = call(e, "audio_generate", R"({"preset":"footstep_grass","path":"audio/step.wav","variations":3})");
    REQUIRE(vars.get("files").size() == 3);
    CHECK(bytes("audio/step_1.wav") != bytes("audio/step_2.wav"));
    CHECK(fs::exists(proj.dir + "/audio/step_3.wav"));

    // Pitch and custom parameters.
    call(e, "audio_generate", R"({"preset":"jump","pitch":0.5,"path":"audio/low_jump.wav"})");
    Json custom = call(e, "audio_generate", R"({"params":{"wave":"sine","freq":523,"decay":0.3,"reverb":{"mix":0.2}},"path":"audio/note.wav"})");
    CHECK(custom.get("files")[0].get("stats").get("duration").asNumber() > 0.3);

    // Music, attached to an entity: loops, music bus, non-spatial.
    EntityId bgm = e.scene().create("Music");
    Json music = call(e, "audio_generate",
                      R"({"music":{"mood":"calm","bars":2,"tempo":100},"path":"audio/theme.wav","entity":"Music"})");
    CHECK(music.get("loops").asBool());
    const AudioSource* a = e.scene().get<AudioSource>(bgm);
    REQUIRE(a);
    CHECK(a->clip == "audio/theme.wav");
    CHECK(a->loop);
    CHECK(a->bus == "music");
    CHECK_FALSE(a->spatial);

    // Ambience presets attach as looping ambience.
    EntityId fire = e.scene().create("Fire");
    call(e, "audio_generate", R"({"preset":"fire_crackle_loop","entity":"Fire","path":"audio/fire.wav"})");
    CHECK(e.scene().get<AudioSource>(fire)->loop);
    CHECK(e.scene().get<AudioSource>(fire)->bus == "ambience");
    CHECK(e.scene().get<AudioSource>(fire)->spatial);

    // Errors teach.
    ToolResult typo = e.callTool("audio_generate", Json::parse(R"({"preset":"explosoin"})").value(), "t");
    REQUIRE(typo.isError);
    CHECK(typo.content.front().text.find("explosion") != std::string::npos);
    ToolResult badParam = e.callTool("audio_generate", Json::parse(R"({"params":{"layers":[{"lowpas":500}]}})").value(), "t");
    REQUIRE(badParam.isError);
    CHECK(badParam.content.front().text.find("lowpass") != std::string::npos);
    ToolResult outside = e.callTool("audio_generate", Json::parse(R"({"preset":"jump","path":"../../escape.wav"})").value(), "t");
    CHECK(outside.isError);
    ToolResult nothing = e.callTool("audio_generate", Json::parse("{}").value(), "t");
    CHECK(nothing.isError);
}

TEST_CASE("tools: audio_info analyzes clips and flags problems") {
    AudioProject proj;
    Engine& e = *proj.engine;
    call(e, "audio_generate", R"({"preset":"explosion","path":"audio/boom.wav"})");
    Json info = call(e, "audio_info", R"({"path":"audio/boom.wav"})");
    CHECK(info.get("stats").get("duration").asNumber() > 1.0);
    CHECK(info.get("stats").get("channels").asInt() == 1);
    CHECK(info.get("stats").get("sampleRate").asInt() == 44100);
    CHECK(info.get("stats").get("peakDb").asNumber() < 0);
    CHECK(info.get("source").get("preset").asString() == "explosion");
    CHECK(info.get("warnings").size() == 0);

    // A clipped, DC-offset, abruptly cut square wave collects warnings.
    audio::Pcm bad = sine(99.3f, 1.f, 3.f);
    for (float& v : bad.samples) v = std::clamp(std::clamp(v * 4.f, -1.f, 1.f) * 0.9f + 0.3f, -1.f, 1.f);
    REQUIRE(audio::writeWav(proj.dir + "/audio/bad.wav", bad));
    Json warn = call(e, "audio_info", R"({"path":"audio/bad.wav"})");
    std::string all = warn.get("warnings").dump();
    CHECK(all.find("clipping") != std::string::npos);
    CHECK(all.find("DC offset") != std::string::npos);
    CHECK(all.find("loop point") != std::string::npos);

    // Seamless loops are reported as such.
    call(e, "audio_generate", R"({"preset":"wind_loop","path":"audio/wind.wav"})");
    Json wind = call(e, "audio_info", R"({"path":"audio/wind.wav"})");
    CHECK(wind.get("loop").get("seamless").asBool());

    ToolResult missing = e.callTool("audio_info", Json::parse(R"({"path":"audio/boomm.wav"})").value(), "t");
    REQUIRE(missing.isError);
    CHECK(missing.content.front().text.find("audio/boom.wav") != std::string::npos);
}

TEST_CASE("tools: audio_mix persists to audio.json and reloads with the project") {
    AudioProject proj;
    Engine& e = *proj.engine;
    Json out = call(e, "audio_mix", R"({"buses":{"music":0.4,"ambience":{"volume":0.7,"mute":true}},"musicFade":2})");
    CHECK(out.get("mix").get("buses").get("music").get("volume").asNumber() == doctest::Approx(0.4));
    CHECK(e.audio().mix().buses.at("ambience").mute);
    CHECK(fs::exists(proj.dir + "/audio.json"));

    auto reopened = proj.open();
    CHECK(reopened->audio().mix().buses.at("music").volume == doctest::Approx(0.4));
    CHECK(reopened->audio().mix().buses.at("ambience").volume == doctest::Approx(0.7));
    CHECK(reopened->audio().mix().musicFade == doctest::Approx(2.0));

    // Editing the file by hand is picked up too.
    {
        std::ofstream f(proj.dir + "/audio.json");
        f << R"({"buses":{"sfx":0.3}})";
    }
    reopened->reloadProjectSettings(true);
    CHECK(reopened->audio().mix().buses.at("sfx").volume == doctest::Approx(0.3));
    CHECK(reopened->audio().mix().buses.at("music").volume == doctest::Approx(1.0));

    ToolResult bad = e.callTool("audio_mix", Json::parse(R"({"buses":{"musik":0.5}})").value(), "t");
    REQUIRE(bad.isError);
    CHECK(bad.content.front().text.find("music") != std::string::npos);
    call(e, "audio_mix", R"({"reset":true})");
    CHECK(e.audio().mix().buses.at("music").volume == doctest::Approx(1.0));
}

TEST_CASE("tools: audio_play auditions clips while editing and starts them while playing") {
    AudioProject proj;
    Engine& e = *proj.engine;
    Json out = call(e, "audio_play", R"({"clip":"audio/tone.wav"})");
    CHECK(out.get("mode").asString() == "preview");
    CHECK_FALSE(out.get("device").asBool());  // tests run on the null device
    REQUIRE(e.audio().voiceCount() == 1);
    CHECK(e.audio().voices()[0].kind == "preview");
    e.update(0.1);
    CHECK(e.audio().voiceCount() == 1);  // previews survive edit-mode updates
    call(e, "audio_play", R"({"stop":true})");
    CHECK(e.audio().voiceCount() == 0);

    EntityId id = proj.source("Speaker", {0, 0, 0}, R"({"playOnStart":false})");
    call(e, "audio_play", R"({"entity":"Speaker"})");
    CHECK(e.audio().voiceCount() == 1);
    call(e, "audio_play", R"({"stop":true})");

    call(e, "sim_control", R"({"action":"step","ticks":2})");
    call(e, "audio_play", R"({"entity":"Speaker"})");
    CHECK(e.audio().entityPlaying(id));
    Json mix = call(e, "audio_mix", "{}");
    REQUIRE(mix.get("voices").size() == 1);
    CHECK(mix.get("voices")[0].get("clip").asString() == "audio/tone.wav");
    call(e, "audio_play", R"({"entity":"Speaker","stop":true})");
    CHECK_FALSE(e.audio().entityPlaying(id));
    ToolResult none = e.callTool("audio_play", Json::parse(R"({"clip":"audio/none.wav"})").value(), "t");
    CHECK(none.isError);
}

TEST_CASE("audio components: reflected fields, defaults and validation") {
    AudioProject proj;
    Scene& s = proj.engine->scene();
    EntityId id = s.create("S");
    REQUIRE(s.patchComponent(id, "audio", Json::parse(R"({"clip":"audio/tone.wav","volume":0.5,"bus":"music","rolloff":"linear"})").value()));
    const AudioSource* a = s.get<AudioSource>(id);
    REQUIRE(a);
    CHECK(a->volume == doctest::Approx(0.5));
    CHECK(a->bus == "music");
    CHECK(a->playOnStart);
    CHECK(a->spatial);
    Status badBus = s.patchComponent(id, "audio", Json::parse(R"({"bus":"musik"})").value());
    REQUIRE_FALSE(badBus);
    CHECK(badBus.error().hint.find("music") != std::string::npos);
    REQUIRE(s.patchComponent(id, "audio", Json::parse(R"({"pitch":50})").value()));
    CHECK(s.get<AudioSource>(id)->pitch == doctest::Approx(4.0));  // ranges clamp
    REQUIRE(s.patchComponent(id, "listener", Json::parse(R"({"volume":0.5})").value()));
    CHECK(s.get<AudioListener>(id)->volume == doctest::Approx(0.5));
    // Components survive scene save/load.
    Json doc = s.toJson();
    Scene other;
    REQUIRE(other.loadJson(doc));
    EntityId again = other.find("S");
    REQUIRE(again);
    CHECK(other.get<AudioSource>(again)->clip == "audio/tone.wav");
    CHECK(other.get<AudioListener>(again) != nullptr);
}
