#include "skywalker/audio/Synth.h"

#include <algorithm>
#include <cmath>

#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"

namespace sky::audio {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr double kMaxSeconds = 40.0;

float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    }
    if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}

/// Smooth random drift in [-1, 1]: cosine interpolation between random knots.
class Wander {
public:
    Wander(Random& rng, float rateHz, int sampleRate) : rng_(rng) {
        step_ = rateHz > 0.f ? rateHz / static_cast<float>(sampleRate) : 0.f;
        a_ = rng_.range(-1.f, 1.f);
        b_ = rng_.range(-1.f, 1.f);
    }
    float next() {
        pos_ += step_;
        if (pos_ >= 1.f) {
            pos_ -= 1.f;
            a_ = b_;
            b_ = rng_.range(-1.f, 1.f);
        }
        float t = 0.5f - 0.5f * std::cos(pos_ * 3.14159265f);
        return a_ + (b_ - a_) * t;
    }

private:
    Random& rng_;
    float step_ = 0.f, pos_ = 0.f, a_ = 0.f, b_ = 0.f;
};

struct Grain {
    float phase = 0.f, freq = 0.f, amp = 0.f, decay = 0.f, age = 0.f, noiseMix = 0.f;
    bool live = false;
};

float envelopeAt(const SfxLayer& l, float t) {
    const float a = std::max(l.attack, 1e-4f);
    if (t < a) return t / a;
    t -= a;
    if (t < l.decay) {
        float x = t / std::max(l.decay, 1e-4f);
        return l.sustain + (1.f - l.sustain) * (1.f - x) * (1.f - x);
    }
    t -= l.decay;
    if (t < l.hold) return l.sustain;
    t -= l.hold;
    if (t < l.release) {
        float x = t / std::max(l.release, 1e-4f);
        return l.sustain * (1.f - x) * (1.f - x);
    }
    return 0.f;
}

float layerLength(const SfxLayer& l) { return l.delay + std::max(l.attack, 1e-4f) + l.decay + l.hold + l.release; }

