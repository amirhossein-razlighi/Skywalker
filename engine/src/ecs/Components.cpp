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
                      "Primitive (cube, sphere, plane, cylinder, cone, quad, capsule, torus) or \"asset:<path.obj>\""),
            SKY_FIELD(MeshRenderer, color, Color, "Base color"),
            SKY_FIELD_RANGE(MeshRenderer, metallic, Float, "0 = dielectric, 1 = metal", 0.f, 1.f),
            SKY_FIELD_RANGE(MeshRenderer, roughness, Float, "0 = mirror, 1 = matte", 0.02f, 1.f),
            SKY_FIELD(MeshRenderer, emissive, Color, "Emitted light color; alpha is strength"),
            SKY_FIELD(MeshRenderer, texture, String, "Optional albedo texture path (png/jpg), relative to project"),
            SKY_FIELD(MeshRenderer, visible, Bool, "Whether the mesh is drawn"),
            SKY_FIELD(MeshRenderer, billboard, Bool, "Always face the camera (useful for 2D sprites)"),
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
        }};
    return info;
}

}  // namespace sky
