#include <zlib.h>

#include <cstdio>
#include <memory>

#include "skywalker/render/Image.h"

namespace sky {

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
    put32(out, static_cast<uint32_t>(size));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size) out.insert(out.end(), data, data + size);
    uLong crc = crc32(0L, out.data() + start, static_cast<uInt>(size + 4));
    put32(out, static_cast<uint32_t>(crc));
}

}  // namespace

std::vector<uint8_t> encodePng(const Image& image) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (image.width <= 0 || image.height <= 0) return {};

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(image.width));
    put32(ihdr, static_cast<uint32_t>(image.height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no interlace
    chunk(out, "IHDR", ihdr.data(), ihdr.size());

    // Filter type 0 (None) per row is fast; "Up" filter compresses renders noticeably better.
    const size_t stride = static_cast<size_t>(image.width) * 4;
    std::vector<uint8_t> raw((stride + 1) * static_cast<size_t>(image.height));
    for (int y = 0; y < image.height; ++y) {
        uint8_t* row = raw.data() + static_cast<size_t>(y) * (stride + 1);
        const uint8_t* src = image.pixels.data() + static_cast<size_t>(y) * stride;
        if (y == 0) {
            row[0] = 0;
            std::copy(src, src + stride, row + 1);
        } else {
            row[0] = 2;  // Up
            const uint8_t* above = src - stride;
            for (size_t i = 0; i < stride; ++i) row[1 + i] = static_cast<uint8_t>(src[i] - above[i]);
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) return {};
    chunk(out, "IDAT", compressed.data(), compressedSize);
    chunk(out, "IEND", nullptr, 0);
    return out;
}

Status writePng(const Image& image, const std::string& path) {
    std::vector<uint8_t> data = encodePng(image);
    if (data.empty()) return Error::make("encode_failed", "could not encode PNG");
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (!f) return Error::make("io_error", "cannot write " + path);
    if (std::fwrite(data.data(), 1, data.size(), f.get()) != data.size()) return Error::make("io_error", "short write to " + path);
    return {};
}

}  // namespace sky
