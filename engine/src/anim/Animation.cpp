#include "skywalker/anim/Animation.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>

#include "skywalker/core/Strings.h"

namespace sky::anim {

static_assert(std::endian::native == std::endian::little, "the .anim format is little-endian");

namespace {

std::string_view stripNamespace(std::string_view s) {
    size_t cut = s.find_last_of(":|");
    return cut == std::string_view::npos ? s : s.substr(cut + 1);
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

Vec3 vec3At(const float* p) { return {p[0], p[1], p[2]}; }
Quat quatAt(const float* p) { return Quat{p[0], p[1], p[2], p[3]}; }

}  // namespace

// ---------------------------------------------------------------------------
// Skeleton / library lookups
// ---------------------------------------------------------------------------

int Skeleton::find(std::string_view name) const {
    for (size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) return static_cast<int>(i);
    }
    for (size_t i = 0; i < bones.size(); ++i) {
        if (iequals(bones[i].name, name)) return static_cast<int>(i);
    }
    std::string_view bare = stripNamespace(name);
    if (bare.empty()) return -1;
    for (size_t i = 0; i < bones.size(); ++i) {
        if (iequals(stripNamespace(bones[i].name), bare)) return static_cast<int>(i);
    }
    return -1;
}

std::vector<std::string> Skeleton::names() const {
    std::vector<std::string> out;
    out.reserve(bones.size());
    for (const auto& b : bones) out.push_back(b.name);
    return out;
}

bool Skeleton::isDescendant(int bone, int ancestor) const {
    for (int b = bone; b >= 0 && b < static_cast<int>(bones.size()); b = bones[static_cast<size_t>(b)].parent) {
        if (b == ancestor) return true;
    }
    return false;
}

bool Clip::hasChannel(int bone, Path path) const {
    for (const auto& c : channels) {
        if (c.bone == bone && c.path == path) return true;
    }
    return false;
}

int Library::clipIndex(std::string_view name) const {
    for (size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].name == name) return static_cast<int>(i);
    }
    for (size_t i = 0; i < clips.size(); ++i) {
        if (iequals(clips[i].name, name)) return static_cast<int>(i);
    }
    return -1;
}

const Clip* Library::clip(std::string_view name) const {
    int i = clipIndex(name);
    return i < 0 ? nullptr : &clips[static_cast<size_t>(i)];
}

