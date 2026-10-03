#include "skywalker/ecs/Components.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<Transform>);
static_assert(std::is_standard_layout_v<MeshRenderer>);
static_assert(std::is_standard_layout_v<Light>);
static_assert(std::is_standard_layout_v<Camera>);
static_assert(std::is_standard_layout_v<Environment>);

const TypeInfo& Transform::type() {
    static const TypeInfo info{
        "transform",
        "Position, rotation (Euler degrees) and scale relative to the parent entity.",
        {
            SKY_FIELD(Transform, position, Vec3, "Position in meters relative to parent; +Y is up"),
            SKY_FIELD(Transform, rotation, Vec3, "Euler angles in degrees [pitch(X), yaw(Y), roll(Z)]"),
            SKY_FIELD(Transform, scale, Vec3, "Scale factor per axis"),
        }};
    return info;
}

const std::vector<std::string>& MeshRenderer::primitives() {
    static const std::vector<std::string> names{"cube", "sphere", "plane", "cylinder", "cone", "quad", "capsule", "torus"};
    return names;
}

const TypeInfo& MeshRenderer::type() {
    static const TypeInfo info{
        "mesh",
        "Renders a mesh with a simple physically based material.",
        {
            SKY_FIELD(MeshRenderer, mesh, String,
                      "Primitive (cube, sphere, plane, cylinder, cone, quad, capsule, torus) or \"asset:<path>\" (.obj/.glb/.gltf)"),
            SKY_FIELD(MeshRenderer, color, Color, "Base color"),
            SKY_FIELD_RANGE(MeshRenderer, metallic, Float, "0 = dielectric, 1 = metal", 0.f, 1.f),
            SKY_FIELD_RANGE(MeshRenderer, roughness, Float, "0 = mirror, 1 = matte", 0.02f, 1.f),
            SKY_FIELD(MeshRenderer, emissive, Color, "Emitted light color; alpha is strength"),
            SKY_FIELD(MeshRenderer, texture, String, "Optional albedo texture path (png/jpg), relative to project"),
            SKY_FIELD(MeshRenderer, visible, Bool, "Whether the mesh is drawn"),
            SKY_FIELD(MeshRenderer, billboard, Bool, "Always face the camera (useful for 2D sprites)"),
            SKY_FIELD(MeshRenderer, material, String,
                      "Material asset path (*.mat.json); when set it overrides color/metallic/roughness/emissive/texture"),
            SKY_FIELD(MeshRenderer, unlit, Bool, "Flat color/texture without lighting (2D, UI, stylized)"),
            SKY_FIELD_ENUM(MeshRenderer, shading, "pbr = physically based, toon = cel bands + crisp highlights, unlit = flat",
                           "pbr", "toon", "unlit"),
            SKY_FIELD(MeshRenderer, normalMap, String, "Normal map (png), project-relative; adds surface detail"),
            SKY_FIELD(MeshRenderer, ormMap, String, "Occlusion/roughness/metallic map (R/G/B, glTF convention)"),
            SKY_FIELD(MeshRenderer, emissiveMap, String, "Emission mask/color map, multiplied with emissive"),
            SKY_FIELD_RANGE(MeshRenderer, normalStrength, Float, "Normal map strength", 0.f, 4.f),
            SKY_FIELD_RANGE(MeshRenderer, tiling, Float, "Texture repeats (per meter when triplanar)", 0.01f, 1000.f),
            SKY_FIELD(MeshRenderer, triplanar, Bool, "Project textures in world space: no stretching on scaled shapes"),
            SKY_FIELD_RANGE(MeshRenderer, clearcoat, Float, "Glossy lacquer layer on top (car paint, varnish)", 0.f, 1.f),
            SKY_FIELD_RANGE(MeshRenderer, subsurface, Float, "Light bleeding through (skin, leaves, wax, snow)", 0.f, 1.f),
            SKY_FIELD_RANGE(MeshRenderer, rim, Float, "Stylized rim light along silhouettes", 0.f, 4.f),
            SKY_FIELD_RANGE(MeshRenderer, outline, Float, "Cartoon outline width in pixels (0 = none)", 0.f, 12.f),
            SKY_FIELD(MeshRenderer, outlineColor, Color, "Outline color"),
            SKY_FIELD(MeshRenderer, doubleSided, Bool, "Render both faces (leaves, cloth, cards)"),
            SKY_FIELD(MeshRenderer, castShadows, Bool, "Cast sun shadows"),
        }};
    return info;
}

