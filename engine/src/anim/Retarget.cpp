#include "skywalker/anim/Retarget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

#include "skywalker/ecs/Reflection.h"

namespace sky::anim {

namespace {

Vec3 posOf(const std::vector<Mat4>& g, int b) { return g[static_cast<size_t>(b)].translation(); }

/// Columns: the rig's left, up and forward axes (a right-handed frame: left x up = forward).
struct Frame {
    Vec3 left, up, fwd;
    bool valid = false;
};

Frame rigFrame(const HumanoidMap& m, const std::vector<Mat4>& rest) {
    Frame f;
    int hips = m[HumanBone::Hips], head = m[HumanBone::Head];
    int la = m[HumanBone::LeftUpperArm], ra = m[HumanBone::RightUpperArm];
    if (hips < 0 || head < 0 || la < 0 || ra < 0) return f;
    Vec3 up = posOf(rest, head) - posOf(rest, hips);
    Vec3 left = posOf(rest, la) - posOf(rest, ra);
    if (length(up) < 1e-6f || length(left) < 1e-6f) return f;
    f.up = normalize(up);
    f.left = left - f.up * dot(left, f.up);
    if (length(f.left) < 1e-6f) return f;
    f.left = normalize(f.left);
    f.fwd = cross(f.left, f.up);
    f.valid = true;
    return f;
}

Mat4 frameMatrix(const Frame& f) {
    Mat4 m;
    m.at(0, 0) = f.left.x, m.at(0, 1) = f.left.y, m.at(0, 2) = f.left.z;
    m.at(1, 0) = f.up.x, m.at(1, 1) = f.up.y, m.at(1, 2) = f.up.z;
    m.at(2, 0) = f.fwd.x, m.at(2, 1) = f.fwd.y, m.at(2, 2) = f.fwd.z;
    return m;
}

/// Snaps a rotation to the nearest axis-aligned one when every axis is within ~20 degrees of it:
/// two rigs that are both "Y up, facing +Z" align exactly instead of by their slight asymmetries.
Quat snapRotation(Quat q) {
    Vec3 axes[3] = {q.rotate({1, 0, 0}), q.rotate({0, 1, 0}), q.rotate({0, 0, 1})};
    const float cosLimit = std::cos(radians(20.f));
    Vec3 snapped[3];
    for (int i = 0; i < 3; ++i) {
        Vec3 a = axes[i];
        Vec3 best{0, 0, 0};
        float bestDot = -2.f;
        for (Vec3 c : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
            float d = dot(a, c);
            if (d > bestDot) {
                bestDot = d;
                best = c;
            }
        }
        if (bestDot < cosLimit) return q;
        snapped[i] = best;
    }
    if (dot(cross(snapped[0], snapped[1]), snapped[2]) < 0.5f) return q;  // not a proper rotation
    Mat4 m;
    for (int c = 0; c < 3; ++c) {
        m.at(c, 0) = snapped[c].x;
        m.at(c, 1) = snapped[c].y;
        m.at(c, 2) = snapped[c].z;
    }
    return Quat::fromMatrix(m);
}

/// The next slot below `k` that both maps have (upper chest missing -> neck), else nothing.
std::optional<HumanBone> nextShared(const HumanoidMap& a, const HumanoidMap& b, HumanBone k) {
    for (auto c = humanChild(k); c; c = humanChild(*c)) {
        if (a[*c] >= 0 && b[*c] >= 0) return c;
    }
    return std::nullopt;
}

float legLength(const HumanoidMap& m, const std::vector<Mat4>& rest) {
    float sum = 0.f;
    int legs = 0;
    for (auto [u, l, f] : {std::tuple{HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot},
                           std::tuple{HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot}}) {
        if (m[u] < 0 || m[l] < 0 || m[f] < 0) continue;
        sum += distance(posOf(rest, m[u]), posOf(rest, m[l])) + distance(posOf(rest, m[l]), posOf(rest, m[f]));
        ++legs;
    }
    return legs ? sum / static_cast<float>(legs) : 0.f;
}

bool isAncestor(const Skeleton& sk, int ancestor, int bone) { return ancestor >= 0 && bone >= 0 && ancestor != bone && sk.isDescendant(bone, ancestor); }

}  // namespace

Json RetargetSetup::toJson(const Skeleton& src, const Skeleton& dst) const {
    Json w = Json::array();
    for (const auto& s : warnings) w.push(s);
    Vec3 e = eulerDegFromQuat(align);
    return Json::object({{"source", source.toJson(src)},
                         {"target", target.toJson(dst)},
                         {"scale", std::round(scale * 1000.f) / 1000.f},
                         {"rigAlignmentDeg", reflect::vec3ToJson({std::round(e.x), std::round(e.y), std::round(e.z)})},
                         {"sourceRoot", sourceRoot >= 0 ? Json(src.bones[static_cast<size_t>(sourceRoot)].name) : Json()},
                         {"targetRoot", targetRoot >= 0 ? Json(dst.bones[static_cast<size_t>(targetRoot)].name) : Json()},
                         {"warnings", w}});
}

Result<RetargetSetup> prepareRetarget(const Skeleton& src, int srcRoot, const Skeleton& dst, int dstRoot, const RetargetOptions& o) {
    RetargetSetup s;
    s.source = detectHumanoid(src);
    s.target = detectHumanoid(dst);
    if (Status st = applyHumanoidOverrides(s.source, src, o.sourceMap); !st) return st.error();
    if (Status st = applyHumanoidOverrides(s.target, dst, o.targetMap); !st) return st.error();
    for (auto [m, what] : {std::pair{&s.source, "source"}, std::pair{&s.target, "target"}}) {
        if (!m->retargetable()) {
            std::string miss;
            for (const auto& x : m->missing(true)) miss += (miss.empty() ? "" : ", ") + x;
            return Error::make("not_humanoid", std::string("the ") + what + " skeleton is not a complete humanoid (missing " + miss + ")",
                               std::string("map the slots by hand with ") + what + "_map, e.g. {\"leftHand\": \"<bone>\"}; "
                               "character_inspect / animation_retarget {preview: true} show what was detected");
        }
    }
    computeGlobals(src, restPose(src), s.sourceRest);
    computeGlobals(dst, restPose(dst), s.targetRest);
    s.sourceRoot = isAncestor(src, srcRoot, s.source[HumanBone::Hips]) ? srcRoot : -1;
    s.targetRoot = isAncestor(dst, dstRoot, s.target[HumanBone::Hips]) ? dstRoot : -1;

    Frame fs = rigFrame(s.source, s.sourceRest), ft = rigFrame(s.target, s.targetRest);
    if (fs.valid && ft.valid) {
        Quat qs = Quat::fromMatrix(frameMatrix(fs)), qt = Quat::fromMatrix(frameMatrix(ft));
        s.align = snapRotation((qt * qs.conjugate()).normalized());
    } else {
        s.warnings.push_back("could not measure the rigs' orientation; assuming both are Y-up and face +Z");
    }

    // Rest-pose alignment: each target limb is turned onto the source's rest direction first
    // (T-pose vs A-pose), so the source motion relative to its rest lands on the same pose.
    for (size_t i = 0; i < kHumanBones; ++i) s.restAlign[i] = Quat{};
    float maxAngle = 0.f;
    std::string maxSlot;
    for (size_t i = 0; i < kHumanBones; ++i) {
        HumanBone k = static_cast<HumanBone>(i);
        if (k == HumanBone::Hips) continue;
        if (s.source[k] < 0 || s.target[k] < 0) continue;
        auto c = nextShared(s.source, s.target, k);
        if (!c) {
            // Ends (hands, head, toes) keep their parent's alignment.
            for (auto p = humanParent(k); p; p = humanParent(*p)) {
                if (s.source[*p] >= 0 && s.target[*p] >= 0) {
                    s.restAlign[i] = s.restAlign[static_cast<size_t>(*p)];
                    break;
                }
            }
            continue;
        }
        Vec3 ds = s.align.rotate(posOf(s.sourceRest, s.source[*c]) - posOf(s.sourceRest, s.source[k]));
        Vec3 dt = posOf(s.targetRest, s.target[*c]) - posOf(s.targetRest, s.target[k]);
        if (length(ds) < 1e-6f || length(dt) < 1e-6f) continue;
        s.restAlign[i] = Quat::fromTo(dt, ds);
        float ang = degrees(s.restAlign[i].angle());
        if (ang > maxAngle) {
            maxAngle = ang;
            maxSlot = humanBoneName(k);
        }
    }
    if (maxAngle > 25.f) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "rest poses differ (up to %.0f deg at %s, e.g. T-pose vs A-pose): aligned", maxAngle,
                      maxSlot.c_str());
        s.warnings.push_back(buf);
    }
    float ls = legLength(s.source, s.sourceRest), lt = legLength(s.target, s.targetRest);
    if (ls > 1e-5f && lt > 1e-5f) {
        s.scale = lt / ls;
    } else {
        float hs = std::fabs(posOf(s.sourceRest, s.source[HumanBone::Hips]).y), ht = std::fabs(posOf(s.targetRest, s.target[HumanBone::Hips]).y);
        s.scale = hs > 1e-5f && ht > 1e-5f ? ht / hs : 1.f;
        s.warnings.push_back("no leg lengths: translation scaled by hips height");
    }
    return s;
}