std::vector<std::string> Library::clipNames() const {
    std::vector<std::string> out;
    for (const auto& c : clips) out.push_back(c.name);
    return out;
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

Pose restPose(const Skeleton& skeleton) {
    Pose p;
    p.reserve(skeleton.bones.size());
    for (const auto& b : skeleton.bones) p.push_back(b.rest);
    return p;
}

Trs sampleChannel(const Channel& ch, float time, const Trs& fallback) {
    Trs out = fallback;
    const size_t n = ch.times.size();
    const int c = ch.components();
    const size_t stride = static_cast<size_t>(ch.interp == Interp::Cubic ? 3 * c : c);
    if (n == 0 || ch.values.size() < n * stride) return out;
    const size_t valueOffset = ch.interp == Interp::Cubic ? static_cast<size_t>(c) : 0;
    auto value = [&](size_t key) { return ch.values.data() + key * stride + valueOffset; };
    auto put = [&](const float* v) {
        switch (ch.path) {
            case Path::Translation: out.t = vec3At(v); break;
            case Path::Scale: out.s = vec3At(v); break;
            case Path::Rotation: out.r = quatAt(v).normalized(); break;
        }
    };
    if (n == 1 || time <= ch.times.front()) {
        put(value(0));
        return out;
    }
    if (time >= ch.times.back()) {
        put(value(n - 1));
        return out;
    }
    size_t i = static_cast<size_t>(std::upper_bound(ch.times.begin(), ch.times.end(), time) - ch.times.begin()) - 1;
    float t0 = ch.times[i], t1 = ch.times[i + 1];
    float dt = t1 - t0;
    float s = dt > 0.f ? (time - t0) / dt : 0.f;
    if (ch.interp == Interp::Step) {
        put(value(i));
        return out;
    }
    float tmp[4];
    if (ch.interp == Interp::Cubic) {
        // Hermite spline: tangents are stored per second, scaled by the key interval.
        const float* v0 = value(i);
        const float* v1 = value(i + 1);
        const float* out0 = ch.values.data() + i * stride + 2 * static_cast<size_t>(c);
        const float* in1 = ch.values.data() + (i + 1) * stride;
        float s2 = s * s, s3 = s2 * s;
        float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        for (int k = 0; k < c; ++k) tmp[k] = h00 * v0[k] + h10 * dt * out0[k] + h01 * v1[k] + h11 * dt * in1[k];
        put(tmp);
        return out;
    }
    const float* a = value(i);
    const float* b = value(i + 1);
    if (ch.path == Path::Rotation) {
        out.r = slerp(quatAt(a).normalized(), quatAt(b).normalized(), s);
        return out;
    }
    for (int k = 0; k < c; ++k) tmp[k] = a[k] + (b[k] - a[k]) * s;
    put(tmp);
    return out;
}

void sampleClip(const Clip& clip, float time, Pose& pose) {
    for (const auto& ch : clip.channels) {
        if (ch.bone < 0 || static_cast<size_t>(ch.bone) >= pose.size()) continue;
        Trs& t = pose[static_cast<size_t>(ch.bone)];
        Trs v = sampleChannel(ch, time, t);
        switch (ch.path) {
            case Path::Translation: t.t = v.t; break;
            case Path::Rotation: t.r = v.r; break;
            case Path::Scale: t.s = v.s; break;
        }
    }
}

Vec3 sampleTranslation(const Clip& clip, int bone, float time, Vec3 fallback) {
    for (const auto& ch : clip.channels) {
        if (ch.bone == bone && ch.path == Path::Translation) {
            Trs base;
            base.t = fallback;
            return sampleChannel(ch, time, base).t;
        }
    }
    return fallback;
}

void blendPoses(const Pose& a, const Pose& b, float w, Pose& out) {
    size_t n = std::min(a.size(), b.size());
    out.resize(std::max(out.size(), n));
    for (size_t i = 0; i < n; ++i) out[i] = lerp(a[i], b[i], w);
}

void blendPosesMasked(const Pose& a, const Pose& b, float w, const std::vector<bool>& mask, Pose& out) {
    size_t n = std::min(a.size(), b.size());
    out.resize(std::max(out.size(), n));
    for (size_t i = 0; i < n; ++i) out[i] = (i < mask.size() && mask[i]) ? lerp(a[i], b[i], w) : a[i];
}

void computeGlobals(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4>& globals) {
    const size_t n = skeleton.bones.size();
    globals.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Mat4 local = i < pose.size() ? pose[i].matrix() : skeleton.bones[i].rest.matrix();
        int p = skeleton.bones[i].parent;
        globals[i] = p >= 0 ? globals[static_cast<size_t>(p)] * local : local;
    }
}

