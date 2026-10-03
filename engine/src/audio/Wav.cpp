#include "skywalker/audio/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "skywalker/core/Strings.h"

namespace sky::audio {

namespace {

constexpr double kPiD = 3.14159265358979323846;

void put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v & 0xff));
    o.push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

double toDb(double linear) { return linear > 1e-6 ? 20.0 * std::log10(linear) : -120.0; }

}  // namespace

std::vector<uint8_t> encodeWav(const Pcm& pcm) {
    const auto dataBytes = static_cast<uint32_t>(pcm.samples.size() * 2);
    std::vector<uint8_t> o;
    o.reserve(44 + dataBytes);
    auto tag = [&](const char* t) { o.insert(o.end(), t, t + 4); };
    tag("RIFF");
    put32(o, 36 + dataBytes);
    tag("WAVE");
    tag("fmt ");
    put32(o, 16);
    put16(o, 1);  // PCM
    put16(o, static_cast<uint16_t>(pcm.channels));
    put32(o, static_cast<uint32_t>(pcm.sampleRate));
    put32(o, static_cast<uint32_t>(pcm.sampleRate * pcm.channels * 2));
    put16(o, static_cast<uint16_t>(pcm.channels * 2));
    put16(o, 16);
    tag("data");
    put32(o, dataBytes);
    for (float f : pcm.samples) {
        float c = std::isfinite(f) ? std::clamp(f, -1.f, 1.f) : 0.f;
        auto v = static_cast<int16_t>(std::lrint(c * 32767.f));
        put16(o, static_cast<uint16_t>(v));
    }
    return o;
}

Status writeWav(const std::string& path, const Pcm& pcm) {
    if (pcm.channels < 1 || pcm.channels > 8 || pcm.sampleRate < 1000) {
        return Error::make("invalid_audio", "cannot write audio with " + std::to_string(pcm.channels) + " channels at " +
                                                std::to_string(pcm.sampleRate) + " Hz");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::vector<uint8_t> bytes = encodeWav(pcm);
    // Write to a temp file and rename so a running audio engine never reads a half-written clip.
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return Error::make("io_error", "cannot write " + path);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) return Error::make("io_error", "cannot write " + path);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return Error::make("io_error", "cannot write " + path + ": " + ec.message());
    return {};
}

Result<Pcm> decodeWav(const uint8_t* data, size_t size) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return Error::make("invalid_audio", "not a RIFF/WAVE file");
    }
    int format = 0, channels = 0, rate = 0, bits = 0;
    const uint8_t* pcmData = nullptr;
    size_t pcmSize = 0;
    size_t pos = 12;
    while (pos + 8 <= size) {
        uint32_t chunk = get32(data + pos + 4);
        const uint8_t* body = data + pos + 8;
        size_t avail = std::min<size_t>(chunk, size - pos - 8);
        if (std::memcmp(data + pos, "fmt ", 4) == 0 && avail >= 16) {
            format = get16(body);
            channels = get16(body + 2);
            rate = static_cast<int>(get32(body + 4));
            bits = get16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = get16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(data + pos, "data", 4) == 0) {
            pcmData = body;
            pcmSize = avail;
        }
        pos += 8 + static_cast<size_t>(chunk) + (chunk & 1u);
    }
    if (!pcmData || channels < 1 || channels > 8 || rate < 1000) return Error::make("invalid_audio", "malformed WAV header");
    const bool isFloat = format == 3 && bits == 32;
    if (!(format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) && !isFloat) {
        return Error::make("unsupported_audio",
                           "WAV format " + std::to_string(format) + " with " + std::to_string(bits) + " bits is not supported",
                           "use 16-bit PCM or 32-bit float WAV, or mp3/flac");
    }
    Pcm pcm;
    pcm.sampleRate = rate;
    pcm.channels = channels;
    const size_t bytesPer = static_cast<size_t>(bits / 8);
    const size_t count = pcmSize / bytesPer / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    pcm.samples.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = pcmData + i * bytesPer;
        float v = 0.f;
        if (isFloat) {
            uint32_t u = get32(p);
            std::memcpy(&v, &u, 4);
        } else if (bits == 8) {
            v = (static_cast<int>(p[0]) - 128) / 128.f;
        } else if (bits == 16) {
            v = static_cast<int16_t>(get16(p)) / 32768.f;
        } else if (bits == 24) {
            auto s = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) | (static_cast<uint32_t>(p[1]) << 16) |
                                          (static_cast<uint32_t>(p[2]) << 24));
            v = static_cast<float>(s / 2147483648.0);
        } else {
            v = static_cast<float>(static_cast<int32_t>(get32(p)) / 2147483648.0);
        }
        pcm.samples[i] = v;
    }
    return pcm;
}

Result<Pcm> readWav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decodeWav(bytes.data(), bytes.size());
}

