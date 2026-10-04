// SDF fonts and text layout: atlases, kerning, UTF-8, wrapping, alignment, rich text.

#include <doctest/doctest.h>

#include "skywalker/text/TextLayout.h"

using namespace sky;
using namespace sky::text;

TEST_CASE("text: UTF-8 decoding and encoding") {
    std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // a é € 😀
    size_t i = 0;
    CHECK(decodeUtf8(s, i) == 'a');
    CHECK(decodeUtf8(s, i) == 0xE9);
    CHECK(decodeUtf8(s, i) == 0x20AC);
    CHECK(decodeUtf8(s, i) == 0x1F600);
    CHECK(i == s.size());
    std::string bad = "\xFF\xC3";  // invalid lead byte, truncated sequence
    i = 0;
    CHECK(decodeUtf8(bad, i) == 0xFFFD);
    CHECK(decodeUtf8(bad, i) == 0xFFFD);
    CHECK(i == bad.size());
    std::string out;
    for (uint32_t cp : {0x61u, 0xE9u, 0x20ACu, 0x1F600u}) appendUtf8(out, cp);
    CHECK(out == s);
}

TEST_CASE("text: font library resolves built-in names, aliases and errors") {
    FontLibrary lib;
    auto inter = lib.find("Inter");
    REQUIRE(inter.ok());
    CHECK(lib.find("sans").value() == inter.value());
    CHECK(lib.find("").value() == inter.value());
    auto serif = lib.find("serif");
    REQUIRE(serif.ok());
    CHECK(serif.value()->name() == "EB Garamond");
    auto mono = lib.find("JetBrains Mono");
    REQUIRE(mono.ok());
    auto bad = lib.find("Garamnod");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == "unknown_font");
    CHECK(bad.error().hint.find("EB Garamond") != std::string::npos);
    // get() falls back to the default font
    CHECK(lib.get("NoSuchFont") == inter.value());
    // Metrics are sane (em units).
    CHECK(inter.value()->ascent() > 0.7f);
    CHECK(inter.value()->descent() > 0.1f);
}

TEST_CASE("text: SDF glyphs rasterize lazily into atlas pages") {
    FontLibrary lib;
    auto font = lib.defaultFont();
    REQUIRE(font);
    size_t before = font->glyphCount();
    const Glyph& a = font->glyph('A');
    CHECK(font->glyphCount() == before + 1);
    CHECK(a.page == 0);
    CHECK(a.advance > 0.5f);
    CHECK(a.x1 > a.x0);
    CHECK(a.y0 < 0.f);  // above the baseline
    CHECK(a.u1 > a.u0);
    const TextureImage& page = *font->pages()[0];
    CHECK(page.channels == 1);
    CHECK(page.version > 0);
    // The SDF is inside (> 0.5) at the center of the stem area and outside at the padding.
    int px = static_cast<int>(a.u0 * Font::kPageSize), py = static_cast<int>(a.v0 * Font::kPageSize);
    CHECK(page.pixels[static_cast<size_t>(py) * Font::kPageSize + px] < 64);
    const Glyph& space = font->glyph(' ');
    CHECK(space.page == -1);
    CHECK(space.advance > 0.1f);
    // Kerning: "AV" pulls together in Inter.
    CHECK(font->kerning('A', 'V') < 0.f);
    CHECK(std::fabs(font->kerning('A', 'A')) < 0.03f);
    CHECK(font->kerning('T', 'o') < -0.03f);
}

TEST_CASE("text: layout widths, wrapping and alignment") {
    FontLibrary lib;
    LayoutParams p;
    p.style.size = 20;
    TextLayout one = layoutText(lib, "Hello world", p);
    REQUIRE(one.lines.size() == 1);
    CHECK(one.width > 80.f);
    CHECK(one.width < 140.f);
    CHECK(one.glyphs.size() == 10);  // the space has no quad
    CHECK(one.height == doctest::Approx(lib.defaultFont()->lineHeight() * 20.f).epsilon(0.01));
    CHECK(one.charCount == 11);

    p.maxWidth = 70;
    TextLayout wrapped = layoutText(lib, "Hello world, this wraps", p);
    CHECK(wrapped.lines.size() >= 3);
    for (const auto& l : wrapped.lines) CHECK(l.width <= 70.f + 0.01f);
    // Glyphs of a line never start left of the box.
    const float pad = static_cast<float>(Font::kSpread) / Font::kEmPixels * 20.f;  // SDF quads include the spread
    for (const auto& g : wrapped.glyphs) CHECK(g.x0 >= -pad - 2.f);

    // A word longer than the box breaks inside the word.
    TextLayout longWord = layoutText(lib, "Supercalifragilistic", p);
    CHECK(longWord.lines.size() >= 2);

    p.align = Align::Center;
    TextLayout centered = layoutText(lib, "Hi", p);
    REQUIRE(centered.lines.size() == 1);
    CHECK(centered.lines[0].x == doctest::Approx((70.f - centered.lines[0].width) * 0.5f));
    p.align = Align::Right;
    TextLayout right = layoutText(lib, "Hi", p);
    CHECK(right.lines[0].x + right.lines[0].width == doctest::Approx(70.f));

    // Explicit newlines and <br>.
    p.maxWidth = 0;
    p.align = Align::Left;
    CHECK(layoutText(lib, "a\nb\nc", p).lines.size() == 3);
    CHECK(layoutText(lib, "a<br>b", p).lines.size() == 2);
    // Empty lines keep their height.
    TextLayout blank = layoutText(lib, "a\n\nb", p);
    CHECK(blank.lines.size() == 3);
    CHECK(blank.lines[1].height > 0.f);
}

