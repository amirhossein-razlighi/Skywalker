// Skywalker standard shaders (Metal Shading Language).
//
// Compiled at runtime from source so that the editor and agents can hot-reload them
// (`shader_set`) and receive compiler diagnostics. Struct layouts must match
// MetalRenderer.mm exactly.
//
// Lighting model
//   * PBR: GGX specular + Lambert diffuse (metal/roughness), normal / ORM / emissive maps,
//     optional world-space triplanar projection, clearcoat lobe, subsurface wrap +
//     transmission, specular anti-aliasing.
//   * Toon: banded diffuse, crisp highlight, rim light; outlines are drawn as inverted hulls.
//   * Image-based lighting: the sky is rendered into a cubemap, GGX-prefiltered per mip
//     (split-sum with a BRDF lookup texture). Diffuse irradiance comes from the
//     roughest mip.
//   * Sun shadows: 4 cascades in one atlas, rotated Poisson PCF.
//   * The main pass (4x MSAA) writes lit color plus a thin G-buffer (albedo + material AO,
//     octahedral normal + roughness + metallic). A lighting-resolve pass then swaps the
//     sky-probe indirect light for screen-space GI and reflections and applies SSAO.
//   * Temporal: sub-pixel jitter every frame; a TAA pass (or N-sample accumulation for
//     stills) resolves aliasing and the noise of stochastic effects.

#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 cascadeViewProj[4];
    float4 cascadeSplits;  // view-space distance where each cascade ends
    float4 cameraPos;      // xyz, w = time
    float4 cameraForward;  // xyz, w = orthographic (1) / perspective (0)
    float4 sunDir;         // xyz = direction light travels, w = intensity
    float4 sunColor;       // rgb, w = sun disc size
    float4 skyTop;
    float4 skyHorizon;
    float4 ground;         // rgb, w = ambient strength
    float4 fog;            // rgb, w = density
    float4 params;         // x = exposure, y = light count, z = shadows on, w = shadow tile texel size (uv)
    float4 viewport;       // x = width, y = height (pixels), z = 1/width, w = 1/height
    float4 sky;            // x = mode (0 gradient, 1 atmosphere, 2 hdri), y = clouds, z = stars, w = reflections
    float4 extra;          // x = fog height falloff, y = shadow softness, z = env max mip, w = unused
    float4 hdri;           // x = rotation (rad), y = intensity, z = mip for env cube faces, w = mip count
    float4x4 prevViewProj; // previous frame, unjittered (reprojection)
    float4x4 viewProjNoJitter;
    float4 temporal;       // xy = jitter (NDC), z = frame index, w = sub-sample index
    float4 clouds;         // x = unused (coverage is sky.y), y = base height (m), z = thickness (m), w = density
    float4 clouds2;        // x = scale, y = drift speed (m/s), z = mode (0 volumetric, 1 flat), w = wind angle (rad)
    float4 cluster;        // x = tiles x, y = tiles y, z = depth slices, w = log(far / near)
    float4 cluster2;       // x = near (m), y = directional light count, zw = unused
};

// Clustered lighting: which cluster a pixel at `fragXY` (pixels) / `worldPos` belongs to.
static uint clusterOf(constant FrameUniforms& f, float2 fragXY, float3 worldPos) {
    float z = max(dot(worldPos - f.cameraPos.xyz, f.cameraForward.xyz), f.cluster2.x);
    int tx = clamp(int(fragXY.x * f.viewport.z * f.cluster.x), 0, int(f.cluster.x) - 1);
    int ty = clamp(int(fragXY.y * f.viewport.w * f.cluster.y), 0, int(f.cluster.y) - 1);
    int sl = clamp(int(log(z / f.cluster2.x) / max(f.cluster.w, 1e-3) * f.cluster.z), 0, int(f.cluster.z) - 1);
    return uint((sl * int(f.cluster.y) + ty) * int(f.cluster.x) + tx);
}

struct DrawUniforms {
    float4x4 model;
    float4x4 normalMatrix;
    float4 color;
    float4 emissive;      // rgb, w = strength
    float4 material;      // x = metallic, y = roughness, z = selected, w = shading (0 pbr, 1 toon, 2 unlit)
    float4 material2;     // xy = tiling, z = normal strength, w = triplanar
    float4 material3;     // x = clearcoat, y = subsurface, z = rim, w = outline width (px)
    float4 maps;          // x = albedo, y = normal, z = orm (0 = none, else 1 + occlusion strength), w = emissive map
    float4 outlineColor;
    float4 material4;     // x = alpha cutoff (0 = off)
};

