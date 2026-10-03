#include "skywalker/render/Gltf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

#include "skywalker/core/Json.h"
#include "skywalker/core/Strings.h"

namespace sky {

namespace {

namespace fs = std::filesystem;

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

struct Doc {
    Json json;
    std::vector<std::vector<uint8_t>> buffers;
};

Error bad(const std::string& why) { return Error::make("invalid_gltf", why); }

/// Raw bytes + stride for an accessor.
struct View {
    const uint8_t* data = nullptr;
    size_t count = 0;
    size_t stride = 0;
    int componentType = 0;
    int components = 0;
    bool normalized = false;
};

int componentCount(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    return 0;
}

size_t componentSize(int ct) {
    switch (ct) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
    }
}

Result<View> accessor(const Doc& d, int index) {
    const Json& acc = d.json.get("accessors")[static_cast<size_t>(index)];
    if (!acc.isObject()) return bad("missing accessor " + std::to_string(index));
    View v;
    v.count = static_cast<size_t>(acc.get("count").asInt());
    v.componentType = static_cast<int>(acc.get("componentType").asInt());
    v.components = componentCount(acc.get("type").asString());
    v.normalized = acc.get("normalized").asBool();
    size_t elem = componentSize(v.componentType) * static_cast<size_t>(v.components);
    if (!elem) return bad("unsupported accessor format");
    if (!acc.contains("bufferView")) return bad("sparse/empty accessors are not supported");
    const Json& bv = d.json.get("bufferViews")[static_cast<size_t>(acc.get("bufferView").asInt())];
    size_t bi = static_cast<size_t>(bv.get("buffer").asInt());
    if (bi >= d.buffers.size()) return bad("buffer index out of range");
    const auto& buf = d.buffers[bi];
    size_t offset = static_cast<size_t>(bv.get("byteOffset").asInt() + acc.get("byteOffset").asInt());
    v.stride = bv.contains("byteStride") ? static_cast<size_t>(bv.get("byteStride").asInt()) : elem;
    if (v.count && offset + v.stride * (v.count - 1) + elem > buf.size()) return bad("accessor exceeds buffer");
    v.data = buf.data() + offset;
    return v;
}

float readComponent(const View& v, size_t i, int c) {
    const uint8_t* p = v.data + i * v.stride + static_cast<size_t>(c) * componentSize(v.componentType);
    switch (v.componentType) {
        case 5126: { float f; std::memcpy(&f, p, 4); return f; }
        case 5121: return v.normalized ? p[0] / 255.f : p[0];
        case 5123: { uint16_t u; std::memcpy(&u, p, 2); return v.normalized ? u / 65535.f : u; }
        case 5125: { uint32_t u; std::memcpy(&u, p, 4); return static_cast<float>(u); }
        case 5120: { int8_t s; std::memcpy(&s, p, 1); return v.normalized ? std::max(s / 127.f, -1.f) : s; }
        case 5122: { int16_t s; std::memcpy(&s, p, 2); return v.normalized ? std::max(s / 32767.f, -1.f) : s; }
    }
    return 0;
}

uint32_t readIndex(const View& v, size_t i) {
    const uint8_t* p = v.data + i * v.stride;
    switch (v.componentType) {
        case 5121: return p[0];
        case 5123: { uint16_t u; std::memcpy(&u, p, 2); return u; }
        case 5125: { uint32_t u; std::memcpy(&u, p, 4); return u; }
    }
    return 0;
}

Mat4 nodeLocal(const Json& node) {
    if (node.get("matrix").size() == 16) {
        Mat4 m;
        for (int i = 0; i < 16; ++i) m.m[i] = node.get("matrix")[static_cast<size_t>(i)].asFloat();
        return m;  // glTF matrices are column-major, like ours
    }
    Vec3 t{0, 0, 0}, s{1, 1, 1};
    Vec4 q{0, 0, 0, 1};
    const Json& T = node.get("translation");
    const Json& R = node.get("rotation");
    const Json& S = node.get("scale");
    if (T.size() == 3) t = {T[0].asFloat(), T[1].asFloat(), T[2].asFloat()};
    if (R.size() == 4) q = {R[0].asFloat(), R[1].asFloat(), R[2].asFloat(), R[3].asFloat()};
    if (S.size() == 3) s = {S[0].asFloat(), S[1].asFloat(), S[2].asFloat()};
    return Mat4::translate(t) * Mat4::fromQuat(q) * Mat4::scale(s);
}

anim::Trs nodeTrs(const Json& node) {
    if (node.get("matrix").size() == 16) return anim::Trs::fromMatrix(nodeLocal(node));
    anim::Trs out;
    const Json& T = node.get("translation");
    const Json& R = node.get("rotation");
    const Json& S = node.get("scale");
    if (T.size() == 3) out.t = {T[0].asFloat(), T[1].asFloat(), T[2].asFloat()};
    if (R.size() == 4) out.r = anim::Quat{R[0].asFloat(), R[1].asFloat(), R[2].asFloat(), R[3].asFloat()}.normalized();
    if (S.size() == 3) out.s = {S[0].asFloat(), S[1].asFloat(), S[2].asFloat()};
    return out;
}

Result<std::vector<uint8_t>> loadUri(const std::string& uri, const std::string& baseDir) {
    std::vector<uint8_t> out;
    if (str::startsWith(uri, "data:")) {
        size_t comma = uri.find(',');
        if (comma == std::string::npos || uri.find(";base64") == std::string::npos) return bad("unsupported data URI");
        if (!str::base64Decode(std::string_view(uri).substr(comma + 1), out)) return bad("invalid base64 data URI");
        return out;
    }
    // Reject path traversal outside the asset's folder.
    fs::path p = (fs::path(baseDir) / uri).lexically_normal();
    if (p.string().rfind(fs::path(baseDir).lexically_normal().string(), 0) != 0) return bad("external URI escapes asset folder");
    if (!readFile(p.string(), out)) return bad("cannot read " + p.string());
    return out;
}

/// "Armature|Walk" -> "Walk"; exporter placeholders ("mixamo.com", "Take 001") stay usable.
std::string cleanClipName(const std::string& raw, size_t index) {
    std::string name = raw;
    size_t bar = name.rfind('|');
    if (bar != std::string::npos && bar + 1 < name.size()) {
        std::string tail = name.substr(bar + 1);
        if (str::lower(tail) != "layer0" && str::lower(tail) != "baselayer") {
            name = tail;
        } else {
            std::string head = name.substr(0, bar);
            size_t bar2 = head.rfind('|');
            name = bar2 == std::string::npos ? head : head.substr(bar2 + 1);
        }
    }
    if (name.empty()) name = "Clip" + std::to_string(index);
    return name;
}

/// Smooth bind-space normals for skinned vertices whose source had no NORMAL.
void computeMissingBindNormals(MeshData& m) {
    SkinStream& s = m.skin;
    const size_t n = m.vertexCount();
    std::vector<Vec3> acc(n, Vec3{0, 0, 0});
    bool any = false;
    for (size_t v = 0; v < n; ++v) any = any || (s.bind[v * 6 + 3] == 0.f && s.bind[v * 6 + 4] == 0.f && s.bind[v * 6 + 5] == 0.f);
    if (!any) return;
    auto bp = [&](uint32_t i) { return Vec3{s.bind[i * 6], s.bind[i * 6 + 1], s.bind[i * 6 + 2]}; };
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        Vec3 f = cross(bp(b) - bp(a), bp(c) - bp(a));
        acc[a] += f;
        acc[b] += f;
        acc[c] += f;
    }
    for (size_t v = 0; v < n; ++v) {
        float* bn = &s.bind[v * 6 + 3];
        if (bn[0] != 0.f || bn[1] != 0.f || bn[2] != 0.f) continue;
        Vec3 nn = normalize(acc[v]);
        bn[0] = nn.x;
        bn[1] = nn.y;
        bn[2] = nn.z;
    }
}