/// Renders one layer, adding it into `mix`. Loop mode ignores the envelope (continuous signal).
void renderLayer(const SfxLayer& l, uint32_t seed, int sr, bool loop, std::vector<float>& mix) {
    Random rng(seed);
    Wander ampDrift(rng, l.wanderRate, sr), cutDrift(rng, l.wanderRate * 0.7f, sr);
    const float fs = static_cast<float>(sr);
    const size_t frames = mix.size();
    const size_t start = static_cast<size_t>(std::max(0.f, l.delay) * fs);
    const float len = layerLength(l) - l.delay;

    StateVariableFilter lp, hp;
    float phase = 0.f, modPhase = 0.f;
    float pink[7] = {0, 0, 0, 0, 0, 0, 0}, brown = 0.f;
    std::vector<Grain> grains(48);
    const bool useLp = l.lowpass > 0.f, useHp = l.highpass > 0.f;
    const float crushLevels = l.bitcrush > 0.f ? std::pow(2.f, std::clamp(l.bitcrush, 1.f, 16.f) - 1.f) : 0.f;
    const int hold = std::max(1, static_cast<int>(l.downsample));
    float duty = l.duty;
    float cutMul = 1.f, ampMul = 1.f;
    float lastOut = 0.f;

    for (size_t n = start; n < frames; ++n) {
        const float t = static_cast<float>(n - start) / fs;
        float env = 1.f;
        if (!loop) {
            if (t >= len) break;
            env = envelopeAt(l, t);
            if (l.punch > 0.f) env *= 1.f + l.punch * std::exp(-std::max(0.f, t - l.attack) / 0.04f);
        }

        // --- pitch -------------------------------------------------------------------------
        float oct = l.slide * t + 0.5f * l.slideAccel * t * t;
        float f = l.freq * std::exp2(std::clamp(oct, -12.f, 12.f));
        if (!l.arp.empty()) {
            auto step = static_cast<size_t>(std::max(0.f, t * l.arpRate));
            size_t idx = l.arpLoop ? step % l.arp.size() : std::min(step, l.arp.size() - 1);
            f *= std::exp2(l.arp[idx] / 12.f);
        }
        if (l.vibratoDepth > 0.f) f *= std::exp2(l.vibratoDepth / 12.f * std::sin(kTwoPi * l.vibratoRate * t));
        if (l.minFreq > 0.f && f < l.minFreq) break;
        f = std::clamp(f, 1.f, fs * 0.45f);
        const float dt = f / fs;
        phase += dt;
        phase -= std::floor(phase);

        // --- source ------------------------------------------------------------------------
        float v = 0.f;
        const std::string& w = l.wave;
        if (w == "sine") {
            v = std::sin(kTwoPi * phase);
        } else if (w == "square") {
            duty = std::clamp(duty + l.dutySweep / fs, 0.05f, 0.95f);
            v = phase < duty ? 1.f : -1.f;
            v += polyBlep(phase, dt);
            v -= polyBlep(std::fmod(phase + 1.f - duty, 1.f), dt);
        } else if (w == "saw") {
            v = 2.f * phase - 1.f - polyBlep(phase, dt);
        } else if (w == "triangle") {
            v = 4.f * std::fabs(phase - 0.5f) - 1.f;
        } else if (w == "noise") {
            v = rng.range(-1.f, 1.f);
        } else if (w == "pink") {
            float white = rng.range(-1.f, 1.f);
            pink[0] = 0.99886f * pink[0] + white * 0.0555179f;
            pink[1] = 0.99332f * pink[1] + white * 0.0750759f;
            pink[2] = 0.96900f * pink[2] + white * 0.1538520f;
            pink[3] = 0.86650f * pink[3] + white * 0.3104856f;
            pink[4] = 0.55000f * pink[4] + white * 0.5329522f;
            pink[5] = -0.7616f * pink[5] - white * 0.0168980f;
            v = (pink[0] + pink[1] + pink[2] + pink[3] + pink[4] + pink[5] + pink[6] + white * 0.5362f) * 0.2f;
            pink[6] = white * 0.115926f;
        } else if (w == "brown") {
            float white = rng.range(-1.f, 1.f);
            brown = (brown + 0.02f * white) / 1.02f;
            v = brown * 3.5f;
        } else if (w == "fm") {
            modPhase += dt * l.fmRatio;
            modPhase -= std::floor(modPhase);
            v = std::sin(kTwoPi * phase + l.fmDepth * (loop ? 1.f : env) * std::sin(kTwoPi * modPhase));
        } else if (w == "grains") {
            if (rng.nextFloat() < l.grainRate / fs) {
                for (auto& g : grains) {
                    if (g.live) continue;
                    float spread = l.grainSpread;
                    g.live = true;
                    g.age = 0.f;
                    g.freq = l.freq * std::exp2(spread * rng.range(-1.5f, 1.5f));
                    g.amp = 1.f - spread * rng.nextFloat() * 0.9f;
                    g.decay = l.grainDecay * rng.range(0.5f, 1.5f);
                    g.noiseMix = rng.range(0.3f, 1.f);
                    g.phase = 0.f;
                    break;
                }
            }
            for (auto& g : grains) {
                if (!g.live) continue;
                g.age += 1.f / fs;
                if (g.age > g.decay * 6.f) {
                    g.live = false;
                    continue;
                }
                g.phase += g.freq / fs;
                float e = std::exp(-g.age / g.decay);
                float tone = std::sin(kTwoPi * g.phase);
                v += g.amp * e * (g.noiseMix * rng.range(-1.f, 1.f) + (1.f - g.noiseMix) * tone);
            }
        }

        // --- slow drift, filters, shaping --------------------------------------------------------
        if ((n & 7u) == 0) {
            float u = static_cast<float>(n - start) / std::max(1.f, loop ? static_cast<float>(frames - start) : len * fs);
            u = std::clamp(u, 0.f, 1.f);
            if (l.filterWander > 0.f) cutMul = std::exp2(l.filterWander * cutDrift.next());
            if (useLp) {
                float c = l.lowpassEnd > 0.f ? l.lowpass * std::pow(l.lowpassEnd / l.lowpass, u) : l.lowpass;
                lp.setup(std::clamp(c * cutMul, 20.f, fs * 0.45f), l.resonance, fs);
            }
            if (useHp) {
                float c = l.highpassEnd > 0.f ? l.highpass * std::pow(l.highpassEnd / l.highpass, u) : l.highpass;
                hp.setup(std::clamp(c * cutMul, 20.f, fs * 0.45f), l.resonance * 0.5f, fs);
            }
            if (l.ampWander > 0.f) ampMul = 1.f - l.ampWander * (0.5f - 0.5f * ampDrift.next());
        }
        if (useLp) v = lp.process(v);
        if (useHp) {
            hp.process(v);
            v = hp.highpass();
        }
        if (l.drive > 0.f) v = std::tanh(v * (1.f + l.drive * 6.f));
        if (crushLevels > 0.f) v = std::round(v * crushLevels) / crushLevels;
        if (hold > 1) {
            if (static_cast<int>(n) % hold == 0) lastOut = v;
            v = lastOut;
        }

        float amp = env * l.gain * ampMul;
        if (l.tremolo > 0.f) amp *= 1.f - l.tremolo * (0.5f - 0.5f * std::sin(kTwoPi * l.tremoloRate * t));
        mix[n] += v * amp;
    }
}

}  // namespace