struct PostUniforms {
    float4 params;   // x = exposure, y = bloom intensity, z = bloom threshold, w = saturation
    float4 params2;  // x = contrast, y = vignette, z = aspect, w = tonemap (0 aces, 1 agx, 2 neutral, 3 filmic, 4 none)
    float4 texel;    // xy = source texel size, z = ao strength, w = frame index
    float4 grade;    // x = temperature, y = tint
};

struct AOUniforms {
    float4x4 proj;
    float4x4 invProj;
    float4 params;   // x = radius (m), y = strength, zw = texel of the depth texture
};

struct EnvUniforms {
    float4 face;     // x = cube face, y = roughness, z = source size, w = mip
};

struct GPULight {
    float4 positionRange;   // xyz, w = range
    float4 colorIntensity;  // rgb, w = intensity
    float4 directionCone;   // xyz, w = cos(cone)
    float4 kind;            // x: 0 directional, 1 point, 2 spot
};

struct Vertex {
    packed_float3 position;
    packed_float3 normal;
    packed_float2 uv;
    packed_float4 color;  // vertex color (white for primitives)
};

struct MeshOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
    float4 color;
};

// Main pass outputs: lit HDR color + G-buffer.
//   gbufA (RGBA8):   rgb = albedo (linear), a = material ambient occlusion
//   gbufB (RGBA16F): xy = octahedral normal, z = roughness, w = metallic (0..1) or a
//                    "no screen-space lighting" flag (>= 2: sky, unlit, toon, outlines)
struct MainOut {
    float4 color [[color(0)]];
    float4 gbufA [[color(1)]];
    float4 gbufB [[color(2)]];
};

// Passes drawn over the resolved scene (water, particles, fluids) write color only.
struct EffectOut {
    float4 color [[color(0)]];
};

constant float kGbufNoLighting = 4.0;

static float2 octWrap(float2 v) { return (1.0 - abs(v.yx)) * select(float2(-1.0), float2(1.0), v.xy >= 0.0); }

static float2 octEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    n.xy = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    return n.xy;
}

static float3 octDecode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += select(float2(t), float2(-t), n.xy >= 0.0);
    return normalize(n);
}

static MainOut mainOut(float4 color, float3 albedo, float ao, float3 N, float roughness, float metallicOrFlag) {
    MainOut o;
    o.color = color;
    o.gbufA = float4(albedo, ao);
    o.gbufB = float4(octEncode(N), roughness, metallicOrFlag);
    return o;
}

static MainOut mainOutFlat(float4 color) { return mainOut(color, float3(0.0), 1.0, float3(0, 1, 0), 1.0, kGbufNoLighting); }

constexpr sampler shadowSampler(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
constexpr sampler materialSampler(coord::normalized, filter::linear, mip_filter::linear, address::repeat, max_anisotropy(8));
constexpr sampler presentSampler(coord::normalized, filter::linear, address::clamp_to_edge);
constexpr sampler linearClamp(coord::normalized, filter::linear, address::clamp_to_edge);
constexpr sampler cubeSampler(coord::normalized, filter::linear, mip_filter::linear);
constexpr sampler panoSampler(coord::normalized, filter::linear, mip_filter::linear, s_address::repeat, t_address::clamp_to_edge);

// ---------------------------------------------------------------------------
// Noise & hashing
// ---------------------------------------------------------------------------

static float hash12(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

static float valueNoise(float2 p) {
    float2 i = floor(p), f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + float2(1, 0)), u.x), mix(hash12(i + float2(0, 1)), hash12(i + float2(1, 1)), u.x), u.y);
}

static float fbm(float2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) {
        s += a * valueNoise(p);
        p = p * 2.03 + float2(17.1, 9.2);
        a *= 0.5;
    }
    return s;
}

static float interleavedGradientNoise(float2 px) { return fract(52.9829189 * fract(dot(px, float2(0.06711056, 0.00583715)))); }

// ---------------------------------------------------------------------------
// BRDF helpers
// ---------------------------------------------------------------------------

static float D_GGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (M_PI_F * d * d + 1e-7);
}

static float V_SmithGGX(float NdotV, float NdotL, float a) {
    float a2 = a * a;
    float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

static float3 F_Schlick(float3 F0, float VdotH) { return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0); }

