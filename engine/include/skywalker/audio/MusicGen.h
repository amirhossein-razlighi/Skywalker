#pragma once
// Procedural music: a chord progression rendered with a pad, bass, arpeggio, a motif-based
// melody and drums, mixed in stereo with reverb. Deterministic per seed, and the result
// loops seamlessly (note tails and reverb wrap around the loop point).

#include <string>
#include <vector>

#include "skywalker/audio/Wav.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::audio {

struct MusicParams {
    std::string mood = "calm";   // see musicMoods()
    int tempo = 0;               // beats per minute (0 = the mood's tempo)
    std::string key;             // "A minor", "C major", "F# dorian" (empty = mood default)
    std::string progression;     // roman numerals, one chord per bar: "i VI III VII" (empty = by mood + seed)
    int bars = 16;               // length of the loop in bars (4/4), 2..64
    float energy = -1.f;         // 0..1: busier arps, melody and drums (-1 = the mood's energy)
    bool pad = true;
    bool bass = true;
    bool arp = true;
    bool melody = true;
    bool drums = true;
    float reverb = 0.3f;         // 0..1
    int sampleRate = 44100;
    uint32_t seed = 1;

    Json toJson() const;
    static Result<MusicParams> fromJson(const Json& j);
};

const std::vector<std::string>& musicMoods();
std::string musicMoodDescription(const std::string& mood);

/// Stereo float PCM, exactly `bars` bars long, peak-limited. Deterministic.
Result<Pcm> renderMusic(const MusicParams& params);

// Music theory helpers (exposed for tests).
struct Chord {
    std::vector<int> notes;  // MIDI notes, root first
    std::string name;        // e.g. "Am", "F", "G7"
};
/// Parses a key like "A minor" into the tonic pitch class (0..11) and scale intervals.
Result<std::pair<int, std::vector<int>>> parseKey(const std::string& key, const std::string& defaultMode);
/// Parses "i VI III VII" in the given key into one chord per entry.
Result<std::vector<Chord>> parseProgression(const std::string& progression, int tonicPitchClass,
                                            const std::vector<int>& scale, int baseOctaveMidi = 48);

}  // namespace sky::audio
