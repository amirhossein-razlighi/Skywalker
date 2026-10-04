// UI drawing (quads), hit testing and input.

#include "skywalker/ui/UiSystem.h"

#include <algorithm>
#include <cmath>

#include "skywalker/core/Strings.h"

namespace sky::ui {

namespace {

void set4(float* d, float a, float b, float c, float e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}
void set4(float* d, Vec4 v) { set4(d, v.x, v.y, v.z, v.w); }

Vec4 mix(Vec4 a, Vec4 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}; }

/// Blends the colors of two styles (hover/pressed transitions).
Style blend(const Style& base, const Style& target, float t) {
    if (t >= 0.999f) return target;
    Style s = target;
    s.background = mix(base.background, target.background, t);
    s.background2 = mix(base.background2, target.background2, t);
    s.borderColor = mix(base.borderColor, target.borderColor, t);
    s.shadowColor = mix(base.shadowColor, target.shadowColor, t);
    s.color = mix(base.color, target.color, t);
    s.accent = mix(base.accent, target.accent, t);
    s.track = mix(base.track, target.track, t);
    s.knob = mix(base.knob, target.knob, t);
    s.imageTint = mix(base.imageTint, target.imageTint, t);
    s.shadowOffset = base.shadowOffset + (target.shadowOffset - base.shadowOffset) * t;
    return s;
}

struct Sink {
    Frame2D& f;
    int canvas = 0;
    std::vector<EntityId> boxesFor;

    void push(const UIQuad& q, const TextureRef& tex = {}, bool nearest = false) {
        UIBatch* last = f.uiBatches.empty() ? nullptr : &f.uiBatches.back();
        if (last && last->canvas == canvas && last->texture == tex && last->nearest == nearest &&
            last->first + last->count == f.ui.size()) {
            ++last->count;
        } else {
            UIBatch b;
            b.texture = tex;
            b.nearest = nearest;
            b.canvas = canvas;
            b.first = static_cast<uint32_t>(f.ui.size());
            b.count = 1;
            f.uiBatches.push_back(std::move(b));
        }
        f.ui.push_back(q);
    }
};

UIQuad baseQuad(const Rect& r, UIQuadKind kind, float opacity, const Rect& clip) {
    UIQuad q{};
    set4(q.rect, r.x, r.y, r.w, r.h);
    set4(q.uv, 0, 0, 1, 1);
    set4(q.params, static_cast<float>(kind), 0, 0, opacity);
    if (clip.w >= 0) set4(q.clip, clip.x, clip.y, clip.x + clip.w, clip.y + clip.h);
    else set4(q.clip, 0, 0, -1, -1);
    return q;
}

struct Emitter {
    const UiSystem& sys;
    render2d::Assets2D& assets;
    const Layout& lay;
    Sink& sink;
    float scale = 1;
    float time = 0;
    const std::unordered_map<EntityId, float>& blendMap;
    const std::unordered_map<EntityId, int>& reveal;
    EntityId focused = kNoEntity;
    const std::vector<EntityId>& selection;

    Rect out(const Rect& r) const { return r.scaled(scale); }

    void rect(const Rect& r, Vec4 c1, Vec4 c2, float radius, float bw, Vec4 bc, float opacity, const Rect& clip) {
        if (c1.w <= 0.f && c2.w <= 0.f && (bw <= 0.f || bc.w <= 0.f)) return;
        UIQuad q = baseQuad(r, UIQuadKind::Rect, opacity, clip);
        set4(q.color, c1);
        set4(q.color2, c2.w > 0.f ? c2 : c1);
        set4(q.borderColor, bc);
        q.params[1] = std::min(radius, std::min(r.w, r.h) * 0.5f);
        q.params[2] = bw;
        sink.push(q);
    }

    void shadow(const Rect& r, Vec4 color, Vec2 offset, float radius, float blur, float opacity, const Rect& clip) {
        if (color.w <= 0.f) return;
        Rect s{r.x + offset.x - blur, r.y + offset.y - blur, r.w + 2 * blur, r.h + 2 * blur};
        UIQuad q = baseQuad(s, UIQuadKind::Shadow, opacity, clip);
        set4(q.color, color);
        q.params[1] = std::min(radius, std::min(r.w, r.h) * 0.5f);
        q.params2[0] = blur;
        sink.push(q);
    }

