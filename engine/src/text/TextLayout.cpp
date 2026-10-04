#include "skywalker/text/TextLayout.h"

#include <algorithm>
#include <cmath>

#include "skywalker/core/Strings.h"
#include "skywalker/ecs/Reflection.h"

namespace sky::text {

Align alignFromString(std::string_view s) {
    if (s == "center") return Align::Center;
    if (s == "right") return Align::Right;
    if (s == "justify") return Align::Justify;
    return Align::Left;
}

VAlign valignFromString(std::string_view s) {
    if (s == "middle" || s == "center") return VAlign::Middle;
    if (s == "bottom") return VAlign::Bottom;
    return VAlign::Top;
}

bool parseColor(std::string_view s, Vec4& out) {
    if (!s.empty() && s.front() == '"' && s.back() == '"' && s.size() >= 2) s = s.substr(1, s.size() - 2);
    if (!s.empty() && s[0] == '#') return reflect::parseHexColor(s, out);
    struct Named {
        const char* name;
        const char* hex;
    };
    static const Named kNames[] = {
        {"white", "#ffffff"},  {"black", "#000000"},  {"red", "#ff3b30"},    {"green", "#34c759"},
        {"blue", "#0a84ff"},   {"yellow", "#ffd60a"}, {"orange", "#ff9f0a"}, {"purple", "#bf5af2"},
        {"pink", "#ff6482"},   {"cyan", "#64d2ff"},   {"teal", "#30b0c7"},   {"gray", "#8e8e93"},
        {"grey", "#8e8e93"},   {"gold", "#e6b422"},   {"silver", "#c0c0c8"}, {"brown", "#a2845e"},
        {"crimson", "#c8102e"}, {"lime", "#a4e400"},  {"navy", "#1c2a5a"},   {"magenta", "#ff2d92"},
    };
    std::string lowered = str::lower(s);
    for (const auto& n : kNames) {
        if (lowered == n.name) return reflect::parseHexColor(n.hex, out);
    }
    return false;
}

namespace {

struct Run {
    uint32_t cp = 0;
    int style = 0;  // index into styles
};

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

/// Applies one tag to a style. Returns false if the tag is not recognized.
bool applyTag(std::string_view name, std::string_view value, TextStyle& st) {
    if (ieq(name, "b")) return st.bold = true;
    if (ieq(name, "i")) return st.italic = true;
    if (ieq(name, "u")) return st.underline = true;
    if (ieq(name, "s")) return st.strike = true;
    if (ieq(name, "color")) {
        Vec4 c;
        if (!parseColor(value, c)) return false;
        st.color = {c.x, c.y, c.z, st.color.w * c.w};
        return true;
    }
    if (ieq(name, "alpha")) {
        Vec4 c;
        double a = 0;
        if (!value.empty() && value[0] == '#' && value.size() == 3) {
            std::string hex = "#000000" + std::string(value.substr(1));
            if (!reflect::parseHexColor(hex, c)) return false;
            st.color.w = c.w;
            return true;
        }
        if (str::parseDouble(value, a)) {
            st.color.w = static_cast<float>(std::clamp(a, 0.0, 1.0));
            return true;
        }
        return false;
    }
    if (ieq(name, "size")) {
        if (value.empty()) return false;
        double v = 0;
        if (value.back() == '%') {
            if (!str::parseDouble(value.substr(0, value.size() - 1), v)) return false;
            st.size = std::max(0.01f, st.size * static_cast<float>(v / 100.0));
        } else if (value[0] == '+' || value[0] == '-') {
            if (!str::parseDouble(value.substr(1), v)) return false;
            st.size = std::max(0.01f, st.size + static_cast<float>(value[0] == '-' ? -v : v));
        } else {
            if (!str::parseDouble(value, v)) return false;
            st.size = std::max(0.01f, static_cast<float>(v));
        }
        return true;
    }
    if (ieq(name, "font")) {
        std::string_view v = value;
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        st.font = std::string(v);
        return true;
    }
    return false;
}

/// Rich text -> code points with style indices. Unknown or malformed tags stay literal.
void parse(std::string_view text, const TextStyle& base, bool rich, std::vector<Run>& runs, std::vector<TextStyle>& styles) {
    styles.push_back(base);
    struct Open {
        std::string name;
        int style;  // style index before the tag
    };
    std::vector<Open> stack;
    int cur = 0;
    bool noparse = false;
    size_t i = 0;
    while (i < text.size()) {
        if (rich && text[i] == '<') {
            size_t close = text.find('>', i + 1);
            if (close != std::string_view::npos && close - i <= 64) {
                std::string_view body = text.substr(i + 1, close - i - 1);
                bool closing = !body.empty() && body[0] == '/';
                if (closing) body.remove_prefix(1);
                std::string_view name = body, value;
                if (size_t eq = body.find('='); eq != std::string_view::npos) {
                    name = body.substr(0, eq);
                    value = body.substr(eq + 1);
                }
                std::string lname = str::lower(str::trim(name));
                if (noparse) {
                    if (closing && lname == "noparse") {
                        noparse = false;
                        i = close + 1;
                        continue;
                    }
                } else if (lname == "noparse" && !closing) {
                    noparse = true;
                    i = close + 1;
                    continue;
                } else if ((lname == "br" || lname == "br/") && !closing) {
                    runs.push_back({'\n', cur});
                    i = close + 1;
                    continue;
                } else if (closing) {
                    // Pop to the most recent matching tag; a stray closing tag of a known kind is dropped.
                    bool known = lname == "b" || lname == "i" || lname == "u" || lname == "s" || lname == "color" ||
                                 lname == "size" || lname == "font" || lname == "alpha";
                    for (size_t k = stack.size(); k-- > 0;) {
                        if (stack[k].name == lname) {
                            cur = stack[k].style;
                            stack.resize(k);
                            known = true;
                            break;
                        }
                    }
                    if (known) {
                        i = close + 1;
                        continue;
                    }
                } else {
                    TextStyle st = styles[static_cast<size_t>(cur)];
                    if (applyTag(lname, str::trim(value), st)) {
                        stack.push_back({lname, cur});
                        styles.push_back(st);
                        cur = static_cast<int>(styles.size() - 1);
                        i = close + 1;
                        continue;
                    }
                }
            }
        }
        runs.push_back({decodeUtf8(text, i), cur});
    }
}

bool isSpace(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000; }
bool isCjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF00 && c <= 0xFFEF);
}