struct SurfaceData {
    float3 albedo;
    float alpha;
    float metallic;
    float roughness;
    float ao;
    float clearcoat;
    float subsurface;
    float3 N;
};

static float3 directLight(SurfaceData s, float3 V, float3 L, float3 radiance) {
    float3 N = s.N;
    float NdotL = dot(N, L);
    float NdotV = max(dot(N, V), 1e-4);
    // Subsurface: wrap lighting so light reaches past the terminator.
    float wrap = s.subsurface * 0.5;
    float diffNdotL = saturate((NdotL + wrap) / (1.0 + wrap));
    float nl = saturate(NdotL);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));
    float a = s.roughness * s.roughness;
    float3 F0 = mix(float3(0.04), s.albedo, s.metallic);
    float3 F = F_Schlick(F0, VdotH);
    float3 spec = D_GGX(NdotH, a) * V_SmithGGX(NdotV, max(nl, 1e-4), a) * F;
    float3 kd = (1.0 - F) * (1.0 - s.metallic);
    float3 color = (kd * s.albedo / M_PI_F * diffNdotL + spec * nl) * radiance * M_PI_F;
    // Transmission (thin parts glow when backlit)
    if (s.subsurface > 0.0) {
        float back = pow(saturate(dot(V, -L)), 4.0) * s.subsurface;
        color += s.albedo * radiance * back * 0.6 * (1.0 - s.metallic);
    }
    // Clearcoat lobe (fixed glossy layer, IOR 1.5)
    if (s.clearcoat > 0.0) {
        float ac = 0.06 * 0.06;
        float Fc = 0.04 + 0.96 * pow(1.0 - VdotH, 5.0);
        float cc = D_GGX(NdotH, ac) * V_SmithGGX(NdotV, max(nl, 1e-4), ac) * Fc * s.clearcoat;
        color = color * (1.0 - Fc * s.clearcoat) + cc * radiance * nl * M_PI_F;
    }
    return color;
}

static float3 toonLight(SurfaceData s, float3 V, float3 L, float3 radiance) {
    float NdotL = dot(s.N, L);
    float w = fwidth(NdotL) * 1.5 + 0.01;
    float band = smoothstep(0.0, w, NdotL) * 0.55 + smoothstep(0.45, 0.45 + w, NdotL) * 0.45;
    float3 H = normalize(V + L);
    float shin = mix(180.0, 12.0, s.roughness);
    float sp = pow(saturate(dot(s.N, H)), shin);
    float hl = smoothstep(0.5 - w, 0.5 + w, sp) * (1.0 - s.roughness * 0.7);
    float3 base = s.albedo * (1.0 - s.metallic * 0.5);
    return (base * band + mix(float3(1.0), s.albedo, s.metallic) * hl * 0.6) * radiance;
}

// ---------------------------------------------------------------------------
// Shadows: 4 cascades in a 2x2 atlas, rotated Poisson PCF
// ---------------------------------------------------------------------------

constant float2 kPoisson[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457), float2(-0.203, 0.621),
    float2(0.962, -0.195), float2(0.473, -0.480), float2(0.519, 0.767), float2(0.185, -0.893),
    float2(0.507, 0.064), float2(0.896, 0.412), float2(-0.322, -0.933), float2(-0.792, -0.598)};

static float shadowFactor(float3 worldPos, float3 N, float2 pixel, constant FrameUniforms& f, depth2d<float> atlas) {
    if (f.params.z < 0.5) return 1.0;
    float viewDepth = dot(worldPos - f.cameraPos.xyz, f.cameraForward.xyz);
    int c = 0;
    if (viewDepth > f.cascadeSplits.x) c = 1;
    if (viewDepth > f.cascadeSplits.y) c = 2;
    if (viewDepth > f.cascadeSplits.z) c = 3;
    if (viewDepth > f.cascadeSplits.w) return 1.0;
    float3 L = -f.sunDir.xyz;
    float texelWorld = f.cascadeSplits[c] * 0.004 + 0.01;
    float3 p = worldPos + N * texelWorld * 1.5;  // normal offset against acne
    float4 lc = f.cascadeViewProj[c] * float4(p, 1.0);
    float3 ndc = lc.xyz / lc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0) return 1.0;
    float2 tile = float2(c % 2, c / 2) * 0.5;
    float2 auv = tile + uv * 0.5;
    float bias = 0.0006 + 0.0012 * (1.0 - saturate(dot(N, L)));
    float radius = f.params.w * (1.0 + 1.6 * f.extra.y) / (1.0 + float(c) * 0.6);
    float ang = interleavedGradientNoise(pixel) * 6.2831853;
    float2x2 rot = float2x2(float2(cos(ang), sin(ang)), float2(-sin(ang), cos(ang)));
    float2 lo = tile + f.params.w, hi = tile + 0.5 - f.params.w;
    float sum = 0.0;
    for (int i = 0; i < 12; ++i) {
        float2 s = clamp(auv + rot * kPoisson[i] * radius, lo, hi);
        sum += atlas.sample_compare(shadowSampler, s, ndc.z - bias);
    }
    float sh = sum / 12.0;
    float fade = saturate((f.cascadeSplits.w - viewDepth) / (f.cascadeSplits.w * 0.1));
    return mix(1.0, sh, fade);
}