float midiToHz(float midi) { return 440.f * std::exp2((midi - 69.f) / 12.f); }

// --- State variable filter -------------------------------------------------------------------

void StateVariableFilter::setup(float cutoffHz, float resonance, float sampleRate) {
    g_ = std::tan(3.14159265f * std::clamp(cutoffHz, 10.f, sampleRate * 0.49f) / sampleRate);
    float q = 0.707f + std::clamp(resonance, 0.f, 1.f) * 11.f;
    k_ = 1.f / q;
    a1_ = 1.f / (1.f + g_ * (g_ + k_));
    a2_ = g_ * a1_;
    a3_ = g_ * a2_;
}

float StateVariableFilter::process(float x) {
    float v3 = x - s2_;
    float v1 = a1_ * s1_ + a2_ * v3;
    float v2 = s2_ + a2_ * s1_ + a3_ * v3;
    s1_ = 2.f * v1 - s1_;
    s2_ = 2.f * v2 - s2_;
    lp_ = v2;
    bp_ = v1;
    hp_ = x - k_ * v1 - v2;
    return lp_;
}

// --- Reverb ----------------------------------------------------------------------------------

Reverb::Reverb(int sampleRate, float room, float damping, bool stereoSpread) {
    static const int combTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allpassTuning[4] = {556, 441, 341, 225};
    const float scale = static_cast<float>(sampleRate) / 44100.f;
    const int spread = stereoSpread ? 23 : 0;
    feedback_ = 0.7f + 0.28f * std::clamp(room, 0.f, 1.f);
    damp_ = 0.4f * std::clamp(damping, 0.f, 1.f);
    for (int i = 0; i < 8; ++i) {
        combL_.push_back({std::vector<float>(static_cast<size_t>(combTuning[i] * scale), 0.f), 0, 0.f});
        combR_.push_back({std::vector<float>(static_cast<size_t>((combTuning[i] + spread) * scale), 0.f), 0, 0.f});
    }
    for (int i = 0; i < 4; ++i) {
        apL_.push_back({std::vector<float>(static_cast<size_t>(allpassTuning[i] * scale), 0.f), 0});
        apR_.push_back({std::vector<float>(static_cast<size_t>((allpassTuning[i] + spread) * scale), 0.f), 0});
    }
}

void Reverb::process(float inL, float inR, float& outL, float& outR) {
    const float input = (inL + inR) * 0.015f;
    auto run = [&](std::vector<Comb>& combs, std::vector<Allpass>& aps) {
        float sum = 0.f;
        for (auto& c : combs) {
            float out = c.buf[c.pos];
            c.store = out * (1.f - damp_) + c.store * damp_;
            c.buf[c.pos] = input + c.store * feedback_;
            if (++c.pos >= c.buf.size()) c.pos = 0;
            sum += out;
        }
        for (auto& a : aps) {
            float buffered = a.buf[a.pos];
            float out = -sum + buffered;
            a.buf[a.pos] = sum + buffered * 0.5f;
            if (++a.pos >= a.buf.size()) a.pos = 0;
            sum = out;
        }
        return sum;
    };
    outL = run(combL_, apL_) * 3.f;
    outR = run(combR_, apR_) * 3.f;
}

// --- Rendering ---------------------------------------------------------------------------------