bool solveTwoBoneIk(const Skeleton& sk, Pose& pose, std::vector<Mat4>& globals, int end, Vec3 t, Vec3 pole, float weight) {
    if (end < 0 || static_cast<size_t>(end) >= sk.bones.size() || weight <= 0.f) return false;
    const int mid = sk.bones[static_cast<size_t>(end)].parent;
    const int root = mid >= 0 ? sk.bones[static_cast<size_t>(mid)].parent : -1;
    if (root < 0 || globals.size() != sk.bones.size() || pose.size() != sk.bones.size()) return false;
    const Vec3 a = globals[static_cast<size_t>(root)].translation();
    const Vec3 b = globals[static_cast<size_t>(mid)].translation();
    const Vec3 c = globals[static_cast<size_t>(end)].translation();
    const float lab = length(b - a), lcb = length(c - b);
    if (lab < 1e-5f || lcb < 1e-5f) return false;
    constexpr float eps = 1e-4f;
    const float lat = std::clamp(length(t - a), eps, lab + lcb - eps);
    auto angle = [](Vec3 u, Vec3 v) { return std::acos(std::clamp(dot(normalize(u), normalize(v)), -1.f, 1.f)); };
    const float acab0 = angle(c - a, b - a);
    const float babc0 = angle(a - b, c - b);
    const float acat0 = angle(c - a, t - a);
    const float acab1 = std::acos(std::clamp((lcb * lcb - lab * lab - lat * lat) / (-2.f * lab * lat), -1.f, 1.f));
    const float babc1 = std::acos(std::clamp((lat * lat - lab * lab - lcb * lcb) / (-2.f * lab * lcb), -1.f, 1.f));
    // Bend plane: the pole hint, else the current elbow/knee direction.
    Vec3 bend = length(pole) > 1e-6f ? pole : b - a;
    Vec3 axis0 = cross(c - a, bend);
    if (length(axis0) < 1e-6f) axis0 = cross(c - a, Vec3{0, 0, 1});
    if (length(axis0) < 1e-6f) axis0 = cross(c - a, Vec3{1, 0, 0});
    axis0 = normalize(axis0);
    Vec3 axis1 = cross(c - a, t - a);
    const bool aligned = length(axis1) < 1e-6f;
    axis1 = aligned ? axis0 : normalize(axis1);
    const Quat aGr = rotationOf(globals[static_cast<size_t>(root)]);
    const Quat bGr = rotationOf(globals[static_cast<size_t>(mid)]);
    const Quat r0 = Quat::axisAngle(aGr.conjugate().rotate(axis0), acab1 - acab0);
    const Quat r1 = Quat::axisAngle(bGr.conjugate().rotate(axis0), babc1 - babc0);
    const Quat r2 = aligned ? Quat{} : Quat::axisAngle(aGr.conjugate().rotate(axis1), acat0);
    Trs& ra = pose[static_cast<size_t>(root)];
    Trs& rb = pose[static_cast<size_t>(mid)];
    const Quat newA = (ra.r * (r2 * r0)).normalized();  // world: bend (r0) first, then swing (r2)
    const Quat newB = (rb.r * r1).normalized();
    ra.r = weight >= 1.f ? newA : slerp(ra.r, newA, weight);
    rb.r = weight >= 1.f ? newB : slerp(rb.r, newB, weight);
    computeGlobals(sk, pose, globals);
    return true;
}

float rootSpeed(const Library& lib, const Clip& clip, Vec3 up) {
    if (lib.rootBone < 0 || clip.duration <= 1e-4f) return 0.f;
    const Bone& root = lib.skeleton.bones[static_cast<size_t>(lib.rootBone)];
    Mat4 parent;
    if (root.parent >= 0) {
        std::vector<Mat4> rest;
        computeGlobals(lib.skeleton, restPose(lib.skeleton), rest);
        parent = rest[static_cast<size_t>(root.parent)];
    }
    up = normalize(up);
    auto flat = [&](float t) {
        Vec3 p = parent.transformPoint(sampleTranslation(clip, lib.rootBone, t, root.rest.t));
        return p - up * dot(p, up);
    };
    return length(flat(clip.duration) - flat(0.f)) / clip.duration;
}

size_t retarget(const Clip& clip, const Skeleton& source, const Skeleton& target, Clip& out) {
    out.name = clip.name;
    out.duration = clip.duration;
    out.channels.clear();
    // Only the top-most translated bone (the hips) keeps its translation, rescaled to the
    // target's proportions; other bones keep the target's own bone lengths.
    int hips = -1;
    int hipsDepth = 1 << 20;
    for (const auto& ch : clip.channels) {
        if (ch.path != Path::Translation || ch.bone < 0 || static_cast<size_t>(ch.bone) >= source.bones.size()) continue;
        int depth = 0;
        for (int b = source.bones[static_cast<size_t>(ch.bone)].parent; b >= 0; b = source.bones[static_cast<size_t>(b)].parent) ++depth;
        if (depth < hipsDepth) {
            hipsDepth = depth;
            hips = ch.bone;
        }
    }
    for (const auto& ch : clip.channels) {
        if (ch.bone < 0 || static_cast<size_t>(ch.bone) >= source.bones.size()) continue;
        int t = target.find(source.bones[static_cast<size_t>(ch.bone)].name);
        if (t < 0) continue;
        if (ch.path == Path::Translation && ch.bone != hips) continue;
        Channel c = ch;
        c.bone = t;
        if (ch.path == Path::Translation) {
            float ls = length(source.bones[static_cast<size_t>(ch.bone)].rest.t);
            float lt = length(target.bones[static_cast<size_t>(t)].rest.t);
            if (ls > 1e-5f && lt > 1e-5f && std::fabs(ls - lt) > 1e-4f * ls) {
                float k = lt / ls;
                for (float& v : c.values) v *= k;
            }
        }
        out.channels.push_back(std::move(c));
    }
    return out.channels.size();
}