/// Images and materials (shared by every import path).
void readMaterials(const Doc& d, const std::string& baseDir, GltfImport& out) {
    // Images: embedded bytes are decoded here; external files are only referenced (by URI).
    for (const auto& img : d.json.get("images").elements()) {
        GltfImport::ImageData im;
        if (img.contains("bufferView")) {
            const Json& bv = d.json.get("bufferViews")[static_cast<size_t>(img.get("bufferView").asInt())];
            size_t bi = static_cast<size_t>(bv.get("buffer").asInt());
            size_t off = static_cast<size_t>(bv.get("byteOffset").asInt()), len = static_cast<size_t>(bv.get("byteLength").asInt());
            if (bi < d.buffers.size() && off + len <= d.buffers[bi].size()) {
                im.bytes.assign(d.buffers[bi].begin() + static_cast<std::ptrdiff_t>(off),
                                d.buffers[bi].begin() + static_cast<std::ptrdiff_t>(off + len));
            }
            im.mime = img.get("mimeType").asString("image/png");
        } else if (img.contains("uri")) {
            std::string uri = img.get("uri").asString();
            std::string u = str::lower(uri);
            im.mime = (u.find(".jpg") != std::string::npos || u.find(".jpeg") != std::string::npos ||
                       u.find("image/jpeg") != std::string::npos) ? "image/jpeg" : "image/png";
            if (str::startsWith(uri, "data:")) {
                if (auto data = loadUri(uri, baseDir)) im.bytes = std::move(data.value());
            } else {
                im.uri = uri;
            }
        }
        out.images.push_back(std::move(im));
    }
    auto imageOf = [&](const Json& ref) -> int {
        if (!ref.isObject()) return -1;
        const Json& textures = d.json.get("textures");
        auto ti = static_cast<size_t>(ref.get("index").asInt(-1));
        if (ti >= textures.size()) return -1;
        int source = static_cast<int>(textures[ti].get("source").asInt(-1));
        return source >= 0 && static_cast<size_t>(source) < out.images.size() ? source : -1;
    };
    for (const auto& mj : d.json.get("materials").elements()) {
        GltfImport::Material m;
        m.name = mj.get("name").asString();
        const Json& pbr = mj.get("pbrMetallicRoughness");
        const Json& bc = pbr.get("baseColorFactor");
        if (bc.size() == 4) m.baseColor = {bc[0].asFloat(), bc[1].asFloat(), bc[2].asFloat(), bc[3].asFloat()};
        m.metallic = pbr.get("metallicFactor").asFloat(1.f);
        m.roughness = pbr.get("roughnessFactor").asFloat(1.f);
        const Json& em = mj.get("emissiveFactor");
        if (em.size() == 3) m.emissive = {em[0].asFloat(), em[1].asFloat(), em[2].asFloat(), 1.f};
        m.emissive.w = mj.get("extensions").get("KHR_materials_emissive_strength").get("emissiveStrength").asFloat(1.f);
        m.baseColorImage = imageOf(pbr.get("baseColorTexture"));
        m.normalImage = imageOf(mj.get("normalTexture"));
        m.normalScale = mj.get("normalTexture").get("scale").asFloat(1.f);
        m.metallicRoughnessImage = imageOf(pbr.get("metallicRoughnessTexture"));
        int occ = imageOf(mj.get("occlusionTexture"));
        if (m.metallicRoughnessImage >= 0 && m.metallicRoughnessImage == occ) {
            m.occlusionStrength = mj.get("occlusionTexture").get("strength").asFloat(1.f);
        } else if (m.metallicRoughnessImage >= 0 && occ < 0 &&
                   str::lower(out.images[static_cast<size_t>(m.metallicRoughnessImage)].uri).find("_arm") != std::string::npos) {
            m.occlusionStrength = 1.f;  // "ARM" maps (AO/rough/metal) pack occlusion in R without declaring it
        }
        m.emissiveImage = imageOf(mj.get("emissiveTexture"));
        if (m.emissiveImage >= 0 && m.emissive.x + m.emissive.y + m.emissive.z <= 0.f) m.emissive = {1, 1, 1, m.emissive.w};
        m.alphaMode = mj.get("alphaMode").asString("OPAQUE");
        m.alphaCutoff = mj.get("alphaCutoff").asFloat(0.5f);
        m.doubleSided = mj.get("doubleSided").asBool(false);
        out.materials.push_back(std::move(m));
    }
    out.materialCount = out.materials.size();
}