void retargetPose(const RetargetSetup& s, const Skeleton& src, const Pose& srcPose, const Skeleton& dst, Pose& out, bool translation) {
    std::vector<Mat4> gs;
    computeGlobals(src, srcPose, gs);
    const size_t n = dst.bones.size();
    out = restPose(dst);
    std::vector<Mat4> gt(n);
    // Target bone -> slot.
    std::vector<int> slotOf(n, -1);
    for (size_t i = 0; i < kHumanBones; ++i) {
        int b = s.target.bones[i];
        if (b >= 0 && s.source.bones[i] >= 0) slotOf[static_cast<size_t>(b)] = static_cast<int>(i);
    }
    const Quat A = s.align, Ainv = s.align.conjugate();
    const int hipsS = s.source[HumanBone::Hips], hipsT = s.target[HumanBone::Hips];
    auto motionTranslation = [&](int boneS) {
        return A.rotate(posOf(gs, boneS) - posOf(s.sourceRest, boneS)) * s.scale;
    };
    for (size_t b = 0; b < n; ++b) {
        const int p = dst.bones[b].parent;
        const Mat4 parentG = p >= 0 ? gt[static_cast<size_t>(p)] : Mat4{};
        const Quat parentQ = p >= 0 ? rotationOf(parentG) : Quat{};
        Trs& local = out[b];
        int srcBone = -1;
        Quat align;
        if (slotOf[b] >= 0) {
            srcBone = s.source.bones[static_cast<size_t>(slotOf[b])];
            align = s.restAlign[static_cast<size_t>(slotOf[b])];
        } else if (static_cast<int>(b) == s.targetRoot && s.sourceRoot >= 0) {
            srcBone = s.sourceRoot;
        }
        if (srcBone >= 0) {
            Quat delta = A * rotationOf(gs[static_cast<size_t>(srcBone)]) * rotationOf(s.sourceRest[static_cast<size_t>(srcBone)]).conjugate() * Ainv;
            Quat q = (delta * align * rotationOf(s.targetRest[b])).normalized();
            local.r = (parentQ.conjugate() * q).normalized();
        }
        if (translation) {
            int moving = static_cast<int>(b) == hipsT ? hipsS : (static_cast<int>(b) == s.targetRoot && s.sourceRoot >= 0 ? s.sourceRoot : -1);
            if (moving >= 0) {
                Vec3 world = posOf(s.targetRest, static_cast<int>(b)) + motionTranslation(moving);
                local.t = p >= 0 ? parentG.inverse().transformPoint(world) : world;
            }
        }
        gt[b] = p >= 0 ? parentG * local.matrix() : local.matrix();
    }
}

