#pragma once
// Built-in components. Each one is plain data plus a reflection table (see
// Components.cpp). Components must stay standard-layout so offsetof is valid.

#include <memory>
#include <string>
#include <vector>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

namespace wander {
struct Program;
}

struct Transform {
    Vec3 position{0.f};
    Vec3 rotation{0.f};  // Euler degrees (yaw=Y, pitch=X, roll=Z)
    Vec3 scale{1.f};

    Mat4 local() const { return Mat4::trs(position, rotation, scale); }
    static const TypeInfo& type();
};

struct MeshRenderer {
    std::string mesh = "cube";  // primitive name or "asset:<path>"
    Vec4 color{0.8f, 0.8f, 0.82f, 1.f};
    float metallic = 0.f;
    float roughness = 0.55f;
    Vec4 emissive{0.f, 0.f, 0.f, 1.f};  // rgb * a(strength)
    std::string texture;               // optional albedo texture path
    bool visible = true;
    bool billboard = false;  // always faces the camera (handy for 2D sprites)
    std::string material;    // optional material asset (*.mat.json); overrides the inline values
    bool unlit = false;      // flat shading without lighting (2D, UI, stylized)
    // Advanced surface (PBR maps, stylization). The same fields exist on material assets.
    std::string shading = "pbr";  // pbr | toon | unlit
    std::string normalMap;        // tangent-space normal map (linear), project-relative
    std::string ormMap;           // R = occlusion, G = roughness, B = metallic (glTF convention)
    std::string emissiveMap;      // multiplied with `emissive`
    float normalStrength = 1.f;
    float tiling = 1.f;           // texture repeats (uniform)
    bool triplanar = false;       // project textures in world space (no stretching on scaled shapes)
    float clearcoat = 0.f;        // glossy lacquer layer (car paint, varnish)
    float subsurface = 0.f;       // light bleeding through (skin, leaves, wax, snow)
    float rim = 0.f;              // stylized rim light
    float outline = 0.f;          // toon outline width in pixels
    Vec4 outlineColor{0.04f, 0.04f, 0.06f, 1.f};
    bool doubleSided = false;
    bool castShadows = true;

    static const TypeInfo& type();
    static const std::vector<std::string>& primitives();
};

struct Light {
    std::string kind = "point";  // directional | point | spot
    Vec4 color{1.f, 0.96f, 0.9f, 1.f};
    float intensity = 1.f;
    float range = 10.f;
    float spotAngle = 35.f;

    static const TypeInfo& type();
};

struct Camera {
    float fov = 60.f;
    float nearPlane = 0.1f;
    float farPlane = 500.f;
    bool orthographic = false;
    float orthoSize = 5.f;
    bool primary = true;

    static const TypeInfo& type();
};

/// One Wander behaviour attached to an entity: the human/agent intent (natural
/// language) and its compiled, deterministic Wander source.
struct Script {
    std::string name;
    std::string intent;  // natural-language description (source of truth for humans)
    std::string source;  // Wander code (source of truth for the runtime)
    bool enabled = true;

    // Runtime cache (not serialized).
    std::shared_ptr<const wander::Program> program;
    std::string compiledSource;
    bool hasErrors = false;
    int runtimeErrors = 0;
};

struct Behavior {
    std::vector<Script> scripts;
};

/// Scene-wide look & feel. Reflected like a component so agents can tweak lighting
/// with the same generic machinery ("environment": {"sunElevation": 20}).
struct Environment {
    Vec4 skyTop{0.30f, 0.48f, 0.78f, 1.f};
    Vec4 skyHorizon{0.74f, 0.80f, 0.88f, 1.f};
    Vec4 ground{0.30f, 0.31f, 0.33f, 1.f};
    float ambient = 0.3f;
    float sunAzimuth = 35.f;    // degrees, 0 = +Z, clockwise when seen from above
    float sunElevation = 50.f;  // degrees above horizon
    Vec4 sunColor{1.f, 0.95f, 0.86f, 1.f};
    float sunIntensity = 1.9f;
    Vec4 fogColor{0.74f, 0.80f, 0.88f, 1.f};
    float fogDensity = 0.004f;
    float exposure = 1.f;
    bool showGrid = true;
    // Post-processing (HDR pipeline)
    float bloomIntensity = 0.55f;
    float bloomThreshold = 1.0f;
    float saturation = 1.05f;
    float contrast = 1.05f;
    float vignette = 0.22f;
    // Sky, atmosphere, lighting quality
    std::string skyMode = "gradient";  // gradient | atmosphere
    float clouds = 0.f;                // procedural cloud cover 0..1
    float stars = 0.f;                 // night-sky stars 0..1
    float sunSize = 1.f;               // sun/moon disc size multiplier
    float fogHeight = 0.f;             // height falloff: > 0 makes fog pool near the ground
    float reflections = 1.f;           // image-based (sky) reflections strength
    float ao = 0.8f;                   // screen-space ambient occlusion strength
    float aoRadius = 0.6f;             // meters
    float shadowSoftness = 1.f;        // penumbra size multiplier
    std::string tonemap = "aces";      // aces | agx | neutral | filmic | none
    float temperature = 0.f;           // white balance: -1 cool .. +1 warm
    float tint = 0.f;                  // -1 green .. +1 magenta

    Vec3 sunDirection() const;  // direction light travels (from sun towards ground)
    static const TypeInfo& type();
};

}  // namespace sky