    bool frame(const std::string& ref, render2d::FrameRef& fr) {
        std::string tex = ref, sel;
        if (size_t hash = tex.find('#'); hash != std::string::npos) {
            sel = tex.substr(hash + 1);
            tex = tex.substr(0, hash);
        }
        return assets.frame(tex, sel, 1, 1, {}, fr).ok() && !fr.path.empty();
    }

    void image(const Rect& r, const render2d::FrameRef& fr, float u0, float v0, float u1, float v1, Vec4 tint, float radius, float opacity,
               const Rect& clip, bool nearest) {
        UIQuad q = baseQuad(r, UIQuadKind::Image, opacity, clip);
        const float iw = 1.f / static_cast<float>(fr.texW), ih = 1.f / static_cast<float>(fr.texH);
        set4(q.uv, u0 * iw, v0 * ih, u1 * iw, v1 * ih);
        set4(q.color, tint);
        q.params[1] = std::min(radius, std::min(r.w, r.h) * 0.5f);
        TextureRef t;
        t.path = fr.path;
        sink.push(q, t, nearest);
    }

    /// Image fitted into `r` (stretch | contain | cover), or 9-sliced when `slice` is set.
    void fittedImage(const Rect& r, const std::string& ref, const std::string& fit, Vec4 slice, Vec4 tint, float radius, float opacity,
                     const Rect& clip, bool nearest = false, float sliceScale = 1.f) {
        render2d::FrameRef fr;
        if (!frame(ref, fr)) {
            // Missing image: a visible placeholder so the layout reads in captures.
            rect(r, {0.5f, 0.5f, 0.55f, 0.35f}, {}, radius, 1.f * scale, {1, 1, 1, 0.25f}, opacity, clip);
            return;
        }
        const bool sliced = slice.x > 0 || slice.y > 0 || slice.z > 0 || slice.w > 0;
        if (sliced) {
            // 9-slice: corners keep their size (in canvas units), edges and center stretch.
            const float xs[4] = {fr.x, fr.x + slice.w, fr.x + fr.w - slice.y, fr.x + fr.w};
            const float ys[4] = {fr.y, fr.y + slice.x, fr.y + fr.h - slice.z, fr.y + fr.h};
            const float k = scale * std::max(0.01f, sliceScale);  // pixel art: whole canvas pixels per image pixel
            float l = std::min(slice.w * k, r.w * 0.5f), rr = std::min(slice.y * k, r.w * 0.5f);
            float t = std::min(slice.x * k, r.h * 0.5f), b = std::min(slice.z * k, r.h * 0.5f);
            const float dx[4] = {r.x, r.x + l, r.x + r.w - rr, r.x + r.w};
            const float dy[4] = {r.y, r.y + t, r.y + r.h - b, r.y + r.h};
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) {
                    Rect cell{dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j]};
                    if (cell.w <= 0.f || cell.h <= 0.f) continue;
                    image(cell, fr, xs[i], ys[j], xs[i + 1], ys[j + 1], tint, 0.f, opacity, clip, nearest);
                }
            }
            return;
        }
        float u0 = fr.x, v0 = fr.y, u1 = fr.x + fr.w, v1 = fr.y + fr.h;
        Rect d = r;
        const float ia = fr.w / std::max(1.f, fr.h), ra = r.w / std::max(1e-3f, r.h);
        if (fit == "contain") {
            if (ia > ra) {
                d.h = r.w / ia;
                d.y = r.y + (r.h - d.h) * 0.5f;
            } else {
                d.w = r.h * ia;
                d.x = r.x + (r.w - d.w) * 0.5f;
            }
        } else if (fit == "cover") {
            if (ia > ra) {  // crop the sides
                float keep = fr.h * ra;
                u0 = fr.x + (fr.w - keep) * 0.5f;
                u1 = u0 + keep;
            } else {
                float keep = fr.w / ra;
                v0 = fr.y + (fr.h - keep) * 0.5f;
                v1 = v0 + keep;
            }
        }
        image(d, fr, u0, v0, u1, v1, tint, radius, opacity, clip, nearest);
    }

    /// Text inside a content rect. Returns the layout (for carets).
    text::TextLayout textBlock(const std::string& txt, const Style& st, const Rect& content, bool wrap, int revealChars, float opacity,
                               const Rect& clip, Vec4 colorOverride = {0, 0, 0, 0}) {
        Style s = st;
        if (colorOverride.w > 0.f) s.color = colorOverride;
        text::TextLayout t = sys.layoutText(txt, s, scale, std::max(1.f, content.w), wrap);
        float oy = 0;
        if (s.verticalAlign == "middle") oy = (content.h - t.height) * 0.5f;
        else if (s.verticalAlign == "bottom") oy = content.h - t.height;
        auto glyphs = [&](Vec2 shift, bool shadowPass) {
            for (const auto& g : t.glyphs) {
                if (revealChars >= 0 && g.charIndex >= revealChars) continue;
                Rect r{content.x + g.x0 + shift.x, content.y + oy + g.y0 + shift.y, g.x1 - g.x0, g.y1 - g.y0};
                UIQuad q = baseQuad(r, UIQuadKind::Glyph, opacity, clip);
                set4(q.uv, g.u0, g.v0, g.u1, g.v1);
                set4(q.color, shadowPass ? s.textShadowColor : g.color);
                set4(q.color2, g.outlineColor);
                q.params2[0] = g.dilate;
                q.params2[1] = shadowPass ? 0.f : g.outline;
                q.params2[2] = g.shear * (g.y1 - g.y0);
                TextureRef tex;
                tex.image = g.page;
                sink.push(q, tex);
            }
            for (const auto& d : t.decorations) {
                if (revealChars >= 0 && d.charIndex >= revealChars) continue;
                Rect r{content.x + d.x0 + shift.x, content.y + oy + d.y0 + shift.y, d.x1 - d.x0, d.y1 - d.y0};
                rect(r, shadowPass ? s.textShadowColor : d.color, {}, 0, 0, {}, opacity, clip);
            }
        };
        if (s.textShadowColor.w > 0.f) glyphs(s.textShadowOffset * scale, true);
        glyphs({0, 0}, false);
        return t;
    }

    static float frac(const UIElement& el) {
        float range = el.maxValue - el.minValue;
        return range != 0.f ? std::clamp((el.value - el.minValue) / range, 0.f, 1.f) : 0.f;
    }

    void node(int i, float /*parentOpacity*/) {
        const Node& n = lay.nodes[static_cast<size_t>(i)];
        if (!n.visible) return;
        auto bit = blendMap.find(n.entity);
        const Style st = blend(n.baseStyle, n.style, bit == blendMap.end() ? (n.state.hover || n.state.pressed ? 1.f : 0.f) : bit->second);
        const float op = n.opacity;
        const Rect r = out(n.rect);
        const Rect clip = n.clip.w >= 0 ? out(n.clip) : Rect{0, 0, -1, -1};
        const float radius = st.radius * scale;
        const std::string& w = n.el.widget;
        const Vec4 pad = contentPadding(n) * scale;
        const Rect content = r.inset(pad);

        if (w != "spacer") {
            shadow(r, st.shadowColor, st.shadowOffset * scale, radius, st.shadowBlur * scale, op, clip);
            if (!st.backgroundImage.empty()) {
                fittedImage(r, st.backgroundImage, "stretch", st.slice, st.imageTint, radius, op, clip, st.imageFilter == "nearest",
                            st.sliceScale);
                if (st.borderWidth > 0.f) rect(r, {}, {}, radius, st.borderWidth * scale, st.borderColor, op, clip);
            } else {
                rect(r, st.background, st.background2, radius, st.borderWidth * scale, st.borderColor, op, clip);
            }
        }
        auto rev = reveal.find(n.entity);
        const int revealChars = rev == reveal.end() ? -1 : rev->second;
        if (w == "image") {
            if (!n.el.image.empty()) {
                fittedImage(content, n.el.image, st.imageFit, {}, st.imageTint, radius, op, clip, st.imageFilter == "nearest");
            }
        } else if (w == "toggle") {
            float h = std::max(16.f, st.fontSize * 1.15f) * scale, sw = h * 1.8f;
            Rect track{content.x, content.y + (content.h - h) * 0.5f, sw, h};
            bool on = n.el.value > 0.5f;
            rect(track, on ? st.accent : st.track, {}, h * 0.5f, 0, {}, op, clip);
            float k = h - 4.f * scale;
            Rect knob{on ? track.x + track.w - k - 2.f * scale : track.x + 2.f * scale, track.y + 2.f * scale, k, k};
            shadow(knob, {0, 0, 0, 0.3f}, {0, 1.f * scale}, k * 0.5f, 2.f * scale, op, clip);
            rect(knob, st.knob, {}, k * 0.5f, 0, {}, op, clip);
            if (!n.el.text.empty()) {
                Rect label{track.x + sw + 10.f * scale, content.y, std::max(0.f, content.w - sw - 10.f * scale), content.h};
                textBlock(n.el.text, st, label, false, revealChars, op, clip);
            }
        } else if (w == "slider") {
            float th = st.trackHeight * scale, ks = st.knobSize * scale;
            Rect track{content.x + ks * 0.5f, content.y + (content.h - th) * 0.5f, std::max(0.f, content.w - ks), th};
            rect(track, st.track, {}, th * 0.5f, 0, {}, op, clip);
            float fx = frac(n.el);
            rect({track.x, track.y, track.w * fx, th}, st.accent, {}, th * 0.5f, 0, {}, op, clip);
            Rect knob{track.x + track.w * fx - ks * 0.5f, content.y + (content.h - ks) * 0.5f, ks, ks};
            shadow(knob, {0, 0, 0, 0.35f}, {0, 1.f * scale}, ks * 0.5f, 3.f * scale, op, clip);
            rect(knob, st.knob, {}, ks * 0.5f, n.state.focus ? 2.f * scale : 0.f, st.accent, op, clip);
        } else if (w == "progress") {
            float fx = frac(n.el);
            if (st.background.w <= 0.f) rect(content, st.track, {}, radius, 0, {}, op, clip);
            if (fx > 0.f) {
                Rect fill{content.x, content.y, std::max(content.w * fx, std::min(content.w, radius * 2.f)), content.h};
                rect(fill, st.accent, mix(st.accent, {0, 0, 0, st.accent.w}, 0.12f), radius, 0, {}, op, clip);
            }
            if (!n.el.text.empty()) textBlock(n.el.text, st, content, false, revealChars, op, clip);
        } else if (w == "input") {
            const bool empty = n.el.text.empty();
            Rect field = content;
            text::TextLayout t = textBlock(empty ? n.el.placeholder : n.el.text, st, field, false, revealChars, op, r,
                                           empty ? st.placeholderColor : Vec4{0, 0, 0, 0});
            if (focused == n.entity && static_cast<int>(time * 2.f) % 2 == 0) {
                float cx = field.x + (empty ? 0.f : t.width) + 1.f * scale;
                float ch = st.fontSize * 1.15f * scale;
                rect({cx, field.y + (field.h - ch) * 0.5f, std::max(1.f, 1.5f * scale), ch}, st.accent, {}, 0, 0, {}, op, clip);
            }
        } else if (!n.el.text.empty() && w != "scroll") {
            bool wrap = w == "text" || w == "panel";
            textBlock(n.el.text, st, content, wrap, revealChars, op, clip);
        }
        for (int c : n.children) node(c, op);
        if (w == "scroll" && n.maxScroll > 0.f) {
            float viewH = content.h, total = viewH + n.maxScroll * scale;
            float thumbH = std::max(24.f * scale, viewH * viewH / std::max(1.f, total));
            float t = std::clamp(n.el.scroll * scale / std::max(1e-3f, n.maxScroll * scale), 0.f, 1.f);
            Rect bar{r.x + r.w - 6.f * scale, content.y + (viewH - thumbH) * t, 4.f * scale, thumbH};
            rect(bar, st.track, {}, 2.f * scale, 0, {}, op, clip);
        }
        if (std::find(selection.begin(), selection.end(), n.entity) != selection.end()) {
            rect(r, {}, {}, radius, 2.f, {1.f, 0.55f, 0.1f, 1.f}, 1.f, {0, 0, -1, -1});
        }
    }
};

}  // namespace

