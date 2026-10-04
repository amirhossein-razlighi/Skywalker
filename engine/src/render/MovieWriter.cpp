// Platform-independent movie output: frame buffers, codec names, PNG image sequences. Video
// encoding is in engine/platform/metal/MovieWriter.mm (AVFoundation); other platforms get the
// stub at the bottom of this file.

#include "skywalker/render/MovieWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "skywalker/core/Strings.h"

namespace sky::movie {

namespace fs = std::filesystem;

void FrameBuffer::accumulate(const Image& image, float weight) {
    if (image.width != width || image.height != height) return;
    const float scale = weight / 255.f;
    const size_t n = rgba.size();
    for (size_t i = 0; i < n; ++i) rgba[i] += static_cast<float>(image.pixels[i]) * scale;
}

Image FrameBuffer::toImage() const {
    Image img(width, height);
    const size_t n = rgba.size();
    for (size_t i = 0; i < n; ++i) {
        img.pixels[i] = static_cast<uint8_t>(std::lround(std::clamp(rgba[i], 0.f, 1.f) * 255.f));
    }
    return img;
}

const std::vector<std::string>& codecNames() {
    static const std::vector<std::string> names{"png", "h264", "hevc", "prores", "prores4444"};
    return names;
}

const char* codecName(Codec c) {
    switch (c) {
        case Codec::Png: return "png";
        case Codec::H264: return "h264";
        case Codec::Hevc: return "hevc";
        case Codec::ProRes422HQ: return "prores";
        case Codec::ProRes4444: return "prores4444";
    }
    return "png";
}

std::optional<Codec> codecFromName(std::string_view name) {
    std::string n = str::lower(std::string(name));
    if (n == "png") return Codec::Png;
    if (n == "h264" || n == "avc") return Codec::H264;
    if (n == "hevc" || n == "h265") return Codec::Hevc;
    if (n == "prores" || n == "prores422" || n == "prores422hq") return Codec::ProRes422HQ;
    if (n == "prores4444") return Codec::ProRes4444;
    return std::nullopt;
}

Codec codecForPath(const std::string& path) {
    std::string ext = str::lower(fs::path(path).extension().string());
    if (ext == ".mp4" || ext == ".m4v") return Codec::H264;
    if (ext == ".mov") return Codec::ProRes422HQ;
    return Codec::Png;
}

double defaultBitrateMbps(Codec codec, int width, int height, int fps) {
    // Bits per pixel per frame for a high-quality master that still streams: HEVC needs ~2/3 of H.264.
    double bpp = codec == Codec::H264 ? 0.30 : codec == Codec::Hevc ? 0.20 : 0.0;
    return bpp * static_cast<double>(width) * static_cast<double>(height) * static_cast<double>(fps) / 1e6;
}

// ---------------------------------------------------------------------------
// PNG sequences
// ---------------------------------------------------------------------------

PngSequence::PngSequence(const std::string& patternIn) {
    std::string pattern = patternIn;
    // printf style: %04d -> ####
    if (size_t p = pattern.find('%'); p != std::string::npos) {
        size_t q = p + 1;
        int width = 0;
        while (q < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[q]))) width = width * 10 + (pattern[q++] - '0');
        if (q < pattern.size() && pattern[q] == 'd') pattern.replace(p, q + 1 - p, std::string(static_cast<size_t>(std::clamp(width, 1, 9)), '#'));
    }
    size_t hash = pattern.rfind('#');
    if (hash == std::string::npos) {
        if (str::lower(fs::path(pattern).extension().string()) == ".png") {
            fs::path p(pattern);
            pattern = (p.parent_path() / (p.stem().string() + "_####.png")).string();
        } else {
            pattern = (fs::path(pattern) / "frame_####.png").string();
        }
        hash = pattern.rfind('#');
    }
    size_t first = hash;
    while (first > 0 && pattern[first - 1] == '#') --first;
    prefix_ = pattern.substr(0, first);
    suffix_ = pattern.substr(hash + 1);
    digits_ = static_cast<int>(hash + 1 - first);
}

std::string PngSequence::pathFor(int frameNumber) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*d", digits_, frameNumber);
    return prefix_ + buf + suffix_;
}

std::string PngSequence::pattern() const { return prefix_ + std::string(static_cast<size_t>(digits_), '#') + suffix_; }

std::string PngSequence::folder() const { return fs::path(pathFor(0)).parent_path().string(); }

Status PngSequence::write(int frameNumber, const FrameBuffer& frame) const {
    std::string path = pathFor(frameNumber);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".part";
    if (Status s = writePng(frame.toImage(), tmp); !s) return s;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return Error::make("io_error", "cannot write " + path);
    }
    return {};
}

int PngSequence::firstMissing(int first, int last) const {
    std::error_code ec;
    for (int f = first; f <= last; ++f) {
        if (!fs::exists(pathFor(f), ec)) return f;
    }
    return last + 1;
}

// ---------------------------------------------------------------------------
// No video encoder on this platform
// ---------------------------------------------------------------------------

#if !SKY_HAS_AVFOUNDATION
bool videoEncodingAvailable() { return false; }

Result<std::unique_ptr<VideoWriter>> openVideoWriter(const VideoSettings& settings) {
    return Error::make("unsupported_codec",
                       std::string("this build cannot encode ") + codecName(settings.codec) + " video (no AVFoundation)",
                       "render a PNG sequence (a folder or name_####.png) and encode it with your video tool");
}
#endif

}  // namespace sky::movie