/// Animation clips (translation / rotation / scale channels; morph weights are skipped).
Status readClips(const Doc& d, const std::vector<int>& nodeToBone, anim::Library& lib) {
    const Json& nodes = d.json.get("nodes");
    const Json& animations = d.json.get("animations");
    std::set<std::string> clipNames;
    for (size_t ai = 0; ai < animations.size(); ++ai) {
        const Json& aj = animations[ai];
        anim::Clip clip;
        clip.name = cleanClipName(aj.get("name").asString(), ai);
        if (!clipNames.insert(clip.name).second) {
            clip.name += "_" + std::to_string(ai);
            clipNames.insert(clip.name);
        }
        const Json& samplers = aj.get("samplers");
        for (const auto& cj : aj.get("channels").elements()) {
            const Json& target = cj.get("target");
            std::string path = target.get("path").asString();
            if (!target.contains("node") || (path != "translation" && path != "rotation" && path != "scale")) continue;
            auto tn = static_cast<size_t>(target.get("node").asInt());
            if (tn >= nodes.size() || nodeToBone[tn] < 0) continue;
            auto si = static_cast<size_t>(cj.get("sampler").asInt(-1));
            if (si >= samplers.size()) return bad("animation channel references a missing sampler");
            const Json& sj = samplers[si];
            anim::Channel ch;
            ch.bone = nodeToBone[tn];
            ch.path = path == "translation" ? anim::Path::Translation : path == "rotation" ? anim::Path::Rotation : anim::Path::Scale;
            std::string interp = sj.get("interpolation").asString("LINEAR");
            ch.interp = interp == "STEP" ? anim::Interp::Step : interp == "CUBICSPLINE" ? anim::Interp::Cubic : anim::Interp::Linear;
            auto in = accessor(d, static_cast<int>(sj.get("input").asInt(-1)));
            auto outv = accessor(d, static_cast<int>(sj.get("output").asInt(-1)));
            if (!in) return in.error();
            if (!outv) return outv.error();
            const int comps = ch.components();
            const size_t perKey = ch.interp == anim::Interp::Cubic ? 3 : 1;
            if (in->components != 1 || outv->components != comps || outv->count < in->count * perKey) {
                return bad("animation sampler output does not match its input (" + path + ")");
            }
            float last = -1e30f;
            bool monotonic = true;
            for (size_t k = 0; k < in->count; ++k) {
                float t = readComponent(*in, k, 0);
                monotonic = monotonic && t > last;
                last = t;
                ch.times.push_back(t);
            }
            if (!monotonic || ch.times.empty()) continue;  // malformed sampler: skip the channel
            ch.values.reserve(in->count * perKey * static_cast<size_t>(comps));
            for (size_t k = 0; k < in->count * perKey; ++k) {
                for (int c = 0; c < comps; ++c) ch.values.push_back(readComponent(*outv, k, c));
            }
            clip.duration = std::max(clip.duration, ch.times.back());
            clip.channels.push_back(std::move(ch));
        }
        lib.clips.push_back(std::move(clip));
    }
    return {};
}

}  // namespace

