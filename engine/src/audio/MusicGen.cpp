#include "skywalker/audio/MusicGen.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "skywalker/audio/Synth.h"
#include "skywalker/core/Random.h"
#include "skywalker/core/Strings.h"

namespace sky::audio {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// --- theory ------------------------------------------------------------------------------------

const std::map<std::string, std::vector<int>>& modes() {
    static const std::map<std::string, std::vector<int>> m{
        {"major", {0, 2, 4, 5, 7, 9, 11}},      {"ionian", {0, 2, 4, 5, 7, 9, 11}},
        {"minor", {0, 2, 3, 5, 7, 8, 10}},      {"aeolian", {0, 2, 3, 5, 7, 8, 10}},
        {"dorian", {0, 2, 3, 5, 7, 9, 10}},     {"phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"lydian", {0, 2, 4, 6, 7, 9, 11}},     {"mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"locrian", {0, 1, 3, 5, 6, 8, 10}},    {"harmonic_minor", {0, 2, 3, 5, 7, 8, 11}},
    };
    return m;
}

int pitchClass(char letter) {
    switch (std::tolower(static_cast<unsigned char>(letter))) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
    }
    return -1;
}

struct Mood {
    const char* name;
    const char* description;
    const char* mode;
    const char* key;
    int tempo;
    float energy;
    std::vector<const char*> progressions;
    const char* drums;   // none | soft | four | half | drive | pulse
    const char* bass;    // whole | pulse | walk | epic | none
    int timbre;          // melody timbre: 0 mellow, 1 bright, 2 bell
    float padBrightness; // 0..1
    float reverb;
};

const std::vector<Mood>& moodTable() {
    static const std::vector<Mood> t{
        {"calm", "Gentle, warm, slow; soft pads and a light arpeggio", "major", "C", 72, 0.25f,
         {"I vi IV V", "I V vi IV", "I iii IV V"}, "soft", "whole", 0, 0.45f, 0.45f},
        {"happy", "Bright and bouncy major-key groove", "major", "G", 124, 0.7f,
         {"I V vi IV", "I IV V IV", "I vi ii V"}, "four", "walk", 1, 0.8f, 0.25f},
        {"sad", "Slow, melancholic minor with a plaintive melody", "minor", "A", 66, 0.3f,
         {"i VI III VII", "i iv VI V", "i VII VI VII"}, "none", "whole", 0, 0.35f, 0.5f},
        {"tense", "Pulsing low bass, dissonant color, a heartbeat of drums", "phrygian", "E", 96, 0.6f,
         {"i bII i v", "i iv bVI V", "i bII bIII bII"}, "pulse", "pulse", 2, 0.4f, 0.35f},
        {"epic", "Big minor-key cinematic theme with heavy drums", "minor", "D", 100, 0.9f,
         {"i VI III VII", "i VII VI VII", "i iv VI VII"}, "half", "epic", 1, 0.75f, 0.4f},
        {"mysterious", "Dorian wander, sparse bells and shifting color", "dorian", "D", 80, 0.35f,
         {"i IV i VII", "i ii i IV", "i VII IV i"}, "none", "whole", 2, 0.4f, 0.5f},
        {"eerie", "Unsettling, slow, near-static with cold bells", "phrygian", "F#", 60, 0.15f,
         {"i bII i bII", "i bII bVII i"}, "none", "none", 2, 0.3f, 0.6f},
        {"adventure", "Heroic, open mixolydian travel music", "mixolydian", "D", 112, 0.65f,
         {"I bVII IV I", "I V IV V", "I IV bVII IV"}, "four", "walk", 1, 0.7f, 0.3f},
        {"battle", "Fast, driving minor with relentless drums", "minor", "E", 150, 1.0f,
         {"i VI VII i", "i VII VI V", "i iv VII VI"}, "drive", "pulse", 1, 0.7f, 0.2f},
    };
    return t;
}

const Mood* findMood(const std::string& name) {
    for (const auto& m : moodTable()) {
        if (name == m.name) return &m;
    }
    return nullptr;
}

}  // namespace

const std::vector<std::string>& musicMoods() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& m : moodTable()) n.emplace_back(m.name);
        return n;
    }();
    return names;
}

std::string musicMoodDescription(const std::string& mood) {
    const Mood* m = findMood(mood);
    return m ? m->description : std::string();
}

