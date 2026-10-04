#include "skywalker/render2d/ImageIO.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "skywalker/core/FileTime.h"
#include "stb_image.h"

namespace sky::render2d {

namespace fs = std::filesystem;

int64_t fileMTime(const std::string& path) { return fileModifiedNs(path); }

Result<Image> decodeImage(const uint8_t* data, size_t size) {
    if (!data || size == 0 || size > static_cast<size_t>(INT32_MAX)) return Error::make("invalid_image", "empty image data");
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
    if (!px) {
        const char* why = stbi_failure_reason();
        return Error::make("invalid_image", std::string("cannot decode image: ") + (why ? why : "unknown format"));
    }
    if (w <= 0 || h <= 0 || static_cast<int64_t>(w) * h > (int64_t{1} << 28)) {
        stbi_image_free(px);
        return Error::make("invalid_image", "image is empty or too large");
    }
    Image img(w, h);
    std::memcpy(img.pixels.data(), px, img.pixels.size());
    stbi_image_free(px);
    return img;
}

Result<Image> loadImage(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot open image " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto img = decodeImage(bytes.data(), bytes.size());
    if (!img) return Error::make(img.error().code, img.error().message + " (" + path + ")");
    return img;
}

bool imageInfo(const std::string& path, int& width, int& height) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    int n = 0;
    int ok = stbi_info_from_file(f, &width, &height, &n);
    std::fclose(f);
    return ok != 0 && width > 0 && height > 0;
}

namespace {
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

ImageCache::Entry& ImageCache::entry(const std::string& path) {
    Entry& e = entries_[path];
    double now = nowSeconds();
    if (now - e.checkedAt >= revalidateSeconds) {
        e.checkedAt = now;
        int64_t m = fileMTime(path);
        if (m != e.mtime) {
            e.mtime = m;
            e.infoOk = m >= 0 && imageInfo(path, e.width, e.height);
            e.decoded = false;
            e.image.reset();
        }
    }
    return e;
}

std::shared_ptr<const Image> ImageCache::image(const std::string& path) {
    Entry& e = entry(path);
    if (!e.decoded) {
        e.decoded = true;
        if (e.infoOk) {
            auto img = loadImage(path);
            if (img) e.image = std::make_shared<const Image>(std::move(img.value()));
        }
    }
    return e.image;
}

bool ImageCache::size(const std::string& path, int& width, int& height) {
    Entry& e = entry(path);
    if (!e.infoOk) return false;
    width = e.width;
    height = e.height;
    return true;
}

void ImageCache::invalidate(const std::string& path) { entries_.erase(path); }

}  // namespace sky::render2d