Result<Pcm> renderSfx(const SfxParams& p) {
    if (p.layers.empty()) return Error::make("invalid_arguments", "a sound needs at least one layer");
    if (p.layers.size() > 16) return Error::make("invalid_arguments", "at most 16 layers are supported");
    if (p.sampleRate < 8000 || p.sampleRate > 96000) {
        return Error::make("invalid_arguments", "sampleRate must be between 8000 and 96000");
    }
    const int sr = p.sampleRate;
    const bool loop = p.loopSeconds > 0.f;

    double seconds = 0;
    if (loop) {
        seconds = p.loopSeconds;
    } else if (p.duration > 0.f) {
        seconds = p.duration;
    } else {
        for (const auto& l : p.layers) seconds = std::max<double>(seconds, layerLength(l));
        if (p.echo.mix > 0.f && p.echo.time > 0.f) {
            double repeats = std::ceil(std::log(0.01) / std::log(std::clamp<double>(p.echo.feedback, 0.05, 0.95)));
            seconds += std::min(3.0, p.echo.time * repeats);
        }
        if (p.reverb.mix > 0.f) seconds += 0.4 + 2.2 * p.reverb.room;
    }
    if (seconds <= 0 || seconds > kMaxSeconds) {
        return Error::make("invalid_arguments", "sound length " + std::to_string(seconds) + " s is out of range (0..40 s)");
    }
    size_t loopFrames = static_cast<size_t>(seconds * sr);
    // Looping: render a continuation to crossfade into the start.
    const size_t xfade = loop ? std::min(loopFrames / 2, static_cast<size_t>(std::min(1.5, seconds * 0.4) * sr)) : 0;
    const size_t frames = loopFrames + xfade;

    std::vector<float> mix(frames, 0.f);
    for (size_t i = 0; i < p.layers.size(); ++i) {
        renderLayer(p.layers[i], p.seed * 2654435761u + static_cast<uint32_t>(i) * 40503u + 17u, sr, loop, mix);
    }

    if (p.echo.mix > 0.f && p.echo.time > 0.f) {
        size_t d = std::max<size_t>(1, static_cast<size_t>(p.echo.time * sr));
        std::vector<float> line(d, 0.f);
        size_t pos = 0;
        float fb = std::clamp(p.echo.feedback, 0.f, 0.95f);
        for (float& s : mix) {
            float delayed = line[pos];
            line[pos] = s + delayed * fb;
            s += delayed * p.echo.mix;
            if (++pos >= d) pos = 0;
        }
    }
    if (p.reverb.mix > 0.f) {
        Reverb rev(sr, p.reverb.room, p.reverb.damping, false);
        for (float& s : mix) {
            float l, r;
            rev.process(s, s, l, r);
            s += 0.5f * (l + r) * std::clamp(p.reverb.mix, 0.f, 1.f) * 2.f;
        }
    }

    // Safety: a filter blow-up must never leave NaNs in a file.
    for (float& s : mix) {
        if (!std::isfinite(s)) s = 0.f;
    }
    // DC blocker (~14 Hz): asymmetric pulses and drive leave an offset that wastes headroom and clicks.
    {
        float x1 = 0.f, y1 = 0.f;
        for (float& s : mix) {
            float y = s - x1 + 0.998f * y1;
            x1 = s;
            y1 = y;
            s = y;
        }
    }
    Pcm pcm;
    pcm.sampleRate = sr;
    pcm.channels = 1;
    pcm.samples = std::move(mix);
    if (loop) {
        pcm = makeSeamlessLoop(pcm, loopFrames);
    } else {
        if (p.duration <= 0.f) {
            // Cut the inaudible tail (reverb/echo ring-out below about -58 dB of the peak).
            float peak = 0.f;
            for (float s : pcm.samples) peak = std::max(peak, std::fabs(s));
            size_t keep = pcm.samples.size();
            while (keep > 1 && std::fabs(pcm.samples[keep - 1]) < peak * 0.0012f) --keep;
            keep = std::min(pcm.samples.size(), keep + static_cast<size_t>(0.02 * sr));
            pcm.samples.resize(std::max<size_t>(keep, 16));
        }
        fadeEdges(pcm, 0.0f, 0.006f);  // never end on a click
    }
    if (p.normalize > 0.f) {
        normalize(pcm, p.normalize);
        // Dense, buzzy sounds (square waves, noise walls) are far louder than their peak suggests:
        // keep the average level in a comfortable range as well.
        double sumSq = 0;
        for (float s : pcm.samples) sumSq += static_cast<double>(s) * s;
        double rms = std::sqrt(sumSq / std::max<size_t>(1, pcm.samples.size()));
        const double maxRms = std::pow(10.0, -15.0 / 20.0);
        if (rms > maxRms) {
            float g = static_cast<float>(maxRms / rms);
            for (float& s : pcm.samples) s *= g;
        }
    }
    for (float& s : pcm.samples) s *= p.volume;
    return pcm;
}

}  // namespace sky::audio