void UiSystem::setReveal(EntityId e, int chars) { reveal_[e] = chars; }

void UiSystem::reset() {
    hovered_ = pressed_ = focused_ = dragging_ = kNoEntity;
    wasDown_ = false;
    blend_.clear();
    reveal_.clear();
}

void UiSystem::build(const Scene& scene, FrameData& frame, float time) {
    time_ = time;
    Layout lay = computeLayout(scene, frame.width, frame.height);
    Frame2D& f = frame.render2d;
    for (size_t ci = 0; ci < lay.canvases.size(); ++ci) {
        const CanvasInfo& c = lay.canvases[ci];
        const bool world = c.canvas.mode == "world";
        Sink sink{f, static_cast<int>(f.uiCanvases.size()), {}};
        f.uiCanvases.push_back({world, c.model});
        Emitter em{*this, assets_, lay, sink, world ? 1.f : c.scale, time, blend_, reveal_, focused_, selection_};
        for (int r : c.roots) em.node(r, 1.f);
    }
    // Screen boxes for agents (captures, picking): every visible element, topmost last in draw order.
    int64_t order = 1 << 30;
    for (const Node& n : lay.nodes) {
        if (!n.visible || lay.canvases[static_cast<size_t>(n.canvas)].canvas.mode == "world") continue;
        Rect r = lay.screenRect(n);
        ScreenBox b;
        b.entity = n.entity;
        b.x = r.x;
        b.y = r.y;
        b.w = r.w;
        b.h = r.h;
        b.order = ++order;
        b.ui = true;
        f.boxes.push_back(b);
    }
}