Clip retargetClipPose(const RetargetSetup& s, const Skeleton& src, const Clip& clip, const Skeleton& dst, const RetargetOptions& o) {
    Clip out;
    out.name = clip.name;
    out.duration = clip.duration;
    const float fps = std::clamp(o.fps, 1.f, 240.f);
    const int frames = std::max(2, static_cast<int>(std::ceil(clip.duration * fps)) + 1);
    // Which target bones get channels: mapped slots (and the root above the hips).
    std::vector<int> rotBones, moveBones;
    for (size_t i = 0; i < kHumanBones; ++i) {
        if (s.target.bones[i] >= 0 && s.source.bones[i] >= 0) rotBones.push_back(s.target.bones[i]);
    }
    if (s.targetRoot >= 0 && s.sourceRoot >= 0) rotBones.push_back(s.targetRoot);
    std::sort(rotBones.begin(), rotBones.end());
    rotBones.erase(std::unique(rotBones.begin(), rotBones.end()), rotBones.end());
    if (o.translation) {
        moveBones.push_back(s.target[HumanBone::Hips]);
        if (s.targetRoot >= 0 && s.sourceRoot >= 0) moveBones.push_back(s.targetRoot);
    }
    std::vector<Channel> rot(rotBones.size()), mov(moveBones.size());
    for (size_t i = 0; i < rotBones.size(); ++i) {
        rot[i].bone = rotBones[i];
        rot[i].path = Path::Rotation;
    }
    for (size_t i = 0; i < moveBones.size(); ++i) {
        mov[i].bone = moveBones[i];
        mov[i].path = Path::Translation;
    }
    Pose sp, tp;
    for (int f = 0; f < frames; ++f) {
        float t = std::min(clip.duration, static_cast<float>(f) / fps);
        if (f == frames - 1) t = clip.duration;
        if (f > 0 && !rot.empty() && !rot[0].times.empty() && t <= rot[0].times.back()) continue;  // keep times increasing
        sp = restPose(src);
        sampleClip(clip, t, sp);
        retargetPose(s, src, sp, dst, tp, o.translation);
        for (auto& ch : rot) {
            Quat q = tp[static_cast<size_t>(ch.bone)].r;
            // Keep consecutive keys on the same hemisphere (shortest slerp between keys).
            if (!ch.values.empty()) {
                const float* prev = &ch.values[ch.values.size() - 4];
                if (prev[0] * q.x + prev[1] * q.y + prev[2] * q.z + prev[3] * q.w < 0.f) q = Quat{-q.x, -q.y, -q.z, -q.w};
            }
            ch.times.push_back(t);
            ch.values.insert(ch.values.end(), {q.x, q.y, q.z, q.w});
        }
        for (auto& ch : mov) {
            Vec3 v = tp[static_cast<size_t>(ch.bone)].t;
            ch.times.push_back(t);
            ch.values.insert(ch.values.end(), {v.x, v.y, v.z});
        }
    }
    for (auto& ch : rot) out.channels.push_back(std::move(ch));
    for (auto& ch : mov) out.channels.push_back(std::move(ch));
    return out;
}

