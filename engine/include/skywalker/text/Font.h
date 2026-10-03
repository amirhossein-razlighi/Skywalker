#pragma once
// Signed-distance-field fonts (stb_truetype).
//
// Glyphs are rasterized lazily, the first time they are laid out, as signed-distance fields into
// shared atlas pages (R8). One SDF serves every size: text stays crisp under any scale or zoom,
// and outlines, glows, drop shadows and faux bold are just thresholds in the shader. Kerning
// comes from the font's GPOS/kern tables. FontLibrary resolves agent-friendly names ("Inter",
// "serif", "mono") and project paths (*.ttf / *.otf) to fonts and caches them.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/core/Result.h"
#include "skywalker/render/Render2D.h"

namespace sky::text {

struct Glyph {
    int index = 0;      // glyph index in the font (0 = .notdef)
    float advance = 0;  // em units
    // Quad relative to the pen on the baseline, in em units, y down (negative y = above the baseline).
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;  // atlas texture coordinates
    int page = -1;                         // atlas page; -1 = nothing to draw (whitespace)
};

class Font {
public:
    static constexpr float kEmPixels = 48.f;  // SDF raster size of one em
    static constexpr int kSpread = 8;          // distance range (+-pixels) stored around each glyph
    static constexpr int kPageSize = 1024;

    static Result<std::shared_ptr<Font>> fromFile(const std::string& path, std::string name = {});
    static Result<std::shared_ptr<Font>> fromMemory(std::vector<uint8_t> data, std::string name);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    const std::string& name() const { return name_; }
    float ascent() const { return ascent_; }    // em units above the baseline
    float descent() const { return descent_; }  // em units below the baseline (positive)
    float lineGap() const { return lineGap_; }
    float lineHeight() const { return ascent_ + descent_ + lineGap_; }

    bool hasGlyph(uint32_t codepoint) const;
    /// Metrics and atlas location; rasterizes the SDF on first use. Unknown codepoints map to .notdef.
    const Glyph& glyph(uint32_t codepoint);
    /// Kerning between two codepoints, em units (usually negative).
    float kerning(uint32_t left, uint32_t right) const;

    /// Atlas pages (R8 SDF). Their `version` changes whenever glyphs are added.
    const std::vector<TextureImagePtr>& pages() const { return pageViews_; }
    size_t glyphCount() const { return glyphs_.size(); }

private:
    Font() = default;
    struct Impl;
    bool place(int w, int h, int& page, int& x, int& y);

    std::string name_;
    std::vector<uint8_t> data_;
    std::unique_ptr<Impl> impl_;
    float scale_ = 1.f;  // font units -> em
    float ascent_ = 0.8f, descent_ = 0.2f, lineGap_ = 0.f;
    std::unordered_map<uint32_t, Glyph> glyphs_;
    std::vector<std::shared_ptr<TextureImage>> pageImages_;
    std::vector<TextureImagePtr> pageViews_;
    struct Shelf {
        int y = 0, height = 0, x = 0;
    };
    std::vector<std::vector<Shelf>> shelves_;  // per page
    std::vector<int> pageFill_;                // next free y per page
};

/// Resolves font names and paths, caching loaded fonts.
///   "" / "default" / "sans" / "Inter"            -> Inter (built in; the UI default)
///   "serif" / "EB Garamond" / "garamond"          -> EB Garamond (built in; books, letters, documents)
///   "mono" / "monospace" / "JetBrains Mono"       -> JetBrains Mono (built in; terminals, code, data)
///   "fonts/MyFont.ttf"                            -> a project font file
class FontLibrary {
public:
    explicit FontLibrary(std::string projectDir = ".");

    /// The font for `nameOrPath`; falls back to the default font (with an error) when it can't be found.
    std::shared_ptr<Font> get(const std::string& nameOrPath);
    /// Strict lookup with a did-you-mean error (tools validate font names with it).
    Result<std::shared_ptr<Font>> find(const std::string& nameOrPath);
    std::shared_ptr<Font> defaultFont();
    /// A font that has `codepoint`, preferring `preferred` (used for fallback glyphs).
    Font* fallbackFor(uint32_t codepoint, Font* preferred);

    static const std::vector<std::string>& builtinNames();  // "Inter", "EB Garamond", "JetBrains Mono"
    /// Directory with the engine's built-in assets (fonts), or empty when not found.
    static std::string builtinAssetsDir();
    void setProjectDir(std::string dir) { projectDir_ = std::move(dir); }

private:
    std::string canonical(const std::string& nameOrPath) const;
    std::string projectDir_;
    std::unordered_map<std::string, std::shared_ptr<Font>> fonts_;
    std::unordered_map<std::string, Error> failures_;
};

/// Decodes one UTF-8 code point at `i` (advancing it). Invalid bytes decode as U+FFFD.
uint32_t decodeUtf8(std::string_view s, size_t& i);
void appendUtf8(std::string& out, uint32_t codepoint);

}  // namespace sky::text
