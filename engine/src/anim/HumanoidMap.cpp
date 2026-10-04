#include "skywalker/anim/HumanoidMap.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "skywalker/core/Strings.h"

namespace sky::anim {

namespace {

constexpr const char* kSlotNames[kHumanBones] = {
    "hips",          "spine",        "chest",         "upperChest",    "neck",          "head",
    "leftShoulder",  "leftUpperArm", "leftLowerArm",  "leftHand",      "rightShoulder", "rightUpperArm",
    "rightLowerArm", "rightHand",    "leftUpperLeg",  "leftLowerLeg",  "leftFoot",      "leftToes",
    "rightUpperLeg", "rightLowerLeg", "rightFoot",    "rightToes",
};

constexpr HumanBone kRequired[] = {HumanBone::Hips,          HumanBone::Spine,         HumanBone::Head,
                                   HumanBone::LeftUpperArm,  HumanBone::LeftLowerArm,  HumanBone::LeftHand,
                                   HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightHand,
                                   HumanBone::LeftUpperLeg,  HumanBone::LeftLowerLeg,  HumanBone::LeftFoot,
                                   HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot};

enum class Side { None, Left, Right };

/// Splits a bone name into lower-case words at separators, case changes and digit boundaries.
std::vector<std::string> tokens(std::string_view raw) {
    size_t cut = raw.find_last_of(":|");
    if (cut != std::string_view::npos) raw = raw.substr(cut + 1);
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back(str::lower(cur));
        cur.clear();
    };
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            flush();
            continue;
        }
        if (!cur.empty()) {
            char p = cur.back();
            bool lowerToUpper = std::islower(static_cast<unsigned char>(p)) && std::isupper(static_cast<unsigned char>(c));
            bool digitEdge = std::isdigit(static_cast<unsigned char>(p)) != std::isdigit(static_cast<unsigned char>(c));
            // "LThigh": a single upper-case letter followed by an upper+lower pair starts a new word.
            bool acronymEnd = std::isupper(static_cast<unsigned char>(p)) && std::isupper(static_cast<unsigned char>(c)) &&
                              i + 1 < raw.size() && std::islower(static_cast<unsigned char>(raw[i + 1]));
            if (lowerToUpper || digitEdge || acronymEnd) flush();
        }
        cur += c;
    }
    flush();
    return out;
}

struct Parsed {
    Side side = Side::None;
    std::string key;  // body-part words joined ("upleg", "forearm", "spine")
    bool skip = false;  // fingers, twist / helper / end bones
};