Json Stats::toJson() const {
    auto r = [](double v, double scale = 100.0) { return std::round(v * scale) / scale; };
    return Json::object({{"duration", r(duration, 1000.0)},
                         {"sampleRate", sampleRate},
                         {"channels", channels},
                         {"peak", r(peak, 1000.0)},
                         {"peakDb", r(peakDb)},
                         {"rmsDb", r(rmsDb)},
                         {"loudnessLufs", r(loudnessLufs)},
                         {"dcOffset", r(dcOffset, 10000.0)},
                         {"clippedSamples", clipped},
                         {"silenceRatio", r(silenceRatio, 1000.0)},
                         {"startLevelDb", r(startLevelDb)},
                         {"endLevelDb", r(endLevelDb)},
                         {"firstSample", r(firstSample, 10000.0)},
                         {"lastSample", r(lastSample, 10000.0)}});
}

namespace {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// BS.1770 K-weighting (high shelf + high-pass), coefficients derived for any sample rate.
void kWeighting(double fs, Biquad& shelf, Biquad& hp) {
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(kPiD * f0 / fs);
        double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        shelf.b1 = 2.0 * (K * K - Vh) / a0;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        shelf.a1 = 2.0 * (K * K - 1.0) / a0;
        shelf.a2 = (1.0 - K / Q + K * K) / a0;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        double K = std::tan(kPiD * f0 / fs);
        double a0 = 1.0 + K / Q + K * K;
        hp.b0 = 1.0;
        hp.b1 = -2.0;
        hp.b2 = 1.0;
        hp.a1 = 2.0 * (K * K - 1.0) / a0;
        hp.a2 = (1.0 - K / Q + K * K) / a0;
    }
}

double integratedLoudness(const Pcm& pcm) {
    const size_t frames = pcm.frames();
    const double fs = pcm.sampleRate;
    const auto block = static_cast<size_t>(0.4 * fs), hop = static_cast<size_t>(0.1 * fs);
    if (frames == 0) return -120.0;
    // K-weighted power summed over channels per 100 ms hop; a 400 ms block is four hops (75% overlap).
    const size_t hops = hop > 0 ? frames / hop : 0;
    std::vector<double> hopPower(hops, 0.0);
    double totalPower = 0;
    for (int c = 0; c < pcm.channels; ++c) {
        Biquad shelf, hp;
        kWeighting(fs, shelf, hp);
        for (size_t i = 0; i < frames; ++i) {
            double x = pcm.samples[i * static_cast<size_t>(pcm.channels) + static_cast<size_t>(c)];
            double y = hp.process(shelf.process(x));
            totalPower += y * y;
            if (hop > 0 && i / hop < hops) hopPower[i / hop] += y * y;
        }
    }
    std::vector<double> blocks;
    if (frames < block || hops < 4) {
        blocks.push_back(totalPower / static_cast<double>(frames));  // shorter than one block: whole file
    } else {
        const double blockFrames = static_cast<double>(hop * 4);
        for (size_t h = 0; h + 4 <= hops; ++h) {
            blocks.push_back((hopPower[h] + hopPower[h + 1] + hopPower[h + 2] + hopPower[h + 3]) / blockFrames);
        }
    }
    auto lufs = [](double z) { return -0.691 + 10.0 * std::log10(std::max(z, 1e-12)); };
    double sum = 0;
    size_t n = 0;
    for (double z : blocks) {
        if (lufs(z) > -70.0) {
            sum += z;
            ++n;
        }
    }
    if (n == 0) return -120.0;
    double relGate = lufs(sum / static_cast<double>(n)) - 10.0;
    sum = 0;
    n = 0;
    for (double z : blocks) {
        if (lufs(z) > relGate) {
            sum += z;
            ++n;
        }
    }
    return n ? lufs(sum / static_cast<double>(n)) : -120.0;
}

}  // namespace

