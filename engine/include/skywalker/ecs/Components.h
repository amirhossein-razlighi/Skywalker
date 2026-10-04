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
    std::string shading = "pbr";  // pbr | toon | unlit | water
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
    float alphaCutoff = 0.f;      // > 0: alpha-tested cutout (foliage, fences, sails); pixels below are cut
    // Render layers (docs/RENDERING.md "Render layers"): bit i = layer i+1 (20 layers).
    int layers = 1;               // which layers this mesh is on; cameras and lights pick layers with cullMask

    static const TypeInfo& type();
    static const std::vector<std::string>& primitives();
};

struct Light {
    std::string kind = "point";  // directional | point | spot
    Vec4 color{1.f, 0.96f, 0.9f, 1.f};
    float intensity = 1.f;
    float range = 10.f;
    float spotAngle = 35.f;
    // Light v2 (docs/RENDERING.md "Lights"): masks, per-term strengths, color temperature, falloff.
    int cullMask = 0xFFFFF;       // render layers this light illuminates (bit i = layer i+1; all 20 by default)
    float specular = 1.f;         // highlight strength (0 = diffuse only, no glints)
    float indirect = 1.f;         // contribution to baked / world-space GI (reserved: used when GI probes exist)
    float volumetric = 1.f;       // strength in volumetric fog / god rays
    float temperature = 0.f;      // Kelvin (1000..40000) tints `color`; 0 = use color as is
    float innerAngle = 0.f;       // spot: full-intensity inner cone half-angle in degrees (0 = automatic soft edge)
    bool negative = false;        // subtracts light (darkens; stylized shadows, fake occlusion)
    bool distanceFade = false;    // fade out with camera distance (many small lights in big levels)
    float fadeBegin = 40.f;       // meters from the camera where fading starts
    float fadeLength = 10.f;      // meters over which it fades to nothing
    std::string attenuation = "smooth";  // smooth (soft, artist-friendly) | inverse_square (physical, uses size)
    float size = 0.1f;            // emitter radius in meters (inverse_square: caps the peak near the source)

    static const TypeInfo& type();
};

struct Camera {
    float fov = 60.f;
    float nearPlane = 0.1f;
    float farPlane = 500.f;
    bool orthographic = false;
    float orthoSize = 5.f;
    bool primary = true;
    // Cinematic lens (used by the post stack)
    float aperture = 0.f;       // f-stop for depth of field (1.4 shallow .. 16 deep), 0 = everything sharp
    float focusDistance = 0.f;  // meters, 0 = autofocus on the center of the frame
    float motionBlur = 0.f;     // shutter fraction 0..1 (0.5 = 180-degree shutter), 0 = off
    float tiltShift = 0.f;      // miniature look: 0..1 blur above and below a sharp horizontal band (tilt-shift lens)
    int cullMask = 0xFFFFF;     // render layers this camera sees (bit i = layer i+1; all 20 by default)

    static const TypeInfo& type();
};

