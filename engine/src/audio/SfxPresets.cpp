// Sound-effect presets. Each is a recipe of layers; the seed nudges pitch and timing so the
// same preset yields natural variants (footsteps, impacts) instead of identical copies.

#include <functional>

#include "skywalker/audio/Synth.h"
#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"

namespace sky::audio {

namespace {

using Builder = std::function<SfxParams(Random&)>;

struct Preset {
    const char* name;
    bool loops;
    const char* description;
    Builder build;
};

SfxLayer layer(const char* wave, float freq) {
    SfxLayer l;
    l.wave = wave;
    l.freq = freq;
    return l;
}

SfxParams make(std::vector<SfxLayer> layers) {
    SfxParams p;
    p.layers = std::move(layers);
    return p;
}

// Pitch / amount variation: 1 +- spread.
float vary(Random& r, float spread) { return 1.f + spread * (r.nextFloat() * 2.f - 1.f); }

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list{
        {"jump", false, "Rising square-wave hop (platformer jump)",
         [](Random& r) {
             SfxLayer a = layer("square", 270.f * vary(r, 0.08f));
             a.slide = 2.4f;
             a.duty = 0.35f;
             a.dutySweep = -0.6f;
             a.attack = 0.002f;
             a.decay = 0.15f;
             a.release = 0.04f;
             a.lowpass = 5200.f;
             SfxLayer b = layer("triangle", a.freq);
             b.slide = a.slide;
             b.decay = 0.15f;
             b.release = 0.04f;
             b.gain = 0.5f;
             return make({a, b});
         }},
        {"coin", false, "Bright two-note pickup chime (rising fourth)",
         [](Random& r) {
             float f = 988.f * vary(r, 0.02f);
             SfxLayer a = layer("square", f);
             a.arp = {0.f, 5.f};
             a.arpRate = 15.f;
             a.attack = 0.001f;
             a.decay = 0.05f;
             a.sustain = 0.5f;
             a.hold = 0.09f;
             a.release = 0.24f;
             a.duty = 0.25f;
             a.lowpass = 7000.f;
             SfxLayer b = layer("triangle", f * 2.f);
             b.arp = a.arp;
             b.arpRate = a.arpRate;
             b.decay = 0.05f;
             b.sustain = 0.4f;
             b.hold = 0.09f;
             b.release = 0.3f;
             b.gain = 0.35f;
             SfxParams p = make({a, b});
             p.reverb = {0.12f, 0.35f, 0.5f};
             return p;
         }},
        {"pickup", false, "Soft rising glass 'pling' for collecting an item",
         [](Random& r) {
             SfxLayer a = layer("triangle", 660.f * vary(r, 0.05f));
             a.arp = {0.f, 4.f, 7.f};
             a.arpRate = 20.f;
             a.decay = 0.08f;
             a.sustain = 0.3f;
             a.hold = 0.05f;
             a.release = 0.18f;
             SfxLayer b = layer("fm", a.freq * 2.f);
             b.arp = a.arp;
             b.arpRate = a.arpRate;
             b.fmRatio = 3.f;
             b.fmDepth = 1.8f;
             b.decay = 0.35f;
             b.release = 0.2f;
             b.gain = 0.3f;
             SfxParams p = make({a, b});
             p.reverb = {0.18f, 0.4f, 0.45f};
             return p;
         }},
        {"hit", false, "Punchy impact: noise snap over a low thump",
         [](Random& r) {
             SfxLayer a = layer("noise", 1000.f);
             a.lowpass = 3600.f * vary(r, 0.15f);
             a.lowpassEnd = 500.f;
             a.attack = 0.001f;
             a.decay = 0.08f;
             a.release = 0.04f;
             a.punch = 1.5f;
             SfxLayer b = layer("sine", 160.f * vary(r, 0.1f));
             b.slide = -3.f;
             b.attack = 0.001f;
             b.decay = 0.12f;
             b.release = 0.05f;
             b.gain = 1.2f;
             return make({a, b});
         }},
        {"explosion", false, "Big explosion: crack, low boom and a rolling rumble tail",
         [](Random& r) {
             SfxLayer boom = layer("brown", 100.f);
             boom.lowpass = 2600.f * vary(r, 0.1f);
             boom.lowpassEnd = 140.f;
             boom.attack = 0.002f;
             boom.decay = 1.1f;
             boom.release = 0.6f;
             boom.punch = 1.8f;
             boom.drive = 1.5f;
             boom.gain = 1.6f;
             SfxLayer sub = layer("sine", 72.f * vary(r, 0.1f));
             sub.slide = -0.7f;
             sub.attack = 0.004f;
             sub.decay = 1.0f;
             sub.release = 0.5f;
             sub.gain = 1.4f;
             SfxLayer crack = layer("noise", 1000.f);
             crack.highpass = 1500.f;
             crack.lowpass = 9000.f;
             crack.lowpassEnd = 2000.f;
             crack.attack = 0.001f;
             crack.decay = 0.16f;
             crack.release = 0.05f;
             crack.gain = 0.7f;
             SfxLayer debris = layer("grains", 1800.f);
             debris.grainRate = 90.f;
             debris.grainDecay = 0.012f;
             debris.grainSpread = 0.9f;
             debris.attack = 0.05f;
             debris.decay = 0.9f;
             debris.release = 0.5f;
             debris.gain = 0.35f;
             debris.delay = 0.08f;
             SfxParams p = make({boom, sub, crack, debris});
             p.reverb = {0.3f, 0.65f, 0.55f};
             return p;
         }},
        {"laser", false, "Sci-fi blaster: falling saw zap",
         [](Random& r) {
             SfxLayer a = layer("saw", 1900.f * vary(r, 0.06f));
             a.slide = -3.4f;
             a.minFreq = 140.f;
             a.attack = 0.001f;
             a.decay = 0.2f;
             a.release = 0.03f;
             a.lowpass = 7000.f;
             a.lowpassEnd = 1200.f;
             a.resonance = 0.4f;
             SfxLayer b = layer("square", 950.f);
             b.slide = -3.f;
             b.duty = 0.2f;
             b.decay = 0.16f;
             b.release = 0.03f;
             b.gain = 0.4f;
             SfxParams p = make({a, b});
             p.echo = {0.07f, 0.3f, 0.18f};
             return p;
         }},
        {"powerup", false, "Rising arpeggio fanfare for a power-up",
         [](Random& r) {
             SfxLayer a = layer("square", 392.f * vary(r, 0.03f));
             a.arp = {0.f, 4.f, 7.f, 12.f, 16.f, 19.f, 24.f};
             a.arpRate = 17.f;
             a.slide = 0.04f;
             a.duty = 0.4f;
             a.dutySweep = -0.3f;
             a.attack = 0.002f;
             a.decay = 0.1f;
             a.sustain = 0.5f;
             a.hold = 0.28f;
             a.release = 0.25f;
             a.vibratoDepth = 0.15f;
             a.vibratoRate = 9.f;
             a.lowpass = 6500.f;
             SfxLayer b = layer("triangle", a.freq * 2.f);
             b.arp = a.arp;
             b.arpRate = a.arpRate;
             b.decay = 0.1f;
             b.sustain = 0.5f;
             b.hold = 0.28f;
             b.release = 0.3f;
             b.gain = 0.5f;
             SfxParams p = make({a, b});
             p.reverb = {0.2f, 0.45f, 0.5f};
             return p;
         }},
        {"footstep_grass", false, "Soft footstep on grass (seed varies each step)",
         [](Random& r) {
             SfxLayer a = layer("pink", 500.f);
             a.lowpass = 2200.f * vary(r, 0.2f);
             a.highpass = 350.f;
             a.attack = 0.006f;
             a.decay = 0.07f;
             a.release = 0.05f;
             SfxLayer b = layer("noise", 1000.f);
             b.highpass = 3500.f;
             b.attack = 0.005f;
             b.decay = 0.05f;
             b.release = 0.05f;
             b.gain = 0.25f;
             b.delay = 0.02f;
             SfxLayer c = layer("sine", 90.f * vary(r, 0.15f));
             c.slide = -1.f;
             c.decay = 0.06f;
             c.release = 0.03f;
             c.gain = 0.35f;
             return make({a, b, c});
         }},
        {"footstep_stone", false, "Hard footstep on stone with a touch of room echo",
         [](Random& r) {
             SfxLayer click = layer("noise", 1000.f);
             click.highpass = 1400.f;
             click.lowpass = 6500.f * vary(r, 0.15f);
             click.attack = 0.0005f;
             click.decay = 0.025f;
             click.release = 0.02f;
             click.punch = 1.f;
             SfxLayer body = layer("triangle", 190.f * vary(r, 0.15f));
             body.slide = -1.6f;
             body.decay = 0.06f;
             body.release = 0.04f;
             body.gain = 0.7f;
             SfxParams p = make({click, body});
             p.reverb = {0.16f, 0.3f, 0.6f};
             return p;
         }},
        {"footstep_wood", false, "Hollow footstep on wooden planks",
         [](Random& r) {
             SfxLayer knock = layer("sine", 170.f * vary(r, 0.12f));
             knock.slide = -1.2f;
             knock.attack = 0.001f;
             knock.decay = 0.09f;
             knock.release = 0.05f;
             knock.punch = 1.f;
             SfxLayer body = layer("noise", 1000.f);
             body.lowpass = 1500.f;
             body.highpass = 200.f;
             body.resonance = 0.6f;
             body.decay = 0.05f;
             body.release = 0.04f;
             body.gain = 0.6f;
             SfxLayer overtone = layer("fm", 340.f * vary(r, 0.12f));
             overtone.fmRatio = 1.5f;
             overtone.fmDepth = 1.2f;
             overtone.decay = 0.1f;
             overtone.release = 0.05f;
             overtone.gain = 0.25f;
             SfxParams p = make({knock, body, overtone});
             p.reverb = {0.1f, 0.3f, 0.7f};
             return p;
         }},
        {"door", false, "Door creaking open and thudding shut",
         [](Random& r) {
             SfxLayer creak = layer("saw", 150.f * vary(r, 0.1f));
             creak.slide = 0.25f;
             creak.vibratoDepth = 0.5f;
             creak.vibratoRate = 4.f;
             creak.tremolo = 0.5f;
             creak.tremoloRate = 15.f;
             creak.lowpass = 1100.f;
             creak.highpass = 180.f;
             creak.resonance = 0.6f;
             creak.attack = 0.08f;
             creak.decay = 0.25f;
             creak.sustain = 0.55f;
             creak.hold = 0.55f;
             creak.release = 0.25f;
             creak.gain = 0.7f;
             SfxLayer thud = layer("sine", 75.f);
             thud.slide = -1.f;
             thud.decay = 0.3f;
             thud.release = 0.1f;
             thud.punch = 1.f;
             thud.delay = 1.1f;
             SfxLayer thudNoise = layer("noise", 1000.f);
             thudNoise.lowpass = 500.f;
             thudNoise.decay = 0.15f;
             thudNoise.release = 0.05f;
             thudNoise.delay = 1.1f;
             thudNoise.gain = 0.8f;
             SfxParams p = make({creak, thud, thudNoise});
             p.reverb = {0.16f, 0.4f, 0.6f};
             return p;
         }},
        {"ui_click", false, "Short crisp interface click",
         [](Random& r) {
             SfxLayer a = layer("triangle", 1900.f * vary(r, 0.03f));
             a.attack = 0.0005f;
             a.decay = 0.022f;
             a.release = 0.01f;
             SfxLayer b = layer("noise", 1000.f);
             b.highpass = 4500.f;
             b.attack = 0.0005f;
             b.decay = 0.008f;
             b.release = 0.004f;
             b.gain = 0.25f;
             return make({a, b});
         }},
        {"ui_hover", false, "Very soft, quiet rising blip for hovering a control",
         [](Random& r) {
             SfxLayer a = layer("sine", 1250.f * vary(r, 0.02f));
             a.slide = 0.7f;
             a.attack = 0.006f;
             a.decay = 0.04f;
             a.release = 0.03f;
             SfxParams p = make({a});
             p.normalize = 0.35f;
             return p;
         }},
        {"error", false, "Low double buzz for an invalid action",
         [](Random& r) {
             SfxLayer a = layer("square", 180.f * vary(r, 0.04f));
             a.arp = {0.f, -4.f};
             a.arpRate = 9.f;
             a.arpLoop = true;
             a.duty = 0.45f;
             a.attack = 0.002f;
             a.decay = 0.04f;
             a.sustain = 0.8f;
             a.hold = 0.2f;
             a.release = 0.06f;
             a.lowpass = 1600.f;
             a.drive = 0.5f;
             return make({a});
         }},
        {"swoosh", false, "Air swoosh of a fast swing or whoosh-by",
         [](Random& r) {
             SfxLayer a = layer("pink", 500.f);
             a.lowpass = 500.f;
             a.lowpassEnd = 5200.f * vary(r, 0.1f);
             a.highpass = 200.f;
             a.highpassEnd = 1500.f;
             a.resonance = 0.35f;
             a.attack = 0.07f;
             a.decay = 0.14f;
             a.release = 0.12f;
             return make({a});
         }},
        {"shoot", false, "Gunshot: sharp crack, thump and a short tail",
         [](Random& r) {
             SfxLayer crack = layer("noise", 1000.f);
             crack.highpass = 900.f;
             crack.lowpass = 9000.f;
             crack.lowpassEnd = 1800.f;
             crack.attack = 0.0005f;
             crack.decay = 0.07f;
             crack.release = 0.04f;
             crack.punch = 2.f;
             SfxLayer thump = layer("sine", 120.f * vary(r, 0.08f));
             thump.slide = -3.f;
             thump.attack = 0.001f;
             thump.decay = 0.12f;
             thump.release = 0.05f;
             thump.gain = 1.1f;
             SfxParams p = make({crack, thump});
             p.reverb = {0.2f, 0.5f, 0.5f};
             return p;
         }},
        {"wind_loop", true, "Seamless wind: gusting filtered noise with airy whistle",
         [](Random&) {
             SfxLayer body = layer("pink", 400.f);
             body.lowpass = 700.f;
             body.highpass = 110.f;
             body.resonance = 0.35f;
             body.filterWander = 1.4f;
             body.wanderRate = 0.22f;
             body.ampWander = 0.75f;
             SfxLayer air = layer("noise", 1000.f);
             air.highpass = 2200.f;
             air.lowpass = 5200.f;
             air.resonance = 0.5f;
             air.filterWander = 1.2f;
             air.ampWander = 0.9f;
             air.wanderRate = 0.18f;
             air.gain = 0.1f;
             SfxParams p = make({body, air});
             p.loopSeconds = 14.f;
             return p;
         }},
        {"rain_loop", true, "Seamless steady rain with soft patter",
         [](Random&) {
             SfxLayer hiss = layer("noise", 1000.f);
             hiss.highpass = 1400.f;
             hiss.lowpass = 9000.f;
             hiss.gain = 0.3f;
             hiss.ampWander = 0.25f;
             SfxLayer body = layer("pink", 400.f);
             body.lowpass = 600.f;
             body.gain = 0.28f;
             body.ampWander = 0.3f;
             SfxLayer drops = layer("grains", 2300.f);
             drops.grainRate = 160.f;
             drops.grainDecay = 0.006f;
             drops.grainSpread = 0.8f;
             drops.gain = 0.55f;
             SfxLayer big = layer("grains", 900.f);
             big.grainRate = 12.f;
             big.grainDecay = 0.02f;
             big.grainSpread = 0.7f;
             big.gain = 0.4f;
             SfxParams p = make({hiss, body, drops, big});
             p.loopSeconds = 12.f;
             p.reverb = {0.12f, 0.4f, 0.6f};
             return p;
         }},
        {"fire_crackle_loop", true, "Seamless campfire: rumble, hiss and random crackles",
         [](Random&) {
             SfxLayer rumble = layer("brown", 100.f);
             rumble.lowpass = 420.f;
             rumble.gain = 0.55f;
             rumble.ampWander = 0.6f;
             rumble.wanderRate = 0.7f;
             SfxLayer hiss = layer("pink", 400.f);
             hiss.highpass = 1500.f;
             hiss.lowpass = 6500.f;
             hiss.gain = 0.14f;
             hiss.ampWander = 0.6f;
             hiss.wanderRate = 1.1f;
             SfxLayer crackle = layer("grains", 2600.f);
             crackle.grainRate = 13.f;
             crackle.grainDecay = 0.004f;
             crackle.grainSpread = 0.9f;
             crackle.gain = 1.0f;
             SfxLayer pops = layer("grains", 6000.f);
             pops.grainRate = 55.f;
             pops.grainDecay = 0.0015f;
             pops.grainSpread = 0.8f;
             pops.gain = 0.35f;
             SfxParams p = make({rumble, hiss, crackle, pops});
             p.loopSeconds = 10.f;
             return p;
         }},
        {"ocean_waves_loop", true, "Seamless sea: slow swells with foamy wash",
         [](Random&) {
             SfxLayer swell = layer("pink", 300.f);
             swell.lowpass = 1200.f;
             swell.highpass = 60.f;
             swell.filterWander = 0.6f;
             swell.wanderRate = 0.15f;
             swell.tremolo = 0.85f;
             swell.tremoloRate = 0.125f;
             SfxLayer foam = layer("noise", 1000.f);
             foam.highpass = 1800.f;
             foam.lowpass = 6500.f;
             foam.tremolo = 1.f;
             foam.tremoloRate = 0.125f;
             foam.ampWander = 0.5f;
             foam.wanderRate = 0.3f;
             foam.gain = 0.22f;
             SfxLayer deep = layer("brown", 80.f);
             deep.lowpass = 250.f;
             deep.gain = 0.5f;
             deep.ampWander = 0.5f;
             deep.wanderRate = 0.12f;
             SfxParams p = make({swell, foam, deep});
             p.loopSeconds = 16.f;
             return p;
         }},
        {"ambient_drone", true, "Seamless dark ambient drone pad (tension, caves, space)",
         [](Random&) {
             SfxLayer root = layer("saw", 55.f);
             root.lowpass = 320.f;
             root.filterWander = 0.8f;
             root.wanderRate = 0.1f;
             root.resonance = 0.2f;
             root.gain = 0.7f;
             SfxLayer fifth = layer("triangle", 82.41f);
             fifth.ampWander = 0.5f;
             fifth.wanderRate = 0.08f;
             fifth.gain = 0.6f;
             SfxLayer shimmer = layer("saw", 110.4f);
             shimmer.lowpass = 600.f;
             shimmer.filterWander = 1.f;
             shimmer.wanderRate = 0.12f;
             shimmer.ampWander = 0.7f;
             shimmer.gain = 0.35f;
             SfxLayer detune = layer("saw", 55.35f);
             detune.lowpass = 300.f;
             detune.gain = 0.5f;
             SfxLayer air = layer("pink", 400.f);
             air.lowpass = 900.f;
             air.highpass = 200.f;
             air.ampWander = 0.8f;
             air.wanderRate = 0.1f;
             air.gain = 0.12f;
             SfxParams p = make({root, fifth, shimmer, detune, air});
             p.loopSeconds = 20.f;
             p.reverb = {0.45f, 0.9f, 0.6f};
             return p;
         }},
        {"heartbeat", true, "Looping heartbeat (lub-dub) at 70 bpm",
         [](Random&) {
             SfxLayer lub = layer("sine", 64.f);
             lub.slide = -0.9f;
             lub.attack = 0.006f;
             lub.decay = 0.11f;
             lub.release = 0.05f;
             lub.punch = 1.f;
             lub.lowpass = 240.f;
             lub.resonance = 0.3f;
             SfxLayer dub = layer("sine", 56.f);
             dub.slide = -0.9f;
             dub.attack = 0.006f;
             dub.decay = 0.1f;
             dub.release = 0.05f;
             dub.lowpass = 220.f;
             dub.gain = 0.7f;
             dub.delay = 0.24f;
             SfxParams p = make({lub, dub});
             p.duration = 60.f / 70.f;
             p.reverb = {0.08f, 0.3f, 0.7f};
             return p;
         }},
    };
    return list;
}

}  // namespace

const std::vector<std::string>& sfxPresetNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& p : presets()) n.emplace_back(p.name);
        return n;
    }();
    return names;
}

Result<SfxParams> sfxPreset(const std::string& name, uint32_t seed) {
    for (const auto& p : presets()) {
        if (name == p.name) {
            Random rng(seed * 7919u + 3u);
            SfxParams params = p.build(rng);
            params.seed = seed;
            return params;
        }
    }
    std::string guess = str::closest(name, sfxPresetNames(), 4);
    std::string list;
    for (const auto& n : sfxPresetNames()) list += (list.empty() ? "" : ", ") + n;
    return Error::make("not_found", "unknown sound preset '" + name + "'",
                       guess.empty() ? "presets: " + list : "did you mean '" + guess + "'? (presets: " + list + ")");
}

std::string sfxPresetDescription(const std::string& name) {
    for (const auto& p : presets()) {
        if (name == p.name) return p.description;
    }
    return {};
}

bool sfxPresetLoops(const std::string& name) {
    for (const auto& p : presets()) {
        if (name == p.name) return p.loops;
    }
    return false;
}

}  // namespace sky::audio