Result<std::pair<int, std::vector<int>>> parseKey(const std::string& key, const std::string& defaultMode) {
    std::string k = str::trim(key);
    if (k.empty()) return Error::make("invalid_arguments", "empty key");
    int pc = pitchClass(k[0]);
    if (pc < 0) {
        return Error::make("invalid_arguments", "key '" + key + "' must start with a note name A..G", "e.g. \"A minor\", \"C# dorian\"");
    }
    size_t i = 1;
    if (i < k.size() && (k[i] == '#' || k[i] == 'b')) {
        pc += k[i] == '#' ? 1 : -1;
        ++i;
    }
    pc = ((pc % 12) + 12) % 12;
    std::string modeName = str::lower(str::trim(k.substr(i)));
    if (modeName.empty()) modeName = defaultMode;
    for (char& c : modeName) {
        if (c == ' ') c = '_';
    }
    auto it = modes().find(modeName);
    if (it == modes().end()) {
        std::vector<std::string> names;
        for (const auto& [n, _] : modes()) names.push_back(n);
        std::string guess = str::closest(modeName, names, 3);
        return Error::make("invalid_arguments", "unknown mode '" + modeName + "' in key '" + key + "'",
                           guess.empty() ? "modes: major, minor, dorian, phrygian, lydian, mixolydian, locrian, harmonic_minor"
                                         : "did you mean '" + guess + "'?");
    }
    return std::make_pair(pc, it->second);
}

Result<std::vector<Chord>> parseProgression(const std::string& progression, int tonic, const std::vector<int>& scale,
                                            int baseMidi) {
    static const char* numerals[] = {"vii", "iii", "vi", "iv", "ii", "v", "i"};  // longest first
    static const int degrees[] = {6, 2, 5, 3, 1, 4, 0};
    std::vector<Chord> chords;
    for (const std::string& tokenRaw : str::split(progression, ' ')) {
        std::string token = str::trim(tokenRaw);
        if (token.empty()) continue;
        size_t pos = 0;
        int accidental = 0;
        if (token[pos] == 'b' || token[pos] == '#') {
            accidental = token[pos] == 'b' ? -1 : 1;
            ++pos;
        }
        std::string rest = token.substr(pos);
        std::string lowered = str::lower(rest);
        int degree = -1;
        size_t len = 0;
        for (size_t n = 0; n < 7; ++n) {
            if (str::startsWith(lowered, numerals[n])) {
                degree = degrees[n];
                len = std::string(numerals[n]).size();
                break;
            }
        }
        if (degree < 0) {
            return Error::make("invalid_arguments", "cannot read chord '" + token + "' in progression",
                               "use roman numerals like \"i VI III VII\" (uppercase = major, lowercase = minor, b = flat)");
        }
        bool upper = std::isupper(static_cast<unsigned char>(rest[0])) != 0;
        std::string suffix = lowered.substr(len);
        int root = baseMidi + tonic + scale[static_cast<size_t>(degree)] + accidental;
        int third = upper ? 4 : 3, fifth = 7;
        Chord c;
        if (suffix == "dim" || suffix == "o" || suffix == "°") {
            third = 3;
            fifth = 6;
        } else if (suffix == "sus4") {
            third = 5;
        } else if (suffix == "sus2") {
            third = 2;
        }
        c.notes = {root, root + third, root + fifth};
        if (suffix == "7") c.notes.push_back(root + 10);
        else if (suffix == "maj7") c.notes.push_back(root + 11);
        else if (suffix == "add9") c.notes.push_back(root + 14);
        static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        c.name = std::string(names[((root % 12) + 12) % 12]) + (third == 3 ? "m" : "") + (fifth == 6 ? "dim" : "") +
                 (suffix == "7" ? "7" : suffix == "maj7" ? "maj7" : "");
        chords.push_back(std::move(c));
    }
    if (chords.empty()) return Error::make("invalid_arguments", "progression has no chords");
    return chords;
}

