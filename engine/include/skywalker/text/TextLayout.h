#pragma once
// Text layout: UTF-8, rich text, kerning, wrapping and alignment.
//
// Rich text tags (TextMeshPro-style, case-insensitive; unknown tags are kept as literal text):
//   <b> <i> <u> <s>                    bold (SDF dilation), italic (shear), underline, strikethrough
//   <color=#ff8800> <color=red>        text color (hex or a CSS color name)
//   <alpha=#80> <alpha=0.5>            opacity
//   <size=24> <size=150%> <size=+4>    font size (absolute, relative or delta)
//   <font=serif>                       font (name or path)
//   <br>                               line break       <noparse>...</noparse>  literal text
// Every tag closes with </tag> (closing restores the previous value).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/math/Math.h"
#include "skywalker/render/Render2D.h"
#include "skywalker/text/Font.h"

namespace sky::text {

enum class Align { Left, Center, Right, Justify };
enum class VAlign { Top, Middle, Bottom };
Align alignFromString(std::string_view s);    // left | center | right | justify
VAlign valignFromString(std::string_view s);  // top | middle | bottom

struct TextStyle {
    std::string font;           // name or path ("" = default)
    float size = 16.f;          // em size in layout units (pixels for UI, world units for world text)
    Vec4 color{1, 1, 1, 1};     // straight alpha, color space of the caller (UI: sRGB)
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    float outline = 0.f;        // outline width in em (0 .. 0.15)
    Vec4 outlineColor{0, 0, 0, 1};
};

struct LayoutParams {
    TextStyle style;
    float maxWidth = 0.f;      // wrap width (0 = no wrapping)
    Align align = Align::Left;
    float lineSpacing = 1.f;   // line height multiplier
    float letterSpacing = 0.f; // extra advance in em
    bool richText = true;
    bool wrap = true;
};

struct GlyphQuad {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // layout space: origin at the text box top-left, y down
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    TextureImagePtr page;
    Vec4 color;
    Vec4 outlineColor;
    float outline = 0.f;  // SDF units (0 .. 0.45)
    float dilate = 0.f;   // SDF units (bold)
    float shear = 0.f;    // italic: x offset per unit of height above the bottom
    int charIndex = 0;    // index of the character in the visible text (typewriter reveal)
    int line = 0;
};

struct DecorationQuad {  // underline / strikethrough
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Vec4 color;
    int charIndex = 0;
};

struct TextLine {
    float top = 0, baseline = 0, height = 0;
    float x = 0, width = 0;  // after alignment
    int firstChar = 0, charCount = 0;
};

struct TextLayout {
    std::vector<GlyphQuad> glyphs;
    std::vector<DecorationQuad> decorations;
    std::vector<TextLine> lines;
    float width = 0;       // widest line
    float height = 0;      // all lines
    int charCount = 0;     // visible characters (typewriter length)
    std::string plainText; // text without tags
};

/// Lays out `text` (rich text unless disabled). Pure function of the inputs plus the fonts.
TextLayout layoutText(FontLibrary& fonts, std::string_view text, const LayoutParams& params);
/// Text with rich-text tags removed.
std::string stripTags(std::string_view text);
/// Number of visible characters (typewriter length) of a rich text.
int visibleLength(std::string_view text);
/// Parses CSS color names ("red", "gold", ...) and hex strings.
bool parseColor(std::string_view s, Vec4& out);

}  // namespace sky::text