// One-tap sun visibility (for ray-marched volumetric light, where many samples average out).
static float sunVisibility(float3 worldPos, constant FrameUniforms& f, depth2d<float> atlas) {
    if (f.params.z < 0.5) return 1.0;
    float viewDepth = dot(worldPos - f.cameraPos.xyz, f.cameraForward.xyz);
    int c = 0;
    if (viewDepth > f.cascadeSplits.x) c = 1;
    if (viewDepth > f.cascadeSplits.y) c = 2;
    if (viewDepth > f.cascadeSplits.z) c = 3;
    if (viewDepth > f.cascadeSplits.w) return 1.0;
    float4 lc = f.cascadeViewProj[c] * float4(worldPos, 1.0);
    float3 ndc = lc.xyz / lc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0) return 1.0;
    float2 auv = float2(c % 2, c / 2) * 0.5 + uv * 0.5;
    return atlas.sample_compare(shadowSampler, auv, ndc.z - 0.0008);
}

// ---------------------------------------------------------------------------
// Fullscreen helpers
// ---------------------------------------------------------------------------

struct FullscreenOut {
    float4 position [[position]];
    float2 ndc;
};

vertex FullscreenOut fullscreenVertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    FullscreenOut o;
    o.position = float4(p, 0.0, 1.0);
    o.ndc = p;
    return o;
}

static float2 uvOf(FullscreenOut in) { return float2(in.ndc.x * 0.5 + 0.5, 0.5 - in.ndc.y * 0.5); }

static float fogFactor(constant FrameUniforms& f, float3 worldPos) {
    float3 d = worldPos - f.cameraPos.xyz;
    float dist = length(d);
    float density = f.fog.w;
    float k = f.extra.x;
    float amount;
    if (k > 1e-4) {
        float y0 = f.cameraPos.y, dy = d.y;
        float base = exp(-k * max(y0, 0.0));
        float integral = abs(dy) > 1e-3 ? base * (1.0 - exp(-k * dy)) / (k * dy) : base;
        amount = 1.0 - exp(-density * dist * max(integral, 0.0) * 3.0);
    } else {
        amount = 1.0 - exp(-dist * density);
    }
    return saturate(amount);
}

// ---------------------------------------------------------------------------
// Effects helpers
// ---------------------------------------------------------------------------

constexpr sampler pointClamp(coord::normalized, filter::nearest, address::clamp_to_edge);
constexpr sampler oceanSampler(coord::normalized, filter::linear, mip_filter::linear, address::repeat, max_anisotropy(8));

static float3 reconstructWorld(constant FrameUniforms& f, float2 uv, float depth) {
    float4 p = f.invViewProj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return p.xyz / p.w;
}

static float3 pointLightAt(GPULight l, float3 pos, float3 n, thread float3& dirOut) {
    float3 Ll;
    float atten = 1.0;
    if (l.kind.x < 0.5) {
        Ll = -l.directionCone.xyz;
    } else {
        float3 toL = l.positionRange.xyz - pos;
        float dist = length(toL);
        Ll = toL / max(dist, 1e-4);
        float r = l.positionRange.w;
        float falloff = saturate(1.0 - pow(dist / r, 4.0));
        atten = falloff * falloff / (dist * dist + 1.0);
        if (l.kind.x > 1.5) {
            float cd = dot(-Ll, l.directionCone.xyz);
            atten *= smoothstep(l.directionCone.w, mix(l.directionCone.w, 1.0, 0.2), cd);
        }
    }
    dirOut = Ll;
    return l.colorIntensity.rgb * l.colorIntensity.w * atten;
}