// ---------------------------------------------------------------------------
// Skinning
// ---------------------------------------------------------------------------

std::vector<int> mapSkin(const SkinStream& skin, const Skeleton& skeleton) {
    std::vector<int> map(skin.slots(), -1);
    for (size_t k = 0; k < skin.slots(); ++k) map[k] = skeleton.find(skin.jointNames[k]);
    return map;
}

void skinPalette(const SkinStream& skin, const std::vector<int>& slotToBone, const std::vector<Mat4>& globals,
                 std::vector<Mat4>& palette) {
    const size_t n = skin.slots();
    palette.resize(n);
    for (size_t k = 0; k < n; ++k) {
        int b = k < slotToBone.size() ? slotToBone[k] : -1;
        const Mat4& g = (b >= 0 && static_cast<size_t>(b) < globals.size()) ? globals[static_cast<size_t>(b)]
                                                                            : skin.restGlobal[k];
        palette[k] = skin.transform * g * skin.inverseBind[k];
    }
}

void restPalette(const SkinStream& skin, std::vector<Mat4>& palette) {
    palette.resize(skin.slots());
    for (size_t k = 0; k < skin.slots(); ++k) palette[k] = skin.transform * skin.restGlobal[k] * skin.inverseBind[k];
}

void skinMesh(const MeshData& mesh, const std::vector<Mat4>& palette, MeshData& out) {
    out.vertices = mesh.vertices;
    out.indices = mesh.indices;
    out.hasVertexColors = mesh.hasVertexColors;
    const SkinStream& s = mesh.skin;
    const size_t n = mesh.vertexCount();
    if (s.joints.size() < n * SkinStream::kInfluences || s.bind.size() < n * 6) {
        out.computeBounds();
        return;
    }
    for (size_t v = 0; v < n; ++v) {
        Vec3 bp = vec3At(&s.bind[v * 6]), bn = vec3At(&s.bind[v * 6 + 3]);
        Vec3 p{0, 0, 0}, nrm{0, 0, 0};
        for (int k = 0; k < SkinStream::kInfluences; ++k) {
            float w = s.weights[v * SkinStream::kInfluences + static_cast<size_t>(k)];
            if (w <= 0.f) continue;
            size_t slot = s.joints[v * SkinStream::kInfluences + static_cast<size_t>(k)];
            if (slot >= palette.size()) continue;
            const Mat4& m = palette[slot];
            p += m.transformPoint(bp) * w;
            nrm += m.transformDir(bn) * w;
        }
        float* dst = &out.vertices[v * MeshData::kFloatsPerVertex];
        dst[0] = p.x;
        dst[1] = p.y;
        dst[2] = p.z;
        Vec3 nn = normalize(nrm);
        dst[3] = nn.x;
        dst[4] = nn.y;
        dst[5] = nn.z;
    }
    out.computeBounds();
}

Aabb posedBounds(const SkinStream& skin, const std::vector<Mat4>& palette) {
    Aabb box{Vec3(1e30f), Vec3(-1e30f)};
    for (size_t k = 0; k < skin.slotBounds.size() && k < palette.size(); ++k) {
        const Aabb& b = skin.slotBounds[k];
        if (b.min.x > b.max.x) continue;
        Aabb t = b.transformed(palette[k]);
        box.min = vmin(box.min, t.min);
        box.max = vmax(box.max, t.max);
    }
    if (box.min.x > box.max.x) return {};
    return box;
}