/// A particle emitter (fire, smoke, embers, sparks, rain, snow, mist, magic...). Simulated
/// by the engine (deterministic in play mode, live preview while editing) and rendered as
/// soft, sorted, HDR particles. Start from a preset with the fx_create tool.
struct ParticleEmitter {
    std::string preset;            // the preset it was made from (informational)
    bool emitting = true;
    std::string look = "glow";     // glow | flame | smoke | spark | rain | snow | mist | sprite (gpu)
    float rate = 20.f;             // particles per second
    int burst = 0;                 // particles emitted at once when the emitter starts (explosions)
    int maxParticles = 600;
    float lifetime = 2.f;          // seconds
    float lifetimeJitter = 0.3f;   // fraction
    std::string shape = "point";   // point | sphere | box | disc | cone
    Vec3 shapeSize{0.2f, 0.2f, 0.2f};
    Vec3 direction{0.f, 1.f, 0.f}; // local emission direction
    float speed = 1.f;
    float speedJitter = 0.3f;      // fraction
    float spread = 15.f;           // cone half-angle (degrees) around direction
    float gravity = 0.f;           // m/s^2 downwards; negative = buoyant (hot gas rises)
    float drag = 0.5f;             // velocity damping per second
    float turbulence = 0.5f;       // curl-noise swirl strength (m/s)
    float turbulenceScale = 1.f;   // swirl size (m)
    float wind = 1.f;              // how much the environment wind carries particles
    float sizeStart = 0.3f;
    float sizeEnd = 0.6f;
    float sizeJitter = 0.2f;       // fraction
    Vec4 colorStart{1.f, 0.85f, 0.55f, 1.f};
    Vec4 colorEnd{1.f, 0.35f, 0.1f, 0.f};
    float intensity = 1.f;         // HDR brightness (emissive looks)
    float stretch = 0.f;           // streak length along velocity (seconds of motion)
    float spin = 0.f;              // random rotation speed (degrees/s)
    float softness = 0.4f;         // soft-particle fade where they meet geometry (m)
    bool worldSpace = true;        // particles stay where they were emitted when the emitter moves
    bool collide = false;          // stop at floorHeight
    float floorHeight = 0.f;       // world y of the collision plane
    float bounce = 0.f;            // 0 = die on impact, >0 = bounce
    int splash = 0;                // droplets spawned per impact (rain)
    float light = 0.f;             // the emitter lights the scene (flickers with the simulation)
    Vec4 lightColor{1.f, 0.6f, 0.3f, 1.f};
    float lightRange = 8.f;
    bool prewarm = true;           // start fully developed (a fire that is already burning)
    int seed = 0;
    // --- GPU simulation (visual effects with up to millions of particles) -------------------
    // simulation "gpu" runs on the GPU compute pipeline (not deterministic, not countable by
    // gameplay); "cpu" is the deterministic gameplay path. Fields below only apply to "gpu".
    std::string simulation = "cpu";   // cpu | gpu
    std::string facing = "camera";    // camera | velocity | horizontal | ribbon | mesh
    std::string mesh;                 // facing "mesh": primitive, "asset:...", fx:leaf, fx:shard, fx:pebble
    float roughness = 0.6f;           // mesh particles
    float metallic = 0.f;             // mesh particles
    std::string texture;              // look "sprite": sprite / flipbook sheet (project-relative)
    int flipbookColumns = 1;
    int flipbookRows = 1;
    float flipbookFps = 0.f;          // 0 = the sheet plays once over each particle's life
    std::string colorGradient;        // "#rrggbbaa@0 #rrggbbaa@0.5 ..." (overrides colorStart/colorEnd)
    std::string sizeCurve;            // "1@0 1.4@0.3 0@1": multiplies the size over life
    std::string opacityCurve;         // multiplies the opacity over life
    std::string shapeMesh;            // shape "mesh": mesh to emit from ("" = this entity's mesh)
    std::string field = "none";       // none | vortex | attractor | texture
    float fieldStrength = 1.f;
    float fieldRadius = 2.f;          // m
    Vec3 fieldCenter{0.f, 0.f, 0.f};  // local offset
    Vec3 fieldAxis{0.f, 1.f, 0.f};    // vortex axis (local)
    float fieldPull = 0.f;            // vortex: inward pull (m/s)
    float fieldLift = 0.f;            // vortex: lift along the axis (m/s)
    std::string fieldTexture;         // field "texture": vector field (.fga), project-relative
    Vec3 fieldSize{4.f, 4.f, 4.f};    // field "texture": box covered by the field (m)
    bool depthCollision = false;      // collide with everything on screen (scene depth + normals)
    bool stick = false;               // stick where they hit instead of bouncing / dying
    float friction = 0.3f;            // tangential slow-down on bounce
    std::string colliders;            // comma-separated entity names: spheres / planes from their meshes
    std::string subEmitter;           // entity (with gpu particles) spawned from these particles
    std::string subEmitOn = "death";  // death | collision | both
    int subEmitCount = 8;             // particles spawned per event
    float subEmitInherit = 0.3f;      // fraction of the parent's velocity inherited
    float trailLength = 0.25f;        // facing "ribbon": seconds of history
    int trailSegments = 12;           // facing "ribbon": history samples (2..32)
    bool sort = true;                 // blended looks: GPU sort back to front
    float hueVariation = 0.f;         // random hue rotation per particle (per burst for sub-emitted ones)

    static const TypeInfo& type();
};

