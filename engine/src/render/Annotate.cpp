// "Set-of-mark" annotation: outlines each visible entity and stamps its id in a label.
// Vision models ground references far more reliably when every object carries a visible
// id that matches the structured list returned alongside the image.

#include <algorithm>
#include <string>

#include "skywalker/render/Renderer.h"

namespace sky {

namespace {

// 3x5 bitmap font for '#' and digits; each row is 3 bits, MSB = left pixel.
const uint8_t kGlyphs[11][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
    {0b111, 0b001, 0b010, 0b010, 0b010},  // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
    {0b101, 0b111, 0b101, 0b111, 0b101},  // #
};

void put(Image& img, int x, int y, const uint8_t c[3]) {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
    uint8_t* p = img.at(x, y);
    p[0] = c[0];
    p[1] = c[1];
    p[2] = c[2];
}

void fillRect(Image& img, int x0, int y0, int w, int h, const uint8_t c[3]) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) put(img, x, y, c);
}

void drawText(Image& img, int x, int y, const std::string& text, int scale, const uint8_t c[3]) {
    for (char ch : text) {
        int g = ch == '#' ? 10 : (ch >= '0' && ch <= '9' ? ch - '0' : -1);
        if (g >= 0) {
            for (int row = 0; row < 5; ++row) {
                for (int col = 0; col < 3; ++col) {
                    if (kGlyphs[g][row] & (0b100 >> col)) fillRect(img, x + col * scale, y + row * scale, scale, scale, c);
                }
            }
        }
        x += 4 * scale;
    }
}

}  // namespace

void annotate(Image& img, const std::vector<VisibleEntity>& visible) {
    static const uint8_t palette[6][3] = {{255, 92, 92},  {92, 200, 255}, {255, 200, 64},
                                          {120, 230, 120}, {220, 120, 255}, {255, 150, 60}};
    const uint8_t white[3] = {255, 255, 255};
    const int scale = std::max(1, img.height / 360);
    for (size_t i = 0; i < visible.size(); ++i) {
        const auto& v = visible[i];
        const uint8_t* c = palette[v.id % 6];
        int x0 = static_cast<int>(v.x), y0 = static_cast<int>(v.y);
        int x1 = static_cast<int>(v.x + v.w) - 1, y1 = static_cast<int>(v.y + v.h) - 1;
        for (int t = 0; t < scale; ++t) {
            for (int x = x0; x <= x1; ++x) {
                put(img, x, y0 + t, c);
                put(img, x, y1 - t, c);
            }
            for (int y = y0; y <= y1; ++y) {
                put(img, x0 + t, y, c);
                put(img, x1 - t, y, c);
            }
        }
        std::string label = "#" + std::to_string(v.id);
        int lw = static_cast<int>(label.size()) * 4 * scale + scale;
        int lh = 7 * scale;
        int lx = std::clamp(x0, 0, std::max(0, img.width - lw));
        int ly = std::clamp(y0 - lh, 0, std::max(0, img.height - lh));
        fillRect(img, lx, ly, lw, lh, c);
        drawText(img, lx + scale, ly + scale, label, scale, white);
    }
}

}  // namespace sky