namespace {

// --- stems and instruments -------------------------------------------------------------------------

struct Stem {
    std::vector<float> l, r;
    explicit Stem(size_t n) : l(n, 0.f), r(n, 0.f) {}
    size_t size() const { return l.size(); }
    // Notes wrap around the loop point so tails fold onto the start (seamless looping).
    void add(size_t pos, float vl, float vr) {
        size_t i = pos % l.size();
        l[i] += vl;
        r[i] += vr;
    }
};

float hz(float midi) { return midiToHz(midi); }

float panGainL(float pan) { return std::cos((pan * 0.5f + 0.5f) * 1.5707963f); }
float panGainR(float pan) { return std::sin((pan * 0.5f + 0.5f) * 1.5707963f); }

float saw(float phase, float dt) {
    float v = 2.f * phase - 1.f;
    // polyBLEP
    if (phase < dt) {
        float t = phase / dt;
        v -= t + t - t * t - 1.f;
    } else if (phase > 1.f - dt) {
        float t = (phase - 1.f) / dt;
        v -= t * t + t + t + 1.f;
    }
    return v;
}

/// Pad voice: three detuned saws through a slowly opening low-pass.
void padNote(Stem& s, int sr, size_t start, size_t len, float midi, float gain, float brightness) {
    const float fs = static_cast<float>(sr);
    const float attack = std::min(0.9f, static_cast<float>(len) / fs * 0.45f);
    const float release = 0.9f;
    const size_t total = len + static_cast<size_t>(release * fs);
    static const float detune[3] = {-8.f, 0.f, 8.f};
    static const float pans[3] = {-0.6f, 0.f, 0.6f};
    for (int v = 0; v < 3; ++v) {
        float f = hz(midi + detune[v] / 100.f);
        float dt = f / fs, phase = static_cast<float>(v) * 0.31f;
        StateVariableFilter lp;
        float gl = panGainL(pans[v]), gr = panGainR(pans[v]);
        for (size_t i = 0; i < total; ++i) {
            float t = static_cast<float>(i) / fs;
            float env = t < attack ? t / attack : 1.f;
            if (i >= len) {
                float x = static_cast<float>(i - len) / (release * fs);
                env *= (1.f - x) * (1.f - x);
            }
            if ((i & 15u) == 0) {
                float open = std::min(1.f, t / (attack * 1.5f + 0.01f));
                lp.setup((500.f + 2800.f * brightness) * (0.55f + 0.45f * open), 0.1f, fs);
            }
            phase += dt;
            phase -= std::floor(phase);
            float out = lp.process(saw(phase, dt)) * env * gain * 0.33f;
            s.add(start + i, out * gl, out * gr);
        }
    }
}

/// Plucked note (arpeggios): saw/triangle blend with a closing filter.
void pluckNote(Stem& s, int sr, size_t start, float len, float midi, float gain, float pan, float brightness) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>((len + 0.15f) * fs);
    float f = hz(midi), dt = f / fs, phase = 0.f;
    StateVariableFilter lp;
    float gl = panGainL(pan), gr = panGainR(pan);
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        float env = std::exp(-t / (len * 0.45f)) * std::min(1.f, t / 0.004f);
        if ((i & 7u) == 0) lp.setup(std::min(9000.f, f * (2.f + 7.f * brightness * std::exp(-t / (len * 0.35f)))), 0.15f, fs);
        phase += dt;
        phase -= std::floor(phase);
        float osc = 0.6f * saw(phase, dt) + 0.4f * (4.f * std::fabs(phase - 0.5f) - 1.f);
        float out = lp.process(osc) * env * gain;
        s.add(start + i, out * gl, out * gr);
    }
}

void bassNote(Stem& s, int sr, size_t start, float lenSec, float midi, float gain) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>((lenSec + 0.12f) * fs);
    float f = hz(midi), phase = 0.f, dt = f / fs;
    StateVariableFilter lp;
    lp.setup(std::min(700.f, f * 5.f), 0.1f, fs);
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        float env = std::min(1.f, t / 0.006f);
        if (t > lenSec) {
            float x = (t - lenSec) / 0.12f;
            env *= (1.f - std::min(1.f, x)) * (1.f - std::min(1.f, x));
        }
        env *= 0.65f + 0.35f * std::exp(-t / 0.25f);
        phase += dt;
        phase -= std::floor(phase);
        float v = std::sin(kTwoPi * phase) + 0.35f * lp.process(saw(phase, dt));
        float out = v * env * gain;
        s.add(start + i, out, out);
    }
}

