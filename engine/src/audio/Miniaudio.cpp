// The single translation unit that compiles miniaudio (public domain / MIT-0), plus the file
// decoding built on its decoder (mp3, flac, and wav variants our own reader does not handle).

#define MINIAUDIO_IMPLEMENTATION
#include "MiniaudioConfig.h"

#include <algorithm>

#include "skywalker/audio/Wav.h"
#include "skywalker/core/Strings.h"

namespace sky::audio {

Result<Pcm> readAudioFile(const std::string& path) {
    const std::string lower = str::lower(path);
    const bool isWav = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".wav") == 0;
    if (isWav) {
        auto wav = readWav(path);
        if (wav || wav.error().code == "not_found") return wav;
        // Exotic WAV (ADPCM, ...): let miniaudio try.
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);  // native channels and rate
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        return Error::make("invalid_audio", "cannot decode " + path, "supported: wav, mp3, flac");
    }
    Pcm pcm;
    pcm.sampleRate = static_cast<int>(decoder.outputSampleRate);
    pcm.channels = static_cast<int>(decoder.outputChannels);
    const size_t channels = static_cast<size_t>(std::max(1, pcm.channels));
    // Ten minutes is plenty for anything we analyze in memory; longer files are streamed at play time.
    const ma_uint64 maxFrames = static_cast<ma_uint64>(pcm.sampleRate) * 600;
    std::vector<float> chunk(4096 * channels);
    ma_uint64 total = 0;
    for (;;) {
        ma_uint64 read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read);
        if (read > 0) pcm.samples.insert(pcm.samples.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read * channels));
        total += read;
        if (r != MA_SUCCESS || read == 0 || total >= maxFrames) break;
    }
    ma_decoder_uninit(&decoder);
    if (pcm.samples.empty()) return Error::make("invalid_audio", path + " contains no audio");
    return pcm;
}

}  // namespace sky::audio