// ---------------------------------------------------------------------------
// *.anim files
// ---------------------------------------------------------------------------

namespace {

constexpr char kMagic[8] = {'S', 'K', 'Y', 'A', 'N', 'I', 'M', '1'};

const char* pathName(Path p) { return p == Path::Translation ? "t" : p == Path::Rotation ? "r" : "s"; }
const char* interpName(Interp i) { return i == Interp::Linear ? "linear" : i == Interp::Step ? "step" : "cubic"; }

Json floats(std::initializer_list<float> v) {
    Json a = Json::array();
    for (float f : v) a.push(f);
    return a;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

Error bad(const std::string& why) { return Error::make("invalid_animation", why); }

}  // namespace

std::vector<uint8_t> encodeLibrary(const Library& lib) {
    std::vector<float> blob;
    Json bones = Json::array();
    for (const auto& b : lib.skeleton.bones) {
        bones.push(Json::object({{"name", b.name},
                                 {"parent", b.parent},
                                 {"t", floats({b.rest.t.x, b.rest.t.y, b.rest.t.z})},
                                 {"r", floats({b.rest.r.x, b.rest.r.y, b.rest.r.z, b.rest.r.w})},
                                 {"s", floats({b.rest.s.x, b.rest.s.y, b.rest.s.z})}}));
    }
    Json clips = Json::array();
    for (const auto& c : lib.clips) {
        Json channels = Json::array();
        for (const auto& ch : c.channels) {
            Json cj = Json::object({{"bone", ch.bone},
                                    {"path", pathName(ch.path)},
                                    {"interp", interpName(ch.interp)},
                                    {"keys", ch.times.size()},
                                    {"offset", blob.size()},
                                    {"values", ch.values.size()}});
            blob.insert(blob.end(), ch.times.begin(), ch.times.end());
            blob.insert(blob.end(), ch.values.begin(), ch.values.end());
            channels.push(std::move(cj));
        }
        clips.push(Json::object({{"name", c.name}, {"duration", c.duration}, {"channels", channels}}));
    }
    Json header = Json::object({{"format", "skywalker.anim"},
                                {"version", 1},
                                {"source", lib.source},
                                {"rootBone", lib.rootBone},
                                {"bones", bones},
                                {"clips", clips}});
    std::string text = header.dump();
    while (text.size() % 4) text.push_back(' ');
    std::vector<uint8_t> out(kMagic, kMagic + 8);
    put32(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    put32(out, static_cast<uint32_t>(blob.size()));
    size_t at = out.size();
    out.resize(at + blob.size() * sizeof(float));
    if (!blob.empty()) std::memcpy(out.data() + at, blob.data(), blob.size() * sizeof(float));
    return out;
}

Result<Library> decodeLibrary(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kMagic, 8) != 0) return bad("not a Skywalker animation library (.anim)");
    uint32_t jsonLen = get32(bytes.data() + 8);
    if (12ull + jsonLen + 4 > bytes.size()) return bad("truncated .anim header");
    auto header = Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data() + 12), jsonLen));
    if (!header) return header.error();
    const Json& h = header.value();
    if (h.get("format").asString() != "skywalker.anim") return bad(".anim header has the wrong format tag");
    size_t at = 12 + jsonLen;
    uint32_t count = get32(bytes.data() + at);
    at += 4;
    if (at + static_cast<size_t>(count) * sizeof(float) > bytes.size()) return bad("truncated .anim data");
    std::vector<float> blob(count);
    if (count) std::memcpy(blob.data(), bytes.data() + at, static_cast<size_t>(count) * sizeof(float));

    Library lib;
    lib.source = h.get("source").asString();
    lib.rootBone = static_cast<int>(h.get("rootBone").asInt(-1));
    for (const auto& bj : h.get("bones").elements()) {
        Bone b;
        b.name = bj.get("name").asString();
        b.parent = static_cast<int>(bj.get("parent").asInt(-1));
        if (b.parent >= static_cast<int>(lib.skeleton.bones.size())) return bad("bone " + b.name + " comes before its parent");
        const Json& t = bj.get("t");
        const Json& r = bj.get("r");
        const Json& s = bj.get("s");
        if (t.size() == 3) b.rest.t = {t[0].asFloat(), t[1].asFloat(), t[2].asFloat()};
        if (r.size() == 4) b.rest.r = Quat{r[0].asFloat(), r[1].asFloat(), r[2].asFloat(), r[3].asFloat()}.normalized();
        if (s.size() == 3) b.rest.s = {s[0].asFloat(), s[1].asFloat(), s[2].asFloat()};
        lib.skeleton.bones.push_back(std::move(b));
    }
    if (lib.rootBone >= static_cast<int>(lib.skeleton.bones.size())) lib.rootBone = -1;
    for (const auto& cj : h.get("clips").elements()) {
        Clip c;
        c.name = cj.get("name").asString();
        c.duration = cj.get("duration").asFloat();
        for (const auto& chj : cj.get("channels").elements()) {
            Channel ch;
            ch.bone = static_cast<int>(chj.get("bone").asInt(-1));
            if (ch.bone < 0 || ch.bone >= static_cast<int>(lib.skeleton.bones.size())) return bad("channel targets a missing bone");
            std::string p = chj.get("path").asString();
            ch.path = p == "r" ? Path::Rotation : p == "s" ? Path::Scale : Path::Translation;
            std::string ip = chj.get("interp").asString();
            ch.interp = ip == "step" ? Interp::Step : ip == "cubic" ? Interp::Cubic : Interp::Linear;
            size_t keys = static_cast<size_t>(chj.get("keys").asInt());
            size_t offset = static_cast<size_t>(chj.get("offset").asInt());
            size_t values = static_cast<size_t>(chj.get("values").asInt());
            size_t stride = static_cast<size_t>(ch.components() * (ch.interp == Interp::Cubic ? 3 : 1));
            if (offset + keys + values > blob.size() || values != keys * stride) return bad("channel data out of range in clip " + c.name);
            ch.times.assign(blob.begin() + static_cast<std::ptrdiff_t>(offset), blob.begin() + static_cast<std::ptrdiff_t>(offset + keys));
            ch.values.assign(blob.begin() + static_cast<std::ptrdiff_t>(offset + keys),
                             blob.begin() + static_cast<std::ptrdiff_t>(offset + keys + values));
            c.channels.push_back(std::move(ch));
        }
        lib.clips.push_back(std::move(c));
    }
    return lib;
}

