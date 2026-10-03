#pragma once
// Tiny rigged glTF models generated in code (no binary test assets in the repo).
//
// "Strip": a vertical 0.2 x 2 m strip with two bones:
//   Root  (joint 0) at the origin        — moves vertices with y < 1
//   Upper (joint 1) at (0, 1, 0), child  — moves vertices with y > 1 (y == 1 is shared 50/50)
// Clips:
//   Bend   (LINEAR)      Upper rotates 0 -> 90 degrees about +Z over 1 s
//   Steps  (STEP)        Root translation (0,0,0) at t=0, (1,0,0) at t=1
//   Smooth (CUBICSPLINE) Root translation (0,0,0) -> (0,2,0) with zero tangents over 1 s
//   Walk   (LINEAR)      Root moves 1.5 m along +Z over 1 s (root motion), Upper sways
//   Wave   (LINEAR)      Upper rotates about +X and back over 0.5 s

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "skywalker/core/Json.h"
#include "skywalker/core/Strings.h"

namespace skytest {

class GltfBuilder {
public:
    sky::Json accessors = sky::Json::array();
    sky::Json views = sky::Json::array();
    std::vector<uint8_t> bin;

    int floats(const std::vector<float>& data, const char* type, bool minmax = false) {
        size_t comps = std::strcmp(type, "SCALAR") == 0 ? 1 : std::strcmp(type, "VEC2") == 0 ? 2 : std::strcmp(type, "VEC3") == 0 ? 3
                       : std::strcmp(type, "VEC4") == 0 ? 4 : 16;
        int view = addView(data.data(), data.size() * 4);
        sky::Json acc = sky::Json::object({{"bufferView", view}, {"componentType", 5126}, {"count", data.size() / comps}, {"type", type}});
        if (minmax && comps == 1 && !data.empty()) {
            acc["min"] = sky::Json::array({data.front()});
            acc["max"] = sky::Json::array({data.back()});
        }
        accessors.push(acc);
        return static_cast<int>(accessors.size() - 1);
    }
    int u16(const std::vector<uint16_t>& data, const char* type) {
        size_t comps = std::strcmp(type, "SCALAR") == 0 ? 1 : 4;
        int view = addView(data.data(), data.size() * 2);
        accessors.push(sky::Json::object({{"bufferView", view}, {"componentType", 5123}, {"count", data.size() / comps}, {"type", type}}));
        return static_cast<int>(accessors.size() - 1);
    }
    int u8(const std::vector<uint8_t>& data, const char* type) {
        size_t comps = std::strcmp(type, "SCALAR") == 0 ? 1 : 4;
        int view = addView(data.data(), data.size());
        accessors.push(sky::Json::object({{"bufferView", view}, {"componentType", 5121}, {"count", data.size() / comps}, {"type", type}}));
        return static_cast<int>(accessors.size() - 1);
    }
    std::string uri() const { return "data:application/octet-stream;base64," + sky::str::base64Encode(bin.data(), bin.size()); }

private:
    int addView(const void* p, size_t bytes) {
        while (bin.size() % 4) bin.push_back(0);
        size_t off = bin.size();
        bin.insert(bin.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + bytes);
        views.push(sky::Json::object({{"buffer", 0}, {"byteOffset", off}, {"byteLength", bytes}}));
        return static_cast<int>(views.size() - 1);
    }
};

struct StripOptions {
    bool u8Joints = false;      // JOINTS_0 as unsigned bytes instead of shorts
    bool twoMaterials = false;  // a second primitive (a "hat" quad on the upper bone) with its own material
    bool withMesh = true;       // false: an animation-only file (clip library)
};

inline std::string makeStripGltf(const StripOptions& o = {}) {
    GltfBuilder b;
    std::vector<float> pos, nrm;
    std::vector<uint16_t> joints;
    std::vector<uint8_t> joints8;
    std::vector<float> weights;
    for (int r = 0; r <= 4; ++r) {
        float y = 0.5f * static_cast<float>(r);
        for (float x : {-0.1f, 0.1f}) {
            pos.insert(pos.end(), {x, y, 0.f});
            nrm.insert(nrm.end(), {0.f, 0.f, 1.f});
            float w1 = y < 0.99f ? 0.f : y > 1.01f ? 1.f : 0.5f;
            joints.insert(joints.end(), {0, 1, 0, 0});
            joints8.insert(joints8.end(), {0, 1, 0, 0});
            weights.insert(weights.end(), {1.f - w1, w1, 0.f, 0.f});
        }
    }
    std::vector<uint16_t> idx;
    for (uint16_t r = 0; r < 4; ++r) {
        uint16_t a = static_cast<uint16_t>(r * 2);
        idx.insert(idx.end(), {a, static_cast<uint16_t>(a + 1), static_cast<uint16_t>(a + 2), static_cast<uint16_t>(a + 1),
                               static_cast<uint16_t>(a + 3), static_cast<uint16_t>(a + 2)});
    }
    sky::Json meshes = sky::Json::array();
    if (o.withMesh) {
        int aPos = b.floats(pos, "VEC3");
        int aNrm = b.floats(nrm, "VEC3");
        int aJ = o.u8Joints ? b.u8(joints8, "VEC4") : b.u16(joints, "VEC4");
        int aW = b.floats(weights, "VEC4");
        int aIdx = b.u16(idx, "SCALAR");
        sky::Json prims = sky::Json::array();
        prims.push(sky::Json::object({{"attributes", sky::Json::object({{"POSITION", aPos}, {"NORMAL", aNrm}, {"JOINTS_0", aJ}, {"WEIGHTS_0", aW}})},
                                      {"indices", aIdx},
                                      {"material", 0}}));
        if (o.twoMaterials) {
            // A small quad at the top, fully on the Upper bone.
            std::vector<float> hp{-0.15f, 2.f, 0.f, 0.15f, 2.f, 0.f, -0.15f, 2.3f, 0.f, 0.15f, 2.3f, 0.f};
            std::vector<float> hn{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
            std::vector<uint16_t> hj{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            std::vector<float> hw{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            std::vector<uint16_t> hi{0, 1, 2, 1, 3, 2};
            prims.push(sky::Json::object({{"attributes", sky::Json::object({{"POSITION", b.floats(hp, "VEC3")},
                                                                             {"NORMAL", b.floats(hn, "VEC3")},
                                                                             {"JOINTS_0", b.u16(hj, "VEC4")},
                                                                             {"WEIGHTS_0", b.floats(hw, "VEC4")}})},
                                          {"indices", b.u16(hi, "SCALAR")},
                                          {"material", 1}}));
        }
        meshes.push(sky::Json::object({{"name", "StripMesh"}, {"primitives", prims}}));
    }
    // Inverse bind matrices (column-major): Root = identity, Upper = translate(0, -1, 0).
    std::vector<float> ibm{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
    int aIbm = b.floats(ibm, "MAT4");

    const float s45 = std::sin(3.14159265f / 4.f), c45 = std::cos(3.14159265f / 4.f);
    sky::Json samplers = sky::Json::array();
    auto sampler = [&](const std::vector<float>& t, const std::vector<float>& v, const char* type, const char* interp) {
        samplers.push(sky::Json::object({{"input", b.floats(t, "SCALAR", true)}, {"output", b.floats(v, type)}, {"interpolation", interp}}));
        return static_cast<int>(samplers.size() - 1);
    };
    sky::Json animations = sky::Json::array();
    auto anim = [&](const char* name, sky::Json channels) {
        animations.push(sky::Json::object({{"name", name}, {"samplers", samplers}, {"channels", channels}}));
        samplers = sky::Json::array();
    };
    // Node indices: 0 = Armature, 1 = Root, 2 = Upper, 3 = Body (mesh)
    auto chan = [](int s, int node, const char* path) {
        return sky::Json::object({{"sampler", s}, {"target", sky::Json::object({{"node", node}, {"path", path}})}});
    };
    {
        int s = sampler({0.f, 1.f}, {0, 0, 0, 1, 0, 0, s45, c45}, "VEC4", "LINEAR");
        anim("Bend", sky::Json::array({chan(s, 2, "rotation")}));
    }
    {
        int s = sampler({0.f, 1.f}, {0, 0, 0, 1, 0, 0}, "VEC3", "STEP");
        anim("Steps", sky::Json::array({chan(s, 1, "translation")}));
    }
    {
        // CUBICSPLINE: per key [in-tangent, value, out-tangent]
        int s = sampler({0.f, 1.f}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0}, "VEC3", "CUBICSPLINE");
        anim("Smooth", sky::Json::array({chan(s, 1, "translation")}));
    }
    {
        int s0 = sampler({0.f, 1.f}, {0, 0, 0, 0, 0, 1.5f}, "VEC3", "LINEAR");
        const float s10 = std::sin(0.0873f), c10 = std::cos(0.0873f);
        int s1 = sampler({0.f, 0.5f, 1.f}, {0, 0, 0, 1, 0, 0, s10, c10, 0, 0, 0, 1}, "VEC4", "LINEAR");
        anim("Armature|Walk", sky::Json::array({chan(s0, 1, "translation"), chan(s1, 2, "rotation")}));
    }
    {
        const float s30 = std::sin(0.2618f), c30 = std::cos(0.2618f);
        int s = sampler({0.f, 0.25f, 0.5f}, {0, 0, 0, 1, s30, 0, 0, c30, 0, 0, 0, 1}, "VEC4", "LINEAR");
        anim("Wave", sky::Json::array({chan(s, 2, "rotation")}));
    }

    sky::Json nodes = sky::Json::array();
    nodes.push(sky::Json::object({{"name", "Armature"}, {"children", sky::Json::array({1})}}));
    nodes.push(sky::Json::object({{"name", "Root"}, {"children", sky::Json::array({2})}}));
    nodes.push(sky::Json::object({{"name", "Upper"}, {"translation", sky::Json::array({0, 1, 0})}}));
    sky::Json sceneNodes = sky::Json::array({0});
    if (o.withMesh) {
        nodes.push(sky::Json::object({{"name", "Body"}, {"mesh", 0}, {"skin", 0}}));
        sceneNodes.push(3);
    }
    sky::Json doc = sky::Json::object(
        {{"asset", sky::Json::object({{"version", "2.0"}})},
         {"scene", 0},
         {"scenes", sky::Json::array({sky::Json::object({{"nodes", sceneNodes}})})},
         {"nodes", nodes},
         {"skins", sky::Json::array({sky::Json::object({{"joints", sky::Json::array({1, 2})}, {"inverseBindMatrices", aIbm}, {"skeleton", 1}})})},
         {"animations", animations},
         {"materials", sky::Json::array({sky::Json::object({{"name", "strip_body"}, {"pbrMetallicRoughness", sky::Json::object({{"baseColorFactor", sky::Json::array({0.8, 0.3, 0.2, 1})}})}}),
                                         sky::Json::object({{"name", "strip_hat"}, {"pbrMetallicRoughness", sky::Json::object({{"baseColorFactor", sky::Json::array({0.2, 0.3, 0.8, 1})}})}})})}});
    if (o.withMesh) doc["meshes"] = meshes;
    doc["accessors"] = b.accessors;
    doc["bufferViews"] = b.views;
    doc["buffers"] = sky::Json::array({sky::Json::object({{"byteLength", b.bin.size()}, {"uri", b.uri()}})});
    return doc.dump();
}

inline std::vector<uint8_t> bytesOfString(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace skytest
