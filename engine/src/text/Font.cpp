#include "skywalker/text/Font.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "stb_truetype.h"

// The default UI font is compiled into the engine (when the compiler supports #embed), so text
// always renders, even in binaries moved away from the source tree.
#if defined(__has_embed) && defined(SKY_DEFAULT_FONT_FILE)
#if __has_embed(SKY_DEFAULT_FONT_FILE)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
static const unsigned char kEmbeddedDefaultFont[] = {
#embed SKY_DEFAULT_FONT_FILE
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#define SKY_HAS_EMBEDDED_FONT 1
#endif
#endif

namespace sky::text {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

uint32_t decodeUtf8(std::string_view s, size_t& i) {
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0 || i + static_cast<size_t>(extra) >= s.size() || c >= 0xF8) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k) {
        unsigned char cc = byte(i + static_cast<size_t>(k));
        if ((cc & 0xC0) != 0x80) {
            i += static_cast<size_t>(k);
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += static_cast<size_t>(extra) + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------

// OpenType GPOS pair kerning ('kern' feature lookups, pair adjustment formats 1 and 2, extension
// lookups, any value format). stb_truetype only reads the simplest layout, which most modern
// (and all variable) fonts don't use. Every read is bounds-checked.
class GposKerning {
public:
    void init(const uint8_t* data, size_t size, uint32_t gposOffset) {
        data_ = data;
        size_ = size;
        if (!gposOffset || static_cast<size_t>(gposOffset) + 10 > size) return;
        const uint32_t g = gposOffset;
        if (u16(g) != 1) return;
        const uint32_t featureList = g + u16(g + 6), lookupList = g + u16(g + 8);
        std::vector<uint16_t> lookups;
        const uint16_t featureCount = u16(featureList);
        for (uint32_t i = 0; i < featureCount; ++i) {
            uint32_t rec = featureList + 2 + i * 6;
            if (!has(rec, 6) || std::memcmp(data_ + rec, "kern", 4) != 0) continue;
            uint32_t feature = featureList + u16(rec + 4);
            uint16_t n = u16(feature + 2);
            for (uint32_t k = 0; k < n; ++k) lookups.push_back(u16(feature + 4 + k * 2));
        }
        std::sort(lookups.begin(), lookups.end());
        lookups.erase(std::unique(lookups.begin(), lookups.end()), lookups.end());
        const uint16_t lookupCount = u16(lookupList);
        for (uint16_t li : lookups) {
            if (li >= lookupCount) continue;
            uint32_t lookup = lookupList + u16(lookupList + 2 + li * 2u);
            uint16_t type = u16(lookup), subCount = u16(lookup + 4);
            std::vector<uint32_t> subs;
            for (uint32_t s = 0; s < subCount; ++s) {
                uint32_t sub = lookup + u16(lookup + 6 + s * 2);
                if (type == 9 && u16(sub) == 1 && u16(sub + 2) == 2) subs.push_back(sub + u32(sub + 4));
                else if (type == 2) subs.push_back(sub);
            }
            if (!subs.empty()) lookups_.push_back(std::move(subs));
        }
    }
    bool available() const { return !lookups_.empty(); }

    int advance(int g1, int g2) const {
        int total = 0;
        for (const auto& subs : lookups_) {
            for (uint32_t t : subs) {
                int value = 0;
                if (pair(t, g1, g2, value)) {
                    total += value;
                    break;  // the first subtable that applies wins within a lookup
                }
            }
        }
        return total;
    }

private:
    bool has(uint32_t off, uint32_t n) const { return static_cast<size_t>(off) + n <= size_; }
    uint16_t u16(uint32_t off) const { return has(off, 2) ? static_cast<uint16_t>(data_[off] << 8 | data_[off + 1]) : 0; }
    int16_t s16(uint32_t off) const { return static_cast<int16_t>(u16(off)); }
    uint32_t u32(uint32_t off) const { return static_cast<uint32_t>(u16(off)) << 16 | u16(off + 2); }
    static int bitCount(uint16_t v) {
        int n = 0;
        for (; v; v &= static_cast<uint16_t>(v - 1)) ++n;
        return n;
    }

    int coverage(uint32_t cov, int glyph) const {
        uint16_t format = u16(cov), count = u16(cov + 2);
        int lo = 0, hi = count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (format == 1) {
                int g = u16(cov + 4 + static_cast<uint32_t>(mid) * 2);
                if (glyph < g) hi = mid - 1;
                else if (glyph > g) lo = mid + 1;
                else return mid;
            } else if (format == 2) {
                uint32_t rec = cov + 4 + static_cast<uint32_t>(mid) * 6;
                int start = u16(rec), end = u16(rec + 2);
                if (glyph < start) hi = mid - 1;
                else if (glyph > end) lo = mid + 1;
                else return u16(rec + 4) + (glyph - start);
            } else {
                return -1;
            }
        }
        return -1;
    }

    int glyphClass(uint32_t def, int glyph) const {
        uint16_t format = u16(def);
        if (format == 1) {
            int start = u16(def + 2), count = u16(def + 4);
            if (glyph >= start && glyph < start + count) return u16(def + 6 + static_cast<uint32_t>(glyph - start) * 2);
            return 0;
        }
        if (format == 2) {
            int lo = 0, hi = u16(def + 2) - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                uint32_t rec = def + 4 + static_cast<uint32_t>(mid) * 6;
                if (glyph < u16(rec)) hi = mid - 1;
                else if (glyph > u16(rec + 2)) lo = mid + 1;
                else return u16(rec + 4);
            }
        }
        return 0;
    }

    /// x advance of the first glyph inside a value record (0 if the format has none).
    int xAdvance(uint32_t record, uint16_t format) const {
        if (!(format & 0x4)) return 0;
        return s16(record + 2u * static_cast<uint32_t>(bitCount(format & 0x3)));
    }

    bool pair(uint32_t t, int g1, int g2, int& value) const {
        uint16_t format = u16(t);
        int ci = coverage(t + u16(t + 2), g1);
        if (ci < 0) return false;
        uint16_t vf1 = u16(t + 4), vf2 = u16(t + 6);
        uint32_t size1 = 2u * static_cast<uint32_t>(bitCount(vf1)), size2 = 2u * static_cast<uint32_t>(bitCount(vf2));
        if (format == 1) {
            if (ci >= u16(t + 8)) return false;
            uint32_t set = t + u16(t + 10 + static_cast<uint32_t>(ci) * 2);
            uint16_t n = u16(set);
            uint32_t recSize = 2 + size1 + size2;
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                uint32_t rec = set + 2 + static_cast<uint32_t>(mid) * recSize;
                int second = u16(rec);
                if (g2 < second) hi = mid - 1;
                else if (g2 > second) lo = mid + 1;
                else {
                    value = xAdvance(rec + 2, vf1);
                    return true;
                }
            }
            return false;
        }
        if (format == 2) {
            int c1 = glyphClass(t + u16(t + 8), g1), c2 = glyphClass(t + u16(t + 10), g2);
            uint16_t n1 = u16(t + 12), n2 = u16(t + 14);
            if (c1 >= n1 || c2 >= n2) return false;
            uint32_t rec = t + 16 + (static_cast<uint32_t>(c1) * n2 + static_cast<uint32_t>(c2)) * (size1 + size2);
            value = xAdvance(rec, vf1);
            return true;
        }
        return false;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    std::vector<std::vector<uint32_t>> lookups_;  // per kern lookup: its pair-adjustment subtables
};

struct Font::Impl {
    stbtt_fontinfo info{};
    GposKerning gpos;
    mutable std::unordered_map<uint64_t, float> kernCache;
};

Font::~Font() = default;

Result<std::shared_ptr<Font>> Font::fromFile(const std::string& path, std::string name) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot open font " + path);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (name.empty()) name = fs::path(path).stem().string();
    return fromMemory(std::move(data), std::move(name));
}