Stats analyze(const Pcm& pcm) {
    Stats s;
    s.sampleRate = pcm.sampleRate;
    s.channels = pcm.channels;
    s.duration = pcm.duration();
    if (pcm.samples.empty()) return s;
    double sumSq = 0, sum = 0, peak = 0;
    for (float v : pcm.samples) {
        double a = std::fabs(static_cast<double>(v));
        peak = std::max(peak, a);
        sumSq += static_cast<double>(v) * v;
        sum += v;
        if (a >= 0.9999) ++s.clipped;
    }
    const auto n = static_cast<double>(pcm.samples.size());
    s.peak = peak;
    s.peakDb = toDb(peak);
    s.rmsDb = toDb(std::sqrt(sumSq / n));
    s.dcOffset = sum / n;
    s.loudnessLufs = integratedLoudness(pcm);

    const size_t frames = pcm.frames();
    const auto ch = static_cast<size_t>(pcm.channels);
    auto windowRms = [&](size_t from, size_t count) {
        count = std::min(count, frames - std::min(from, frames));
        if (count == 0) return 0.0;
        double acc = 0;
        for (size_t i = from; i < from + count; ++i) {
            for (size_t c = 0; c < ch; ++c) {
                double v = pcm.samples[i * ch + c];
                acc += v * v;
            }
        }
        return std::sqrt(acc / static_cast<double>(count * ch));
    };
    const size_t win = std::max<size_t>(1, static_cast<size_t>(pcm.sampleRate * 0.01));
    size_t quiet = 0, windows = 0;
    for (size_t i = 0; i < frames; i += win) {
        ++windows;
        if (toDb(windowRms(i, win)) < -60.0) ++quiet;
    }
    s.silenceRatio = windows ? static_cast<double>(quiet) / static_cast<double>(windows) : 0.0;
    const size_t edge = std::max<size_t>(1, static_cast<size_t>(pcm.sampleRate * 0.005));
    s.startLevelDb = toDb(windowRms(0, edge));
    s.endLevelDb = toDb(windowRms(frames > edge ? frames - edge : 0, edge));
    for (size_t c = 0; c < ch; ++c) {
        s.firstSample = std::max(s.firstSample, std::fabs(static_cast<double>(pcm.samples[c])));
        s.lastSample = std::max(s.lastSample, std::fabs(static_cast<double>(pcm.samples[(frames - 1) * ch + c])));
    }
    return s;
}

float normalize(Pcm& pcm, float peak) {
    float maxv = 0.f;
    for (float v : pcm.samples) maxv = std::max(maxv, std::fabs(v));
    if (maxv < 1e-6f) return 1.f;
    float gain = peak / maxv;
    for (float& v : pcm.samples) v *= gain;
    return gain;
}

void fadeEdges(Pcm& pcm, float fadeInSeconds, float fadeOutSeconds) {
    const size_t frames = pcm.frames();
    const auto ch = static_cast<size_t>(pcm.channels);
    const auto inFrames = std::min(frames, static_cast<size_t>(std::max(0.f, fadeInSeconds) * static_cast<float>(pcm.sampleRate)));
    const auto outFrames = std::min(frames, static_cast<size_t>(std::max(0.f, fadeOutSeconds) * static_cast<float>(pcm.sampleRate)));
    for (size_t i = 0; i < inFrames; ++i) {
        float g = static_cast<float>(i) / static_cast<float>(inFrames);
        for (size_t c = 0; c < ch; ++c) pcm.samples[i * ch + c] *= g;
    }
    for (size_t i = 0; i < outFrames; ++i) {
        float g = static_cast<float>(i) / static_cast<float>(outFrames);
        size_t idx = frames - 1 - i;
        for (size_t c = 0; c < ch; ++c) pcm.samples[idx * ch + c] *= g;
    }
}

Pcm toMono(const Pcm& pcm) {
    if (pcm.channels == 1) return pcm;
    Pcm out;
    out.sampleRate = pcm.sampleRate;
    out.channels = 1;
    const size_t frames = pcm.frames();
    const auto ch = static_cast<size_t>(pcm.channels);
    out.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        float acc = 0;
        for (size_t c = 0; c < ch; ++c) acc += pcm.samples[i * ch + c];
        out.samples[i] = acc / static_cast<float>(ch);
    }
    return out;
}

Pcm toStereo(const Pcm& pcm) {
    if (pcm.channels == 2) return pcm;
    Pcm mono = toMono(pcm);
    Pcm out;
    out.sampleRate = pcm.sampleRate;
    out.channels = 2;
    out.samples.resize(mono.samples.size() * 2);
    for (size_t i = 0; i < mono.samples.size(); ++i) out.samples[2 * i] = out.samples[2 * i + 1] = mono.samples[i];
    return out;
}

Pcm makeSeamlessLoop(const Pcm& in, size_t frames) {
    const auto ch = static_cast<size_t>(in.channels);
    const size_t total = in.frames();
    Pcm out;
    out.sampleRate = in.sampleRate;
    out.channels = in.channels;
    frames = std::min(frames, total);
    const size_t overlap = std::min(frames / 2, total - frames);
    out.samples.assign(in.samples.begin(), in.samples.begin() + static_cast<std::ptrdiff_t>(frames * ch));
    // out[i] blends the *continuation* of the signal (in[frames + i]) into in[i]. At i = 0 it equals
    // the sample that follows out[frames - 1] in the original, so the wrap is seamless.
    for (size_t i = 0; i < overlap; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(overlap);
        float wIn = std::sin(t * 1.57079632679f), wCont = std::cos(t * 1.57079632679f);
        for (size_t c = 0; c < ch; ++c) {
            out.samples[i * ch + c] = in.samples[i * ch + c] * wIn + in.samples[(frames + i) * ch + c] * wCont;
        }
    }
    return out;
}

}  // namespace sky::audio