EntityId UiSystem::hitIn(const Layout& lay, float x, float y, const ViewCamera* camera, bool interactiveOnly) const {
    for (size_t ci = lay.canvases.size(); ci-- > 0;) {
        const CanvasInfo& c = lay.canvases[ci];
        if (!c.canvas.interactable) continue;
        float px = x, py = y;
        if (c.canvas.mode == "world") {
            if (!camera) continue;
            Ray ray = camera->rayAt(x, y, lay.width, lay.height);
            Mat4 inv = c.model.inverse();
            Vec3 o = inv.transformPoint(ray.origin), d = inv.transformDir(ray.dir);
            if (std::fabs(d.z) < 1e-6f) continue;
            float t = -o.z / d.z;
            if (t < 0) continue;
            px = o.x + d.x * t;
            py = o.y + d.y * t;
        } else {
            px /= c.scale;
            py /= c.scale;
        }
        // Nodes of this canvas, topmost (last drawn) first.
        for (size_t k = lay.nodes.size(); k-- > 0;) {
            const Node& n = lay.nodes[k];
            if (n.canvas != static_cast<int>(ci) || !n.visible || !n.rect.contains(px, py)) continue;
            if (n.clip.w >= 0 && !n.clip.contains(px, py)) continue;
            bool look = n.style.background.w > 0.01f || !n.style.backgroundImage.empty() || n.el.widget == "image" ||
                        (n.style.borderWidth > 0.f && n.style.borderColor.w > 0.01f);
            bool interactive = interactiveWidget(n.el.widget);
            if (!look && !interactive) continue;
            if (!interactiveOnly) return n.entity;
            for (int cur = static_cast<int>(k); cur >= 0; cur = lay.nodes[static_cast<size_t>(cur)].parent) {
                const Node& a = lay.nodes[static_cast<size_t>(cur)];
                if (interactiveWidget(a.el.widget) && a.interactable) return a.entity;
            }
            return kNoEntity;  // a panel covers the point: it consumes the pointer
        }
    }
    return kNoEntity;
}