Result<std::shared_ptr<Font>> Font::fromMemory(std::vector<uint8_t> data, std::string name) {
    if (data.size() < 12) return Error::make("invalid_font", "font data is too small: " + name);
    std::shared_ptr<Font> font(new Font());
    font->name_ = std::move(name);
    font->data_ = std::move(data);
    font->impl_ = std::make_unique<Impl>();
    int offset = stbtt_GetFontOffsetForIndex(font->data_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font->impl_->info, font->data_.data(), offset)) {
        return Error::make("invalid_font", "not a TrueType/OpenType font: " + font->name_);
    }
    const stbtt_fontinfo& info = font->impl_->info;
    font->impl_->gpos.init(font->data_.data(), font->data_.size(), static_cast<uint32_t>(info.gpos));
    font->scale_ = stbtt_ScaleForMappingEmToPixels(&info, 1.f);
    int asc = 0, desc = 0, gap = 0;
    // Prefer the OS/2 typographic metrics (consistent line heights across platforms).
    if (!stbtt_GetFontVMetricsOS2(&info, &asc, &desc, &gap)) stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    font->ascent_ = static_cast<float>(asc) * font->scale_;
    font->descent_ = static_cast<float>(-desc) * font->scale_;
    font->lineGap_ = std::max(0.f, static_cast<float>(gap) * font->scale_);
    if (font->ascent_ <= 0.f) font->ascent_ = 0.8f;
    if (font->descent_ < 0.f) font->descent_ = 0.2f;
    return font;
}

