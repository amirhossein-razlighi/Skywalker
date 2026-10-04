#pragma once
// Image decoding (stb_image: PNG, JPEG, BMP, TGA) and a decoded-image cache that notices file
// changes. Used by atlas packing, sprite sizing and the CPU rasterizer.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "skywalker/core/Result.h"
#include "skywalker/render/Image.h"

namespace sky::render2d {

/// Decodes an image file into RGBA8.
Result<Image> loadImage(const std::string& path);
Result<Image> decodeImage(const uint8_t* data, size_t size);
/// Reads only the header: width and height.
bool imageInfo(const std::string& path, int& width, int& height);

/// Decoded images and sizes keyed by absolute path. Entries are re-validated against the
/// file's modification time at most once per `revalidateSeconds` (steady clock).
class ImageCache {
public:
    std::shared_ptr<const Image> image(const std::string& path);
    bool size(const std::string& path, int& width, int& height);
    void invalidate(const std::string& path);
    void clear() { entries_.clear(); }

    double revalidateSeconds = 1.0;

private:
    struct Entry {
        int64_t mtime = -1;
        double checkedAt = -1e9;
        bool infoOk = false;
        int width = 0, height = 0;
        bool decoded = false;
        std::shared_ptr<const Image> image;
    };
    Entry& entry(const std::string& path);
    std::unordered_map<std::string, Entry> entries_;
};

/// File modification time (nanoseconds since epoch) or -1.
int64_t fileMTime(const std::string& path);

}  // namespace sky::render2d
