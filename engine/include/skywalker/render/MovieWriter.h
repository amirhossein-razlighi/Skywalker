#pragma once
// Movie output (docs/MOVIE_RENDER.md): PNG image sequences on every platform, and video files
// encoded natively with AVFoundation on Apple platforms (H.264 / 10-bit HEVC in .mp4 or .mov,
// ProRes 422 HQ / 4444 in .mov). No ffmpeg: the encoder lives in the platform layer
// (engine/platform/metal/MovieWriter.mm) behind this small interface.

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/render/Image.h"

namespace sky::movie {

/// One output frame: display-referred (sRGB-encoded) RGBA in [0, 1], row 0 at the top. Floats keep
/// the extra precision of accumulated sub-frames for the 10/12-bit codecs.
struct FrameBuffer {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;  // width * height * 4

    FrameBuffer() = default;
    FrameBuffer(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0.f) {}
    /// Adds an 8-bit image (same size) with a weight: accumulation of sub-frames.
    void accumulate(const Image& image, float weight);
    /// 8-bit RGBA, rounded.
    Image toImage() const;
};

enum class Codec { Png, H264, Hevc, ProRes422HQ, ProRes4444 };
const char* codecName(Codec c);  // "png", "h264", "hevc", "prores", "prores4444"
std::optional<Codec> codecFromName(std::string_view name);
const std::vector<std::string>& codecNames();
/// The codec a path implies: ".mp4"/".m4v" H.264, ".mov" ProRes 422 HQ, anything else a PNG sequence.
Codec codecForPath(const std::string& path);
inline bool isVideo(Codec c) { return c != Codec::Png; }
/// True when this build can encode video (AVFoundation on Apple platforms); PNG sequences always work.
bool videoEncodingAvailable();
/// A sensible average bitrate (Mbit/s) for H.264 / HEVC at this size and rate (0 for ProRes / PNG).
double defaultBitrateMbps(Codec codec, int width, int height, int fps);

struct VideoSettings {
    std::string path;  // absolute; an existing file is replaced
    Codec codec = Codec::H264;
    int width = 1920;
    int height = 1080;
    int fps = 24;
    double bitrateMbps = 0;  // H.264 / HEVC; 0 = defaultBitrateMbps()
};

/// Encodes consecutive frames (1/fps apart) into a video file. Tagged Rec.709; HEVC is Main10 and
/// ProRes 422 HQ is 10-bit, both fed 16 bits per channel.
class VideoWriter {
public:
    virtual ~VideoWriter() = default;
    virtual Status append(const FrameBuffer& frame) = 0;
    /// Completes and closes the file (a cancelled render still produces a valid, shorter movie).
    virtual Status finish() = 0;
    virtual int framesWritten() const = 0;
};

/// Opens a video file for writing. Fails with `unsupported_codec` where video encoding is unavailable.
Result<std::unique_ptr<VideoWriter>> openVideoWriter(const VideoSettings& settings);

/// A PNG image sequence such as "renders/shot/frame_####.png": a run of '#' (or a printf-style
/// %04d) marks the zero-padded frame number. A path ending in ".png" without one gets "_####"
/// before the extension; any other path is a folder that receives "frame_####.png".
class PngSequence {
public:
    explicit PngSequence(const std::string& pattern);
    std::string pathFor(int frameNumber) const;
    /// The pattern with '#' placeholders (for reports), e.g. "/abs/renders/shot/frame_####.png".
    std::string pattern() const;
    std::string folder() const;
    /// Writes atomically (a temporary file renamed into place), so an interrupted render never
    /// leaves a truncated frame behind for resume to trust.
    Status write(int frameNumber, const FrameBuffer& frame) const;
    /// The first frame number in [first, last] whose file does not exist (last + 1 if all do):
    /// where a resumed render continues.
    int firstMissing(int first, int last) const;

private:
    std::string prefix_;
    std::string suffix_;
    int digits_ = 4;
};

}  // namespace sky::movie
