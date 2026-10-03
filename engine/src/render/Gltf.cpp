#include "skywalker/render/Gltf.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

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

}  // namespace

Result<GltfImport> parseGltf(const std::vector<uint8_t>& bytes, const std::string& baseDir, bool normalizeMesh) {
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
    const Json& nodes = d.json.get("nodes");
    std::function<Status(size_t, const Mat4&, int)> visit = [&](size_t ni, const Mat4& parentM, int depth) -> Status {
        if (depth > 64) return bad("node hierarchy too deep");
        const Json& node = nodes[ni];
        Mat4 world = parentM * nodeLocal(node);
        if (node.contains("mesh")) {
            const Json& m = d.json.get("meshes")[static_cast<size_t>(node.get("mesh").asInt())];
            Mat4 normalM = world.inverse().transposed();
            for (const auto& prim : m.get("primitives").elements()) {
                int mode = static_cast<int>(prim.get("mode").asInt(4));
                if (mode != 4) continue;  // triangles only
                const Json& attrs = prim.get("attributes");
                if (!attrs.contains("POSITION")) continue;
                auto pos = accessor(d, static_cast<int>(attrs.get("POSITION").asInt()));
                if (!pos) return pos.error();
                Result<View> nrm = attrs.contains("NORMAL") ? accessor(d, static_cast<int>(attrs.get("NORMAL").asInt())) : Result<View>(View{});
                Result<View> uv = attrs.contains("TEXCOORD_0") ? accessor(d, static_cast<int>(attrs.get("TEXCOORD_0").asInt())) : Result<View>(View{});
                if (!nrm) return nrm.error();
                if (!uv) return uv.error();
                auto base = static_cast<uint32_t>(mesh.vertexCount());
                for (size_t i = 0; i < pos->count; ++i) {
                    Vec3 p = world.transformPoint({readComponent(*pos, i, 0), readComponent(*pos, i, 1), readComponent(*pos, i, 2)});
                    Vec3 n{0, 0, 0};
                    if (nrm->data && i < nrm->count) {
                        n = normalize(normalM.transformDir({readComponent(*nrm, i, 0), readComponent(*nrm, i, 1), readComponent(*nrm, i, 2)}));
                    }
                    Vec2 t{0, 0};
                    if (uv->data && i < uv->count) t = {readComponent(*uv, i, 0), readComponent(*uv, i, 1)};
                    mesh.addVertex(p, n, t);
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
        for (const auto& c : node.get("children").elements()) {
            if (Status s = visit(static_cast<size_t>(c.asInt()), world, depth + 1); !s) return s;
        }
        return {};
    };

    std::vector<size_t> roots;
    const Json& scenes = d.json.get("scenes");
    size_t sceneIndex = static_cast<size_t>(d.json.get("scene").asInt(0));
    if (scenes.size() > sceneIndex) {
        for (const auto& n : scenes[sceneIndex].get("nodes").elements()) roots.push_back(static_cast<size_t>(n.asInt()));
    } else {
        for (size_t i = 0; i < nodes.size(); ++i) roots.push_back(i);  // no scene: treat all nodes as roots
    }
    for (size_t r : roots) {
        if (r >= nodes.size()) return bad("scene references a missing node");
        if (Status s = visit(r, Mat4{}, 0); !s) return s.error();
    }
    if (mesh.indices.empty()) return bad("no triangle geometry found");

    mesh::computeMissingNormals(mesh);
    mesh.computeBounds();
    if (normalizeMesh) mesh::normalizeToUnit(mesh);

    // First material -> base color / PBR / texture.
    const Json& mats = d.json.get("materials");
    out.materialCount = mats.size();
    if (mats.size()) {
        const Json& pbr = mats[0].get("pbrMetallicRoughness");
        const Json& bc = pbr.get("baseColorFactor");
        if (bc.size() == 4) out.baseColor = {bc[0].asFloat(), bc[1].asFloat(), bc[2].asFloat(), bc[3].asFloat()};
        out.metallic = pbr.get("metallicFactor").asFloat(1.f);
        out.roughness = pbr.get("roughnessFactor").asFloat(1.f);
        const Json& em = mats[0].get("emissiveFactor");
        if (em.size() == 3) out.emissive = {em[0].asFloat(), em[1].asFloat(), em[2].asFloat(), 1.f};
        // Texture reference -> encoded image bytes (embedded bufferView, data URI or file).
        auto image = [&](const Json& ref, int* sourceOut = nullptr) {
            GltfImport::ImageData outImg;
            if (!ref.isObject()) return outImg;
            const Json& textures = d.json.get("textures");
            auto ti = static_cast<size_t>(ref.get("index").asInt(-1));
            if (ti >= textures.size()) return outImg;
            int source = static_cast<int>(textures[ti].get("source").asInt(-1));
            if (sourceOut) *sourceOut = source;
            const Json& images = d.json.get("images");
            if (source < 0 || static_cast<size_t>(source) >= images.size()) return outImg;
            const Json& img = images[static_cast<size_t>(source)];
            if (img.contains("bufferView")) {
                const Json& bv = d.json.get("bufferViews")[static_cast<size_t>(img.get("bufferView").asInt())];
                size_t bi = static_cast<size_t>(bv.get("buffer").asInt());
                size_t off = static_cast<size_t>(bv.get("byteOffset").asInt()), len = static_cast<size_t>(bv.get("byteLength").asInt());
                if (bi < d.buffers.size() && off + len <= d.buffers[bi].size()) {
                    outImg.bytes.assign(d.buffers[bi].begin() + static_cast<std::ptrdiff_t>(off),
                                        d.buffers[bi].begin() + static_cast<std::ptrdiff_t>(off + len));
                    outImg.mime = img.get("mimeType").asString("image/png");
                }
            } else if (img.contains("uri")) {
                auto data = loadUri(img.get("uri").asString(), baseDir);
                if (data) {
                    outImg.bytes = std::move(data.value());
                    std::string u = str::lower(img.get("uri").asString());
                    outImg.mime = (u.find(".jpg") != std::string::npos || u.find(".jpeg") != std::string::npos ||
                                   u.find("image/jpeg") != std::string::npos) ? "image/jpeg" : "image/png";
                }
            }
            return outImg;
        };
        auto base = image(pbr.get("baseColorTexture"));
        out.textureBytes = std::move(base.bytes);
        out.textureMime = base.mime;
        out.normalMap = image(mats[0].get("normalTexture"));
        out.normalScale = mats[0].get("normalTexture").get("scale").asFloat(1.f);
        int mrSource = -1, occSource = -2;
        out.metallicRoughnessMap = image(pbr.get("metallicRoughnessTexture"), &mrSource);
        (void)image(mats[0].get("occlusionTexture"), &occSource);
        if (mrSource >= 0 && mrSource == occSource) {
            out.occlusionStrength = mats[0].get("occlusionTexture").get("strength").asFloat(1.f);
        }
        out.emissiveMap = image(mats[0].get("emissiveTexture"));
        if (!out.emissiveMap.empty() && out.emissive.x + out.emissive.y + out.emissive.z <= 0.f) out.emissive = {1, 1, 1, 1};
    }
    return out;
}

Result<GltfImport> loadGltf(const std::string& path, bool normalizeMesh) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) return Error::make("io_error", "cannot read " + path);
    return parseGltf(bytes, fs::path(path).parent_path().string(), normalizeMesh);
}

namespace mesh {
Result<MeshData> loadMeshFile(const std::string& path, bool normalizeMesh) {
    std::string lower = str::lower(path);
    if (lower.size() > 4 && (lower.rfind(".glb") == lower.size() - 4 || lower.rfind(".gltf") == lower.size() - 5)) {
        auto g = loadGltf(path, normalizeMesh);
        if (!g) return g.error();
        return std::move(g.value().mesh);
    }
    return loadObj(path, normalizeMesh);
}
}  // namespace mesh

}  // namespace sky
