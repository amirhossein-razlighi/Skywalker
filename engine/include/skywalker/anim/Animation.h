#pragma once
// Skeletal animation data: skeletons, clips, poses, sampling, skinning.
//
// An *animation library* (`*.anim` file, written when a rigged/animated glTF is imported)
// holds one model's skeleton (every node of the glTF scene, parents first) and its clips.
// Clips address bones by index; clips from another library are retargeted by bone name.
// Meshes carry their own skinning data (MeshData::skin) naming the bones they follow, so
// any skeleton with matching bone names can pose them.
//
// Everything here is plain CPU code, deterministic, and unit tested. The renderer only
// receives final joint matrices ("palettes").

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "skywalker/anim/AnimMath.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/render/MeshData.h"

namespace sky::anim {

struct Bone {
    std::string name;
    int parent = -1;
    Trs rest;  // local rest transform
};

struct Skeleton {
    std::vector<Bone> bones;  // parents always precede their children

    /// Exact name, then case-insensitive, then ignoring "prefix:" / "prefix|" namespaces
    /// ("mixamorig:Hips" matches "Hips"). -1 if none.
    int find(std::string_view name) const;
    std::vector<std::string> names() const;
    /// Is `bone` equal to or below `ancestor`?
    bool isDescendant(int bone, int ancestor) const;
};

enum class Interp : uint8_t { Linear, Step, Cubic };
enum class Path : uint8_t { Translation, Rotation, Scale };

struct Channel {
    int bone = -1;
    Path path = Path::Translation;
    Interp interp = Interp::Linear;
    std::vector<float> times;   // strictly increasing seconds
    std::vector<float> values;  // per key: components (Cubic: in-tangent, value, out-tangent)
    int components() const { return path == Path::Rotation ? 4 : 3; }
};

struct Clip {
    std::string name;
    float duration = 0.f;
    std::vector<Channel> channels;

    bool hasChannel(int bone, Path path) const;
};

struct Library {
    Skeleton skeleton;
    std::vector<Clip> clips;
    int rootBone = -1;   // the skeleton root of the first skin (root motion source), -1 if none
    std::string source;  // the file it was imported from (informational)

    int clipIndex(std::string_view name) const;  // exact, then case-insensitive
    const Clip* clip(std::string_view name) const;
    std::vector<std::string> clipNames() const;
};

using Pose = std::vector<Trs>;  // local transform per bone

Pose restPose(const Skeleton& skeleton);

/// Writes the clip's channels at `time` (seconds, clamped to the keys) into `pose`.
/// Bones without channels keep their current value.
void sampleClip(const Clip& clip, float time, Pose& pose);
/// One channel's value at `time` (translation/scale xyz in .t/.s, rotation in .r).
Trs sampleChannel(const Channel& channel, float time, const Trs& fallback);
/// The root bone's local translation in a clip at `time` (fallback when it is not animated).
Vec3 sampleTranslation(const Clip& clip, int bone, float time, Vec3 fallback);

/// out = a * (1 - w) + b * w per bone (out may alias a or b).
void blendPoses(const Pose& a, const Pose& b, float w, Pose& out);
/// Same, only for bones where mask[bone] is true.
void blendPosesMasked(const Pose& a, const Pose& b, float w, const std::vector<bool>& mask, Pose& out);

/// Model-space (glTF space) matrix of every bone.
void computeGlobals(const Skeleton& skeleton, const Pose& pose, std::vector<Mat4>& globals);

/// Average horizontal speed of the root bone over a clip (model units per second, `up` is
/// the model's up axis). Locomotion clips report how fast they move: blend thresholds.
float rootSpeed(const Library& library, const Clip& clip, Vec3 up = {0, 1, 0});

/// Re-addresses a clip from another skeleton onto `target` by bone name. Channels whose
/// bone has no match are dropped. Returns the number of channels kept.
size_t retarget(const Clip& clip, const Skeleton& source, const Skeleton& target, Clip& out);

// --- Skinning ---------------------------------------------------------------------------

/// Which skeleton bone drives each slot of a mesh's skin (-1 = keep the rest transform).
std::vector<int> mapSkin(const SkinStream& skin, const Skeleton& skeleton);
/// Joint matrices in mesh space: M_k = skin.transform * G[bone_k] * inverseBind_k.
void skinPalette(const SkinStream& skin, const std::vector<int>& slotToBone, const std::vector<Mat4>& globals,
                 std::vector<Mat4>& palette);
/// The rest-pose palette (no skeleton needed).
void restPalette(const SkinStream& skin, std::vector<Mat4>& palette);
/// CPU skinning: `out` becomes `mesh` with posed positions and normals.
void skinMesh(const MeshData& mesh, const std::vector<Mat4>& palette, MeshData& out);
/// Conservative mesh-space bounds of the posed mesh (per-slot bounds through the palette).
Aabb posedBounds(const SkinStream& skin, const std::vector<Mat4>& palette);

// --- Files (*.anim) ----------------------------------------------------------------------

/// Compact binary: "SKYANIM1", a JSON header (skeleton, clips, channel layout) and a
/// little-endian float blob with all key times and values.
std::vector<uint8_t> encodeLibrary(const Library& library);
Result<Library> decodeLibrary(const std::vector<uint8_t>& bytes);
Status saveLibrary(const std::string& absolutePath, const Library& library);
Result<Library> loadLibrary(const std::string& absolutePath);

/// Summary for tools: bones (name, parent) and clips (name, duration, channels).
Json libraryToJson(const Library& library, bool includeBones = true);

}  // namespace sky::anim
