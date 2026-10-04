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
            SKY_FIELD_RANGE(MeshRenderer, layers, Int,
                            "Render layers bitmask (bit 0 = layer 1 .. bit 19 = layer 20; default 1). A camera draws the mesh when "
                            "its cullMask shares a bit; a light lights it when the light's cullMask does. Name layers in game.json "
                            "and set them by name with render_layers",
                            0.f, 1048575.f),
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
            SKY_FIELD_RANGE(Light, cullMask, Int,
                            "Render layers this light illuminates (bitmask, bit 0 = layer 1; default all 20). E.g. a rim light that "
                            "only lights the hero: put the hero on layer 2 and set cullMask 2",
                            0.f, 1048575.f),
            SKY_FIELD_RANGE(Light, specular, Float, "Specular highlight strength (0 = diffuse only)", 0.f, 16.f),
            SKY_FIELD_RANGE(Light, indirect, Float, "Contribution to world-space GI (reserved for GI probes / lightmaps)", 0.f, 16.f),
            SKY_FIELD_RANGE(Light, volumetric, Float, "Strength in volumetric light / god rays (environment godRays)", 0.f, 16.f),
            SKY_FIELD_RANGE(Light, temperature, Float,
                            "Color temperature in Kelvin, multiplied with color (1900 candle, 2700 tungsten, 4000 fluorescent, "
                            "5500 noon, 6500 white, 9000 overcast sky); 0 = off",
                            0.f, 40000.f),
            SKY_FIELD_RANGE(Light, innerAngle, Float,
                            "Spot: half-angle of the full-intensity inner cone in degrees (0 = automatic); smaller = softer edge", 0.f, 89.f),
            SKY_FIELD(Light, negative, Bool, "Subtract light instead of adding it (stylized darkening)"),
            SKY_FIELD(Light, distanceFade, Bool, "Fade the light out with distance from the camera (cheap crowds of small lights)"),
            SKY_FIELD_RANGE(Light, fadeBegin, Float, "Distance fade: meters from the camera where fading starts", 0.f, 100000.f),
            SKY_FIELD_RANGE(Light, fadeLength, Float, "Distance fade: meters over which the light fades out", 0.01f, 100000.f),
            SKY_FIELD_ENUM(Light, attenuation,
                           "Falloff: smooth = soft artist-friendly curve that reaches 0 at range; inverse_square = physical 1/d^2 "
                           "(brighter near the source, uses size), still windowed to range",
                           "smooth", "inverse_square"),
            SKY_FIELD_RANGE(Light, size, Float, "Emitter radius in meters (inverse_square falloff peak)", 0.001f, 100.f),
            SKY_FIELD(Light, castShadows, Bool,
                      "Point/spot: occluders block the light (no light through walls). Rendered in the local shadow atlas, "
                      "cached while nothing in range moves; see shadow_atlas_info"),
            SKY_FIELD_RANGE(Light, shadowBias, Float,
                            "Shadow depth bias in meters: raise if lit surfaces show dark speckles (acne), lower if "
                            "shadows detach from their casters (peter-panning)", 0.f, 1.f),
            SKY_FIELD_RANGE(Light, shadowNormalBias, Float, "Shadow offset along the surface normal in shadow-map texels "
                            "(acne on surfaces at grazing angles)", 0.f, 8.f),
            SKY_FIELD_RANGE(Light, shadowResolution, Int,
                            "Shadow map slot hint in px (256 small, 1024 hero light, 2048 max); 0 = automatic from the "
                            "light's size on screen", 0.f, 4096.f),
            SKY_FIELD_RANGE(Light, shadowMaxDistance, Float,
                            "Camera distance (m) beyond which this light's shadow fades out (saves atlas space); 0 = no limit",
                            0.f, 100000.f),
            SKY_FIELD_ENUM(Light, shadowMode,
                           "Point light shadow projection: cube = 6 exact views; dual_paraboloid = 2 views, cheaper to "
                           "update but approximate for large flat polygons",
                           "cube", "dual_paraboloid"),
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
            SKY_FIELD_RANGE(Camera, aperture, Float, "Depth of field f-stop: 1.4 very shallow .. 16 deep (0 = off)", 0.f, 64.f),
            SKY_FIELD_RANGE(Camera, focusDistance, Float, "Focus distance in meters (0 = autofocus on the frame center)", 0.f, 100000.f),
            SKY_FIELD_RANGE(Camera, motionBlur, Float, "Motion blur shutter (0.5 = film-like 180 degrees, 0 = off)", 0.f, 1.f),
            SKY_FIELD_RANGE(Camera, tiltShift, Float,
                            "Tilt-shift miniature look: blur above and below a sharp band across the middle of the frame (0 = off, 1 = strong)",
                            0.f, 1.f),
            SKY_FIELD_RANGE(Camera, cullMask, Int,
                            "Render layers this camera draws (bitmask, bit 0 = layer 1; default all 20). E.g. hide first-person arms "
                            "from a security camera, or editor-only helpers from the game camera",
                            0.f, 1048575.f),
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
                           "spark = glowing streaks; rain = lit falling streaks; snow = soft flakes; mist = drifting fog; "
                           "sprite = textured / flipbook sheet (`texture`, gpu only)",
                           "glow", "flame", "smoke", "spark", "rain", "snow", "mist", "sprite"),
            SKY_FIELD_RANGE(ParticleEmitter, rate, Float, "Particles per second", 0.f, 4000000.f),
            SKY_FIELD_RANGE(ParticleEmitter, burst, Int, "Particles emitted at once when it starts (explosions); Wander burst(n) "
                            "emits more", 0.f, 20000.f),
            SKY_FIELD_RANGE(ParticleEmitter, maxParticles, Int,
                            "Upper bound of live particles (cpu: up to 50000; gpu: up to 4000000)", 1.f, 4000000.f),
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
            // GPU simulation
            SKY_FIELD_ENUM(ParticleEmitter, simulation,
                           "cpu = deterministic engine simulation (gameplay, countable); gpu = compute shaders, "
                           "millions of particles, depth collisions, sub-emitters, ribbons, mesh particles (visuals only)",
                           "cpu", "gpu"),
            SKY_FIELD_ENUM(ParticleEmitter, facing,
                           "gpu: camera = billboards; velocity = stretched along motion (`stretch`); horizontal = flat "
                           "on the ground plane (ripples, decals); ribbon = trails (`trailLength`); mesh = instanced lit meshes (`mesh`)",
                           "camera", "velocity", "horizontal", "ribbon", "mesh"),
            SKY_FIELD(ParticleEmitter, mesh, String,
                      "gpu facing mesh: primitive (cube, sphere...), \"asset:<path>\", or built-in fx:leaf, fx:shard, fx:pebble"),
            SKY_FIELD_RANGE(ParticleEmitter, roughness, Float, "gpu mesh particles: surface roughness", 0.02f, 1.f),
            SKY_FIELD_RANGE(ParticleEmitter, metallic, Float, "gpu mesh particles: metalness", 0.f, 1.f),
            SKY_FIELD(ParticleEmitter, texture, String, "gpu look sprite: sprite or flipbook sheet (png/jpg), project-relative"),
            SKY_FIELD_RANGE(ParticleEmitter, flipbookColumns, Int, "Flipbook sheet columns", 1.f, 64.f),
            SKY_FIELD_RANGE(ParticleEmitter, flipbookRows, Int, "Flipbook sheet rows", 1.f, 64.f),
            SKY_FIELD_RANGE(ParticleEmitter, flipbookFps, Float, "Flipbook frames per second (0 = once over each life; frames cross-fade)",
                            0.f, 240.f),
            SKY_FIELD(ParticleEmitter, colorGradient, String,
                      "gpu: color over life, e.g. \"#fff6d0@0 #ffa030@0.2 #a01800@0.7 #20000000@1\" (overrides colorStart/End)"),
            SKY_FIELD(ParticleEmitter, sizeCurve, String, "gpu: size multiplier over life, e.g. \"0@0 1@0.1 0.3@1\""),
            SKY_FIELD(ParticleEmitter, opacityCurve, String, "gpu: opacity multiplier over life, e.g. \"0@0 1@0.2 0@1\""),
            SKY_FIELD(ParticleEmitter, shapeMesh, String, "gpu shape mesh: emit from this mesh's surface (\"\" = this entity's mesh)"),
            SKY_FIELD_ENUM(ParticleEmitter, field,
                           "gpu force field: vortex = swirl around fieldAxis (tornado, magic); attractor = pull toward "
                           "fieldCenter; texture = vector field from fieldTexture (.fga)",
                           "none", "vortex", "attractor", "texture"),
            SKY_FIELD_RANGE(ParticleEmitter, fieldStrength, Float, "Force field strength (m/s; attractor m/s^2)", -1000.f, 1000.f),
            SKY_FIELD_RANGE(ParticleEmitter, fieldRadius, Float, "Force field radius (m)", 0.01f, 1000.f),
            SKY_FIELD(ParticleEmitter, fieldCenter, Vec3, "Force field center (local offset)"),
            SKY_FIELD(ParticleEmitter, fieldAxis, Vec3, "Vortex axis (local)"),
            SKY_FIELD_RANGE(ParticleEmitter, fieldPull, Float, "Vortex inward pull (m/s)", -100.f, 100.f),
            SKY_FIELD_RANGE(ParticleEmitter, fieldLift, Float, "Vortex lift along its axis (m/s)", -100.f, 100.f),
            SKY_FIELD(ParticleEmitter, fieldTexture, String, "Vector field file (.fga from Houdini or EmberGen)"),
            SKY_FIELD(ParticleEmitter, fieldSize, Vec3, "Box covered by the vector field (m, centered on fieldCenter)"),
            SKY_FIELD(ParticleEmitter, depthCollision, Bool,
                      "gpu: collide with everything visible on screen (scene depth + normals): sparks bouncing off "
                      "props, rain splashing on roofs"),
            SKY_FIELD(ParticleEmitter, stick, Bool, "gpu: particles stick where they collide (paint, snow, blood)"),
            SKY_FIELD_RANGE(ParticleEmitter, friction, Float, "gpu: tangential slow-down on bounce", 0.f, 1.f),
            SKY_FIELD_ENTITIES(ParticleEmitter, colliders,
                      "gpu: entities particles bounce off: sphere meshes collide as spheres, planes/quads as planes, "
                      "other meshes as bounding spheres"),
            SKY_FIELD_ENTITY(ParticleEmitter, subEmitter,
                      "gpu: another gpu particles entity spawned where these particles die/hit (sparks -> embers, "
                      "rockets -> fireworks)"),
            SKY_FIELD_ENUM(ParticleEmitter, subEmitOn, "When sub-emitter particles spawn", "death", "collision", "both"),
            SKY_FIELD_RANGE(ParticleEmitter, subEmitCount, Int, "Sub-emitter particles per event", 1.f, 1024.f),
            SKY_FIELD_RANGE(ParticleEmitter, subEmitInherit, Float, "Fraction of the parent's velocity inherited", 0.f, 2.f),
            SKY_FIELD_RANGE(ParticleEmitter, trailLength, Float, "Ribbon trail length (seconds of history)", 0.01f, 10.f),
            SKY_FIELD_RANGE(ParticleEmitter, trailSegments, Int, "Ribbon history samples", 2.f, 32.f),
            SKY_FIELD(ParticleEmitter, sort, Bool, "gpu blended looks: sort back to front on the GPU (off = faster)"),
            SKY_FIELD(ParticleEmitter, lightShadows, Bool,
                      "The cast light (light > 0) is blocked by occluders (torches in caves; costs a cube shadow map)"),
            SKY_FIELD_RANGE(ParticleEmitter, hueVariation, Float,
                            "gpu: random hue rotation per particle; sub-emitted particles share their event's hue (fireworks)",
                            0.f, 1.f),
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
            SKY_FIELD(FluidVolume, lightShadows, Bool,
                      "The fire's light is blocked by occluders (campfires in caves and rooms; costs a cube shadow map)"),
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
                                               "warp, erosion, thermal, terraces, beachWidth, seaLevel; shape heightmap: heightmap image, detailNoise)",
                           R"({"type":"object"})"),
            SKY_FIELD_JSON(Terrain, layers,
                           "Material layers, base first. Each: {name, texture, normalMap, ormMap, color, roughness, tiling (m), "
                           "heightMin, heightMax, slopeMin, slopeMax, noise, sharpness}. Rules auto-paint the weights.",
                           R"({"type":"array","items":{"type":"object"}})"),
            SKY_FIELD_JSON(Terrain, edits,
                           "Recorded hand edits in terrain-local meters, replayed in order over the generator when the terrain "
                           "cache is rebuilt: {op: sculpt|paint|autopaint, strokes, layer}. Written by terrain_sculpt, "
                           "terrain_paint and terrain_layers; cleared by terrain_generate.",
                           R"({"type":"array","items":{"type":"object"}})"),
            SKY_FIELD(Terrain, waterLevel, Float, "World height of the water line (wet sand/soil just above it)"),
            SKY_FIELD_RANGE(Terrain, wetBand, Float, "Meters above the water line that stay damp", 0.f, 20.f),
            SKY_FIELD_RANGE(Terrain, detail, Float, "Level-of-detail quality multiplier", 0.25f, 4.f),
            SKY_FIELD_RANGE(Terrain, macroVariation, Float,
                            "Large-scale tone and hue variation over the whole landscape (0 = off, 0.5 natural)", 0.f, 1.f),
            SKY_FIELD(Terrain, castShadows, Bool, "Cast sun shadows"),
            SKY_FIELD(Terrain, overlay, String,
                      "Image draped over the whole terrain (row 0 = -Z edge): political/region maps, borders, paper maps. "
                      "Alpha masks it; swap it at run time for map modes"),
            SKY_FIELD_RANGE(Terrain, overlayOpacity, Float, "Overlay strength (times the image alpha)", 0.f, 1.f),
            SKY_FIELD_ENUM(Terrain, overlayBlend, "mix paints over the ground (matte, keeps the relief shading), multiply tints it, "
                           "glow adds the color unlit (highlights, borders that read at night)", "mix", "multiply", "glow"),
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
                           "Layers: [{preset, mesh | prefab, color, density (/m²), scaleMin, scaleMax, slopeMin, slopeMax, heightMin, "
                           "heightMax, terrainLayer, wind, cullDistance, castShadows, clumping, alignToNormal, impostors, "
                           "impostorDistance, impostorResolution, impostorFrames}]. Distant instances of heavy meshes draw as "
                           "octahedral impostors (see impostor_bake).",
                           R"json({"type":"array","items":{"type":"object","properties":{
                               "impostors":{"type":"boolean","description":"Draw far instances as baked octahedral impostors (default true; used when the mesh has 300+ triangles)"},
                               "impostorDistance":{"type":"number","description":"Camera distance in meters where instances become impostors: 0 = automatic from on-screen size, -1 = never"},
                               "impostorResolution":{"type":"integer","description":"Impostor atlas edge in pixels: 0 = automatic, 512-2048 by model size"},
                               "impostorFrames":{"type":"integer","description":"Capture directions per atlas side, 4-32 (default 12)"}}}})json"),
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
            SKY_FIELD(Environment, autoExposure, Bool, "Adapt exposure to scene brightness (eye adaptation)"),
            SKY_FIELD_RANGE(Environment, exposureCompensation, Float, "Exposure compensation in EV stops", -6.f, 6.f),
            SKY_FIELD_RANGE(Environment, adaptationSpeed, Float, "Auto exposure adaptation speed", 0.05f, 20.f),
            SKY_FIELD(Environment, showGrid, Bool, "Draw the editor ground grid"),
            SKY_FIELD_RANGE(Environment, bloomIntensity, Float, "Glow around bright/emissive things (0 = off)", 0.f, 5.f),
            SKY_FIELD_RANGE(Environment, bloomThreshold, Float, "Brightness where glow starts (lower = more glow)", 0.f, 10.f),
            SKY_FIELD_RANGE(Environment, saturation, Float, "Color saturation (1 = neutral)", 0.f, 2.f),
            SKY_FIELD_RANGE(Environment, contrast, Float, "Contrast (1 = neutral)", 0.5f, 2.f),
            SKY_FIELD_RANGE(Environment, vignette, Float, "Darken the image corners", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, grain, Float, "Film grain", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, chromaticAberration, Float, "Lens color fringing toward the frame edges", 0.f, 1.f),
            SKY_FIELD_ENUM(Environment, look, "Color grading look", "none", "warm", "cool", "teal_orange", "golden_hour", "bleach",
                           "noir", "vivid", "moonlight", "vintage"),
            SKY_FIELD(Environment, lut, String, "Optional .cube 3D LUT file (project-relative); when set it replaces the look (lookStrength blends it)"),
            SKY_FIELD_RANGE(Environment, lookStrength, Float, "Strength of the look / LUT", 0.f, 1.f),
            SKY_FIELD_ENUM(Environment, skyMode,
                           "gradient = two artist colors; atmosphere = physically inspired sky from the sun; hdri = a "
                           "photographed .hdr panorama lights and backs the scene (set `hdri`)",
                           "gradient", "atmosphere", "hdri"),
            SKY_FIELD(Environment, hdri, String, "Equirectangular .hdr panorama, project-relative (skyMode hdri)"),
            SKY_FIELD(Environment, hdriRotation, Float, "Panorama rotation in degrees (line its sun up with sunAzimuth)"),
            SKY_FIELD_RANGE(Environment, hdriIntensity, Float, "Panorama brightness", 0.f, 20.f),
            SKY_FIELD_RANGE(Environment, clouds, Float, "Procedural cloud cover", 0.f, 1.f),
            SKY_FIELD_ENUM(Environment, cloudMode, "Clouds: volumetric (ray-marched, lit, shadows) or flat (cheap)", "volumetric", "flat"),
            SKY_FIELD_RANGE(Environment, cloudHeight, Float, "Cloud layer base height in meters", 100.f, 12000.f),
            SKY_FIELD_RANGE(Environment, cloudThickness, Float, "Cloud layer thickness in meters", 100.f, 10000.f),
            SKY_FIELD_RANGE(Environment, cloudDensity, Float, "Cloud density: 0.3 wispy .. 2 stormy", 0.f, 4.f),
            SKY_FIELD_RANGE(Environment, cloudScale, Float, "Cloud feature size (0.5 small puffs .. 3 huge banks)", 0.1f, 8.f),
            SKY_FIELD_RANGE(Environment, cloudSpeed, Float, "Cloud drift speed in m/s (along windDirection)", 0.f, 100.f),
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
            SKY_FIELD_RANGE(Environment, renderScale, Float,
                            "Real-time internal resolution: 0.5-0.77 renders fewer pixels and MetalFX temporal upscaling "
                            "reconstructs full resolution (faster); 1 = native. Stills and captures with samples > 1 render native.",
                            0.33f, 1.f),
            SKY_FIELD_RANGE(Environment, sharpen, Float, "Sharpening after temporal anti-aliasing", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, shadowSoftness, Float, "Sun shadow penumbra size", 0.f, 6.f),
            SKY_FIELD_ENUM(Environment, tonemap, "HDR to display curve", "aces", "agx", "neutral", "filmic", "none"),
            SKY_FIELD_RANGE(Environment, temperature, Float, "White balance: -1 cool .. +1 warm", -1.f, 1.f),
            SKY_FIELD_RANGE(Environment, tint, Float, "White balance: -1 green .. +1 magenta", -1.f, 1.f),
            SKY_FIELD_RANGE(Environment, shadowDistance, Float, "Sun shadow range in meters (0 = automatic)", 0.f, 5000.f),
            SKY_FIELD_RANGE(Environment, godRays, Float,
                            "Volumetric light: sun shafts through trees and windows, cones under lamps (0 = off, 1 = natural)",
                            0.f, 8.f),
            SKY_FIELD_RANGE(Environment, haze, Float, "Air density for volumetric light, extinction per meter: 0.0003 clear landscape, 0.002 hazy valley, 0.01-0.03 misty alley or interior", 0.f, 1.f),
            SKY_FIELD_RANGE(Environment, windSpeed, Float, "Wind (m/s) that carries smoke, rain, snow and particles", 0.f, 60.f),
            SKY_FIELD(Environment, windDirection, Float, "Direction the wind blows toward (degrees, 0 = +Z)"),
            SKY_FIELD_RANGE(Environment, localShadowAtlas, Int,
                            "Point/spot shadow atlas size in px: 2048 (16 MB), 4096 (64 MB, default), 8192 (256 MB)",
                            1024.f, 8192.f),
            SKY_FIELD_RANGE(Environment, localShadowLights, Int,
                            "Most point/spot lights with shadows per frame (the most important on screen); the rest light "
                            "without shadows. 0 = no local shadows", 0.f, 64.f),
            SKY_FIELD_RANGE(Environment, localShadowUpdates, Int,
                            "Point/spot shadow views re-rendered per frame when lights or casters move (cube light = 6, "
                            "spot = 1); later updates wait a frame. Stills render all", 1.f, 384.f),
            SKY_FIELD_RANGE(Environment, probeBudget, Int,
                            "Reflection probes with a slot in the probe atlas (the most important in view first; the rest "
                            "fall back to the sky). Max 32; 0 = probes off", 0.f, 32.f),
            SKY_FIELD_RANGE(Environment, probeUpdates, Int,
                            "Reflection probe cube faces captured per frame (a whole probe = 6); later captures wait a "
                            "frame. Stills capture everything", 1.f, 192.f),
        }};
    return info;
}

}  // namespace sky
