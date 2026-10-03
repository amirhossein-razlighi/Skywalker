#pragma once
// `groom`: strand-based hair and fur (see docs/HAIR_AND_VFX.md).
//
// Included at the end of Components.h. A groom grows strands on the entity's mesh (or a
// named target entity's mesh), or loads them from a file (.hair, .groom.json, .skygroom).
// Generation runs on the CPU and is cached per parameter hash; shading fields (color,
// roughness...) and motion fields take effect immediately without regenerating.

#include <string>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

struct Groom {
    std::string preset;              // the preset it was made from (informational)
    bool visible = true;
    // --- Source -------------------------------------------------------------------------
    std::string source;              // "" = grow on the mesh; or a .hair / .groom.json / .skygroom file
    std::string target;              // entity whose mesh grows the hair ("" = this entity)
    float importScale = 1.f;         // imported files: unit scale (0.01 for centimeters)
    bool importZUp = false;          // imported files: Z-up source
    // --- Growth -------------------------------------------------------------------------
    int strands = 20000;             // rendered strands
    int guides = 0;                  // simulated guide strands (0 = automatic)
    int segments = 14;               // segments per strand (points - 1)
    float length = 0.25f;            // meters
    float lengthVariation = 0.2f;    // fraction
    float widthRoot = 0.08f;         // millimeters
    float widthTip = 0.03f;          // millimeters
    Vec3 direction{0.f, -1.f, 0.f};  // comb direction in the mesh's space
    float directionBlend = 0.5f;     // 0 = grow along the surface normal .. 1 = along `direction`
    float gravity = 0.5f;            // droop: 0 = stiff (fur, crew cut) .. 1 = hangs down
    float curlRadius = 0.f;          // meters (helix radius)
    float curlFrequency = 0.f;       // turns per meter of strand
    float wave = 0.f;                // meters of wave amplitude
    float waveFrequency = 8.f;       // waves per meter of strand
    int clumps = 0;                  // clump count (0 = no clumping)
    float clumpStrength = 0.5f;      // 0..1: how far strands pull into their clump
    float clumpShape = 1.f;          // > 1: tips clump more than roots
    float frizz = 0.f;               // meters of noise (flyaways)
    float frizzScale = 30.f;         // noise frequency (per meter)
    // --- Placement ----------------------------------------------------------------------
    std::string maskChannel = "none";  // none | r | g | b | a: vertex-color density mask
    Vec3 maskDirection{0.f, 1.f, 0.f};
    float maskAngle = 180.f;         // degrees: grow where the normal is within this angle of maskDirection
    float maskSoftness = 12.f;       // degrees of soft hairline
    // --- Color & shading ----------------------------------------------------------------
    float melanin = 0.4f;            // eumelanin+pheomelanin amount: 0 white .. 0.2 blond .. 0.5 brown .. 1 black
    float redness = 0.15f;           // pheomelanin fraction: auburn, ginger
    Vec4 dye{1.f, 1.f, 1.f, 1.f};    // dye tint (white = natural)
    Vec4 rootColor{1.f, 1.f, 1.f, 1.f};  // multiplier at the root (root-to-tip gradient)
    Vec4 tipColor{1.f, 1.f, 1.f, 1.f};   // multiplier at the tip
    float colorVariation = 0.15f;    // per-strand melanin variation
    float roughness = 0.35f;         // longitudinal roughness (highlight width)
    float radialRoughness = 0.7f;    // azimuthal roughness (softness of the glow around the strand)
    float specular = 1.f;            // primary highlight strength
    float scatter = 1.f;             // multiple scattering (light hair glows through)
    float cuticleTilt = 3.f;         // degrees: separates the white and colored highlights
    float density = 1.f;             // opacity per strand (shadows and coverage)
    // --- Motion -------------------------------------------------------------------------
    bool simulate = true;
    float stiffness = 0.45f;         // how strongly strands keep their groomed shape
    float rootStiffness = 0.9f;      // stiffness near the roots
    float damping = 0.2f;
    float wind = 1.f;                // environment wind influence
    bool collide = true;             // collide with the mesh (sphere/capsule proxy) and `colliders`
    std::string colliders;           // comma-separated entity names (proxies from their bounds)
    // --- Rendering ----------------------------------------------------------------------
    std::string lod = "auto";        // auto | strands | cards
    float cardsBelow = 90.f;         // auto: cards when the groom is smaller than this (pixels)
    bool castShadows = true;
    int seed = 0;

    static const TypeInfo& type();
    /// Named starting points (hair_straight, hair_wavy, hair_curly, hair_ponytail, fur_short, fur_long...).
    static const std::vector<std::string>& presets();
};

}  // namespace sky