bool Font::hasGlyph(uint32_t codepoint) const {
    return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(codepoint)) != 0;
}

float Font::kerning(uint32_t left, uint32_t right) const {
    const uint64_t key = static_cast<uint64_t>(left) << 32 | right;
    if (auto it = impl_->kernCache.find(key); it != impl_->kernCache.end()) return it->second;
    int a = stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(left));
    int b = stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(right));
    float k = 0.f;
    if (a && b) {
        int units = impl_->gpos.available() ? impl_->gpos.advance(a, b) : stbtt_GetGlyphKernAdvance(&impl_->info, a, b);
        k = static_cast<float>(units) * scale_;
    }
    if (impl_->kernCache.size() > 65536) impl_->kernCache.clear();
    impl_->kernCache.emplace(key, k);
    return k;
}

bool Font::place(int w, int h, int& page, int& x, int& y) {
    const int gutter = 1;
    w += gutter;
    h += gutter;
    if (w > kPageSize || h > kPageSize) return false;
    for (size_t p = 0; p <= pageImages_.size(); ++p) {
        if (p == pageImages_.size()) {
            auto img = std::make_shared<TextureImage>();
            img->key = "font:" + name_ + "#" + std::to_string(p);
            img->width = img->height = kPageSize;
            img->channels = 1;
            img->pixels.assign(static_cast<size_t>(kPageSize) * kPageSize, 0);
            pageImages_.push_back(img);
            pageViews_.push_back(img);
            shelves_.emplace_back();
            pageFill_.push_back(0);
        }
        auto& shelves = shelves_[p];
        // Best-fitting existing shelf (height within 30%), else open a new one.
        Shelf* best = nullptr;
        for (auto& s : shelves) {
            if (s.height >= h && s.height <= h + h / 3 + 2 && s.x + w <= kPageSize && (!best || s.height < best->height)) best = &s;
        }
        if (!best && pageFill_[p] + h <= kPageSize) {
            shelves.push_back({pageFill_[p], h, 0});
            pageFill_[p] += h;
            best = &shelves.back();
        }
        if (best) {
            page = static_cast<int>(p);
            x = best->x;
            y = best->y;
            best->x += w;
            return true;
        }
    }
    return false;
}

const Glyph& Font::glyph(uint32_t codepoint) {
    if (auto it = glyphs_.find(codepoint); it != glyphs_.end()) return it->second;
    const stbtt_fontinfo& info = impl_->info;
    Glyph g;
    g.index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, g.index, &adv, &lsb);
    g.advance = static_cast<float>(adv) * scale_;
    bool invisible = codepoint == ' ' || codepoint == '\t' || codepoint == 0xA0 || codepoint == '\n' ||
                     (codepoint >= 0x2000 && codepoint <= 0x200B) || stbtt_IsGlyphEmpty(&info, g.index);
    if (!invisible) {
        const float pixelScale = scale_ * kEmPixels;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* sdf = stbtt_GetGlyphSDF(&info, pixelScale, g.index, kSpread, 128,
                                               128.f / static_cast<float>(kSpread), &w, &h, &xoff, &yoff);
        int page = -1, px = 0, py = 0;
        if (sdf && w > 0 && h > 0 && place(w, h, page, px, py)) {
            TextureImage& img = *pageImages_[static_cast<size_t>(page)];
            for (int row = 0; row < h; ++row) {
                std::copy_n(sdf + static_cast<size_t>(row) * w, w,
                            img.pixels.begin() + static_cast<std::ptrdiff_t>((py + row) * kPageSize + px));
            }
            ++img.version;
            const float inv = 1.f / kEmPixels, tex = 1.f / static_cast<float>(kPageSize);
            g.page = page;
            g.x0 = static_cast<float>(xoff) * inv;
            g.y0 = static_cast<float>(yoff) * inv;
            g.x1 = static_cast<float>(xoff + w) * inv;
            g.y1 = static_cast<float>(yoff + h) * inv;
            g.u0 = static_cast<float>(px) * tex;
            g.v0 = static_cast<float>(py) * tex;
            g.u1 = static_cast<float>(px + w) * tex;
            g.v1 = static_cast<float>(py + h) * tex;
        } else if (sdf) {
            log::warn("text", "font atlas full for " + name_);
        }
        if (sdf) stbtt_FreeSDF(sdf, nullptr);
    }
    return glyphs_.emplace(codepoint, g).first->second;
}

// ---------------------------------------------------------------------------
// FontLibrary
// ---------------------------------------------------------------------------