bool isNoise(const std::string& t) {
    static const char* kNoise[] = {"def", "mch", "org", "bip", "bip01", "bip001", "b", "bn", "jnt", "joint", "bone",
                                   "mixamorig", "cc", "base", "ctrl", "sk", "rig", "j"};
    for (const char* n : kNoise) {
        if (t == n) return true;
    }
    return std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

bool isSkip(const std::string& t) {
    static const char* kSkip[] = {"thumb", "index", "middle", "ring", "pinky", "little", "finger", "twist", "roll", "ik", "pole",
                                  "target", "end", "nub", "top", "tip", "helper", "corrective", "jaw", "eye", "lid", "brow",
                                  "tongue", "teeth", "breast", "weapon", "prop", "attach", "socket", "ear", "tail", "wing"};
    for (const char* s : kSkip) {
        if (t == s) return true;
    }
    return false;
}

Parsed parse(std::string_view name) {
    Parsed p;
    std::vector<std::string> t = tokens(name);
    // "toe base" keeps "base" (mixamo ToeBase): strip it after skip checks only when other words remain.
    std::string key;
    for (size_t i = 0; i < t.size(); ++i) {
        const std::string& w = t[i];
        if (isSkip(w)) p.skip = true;
        if (w == "left" || w == "l") {
            p.side = Side::Left;
            continue;
        }
        if (w == "right" || w == "r") {
            p.side = Side::Right;
            continue;
        }
        if (isNoise(w)) continue;
        key += w;
    }
    p.key = key;
    return p;
}

std::optional<HumanBone> slotForKey(const std::string& k, Side side) {
    auto sided = [&](HumanBone l, HumanBone r) -> std::optional<HumanBone> {
        if (side == Side::Left) return l;
        if (side == Side::Right) return r;
        return std::nullopt;
    };
    if (side == Side::None) {
        if (k == "hips" || k == "pelvis" || k == "hip") return HumanBone::Hips;
        if (k == "head") return HumanBone::Head;
        if (str::startsWith(k, "neck")) return HumanBone::Neck;
        return std::nullopt;  // spine-like bones are collected separately
    }
    if (k == "shoulder" || k == "clavicle" || k == "collar" || k == "collarbone") {
        return sided(HumanBone::LeftShoulder, HumanBone::RightShoulder);
    }
    if (k == "arm" || k == "upperarm" || k == "uparm" || k == "humerus") return sided(HumanBone::LeftUpperArm, HumanBone::RightUpperArm);
    if (k == "forearm" || k == "lowerarm" || k == "elbow" || k == "loarm") return sided(HumanBone::LeftLowerArm, HumanBone::RightLowerArm);
    if (k == "hand" || k == "wrist" || k == "fist" || k == "palm") return sided(HumanBone::LeftHand, HumanBone::RightHand);
    if (k == "upleg" || k == "thigh" || k == "upperleg" || k == "femur" || k == "hipjoint")
        return sided(HumanBone::LeftUpperLeg, HumanBone::RightUpperLeg);
    if (k == "leg" || k == "calf" || k == "shin" || k == "lowerleg" || k == "knee" || k == "loleg")
        return sided(HumanBone::LeftLowerLeg, HumanBone::RightLowerLeg);
    if (k == "foot" || k == "ankle") return sided(HumanBone::LeftFoot, HumanBone::RightFoot);
    if (k == "toe" || k == "toes" || k == "toebase" || k == "ball") return sided(HumanBone::LeftToes, HumanBone::RightToes);
    return std::nullopt;
}

bool spineLike(const std::string& k) {
    return str::startsWith(k, "spine") || k == "abdomen" || k == "torso" || k == "chest" || k == "upperchest" ||
           k == "belly" || k == "waist" || k == "ribcage";
}

int depthOf(const Skeleton& sk, int b) {
    int d = 0;
    for (int p = sk.bones[static_cast<size_t>(b)].parent; p >= 0; p = sk.bones[static_cast<size_t>(p)].parent) ++d;
    return d;
}

int parentOf(const Skeleton& sk, int b) { return b >= 0 ? sk.bones[static_cast<size_t>(b)].parent : -1; }

int commonAncestor(const Skeleton& sk, int a, int b) {
    if (a < 0 || b < 0) return -1;
    for (int x = a; x >= 0; x = parentOf(sk, x)) {
        if (sk.isDescendant(b, x)) return x;
    }
    return -1;
}

}  // namespace

const char* humanBoneName(HumanBone b) {
    size_t i = static_cast<size_t>(b);
    return i < kHumanBones ? kSlotNames[i] : "";
}

std::optional<HumanBone> humanBoneFromName(std::string_view name) {
    for (size_t i = 0; i < kHumanBones; ++i) {
        if (name == kSlotNames[i]) return static_cast<HumanBone>(i);
    }
    // Forgiving: case-insensitive, "left_upper_arm" style.
    std::string n = normalizeBoneName(name);
    for (size_t i = 0; i < kHumanBones; ++i) {
        if (n == normalizeBoneName(kSlotNames[i])) return static_cast<HumanBone>(i);
    }
    return std::nullopt;
}

std::vector<std::string> humanBoneNames() { return {std::begin(kSlotNames), std::end(kSlotNames)}; }

std::optional<HumanBone> humanParent(HumanBone b) {
    using H = HumanBone;
    switch (b) {
        case H::Hips: return std::nullopt;
        case H::Spine: return H::Hips;
        case H::Chest: return H::Spine;
        case H::UpperChest: return H::Chest;
        case H::Neck: return H::UpperChest;
        case H::Head: return H::Neck;
        case H::LeftShoulder:
        case H::RightShoulder: return H::UpperChest;
        case H::LeftUpperArm: return H::LeftShoulder;
        case H::RightUpperArm: return H::RightShoulder;
        case H::LeftLowerArm: return H::LeftUpperArm;
        case H::RightLowerArm: return H::RightUpperArm;
        case H::LeftHand: return H::LeftLowerArm;
        case H::RightHand: return H::RightLowerArm;
        case H::LeftUpperLeg:
        case H::RightUpperLeg: return H::Hips;
        case H::LeftLowerLeg: return H::LeftUpperLeg;
        case H::RightLowerLeg: return H::RightUpperLeg;
        case H::LeftFoot: return H::LeftLowerLeg;
        case H::RightFoot: return H::RightLowerLeg;
        case H::LeftToes: return H::LeftFoot;
        case H::RightToes: return H::RightFoot;
        case H::Count: break;
    }
    return std::nullopt;
}

