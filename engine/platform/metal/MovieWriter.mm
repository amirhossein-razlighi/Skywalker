// Native video encoding with AVFoundation (AVAssetWriter + VideoToolbox): H.264, 10-bit HEVC and
// ProRes, no ffmpeg. See docs/MOVIE_RENDER.md.
//
// Frames arrive as display-referred float RGBA. H.264 is fed 8-bit BGRA; HEVC Main10 and ProRes
// are fed 16-bit ARGB so accumulated sub-frames keep their precision (VideoToolbox converts to
// the codec's 10/12-bit 4:2:0 / 4:2:2 / 4:4:4 itself). Everything is tagged Rec.709.
//
// Memory: ARC-managed Objective-C members of a C++ class; each entry point drains its own
// autorelease pool (the CLI calls from a plain thread).

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include "skywalker/render/MovieWriter.h"

namespace sky::movie {

namespace {

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

std::string describe(NSError* error) {
    if (!error) return "unknown error";
    std::string msg = error.localizedDescription.UTF8String ?: "unknown error";
    if (NSError* under = error.userInfo[NSUnderlyingErrorKey]) msg += " (" + std::string(under.localizedDescription.UTF8String ?: "") + ")";
    return msg;
}

inline uint16_t to16(float v) { return static_cast<uint16_t>(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f)); }
inline uint8_t to8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

class AvVideoWriter final : public VideoWriter {
public:
    static Result<std::unique_ptr<VideoWriter>> open(const VideoSettings& s) {
        @autoreleasepool {
            if (s.width <= 0 || s.height <= 0 || (s.width % 2) || (s.height % 2)) {
                return Error::make("invalid_size", "video frames need an even width and height (got " + std::to_string(s.width) +
                                                       "x" + std::to_string(s.height) + ")");
            }
            if (s.fps <= 0) return Error::make("invalid_fps", "fps must be positive");
            const std::string ext = [&] {
                std::string e = std::filesystem::path(s.path).extension().string();
                std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return e;
            }();
            const bool prores = s.codec == Codec::ProRes422HQ || s.codec == Codec::ProRes4444;
            if (s.codec == Codec::Png) return Error::make("invalid_codec", "png is an image sequence, not a video codec");
            if (prores && ext != ".mov") {
                return Error::make("invalid_container", "ProRes needs a .mov file (got " + s.path + ")", "use a .mov path, or codec h264/hevc for .mp4");
            }
            if (ext != ".mov" && ext != ".mp4" && ext != ".m4v") {
                return Error::make("invalid_container", "video files must end in .mp4 or .mov (got " + s.path + ")");
            }
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(s.path).parent_path(), ec);
            std::filesystem::remove(s.path, ec);  // AVAssetWriter refuses to overwrite

            NSError* error = nil;
            NSURL* url = [NSURL fileURLWithPath:ns(s.path)];
            AVFileType type = ext == ".mov" ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4;
            AVAssetWriter* writer = [AVAssetWriter assetWriterWithURL:url fileType:type error:&error];
            if (!writer) return Error::make("encoder_error", "cannot create " + s.path + ": " + describe(error));

            NSMutableDictionary* settings = [NSMutableDictionary dictionary];
            settings[AVVideoWidthKey] = @(s.width);
            settings[AVVideoHeightKey] = @(s.height);
            settings[AVVideoColorPropertiesKey] = @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            };
            OSType pixelFormat = kCVPixelFormatType_64ARGB;
            const double mbps = s.bitrateMbps > 0 ? s.bitrateMbps : defaultBitrateMbps(s.codec, s.width, s.height, s.fps);
            switch (s.codec) {
                case Codec::H264:
                    settings[AVVideoCodecKey] = AVVideoCodecTypeH264;
                    settings[AVVideoCompressionPropertiesKey] = @{
                        AVVideoAverageBitRateKey : @(static_cast<long long>(mbps * 1e6)),
                        AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                        AVVideoExpectedSourceFrameRateKey : @(s.fps),
                        AVVideoMaxKeyFrameIntervalKey : @(s.fps * 2),
                    };
                    pixelFormat = kCVPixelFormatType_32BGRA;
                    break;
                case Codec::Hevc:
                    settings[AVVideoCodecKey] = AVVideoCodecTypeHEVC;
                    settings[AVVideoCompressionPropertiesKey] = @{
                        AVVideoAverageBitRateKey : @(static_cast<long long>(mbps * 1e6)),
                        AVVideoProfileLevelKey : (__bridge NSString*)kVTProfileLevel_HEVC_Main10_AutoLevel,
                        AVVideoExpectedSourceFrameRateKey : @(s.fps),
                        AVVideoMaxKeyFrameIntervalKey : @(s.fps * 2),
                    };
                    break;
                case Codec::ProRes422HQ: settings[AVVideoCodecKey] = AVVideoCodecTypeAppleProRes422HQ; break;
                case Codec::ProRes4444: settings[AVVideoCodecKey] = AVVideoCodecTypeAppleProRes4444; break;
                case Codec::Png: break;
            }
            AVAssetWriterInput* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
            input.expectsMediaDataInRealTime = NO;
            if (![writer canAddInput:input]) {
                return Error::make("encoder_error", std::string("this Mac cannot encode ") + codecName(s.codec) + " at " +
                                                        std::to_string(s.width) + "x" + std::to_string(s.height));
            }
            [writer addInput:input];
            NSDictionary* attrs = @{
                (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey : @(pixelFormat),
                (__bridge NSString*)kCVPixelBufferWidthKey : @(s.width),
                (__bridge NSString*)kCVPixelBufferHeightKey : @(s.height),
            };
            AVAssetWriterInputPixelBufferAdaptor* adaptor =
                [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
                                                                                 sourcePixelBufferAttributes:attrs];
            if (![writer startWriting]) return Error::make("encoder_error", "cannot start writing " + s.path + ": " + describe(writer.error));
            [writer startSessionAtSourceTime:kCMTimeZero];
            auto w = std::unique_ptr<AvVideoWriter>(new AvVideoWriter());
            w->settings_ = s;
            w->writer_ = writer;
            w->input_ = input;
            w->adaptor_ = adaptor;
            w->pixelFormat_ = pixelFormat;
            return std::unique_ptr<VideoWriter>(std::move(w));
        }
    }