EntityId UiSystem::hitTest(const Scene& scene, float x, float y, int width, int height, const ViewCamera* camera, bool interactiveOnly) const {
    Layout lay = computeLayout(scene, width, height);
    return hitIn(lay, x, y, camera, interactiveOnly);
}

void UiSystem::activate(Scene& scene, EntityId e, const UiEvents& ev) {
    UIElement* el = scene.get<UIElement>(e);
    const EntityRecord* rec = scene.record(e);
    if (!el || !rec || !el->interactable) return;
    if (ev.activate && ev.activate(e)) return;
    if (el->widget == "toggle") {
        el->value = el->value > 0.5f ? 0.f : 1.f;
        scene.markDirty();
    }
    if (ev.emit) {
        ev.emit("ui:" + rec->name, kNoEntity);
        if (!el->event.empty()) ev.emit(el->event, kNoEntity);
    }
    if (ev.click) ev.click(e);
}

void UiSystem::tick(Scene& scene, const UiInput& in, float dt, const ViewCamera* camera, const UiEvents& ev) {
    time_ += dt;
    const int w = in.width > 0 ? in.width : lastWidth_, h = in.height > 0 ? in.height : lastHeight_;
    lastWidth_ = w;
    lastHeight_ = h;
    Layout lay = computeLayout(scene, w, h);
    if (gate_) {  // canvases that do not run this tick (process mode) take no input
        for (auto& c : lay.canvases) {
            if (!gate_->runs(c.entity)) c.canvas.interactable = false;
        }
        for (auto& n : lay.nodes) {
            if (!lay.canvases[static_cast<size_t>(n.canvas)].canvas.interactable) n.interactable = false;
        }
    }
    const bool hasPointer = in.x >= 0.f && in.y >= 0.f;
    const EntityId target = hasPointer ? hitIn(lay, in.x, in.y, camera, true) : kNoEntity;
    hovered_ = target;
    const bool down = in.down && hasPointer;
    auto setValue = [&](EntityId e, float v) {
        if (UIElement* el = scene.get<UIElement>(e)) {
            float lo = std::min(el->minValue, el->maxValue), hi = std::max(el->minValue, el->maxValue);
            el->value = std::clamp(v, lo, hi);
            scene.markDirty();
        }
    };
    auto sliderFromPointer = [&](EntityId e) {
        const Node* n = lay.find(e);
        if (!n) return;
        const CanvasInfo& c = lay.canvases[static_cast<size_t>(n->canvas)];
        float px = c.canvas.mode == "world" ? in.x : in.x / c.scale;
        Rect content = n->rect.inset(contentPadding(*n));
        float ks = n->style.knobSize;
        float t = std::clamp((px - content.x - ks * 0.5f) / std::max(1.f, content.w - ks), 0.f, 1.f);
        setValue(e, n->el.minValue + (n->el.maxValue - n->el.minValue) * t);
    };
    if (down && !wasDown_) {
        pressed_ = target;
        const Node* n = target ? lay.find(target) : nullptr;
        focused_ = n && n->el.widget != "scroll" ? target : kNoEntity;
        if (n && n->el.widget == "slider") dragging_ = target;
    }
    if (down && dragging_) sliderFromPointer(dragging_);
    if (!down && wasDown_) {
        if (dragging_) {
            if (const EntityRecord* r = scene.record(dragging_); r && ev.emit) ev.emit("ui:" + r->name, kNoEntity);
            dragging_ = kNoEntity;
        } else if (pressed_ && pressed_ == target) {
            const Node* n = lay.find(pressed_);
            if (n && n->el.widget != "input" && n->el.widget != "scroll") activate(scene, pressed_, ev);
        }
        pressed_ = kNoEntity;
    }
    wasDown_ = down;

    if (in.wheel != 0.f && hasPointer) {
        // The innermost scroll view under the pointer.
        for (size_t k = lay.nodes.size(); k-- > 0;) {
            const Node& n = lay.nodes[k];
            if (n.el.widget != "scroll" || !n.visible) continue;
            const CanvasInfo& c = lay.canvases[static_cast<size_t>(n.canvas)];
            if (c.canvas.mode == "world" || !n.rect.contains(in.x / c.scale, in.y / c.scale)) continue;
            if (UIElement* el = scene.get<UIElement>(n.entity)) {
                el->scroll = std::clamp(el->scroll - in.wheel * 40.f, 0.f, n.maxScroll);
                scene.markDirty();
            }
            break;
        }
    }

    // Keyboard: focus navigation, activation, sliders, text input.
    std::vector<int> focusable;
    for (size_t k = 0; k < lay.nodes.size(); ++k) {
        const Node& n = lay.nodes[k];
        if (n.visible && n.interactable && interactiveWidget(n.el.widget) && n.el.widget != "scroll") focusable.push_back(static_cast<int>(k));
    }
    auto moveFocus = [&](int dir) {
        if (focusable.empty()) return;
        int cur = -1;
        for (size_t j = 0; j < focusable.size(); ++j) {
            if (lay.nodes[static_cast<size_t>(focusable[j])].entity == focused_) cur = static_cast<int>(j);
        }
        int n = static_cast<int>(focusable.size());
        int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : ((cur + dir) % n + n) % n;
        focused_ = lay.nodes[static_cast<size_t>(focusable[static_cast<size_t>(next)])].entity;
    };
    const Node* fn = focused_ ? lay.find(focused_) : nullptr;
    if (fn && (!fn->visible || !fn->interactable)) {
        focused_ = kNoEntity;
        fn = nullptr;
    }
    // Typed text first, then editing keys (so "abc" + backspace leaves "ab").
    if (fn && fn->el.widget == "input" && !in.text.empty()) {
        if (UIElement* el = scene.get<UIElement>(focused_)) {
            for (char ch : in.text) {
                if (static_cast<unsigned char>(ch) >= 0x20 && ch != 0x7F) el->text += ch;
            }
            scene.markDirty();
        }
    }
    for (const std::string& key : in.keys) {
        if (key == "tab") {
            moveFocus(1);
        } else if (!fn) {
            continue;
        } else if (key == "escape") {
            focused_ = kNoEntity;
        } else if ((key == "down" || key == "up") && fn->el.widget != "input") {
            moveFocus(key == "down" ? 1 : -1);
        } else if (key == "enter" || key == "return" || (key == "space" && fn->el.widget != "input")) {
            if (fn->el.widget == "input") {
                if (const EntityRecord* r = scene.record(focused_); r && ev.emit) ev.emit("ui:" + r->name, kNoEntity);
                focused_ = kNoEntity;
            } else {
                activate(scene, focused_, ev);
            }
        } else if ((key == "left" || key == "right") && fn->el.widget == "slider") {
            float step = (fn->el.maxValue - fn->el.minValue) * 0.05f;
            setValue(focused_, fn->el.value + (key == "right" ? step : -step));
            if (const EntityRecord* r = scene.record(focused_); r && ev.emit) ev.emit("ui:" + r->name, kNoEntity);
        } else if (key == "backspace" && fn->el.widget == "input") {
            if (UIElement* el = scene.get<UIElement>(focused_); el && !el->text.empty()) {
                size_t cut = el->text.size() - 1;
                while (cut > 0 && (static_cast<unsigned char>(el->text[cut]) & 0xC0) == 0x80) --cut;
                el->text.erase(cut);
                scene.markDirty();
            }
        }
        fn = focused_ ? lay.find(focused_) : nullptr;
    }

    // Hover/press transitions.
    for (const Node& n : lay.nodes) {
        float target01 = (n.entity == hovered_ || n.entity == pressed_) && n.interactable ? 1.f : 0.f;
        float& b = blend_[n.entity];
        float rate = dt / std::max(1e-3f, n.style.transition);
        b = target01 > b ? std::min(target01, b + rate) : std::max(target01, b - rate);
    }
    if (blend_.size() > lay.nodes.size() * 2 + 64) {
        std::erase_if(blend_, [&](const auto& kv) { return !lay.index.count(kv.first); });
    }
}

}  // namespace sky::ui
