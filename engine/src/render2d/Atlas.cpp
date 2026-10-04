#include "skywalker/render2d/Atlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "skywalker/core/Strings.h"
#include "stb_rect_pack.h"

namespace sky::render2d {

const AtlasFrame* Atlas::find(std::string_view name) const {
    for (const auto& f : frames) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

int Atlas::indexOf(std::string_view name) const {
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Json Atlas::toJson() const {
    Json fr = Json::object();
    for (const auto& f : frames) {
        fr[f.name] = Json::object({{"rect", Json::array({f.x, f.y, f.w, f.h})},
                                   {"source", Json::array({f.sourceW, f.sourceH})},
                                   {"offset", Json::array({f.offsetX, f.offsetY})}});
    }
    Json doc = Json::object({{"format", "skywalker.atlas"}, {"version", 1}, {"image", image}, {"width", width},
                             {"height", height}, {"frames", fr}});
    if (!normalMap.empty()) doc["normalMap"] = normalMap;
    return doc;
}

bool isAtlasPath(std::string_view path) {
    std::string l = str::lower(path);
    return l.size() > 5 && l.compare(l.size() - 5, 5, ".json") == 0;
}

namespace {

bool rectOf(const Json& j, int& x, int& y, int& w, int& h) {
    if (j.isArray() && j.size() == 4) {
        x = static_cast<int>(j[0].asInt());
        y = static_cast<int>(j[1].asInt());
        w = static_cast<int>(j[2].asInt());
        h = static_cast<int>(j[3].asInt());
        return w > 0 && h > 0;
    }
    if (j.isObject()) {
        x = static_cast<int>(j.get("x").asInt());
        y = static_cast<int>(j.get("y").asInt());
        w = static_cast<int>(j.get("w").asInt());
        h = static_cast<int>(j.get("h").asInt());
        return w > 0 && h > 0;
    }
    return false;
}

bool pairOf(const Json& j, const char* a, const char* b, int& x, int& y) {
    if (j.isArray() && j.size() == 2) {
        x = static_cast<int>(j[0].asInt());
        y = static_cast<int>(j[1].asInt());
        return true;
    }
    if (j.isObject() && j.contains(a)) {
        x = static_cast<int>(j.get(a).asInt());
        y = static_cast<int>(j.get(b).asInt());
        return true;
    }
    return false;
}

Status parseFrame(const std::string& name, const Json& f, AtlasFrame& out) {
    out.name = name;
    // skywalker: rect/source/offset; TexturePacker: frame/sourceSize/spriteSourceSize/rotated
    const Json& rect = f.contains("rect") ? f.get("rect") : f.get("frame");
    if (!rectOf(rect, out.x, out.y, out.w, out.h)) return Error::make("invalid_atlas", "frame \"" + name + "\" has no valid rect");
    if (f.get("rotated").asBool(false)) {
        return Error::make("invalid_atlas", "frame \"" + name + "\" is rotated; export the atlas without rotation");
    }
    out.sourceW = out.w;
    out.sourceH = out.h;
    if (!pairOf(f.get("source"), "w", "h", out.sourceW, out.sourceH)) (void)pairOf(f.get("sourceSize"), "w", "h", out.sourceW, out.sourceH);
    if (!pairOf(f.get("offset"), "x", "y", out.offsetX, out.offsetY)) {
        int sx = 0, sy = 0, sw = 0, sh = 0;
        if (rectOf(f.get("spriteSourceSize"), sx, sy, sw, sh)) {
            out.offsetX = sx;
            out.offsetY = sy;
        }
    }
    out.sourceW = std::max(out.sourceW, out.w);
    out.sourceH = std::max(out.sourceH, out.h);
    return {};
}

}  // namespace

Result<Atlas> parseAtlas(const Json& doc) {
    if (!doc.isObject()) return Error::make("invalid_atlas", "an atlas must be a JSON object");
    Atlas a;
    a.image = doc.get("image").asString();
    if (a.image.empty()) a.image = doc.get("meta").get("image").asString();
    a.normalMap = doc.get("normalMap").asString();
    a.width = static_cast<int>(doc.get("width").asInt());
    a.height = static_cast<int>(doc.get("height").asInt());
    if (a.width == 0) (void)pairOf(doc.get("meta").get("size"), "w", "h", a.width, a.height);
    const Json& frames = doc.get("frames");
    if (frames.isObject()) {
        for (const auto& [name, f] : frames.members()) {
            AtlasFrame fr;
            if (Status s = parseFrame(name, f, fr); !s) return s.error();
            a.frames.push_back(std::move(fr));
        }
    } else if (frames.isArray()) {
        for (const auto& f : frames.elements()) {
            AtlasFrame fr;
            std::string name = f.get("filename").asString(f.get("name").asString("frame" + std::to_string(a.frames.size())));
            // Drop image extensions from TexturePacker filenames ("run_0.png" -> "run_0").
            for (const char* ext : {".png", ".jpg", ".jpeg"}) {
                if (str::lower(name).size() > std::strlen(ext) &&
                    str::lower(name).compare(name.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
                    name = name.substr(0, name.size() - std::strlen(ext));
                }
            }
            if (Status s = parseFrame(name, f, fr); !s) return s.error();
            a.frames.push_back(std::move(fr));
        }
    } else {
        return Error::make("invalid_atlas", "atlas has no \"frames\"");
    }
    if (a.image.empty()) return Error::make("invalid_atlas", "atlas has no \"image\"");
    return a;
}

Result<Atlas> loadAtlas(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("not_found", "cannot open atlas " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    auto doc = Json::parse(ss.str());
    if (!doc) return Error::make("invalid_atlas", path + ": " + doc.error().message);
    return parseAtlas(doc.value());
}

Result<PackResult> packAtlas(const std::vector<PackInput>& inputs, const PackOptions& o) {
    if (inputs.empty()) return Error::make("invalid_arguments", "nothing to pack");
    const int pad = std::max(0, o.padding), ext = std::clamp(o.extrude, 0, 8);
    struct Item {
        int srcX = 0, srcY = 0, w = 0, h = 0;  // trimmed region of the input
    };
    std::vector<Item> items(inputs.size());
    int64_t area = 0;
    int maxW = 0, maxH = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const Image& img = inputs[i].image;
        if (img.width <= 0 || img.height <= 0) return Error::make("invalid_image", "\"" + inputs[i].name + "\" is empty");
        int x0 = 0, y0 = 0, x1 = img.width, y1 = img.height;
        if (o.trim) {
            x0 = img.width, y0 = img.height, x1 = 0, y1 = 0;
            for (int y = 0; y < img.height; ++y) {
                for (int x = 0; x < img.width; ++x) {
                    if (img.at(x, y)[3] == 0) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x + 1);
                    y1 = std::max(y1, y + 1);
                }
            }
            if (x1 <= x0) x0 = y0 = 0, x1 = y1 = 1;  // fully transparent: keep one pixel
        }
        items[i] = {x0, y0, x1 - x0, y1 - y0};
        int cw = items[i].w + 2 * ext + pad, ch = items[i].h + 2 * ext + pad;
        area += static_cast<int64_t>(cw) * ch;
        maxW = std::max(maxW, cw);
        maxH = std::max(maxH, ch);
    }
    auto roundUp = [&](int v) {
        if (!o.powerOfTwo) return v;
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    };
    int side = std::max({static_cast<int>(std::ceil(std::sqrt(static_cast<double>(area)))), maxW, maxH});
    int width = roundUp(side), height = roundUp(side);
    std::vector<stbrp_rect> rects(inputs.size());
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (width > o.maxSize + pad || height > o.maxSize + pad) break;
        for (size_t i = 0; i < items.size(); ++i) {
            rects[i] = {};
            rects[i].id = static_cast<int>(i);
            rects[i].w = items[i].w + 2 * ext + pad;
            rects[i].h = items[i].h + 2 * ext + pad;
        }
        std::vector<stbrp_node> nodes(static_cast<size_t>(width) + 1);
        stbrp_context ctx;
        // The trailing padding of the last row/column may hang over the edge: pack into size + pad.
        stbrp_init_target(&ctx, width + pad, height + pad, nodes.data(), static_cast<int>(nodes.size()));
        if (stbrp_pack_rects(&ctx, rects.data(), static_cast<int>(rects.size()))) {
            PackResult r;
            r.image = Image(width, height);
            r.atlas.width = width;
            r.atlas.height = height;
            for (const stbrp_rect& rc : rects) {
                const Item& it = items[static_cast<size_t>(rc.id)];
                const Image& src = inputs[static_cast<size_t>(rc.id)].image;
                const int dx = rc.x + ext, dy = rc.y + ext;
                for (int y = -ext; y < it.h + ext; ++y) {
                    for (int x = -ext; x < it.w + ext; ++x) {
                        int tx = dx + x, ty = dy + y;
                        if (tx < 0 || ty < 0 || tx >= width || ty >= height) continue;
                        int sx = it.srcX + std::clamp(x, 0, it.w - 1), sy = it.srcY + std::clamp(y, 0, it.h - 1);
                        std::copy_n(src.at(sx, sy), 4, r.image.at(tx, ty));
                    }
                }
                AtlasFrame f;
                f.name = inputs[static_cast<size_t>(rc.id)].name;
                f.x = dx;
                f.y = dy;
                f.w = it.w;
                f.h = it.h;
                f.sourceW = src.width;
                f.sourceH = src.height;
                f.offsetX = it.srcX;
                f.offsetY = it.srcY;
                r.atlas.frames.push_back(std::move(f));
            }
            // Input order (animation frame order), not packing order.
            std::vector<AtlasFrame> ordered(inputs.size());
            for (size_t k = 0; k < rects.size(); ++k) ordered[static_cast<size_t>(rects[k].id)] = r.atlas.frames[k];
            r.atlas.frames = std::move(ordered);
            return r;
        }
        if (o.powerOfTwo) {
            if (width <= height) width *= 2;
            else height *= 2;
        } else {
            width = static_cast<int>(width * 1.12f) + 1;
            height = static_cast<int>(height * 1.12f) + 1;
        }
    }
    return Error::make("atlas_too_large", "the images do not fit in a " + std::to_string(o.maxSize) + "x" +
                                              std::to_string(o.maxSize) + " atlas",
                       "pack fewer or smaller images, or raise max_size");
}

Atlas sliceGrid(int imageWidth, int imageHeight, int cellWidth, int cellHeight, int margin, int spacing,
                const std::string& prefix, const std::vector<std::string>& names) {
    Atlas a;
    a.width = imageWidth;
    a.height = imageHeight;
    if (cellWidth <= 0 || cellHeight <= 0) return a;
    int index = 0;
    for (int y = margin; y + cellHeight <= imageHeight - margin; y += cellHeight + spacing) {
        for (int x = margin; x + cellWidth <= imageWidth - margin; x += cellWidth + spacing) {
            AtlasFrame f;
            f.name = static_cast<size_t>(index) < names.size() ? names[static_cast<size_t>(index)] : prefix + std::to_string(index);
            f.x = x;
            f.y = y;
            f.w = f.sourceW = cellWidth;
            f.h = f.sourceH = cellHeight;
            a.frames.push_back(std::move(f));
            ++index;
        }
    }
    return a;
}

Result<std::vector<int>> parseFrameList(const Json& frames, int frameCount, const Atlas* atlas) {
    std::vector<int> out;
    auto addIndex = [&](int64_t i) -> Status {
        if (i < 0 || (frameCount > 0 && i >= frameCount)) {
            return Error::make("invalid_frames", "frame index " + std::to_string(i) + " is out of range (0-" +
                                                     std::to_string(std::max(0, frameCount - 1)) + ")");
        }
        out.push_back(static_cast<int>(i));
        return {};
    };
    auto addName = [&](const std::string& name) -> Status {
        if (!atlas) return Error::make("invalid_frames", "frame name \"" + name + "\" needs an atlas texture (*.atlas.json)");
        if (name.find('*') != std::string::npos || name.find('?') != std::string::npos) {
            size_t before = out.size();
            for (size_t i = 0; i < atlas->frames.size(); ++i) {
                if (str::globMatch(name, atlas->frames[i].name)) out.push_back(static_cast<int>(i));
            }
            if (out.size() == before) return Error::make("invalid_frames", "no atlas frames match \"" + name + "\"");
            return {};
        }
        int idx = atlas->indexOf(name);
        if (idx < 0) {
            std::vector<std::string> names;
            for (const auto& f : atlas->frames) names.push_back(f.name);
            std::string guess = str::closest(name, names, 3);
            return Error::make("invalid_frames", "atlas has no frame \"" + name + "\"",
                               guess.empty() ? "" : "did you mean \"" + guess + "\"?");
        }
        out.push_back(idx);
        return {};
    };
    auto parseSpec = [&](const std::string& spec) -> Status {
        for (const auto& partRaw : str::split(spec, ',')) {
            std::string part = str::trim(partRaw);
            if (part.empty()) continue;
            double a = 0, b = 0;
            size_t dash = part.find('-', 1);
            if (dash != std::string::npos && str::parseDouble(part.substr(0, dash), a) && str::parseDouble(part.substr(dash + 1), b)) {
                int from = static_cast<int>(a), to = static_cast<int>(b);
                int step = from <= to ? 1 : -1;
                for (int i = from;; i += step) {
                    if (Status s = addIndex(i); !s) return s;
                    if (i == to || out.size() > 100000) break;
                }
            } else if (str::parseDouble(part, a)) {
                if (Status s = addIndex(static_cast<int64_t>(a)); !s) return s;
            } else if (Status s = addName(part); !s) {
                return s;
            }
        }
        return {};
    };
    Status st;
    if (frames.isNumber()) st = addIndex(frames.asInt());
    else if (frames.isString()) st = parseSpec(frames.asString());
    else if (frames.isArray()) {
        for (const auto& f : frames.elements()) {
            st = f.isNumber() ? addIndex(f.asInt()) : f.isString() ? parseSpec(f.asString())
                                                                     : Status(Error::make("invalid_frames", "frames must be numbers or names"));
            if (!st) break;
        }
    } else if (frames.isNull() && frameCount > 0) {
        for (int i = 0; i < frameCount; ++i) out.push_back(i);  // every frame
    } else {
        st = Error::make("invalid_frames", "frames must be \"0-7\", [0, 1, 2] or [\"run_0\", ...]");
    }
    if (!st) return st.error();
    if (out.empty()) return Error::make("invalid_frames", "the clip has no frames");
    return out;
}

}  // namespace sky::render2d
