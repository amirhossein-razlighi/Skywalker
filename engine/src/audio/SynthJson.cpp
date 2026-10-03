// JSON (de)serialization and the self-describing schema of procedural sounds.

#include <algorithm>
#include <string>

#include "skywalker/audio/Synth.h"
#include "skywalker/core/Strings.h"

namespace sky::audio {

namespace {

struct FloatField {
    const char* name;
    float SfxLayer::*ptr;
    float lo, hi;
    const char* doc;
};

const std::vector<FloatField>& layerFloats() {
    static const std::vector<FloatField> fields{
        {"freq", &SfxLayer::freq, 20.f, 20000.f, "Pitch in Hz at the start (grains: center frequency)"},
        {"slide", &SfxLayer::slide, -12.f, 12.f, "Pitch slide in octaves per second (negative falls: lasers, explosions)"},
        {"slideAccel", &SfxLayer::slideAccel, -40.f, 40.f, "Change of the slide per second"},
        {"minFreq", &SfxLayer::minFreq, 0.f, 20000.f, "The layer ends when the pitch falls below this (0 = never)"},
        {"arpRate", &SfxLayer::arpRate, 0.5f, 60.f, "Arpeggio steps per second"},
        {"vibratoDepth", &SfxLayer::vibratoDepth, 0.f, 12.f, "Vibrato depth in semitones"},
        {"vibratoRate", &SfxLayer::vibratoRate, 0.1f, 40.f, "Vibrato speed in Hz"},
        {"duty", &SfxLayer::duty, 0.05f, 0.95f, "Square pulse width"},
        {"dutySweep", &SfxLayer::dutySweep, -5.f, 5.f, "Pulse width change per second"},
        {"fmRatio", &SfxLayer::fmRatio, 0.1f, 32.f, "FM modulator frequency / carrier frequency"},
        {"fmDepth", &SfxLayer::fmDepth, 0.f, 40.f, "FM modulation index (bright metallic when high)"},
        {"attack", &SfxLayer::attack, 0.f, 10.f, "Envelope attack seconds"},
        {"decay", &SfxLayer::decay, 0.f, 20.f, "Envelope decay seconds (to the sustain level)"},
        {"sustain", &SfxLayer::sustain, 0.f, 1.f, "Envelope sustain level (0 = percussive)"},
        {"hold", &SfxLayer::hold, 0.f, 20.f, "Seconds spent at the sustain level"},
        {"release", &SfxLayer::release, 0.f, 20.f, "Envelope release seconds"},
        {"punch", &SfxLayer::punch, 0.f, 4.f, "Extra gain right after the attack (kicks, hits)"},
        {"gain", &SfxLayer::gain, 0.f, 8.f, "Layer volume when mixing layers"},
        {"tremolo", &SfxLayer::tremolo, 0.f, 1.f, "Amplitude modulation depth"},
        {"tremoloRate", &SfxLayer::tremoloRate, 0.05f, 60.f, "Amplitude modulation speed in Hz"},
        {"ampWander", &SfxLayer::ampWander, 0.f, 1.f, "Slow random loudness drift (wind gusts, surf)"},
        {"filterWander", &SfxLayer::filterWander, 0.f, 4.f, "Slow random cutoff drift in octaves"},
        {"wanderRate", &SfxLayer::wanderRate, 0.01f, 10.f, "Speed of the random drift in Hz"},
        {"lowpass", &SfxLayer::lowpass, 0.f, 20000.f, "Low-pass cutoff in Hz (0 = off)"},
        {"lowpassEnd", &SfxLayer::lowpassEnd, 0.f, 20000.f, "Low-pass cutoff at the end of the layer (sweep)"},
        {"highpass", &SfxLayer::highpass, 0.f, 20000.f, "High-pass cutoff in Hz (0 = off)"},
        {"highpassEnd", &SfxLayer::highpassEnd, 0.f, 20000.f, "High-pass cutoff at the end of the layer (sweep)"},
        {"resonance", &SfxLayer::resonance, 0.f, 1.f, "Filter resonance"},
        {"drive", &SfxLayer::drive, 0.f, 10.f, "Soft-clipping distortion"},
        {"bitcrush", &SfxLayer::bitcrush, 0.f, 16.f, "Bit depth reduction (4 = gritty retro, 0 = off)"},
        {"downsample", &SfxLayer::downsample, 1.f, 64.f, "Sample-rate reduction factor"},
        {"grainRate", &SfxLayer::grainRate, 0.1f, 2000.f, "Grains: bursts per second"},
        {"grainDecay", &SfxLayer::grainDecay, 0.0005f, 1.f, "Grains: burst length in seconds"},
        {"grainSpread", &SfxLayer::grainSpread, 0.f, 1.f, "Grains: random pitch/loudness spread"},
        {"delay", &SfxLayer::delay, 0.f, 20.f, "Start offset in seconds (multi-part sounds)"},
    };
    return fields;
}

const std::vector<std::string>& waves() {
    static const std::vector<std::string> w{"sine", "square", "saw", "triangle", "noise", "pink", "brown", "fm", "grains"};
    return w;
}

const std::vector<std::string>& globalKeys() {
    static const std::vector<std::string> k{"layers", "echo", "reverb", "volume", "normalize", "sampleRate",
                                            "duration", "loopSeconds", "seed"};
    return k;
}

std::vector<std::string> layerKeys() {
    std::vector<std::string> keys{"wave", "arp", "arpLoop"};
    for (const auto& f : layerFloats()) keys.push_back(f.name);
    return keys;
}

Error unknownKey(const std::string& where, const std::string& key, const std::vector<std::string>& valid) {
    std::string guess = str::closest(key, valid, 3);
    std::string list;
    for (size_t i = 0; i < valid.size(); ++i) list += (i ? ", " : "") + valid[i];
    return Error::make("invalid_arguments", "unknown " + where + " parameter '" + key + "'",
                       guess.empty() ? "valid: " + list : "did you mean '" + guess + "'?");
}

Status readNumber(const Json& v, const std::string& where, float lo, float hi, float& out) {
    if (!v.isNumber()) return Error::make("invalid_arguments", where + " must be a number");
    float x = v.asFloat();
    if (x < lo || x > hi) {
        return Error::make("invalid_arguments", where + " = " + std::to_string(x) + " is out of range", "allowed: " +
                                                                                                               std::to_string(lo) + " .. " + std::to_string(hi));
    }
    out = x;
    return {};
}

Result<SfxLayer> layerFromJson(const Json& j, const std::string& where) {
    if (!j.isObject()) return Error::make("invalid_arguments", where + " must be an object");
    SfxLayer l;
    const auto keys = layerKeys();
    for (const auto& [key, value] : j.members()) {
        if (key == "wave") {
            std::string w = value.asString();
            if (std::find(waves().begin(), waves().end(), w) == waves().end()) {
                std::string guess = str::closest(w, waves(), 3);
                return Error::make("invalid_arguments", where + ".wave '" + w + "' is not a waveform",
                                   guess.empty() ? "use sine, square, saw, triangle, noise, pink, brown, fm or grains"
                                                 : "did you mean '" + guess + "'?");
            }
            l.wave = w;
        } else if (key == "arp") {
            if (!value.isArray()) return Error::make("invalid_arguments", where + ".arp must be an array of semitone offsets");
            l.arp.clear();
            for (const auto& e : value.elements()) {
                if (!e.isNumber()) return Error::make("invalid_arguments", where + ".arp must contain only numbers");
                l.arp.push_back(std::clamp(e.asFloat(), -48.f, 48.f));
            }
            if (l.arp.size() > 64) return Error::make("invalid_arguments", where + ".arp has at most 64 steps");
        } else if (key == "arpLoop") {
            l.arpLoop = value.asBool();
        } else {
            const FloatField* field = nullptr;
            for (const auto& f : layerFloats()) {
                if (key == f.name) field = &f;
            }
            if (!field) return unknownKey(where, key, keys);
            if (Status s = readNumber(value, where + "." + key, field->lo, field->hi, l.*(field->ptr)); !s) return s.error();
        }
    }
    return l;
}

Json layerToJson(const SfxLayer& l) {
    Json j = Json::object({{"wave", l.wave}});
    const SfxLayer defaults;
    for (const auto& f : layerFloats()) {
        // freq is always written; everything else only when it differs from the default.
        if (std::string(f.name) == "freq" || l.*(f.ptr) != defaults.*(f.ptr)) j[f.name] = l.*(f.ptr);
    }
    if (!l.arp.empty()) {
        Json a = Json::array();
        for (float s : l.arp) a.push(s);
        j["arp"] = a;
        if (l.arpLoop) j["arpLoop"] = true;
    }
    return j;
}

}  // namespace

Json SfxParams::toJson() const {
    Json layersJson = Json::array();
    for (const auto& l : layers) layersJson.push(layerToJson(l));
    Json j = Json::object({{"layers", layersJson}});
    if (echo.mix > 0.f) {
        j["echo"] = Json::object({{"time", echo.time}, {"feedback", echo.feedback}, {"mix", echo.mix}});
    }
    if (reverb.mix > 0.f) {
        j["reverb"] = Json::object({{"mix", reverb.mix}, {"room", reverb.room}, {"damping", reverb.damping}});
    }
    if (volume != 1.f) j["volume"] = volume;
    j["normalize"] = normalize;
    j["sampleRate"] = sampleRate;
    if (duration > 0.f) j["duration"] = duration;
    if (loopSeconds > 0.f) j["loopSeconds"] = loopSeconds;
    j["seed"] = static_cast<int64_t>(seed);
    return j;
}

Result<SfxParams> SfxParams::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_arguments", "sound parameters must be an object");
    SfxParams p;
    Json layerFields = Json::object();
    const auto& globals = globalKeys();
    const auto lkeys = layerKeys();
    for (const auto& [key, value] : j.members()) {
        const bool isGlobal = std::find(globals.begin(), globals.end(), key) != globals.end();
        const bool isLayer = std::find(lkeys.begin(), lkeys.end(), key) != lkeys.end();
        if (!isGlobal && !isLayer) {
            std::vector<std::string> all = globals;
            all.insert(all.end(), lkeys.begin(), lkeys.end());
            return unknownKey("sound", key, all);
        }
        if (isLayer && !isGlobal) layerFields[key] = value;
    }
    if (const Json* layers = j.find("layers")) {
        if (!layers->isArray() || layers->size() == 0) {
            return Error::make("invalid_arguments", "layers must be a non-empty array of layer objects");
        }
        for (size_t i = 0; i < layers->size(); ++i) {
            auto l = layerFromJson((*layers)[i], "layers[" + std::to_string(i) + "]");
            if (!l) return l.error();
            p.layers.push_back(std::move(*l));
        }
        if (layerFields.size()) {
            return Error::make("invalid_arguments", "put layer fields inside `layers`, not next to it",
                               "or omit `layers` to describe a single-layer sound");
        }
    } else {
        auto l = layerFromJson(layerFields, "sound");
        if (!l) return l.error();
        p.layers.push_back(std::move(*l));
    }
    if (const Json* e = j.find("echo")) {
        if (!e->isObject()) return Error::make("invalid_arguments", "echo must be {time, feedback, mix}");
        for (const auto& [k, v] : e->members()) {
            Status s;
            if (k == "time") s = readNumber(v, "echo.time", 0.f, 2.f, p.echo.time);
            else if (k == "feedback") s = readNumber(v, "echo.feedback", 0.f, 0.95f, p.echo.feedback);
            else if (k == "mix") s = readNumber(v, "echo.mix", 0.f, 1.f, p.echo.mix);
            else return unknownKey("echo", k, {"time", "feedback", "mix"});
            if (!s) return s.error();
        }
    }
    if (const Json* r = j.find("reverb")) {
        if (!r->isObject()) return Error::make("invalid_arguments", "reverb must be {mix, room, damping}");
        for (const auto& [k, v] : r->members()) {
            Status s;
            if (k == "mix") s = readNumber(v, "reverb.mix", 0.f, 1.f, p.reverb.mix);
            else if (k == "room") s = readNumber(v, "reverb.room", 0.f, 1.f, p.reverb.room);
            else if (k == "damping") s = readNumber(v, "reverb.damping", 0.f, 1.f, p.reverb.damping);
            else return unknownKey("reverb", k, {"mix", "room", "damping"});
            if (!s) return s.error();
        }
    }
    if (const Json* v = j.find("volume")) {
        if (Status s = readNumber(*v, "volume", 0.f, 4.f, p.volume); !s) return s.error();
    }
    if (const Json* v = j.find("normalize")) {
        if (Status s = readNumber(*v, "normalize", 0.f, 1.f, p.normalize); !s) return s.error();
    }
    if (const Json* v = j.find("duration")) {
        if (Status s = readNumber(*v, "duration", 0.f, 40.f, p.duration); !s) return s.error();
    }
    if (const Json* v = j.find("loopSeconds")) {
        if (Status s = readNumber(*v, "loopSeconds", 0.f, 40.f, p.loopSeconds); !s) return s.error();
    }
    if (const Json* v = j.find("sampleRate")) {
        float sr = 0;
        if (Status s = readNumber(*v, "sampleRate", 8000.f, 96000.f, sr); !s) return s.error();
        p.sampleRate = static_cast<int>(sr);
    }
    if (const Json* v = j.find("seed")) p.seed = static_cast<uint32_t>(v->asInt(1));
    return p;
}

Json sfxParameterDocs() {
    Json fields = Json::object();
    fields["wave"] = "sine | square | saw | triangle | noise | pink | brown | fm | grains";
    fields["arp"] = "array of semitone offsets played in order, e.g. [0, 12] (two-note coin)";
    fields["arpLoop"] = "cycle the arp instead of holding the last note";
    for (const auto& f : layerFloats()) {
        fields[f.name] = std::string(f.doc) + " [" + std::to_string(f.lo).substr(0, std::to_string(f.lo).find('.') + 3) +
                         " .. " + std::to_string(f.hi).substr(0, std::to_string(f.hi).find('.') + 3) + "]";
    }
    return fields;
}

}  // namespace sky::audio
