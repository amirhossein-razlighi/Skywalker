#pragma once
// PCM buffers, WAV (RIFF) reading/writing and signal analysis.
//
// Everything the procedural audio tools generate is a `Pcm` (interleaved float samples in
// -1..1). WAV files are written as 16-bit PCM, the universal format every engine, DAW and
// browser reads. The reader accepts 8/16/24/32-bit integer and 32-bit float WAVs; other
// formats (mp3, flac) are decoded through miniaudio by `readAudioFile`.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"

namespace sky::audio {

struct Pcm {
    int sampleRate = 44100;
    int channels = 1;
    std::vector<float> samples;  // interleaved

    size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
    double duration() const { return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
};

/// 16-bit PCM WAV bytes. Samples are clamped; no dither (output is bit-exact deterministic).
std::vector<uint8_t> encodeWav(const Pcm& pcm);
Status writeWav(const std::string& path, const Pcm& pcm);
Result<Pcm> decodeWav(const uint8_t* data, size_t size);
Result<Pcm> readWav(const std::string& path);
/// Reads wav directly, other containers (mp3, flac, ...) through miniaudio. Decodes at the
/// file's native rate and channel count.
Result<Pcm> readAudioFile(const std::string& path);

struct Stats {
    double duration = 0;      // seconds
    int sampleRate = 0;
    int channels = 0;
    double peak = 0;          // linear, 0..1+
    double peakDb = -120;     // dBFS
    double rmsDb = -120;      // dBFS over the whole file
    double loudnessLufs = -120;  // ITU-R BS.1770 integrated loudness (gated): perceived level
    double dcOffset = 0;      // mean sample value; large values waste headroom
    size_t clipped = 0;       // samples at or beyond full scale
    double silenceRatio = 0;  // fraction of 10 ms windows below -60 dBFS
    double startLevelDb = -120;  // level of the first/last 5 ms: louder than about -50 dB clicks when played
    double endLevelDb = -120;
    Json toJson() const;
};
Stats analyze(const Pcm& pcm);

/// Peak-normalizes to `peak` (linear). Returns the applied gain.
float normalize(Pcm& pcm, float peak = 0.9f);
/// Short fade at both ends to avoid clicks.
void fadeEdges(Pcm& pcm, float fadeInSeconds, float fadeOutSeconds);
/// Mono mixdown / mono to stereo.
Pcm toMono(const Pcm& pcm);
Pcm toStereo(const Pcm& pcm);
/// Turns a continuous signal into a seamless loop of exactly `frames` frames: the signal
/// that follows the loop point is crossfaded (equal power) into the start. The input needs
/// at least `frames` frames, and `frames + overlap` for the crossfade to have material.
Pcm makeSeamlessLoop(const Pcm& continuous, size_t frames);

}  // namespace sky::audio
