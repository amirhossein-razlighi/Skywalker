#include "skywalker/ecs/Components.h"

#include <type_traits>

namespace sky {

static_assert(std::is_standard_layout_v<Transform>);
static_assert(std::is_standard_layout_v<MeshRenderer>);
static_assert(std::is_standard_layout_v<Light>);
static_assert(std::is_standard_layout_v<Camera>);
static_assert(std::is_standard_layout_v<Environment>);
static_assert(std::is_standard_layout_v<ParticleEmitter>);
static_assert(std::is_standard_layout_v<Water>);
static_assert(std::is_standard_layout_v<FluidVolume>);
static_assert(std::is_standard_layout_v<Terrain>);
static_assert(std::is_standard_layout_v<Foliage>);

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
    static const std::vector<std::string> names{"cube",  "sphere", "plane",  "cylinder", "cone",   "quad",   "capsule", "torus",
                                                "grass", "grass_tall", "fern", "flowers", "pebbles", "shell", "rock"};
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
            SKY_FIELD_ENUM(MeshRenderer, shading,
                           "pbr = physically based, toon = cel bands + crisp highlights, unlit = flat, water = animated waves "
                           "(use on a large plane; color = deep water tint)",
                           "pbr", "toon", "unlit", "water"),
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
            SKY_FIELD_RANGE(MeshRenderer, alphaCutoff, Float,
                            "Alpha-tested cutout: texture alpha below this is cut (foliage, fences, sails); 0 = off", 0.f, 1.f),
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

const TypeInfo& ParticleEmitter::type() {
    static const TypeInfo info{
        "particles",
        "Particle emitter: fire, smoke, embers, sparks, rain, snow, mist, magic. Simulated by the engine and drawn as "
        "soft HDR particles. Easiest start: fx_create with a preset, then tweak fields.",
        {
            SKY_FIELD(ParticleEmitter, preset, String, "Preset it was made from (informational)"),
            SKY_FIELD(ParticleEmitter, emitting, Bool, "Emit new particles (existing ones finish their life)"),
            SKY_FIELD_ENUM(ParticleEmitter, look,
                           "glow = soft light points; flame = animated fire tongues; smoke = lit billowing volume; "
                           "spark = glowing streaks; rain = lit falling streaks; snow = soft flakes; mist = drifting fog",
                           "glow", "flame", "smoke", "spark", "rain", "snow", "mist"),
            SKY_FIELD_RANGE(ParticleEmitter, rate, Float, "Particles per second", 0.f, 20000.f),
            SKY_FIELD_RANGE(ParticleEmitter, burst, Int, "Particles emitted at once when it starts (explosions); Wander burst(n) "
                            "emits more", 0.f, 20000.f),
            SKY_FIELD_RANGE(ParticleEmitter, maxParticles, Int, "Upper bound of live particles", 1.f, 50000.f),
            SKY_FIELD_RANGE(ParticleEmitter, lifetime, Float, "Seconds each particle lives", 0.01f, 120.f),
            SKY_FIELD_RANGE(ParticleEmitter, lifetimeJitter, Float, "Random lifetime variation (fraction)", 0.f, 1.f),
            SKY_FIELD_ENUM(ParticleEmitter, shape, "Where particles are born (scaled by shapeSize)", "point", "sphere",
                           "box", "disc", "cone"),
            SKY_FIELD(ParticleEmitter, shapeSize, Vec3, "Size of the emission shape (m)"),
            SKY_FIELD(ParticleEmitter, direction, Vec3, "Emission direction in the entity's space"),
            SKY_FIELD_RANGE(ParticleEmitter, speed, Float, "Initial speed (m/s)", 0.f, 500.f),
            SKY_FIELD_RANGE(ParticleEmitter, speedJitter, Float, "Random speed variation (fraction)", 0.f, 1.f),
            SKY_FIELD_RANGE(ParticleEmitter, spread, Float, "Cone half-angle around direction (degrees)", 0.f, 180.f),
            SKY_FIELD_RANGE(ParticleEmitter, gravity, Float, "Downward acceleration (m/s^2); negative = rises like hot gas",
                            -50.f, 50.f),
            SKY_FIELD_RANGE(ParticleEmitter, drag, Float, "Air resistance (per second)", 0.f, 20.f),
            SKY_FIELD_RANGE(ParticleEmitter, turbulence, Float, "Curl-noise swirl strength (m/s)", 0.f, 50.f),
            SKY_FIELD_RANGE(ParticleEmitter, turbulenceScale, Float, "Swirl size (m)", 0.01f, 100.f),
            SKY_FIELD_RANGE(ParticleEmitter, wind, Float, "How strongly the environment wind carries particles", 0.f, 5.f),
            SKY_FIELD_RANGE(ParticleEmitter, sizeStart, Float, "Size at birth (m)", 0.f, 100.f),
            SKY_FIELD_RANGE(ParticleEmitter, sizeEnd, Float, "Size at death (m)", 0.f, 100.f),
            SKY_FIELD_RANGE(ParticleEmitter, sizeJitter, Float, "Random size variation (fraction)", 0.f, 1.f),
            SKY_FIELD(ParticleEmitter, colorStart, Color, "Color at birth (alpha = opacity)"),
            SKY_FIELD(ParticleEmitter, colorEnd, Color, "Color at death (alpha = opacity)"),
            SKY_FIELD_RANGE(ParticleEmitter, intensity, Float, "HDR brightness (glows with bloom above ~1)", 0.f, 200.f),
            SKY_FIELD_RANGE(ParticleEmitter, stretch, Float, "Streak length along velocity (seconds of motion)", 0.f, 1.f),
            SKY_FIELD_RANGE(ParticleEmitter, spin, Float, "Random rotation speed (degrees/s)", 0.f, 1440.f),
            SKY_FIELD_RANGE(ParticleEmitter, softness, Float, "Fade where particles meet geometry (m)", 0.f, 10.f),
            SKY_FIELD(ParticleEmitter, worldSpace, Bool, "Particles stay behind when the emitter moves (trails)"),
            SKY_FIELD(ParticleEmitter, collide, Bool, "Collide with the floor plane at floorHeight"),
            SKY_FIELD(ParticleEmitter, floorHeight, Float, "World height of the collision floor"),
            SKY_FIELD_RANGE(ParticleEmitter, bounce, Float, "Bounciness on impact (0 = die)", 0.f, 1.f),
            SKY_FIELD_RANGE(ParticleEmitter, splash, Int, "Droplets spawned per impact (rain splashes)", 0.f, 16.f),
            SKY_FIELD_RANGE(ParticleEmitter, light, Float, "Light cast on the scene (flickers with the particles)", 0.f, 100.f),
            SKY_FIELD(ParticleEmitter, lightColor, Color, "Color of the cast light"),
            SKY_FIELD_RANGE(ParticleEmitter, lightRange, Float, "Reach of the cast light (m)", 0.1f, 200.f),
            SKY_FIELD(ParticleEmitter, prewarm, Bool, "Start fully developed instead of from nothing"),
            SKY_FIELD(ParticleEmitter, seed, Int, "Random seed (variations of the same effect)"),
        }};
    return info;
}

const TypeInfo& FluidVolume::type() {
    static const TypeInfo info{
        "fluid",
        "Volumetric fluid simulation for fire, smoke, steam and explosions (GPU 3D solver, ray-marched): fuel burns "
        "into heat and soot, hot gas rises and swirls. The box sits on the entity (bottom center). Start with fx_create "
        "(volume_fire, volume_smoke, steam_vent, explosion_volume...).",
        {
            SKY_FIELD(FluidVolume, preset, String, "Preset it was made from (informational)"),
            SKY_FIELD(FluidVolume, emitting, Bool, "Feed the source (off: the fire dies down)"),
            SKY_FIELD(FluidVolume, size, Vec3, "Simulation box in meters (fire lives inside it)"),
            SKY_FIELD_RANGE(FluidVolume, resolution, Int, "Cells along the longest side (detail vs. cost)", 16.f, 192.f),
            SKY_FIELD(FluidVolume, sourceOffset, Vec3, "Source position from the box's bottom center (m)"),
            SKY_FIELD_RANGE(FluidVolume, sourceRadius, Float, "Source radius (m)", 0.01f, 50.f),
            SKY_FIELD_RANGE(FluidVolume, fuel, Float, "Fuel fed per second (0 = smoke/steam only)", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, heat, Float, "Heat released (drives flames and lift)", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, smoke, Float, "Soot / vapour produced", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, buoyancy, Float, "How fast hot gas rises", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, vorticity, Float, "Swirl: licking flames, curling smoke", 0.f, 5.f),
            SKY_FIELD_RANGE(FluidVolume, turbulence, Float, "Noise injected at the source", 0.f, 5.f),
            SKY_FIELD_RANGE(FluidVolume, burnRate, Float, "How fast fuel burns (lower = taller flames)", 0.05f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, cooling, Float, "How fast heat fades", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, smokeFade, Float, "How fast smoke thins out", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, speed, Float, "Upward speed at the source (m/s)", 0.f, 50.f),
            SKY_FIELD_RANGE(FluidVolume, wind, Float, "Environment wind influence", 0.f, 5.f),
            SKY_FIELD_RANGE(FluidVolume, flameIntensity, Float, "Fire brightness", 0.f, 50.f),
            SKY_FIELD_RANGE(FluidVolume, flameTemperature, Float, "Flame color temperature in Kelvin (blackbody)", 800.f, 6000.f),
            SKY_FIELD(FluidVolume, smokeColor, Color, "Smoke albedo (dark soot .. white steam)"),
            SKY_FIELD_RANGE(FluidVolume, smokeDensity, Float, "Smoke opacity", 0.f, 20.f),
            SKY_FIELD_RANGE(FluidVolume, light, Float, "Light the fire casts on the scene", 0.f, 200.f),
            SKY_FIELD(FluidVolume, lightColor, Color, "Color of the cast light"),
            SKY_FIELD_RANGE(FluidVolume, lightRange, Float, "Reach of the cast light (m)", 0.1f, 300.f),
            SKY_FIELD_RANGE(FluidVolume, burst, Float, "Seconds of heavy fuel at start (explosions, fireballs)", 0.f, 10.f),
            SKY_FIELD(FluidVolume, seed, Int, "Random seed (variation)"),
        }};
    return info;
}

const TypeInfo& Water::type() {
    static const TypeInfo info{
        "water",
        "Water surface with a spectral (FFT) ocean simulation: wind waves, choppy crests, foam, refraction, depth "
        "color and reflections. Entity y = water level. Query wave heights with water_query / Wander water_height().",
        {
            SKY_FIELD_RANGE(Water, windSpeed, Float, "Wind speed (m/s): 2 calm lake, 8 breezy sea, 18 storm", 0.f, 40.f),
            SKY_FIELD(Water, windDirection, Float, "Direction the waves travel (degrees, 0 = +Z)"),
            SKY_FIELD_RANGE(Water, choppiness, Float, "Sharpness of crests (0 = rolling swell)", 0.f, 3.f),
            SKY_FIELD_RANGE(Water, waveScale, Float, "Wave amplitude multiplier", 0.f, 5.f),
            SKY_FIELD_RANGE(Water, patchSize, Float, "Longest simulated wavelength (m)", 10.f, 2000.f),
            SKY_FIELD_RANGE(Water, size, Float, "Square extent in meters (0 = endless ocean)", 0.f, 100000.f),
            SKY_FIELD_RANGE(Water, depth, Float, "Water depth for wave physics (shallow water slows waves)", 0.5f, 5000.f),
            SKY_FIELD(Water, deepColor, Color, "Light scattered inside deep water"),
            SKY_FIELD(Water, shallowColor, Color, "Tint of what you see through shallow water"),
            SKY_FIELD_RANGE(Water, clarity, Float, "How far you can see into the water (m)", 0.1f, 100.f),
            SKY_FIELD_RANGE(Water, foam, Float, "Whitecaps and shoreline foam", 0.f, 4.f),
            SKY_FIELD_RANGE(Water, reflections, Float, "Reflection strength", 0.f, 2.f),
            SKY_FIELD_RANGE(Water, refraction, Float, "Refraction distortion", 0.f, 4.f),
            SKY_FIELD_RANGE(Water, roughness, Float, "Micro roughness (sun glint size)", 0.005f, 0.5f),
            SKY_FIELD(Water, seed, Int, "Random seed of the wave field"),
        }};
    return info;
}

const TypeInfo& Terrain::type() {
    static const TypeInfo info{
        "terrain",
        "Large heightfield terrain (islands, beaches, mountains, canyons, dunes) with erosion, up to 8 blended "
        "material layers and wet shorelines. Rendered with continuous LOD. Use terrain_create, terrain_sculpt, "
        "terrain_paint and terrain_layers; heights are relative to the entity.",
        {
            SKY_FIELD(Terrain, data, String, "Project-relative .terrain file with heights and layer weights"),
            SKY_FIELD_RANGE(Terrain, size, Float, "Square extent in meters", 8.f, 32768.f),
            SKY_FIELD_RANGE(Terrain, resolution, Int, "Height samples per side (2^n+1: 257, 513, 1025, 2049)", 17, 4097),
            SKY_FIELD_JSON(Terrain, generator, "Generation parameters (shape, seed, minHeight, maxHeight, featureSize, ridges, "
                                               "warp, erosion, thermal, terraces, beachWidth, seaLevel)",
                           R"({"type":"object"})"),
            SKY_FIELD_JSON(Terrain, layers,
                           "Material layers, base first. Each: {name, texture, normalMap, ormMap, color, roughness, tiling (m), "
                           "heightMin, heightMax, slopeMin, slopeMax, noise, sharpness}. Rules auto-paint the weights.",
                           R"({"type":"array","items":{"type":"object"}})"),
            SKY_FIELD(Terrain, waterLevel, Float, "World height of the water line (wet sand/soil just above it)"),
            SKY_FIELD_RANGE(Terrain, wetBand, Float, "Meters above the water line that stay damp", 0.f, 20.f),
            SKY_FIELD_RANGE(Terrain, detail, Float, "Level-of-detail quality multiplier", 0.25f, 4.f),
            SKY_FIELD(Terrain, castShadows, Bool, "Cast sun shadows"),
        }};
    return info;
}

const TypeInfo& Foliage::type() {
    static const TypeInfo info{
        "foliage",
        "GPU-instanced scattering of grass, flowers, ferns, pebbles, shells, rocks and trees over a terrain (this entity or "
        "its parent) or over scene meshes inside `area`. Wind-animated, deterministic. Use foliage_add with presets.",
        {
            SKY_FIELD_JSON(Foliage, layers,
                           "Layers: [{preset, mesh, color, density (/m²), scaleMin, scaleMax, slopeMin, slopeMax, heightMin, "
                           "heightMax, terrainLayer, wind, cullDistance, castShadows, clumping, alignToNormal}]",
                           R"({"type":"array","items":{"type":"object"}})"),
            SKY_FIELD(Foliage, seed, Int, "Random seed of the placement"),
            SKY_FIELD_RANGE(Foliage, density, Float, "Density multiplier for every layer", 0.f, 4.f),
            SKY_FIELD_ENUM(Foliage, surface, "What to grow on", "terrain", "scene"),
            SKY_FIELD(Foliage, area, Vec3, "Scene mode: extent in meters around the entity"),
            SKY_FIELD(Foliage, visible, Bool, "Draw the foliage"),
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
            SKY_FIELD_ENUM(Environment, skyMode,
                           "gradient = two artist colors; atmosphere = physically inspired sky from the sun; hdri = a "
                           "photographed .hdr panorama lights and backs the scene (set `hdri`)",
                           "gradient", "atmosphere", "hdri"),
            SKY_FIELD(Environment, hdri, String, "Equirectangular .hdr panorama, project-relative (skyMode hdri)"),
            SKY_FIELD(Environment, hdriRotation, Float, "Panorama rotation in degrees (line its sun up with sunAzimuth)"),
            SKY_FIELD_RANGE(Environment, hdriIntensity, Float, "Panorama brightness", 0.f, 20.f),
            SKY_FIELD_RANGE(Environment, clouds, Float, "Procedural cloud cover", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, stars, Float, "Stars in the night sky", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, sunSize, Float, "Sun / moon disc size", 0.1f, 8.f),
            SKY_FIELD_RANGE(Environment, fogHeight, Float, "Fog pools near the ground as this grows (0 = uniform)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, reflections, Float, "Sky reflections / image-based lighting strength", 0.f, 3.f),
            SKY_FIELD_RANGE(Environment, ao, Float, "Ambient occlusion (contact shadows in corners and creases)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, aoRadius, Float, "Ambient occlusion radius in meters", 0.05f, 5.f),
            SKY_FIELD_RANGE(Environment, gi, Float, "Screen-space global illumination: bounce and emissive light (0 = sky light only)", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, giDistance, Float, "Global illumination ray length in meters", 0.5f, 50.f),
            SKY_FIELD_RANGE(Environment, ssr, Float, "Screen-space reflections on glossy surfaces (wet streets, floors, metal)", 0.f, 1.f),
            SKY_FIELD(Environment, taa, Bool, "Temporal anti-aliasing (smooth edges, stable shimmer-free detail)"),
            SKY_FIELD_RANGE(Environment, sharpen, Float, "Sharpening after temporal anti-aliasing", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, shadowSoftness, Float, "Sun shadow penumbra size", 0.f, 6.f),
            SKY_FIELD_ENUM(Environment, tonemap, "HDR to display curve", "aces", "agx", "neutral", "filmic", "none"),
            SKY_FIELD_RANGE(Environment, temperature, Float, "White balance: -1 cool .. +1 warm", -1.f, 1.f),
            SKY_FIELD_RANGE(Environment, tint, Float, "White balance: -1 green .. +1 magenta", -1.f, 1.f),
            SKY_FIELD_RANGE(Environment, shadowDistance, Float, "Sun shadow range in meters (0 = automatic)", 0.f, 5000.f),
            SKY_FIELD_RANGE(Environment, godRays, Float,
                            "Volumetric light: sun shafts through trees and windows, cones under lamps (0 = off, 1 = natural)",
                            0.f, 8.f),
            SKY_FIELD_RANGE(Environment, haze, Float, "Air density for volumetric light (0.005 clear .. 0.1 misty)", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, windSpeed, Float, "Wind (m/s) that carries smoke, rain, snow and particles", 0.f, 60.f),
            SKY_FIELD(Environment, windDirection, Float, "Direction the wind blows toward (degrees, 0 = +Z)"),
        }};
    return info;
}

}  // namespace sky