/// Lead voice for the melody. timbre: 0 mellow (sine/triangle + vibrato), 1 bright (pulse), 2 bell (fm).
void leadNote(Stem& s, int sr, size_t start, float lenSec, float midi, float gain, float pan, int timbre) {
    const float fs = static_cast<float>(sr);
    const float release = timbre == 2 ? 1.2f : 0.28f;
    const size_t total = static_cast<size_t>((lenSec + release) * fs);
    float f = hz(midi), phase = 0.f, mod = 0.f;
    StateVariableFilter lp;
    float gl = panGainL(pan), gr = panGainR(pan);
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        float vib = 1.f + 0.0035f * std::sin(kTwoPi * 5.2f * t) * std::min(1.f, t / 0.35f);
        float dt = f * vib / fs;
        phase += dt;
        phase -= std::floor(phase);
        float env, v;
        if (timbre == 2) {
            mod += dt * 3.5f;
            mod -= std::floor(mod);
            env = std::exp(-t / 0.55f) * std::min(1.f, t / 0.002f);
            v = std::sin(kTwoPi * phase + 2.2f * env * std::sin(kTwoPi * mod));
        } else {
            env = std::min(1.f, t / (timbre == 1 ? 0.01f : 0.03f));
            if (t > lenSec) {
                float x = std::min(1.f, (t - lenSec) / release);
                env *= (1.f - x) * (1.f - x);
            }
            env *= 0.85f + 0.15f * std::exp(-t / 0.4f);
            if (timbre == 1) {
                if ((i & 15u) == 0) lp.setup(2600.f + f, 0.2f, fs);
                float pulse = phase < 0.3f ? 1.f : -1.f;
                v = lp.process(pulse) * 0.7f;
            } else {
                v = std::sin(kTwoPi * phase) * 0.8f + 0.18f * std::sin(kTwoPi * 2.f * phase) + 0.12f * (4.f * std::fabs(phase - 0.5f) - 1.f);
            }
        }
        float out = v * env * gain;
        s.add(start + i, out * gl, out * gr);
    }
}

void kick(Stem& s, int sr, size_t start, float gain) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>(0.35f * fs);
    float phase = 0.f;
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        float f = 44.f + 90.f * std::exp(-t / 0.035f);
        phase += f / fs;
        float env = std::exp(-t / 0.11f) * std::min(1.f, t / 0.0015f);
        float click = i < 60 ? (0.5f - static_cast<float>(i) / 120.f) : 0.f;
        float out = (std::sin(kTwoPi * phase) * env + click * 0.3f) * gain;
        s.add(start + i, out, out);
    }
}

void snare(Stem& s, int sr, size_t start, float gain, Random& rng) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>(0.28f * fs);
    float phase = 0.f;
    StateVariableFilter hp;
    hp.setup(1400.f, 0.2f, fs);
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        phase += (180.f - 40.f * std::min(1.f, t / 0.1f)) / fs;
        float tone = std::sin(kTwoPi * phase) * std::exp(-t / 0.05f) * 0.55f;
        hp.process(rng.range(-1.f, 1.f));
        float noise = hp.highpass() * std::exp(-t / 0.09f) * 0.8f;
        float out = (tone + noise) * gain;
        s.add(start + i, out * 0.9f, out * 1.1f);
    }
}

void hat(Stem& s, int sr, size_t start, float gain, bool open, Random& rng, float pan) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>((open ? 0.25f : 0.06f) * fs);
    StateVariableFilter hp;
    hp.setup(7500.f, 0.1f, fs);
    float gl = panGainL(pan), gr = panGainR(pan);
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        hp.process(rng.range(-1.f, 1.f));
        float out = hp.highpass() * std::exp(-t / (open ? 0.08f : 0.016f)) * gain;
        s.add(start + i, out * gl, out * gr);
    }
}

void tom(Stem& s, int sr, size_t start, float freq, float gain) {
    const float fs = static_cast<float>(sr);
    const size_t total = static_cast<size_t>(0.6f * fs);
    float phase = 0.f;
    for (size_t i = 0; i < total; ++i) {
        float t = static_cast<float>(i) / fs;
        phase += (freq * (1.f + 0.6f * std::exp(-t / 0.05f))) / fs;
        float env = std::exp(-t / 0.22f) * std::min(1.f, t / 0.002f);
        float out = std::sin(kTwoPi * phase) * env * gain;
        s.add(start + i, out, out);
    }
}

// --- arrangement ---------------------------------------------------------------------------------------

/// Voices a chord within [52, 76], choosing the inversion closest to the previous voicing.
std::vector<int> voiceChord(const std::vector<int>& chord, const std::vector<int>& previous) {
    std::vector<int> best;
    float bestCost = 1e9f;
    const size_t n = chord.size();
    std::vector<int> pcs;
    for (int note : chord) pcs.push_back(((note % 12) + 12) % 12);
    for (size_t inv = 0; inv < n; ++inv) {
        std::vector<int> v;
        int floorNote = 52;
        // Start at the inversion's lowest pitch class and stack upward.
        int cursor = floorNote - 1;
        for (size_t k = 0; k < n; ++k) {
            int pc = pcs[(inv + k) % n];
            int note = cursor + 1;
            while (((note % 12) + 12) % 12 != pc) ++note;
            v.push_back(note);
            cursor = note;
        }
        float cost = 0;
        for (size_t k = 0; k < v.size(); ++k) {
            if (!previous.empty()) cost += std::fabs(static_cast<float>(v[k] - previous[std::min(k, previous.size() - 1)]));
        }
        cost += std::max(0, v.back() - 76) * 3.f;
        if (previous.empty()) cost += static_cast<float>(inv);  // prefer root position first
        if (cost < bestCost) {
            bestCost = cost;
            best = v;
        }
    }
    return best;
}