Result<GltfImport> parseGltf(const std::vector<uint8_t>& bytes, const std::string& baseDir, bool normalizeMesh, int part) {
    Doc d;
    std::vector<uint8_t> glbBin;
    bool haveBin = false;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
        if (rd32(bytes.data() + 4) != 2) return bad("only glTF 2.0 is supported");
        size_t off = 12;
        bool haveJson = false;
        while (off + 8 <= bytes.size()) {
            uint32_t len = rd32(bytes.data() + off), type = rd32(bytes.data() + off + 4);
            off += 8;
            if (off + len > bytes.size()) return bad("truncated GLB chunk");
            if (type == 0x4E4F534A) {  // JSON
                auto j = Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data() + off), len));
                if (!j) return j.error();
                d.json = std::move(j.value());
                haveJson = true;
            } else if (type == 0x004E4942) {  // BIN
                glbBin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(off), bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
                haveBin = true;
            }
            off += len;
        }
        if (!haveJson) return bad("GLB has no JSON chunk");
    } else {
        auto j = Json::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        if (!j) return j.error();
        d.json = std::move(j.value());
    }
    for (const auto& b : d.json.get("buffers").elements()) {
        if (!b.contains("uri")) {
            if (!haveBin) return bad("buffer without uri and no GLB binary chunk");
            d.buffers.push_back(glbBin);
            continue;
        }
        auto data = loadUri(b.get("uri").asString(), baseDir);
        if (!data) return data.error();
        d.buffers.push_back(std::move(data.value()));
    }

    GltfImport out;
    MeshData& mesh = out.mesh;
    out.fullBounds = {Vec3(1e30f), Vec3(-1e30f)};
    const Json& nodes = d.json.get("nodes");

    // Pass 1: the scene's node hierarchy, parents first. For animated models every node
    // becomes a skeleton bone (so node animations and skins can address it).
    struct NodeEntry {
        size_t node;
        int parent;  // index into `order`
        Mat4 world;  // exact rest world matrix (static geometry)
    };
    std::vector<NodeEntry> order;
    std::vector<int> nodeToBone(nodes.size(), -1);
    std::function<Status(size_t, int, const Mat4&, int)> collect = [&](size_t ni, int parent, const Mat4& parentM,
                                                                       int depth) -> Status {
        if (depth > 64) return bad("node hierarchy too deep");
        if (ni >= nodes.size()) return bad("scene references a missing node");
        const Json& node = nodes[ni];
        Mat4 world = parentM * nodeLocal(node);
        int self = static_cast<int>(order.size());
        if (nodeToBone[ni] < 0) nodeToBone[ni] = self;
        order.push_back({ni, parent, world});
        for (const auto& c : node.get("children").elements()) {
            if (Status s = collect(static_cast<size_t>(c.asInt()), self, world, depth + 1); !s) return s;
        }
        return {};
    };
    std::vector<size_t> roots;
    const Json& scenes = d.json.get("scenes");
    size_t sceneIndex = static_cast<size_t>(d.json.get("scene").asInt(0));
    if (scenes.size() > sceneIndex) {
        for (const auto& n : scenes[sceneIndex].get("nodes").elements()) roots.push_back(static_cast<size_t>(n.asInt()));
    } else {
        // No scene: every node that is nobody's child is a root.
        std::vector<bool> isChild(nodes.size(), false);
        for (const auto& n : nodes.elements()) {
            for (const auto& c : n.get("children").elements()) {
                if (static_cast<size_t>(c.asInt()) < nodes.size()) isChild[static_cast<size_t>(c.asInt())] = true;
            }
        }
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (!isChild[i]) roots.push_back(i);
        }
    }
    for (size_t r : roots) {
        if (Status s = collect(r, -1, Mat4{}, 0); !s) return s.error();
    }

    const Json& skins = d.json.get("skins");
    const bool animated = skins.size() > 0 || d.json.get("animations").size() > 0;
    auto lib = std::make_shared<anim::Library>();
    std::vector<Mat4> restGlobals;
    if (animated) {
        std::set<std::string> used;
        for (const auto& entry : order) {
            const Json& node = nodes[entry.node];
            anim::Bone bone;
            bone.name = node.get("name").asString();
            if (bone.name.empty()) bone.name = "node" + std::to_string(entry.node);
            if (!used.insert(bone.name).second) {
                bone.name += "_" + std::to_string(entry.node);
                used.insert(bone.name);
            }
            bone.parent = entry.parent;
            bone.rest = nodeTrs(node);
            lib->skeleton.bones.push_back(std::move(bone));
        }
        anim::computeGlobals(lib->skeleton, anim::restPose(lib->skeleton), restGlobals);
    }

    // Skinning slots: every (skin, joint) pair, plus one rigid slot per animated mesh node.
    SkinStream& skin = mesh.skin;
    std::vector<size_t> skinSlotBase;
    std::vector<int> rigidSlot(order.size(), -1);
    auto addSlot = [&](const std::string& name, const Mat4& inverseBind, const Mat4& restGlobal) {
        skin.jointNames.push_back(name);
        skin.inverseBind.push_back(inverseBind);
        skin.restGlobal.push_back(restGlobal);
        return skin.jointNames.size() - 1;
    };
    if (animated) {
        for (const auto& sk : skins.elements()) {
            skinSlotBase.push_back(skin.jointNames.size());
            const Json& joints = sk.get("joints");
            Result<View> ibm = sk.contains("inverseBindMatrices")
                                   ? accessor(d, static_cast<int>(sk.get("inverseBindMatrices").asInt()))
                                   : Result<View>(View{});
            if (!ibm) return ibm.error();
            if (ibm->data && (ibm->components != 16 || ibm->count < joints.size())) {
                return bad("skin inverseBindMatrices must be one MAT4 per joint");
            }
            std::set<int> jointBones;
            for (size_t j = 0; j < joints.size(); ++j) {
                auto jn = static_cast<size_t>(joints[j].asInt());
                if (jn >= nodes.size()) return bad("skin joint index out of range");
                Mat4 inv;
                if (ibm->data) {
                    for (int k = 0; k < 16; ++k) inv.m[k] = readComponent(*ibm, j, k);
                }
                int b = nodeToBone[jn];
                jointBones.insert(b);
                if (b < 0) {  // joint outside the scene: it can only ever hold its (identity) rest pose
                    addSlot(nodes[jn].get("name").asString("node" + std::to_string(jn)), inv, Mat4{});
                } else {
                    addSlot(lib->skeleton.bones[static_cast<size_t>(b)].name, inv, restGlobals[static_cast<size_t>(b)]);
                }
            }
            if (lib->rootBone < 0) {
                // Root motion comes from the skin's skeleton root: the declared one if it is a
                // joint, else the top-most joint (the hips on humanoid rigs).
                int root = -1;
                if (sk.contains("skeleton") && static_cast<size_t>(sk.get("skeleton").asInt()) < nodes.size()) {
                    root = nodeToBone[static_cast<size_t>(sk.get("skeleton").asInt())];
                }
                if (root < 0 || !jointBones.count(root)) {
                    root = -1;
                    for (int b : jointBones) {
                        if (b >= 0 && !jointBones.count(lib->skeleton.bones[static_cast<size_t>(b)].parent)) {
                            root = b;
                            break;
                        }
                    }
                }
                lib->rootBone = root;
            }
        }
        if (skin.jointNames.size() > 65000) return bad("too many skin joints");
    }

    // Pass 2: geometry, in hierarchy order.
    for (size_t bi = 0; bi < order.size(); ++bi) {
        const Json& node = nodes[order[bi].node];
        if (!node.contains("mesh")) continue;
        const Mat4& world = order[bi].world;
        const Json& m = d.json.get("meshes")[static_cast<size_t>(node.get("mesh").asInt())];
        Mat4 normalM = world.inverse().transposed();
        int skinIndex = animated && node.contains("skin") ? static_cast<int>(node.get("skin").asInt(-1)) : -1;
        if (skinIndex >= static_cast<int>(skins.size())) return bad("node references a missing skin");
        for (const auto& prim : m.get("primitives").elements()) {
            int mode = static_cast<int>(prim.get("mode").asInt(4));
            if (mode != 4) continue;  // triangles only
            const Json& attrs = prim.get("attributes");
            if (!attrs.contains("POSITION")) continue;
            auto pos = accessor(d, static_cast<int>(attrs.get("POSITION").asInt()));
            if (!pos) return pos.error();
            int material = prim.contains("material") ? static_cast<int>(prim.get("material").asInt()) : -1;
            if (std::find(out.parts.begin(), out.parts.end(), material) == out.parts.end()) out.parts.push_back(material);
            Result<View> nrm = attrs.contains("NORMAL") ? accessor(d, static_cast<int>(attrs.get("NORMAL").asInt())) : Result<View>(View{});
            if (!nrm) return nrm.error();

            // Skinning influences: JOINTS_0/WEIGHTS_0 (+ JOINTS_1/WEIGHTS_1; the strongest four are kept).
            const bool skinnedPrim = skinIndex >= 0 && attrs.contains("JOINTS_0") && attrs.contains("WEIGHTS_0");
            View js[2], ws[2];
            int sets = 0;
            for (int set = 0; skinnedPrim && set < 2; ++set) {
                std::string jn = "JOINTS_" + std::to_string(set), wn = "WEIGHTS_" + std::to_string(set);
                if (!attrs.contains(jn) || !attrs.contains(wn)) break;
                auto jv = accessor(d, static_cast<int>(attrs.get(jn).asInt()));
                auto wv = accessor(d, static_cast<int>(attrs.get(wn).asInt()));
                if (!jv) return jv.error();
                if (!wv) return wv.error();
                if (jv->components != 4 || wv->components != 4 || jv->count < pos->count || wv->count < pos->count) {
                    return bad("JOINTS/WEIGHTS must be VEC4 with one entry per vertex");
                }
                js[set] = *jv;
                ws[set] = *wv;
                ++sets;
            }
            const size_t jointCount = skinnedPrim ? skins[static_cast<size_t>(skinIndex)].get("joints").size() : 0;
            const size_t slotBase = skinnedPrim ? skinSlotBase[static_cast<size_t>(skinIndex)] : 0;
            auto influences = [&](size_t i, uint16_t slots[4], float weights[4]) {
                std::pair<float, uint16_t> all[8];
                int n = 0;
                for (int set = 0; set < sets; ++set) {
                    for (int c = 0; c < 4; ++c) {
                        float w = readComponent(ws[set], i, c);
                        auto j = static_cast<size_t>(readComponent(js[set], i, c));
                        if (w > 0.f && j < jointCount) all[n++] = {w, static_cast<uint16_t>(slotBase + j)};
                    }
                }
                std::stable_sort(all, all + n, [](const auto& a, const auto& b) { return a.first > b.first; });
                float sum = 0.f;
                for (int k = 0; k < 4; ++k) {
                    slots[k] = k < n ? all[k].second : static_cast<uint16_t>(slotBase);
                    weights[k] = k < n ? all[k].first : 0.f;
                    sum += weights[k];
                }
                if (sum <= 1e-8f) {  // unweighted vertex: follow the skin's first joint
                    weights[0] = 1.f;
                    sum = 1.f;
                }
                for (int k = 0; k < 4; ++k) weights[k] /= sum;
            };
            auto restSkin = [&](const uint16_t slots[4], const float weights[4]) {
                Mat4 blended;
                for (float& f : blended.m) f = 0.f;
                for (int k = 0; k < 4; ++k) {
                    if (weights[k] <= 0.f) continue;
                    Mat4 j = skin.restGlobal[slots[k]] * skin.inverseBind[slots[k]];
                    for (int e = 0; e < 16; ++e) blended.m[e] += j.m[e] * weights[k];
                }
                return blended;
            };
            int rigid = -1;
            if (animated && !skinnedPrim) {
                if (rigidSlot[bi] < 0) {
                    rigidSlot[bi] = static_cast<int>(addSlot(lib->skeleton.bones[bi].name, Mat4{}, restGlobals[bi]));
                }
                rigid = rigidSlot[bi];
            }
            const Mat4 staticM = rigid >= 0 ? skin.restGlobal[static_cast<size_t>(rigid)] : world;

            // Bounds of the whole model in its rest pose, even when only one part is kept.
            for (size_t i = 0; i < pos->count; ++i) {
                Vec3 local{readComponent(*pos, i, 0), readComponent(*pos, i, 1), readComponent(*pos, i, 2)};
                Vec3 p;
                if (skinnedPrim) {
                    uint16_t slots[4];
                    float weights[4];
                    influences(i, slots, weights);
                    p = restSkin(slots, weights).transformPoint(local);
                } else {
                    p = staticM.transformPoint(local);
                }
                out.fullBounds.min = vmin(out.fullBounds.min, p);
                out.fullBounds.max = vmax(out.fullBounds.max, p);
            }
            if (part != kGltfAllParts && material != part) continue;
            Result<View> uv = attrs.contains("TEXCOORD_0") ? accessor(d, static_cast<int>(attrs.get("TEXCOORD_0").asInt())) : Result<View>(View{});
            Result<View> col = attrs.contains("COLOR_0") ? accessor(d, static_cast<int>(attrs.get("COLOR_0").asInt())) : Result<View>(View{});
            if (!uv) return uv.error();
            if (!col) return col.error();
            if (col->data) mesh.hasVertexColors = true;
            auto base = static_cast<uint32_t>(mesh.vertexCount());
            const Mat4 staticN = rigid >= 0 ? staticM.inverse().transposed() : normalM;
            for (size_t i = 0; i < pos->count; ++i) {
                Vec3 local{readComponent(*pos, i, 0), readComponent(*pos, i, 1), readComponent(*pos, i, 2)};
                Vec3 localN{0, 0, 0};
                if (nrm->data && i < nrm->count) localN = {readComponent(*nrm, i, 0), readComponent(*nrm, i, 1), readComponent(*nrm, i, 2)};
                const bool hasN = length(localN) > 0.f;
                Vec3 p, n{0, 0, 0};
                if (skinnedPrim) {
                    uint16_t slots[4];
                    float weights[4];
                    influences(i, slots, weights);
                    Mat4 sm = restSkin(slots, weights);
                    p = sm.transformPoint(local);
                    if (hasN) n = normalize(sm.transformDir(localN));
                    skin.joints.insert(skin.joints.end(), slots, slots + 4);
                    skin.weights.insert(skin.weights.end(), weights, weights + 4);
                } else {
                    p = staticM.transformPoint(local);
                    if (hasN) n = normalize(staticN.transformDir(localN));
                    if (rigid >= 0) {
                        skin.joints.insert(skin.joints.end(), {static_cast<uint16_t>(rigid), static_cast<uint16_t>(rigid),
                                                               static_cast<uint16_t>(rigid), static_cast<uint16_t>(rigid)});
                        skin.weights.insert(skin.weights.end(), {1.f, 0.f, 0.f, 0.f});
                    }
                }
                if (animated) {
                    Vec3 bn = hasN ? normalize(localN) : Vec3{0, 0, 0};
                    skin.bind.insert(skin.bind.end(), {local.x, local.y, local.z, bn.x, bn.y, bn.z});
                }
                Vec2 t{0, 0};
                if (uv->data && i < uv->count) t = {readComponent(*uv, i, 0), readComponent(*uv, i, 1)};
                Vec4 c{1, 1, 1, 1};  // COLOR_0 is linear (unlike authored sRGB colors)
                if (col->data && i < col->count) {
                    c = {readComponent(*col, i, 0), readComponent(*col, i, 1), readComponent(*col, i, 2),
                         col->components == 4 ? readComponent(*col, i, 3) : 1.f};
                }
                mesh.addVertex(p, n, t, c);
            }
            if (prim.contains("indices")) {
                auto idx = accessor(d, static_cast<int>(prim.get("indices").asInt()));
                if (!idx) return idx.error();
                for (size_t i = 0; i < idx->count; ++i) {
                    uint32_t k = readIndex(*idx, i);
                    if (k >= pos->count) return bad("index out of range");
                    mesh.indices.push_back(base + k);
                }
            } else {
                for (uint32_t i = 0; i < pos->count; ++i) mesh.indices.push_back(base + i);
            }
            ++out.primitiveCount;
        }
    }

    if (animated) {
        if (Status s = readClips(d, nodeToBone, *lib); !s) return s.error();
        lib->source = baseDir;
        out.animation = lib;
        out.skinned = skins.size() > 0;
    }

    if (mesh.indices.empty()) {
        if (!(animated && part == kGltfAllParts)) {
            return bad(part == kGltfAllParts ? "no triangle geometry found" : "material " + std::to_string(part) + " has no geometry");
        }
        mesh.skin = {};  // animation-only file: a clip library without geometry
        out.fullBounds = {};
    } else {
        if (animated) {
            // Bind-space normals for vertices that had none, and per-slot culling bounds.
            computeMissingBindNormals(mesh);
            skin.slotBounds.assign(skin.slots(), Aabb{Vec3(1e30f), Vec3(-1e30f)});
            for (size_t v = 0; v < mesh.vertexCount(); ++v) {
                Vec3 bp{skin.bind[v * 6], skin.bind[v * 6 + 1], skin.bind[v * 6 + 2]};
                for (size_t k = 0; k < SkinStream::kInfluences; ++k) {
                    if (skin.weights[v * SkinStream::kInfluences + k] <= 1e-4f) continue;
                    Aabb& b = skin.slotBounds[skin.joints[v * SkinStream::kInfluences + k]];
                    b.min = vmin(b.min, bp);
                    b.max = vmax(b.max, bp);
                }
            }
        }
        mesh::computeMissingNormals(mesh);
        mesh.computeBounds();
        if (normalizeMesh) mesh::normalizeToUnit(mesh, out.fullBounds);
    }
    readMaterials(d, baseDir, out);
    return out;
}

Result<GltfImport> loadGltf(const std::string& path, bool normalizeMesh, int part) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return Error::make("io_error", "cannot read " + path);
    auto r = parseGltf(bytes, fs::path(path).parent_path().string(), normalizeMesh, part);
    if (r && r->animation) r->animation->source = fs::path(path).filename().string();
    return r;
}

}  // namespace sky