namespace {

struct Builtin {
    const char* name;
    const char* file;
    std::vector<std::string> aliases;
};

const std::vector<Builtin>& builtins() {
    static const std::vector<Builtin> list{
        {"Inter", "Inter.ttf", {"", "default", "sans", "sans-serif", "ui", "inter"}},
        {"EB Garamond", "EBGaramond.ttf", {"serif", "garamond", "eb garamond", "ebgaramond", "book"}},
        {"JetBrains Mono", "JetBrainsMono.ttf", {"mono", "monospace", "code", "jetbrains mono", "jetbrainsmono"}},
    };
    return list;
}

}  // namespace

FontLibrary::FontLibrary(std::string projectDir) : projectDir_(std::move(projectDir)) {}

const std::vector<std::string>& FontLibrary::builtinNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& b : builtins()) n.push_back(b.name);
        return n;
    }();
    return names;
}

std::string FontLibrary::builtinAssetsDir() {
    std::error_code ec;
    if (const char* env = std::getenv("SKYWALKER_ASSETS"); env && *env && fs::is_directory(env, ec)) return env;
#ifdef SKY_BUILTIN_ASSETS_DIR
    if (fs::is_directory(SKY_BUILTIN_ASSETS_DIR, ec)) return SKY_BUILTIN_ASSETS_DIR;
#endif
    return {};
}

std::string FontLibrary::canonical(const std::string& nameOrPath) const {
    std::string lowered = str::lower(str::trim(nameOrPath));
    for (const auto& b : builtins()) {
        if (lowered == str::lower(b.name)) return b.name;
        for (const auto& a : b.aliases) {
            if (lowered == a) return b.name;
        }
    }
    return nameOrPath;
}

Result<std::shared_ptr<Font>> FontLibrary::find(const std::string& nameOrPath) {
    std::string key = canonical(nameOrPath);
    if (auto it = fonts_.find(key); it != fonts_.end()) return it->second;
    if (auto it = failures_.find(key); it != failures_.end()) return it->second;

    Result<std::shared_ptr<Font>> loaded = Error::make("not_found", "font not found");
    const Builtin* builtin = nullptr;
    for (const auto& b : builtins()) {
        if (key == b.name) builtin = &b;
    }
    if (builtin) {
        std::string dir = builtinAssetsDir();
        if (!dir.empty()) loaded = Font::fromFile((fs::path(dir) / "fonts" / builtin->file).string(), builtin->name);
#ifdef SKY_HAS_EMBEDDED_FONT
        if (!loaded && key == "Inter") {
            loaded = Font::fromMemory(std::vector<uint8_t>(std::begin(kEmbeddedDefaultFont), std::end(kEmbeddedDefaultFont)), "Inter");
        }
#endif
        if (!loaded) {
            loaded = Error::make("not_found", "built-in font " + key + " is missing (set SKYWALKER_ASSETS to the engine's assets folder)");
        }
    } else {
        fs::path p(nameOrPath);
        std::string ext = str::lower(p.extension().string());
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
            fs::path full = p.is_absolute() ? p : fs::path(projectDir_) / p;
            loaded = Font::fromFile(full.lexically_normal().string());
        } else {
            std::vector<std::string> candidates = builtinNames();
            for (const auto& b : builtins()) {
                for (const auto& a : b.aliases) {
                    if (!a.empty()) candidates.push_back(a);
                }
            }
            std::string guess = str::closest(nameOrPath, candidates, 3);
            loaded = Error::make("unknown_font", "unknown font \"" + nameOrPath + "\"",
                                 guess.empty() ? "use Inter (sans), EB Garamond (serif), JetBrains Mono (mono) or a .ttf/.otf path"
                                               : "did you mean \"" + canonical(guess) + "\"?");
        }
    }
    if (!loaded) {
        failures_.emplace(key, loaded.error());
        return loaded.error();
    }
    fonts_[key] = loaded.value();
    return loaded.value();
}

std::shared_ptr<Font> FontLibrary::defaultFont() {
    auto f = find("Inter");
    if (f) return f.value();
    // Last resort: any font we already have.
    for (const auto& [k, v] : fonts_) return v;
    return nullptr;
}

std::shared_ptr<Font> FontLibrary::get(const std::string& nameOrPath) {
    auto f = find(nameOrPath);
    if (f) return f.value();
    static thread_local std::string lastWarned;
    if (lastWarned != nameOrPath) {
        lastWarned = nameOrPath;
        log::warn("text", f.error().message + "; using the default font");
    }
    return defaultFont();
}

Font* FontLibrary::fallbackFor(uint32_t codepoint, Font* preferred) {
    if (preferred && preferred->hasGlyph(codepoint)) return preferred;
    for (const auto& b : builtins()) {
        auto f = find(b.name);
        if (f && f.value()->hasGlyph(codepoint)) return f.value().get();
    }
    return preferred;
}

}  // namespace sky::text