struct Shaped {
    uint32_t cp = 0;
    int style = 0;
    Font* font = nullptr;
    const Glyph* glyph = nullptr;
    float advance = 0;  // including kerning with the previous character and letter spacing
    float size = 0;
};

}  // namespace

std::string stripTags(std::string_view text) {
    std::vector<Run> runs;
    std::vector<TextStyle> styles;
    parse(text, TextStyle{}, true, runs, styles);
    std::string out;
    for (const auto& r : runs) appendUtf8(out, r.cp);
    return out;
}

int visibleLength(std::string_view text) {
    std::vector<Run> runs;
    std::vector<TextStyle> styles;
    parse(text, TextStyle{}, true, runs, styles);
    return static_cast<int>(runs.size());
}

TextLayout layoutText(FontLibrary& fonts, std::string_view text, const LayoutParams& p) {
    TextLayout out;
    std::vector<Run> runs;
    std::vector<TextStyle> styles;
    parse(text, p.style, p.richText, runs, styles);
    for (const auto& r : runs) appendUtf8(out.plainText, r.cp);
    out.charCount = static_cast<int>(runs.size());

    // Resolve fonts per style once.
    std::vector<Font*> styleFonts(styles.size(), nullptr);
    std::vector<std::shared_ptr<Font>> keepAlive;
    for (size_t s = 0; s < styles.size(); ++s) {
        auto f = fonts.get(styles[s].font);
        styleFonts[s] = f.get();
        keepAlive.push_back(std::move(f));
    }
    if (!styleFonts.empty() && !styleFonts[0]) return out;  // no font at all

    // Shape: glyph metrics, kerning, fallback fonts.
    std::vector<Shaped> shaped(runs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
        Shaped& sh = shaped[i];
        sh.cp = runs[i].cp;
        sh.style = runs[i].style;
        const TextStyle& st = styles[static_cast<size_t>(sh.style)];
        sh.size = st.size;
        Font* f = styleFonts[static_cast<size_t>(sh.style)];
        if (sh.cp == '\n') {
            sh.font = f;
            continue;
        }
        if (f && !f->hasGlyph(sh.cp) && !isSpace(sh.cp)) f = fonts.fallbackFor(sh.cp, f);
        sh.font = f;
        if (!f) continue;
        uint32_t cp = sh.cp == '\t' ? ' ' : sh.cp;
        sh.glyph = &f->glyph(cp);
        float adv = sh.glyph->advance * (sh.cp == '\t' ? 4.f : 1.f);
        if (i > 0 && shaped[i - 1].font == f && shaped[i - 1].cp != '\n') adv += f->kerning(shaped[i - 1].cp, cp);
        if (st.bold) adv += 0.02f;
        sh.advance = (adv + p.letterSpacing) * st.size;
    }

    // Break into lines: [start, end) ranges of `shaped`, greedy, preferring breaks after spaces.
    struct LineRange {
        size_t start, end;
        bool hardBreak;
    };
    std::vector<LineRange> lineRanges;
    const bool wrap = p.wrap && p.maxWidth > 0.f;
    size_t lineStart = 0;
    float x = 0;
    size_t lastBreak = SIZE_MAX;  // index after which the line may break
    for (size_t i = 0; i < shaped.size(); ++i) {
        if (shaped[i].cp == '\n') {
            lineRanges.push_back({lineStart, i, true});
            lineStart = i + 1;
            x = 0;
            lastBreak = SIZE_MAX;
            continue;
        }
        x += shaped[i].advance;
        bool space = isSpace(shaped[i].cp);
        if (wrap && x > p.maxWidth && !space && i > lineStart) {
            size_t breakAt = lastBreak != SIZE_MAX && lastBreak >= lineStart ? lastBreak + 1 : i;
            lineRanges.push_back({lineStart, breakAt, false});
            lineStart = breakAt;
            while (lineStart < shaped.size() && isSpace(shaped[lineStart].cp) && lineStart < i) ++lineStart;
            x = 0;
            for (size_t k = lineStart; k <= i; ++k) x += shaped[k].advance;
            lastBreak = SIZE_MAX;
        }
        if (space || shaped[i].cp == '-' || isCjk(shaped[i].cp) || (i + 1 < shaped.size() && isCjk(shaped[i + 1].cp))) lastBreak = i;
    }
    lineRanges.push_back({lineStart, shaped.size(), true});

    // Vertical metrics and placement.
    float y = 0;
    const float spacing = std::max(0.1f, p.lineSpacing);
    for (size_t li = 0; li < lineRanges.size(); ++li) {
        const LineRange& lr = lineRanges[li];
        float asc = 0, desc = 0, gap = 0;
        auto addMetrics = [&](Font* f, float size) {
            if (!f) return;
            asc = std::max(asc, f->ascent() * size);
            desc = std::max(desc, f->descent() * size);
            gap = std::max(gap, f->lineGap() * size);
        };
        for (size_t i = lr.start; i < lr.end; ++i) addMetrics(shaped[i].font, shaped[i].size);
        if (asc == 0 && desc == 0) {  // empty line: metrics of the style at that point
            size_t at = std::min(lr.start, shaped.empty() ? 0 : shaped.size() - 1);
            int style = shaped.empty() ? 0 : shaped[at].style;
            addMetrics(styleFonts[static_cast<size_t>(style)], styles[static_cast<size_t>(style)].size);
        }
        float natural = asc + desc + gap;
        float height = natural * spacing;
        TextLine line;
        line.top = y;
        line.height = height;
        line.baseline = y + (height - (asc + desc)) * 0.5f + asc;
        line.firstChar = static_cast<int>(lr.start);
        line.charCount = static_cast<int>(lr.end - lr.start);
        // Width without trailing spaces.
        float w = 0, trimmed = 0;
        for (size_t i = lr.start; i < lr.end; ++i) {
            w += shaped[i].advance;
            if (!isSpace(shaped[i].cp)) trimmed = w;
        }
        line.width = trimmed;
        out.lines.push_back(line);
        y += height;
    }
    out.height = y;
    for (const auto& l : out.lines) out.width = std::max(out.width, l.width);
    const float boxWidth = p.maxWidth > 0.f ? p.maxWidth : out.width;

    // Emit glyph quads.
    for (size_t li = 0; li < lineRanges.size(); ++li) {
        const LineRange& lr = lineRanges[li];
        TextLine& line = out.lines[li];
        float extraPerSpace = 0;
        if (p.align == Align::Center) line.x = (boxWidth - line.width) * 0.5f;
        else if (p.align == Align::Right) line.x = boxWidth - line.width;
        else if (p.align == Align::Justify && !lr.hardBreak && wrap) {
            int spaces = 0;
            size_t lastVisible = lr.start;
            for (size_t i = lr.start; i < lr.end; ++i) {
                if (!isSpace(shaped[i].cp)) lastVisible = i;
            }
            for (size_t i = lr.start; i < lastVisible; ++i) spaces += isSpace(shaped[i].cp) ? 1 : 0;
            if (spaces > 0) extraPerSpace = (boxWidth - line.width) / static_cast<float>(spaces);
        }
        float pen = line.x;
        for (size_t i = lr.start; i < lr.end; ++i) {
            const Shaped& sh = shaped[i];
            const TextStyle& st = styles[static_cast<size_t>(sh.style)];
            if (sh.glyph && sh.glyph->page >= 0 && sh.font) {
                const Glyph& g = *sh.glyph;
                // Kerning shifts this glyph; its own advance follows.
                float kern = sh.advance - (g.advance + p.letterSpacing + (st.bold ? 0.02f : 0.f)) * sh.size;
                float gx = pen + kern;
                GlyphQuad q;
                q.x0 = gx + g.x0 * sh.size;
                q.x1 = gx + g.x1 * sh.size;
                q.y0 = line.baseline + g.y0 * sh.size;
                q.y1 = line.baseline + g.y1 * sh.size;
                q.u0 = g.u0;
                q.v0 = g.v0;
                q.u1 = g.u1;
                q.v1 = g.v1;
                q.page = sh.font->pages()[static_cast<size_t>(g.page)];
                q.color = st.color;
                q.outlineColor = st.outlineColor;
                // SDF value v = 0.5 + d / (2 * spread) with d in atlas pixels (kEmPixels per em).
                const float perEm = Font::kEmPixels / (2.f * static_cast<float>(Font::kSpread));
                q.outline = std::clamp(st.outline * perEm, 0.f, 0.45f);
                q.dilate = st.bold ? 0.035f * perEm : 0.f;
                q.shear = st.italic ? 0.2f : 0.f;
                q.charIndex = static_cast<int>(i);
                q.line = static_cast<int>(li);
                out.glyphs.push_back(q);
            }
            float adv = sh.advance + (isSpace(sh.cp) ? extraPerSpace : 0.f);
            if ((st.underline || st.strike) && sh.font && !(isSpace(sh.cp) && i + 1 == lr.end)) {
                float thick = std::max(0.06f * sh.size, 0.5f);
                if (st.underline) {
                    float uy = line.baseline + 0.12f * sh.size;
                    out.decorations.push_back({pen, uy, pen + adv, uy + thick, st.color, static_cast<int>(i)});
                }
                if (st.strike) {
                    float sy = line.baseline - 0.28f * sh.size;
                    out.decorations.push_back({pen, sy, pen + adv, sy + thick, st.color, static_cast<int>(i)});
                }
            }
            pen += adv;
        }
    }
    return out;
}

}  // namespace sky::text