Status saveLibrary(const std::string& path, const Library& lib) {
    std::vector<uint8_t> bytes = encodeLibrary(lib);
    std::ofstream f(path, std::ios::binary);
    if (!f) return Error::make("io_error", "cannot write " + path);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f ? Status{} : Status{Error::make("io_error", "failed writing " + path)};
}

Result<Library> loadLibrary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Error::make("not_found", "cannot read animation library " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return decodeLibrary(bytes);
}

Json libraryToJson(const Library& lib, bool includeBones) {
    Json clips = Json::array();
    for (const auto& c : lib.clips) {
        clips.push(Json::object({{"name", c.name},
                                 {"duration", std::round(c.duration * 1000.f) / 1000.f},
                                 {"channels", c.channels.size()}}));
    }
    Json j = Json::object({{"clips", clips}, {"boneCount", lib.skeleton.bones.size()}});
    if (lib.rootBone >= 0) j["rootBone"] = lib.skeleton.bones[static_cast<size_t>(lib.rootBone)].name;
    if (includeBones) {
        Json bones = Json::array();
        for (const auto& b : lib.skeleton.bones) {
            Json bj = Json::object({{"name", b.name}});
            if (b.parent >= 0) bj["parent"] = lib.skeleton.bones[static_cast<size_t>(b.parent)].name;
            bones.push(std::move(bj));
        }
        j["bones"] = bones;
    }
    return j;
}

}  // namespace sky::anim