float retargetStretch(const Skeleton& target, const Clip& clip, int samples) {
    std::vector<Mat4> rest, g;
    computeGlobals(target, restPose(target), rest);
    std::vector<bool> moves(target.bones.size(), false);  // translated bones (hips, root) may move
    for (const auto& ch : clip.channels) {
        if (ch.path == Path::Translation && ch.bone >= 0 && static_cast<size_t>(ch.bone) < moves.size()) moves[static_cast<size_t>(ch.bone)] = true;
    }
    float worst = 0.f;
    samples = std::max(samples, 2);
    for (int i = 0; i < samples; ++i) {
        float t = clip.duration * static_cast<float>(i) / static_cast<float>(samples - 1);
        Pose p = restPose(target);
        sampleClip(clip, t, p);
        computeGlobals(target, p, g);
        for (size_t b = 0; b < target.bones.size(); ++b) {
            int par = target.bones[b].parent;
            if (par < 0 || moves[b]) continue;
            float r = distance(posOf(rest, static_cast<int>(b)), posOf(rest, par));
            if (r < 1e-4f) continue;
            float now = distance(posOf(g, static_cast<int>(b)), posOf(g, par));
            worst = std::max(worst, std::fabs(now - r) / r);
        }
    }
    return worst;
}

}  // namespace sky::anim
