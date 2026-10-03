// UI layout: canvas scaling, anchors, stacks (row/column), grids, fit-to-content, scroll views.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/ui/UiSystem.h"

namespace sky::ui {

bool anchorPreset(const std::string& name, Vec2& mn, Vec2& mx, Vec2& pv) {
    struct P {
        const char* name;
        float a, b, c, d, px, py;
    };
    static const P kPresets[] = {
        {"top_left", 0, 0, 0, 0, 0, 0},           {"top", 0.5f, 0, 0.5f, 0, 0.5f, 0},
        {"top_right", 1, 0, 1, 0, 1, 0},          {"left", 0, 0.5f, 0, 0.5f, 0, 0.5f},
        {"center", 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f}, {"right", 1, 0.5f, 1, 0.5f, 1, 0.5f},
        {"bottom_left", 0, 1, 0, 1, 0, 1},        {"bottom", 0.5f, 1, 0.5f, 1, 0.5f, 1},
        {"bottom_right", 1, 1, 1, 1, 1, 1},       {"fill", 0, 0, 1, 1, 0.5f, 0.5f},
        {"top_stretch", 0, 0, 1, 0, 0.5f, 0},     {"middle_stretch", 0, 0.5f, 1, 0.5f, 0.5f, 0.5f},
        {"bottom_stretch", 0, 1, 1, 1, 0.5f, 1},  {"left_stretch", 0, 0, 0, 1, 0, 0.5f},
        {"center_stretch", 0.5f, 0, 0.5f, 1, 0.5f, 0.5f}, {"right_stretch", 1, 0, 1, 1, 1, 0.5f},
    };
    for (const auto& p : kPresets) {
        if (name == p.name) {
            mn = {p.a, p.b};
            mx = {p.c, p.d};
            pv = {p.px, p.py};
            return true;
        }
    }
    return false;
}

Vec4 contentPadding(const Node& n) {
    const Vec4& p = n.el.padding;
    if (p.x != 0.f || p.y != 0.f || p.z != 0.f || p.w != 0.f) return p;
    return n.style.padding;
}

const Node* Layout::find(EntityId e) const {
    auto it = index.find(e);
    return it == index.end() ? nullptr : &nodes[static_cast<size_t>(it->second)];
}

Rect Layout::screenRect(const Node& n) const {
    const CanvasInfo& c = canvases[static_cast<size_t>(n.canvas)];
    return c.canvas.mode == "world" ? n.rect : n.rect.scaled(c.scale);
}

bool UiSystem::interactiveWidget(const std::string& w) {
    return w == "button" || w == "toggle" || w == "slider" || w == "input" || w == "scroll";
}

text::TextLayout UiSystem::layoutText(const std::string& raw, const Style& st, float scale, float maxWidth, bool wrap) const {
    std::string txt = raw;
    if (st.textTransform == "uppercase" || st.textTransform == "lowercase") {
        bool upper = st.textTransform == "uppercase";
        bool inTag = false;
        for (char& ch : txt) {  // ASCII only; tags keep their case
            if (ch == '<') inTag = true;
            else if (ch == '>') inTag = false;
            else if (!inTag) ch = static_cast<char>(upper ? std::toupper(static_cast<unsigned char>(ch)) : std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    char key[160];
    std::snprintf(key, sizeof(key), "|%g|%g|%g|%d|%d|%d|%g|%g|%g", st.fontSize * scale, maxWidth, st.lineSpacing, wrap ? 1 : 0,
                  st.bold ? 1 : 0, st.italic ? 1 : 0, st.letterSpacing, st.textOutline, static_cast<double>(st.color.w));
    std::string cacheKey = txt + "|" + st.font + "|" + st.textAlign + key + reflect::toHexColor(st.color) +
                           reflect::toHexColor(st.textOutlineColor);
    if (auto it = textCache_.find(cacheKey); it != textCache_.end()) return it->second;
    text::LayoutParams p;
    p.style.font = st.font;
    p.style.size = st.fontSize * scale;
    p.style.color = st.color;
    p.style.bold = st.bold;
    p.style.italic = st.italic;
    p.style.outline = st.textOutline;
    p.style.outlineColor = st.textOutlineColor;
    p.maxWidth = maxWidth;
    p.wrap = wrap;
    p.align = text::alignFromString(st.textAlign);
    p.lineSpacing = st.lineSpacing;
    p.letterSpacing = st.letterSpacing;
    text::TextLayout t = text::layoutText(assets_.fonts(), txt, p);
    if (textCache_.size() > 1024) textCache_.clear();
    textCache_.emplace(cacheKey, t);
    return t;
}

namespace {

std::shared_ptr<const StyleSheet> sheetFor(render2d::Assets2D& assets, const UICanvas& c) {
    auto base = StyleSheet::theme(c.theme);
    if (c.styleSheet.empty()) return base;
    struct Cached {
        int64_t mtime = -2;
        std::string theme;
        std::shared_ptr<const StyleSheet> sheet;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Cached> cache;
    std::string path = assets.resolve(c.styleSheet);
    std::lock_guard lock(mutex);
    Cached& entry = cache[path];
    int64_t m = render2d::fileMTime(path);
    if (m == entry.mtime && entry.theme == c.theme && entry.sheet) return entry.sheet;
    entry.mtime = m;
    entry.theme = c.theme;
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    Result<std::shared_ptr<const StyleSheet>> sheet = Error::make("io_error", "cannot read style sheet " + c.styleSheet);
    if (f && doc) sheet = StyleSheet::parse(doc.value(), base);
    else if (f) sheet = Error::make("invalid_style", c.styleSheet + ": " + doc.error().message);
    if (!sheet) {
        log::warn("ui", sheet.error().message + (sheet.error().hint.empty() ? "" : " (" + sheet.error().hint + ")"));
        entry.sheet = base;
    } else {
        entry.sheet = sheet.value();
    }
    return entry.sheet;
}

struct Builder {
    const Scene& scene;
    const UiSystem& sys;
    Layout& out;
    std::function<StyleState(EntityId, const UIElement&)> stateFor;

    bool fitW(const Node& n) const { return n.el.fit == "width" || n.el.fit == "both"; }
    bool fitH(const Node& n) const { return n.el.fit == "height" || n.el.fit == "both"; }

    std::string textOf(const Node& n) const {
        if (n.el.widget == "input" && n.el.text.empty()) return n.el.placeholder;
        return n.el.text;
    }

    /// Size of the element's own content (text, image, switch) without children, padding included.
    Vec2 ownContent(const Node& n, float width) const {
        Vec4 pad = contentPadding(n);
        float padW = pad.y + pad.w, padH = pad.x + pad.z;
        const std::string& w = n.el.widget;
        std::string txt = textOf(n);
        Vec2 c{0, 0};
        if (!txt.empty() && w != "slider" && w != "progress" && w != "scroll" && w != "spacer" && w != "image") {
            float extra = 0;
            if (w == "toggle") extra = switchWidth(n) + 10.f;
            bool wrap = width > 0.f && !fitW(n);
            float maxW = wrap ? std::max(1.f, width - padW - extra) : 0.f;
            text::TextLayout t = sys.layoutText(txt, n.style, 1.f, maxW, wrap);
            c = {t.width + extra, t.height};
            if (w == "input") c.y = std::max(c.y, n.style.fontSize * 1.2f);
        } else if (w == "toggle") {
            c = {switchWidth(n), switchHeight(n)};
        } else if (w == "image" && !n.el.image.empty()) {
            render2d::FrameRef fr;
            std::string tex = n.el.image, frame;
            if (size_t hash = tex.find('#'); hash != std::string::npos) {
                frame = tex.substr(hash + 1);
                tex = tex.substr(0, hash);
            }
            if (sys.assets().frame(tex, frame, 1, 1, {}, fr)) c = {fr.sourceW, fr.sourceH};
        }
        if (w == "toggle") c.y = std::max(c.y, switchHeight(n));
        return {c.x + padW, c.y + padH};
    }

    static float switchHeight(const Node& n) { return std::max(16.f, n.style.fontSize * 1.15f); }
    static float switchWidth(const Node& n) { return switchHeight(n) * 1.8f; }

    std::vector<int> flowChildren(const Node& n) const {
        std::vector<int> kids;
        for (int c : n.children) {
            const Node& k = out.nodes[static_cast<size_t>(c)];
            if (k.el.visible && !k.el.ignoreLayout) kids.push_back(c);
        }
        return kids;
    }

    std::string flowOf(const Node& n) const {
        if (n.el.layout != "none") return n.el.layout;
        return n.el.widget == "scroll" ? "column" : "none";
    }

    /// Preferred size. `width` > 0 is the width the parent will give (text wraps to it); `force`
    /// imposes it even on fit-width elements (stretched column children, like CSS align: stretch).
    Vec2 pref(int i, float width = -1.f, bool force = false) {
        Node& n = out.nodes[static_cast<size_t>(i)];
        Vec2 size = n.el.size;
        force = force && width > 0.f;
        if (force) size.x = width;
        if (!fitW(n) && !fitH(n)) return size;
        if (fitW(n) && !force) width = -1.f;
        else if (width <= 0.f) width = size.x;
        Vec4 pad = contentPadding(n);
        Vec2 content = ownContent(n, width);
        std::vector<int> kids = flowChildren(n);
        const std::string flow = flowOf(n);
        float innerW = width > 0.f ? std::max(0.f, width - pad.y - pad.w) : -1.f;
        Vec2 kidsSize{0, 0};
        if (!n.children.empty()) {
            if (flow == "row") {
                float wsum = 0, hmax = 0;
                for (int k : kids) {
                    Vec2 s = pref(k);
                    wsum += s.x;
                    hmax = std::max(hmax, s.y);
                }
                wsum += n.el.gap * static_cast<float>(std::max<size_t>(kids.size(), 1) - 1);
                kidsSize = {wsum, hmax};
            } else if (flow == "column") {
                float wmax = 0, hsum = 0;
                const bool stretch = n.el.align == "stretch";
                for (int k : kids) {
                    Vec2 s = pref(k, stretch && innerW > 0.f ? innerW : -1.f, stretch);
                    wmax = std::max(wmax, s.x);
                    hsum += s.y;
                }
                hsum += n.el.gap * static_cast<float>(std::max<size_t>(kids.size(), 1) - 1);
                kidsSize = {wmax, hsum};
            } else if (flow == "grid") {
                int cols = std::max(1, n.el.columns);
                float cellW = 0;
                std::vector<Vec2> sizes;
                for (int k : kids) {
                    sizes.push_back(pref(k));
                    cellW = std::max(cellW, sizes.back().x);
                }
                if (innerW > 0.f) cellW = (innerW - n.el.gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
                float h = 0;
                int rows = 0;
                for (size_t r = 0; r < sizes.size(); r += static_cast<size_t>(cols)) {
                    float rowH = 0;
                    for (size_t c = r; c < std::min(sizes.size(), r + static_cast<size_t>(cols)); ++c) rowH = std::max(rowH, sizes[c].y);
                    h += rowH;
                    ++rows;
                }
                h += n.el.gap * static_cast<float>(std::max(rows, 1) - 1);
                kidsSize = {cellW * static_cast<float>(cols) + n.el.gap * static_cast<float>(cols - 1), h};
            } else {
                // Absolute children: the extent of the top-left anchored ones.
                for (int k : n.children) {
                    const Node& kn = out.nodes[static_cast<size_t>(k)];
                    if (!kn.el.visible) continue;
                    Vec2 s = pref(k);
                    kidsSize.x = std::max(kidsSize.x, kn.el.position.x + s.x);
                    kidsSize.y = std::max(kidsSize.y, kn.el.position.y + s.y);
                }
            }
            kidsSize = {kidsSize.x + pad.y + pad.w, kidsSize.y + pad.x + pad.z};
        }
        Vec2 fit{std::max(content.x, kidsSize.x), std::max(content.y, kidsSize.y)};
        if (fitW(n) && !force) size.x = fit.x;
        if (fitH(n)) size.y = fit.y;
        return size;
    }

    /// Places a child that is not in a flow (anchors relative to the parent's content rect).
    Rect anchored(int i, const Rect& parent) {
        Node& n = out.nodes[static_cast<size_t>(i)];
        Vec2 mn = n.el.anchorMin, mx = n.el.anchorMax, pv = n.el.pivot;
        if (n.el.anchor != "custom") anchorPreset(n.el.anchor, mn, mx, pv);
        Rect r;
        const Vec4& m = n.el.margin;  // [top, right, bottom, left]
        bool stretchX = mx.x != mn.x, stretchY = mx.y != mn.y;
        float width = stretchX ? (parent.x + mx.x * parent.w - m.y) - (parent.x + mn.x * parent.w + m.w) : -1.f;
        Vec2 p = pref(i, width);
        if (stretchX) {
            r.x = parent.x + mn.x * parent.w + m.w + n.el.position.x;
            r.w = std::max(0.f, width);
        } else {
            r.w = p.x;
            r.x = parent.x + mn.x * parent.w + n.el.position.x - pv.x * r.w;
        }
        if (stretchY) {
            float top = parent.y + mn.y * parent.h + m.x, bottom = parent.y + mx.y * parent.h - m.z;
            r.y = top + n.el.position.y;
            r.h = std::max(0.f, bottom - top);
        } else {
            r.h = p.y;
            r.y = parent.y + mn.y * parent.h + n.el.position.y - pv.y * r.h;
        }
        return r;
    }

    void arrange(int i, Rect rect, Rect clip, float opacity, bool parentVisible, bool parentInteractable) {
        Node& n = out.nodes[static_cast<size_t>(i)];
        n.rect = rect;
        n.clip = clip;
        n.visible = parentVisible && n.el.visible;
        n.interactable = parentInteractable && n.el.interactable;
        n.opacity = opacity * std::clamp(n.style.opacity, 0.f, 1.f);
        Rect content = rect.inset(contentPadding(n));
        const std::string flow = flowOf(n);
        const bool scroll = n.el.widget == "scroll";
        Rect childClip = clip;
        if (scroll) {
            // Intersect the clip with the scroll viewport.
            Rect c = content;
            if (clip.w >= 0) {
                float x0 = std::max(c.x, clip.x), y0 = std::max(c.y, clip.y);
                float x1 = std::min(c.x + c.w, clip.x + clip.w), y1 = std::min(c.y + c.h, clip.y + clip.h);
                c = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
            }
            childClip = c;
        }
        std::vector<int> kids = flowChildren(n);
        std::vector<Rect> placed(n.children.size());
        auto slot = [&](int child) {
            return static_cast<size_t>(std::find(n.children.begin(), n.children.end(), child) - n.children.begin());
        };
        float contentExtent = 0;
        if (flow == "row" || flow == "column") {
            const bool row = flow == "row";
            const float mainSize = row ? content.w : content.h, crossSize = row ? content.h : content.w;
            std::vector<Vec2> sizes;
            float used = 0, flexTotal = 0;
            for (int k : kids) {
                const Node& kn = out.nodes[static_cast<size_t>(k)];
                // align: stretch fills the cross axis even for fit-to-content children (text then wraps to it).
                bool stretchCross = n.el.align == "stretch";
                Vec2 s = pref(k, !row && stretchCross ? crossSize : -1.f, !row && stretchCross);
                if (!row && stretchCross) s.x = crossSize;
                if (row && stretchCross) s.y = crossSize;
                sizes.push_back(s);
                used += row ? s.x : s.y;
                flexTotal += kn.el.flex;
            }
            const float gaps = n.el.gap * static_cast<float>(std::max<size_t>(kids.size(), 1) - 1);
            float leftover = mainSize - used - gaps;
            if (scroll) leftover = 0;
            float offset = 0, between = n.el.gap;
            if (flexTotal > 0.f && leftover > 0.f) {
                for (size_t j = 0; j < kids.size(); ++j) {
                    float share = leftover * out.nodes[static_cast<size_t>(kids[j])].el.flex / flexTotal;
                    (row ? sizes[j].x : sizes[j].y) += share;
                    // A flexed column child that fits its height may need its text re-wrapped: width is unchanged.
                }
                leftover = 0;
            }
            if (leftover > 0.f) {
                if (n.el.justify == "center") offset = leftover * 0.5f;
                else if (n.el.justify == "end") offset = leftover;
                else if (n.el.justify == "space_between" && kids.size() > 1) between += leftover / static_cast<float>(kids.size() - 1);
            }
            float cursor = offset - (scroll ? n.el.scroll : 0.f);
            for (size_t j = 0; j < kids.size(); ++j) {
                Vec2 s = sizes[j];
                float crossLen = row ? s.y : s.x;
                float crossPos = 0;
                if (n.el.align == "center") crossPos = (crossSize - crossLen) * 0.5f;
                else if (n.el.align == "end") crossPos = crossSize - crossLen;
                Rect r = row ? Rect{content.x + cursor, content.y + crossPos, s.x, s.y}
                             : Rect{content.x + crossPos, content.y + cursor, s.x, s.y};
                const Node& kn = out.nodes[static_cast<size_t>(kids[j])];
                r.x += kn.el.position.x;
                r.y += kn.el.position.y;
                placed[slot(kids[j])] = r;
                cursor += (row ? s.x : s.y) + between;
            }
            contentExtent = cursor - between + (scroll ? n.el.scroll : 0.f) - offset;
            n.contentSize = row ? Vec2{contentExtent, crossSize} : Vec2{crossSize, contentExtent};
        } else if (flow == "grid") {
            int cols = std::max(1, n.el.columns);
            float cellW = (content.w - n.el.gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
            float y = -(scroll ? n.el.scroll : 0.f);
            for (size_t r0 = 0; r0 < kids.size(); r0 += static_cast<size_t>(cols)) {
                float rowH = 0;
                size_t r1 = std::min(kids.size(), r0 + static_cast<size_t>(cols));
                for (size_t j = r0; j < r1; ++j) rowH = std::max(rowH, pref(kids[j], cellW).y);
                for (size_t j = r0; j < r1; ++j) {
                    float x = static_cast<float>(j - r0) * (cellW + n.el.gap);
                    placed[slot(kids[j])] = {content.x + x, content.y + y, cellW, rowH};
                }
                y += rowH + n.el.gap;
            }
            contentExtent = y - n.el.gap + (scroll ? n.el.scroll : 0.f);
            n.contentSize = {content.w, contentExtent};
        }
        if (scroll) {
            n.maxScroll = std::max(0.f, contentExtent - content.h);
        }
        for (size_t j = 0; j < n.children.size(); ++j) {
            int k = n.children[j];
            const Node& kn = out.nodes[static_cast<size_t>(k)];
            bool inFlow = flow != "none" && kn.el.visible && !kn.el.ignoreLayout;
            Rect r = inFlow ? placed[j] : anchored(k, content);
            arrange(k, r, childClip, n.opacity, n.visible, n.interactable);
        }
    }
};

}  // namespace

Layout UiSystem::computeLayout(const Scene& scene, int width, int height) const {
    Layout out;
    out.width = std::max(1, width);
    out.height = std::max(1, height);
    // Children of every entity, once (scene order).
    std::unordered_map<EntityId, std::vector<EntityId>> kids;
    std::vector<EntityId> canvases;
    for (EntityId e : scene.entities()) {
        const EntityRecord* r = scene.record(e);
        kids[r->parent].push_back(e);
        if (scene.get<UICanvas>(e) && scene.isActive(e)) canvases.push_back(e);
    }
    std::stable_sort(canvases.begin(), canvases.end(), [&](EntityId a, EntityId b) {
        return scene.get<UICanvas>(a)->sortOrder < scene.get<UICanvas>(b)->sortOrder;
    });
    Builder b{scene, *this, out, [this](EntityId e, const UIElement& el) { return stateFor(e, el); }};
    for (EntityId ce : canvases) {
        CanvasInfo ci;
        ci.entity = ce;
        ci.canvas = *scene.get<UICanvas>(ce);
        ci.sheet = sheetFor(assets_, ci.canvas);
        const Vec2 ref{std::max(1.f, ci.canvas.referenceResolution.x), std::max(1.f, ci.canvas.referenceResolution.y)};
        if (ci.canvas.mode == "world") {
            ci.scale = 1.f;
            ci.size = ref;
            Mat4 world = scene.worldMatrix(ce);
            float ws = ci.canvas.worldScale;
            // canvas units (y down, origin top-left) -> canvas entity space (centered, y up)
            Mat4 local = Mat4::translate({-ref.x * 0.5f * ws, ref.y * 0.5f * ws, 0.f}) * Mat4::scale({ws, -ws, ws});
            ci.model = world * local;
        } else {
            if (ci.canvas.scaleMode == "constant") {
                ci.scale = 1.f;
            } else {
                float lw = std::log(static_cast<float>(out.width) / ref.x), lh = std::log(static_cast<float>(out.height) / ref.y);
                ci.scale = std::exp(lw + (lh - lw) * std::clamp(ci.canvas.match, 0.f, 1.f));
            }
            ci.scale = std::max(0.01f, ci.scale);
            ci.size = {static_cast<float>(out.width) / ci.scale, static_cast<float>(out.height) / ci.scale};
        }
        const int canvasIndex = static_cast<int>(out.canvases.size());
        out.canvases.push_back(ci);
        // Depth-first collection of UI elements (draw order), resolving styles top-down.
        std::function<void(EntityId, int, int)> collect = [&](EntityId parentEntity, int parentNode, int depth) {
            for (EntityId e : kids[parentEntity]) {
                const UIElement* el = scene.get<UIElement>(e);
                if (!el) continue;
                Node n;
                n.entity = e;
                n.name = scene.record(e)->name;
                n.canvas = canvasIndex;
                n.parent = parentNode;
                n.depth = depth;
                n.el = *el;
                for (auto& c : str::split(el->style, ' ')) {
                    std::string t = str::trim(c);
                    if (!t.empty()) n.classes.push_back(t.front() == '.' ? t.substr(1) : t);
                }
                n.state = b.stateFor(e, *el);
                if (!scene.isActive(e)) n.el.visible = false;
                const StyleSheet& sheet = *out.canvases[static_cast<size_t>(canvasIndex)].sheet;
                const Style* parentStyle = parentNode >= 0 ? &out.nodes[static_cast<size_t>(parentNode)].style : nullptr;
                n.style = sheet.resolve(el->widget, n.classes, n.name, el->styleOverrides, n.state, parentStyle);
                StyleState base = n.state;
                base.hover = base.pressed = false;
                n.baseStyle = (base.hover == n.state.hover && base.pressed == n.state.pressed)
                                  ? n.style
                                  : sheet.resolve(el->widget, n.classes, n.name, el->styleOverrides, base, parentStyle);
                const int idx = static_cast<int>(out.nodes.size());
                out.nodes.push_back(std::move(n));
                out.index[e] = idx;
                if (parentNode >= 0) out.nodes[static_cast<size_t>(parentNode)].children.push_back(idx);
                else out.canvases[static_cast<size_t>(canvasIndex)].roots.push_back(idx);
                collect(e, idx, depth + 1);
            }
        };
        collect(ce, -1, 0);
        const CanvasInfo& c = out.canvases[static_cast<size_t>(canvasIndex)];
        Rect root{0, 0, c.size.x, c.size.y};
        for (int r : c.roots) b.arrange(r, b.anchored(r, root), {0, 0, -1, -1}, 1.f, true, c.canvas.interactable);
    }
    return out;
}

StyleState UiSystem::stateFor(EntityId e, const UIElement& el) const {
    StyleState s;
    s.hover = e == hovered_;
    s.pressed = e == pressed_ && e == hovered_;
    s.focus = e == focused_;
    s.checked = el.widget == "toggle" && el.value > 0.5f;
    s.disabled = !el.interactable;
    return s;
}

}  // namespace sky::ui