const TypeInfo& Light::type() {
    static const TypeInfo info{
        "light",
        "A light source. Directional lights use the entity rotation; point/spot use position.",
        {
            SKY_FIELD_ENUM(Light, kind, "Light type", "directional", "point", "spot"),
            SKY_FIELD(Light, color, Color, "Light color"),
            SKY_FIELD_RANGE(Light, intensity, Float, "Brightness multiplier", 0.f, 1000.f),
            SKY_FIELD_RANGE(Light, range, Float, "Falloff distance in meters (point/spot)", 0.01f, 10000.f),
            SKY_FIELD_RANGE(Light, spotAngle, Float, "Cone half-angle in degrees (spot)", 1.f, 89.f),
        }};
    return info;
}

const TypeInfo& Camera::type() {
    static const TypeInfo info{
        "camera",
        "A camera. The first primary camera is used when the game runs.",
        {
            SKY_FIELD_RANGE(Camera, fov, Float, "Vertical field of view in degrees", 5.f, 170.f),
            SKY_FIELD_RANGE(Camera, nearPlane, Float, "Near clip distance", 0.001f, 100.f),
            SKY_FIELD_RANGE(Camera, farPlane, Float, "Far clip distance", 1.f, 100000.f),
            SKY_FIELD(Camera, orthographic, Bool, "Orthographic projection (2D games)"),
            SKY_FIELD_RANGE(Camera, orthoSize, Float, "Half the visible height in orthographic mode", 0.01f, 10000.f),
            SKY_FIELD(Camera, primary, Bool, "Use this camera for gameplay"),
        }};
    return info;
}

Vec3 Environment::sunDirection() const {
    float az = radians(sunAzimuth);
    float el = radians(sunElevation);
    Vec3 toSun{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    return -normalize(toSun);
}

const TypeInfo& Environment::type() {
    static const TypeInfo info{
        "environment",
        "Scene-wide sky, sun, ambient light, fog and exposure.",
        {
            SKY_FIELD(Environment, skyTop, Color, "Sky color at the zenith"),
            SKY_FIELD(Environment, skyHorizon, Color, "Sky color at the horizon"),
            SKY_FIELD(Environment, ground, Color, "Ambient bounce color from below"),
            SKY_FIELD_RANGE(Environment, ambient, Float, "Ambient light strength", 0.f, 10.f),
            SKY_FIELD(Environment, sunAzimuth, Float, "Sun compass direction in degrees (0 = +Z)"),
            SKY_FIELD_RANGE(Environment, sunElevation, Float, "Sun height above horizon in degrees", -90.f, 90.f),
            SKY_FIELD(Environment, sunColor, Color, "Sun light color"),
            SKY_FIELD_RANGE(Environment, sunIntensity, Float, "Sun brightness", 0.f, 100.f),
            SKY_FIELD(Environment, fogColor, Color, "Distance fog color"),
            SKY_FIELD_RANGE(Environment, fogDensity, Float, "Exponential fog density (0 = off)", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, exposure, Float, "Camera exposure multiplier", 0.01f, 20.f),
            SKY_FIELD(Environment, showGrid, Bool, "Draw the editor ground grid"),
            SKY_FIELD_RANGE(Environment, bloomIntensity, Float, "Glow around bright/emissive things (0 = off)", 0.f, 5.f),
            SKY_FIELD_RANGE(Environment, bloomThreshold, Float, "Brightness where glow starts (lower = more glow)", 0.f, 10.f),
            SKY_FIELD_RANGE(Environment, saturation, Float, "Color saturation (1 = neutral)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, contrast, Float, "Contrast (1 = neutral)", 0.5f, 2.f),
            SKY_FIELD_RANGE(Environment, vignette, Float, "Darken the image corners", 0.f, 1.f),
            SKY_FIELD_ENUM(Environment, skyMode, "gradient = two artist colors; atmosphere = physically inspired sky from the sun",
                           "gradient", "atmosphere"),
            SKY_FIELD_RANGE(Environment, clouds, Float, "Procedural cloud cover", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, stars, Float, "Stars in the night sky", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, sunSize, Float, "Sun / moon disc size", 0.1f, 8.f),
            SKY_FIELD_RANGE(Environment, fogHeight, Float, "Fog pools near the ground as this grows (0 = uniform)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, reflections, Float, "Sky reflections / image-based lighting strength", 0.f, 3.f),
            SKY_FIELD_RANGE(Environment, ao, Float, "Ambient occlusion (contact shadows in corners and creases)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, aoRadius, Float, "Ambient occlusion radius in meters", 0.05f, 5.f),
            SKY_FIELD_RANGE(Environment, shadowSoftness, Float, "Sun shadow penumbra size", 0.f, 6.f),
            SKY_FIELD_ENUM(Environment, tonemap, "HDR to display curve", "aces", "agx", "neutral", "filmic", "none"),
            SKY_FIELD_RANGE(Environment, temperature, Float, "White balance: -1 cool .. +1 warm", -1.f, 1.f),
            SKY_FIELD_RANGE(Environment, tint, Float, "White balance: -1 green .. +1 magenta", -1.f, 1.f),
        }};
    return info;
}

}  // namespace sky