std::optional<HumanBone> humanChild(HumanBone b) {
    using H = HumanBone;
    switch (b) {
        case H::Hips: return H::Spine;
        case H::Spine: return H::Chest;
        case H::Chest: return H::UpperChest;
        case H::UpperChest: return H::Neck;
        case H::Neck: return H::Head;
        case H::LeftShoulder: return H::LeftUpperArm;
        case H::RightShoulder: return H::RightUpperArm;
        case H::LeftUpperArm: return H::LeftLowerArm;
        case H::RightUpperArm: return H::RightLowerArm;
        case H::LeftLowerArm: return H::LeftHand;
        case H::RightLowerArm: return H::RightHand;
        case H::LeftUpperLeg: return H::LeftLowerLeg;
        case H::RightUpperLeg: return H::RightLowerLeg;
        case H::LeftLowerLeg: return H::LeftFoot;
        case H::RightLowerLeg: return H::RightFoot;
        case H::LeftFoot: return H::LeftToes;
        case H::RightFoot: return H::RightToes;
        default: return std::nullopt;
    }
}

std::string normalizeBoneName(std::string_view name) {
    size_t cut = name.find_last_of(":|");
    if (cut != std::string_view::npos) name = name.substr(cut + 1);
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool HumanoidMap::complete() const {
    return std::all_of(std::begin(kRequired), std::end(kRequired), [&](HumanBone b) { return (*this)[b] >= 0; });
}

std::vector<std::string> HumanoidMap::missing() const {
    std::vector<std::string> out;
    for (HumanBone b : kRequired) {
        if ((*this)[b] < 0) out.emplace_back(humanBoneName(b));
    }
    return out;
}

size_t HumanoidMap::mapped() const {
    return static_cast<size_t>(std::count_if(bones.begin(), bones.end(), [](int b) { return b >= 0; }));
}

Json HumanoidMap::toJson(const Skeleton& sk) const {
    Json map = Json::object();
    for (size_t i = 0; i < kHumanBones; ++i) {
        int b = bones[i];
        if (b >= 0 && static_cast<size_t>(b) < sk.bones.size()) map[kSlotNames[i]] = sk.bones[static_cast<size_t>(b)].name;
    }
    Json miss = Json::array();
    for (const auto& m : missing()) miss.push(m);
    Json warn = Json::array();
    for (const auto& w : warnings) warn.push(w);
    return Json::object({{"convention", convention},
                         {"confidence", std::round(confidence * 100.f) / 100.f},
                         {"complete", complete()},
                         {"bones", map},
                         {"missing", miss},
                         {"warnings", warn}});
}

HumanoidMap detectHumanoid(const Skeleton& sk) {
    HumanoidMap m;
    const size_t n = sk.bones.size();
    if (n == 0) {
        m.warnings.push_back("the skeleton has no bones");
        return m;
    }
    std::vector<Mat4> rest;
    computeGlobals(sk, restPose(sk), rest);
    std::vector<Parsed> parsed(n);
    int mixamoHits = 0, ueHits = 0;
    for (size_t i = 0; i < n; ++i) {
        parsed[i] = parse(sk.bones[i].name);
        std::string lower = str::lower(sk.bones[i].name);
        if (lower.find("mixamorig") != std::string::npos || lower == "leftupleg" || lower == "leftforearm") ++mixamoHits;
        if (lower == "pelvis" || lower == "thigh_l" || lower == "calf_l" || lower == "upperarm_l" || lower == "clavicle_l") ++ueHits;
    }
    m.convention = mixamoHits > 0 ? "mixamo" : ueHits > 0 ? "ue" : "generic";

    // 1. Names: the shallowest candidate per slot wins.
    std::vector<int> spines;
    std::array<std::vector<int>, kHumanBones> sideless;  // sided parts without a side word (resolved by position)
    for (size_t i = 0; i < n; ++i) {
        const Parsed& p = parsed[i];
        if (p.skip || p.key.empty()) continue;
        int b = static_cast<int>(i);
        if (p.side == Side::None && spineLike(p.key)) {
            spines.push_back(b);
            continue;
        }
        auto slot = slotForKey(p.key, p.side);
        if (!slot && p.side == Side::None) {
            // A sided part named without a side ("Thigh" twice): remember it for the position pass.
            if (auto l = slotForKey(p.key, Side::Left)) sideless[static_cast<size_t>(*l)].push_back(b);
            continue;
        }
        if (!slot) continue;
        int& cur = m.at(*slot);
        if (cur < 0 || depthOf(sk, b) < depthOf(sk, cur)) cur = b;
    }
    // Sideless pairs: the one at +X is the character's left (glTF characters face +Z).
    for (size_t s = 0; s < kHumanBones; ++s) {
        auto& cands = sideless[s];
        if (cands.size() < 2) continue;
        HumanBone left = static_cast<HumanBone>(s);
        // Matching right slot: the slot names differ only by the side prefix.
        std::string rightName = std::string("right") + (kSlotNames[s] + 4);
        auto right = humanBoneFromName(rightName);
        if (!right || m[left] >= 0 || m[*right] >= 0) continue;
        std::sort(cands.begin(), cands.end(), [&](int a, int b) { return depthOf(sk, a) < depthOf(sk, b); });
        int a = cands[0], b = cands[1];
        if (rest[static_cast<size_t>(a)].translation().x < rest[static_cast<size_t>(b)].translation().x) std::swap(a, b);
        m.at(left) = a;
        m.at(*right) = b;
    }

    // 2. Topology: infer missing chain links from the hierarchy.
    using H = HumanBone;
    // A limb chain hangs from its end bone: the lower part is the end's nearest named parent, the
    // upper part the lower part's (twist and helper bones in between are skipped).
    auto namedParent = [&](int b) {
        int p = parentOf(sk, b);
        while (p >= 0 && parsed[static_cast<size_t>(p)].skip) p = parentOf(sk, p);
        return p;
    };
    auto fixChain = [&](H end, H lower, H upper) {
        if (m[end] < 0) return;
        int p = namedParent(m[end]);
        if (p >= 0 && (m[lower] < 0 || m[lower] == m[end] || !sk.isDescendant(m[end], m[lower]) || sk.isDescendant(p, m[lower]))) {
            if (m[lower] >= 0 && m[lower] != p && sk.isDescendant(p, m[lower]) && m[upper] < 0) m.at(upper) = m[lower];
            m.at(lower) = p;
        }
        if (m[lower] < 0) return;
        int q = namedParent(m[lower]);
        if (q >= 0 && (m[upper] < 0 || m[upper] == m[lower] || !sk.isDescendant(m[lower], m[upper]))) m.at(upper) = q;
    };
    for (int side = 0; side < 2; ++side) {
        H hand = side ? H::RightHand : H::LeftHand, lower = side ? H::RightLowerArm : H::LeftLowerArm;
        H upper = side ? H::RightUpperArm : H::LeftUpperArm;
        H foot = side ? H::RightFoot : H::LeftFoot, shin = side ? H::RightLowerLeg : H::LeftLowerLeg;
        H thigh = side ? H::RightUpperLeg : H::LeftUpperLeg, toes = side ? H::RightToes : H::LeftToes;
        fixChain(hand, lower, upper);
        fixChain(foot, shin, thigh);
        if (m[toes] < 0 && m[foot] >= 0) {
            for (size_t i = 0; i < n; ++i) {
                if (sk.bones[i].parent == m[foot] && !parsed[i].skip) {
                    m.at(toes) = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    if (m[H::Hips] < 0) {
        int ca = commonAncestor(sk, m[H::LeftUpperLeg], m[H::RightUpperLeg]);
        if (ca >= 0) {
            m.at(H::Hips) = ca;
            m.warnings.push_back("hips inferred from the legs (common ancestor): " + sk.bones[static_cast<size_t>(ca)].name);
        }
    }
    if (m[H::Head] < 0 && m[H::Neck] >= 0) {
        for (size_t i = 0; i < n; ++i) {
            if (sk.bones[i].parent == m[H::Neck] && !parsed[i].skip) {
                m.at(H::Head) = static_cast<int>(i);
                break;
            }
        }
    }
    if (m[H::Neck] < 0 && m[H::Head] >= 0) {
        int p = parentOf(sk, m[H::Head]);
        if (p >= 0 && std::find(spines.begin(), spines.end(), p) == spines.end() && p != m[H::Hips]) m.at(H::Neck) = p;
    }
    // The chest: where both arms (or the neck) branch off.
    int armsRoot = commonAncestor(sk, m[H::LeftUpperArm], m[H::RightUpperArm]);
    int top = m[H::Neck] >= 0 ? parentOf(sk, m[H::Neck]) : armsRoot;
    if (top < 0) top = armsRoot;
    // Spine chain: spine-like bones between the hips and the top (or every spine-like bone by depth).
    std::vector<int> chain;
    if (m[H::Hips] >= 0 && top >= 0 && sk.isDescendant(top, m[H::Hips])) {
        for (int b = top; b >= 0 && b != m[H::Hips]; b = parentOf(sk, b)) chain.push_back(b);
        std::reverse(chain.begin(), chain.end());
    } else {
        chain = spines;
        std::sort(chain.begin(), chain.end(), [&](int a, int b) { return depthOf(sk, a) < depthOf(sk, b); });
    }
    if (!chain.empty()) {
        m.at(H::Spine) = chain.front();
        if (chain.size() == 2) m.at(H::Chest) = chain[1];
        if (chain.size() >= 3) {
            m.at(H::Chest) = chain[chain.size() / 2];
            m.at(H::UpperChest) = chain.back();
        }
    }
    // Shoulders: an arm's parent that is not on the spine chain.
    for (auto [arm, sh] : {std::pair{H::LeftUpperArm, H::LeftShoulder}, std::pair{H::RightUpperArm, H::RightShoulder}}) {
        if (m[sh] >= 0 || m[arm] < 0) continue;
        int p = parentOf(sk, m[arm]);
        if (p >= 0 && std::find(chain.begin(), chain.end(), p) == chain.end() && p != m[H::Neck]) m.at(sh) = p;
    }

    // 3. Sanity: left parts should be at +X in the rest pose.
    auto x = [&](H b) { return m[b] >= 0 ? rest[static_cast<size_t>(m[b])].translation().x : 0.f; };
    if (m[H::LeftHand] >= 0 && m[H::RightHand] >= 0 && x(H::LeftHand) < x(H::RightHand)) {
        m.warnings.push_back("left and right hands look mirrored in the rest pose (left should be at +X when facing +Z)");
    }
    if (m[H::LeftFoot] >= 0 && m[H::RightFoot] >= 0 && x(H::LeftFoot) < x(H::RightFoot)) {
        m.warnings.push_back("left and right feet look mirrored in the rest pose");
    }
    size_t found = 0;
    for (H b : kRequired) found += m[b] >= 0 ? 1 : 0;
    m.confidence = static_cast<float>(found) / static_cast<float>(std::size(kRequired));
    m.confidence *= std::max(0.f, 1.f - 0.15f * static_cast<float>(m.warnings.size()));
    if (!m.complete()) {
        std::string miss;
        for (const auto& s : m.missing()) miss += (miss.empty() ? "" : ", ") + s;
        m.warnings.push_back("not a complete humanoid (missing " + miss + ")");
    }
    return m;
}

Status applyHumanoidOverrides(HumanoidMap& m, const Skeleton& sk, const Json& overrides) {
    if (overrides.isNull()) return {};
    if (!overrides.isObject()) {
        return Error::make("invalid_argument", "bone map overrides must be an object {slot: bone}",
                           "e.g. {\"leftHand\": \"hand_l\", \"rightHand\": \"hand_r\"}");
    }
    for (const auto& [slotName, value] : overrides.members()) {
        auto slot = humanBoneFromName(slotName);
        if (!slot) {
            std::string near = str::closest(slotName, humanBoneNames(), 4);
            return Error::make("unknown_slot", "unknown humanoid slot \"" + slotName + "\"",
                               near.empty() ? "slots: hips, spine, chest, upperChest, neck, head, leftUpperArm, ... rightToes"
                                            : "did you mean \"" + near + "\"?");
        }
        if (value.isNull() || (value.isString() && value.asString().empty())) {
            m.at(*slot) = -1;
            continue;
        }
        int b = sk.find(value.asString());
        if (b < 0) {
            std::string near = str::closest(value.asString(), sk.names(), 4);
            return Error::make("unknown_bone", "no bone \"" + value.asString() + "\" for slot " + slotName,
                               near.empty() ? "list bones with animation_list {bones: true}" : "did you mean \"" + near + "\"?");
        }
        m.at(*slot) = b;
    }
    m.convention = "custom";
    size_t found = 0;
    for (HumanBone b : kRequired) found += m[b] >= 0 ? 1 : 0;
    m.confidence = static_cast<float>(found) / static_cast<float>(std::size(kRequired));
    std::erase_if(m.warnings, [](const std::string& w) { return str::startsWith(w, "not a complete humanoid"); });
    return {};
}

}  // namespace sky::anim