struct MelodyNote {
    int step;       // eighth-note position within the phrase (0..15)
    int length;     // eighths
    int scaleIndex; // diatonic steps relative to the tonic (may be negative)
};

int scaleToMidi(int index, int tonicMidi, const std::vector<int>& scale) {
    int octave = static_cast<int>(std::floor(static_cast<double>(index) / 7.0));
    int degree = ((index % 7) + 7) % 7;
    return tonicMidi + scale[static_cast<size_t>(degree)] + 12 * octave;
}

}  // namespace

Json MusicParams::toJson() const {
    return Json::object({{"mood", mood},
                         {"tempo", tempo},
                         {"key", key},
                         {"progression", progression},
                         {"bars", bars},
                         {"energy", energy},
                         {"pad", pad},
                         {"bass", bass},
                         {"arp", arp},
                         {"melody", melody},
                         {"drums", drums},
                         {"reverb", reverb},
                         {"sampleRate", sampleRate},
                         {"seed", static_cast<int64_t>(seed)}});
}

Result<MusicParams> MusicParams::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_arguments", "music parameters must be an object");
    static const std::vector<std::string> keys{"mood", "tempo", "key", "progression", "bars", "energy", "pad", "bass",
                                               "arp", "melody", "drums", "reverb", "sampleRate", "seed"};
    MusicParams p;
    for (const auto& [k, v] : j.members()) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            std::string guess = str::closest(k, keys, 3);
            return Error::make("invalid_arguments", "unknown music parameter '" + k + "'",
                               guess.empty() ? "valid: mood, tempo, key, progression, bars, energy, pad, bass, arp, melody, "
                                               "drums, reverb, sampleRate, seed"
                                             : "did you mean '" + guess + "'?");
        }
        if (k == "mood") p.mood = v.asString();
        else if (k == "tempo") p.tempo = static_cast<int>(v.asInt());
        else if (k == "key") p.key = v.asString();
        else if (k == "progression") p.progression = v.asString();
        else if (k == "bars") p.bars = static_cast<int>(v.asInt(16));
        else if (k == "energy") p.energy = v.asFloat(-1.f);
        else if (k == "pad") p.pad = v.asBool(true);
        else if (k == "bass") p.bass = v.asBool(true);
        else if (k == "arp") p.arp = v.asBool(true);
        else if (k == "melody") p.melody = v.asBool(true);
        else if (k == "drums") p.drums = v.asBool(true);
        else if (k == "reverb") p.reverb = v.asFloat(0.3f);
        else if (k == "sampleRate") p.sampleRate = static_cast<int>(v.asInt(44100));
        else if (k == "seed") p.seed = static_cast<uint32_t>(v.asInt(1));
    }
    return p;
}