/// A volumetric fluid simulation (fire, smoke, steam, explosions): a 3D Eulerian solver on
/// the GPU (fuel -> heat + soot, buoyancy, vorticity confinement, pressure projection),
/// rendered by ray marching with blackbody flame colors and lit, self-shadowed smoke.
/// The box sits on the entity: origin at the bottom center, `size` in meters.
struct FluidVolume {
    std::string preset;
    bool emitting = true;
    Vec3 size{1.6f, 3.2f, 1.6f};    // simulation box (m)
    int resolution = 96;            // cells along the longest side
    Vec3 sourceOffset{0.f, 0.15f, 0.f};  // source center, from the bottom center (m)
    float sourceRadius = 0.32f;     // m
    float fuel = 1.f;               // fuel fed per second (fire); 0 = pure smoke/steam source
    float heat = 1.f;               // heat released by burning fuel / injected directly
    float smoke = 0.5f;             // soot produced (dark smoke)
    float buoyancy = 1.f;           // how fast hot gas rises
    float vorticity = 0.4f;         // swirl that makes flames lick and smoke curl
    float turbulence = 0.5f;        // noise in the source flow
    float burnRate = 2.f;           // how fast fuel burns (flame height)
    float cooling = 1.2f;           // how fast heat fades
    float smokeFade = 0.25f;        // how fast smoke thins out
    float speed = 0.6f;             // initial upward speed at the source (m/s)
    float wind = 1.f;               // environment wind influence
    float flameIntensity = 1.f;     // brightness of the fire
    float flameTemperature = 1600.f;  // Kelvin at full heat: color from blackbody (1000 red .. 2500 yellow-white)
    Vec4 smokeColor{0.16f, 0.15f, 0.14f, 1.f};
    float smokeDensity = 1.f;
    float light = 5.f;              // light cast on the scene by the fire
    Vec4 lightColor{1.f, 0.62f, 0.32f, 1.f};
    float lightRange = 10.f;
    float burst = 0.f;              // seconds of heavy fuel injection at start (explosions)
    int seed = 0;

    static const TypeInfo& type();
};

/// A body of water with a spectral (FFT) ocean simulation: wind-driven waves with choppy
/// crests, whitecaps, shore foam, refraction, depth color and reflections. The entity's y
/// is the water level; size 0 = an endless ocean.
struct Water {
    float windSpeed = 8.f;         // m/s: wave height grows with it
    float windDirection = 30.f;    // degrees, 0 = waves travel toward +Z
    float choppiness = 1.2f;       // sharp crests (0 = rolling swell)
    float waveScale = 1.f;         // amplitude multiplier
    float patchSize = 220.f;       // largest simulated wavelength (m)
    float size = 0.f;              // square extent in meters (0 = endless)
    float depth = 60.f;            // water depth for wave dispersion (shallow water slows waves)
    Vec4 deepColor{0.015f, 0.07f, 0.1f, 1.f};    // light scattered in deep water
    Vec4 shallowColor{0.12f, 0.5f, 0.45f, 1.f};  // tint of the transmitted light in shallows
    float clarity = 6.f;           // meters you can see into the water
    float foam = 1.f;              // whitecaps + shoreline foam
    float reflections = 1.f;
    float refraction = 1.f;
    float roughness = 0.04f;       // micro-surface roughness (sun glints)
    int seed = 1;

    static const TypeInfo& type();
};

/// One Wander behaviour attached to an entity: the human/agent intent (natural
/// language) and its compiled, deterministic Wander source.
struct Script {
    std::string name;
    std::string intent;  // natural-language description (source of truth for humans)
    std::string source;  // Wander code (source of truth for the runtime)
    bool enabled = true;

    /// Structured spec an agent derived from the intent (rules, notes); params and tests
    /// come from the code itself. Serialized; see docs/WANDER.md "From intent to code".
    Json spec;
    /// Node positions of the graph view {"nodeId": [x, y]}. Serialized.
    Json graph;