    ~AvVideoWriter() override {
        @autoreleasepool {
            if (writer_ && !finished_) [writer_ cancelWriting];  // an error path: no partial file
        }
    }

    Status append(const FrameBuffer& frame) override {
        @autoreleasepool {
            if (finished_) return Error::make("encoder_error", "the movie is already finished");
            if (frame.width != settings_.width || frame.height != settings_.height) {
                return Error::make("invalid_size", "frame size does not match the movie");
            }
            // Not real time: the encoder may still be busy with earlier frames.
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!input_.readyForMoreMediaData) {
                if (writer_.status == AVAssetWriterStatusFailed) return Error::make("encoder_error", describe(writer_.error));
                if (std::chrono::steady_clock::now() > deadline) return Error::make("encoder_timeout", "the video encoder stopped accepting frames");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CVPixelBufferRef pb = nullptr;
            CVReturn rc = adaptor_.pixelBufferPool ? CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor_.pixelBufferPool, &pb)
                                                   : CVPixelBufferCreate(nullptr, static_cast<size_t>(frame.width),
                                                                         static_cast<size_t>(frame.height), pixelFormat_, nullptr, &pb);
            if (rc != kCVReturnSuccess || !pb) return Error::make("encoder_error", "cannot allocate a video frame");
            CVPixelBufferLockBaseAddress(pb, 0);
            auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pb));
            const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
            const float* src = frame.rgba.data();
            for (int y = 0; y < frame.height; ++y) {
                uint8_t* row = base + bpr * static_cast<size_t>(y);
                const float* s = src + static_cast<size_t>(y) * static_cast<size_t>(frame.width) * 4;
                if (pixelFormat_ == kCVPixelFormatType_32BGRA) {
                    for (int x = 0; x < frame.width; ++x, s += 4, row += 4) {
                        row[0] = to8(s[2]);
                        row[1] = to8(s[1]);
                        row[2] = to8(s[0]);
                        row[3] = 255;
                    }
                } else {  // 64ARGB: 16-bit big-endian A, R, G, B
                    for (int x = 0; x < frame.width; ++x, s += 4, row += 8) {
                        const uint16_t c[4] = {65535, to16(s[0]), to16(s[1]), to16(s[2])};
                        for (int i = 0; i < 4; ++i) {
                            row[i * 2] = static_cast<uint8_t>(c[i] >> 8);
                            row[i * 2 + 1] = static_cast<uint8_t>(c[i] & 0xff);
                        }
                    }
                }
            }
            CVPixelBufferUnlockBaseAddress(pb, 0);
            BOOL ok = [adaptor_ appendPixelBuffer:pb withPresentationTime:CMTimeMake(frames_, settings_.fps)];
            CVPixelBufferRelease(pb);
            if (!ok) return Error::make("encoder_error", "cannot encode frame " + std::to_string(frames_) + ": " + describe(writer_.error));
            ++frames_;
            return {};
        }
    }

    Status finish() override {
        @autoreleasepool {
            if (finished_) return {};
            finished_ = true;
            if (frames_ == 0) {  // nothing to encode: leave no empty file behind
                [writer_ cancelWriting];
                std::error_code ec;
                std::filesystem::remove(settings_.path, ec);
                return Error::make("no_frames", "no frames were rendered into " + settings_.path);
            }
            [input_ markAsFinished];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [writer_ endSessionAtSourceTime:CMTimeMake(frames_, settings_.fps)];
            [writer_ finishWritingWithCompletionHandler:^{
              dispatch_semaphore_signal(done);
            }];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (writer_.status != AVAssetWriterStatusCompleted) {
                return Error::make("encoder_error", "cannot finish " + settings_.path + ": " + describe(writer_.error));
            }
            return {};
        }
    }

    int framesWritten() const override { return frames_; }

private:
    AvVideoWriter() = default;
    VideoSettings settings_;
    AVAssetWriter* writer_ = nil;
    AVAssetWriterInput* input_ = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor_ = nil;
    OSType pixelFormat_ = kCVPixelFormatType_32BGRA;
    int frames_ = 0;
    bool finished_ = false;
};

}  // namespace

bool videoEncodingAvailable() { return true; }

Result<std::unique_ptr<VideoWriter>> openVideoWriter(const VideoSettings& settings) { return AvVideoWriter::open(settings); }

}  // namespace sky::movie