TEST_CASE("text: justify fills wrapped lines") {
    FontLibrary lib;
    LayoutParams p;
    p.style.size = 16;
    p.maxWidth = 120;
    p.align = Align::Justify;
    TextLayout t = layoutText(lib, "the quick brown fox jumps over the lazy dog", p);
    REQUIRE(t.lines.size() >= 2);
    // The last glyph of a non-final line reaches the right edge.
    int line0 = 0;
    float maxX = 0;
    for (const auto& g : t.glyphs) {
        if (g.line == line0) maxX = std::max(maxX, g.x1);
    }
    CHECK(maxX > 112.f);
}

TEST_CASE("text: rich text tags") {
    FontLibrary lib;
    LayoutParams p;
    p.style.size = 16;
    p.style.color = {1, 1, 1, 1};
    TextLayout t = layoutText(lib, "<color=#ff0000>R</color>G<size=200%>B</size><b>b</b><i>i</i><alpha=0.5>a</alpha>", p);
    REQUIRE(t.glyphs.size() == 6);
    CHECK(t.plainText == "RGBbia");
    CHECK(t.glyphs[0].color == Vec4{1, 0, 0, 1});
    CHECK(t.glyphs[1].color == Vec4{1, 1, 1, 1});
    CHECK((t.glyphs[2].y1 - t.glyphs[2].y0) > 1.6f * (t.glyphs[1].y1 - t.glyphs[1].y0));
    CHECK(t.glyphs[3].dilate > 0.f);
    CHECK(t.glyphs[1].dilate == 0.f);
    CHECK(t.glyphs[4].shear > 0.f);
    CHECK(t.glyphs[5].color.w == doctest::Approx(0.5f));
    // Named colors, nesting, unknown tags stay literal, stray closers vanish.
    TextLayout n = layoutText(lib, "<color=gold>x<color=red>y</color>z</color></b><foo>", p);
    CHECK(n.plainText == "xyz<foo>");
    CHECK(n.glyphs[0].color == n.glyphs[2].color);
    CHECK_FALSE(n.glyphs[0].color == n.glyphs[1].color);
    // Underline and strike create decorations; noparse keeps tags literal.
    TextLayout u = layoutText(lib, "<u>under</u> <s>x</s>", p);
    CHECK(u.decorations.size() >= 6);
    CHECK(layoutText(lib, "<noparse><b></noparse>", p).plainText == "<b>");
    // Rich text off: everything literal.
    p.richText = false;
    CHECK(layoutText(lib, "<b>x</b>", p).plainText == "<b>x</b>");
    CHECK(stripTags("<color=red>Hi</color> <b>there</b>") == "Hi there");
    CHECK(visibleLength("<b>abc</b>") == 3);
}

TEST_CASE("text: font fallback for missing glyphs and kerning in layout") {
    FontLibrary lib;
    LayoutParams p;
    p.style.size = 32;
    TextLayout av = layoutText(lib, "AV", p);
    TextLayout aa = layoutText(lib, "AA", p);
    REQUIRE(av.glyphs.size() == 2);
    // Kerning moves V closer to A than another A would be (relative to their own advances).
    auto font = lib.defaultFont();
    float kernedGap = av.glyphs[1].x0 - font->glyph('V').x0 * 32.f;
    float plainGap = aa.glyphs[1].x0 - font->glyph('A').x0 * 32.f;
    CHECK(kernedGap < plainGap - 0.5f);
    // Monospace font: every glyph has the same advance.
    p.style.font = "mono";
    TextLayout m = layoutText(lib, "iiWW", p);
    REQUIRE(m.glyphs.size() == 4);
    CHECK(m.width == doctest::Approx(4.f * lib.get("mono")->glyph('W').advance * 32.f).epsilon(0.01));
}