    // Runtime cache (not serialized).
    std::shared_ptr<const wander::Program> program;
    std::string compiledSource;
    uint64_t compiledEpoch = 0;  // modules/builtins generation the program was compiled against
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
    bool autoExposure = false;         // adapt exposure to the scene brightness like an eye / camera
    float exposureCompensation = 0.f;  // EV stops added on top (auto or manual)
    float adaptationSpeed = 1.5f;      // auto exposure speed (higher = faster)
    bool showGrid = true;
    // Post-processing (HDR pipeline)
    float bloomIntensity = 0.55f;
    float bloomThreshold = 1.0f;
    float saturation = 1.05f;
    float contrast = 1.05f;
    float vignette = 0.22f;
    float grain = 0.f;                 // film grain 0..1
    float chromaticAberration = 0.f;   // lens color fringing toward the edges 0..1
    std::string look = "none";         // color grading look: none | warm | cool | teal_orange | golden_hour | bleach | noir | vivid | moonlight | vintage
    std::string lut;                   // optional .cube 3D LUT (project-relative), applied after the look
    float lookStrength = 1.f;          // blend of the look / LUT
    // Sky, atmosphere, lighting quality
    std::string skyMode = "gradient";  // gradient | atmosphere | hdri
    std::string hdri;                  // equirectangular .hdr panorama (skyMode "hdri"), project-relative
    float hdriRotation = 0.f;          // degrees around the vertical axis
    float hdriIntensity = 1.f;         // panorama brightness (sky and image-based light)
    float clouds = 0.f;                // cloud cover 0..1
    std::string cloudMode = "volumetric";  // volumetric (ray-marched, lit, casting shadows) | flat (cheap painted layer)
    float cloudHeight = 1500.f;        // base of the cloud layer (m above the ground)
    float cloudThickness = 1800.f;     // meters
    float cloudDensity = 1.f;          // 0.3 wispy .. 2 heavy/stormy
    float cloudScale = 1.f;            // feature size multiplier (0.5 small puffy .. 3 huge banks)
    float cloudSpeed = 8.f;            // drift (m/s) along windDirection
    float stars = 0.f;                 // night-sky stars 0..1
    float sunSize = 1.f;               // sun/moon disc size multiplier
    float fogHeight = 0.f;             // height falloff: > 0 makes fog pool near the ground
    float reflections = 1.f;           // image-based (sky) reflections strength
    float ao = 0.8f;                   // screen-space ambient occlusion strength
    float gi = 1.f;                    // screen-space global illumination (bounce + emissive light), 0 = off
    float giDistance = 4.f;            // GI ray length in meters (scaled up with view distance)
    float ssr = 1.f;                   // screen-space reflections on glossy surfaces, 0 = off
    bool taa = true;                   // temporal anti-aliasing (jitter + history)
    float renderScale = 1.f;           // real-time internal resolution (0.5..1); < 1 upscales with MetalFX
    float sharpen = 0.35f;             // contrast-adaptive sharpening after temporal filtering
    float aoRadius = 0.6f;             // meters
    float shadowSoftness = 1.f;        // penumbra size multiplier
    std::string tonemap = "aces";      // aces | agx | neutral | filmic | none
    float temperature = 0.f;           // white balance: -1 cool .. +1 warm
    float tint = 0.f;                  // -1 green .. +1 magenta
    float shadowDistance = 0.f;        // sun-shadow range in meters (0 = automatic)
    float godRays = 0.f;               // volumetric light: sun shafts and lamp cones through the air (0 = off)
    float haze = 0.02f;                // density of the air for volumetric light (dust, mist)
    float windSpeed = 2.f;             // m/s: carries smoke, rain, snow and particles
    float windDirection = 60.f;        // degrees, 0 = blowing toward +Z

    Vec3 sunDirection() const;  // direction light travels (from sun towards ground)
    static const TypeInfo& type();
};

}  // namespace sky

#include "skywalker/ecs/Components2D.h"  // sprites, tilemaps, 2D lights, text, UI, dialogue
#include "skywalker/ecs/WorldComponents.h"  // Terrain, Foliage
// Components of other subsystems (each in its own header).
#include "skywalker/ecs/AudioComponents.h"
#include "skywalker/ecs/PhysicsComponents.h"  // body, collider, character, joint, physics_world, nav_agent, navmesh
// Workstream components (each in its own header).
#include "skywalker/ecs/AnimationComponents.h"
// Workstream components (kept in their own headers).
#include "skywalker/ecs/GroomComponent.h"
#include "skywalker/ecs/ProcessComponent.h"  // process: pause modes, run order, interpolation
