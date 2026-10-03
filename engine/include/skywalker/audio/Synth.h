#pragma once
// Procedural sound-effect synthesizer (sfxr/bfxr lineage, extended).
//
// A sound is a stack of *layers* (oscillator or noise + pitch motion + envelope + filter +
// shaping) mixed together and run through an optional echo and reverb. The same description
// always renders the same samples: all randomness comes from a seeded PCG generator, so an
// agent can regenerate or tweak a sound reproducibly (`seed` + parameters = the sound).
//
// Looping ambiences (wind, rain, fire, waves, drones) are rendered longer than requested
// and the continuation is crossfaded into the start, so they loop without a click.

#include <string>
#include <vector>

#include "skywalker/audio/Wav.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::audio {

struct SfxLayer {
    // Source: sine | square | saw | triangle | noise | pink | brown | fm | grains
    //   fm     = sine carrier phase-modulated by a sine (bells, metallic hits, electric tones)
    //   grains = random short decaying bursts (rain drops, fire crackle, gravel, sparkles)
    std::string wave = "square";
    float freq = 440.f;          // Hz at the start (grains: center frequency)
    float slide = 0.f;           // pitch slide, octaves per second (negative falls)
    float slideAccel = 0.f;      // change of slide, octaves per second squared
    float minFreq = 0.f;         // the layer stops when the pitch falls below this (0 = never)
    std::vector<float> arp;      // semitone offsets played in order (coin: [0, 12])
    float arpRate = 12.f;        // arp steps per second
    bool arpLoop = false;        // cycle through the arp instead of holding the last note
    float vibratoDepth = 0.f;    // semitones
    float vibratoRate = 6.f;     // Hz
    float duty = 0.5f;           // square pulse width 0.05..0.95
    float dutySweep = 0.f;       // duty change per second
    float fmRatio = 2.f;         // fm modulator frequency / carrier frequency
    float fmDepth = 0.f;         // fm modulation index (follows the envelope)
    // Amplitude envelope (seconds; sustain is a level 0..1)
    float attack = 0.005f;
    float decay = 0.1f;
    float sustain = 0.f;
    float hold = 0.f;            // time spent at the sustain level
    float release = 0.05f;
    float punch = 0.f;           // extra gain right after the attack (hits, kicks)
    float gain = 1.f;
    float tremolo = 0.f;         // amplitude modulation depth 0..1
    float tremoloRate = 8.f;     // Hz
    float ampWander = 0.f;       // slow random amplitude drift 0..1 (wind gusts, surf)
    float filterWander = 0.f;    // slow random cutoff drift in octaves
    float wanderRate = 0.4f;     // Hz of the drift
    // Filters (state-variable, 12 dB/oct). 0 disables. *End values sweep over the layer's life.
    float lowpass = 0.f;
    float lowpassEnd = 0.f;
    float highpass = 0.f;
    float highpassEnd = 0.f;
    float resonance = 0.f;       // 0..1
    // Shaping
    float drive = 0.f;           // soft clipping (0 = clean)
    float bitcrush = 0.f;        // reduce bit depth (4 = gritty, 0 = off)
    float downsample = 1.f;      // sample-rate reduction factor
    // Grains
    float grainRate = 20.f;      // bursts per second
    float grainDecay = 0.02f;    // seconds
    float grainSpread = 0.5f;    // random pitch / amplitude spread 0..1
    float delay = 0.f;           // start offset in seconds (layered, multi-part sounds)
};

struct SfxEcho {
    float time = 0.f;      // seconds (0 = off)
    float feedback = 0.35f;
    float mix = 0.f;
};

struct SfxReverb {
    float mix = 0.f;       // 0 = off
    float room = 0.5f;     // 0..1 decay length
    float damping = 0.5f;  // 0..1 high-frequency absorption
};

struct SfxParams {
    std::vector<SfxLayer> layers;
    SfxEcho echo;
    SfxReverb reverb;
    float volume = 1.f;
    float normalize = 0.9f;     // peak level after rendering (0 = leave as is)
    int sampleRate = 44100;
    float duration = 0.f;       // force the length in seconds (0 = until the envelopes end)
    float loopSeconds = 0.f;    // > 0: seamless loop of exactly this length (envelopes ignored)
    uint32_t seed = 1;

    Json toJson() const;
    /// Accepts the full object or a single layer's fields at the top level. Unknown keys are
    /// errors with did-you-mean hints.
    static Result<SfxParams> fromJson(const Json& j);
};

/// Renders mono 16-bit-ready float samples. Deterministic for a given SfxParams.
Result<Pcm> renderSfx(const SfxParams& params);

/// Named presets ("jump", "coin", "explosion", "wind_loop", ...). `seed` varies the details
/// (pitch, timing) so repeated calls give natural variants (footsteps, impacts).
const std::vector<std::string>& sfxPresetNames();
Result<SfxParams> sfxPreset(const std::string& name, uint32_t seed = 1);
/// One-line description of a preset (for tool listings).
std::string sfxPresetDescription(const std::string& name);
/// True for presets that are meant to loop (wind_loop, rain_loop, ...).
bool sfxPresetLoops(const std::string& name);

/// Field documentation of a layer (name -> "what it does [range]") for tool descriptions.
Json sfxParameterDocs();

// --- DSP building blocks (shared with the music generator) ---------------------------------

/// Zavalishin TPT state-variable filter: stable under fast cutoff changes.
class StateVariableFilter {
public:
    void setup(float cutoffHz, float resonance, float sampleRate);
    void reset() { s1_ = s2_ = 0.f; }
    /// Returns low-pass; band and high-pass outputs of the last sample are available.
    float process(float x);
    float lowpass() const { return lp_; }
    float bandpass() const { return bp_; }
    float highpass() const { return hp_; }

private:
    float g_ = 0.f, k_ = 1.f, a1_ = 0.f, a2_ = 0.f, a3_ = 0.f;
    float s1_ = 0.f, s2_ = 0.f, lp_ = 0.f, bp_ = 0.f, hp_ = 0.f;
};

/// Freeverb-style reverberator (8 combs + 4 allpasses per channel).
class Reverb {
public:
    Reverb(int sampleRate, float room, float damping, bool stereoSpread);
    void process(float inL, float inR, float& outL, float& outR);

private:
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0.f;
    };
    struct Allpass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    std::vector<Comb> combL_, combR_;
    std::vector<Allpass> apL_, apR_;
    float feedback_ = 0.8f, damp_ = 0.2f;
};

float midiToHz(float midi);

}  // namespace sky::audio