Result<Pcm> renderMusic(const MusicParams& p) {
    const Mood* mood = findMood(p.mood);
    if (!mood) {
        std::string guess = str::closest(p.mood, musicMoods(), 3);
        std::string list;
        for (const auto& m : musicMoods()) list += (list.empty() ? "" : ", ") + m;
        return Error::make("not_found", "unknown mood '" + p.mood + "'", guess.empty() ? "moods: " + list : "did you mean '" + guess + "'? (moods: " + list + ")");
    }
    if (p.bars < 2 || p.bars > 64) return Error::make("invalid_arguments", "bars must be between 2 and 64");
    if (p.sampleRate < 22050 || p.sampleRate > 96000) return Error::make("invalid_arguments", "sampleRate must be between 22050 and 96000");
    const int tempo = p.tempo > 0 ? p.tempo : mood->tempo;
    if (tempo < 30 || tempo > 240) return Error::make("invalid_arguments", "tempo must be between 30 and 240 bpm");
    const float energy = std::clamp(p.energy >= 0.f ? p.energy : mood->energy, 0.f, 1.f);

    Random rng(p.seed * 2246822519u + 101u);
    std::string keyText = p.key.empty() ? std::string(mood->key) + " " + mood->mode : p.key;
    auto parsedKey = parseKey(keyText, mood->mode);
    if (!parsedKey) return parsedKey.error();
    const int tonic = parsedKey->first;
    const std::vector<int> scale = parsedKey->second;

    std::string progression = p.progression;
    if (progression.empty()) progression = mood->progressions[rng.next() % mood->progressions.size()];
    auto chordsOr = parseProgression(progression, tonic, scale, 48);
    if (!chordsOr) return chordsOr.error();
    const std::vector<Chord>& chords = *chordsOr;

    const int sr = p.sampleRate;
    const double beatFrames = sr * 60.0 / tempo;
    const auto barFrames = static_cast<size_t>(std::llround(beatFrames * 4.0));
    const size_t total = barFrames * static_cast<size_t>(p.bars);
    auto at = [&](int bar, double beat) { return static_cast<size_t>(bar) * barFrames + static_cast<size_t>(std::llround(beat * beatFrames)); };
    const float beatSec = 60.f / static_cast<float>(tempo);

    Stem wet(total), dry(total);  // wet: pad/arp/melody (reverb send), dry: bass + drums
    std::string drumStyle = p.drums ? mood->drums : "none";
    std::string bassStyle = p.bass ? mood->bass : "none";
    const bool structured = p.bars >= 8;

    // --- harmony: pad -------------------------------------------------------------------------------
    std::vector<std::vector<int>> voicings;
    {
        std::vector<int> prev;
        for (int bar = 0; bar < p.bars; ++bar) {
            const Chord& c = chords[static_cast<size_t>(bar) % chords.size()];
            if (bar < static_cast<int>(chords.size()) || voicings.empty()) {
                prev = voiceChord(c.notes, prev);
                voicings.push_back(prev);
            } else {
                voicings.push_back(voicings[static_cast<size_t>(bar) % chords.size()]);  // identical repeat
            }
        }
    }
    if (p.pad) {
        for (int bar = 0; bar < p.bars; ++bar) {
            for (int note : voicings[static_cast<size_t>(bar)]) {
                padNote(wet, sr, at(bar, 0), barFrames, static_cast<float>(note), 0.34f, mood->padBrightness);
            }
        }
    }

    // --- bass ---------------------------------------------------------------------------------------
    if (bassStyle != "none") {
        for (int bar = 0; bar < p.bars; ++bar) {
            const Chord& c = chords[static_cast<size_t>(bar) % chords.size()];
            int root = c.notes[0] - 12;
            while (root < 28) root += 12;
            while (root > 43) root -= 12;
            int fifth = root + 7;
            auto note = [&](double beat, double lenBeats, int midi, float g) {
                bassNote(dry, sr, at(bar, beat), static_cast<float>(lenBeats) * beatSec * 0.92f, static_cast<float>(midi), g);
            };
            if (bassStyle == "whole") {
                note(0, 4, root, 0.5f);
            } else if (bassStyle == "pulse") {
                for (int i = 0; i < 8; ++i) note(i * 0.5, 0.45, i % 4 == 3 ? fifth : root, i % 2 ? 0.34f : 0.46f);
            } else if (bassStyle == "walk") {
                note(0, 0.9, root, 0.5f);
                note(1.5, 0.45, root, 0.38f);
                note(2, 0.9, fifth, 0.44f);
                note(3, 0.9, root + 12, 0.36f);
            } else if (bassStyle == "epic") {
                note(0, 2, root, 0.55f);
                note(2, 1.5, root, 0.46f);
                note(3.5, 0.5, fifth, 0.4f);
                note(0, 2, root + 12, 0.2f);
            }
        }
    }

    // --- arpeggio -----------------------------------------------------------------------------------
    if (p.arp && energy > 0.05f) {
        const int perBar = energy > 0.75f ? 16 : (energy > 0.35f ? 8 : 4);
        for (int bar = 0; bar < p.bars; ++bar) {
            if (structured && bar == 0 && energy > 0.4f) continue;  // arp enters after the first bar
            const std::vector<int>& v = voicings[static_cast<size_t>(bar)];
            std::vector<int> pool;
            for (int octave = 0; octave < 2; ++octave) {
                for (int n : v) pool.push_back(n + 12 + 12 * octave);
            }
            const double stepBeats = 4.0 / perBar;
            for (int i = 0; i < perBar; ++i) {
                // Up-and-down contour through the chord tones; sparse rests at low energy.
                size_t span = pool.size() > 1 ? pool.size() : 2;
                size_t cycle = (span - 1) * 2;
                size_t pos = static_cast<size_t>(i) % cycle;
                size_t idx = pos < span ? pos : cycle - pos;
                if (i % 4 != 0 && rng.nextFloat() > 0.55f + 0.45f * energy) continue;
                float g = (i % 4 == 0 ? 0.17f : 0.12f) * (0.6f + 0.4f * energy);
                float pan = (i % 2 ? 0.45f : -0.45f);
                pluckNote(wet, sr, at(bar, i * stepBeats), static_cast<float>(stepBeats * 1.8) * beatSec,
                          static_cast<float>(pool[std::min(idx, pool.size() - 1)]), g, pan, 0.4f + 0.5f * energy);
            }
        }
    }

    // --- melody -------------------------------------------------------------------------------------------
    if (p.melody && energy > 0.05f) {
        const int tonicMidi = 60 + tonic - (tonic > 6 ? 12 : 0);  // C4 area
        // Chord-root degree of each bar in diatonic steps from the tonic.
        std::vector<int> rootDegree;
        for (int bar = 0; bar < p.bars; ++bar) {
            int rootMidi = chords[static_cast<size_t>(bar) % chords.size()].notes[0];
            int rel = ((rootMidi - tonic) % 12 + 12) % 12;
            int best = 0, bestDist = 99;
            for (int d = 0; d < 7; ++d) {
                int dist = std::abs(scale[static_cast<size_t>(d)] - rel);
                if (dist < bestDist) {
                    bestDist = dist;
                    best = d;
                }
            }
            rootDegree.push_back(best);
        }
        // A two-bar motif: rhythm in eighths, contour in diatonic steps.
        std::vector<MelodyNote> motif;
        const int anchor = 7;  // the tonic, one octave above the tonic's C4 register
        {
            int pos = 0, index = anchor;
            while (pos < 16) {
                static const int durations[] = {1, 2, 2, 3, 4, 2};
                int len = durations[rng.next() % 6];
                if (energy > 0.6f && len > 2 && rng.nextFloat() < 0.6f) len = 2;
                if (energy < 0.3f && len < 2) len = 2;
                if (pos + len > 16) len = 16 - pos;
                bool rest = pos > 0 && pos % 8 != 0 && rng.nextFloat() < 0.35f - 0.25f * energy;
                if (!rest) {
                    if (pos % 8 == 0) {
                        // Strong beats land on chord tones of the first chord (root, third, fifth).
                        static const int tones[] = {0, 2, 4, 0};
                        index = anchor + tones[rng.next() % 4];
                    } else {
                        static const int steps[] = {-2, -1, -1, 0, 1, 1, 2};
                        index = std::clamp(index + steps[rng.next() % 7], anchor - 4, anchor + 6);
                    }
                    motif.push_back({pos, len, index});
                }
                pos += len;
            }
            // Resolve the phrase end onto the root or the fifth.
            if (!motif.empty()) motif.back().scaleIndex = anchor + (rng.next() % 2 ? 0 : 4);
        }
        const float lenEighth = beatSec * 0.5f;
        for (int pair = 0; pair * 2 < p.bars; ++pair) {
            int bar0 = pair * 2;
            if (structured && bar0 == 0 && energy < 0.85f) continue;  // lead the loop in with harmony only
            if (structured && (pair % 4) == 3 && energy < 0.5f) continue;  // breathing room
            int shift = rootDegree[static_cast<size_t>(bar0)] - rootDegree[0];
            bool invert = (pair % 2) == 1 && rng.nextFloat() < 0.3f;
            for (const auto& n : motif) {
                if (bar0 * 8 + n.step >= p.bars * 8) continue;
                int idx = n.scaleIndex + shift;
                if (invert && n.step >= 8) idx = 2 * anchor - n.scaleIndex + shift;  // answer phrase turns upside down
                int midi = scaleToMidi(idx, tonicMidi, scale);
                while (midi > 88) midi -= 12;
                while (midi < 62) midi += 12;
                double beat = n.step * 0.5;
                float gain = 0.22f * (n.step % 8 == 0 ? 1.f : 0.85f);
                leadNote(wet, sr, at(bar0, beat), n.length * lenEighth * 0.95f, static_cast<float>(midi), gain,
                         ((pair % 2) ? 0.18f : -0.18f), mood->timbre);
            }
        }
    }

    // --- drums -------------------------------------------------------------------------------------------
    if (drumStyle != "none") {
        for (int bar = 0; bar < p.bars; ++bar) {
            if (structured && bar == 0 && drumStyle != "pulse") continue;  // drums enter on bar 2
            const bool lastBar = bar == p.bars - 1;
            if (drumStyle == "soft") {
                for (int i = 0; i < 8; ++i) hat(dry, sr, at(bar, i * 0.5), i % 2 ? 0.05f : 0.08f, false, rng, i % 2 ? 0.3f : -0.3f);
                snare(dry, sr, at(bar, 1), 0.1f, rng);
                snare(dry, sr, at(bar, 3), 0.1f, rng);
                kick(dry, sr, at(bar, 0), 0.28f);
            } else if (drumStyle == "four") {
                for (int b = 0; b < 4; ++b) kick(dry, sr, at(bar, b), 0.5f);
                snare(dry, sr, at(bar, 1), 0.34f, rng);
                snare(dry, sr, at(bar, 3), 0.34f, rng);
                for (int i = 0; i < 8; ++i) hat(dry, sr, at(bar, i * 0.5), i % 2 ? 0.12f : 0.06f, i == 7, rng, 0.25f);
            } else if (drumStyle == "half") {
                kick(dry, sr, at(bar, 0), 0.6f);
                kick(dry, sr, at(bar, 2.5), 0.45f);
                snare(dry, sr, at(bar, 2), 0.5f, rng);
                for (int i = 0; i < 4; ++i) hat(dry, sr, at(bar, i + 0.5), 0.07f, false, rng, -0.2f);
                if (bar % 2 == 1) {
                    tom(dry, sr, at(bar, 3), 110.f, 0.35f);
                    tom(dry, sr, at(bar, 3.5), 82.f, 0.4f);
                }
                tom(dry, sr, at(bar, 0), 58.f, 0.3f);
            } else if (drumStyle == "drive") {
                for (int i = 0; i < 8; ++i) {
                    if (i % 2 == 0 || i == 7) kick(dry, sr, at(bar, i * 0.5), 0.5f);
                }
                snare(dry, sr, at(bar, 1), 0.42f, rng);
                snare(dry, sr, at(bar, 3), 0.42f, rng);
                for (int i = 0; i < 16; ++i) hat(dry, sr, at(bar, i * 0.25), i % 2 ? 0.07f : 0.11f, false, rng, i % 4 < 2 ? -0.3f : 0.3f);
            } else if (drumStyle == "pulse") {
                kick(dry, sr, at(bar, 0), 0.5f);
                kick(dry, sr, at(bar, 0.5), 0.3f);
                kick(dry, sr, at(bar, 2.5), 0.42f);
                for (int i = 0; i < 4; ++i) hat(dry, sr, at(bar, i + 0.5), 0.05f, false, rng, 0.3f);
                snare(dry, sr, at(bar, 3), 0.15f, rng);
            }
            if (lastBar && energy > 0.5f && drumStyle != "soft") {
                for (int i = 0; i < 4; ++i) snare(dry, sr, at(bar, 3.0 + i * 0.25), 0.12f + 0.08f * static_cast<float>(i), rng);
            }
        }
    }

    // --- mix: reverb on the wet stem (two passes so the tail wraps over the loop point) --------------------------
    const float rv = std::clamp(p.reverb * 0.5f + mood->reverb * 0.5f, 0.f, 1.f);
    Pcm out;
    out.sampleRate = sr;
    out.channels = 2;
    out.samples.resize(total * 2);
    {
        Reverb reverb(sr, 0.55f + 0.4f * rv, 0.45f, true);
        std::vector<float> rl(total), rr(total);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < total; ++i) {
                float l, r;
                reverb.process(wet.l[i], wet.r[i], l, r);
                if (pass == 1) {
                    rl[i] = l;
                    rr[i] = r;
                }
            }
        }
        const float wetDry = 1.f - 0.3f * rv, send = 1.6f * rv;
        for (size_t i = 0; i < total; ++i) {
            out.samples[2 * i] = dry.l[i] + wet.l[i] * wetDry + rl[i] * send;
            out.samples[2 * i + 1] = dry.r[i] + wet.r[i] * wetDry + rr[i] * send;
        }
    }
    // Gentle saturation keeps peaks musical, then level to a consistent loudness.
    for (float& s : out.samples) s = std::tanh(s * 0.9f);
    normalize(out, 0.85f);
    return out;
}

}  // namespace sky::audio
