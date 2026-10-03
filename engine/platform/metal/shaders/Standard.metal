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
//   * The main pass writes two targets: lit color and the "indirect" (ambient) part, so
//     SSAO can darken only indirect light in the composite.

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
};

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

struct MainOut {
    float4 color [[color(0)]];
    float4 ambient [[color(1)]];
};

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
// Sky: artist gradient or a physically inspired single-scattering atmosphere,
// plus procedural clouds and stars.
// ---------------------------------------------------------------------------

static float2 raySphere(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return float2(-1.0);
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

static float3 atmosphere(float3 rd, float3 toSun, float sunIntensity) {
    const float Rg = 6360e3, Ra = 6420e3, Hr = 8e3, Hm = 1.2e3;
    const float3 betaR = float3(5.5e-6, 13.0e-6, 22.4e-6);
    const float betaM = 21e-6;
    float3 ro = float3(0.0, Rg + 50.0, 0.0);
    rd.y = max(rd.y, -0.02);
    rd = normalize(rd);
    float2 t = raySphere(ro, rd, Ra);
    float tMax = t.y;
    const int N = 12, M = 4;
    float seg = tMax / float(N);
    float odR = 0.0, odM = 0.0;
    float3 sumR = 0.0, sumM = 0.0;
    for (int i = 0; i < N; ++i) {
        float3 p = ro + rd * (seg * (float(i) + 0.5));
        float h = length(p) - Rg;
        float hr = exp(-h / Hr) * seg, hm = exp(-h / Hm) * seg;
        odR += hr;
        odM += hm;
        float2 ts = raySphere(p, toSun, Ra);
        float segL = ts.y / float(M);
        float odRL = 0.0, odML = 0.0;
        for (int j = 0; j < M; ++j) {
            float3 q = p + toSun * (segL * (float(j) + 0.5));
            float hl = max(length(q) - Rg, 0.0);
            odRL += exp(-hl / Hr) * segL;
            odML += exp(-hl / Hm) * segL;
        }
        float3 tau = betaR * (odR + odRL) + betaM * 1.1 * (odM + odML);
        float3 att = exp(-tau);
        sumR += att * hr;
        sumM += att * hm;
    }
    float mu = dot(rd, toSun);
    float phaseR = 3.0 / (16.0 * M_PI_F) * (1.0 + mu * mu);
    const float g = 0.76;
    float phaseM = 3.0 / (8.0 * M_PI_F) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    return (sumR * betaR * phaseR + sumM * betaM * phaseM) * sunIntensity * 9.0;
}

static float3 starField(float3 dir, float time) {
    float3 a = abs(dir);
    float2 uv = a.x > a.y && a.x > a.z ? dir.yz / a.x : (a.y > a.z ? dir.xz / a.y : dir.xy / a.z);
    float face = a.x > a.y && a.x > a.z ? (dir.x > 0 ? 0.0 : 1.0) : (a.y > a.z ? (dir.y > 0 ? 2.0 : 3.0) : (dir.z > 0 ? 4.0 : 5.0));
    float2 cell = floor(uv * 180.0);
    float2 f = fract(uv * 180.0) - 0.5;
    float h = hash12(cell + face * 131.7);
    if (h < 0.965) return float3(0.0);
    float2 off = float2(hash12(cell * 1.7 + 3.1), hash12(cell * 2.3 + 7.7)) - 0.5;
    float d = length(f - off * 0.6);
    float b = smoothstep(0.12, 0.0, d) * (h - 0.965) * 28.0;
    float tw = 0.7 + 0.3 * sin(time * (1.5 + h * 4.0) + h * 40.0);
    float3 tintC = mix(float3(0.75, 0.82, 1.0), float3(1.0, 0.86, 0.7), hash12(cell + 9.0));
    return tintC * b * tw;
}

static float3 skyColor(float3 dir, constant FrameUniforms& f, bool withClouds) {
    float3 toSun = -f.sunDir.xyz;
    float3 sky;
    if (f.sky.x > 0.5) {
        sky = atmosphere(dir, toSun, max(f.sunDir.w, 0.05));
        float below = saturate(-dir.y * 6.0);
        sky = mix(sky, f.ground.rgb * max(f.sunDir.w, 0.05) * 0.25 * saturate(toSun.y + 0.2), below);
    } else {
        float t = saturate(dir.y * 1.4 + 0.05);
        sky = mix(f.skyHorizon.rgb, f.skyTop.rgb, pow(t, 0.6));
        float below = saturate(-dir.y * 4.0);
        sky = mix(sky, mix(f.skyHorizon.rgb, f.ground.rgb, 0.45), below);
    }
    // Night: stars fade in as the sky darkens.
    if (f.sky.z > 0.0 && dir.y > -0.05) {
        float dark = saturate(1.0 - dot(sky, float3(0.3, 0.5, 0.2)) * 3.0);
        sky += starField(dir, f.cameraPos.w) * f.sky.z * dark * saturate(dir.y * 8.0 + 0.4);
    }
    // Sun disc + glow
    float sd = max(dot(dir, toSun), 0.0);
    float size = max(f.sunColor.w, 0.1);
    float disc = smoothstep(cos(0.0095 * size), cos(0.0075 * size), sd);
    float above = smoothstep(-0.03, 0.01, toSun.y);
    sky += f.sunColor.rgb * (disc * 14.0 + pow(sd, 12.0 / size) * 0.18) * above * (f.sky.x > 0.5 ? 0.6 : 1.0);
    // Clouds on a virtual plane
    if (withClouds && f.sky.y > 0.0 && dir.y > 0.0) {
        float2 p = dir.xz / (dir.y + 0.08) * 0.55 + float2(f.cameraPos.w * 0.006, f.cameraPos.w * 0.002);
        float n = fbm(p * 1.6);
        float cover = f.sky.y;
        float density = smoothstep(1.0 - cover, 1.0 - cover + 0.32, n);
        float shade = fbm(p * 1.6 + toSun.xz * 0.12);
        float3 sunLit = f.sunColor.rgb * max(f.sunDir.w, 0.05) * (0.55 + 0.6 * saturate(n - shade + 0.3));
        float3 ambient = mix(f.skyHorizon.rgb, f.skyTop.rgb, 0.5) * 0.7 + float3(0.06);
        float3 cloud = (ambient + sunLit * 0.6) * mix(1.0, 0.65, density);
        float horizonFade = smoothstep(0.0, 0.18, dir.y);
        sky = mix(sky, cloud, density * horizonFade * 0.92);
    }
    return sky;
}

// Equirectangular panorama (skyMode "hdri"); `lod` < 0 samples with automatic mip selection.
static float3 panorama(float3 dir, constant FrameUniforms& f, texture2d<float> pano, float lod) {
    float phi = atan2(dir.x, -dir.z) + f.hdri.x;
    float2 uv = float2(phi * (0.5 / M_PI_F) + 0.5, acos(clamp(dir.y, -1.0, 1.0)) / M_PI_F);
    float3 c = lod < 0.0 ? pano.sample(panoSampler, uv).rgb : pano.sample(panoSampler, uv, level(lod)).rgb;
    return c * f.hdri.y;
}

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

fragment MainOut skyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                             texture2d<float> pano [[texture(0)]]) {
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float4 nearP = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    float3 c = f.sky.x > 1.5 ? panorama(dir, f, pano, 0.0) : skyColor(dir, f, true);
    // Height fog veils the horizon when the fog is dense.
    float fogAmt = saturate(f.fog.w * 60.0) * (1.0 - smoothstep(0.0, 0.35, dir.y));
    c = mix(c, f.fog.rgb, fogAmt);
    MainOut o;
    o.color = float4(c, 1.0);  // linear HDR; tonemapped in the composite pass
    o.ambient = float4(0.0);
    return o;
}

// ---------------------------------------------------------------------------
// Environment cubemap: sky -> cube faces, GGX prefilter per mip, BRDF LUT
// ---------------------------------------------------------------------------

static float3 cubeDir(float face, float2 uv) {
    float2 p = uv * 2.0 - 1.0;
    int fi = int(face + 0.5);
    float3 d;
    if (fi == 0) d = float3(1.0, -p.y, -p.x);
    else if (fi == 1) d = float3(-1.0, -p.y, p.x);
    else if (fi == 2) d = float3(p.x, 1.0, p.y);
    else if (fi == 3) d = float3(p.x, -1.0, -p.y);
    else if (fi == 4) d = float3(p.x, -p.y, 1.0);
    else d = float3(-p.x, -p.y, -1.0);
    return normalize(d);
}

fragment float4 envSkyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                               constant EnvUniforms& e [[buffer(1)]], texture2d<float> pano [[texture(0)]]) {
    float3 dir = cubeDir(e.face.x, uvOf(in));
    if (f.sky.x > 1.5) {  // a photographed panorama already contains its ground
        float3 p = panorama(dir, f, pano, f.hdri.z);
        return float4(min(p, float3(48.0)), 1.0);
    }
    float3 c = skyColor(dir, f, true);
    // Below the horizon, reflect a ground tinted by the ambient color and fog.
    if (dir.y < 0.0) {
        float3 groundC = f.ground.rgb * (max(f.sunDir.w, 0.0) * saturate(-f.sunDir.y) * 0.35 + 0.4);
        c = mix(c, groundC, saturate(-dir.y * 5.0));
    }
    c = mix(c, f.fog.rgb, saturate(f.fog.w * 30.0) * (1.0 - saturate(abs(dir.y) * 3.0)));
    return float4(min(c, float3(48.0)), 1.0);
}

static float2 hammersley(uint i, uint n) {
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
}

static float3 importanceGGX(float2 xi, float3 N, float a) {
    float phi = 2.0 * M_PI_F * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    float3 H = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

fragment float4 envPrefilterFragment(FullscreenOut in [[stage_in]], constant EnvUniforms& e [[buffer(0)]],
                                     texturecube<float> src [[texture(0)]]) {
    float3 N = cubeDir(e.face.x, uvOf(in));
    float rough = e.face.y;
    if (rough < 0.01) return float4(src.sample(cubeSampler, N, level(0.0)).rgb, 1.0);
    float a = rough * rough;
    const uint COUNT = 96;
    float3 sum = 0.0;
    float wsum = 0.0;
    float saTexel = 4.0 * M_PI_F / (6.0 * e.face.z * e.face.z);
    for (uint i = 0; i < COUNT; ++i) {
        float3 H = importanceGGX(hammersley(i, COUNT), N, a);
        float3 L = normalize(2.0 * dot(N, H) * H - N);
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float NdotH = saturate(dot(N, H));
        float pdf = D_GGX(NdotH, a) * 0.25 + 1e-4;
        float saSample = 1.0 / (float(COUNT) * pdf);
        float mip = rough < 0.02 ? 0.0 : max(0.5 * log2(saSample / saTexel) + 1.0, 0.0);
        sum += src.sample(cubeSampler, L, level(mip)).rgb * NdotL;
        wsum += NdotL;
    }
    return float4(sum / max(wsum, 1e-4), 1.0);
}

fragment float2 brdfLutFragment(FullscreenOut in [[stage_in]]) {
    float2 uv = uvOf(in);
    float NdotV = max(uv.x, 1e-3);
    float rough = 1.0 - uv.y;
    float a = rough * rough;
    float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float A = 0.0, B = 0.0;
    const uint COUNT = 256;
    for (uint i = 0; i < COUNT; ++i) {
        float3 H = importanceGGX(hammersley(i, COUNT), float3(0, 0, 1), a);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = saturate(L.z), NdotH = saturate(H.z), VdotH = saturate(dot(V, H));
        if (NdotL > 0.0) {
            float G = V_SmithGGX(NdotV, NdotL, a) * 4.0 * NdotL;
            float Gv = G * VdotH / max(NdotH * NdotV, 1e-4) * NdotV;
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * Gv;
            B += Fc * Gv;
        }
    }
    return float2(A, B) / float(COUNT);
}

// ---------------------------------------------------------------------------
// Lit meshes
// ---------------------------------------------------------------------------

vertex MeshOut meshVertex(uint vid [[vertex_id]],
                          const device Vertex* verts [[buffer(0)]],
                          constant DrawUniforms& d [[buffer(1)]],
                          constant FrameUniforms& f [[buffer(2)]]) {
    Vertex v = verts[vid];
    float4 world = d.model * float4(float3(v.position), 1.0);
    MeshOut o;
    o.position = f.viewProj * world;
    o.worldPos = world.xyz;
    o.normal = (d.normalMatrix * float4(float3(v.normal), 0.0)).xyz;
    o.uv = float2(v.uv);
    o.color = float4(v.color);
    return o;
}

struct Triplanar {
    float2 uvX, uvY, uvZ;
    float3 w;
};

static Triplanar triplanar(float3 p, float3 n, float2 tiling) {
    Triplanar t;
    float3 w = pow(abs(n), float3(4.0));
    t.w = w / (w.x + w.y + w.z);
    t.uvX = p.zy * tiling;
    t.uvY = p.xz * tiling;
    t.uvZ = p.xy * tiling;
    return t;
}

static float4 sampleTri(texture2d<float> tex, Triplanar t) {
    return tex.sample(materialSampler, t.uvX) * t.w.x + tex.sample(materialSampler, t.uvY) * t.w.y +
           tex.sample(materialSampler, t.uvZ) * t.w.z;
}

// Normal mapping without vertex tangents: cotangent frame from screen-space derivatives.
static float3 perturbNormal(float3 N, float3 p, float2 uv, float3 mapN) {
    float3 dp1 = dfdx(p), dp2 = dfdy(p);
    float2 duv1 = dfdx(uv), duv2 = dfdy(uv);
    float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    float3x3 TBN = float3x3(T * invmax, B * invmax, N);
    return normalize(TBN * mapN);
}

static float3 triplanarNormal(texture2d<float> tex, Triplanar t, float3 N, float strength) {
    // Whiteout blend of three tangent-space normals into world space.
    float3 tx = tex.sample(materialSampler, t.uvX).xyz * 2.0 - 1.0;
    float3 ty = tex.sample(materialSampler, t.uvY).xyz * 2.0 - 1.0;
    float3 tz = tex.sample(materialSampler, t.uvZ).xyz * 2.0 - 1.0;
    tx.xy *= strength;
    ty.xy *= strength;
    tz.xy *= strength;
    float3 nx = float3(tx.xy + N.zy, abs(tx.z) * N.x);
    float3 ny = float3(ty.xy + N.xz, abs(ty.z) * N.y);
    float3 nz = float3(tz.xy + N.xy, abs(tz.z) * N.z);
    return normalize(nx.zyx * t.w.x + ny.xzy * t.w.y + nz.xyz * t.w.z);
}

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

fragment MainOut meshFragment(MeshOut in [[stage_in]],
                              bool frontFacing [[front_facing]],
                              constant DrawUniforms& d [[buffer(0)]],
                              constant FrameUniforms& f [[buffer(1)]],
                              constant GPULight* lights [[buffer(2)]],
                              texture2d<float> albedoTex [[texture(0)]],
                              depth2d<float> shadowAtlas [[texture(1)]],
                              texture2d<float> normalTex [[texture(2)]],
                              texture2d<float> ormTex [[texture(3)]],
                              texture2d<float> emissiveTex [[texture(4)]],
                              texturecube<float> envTex [[texture(5)]],
                              texture2d<float> brdfLut [[texture(6)]]) {
    float3 Ngeo = normalize(in.normal) * (frontFacing ? 1.0 : -1.0);
    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    if (f.cameraForward.w > 0.5) V = -f.cameraForward.xyz;
    bool tri = d.material2.w > 0.5;
    float2 uv = in.uv * d.material2.xy;
    Triplanar tp = triplanar(in.worldPos, Ngeo, d.material2.xy);

    SurfaceData s;
    s.albedo = d.color.rgb * in.color.rgb;
    s.alpha = d.color.a * in.color.a;
    if (d.maps.x > 0.5) {
        float4 t = tri ? sampleTri(albedoTex, tp) : albedoTex.sample(materialSampler, uv);
        s.albedo *= t.rgb;
        s.alpha *= t.a;
        if (d.material4.x > 0.0) {
            // Alpha test, sharpened to a ~1 px ramp so alpha-to-coverage antialiases the edge.
            s.alpha = saturate((s.alpha - d.material4.x) / max(fwidth(s.alpha), 1e-4) + 0.5);
            if (s.alpha <= 0.0) discard_fragment();
        } else if (s.alpha < 0.02) {
            discard_fragment();
        }
    }
    float3 emissive = d.emissive.rgb * d.emissive.w;
    if (d.maps.w > 0.5) emissive *= (tri ? sampleTri(emissiveTex, tp) : emissiveTex.sample(materialSampler, uv)).rgb;

    MainOut o;
    int shading = int(d.material.w + 0.5);
    if (shading == 2) {  // unlit: flat color, still emissive
        o.color = float4(s.albedo + emissive, s.alpha);
        o.ambient = float4(0.0, 0.0, 0.0, s.alpha);
        return o;
    }

    s.metallic = d.material.x;
    s.roughness = d.material.y;
    s.ao = 1.0;
    if (d.maps.z > 0.5) {
        float3 orm = (tri ? sampleTri(ormTex, tp) : ormTex.sample(materialSampler, uv)).rgb;
        s.ao = mix(1.0, orm.r, saturate(d.maps.z - 1.0));
        s.roughness *= orm.g;
        s.metallic *= orm.b;
    }
    s.N = Ngeo;
    if (d.maps.y > 0.5) {
        if (tri) {
            s.N = triplanarNormal(normalTex, tp, Ngeo, d.material2.z);
        } else {
            float3 mapN = normalTex.sample(materialSampler, uv).xyz * 2.0 - 1.0;
            mapN.xy *= d.material2.z;
            s.N = perturbNormal(Ngeo, in.worldPos, uv, normalize(mapN));
        }
    }
    if (shading == 3) {
        // Water: a sum of wind-driven directional waves (analytic slopes) plus fine ripples,
        // faded with distance so the far sea stays calm instead of aliasing.
        float t = f.cameraPos.w;
        float2 p = in.worldPos.xz;
        float dist = length(in.worldPos - f.cameraPos.xyz);
        float2 slope = float2(0.0);
        float amp = 0.09, freq = 0.32;
        for (int i = 0; i < 9; ++i) {
            float a = 0.6 + float(i) * 2.399;  // golden-angle spread of wave directions
            float2 dir = float2(cos(a), sin(a));
            float speed = sqrt(9.81 * freq);
            float ph = dot(dir, p) * freq + t * speed + float(i) * 1.7;
            float fade = saturate(1.0 - dist * freq / 900.0);
            slope += dir * (amp * freq * cos(ph) * fade);
            amp *= 0.72;
            freq *= 1.42;
        }
        float rip = valueNoise(p * 3.1 + float2(t * 0.6, t * 0.35)) - valueNoise(p * 3.1 + float2(0.37, 0.21) + float2(t * 0.6, t * 0.35));
        slope += float2(rip, -rip) * 0.05 * saturate(1.0 - dist / 80.0);
        s.N = normalize(float3(-slope.x, 1.0, -slope.y));
        if (!frontFacing) s.N = -s.N;
        // Deeper looking from above, brighter turquoise at grazing angles + sun-lit scattering.
        float facing = saturate(dot(s.N, V));
        s.albedo = mix(d.color.rgb * 1.6, d.color.rgb * 0.45, facing);
        s.metallic = 0.0;
    }
    // Specular anti-aliasing (Kaplanyan & Hoffman): widen roughness where normals vary per pixel.
    float3 dn = fwidth(s.N);
    float variance = 0.25 * dot(dn, dn);
    float alpha = s.roughness * s.roughness;
    alpha = sqrt(alpha * alpha + min(2.0 * variance, 0.18));
    s.roughness = clamp(sqrt(alpha), 0.045, 1.0);
    s.clearcoat = d.material3.x;
    s.subsurface = d.material3.y;
    bool toon = shading == 1;

    // Sun
    float3 L = -f.sunDir.xyz;
    float sunVisible = L.y > -0.08 ? shadowFactor(in.worldPos, Ngeo, in.position.xy, f, shadowAtlas) : 0.0;
    float3 sunRad = f.sunColor.rgb * f.sunDir.w * sunVisible;
    float3 color = toon ? toonLight(s, V, L, sunRad) : directLight(s, V, L, sunRad);

    // Punctual lights
    int count = int(f.params.y);
    for (int i = 0; i < count; ++i) {
        GPULight l = lights[i];
        float3 Ll;
        float atten = 1.0;
        if (l.kind.x < 0.5) {
            Ll = -l.directionCone.xyz;
        } else {
            float3 toL = l.positionRange.xyz - in.worldPos;
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
        float3 rad = l.colorIntensity.rgb * l.colorIntensity.w * atten;
        color += toon ? toonLight(s, V, Ll, rad) : directLight(s, V, Ll, rad);
    }

    // Image-based lighting (sky cubemap). `ambient` scales how much sky light reaches the
    // scene (interiors, caves, night); `reflections` scales the specular part.
    float ambientK = f.ground.w * 2.0;
    float maxMip = f.extra.z;
    float NdotV = max(dot(s.N, V), 1e-4);
    float3 irradiance = envTex.sample(cubeSampler, s.N, level(maxMip)).rgb;
    float3 indirect;
    if (toon) {
        float up = s.N.y * 0.5 + 0.5;
        float3 hemi = mix(f.ground.rgb, mix(f.skyHorizon.rgb, f.skyTop.rgb, 0.6), up);
        indirect = (hemi * 0.6 + irradiance * 0.4) * s.albedo * ambientK * 0.5;
    } else {
        float3 F0 = mix(float3(0.04), s.albedo, s.metallic);
        float3 R = reflect(-V, s.N);
        float3 prefiltered = envTex.sample(cubeSampler, R, level(s.roughness * maxMip)).rgb;
        float2 ab = brdfLut.sample(linearClamp, float2(NdotV, 1.0 - s.roughness)).rg;
        float3 Fr = F0 * ab.x + ab.y;
        float3 kd = (1.0 - Fr) * (1.0 - s.metallic);
        float3 diffuse = irradiance * s.albedo * kd;
        float specOcclusion = saturate(pow(NdotV + s.ao, exp2(-16.0 * s.roughness - 1.0)) - 1.0 + s.ao);
        float3 specular = prefiltered * Fr * f.sky.w * specOcclusion;
        if (s.clearcoat > 0.0) {
            float Fc = 0.04 + 0.96 * pow(1.0 - NdotV, 5.0);
            float3 ccEnv = envTex.sample(cubeSampler, reflect(-V, Ngeo), level(0.06 * maxMip)).rgb;
            specular = specular * (1.0 - Fc * s.clearcoat) + ccEnv * Fc * s.clearcoat * f.sky.w;
            diffuse *= 1.0 - Fc * s.clearcoat;
        }
        indirect = (diffuse * s.ao + specular) * ambientK * 0.5;
    }
    // Rim light (stylized sheen along silhouettes, tinted by the sky)
    float rim = d.material3.z;
    if (rim > 0.0) {
        float r = pow(1.0 - NdotV, 3.0) * rim;
        if (toon) r = smoothstep(0.35, 0.4, r);
        indirect += r * (mix(f.skyHorizon.rgb, f.sunColor.rgb, 0.5) + s.albedo * 0.3) * 0.6;
    }
    color += indirect + emissive;

    // Fog (with a warm in-scatter toward the sun)
    float fogAmt = fogFactor(f, in.worldPos);
    float3 fogC = f.fog.rgb + f.sunColor.rgb * f.sunDir.w * pow(saturate(dot(-V, L)), 8.0) * 0.25;
    color = mix(color, fogC, fogAmt);
    indirect *= 1.0 - fogAmt;

    o.color = float4(color, s.alpha);  // linear HDR
    o.ambient = float4(indirect, s.alpha);
    return o;
}

// ---------------------------------------------------------------------------
// Outlines: inverted hull extruded in clip space (constant pixel width). Used for the
// selection highlight and for toon outlines (`outline` > 0).
// ---------------------------------------------------------------------------

vertex float4 outlineVertex(uint vid [[vertex_id]],
                            const device Vertex* verts [[buffer(0)]],
                            constant DrawUniforms& d [[buffer(1)]],
                            constant FrameUniforms& f [[buffer(2)]]) {
    Vertex v = verts[vid];
    float4 clip = f.viewProj * (d.model * float4(float3(v.position), 1.0));
    float3 worldN = normalize((d.normalMatrix * float4(float3(v.normal), 0.0)).xyz);
    float4 clipN = f.viewProj * float4(worldN, 0.0);
    float2 dir = length(clipN.xy) > 1e-5 ? normalize(clipN.xy) : float2(0.0);
    float widthPx = d.material3.w;
    clip.xy += dir * widthPx * 2.0 * f.viewport.zw * clip.w;
    return clip;
}

fragment MainOut outlineFragment(constant DrawUniforms& d [[buffer(0)]]) {
    MainOut o;
    o.color = d.outlineColor;
    o.ambient = float4(0.0, 0.0, 0.0, 1.0);
    return o;
}

// ---------------------------------------------------------------------------
// Overlays (gizmos): lightly shaded, drawn on top of everything
// ---------------------------------------------------------------------------

fragment float4 overlayFragment(MeshOut in [[stage_in]], constant DrawUniforms& d [[buffer(0)]],
                                constant FrameUniforms& f [[buffer(1)]]) {
    float3 N = normalize(in.normal);
    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    float shade = 0.72 + 0.28 * abs(dot(N, V));
    return float4(d.color.rgb * shade, d.color.a);
}

// ---------------------------------------------------------------------------
// Shadow pass (depth only)
// ---------------------------------------------------------------------------

vertex float4 shadowVertex(uint vid [[vertex_id]],
                           const device Vertex* verts [[buffer(0)]],
                           constant DrawUniforms& d [[buffer(1)]],
                           constant float4x4& lightViewProj [[buffer(2)]]) {
    return lightViewProj * (d.model * float4(float3(verts[vid].position), 1.0));
}

struct ShadowAlphaOut {
    float4 position [[position]];
    float2 uv;
};

vertex ShadowAlphaOut shadowAlphaVertex(uint vid [[vertex_id]],
                                        const device Vertex* verts [[buffer(0)]],
                                        constant DrawUniforms& d [[buffer(1)]],
                                        constant float4x4& lightViewProj [[buffer(2)]]) {
    ShadowAlphaOut o;
    o.position = lightViewProj * (d.model * float4(float3(verts[vid].position), 1.0));
    o.uv = float2(verts[vid].uv) * d.material2.xy;
    return o;
}

// Alpha-tested casters (leaves, sails, fences) only shadow where they are solid.
fragment void shadowAlphaFragment(ShadowAlphaOut in [[stage_in]], constant DrawUniforms& d [[buffer(0)]],
                                  texture2d<float> albedoTex [[texture(0)]]) {
    if (albedoTex.sample(materialSampler, in.uv).a * d.color.a < d.material4.x) discard_fragment();
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

// ---------------------------------------------------------------------------
// Water: FFT ocean cascades (displacement in the vertex stage, slopes + Jacobian per
// pixel), refraction with depth absorption and in-scattering, screen-space reflections
// over the sky cubemap, GGX sun glints, crest subsurface scattering, whitecaps and shore foam.
// ---------------------------------------------------------------------------

struct WaterUniforms {
    float4 levelSize;  // x = level, y = size (0 endless), zw = center
    float4 deep;
    float4 shallow;
    float4 params;     // x = clarity, y = foam, z = reflections, w = refraction
    float4 params2;    // x = roughness, y = 1/N, z = endless
    float4 patch;      // xyz = cascade tile sizes
    float4 origin;     // xz = grid origin
};

struct WaterOut {
    float4 position [[position]];
    float3 worldPos;
    float2 baseXZ;
    float height;
};

vertex WaterOut waterVertex(uint vid [[vertex_id]],
                            const device float2* grid [[buffer(0)]],
                            constant WaterUniforms& w [[buffer(1)]],
                            constant FrameUniforms& f [[buffer(2)]],
                            texture2d<float> d0 [[texture(0)]],
                            texture2d<float> d1 [[texture(1)]],
                            texture2d<float> d2 [[texture(2)]]) {
    float2 g = grid[vid];
    bool endless = w.params2.z > 0.5;
    float2 xz = endless ? g + w.origin.xz : w.levelSize.zw + g * w.levelSize.y;
    float dist = length(xz - f.cameraPos.xz) + abs(f.cameraPos.y - w.levelSize.x);
    float edge = 1.0;
    if (!endless) {
        float2 d = abs(xz - w.levelSize.zw);
        edge = saturate((w.levelSize.y * 0.5 - max(d.x, d.y)) / 1.5);
    }
    float ht = 0.5 * w.params2.y;
    float texel0 = w.patch.x * w.params2.y, texel1 = w.patch.y * w.params2.y, texel2 = w.patch.z * w.params2.y;
    float3 disp = d0.sample(oceanSampler, xz / w.patch.x + ht, level(clamp(log2(max(dist, 1.0) / (texel0 * 60.0)), 0.0, 6.0))).xyz;
    disp += d1.sample(oceanSampler, xz / w.patch.y + ht, level(clamp(log2(max(dist, 1.0) / (texel1 * 60.0)), 0.0, 6.0))).xyz *
            saturate(1.5 - dist / (w.patch.y * 12.0));
    disp += d2.sample(oceanSampler, xz / w.patch.z + ht, level(clamp(log2(max(dist, 1.0) / (texel2 * 60.0)), 0.0, 6.0))).xyz *
            saturate(1.5 - dist / (w.patch.z * 12.0));
    disp *= edge;
    float3 world = float3(xz.x + disp.x, w.levelSize.x + disp.y, xz.y + disp.z);
    WaterOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.position.z = min(o.position.z, o.position.w * 0.999999);  // the endless sea reaches the horizon
    o.worldPos = world;
    o.baseXZ = xz;
    o.height = disp.y;
    return o;
}

// Animated caustic pattern (iterated trigonometric warping, after the well-known "tileable water caustic").
static float waterCaustic(float2 p, float t) {
    float2 i = p;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 4; ++n) {
        float t2 = t * (1.0 - 3.5 / float(n + 1));
        i = p + float2(cos(t2 - i.x) + sin(t2 + i.y), sin(t2 - i.y) + cos(t2 + i.x));
        c += 1.0 / length(float2(p.x / (sin(i.x + t2) / inten), p.y / (cos(i.y + t2) / inten)));
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    return saturate(pow(abs(c), 8.0));
}

static float4 traceSSR(constant FrameUniforms& f, float3 p, float3 R, texture2d<float> sceneTex, depth2d<float> depthTex) {
    float stepLen = 0.35;
    float3 pos = p + R * 0.15;
    for (int i = 0; i < 40; ++i) {
        pos += R * stepLen;
        stepLen *= 1.14;
        float4 c = f.viewProj * float4(pos, 1.0);
        if (c.w <= 1e-4) break;
        float3 ndc = c.xyz / c.w;
        float2 suv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        float sd = depthTex.sample(pointClamp, suv);
        if (sd >= 0.99999 || ndc.z <= sd) continue;
        float3 sp = reconstructWorld(f, suv, sd);
        float behind = distance(f.cameraPos.xyz, pos) - distance(f.cameraPos.xyz, sp);
        if (behind > stepLen * 1.6 + 0.4) break;  // passed behind a thick object: no reliable hit
        float2 e = smoothstep(0.0, 0.07, suv) * smoothstep(1.0, 0.93, suv);
        float conf = e.x * e.y * (1.0 - float(i) / 40.0);
        return float4(sceneTex.sample(linearClamp, suv).rgb, conf);
    }
    return float4(0.0);
}

fragment MainOut waterFragment(WaterOut in [[stage_in]],
                               constant WaterUniforms& w [[buffer(0)]],
                               constant FrameUniforms& f [[buffer(1)]],
                               constant GPULight* lights [[buffer(2)]],
                               depth2d<float> shadowAtlas [[texture(1)]],
                               texturecube<float> envTex [[texture(5)]],
                               texture2d<float> sceneTex [[texture(6)]],
                               depth2d<float> sceneDepth [[texture(7)]],
                               texture2d<float> s0 [[texture(8)]],
                               texture2d<float> s1 [[texture(9)]],
                               texture2d<float> s2 [[texture(10)]],
                               texture2d<float> j0 [[texture(11)]],
                               texture2d<float> j1 [[texture(12)]],
                               texture2d<float> j2 [[texture(13)]],
                               texture2d<float> pano [[texture(14)]]) {
    bool endless = w.params2.z > 0.5;
    float2 xz = in.baseXZ;
    if (!endless) {
        // Bounded water (lakes, ponds, puddles) gets an organic, ragged outline.
        float2 rel = (xz - w.levelSize.zw) / (w.levelSize.y * 0.5);
        float r = length(rel * rel * rel * rel);  // squircle
        float ragged = (fbm(xz * (6.0 / max(w.levelSize.y, 0.5)) + w.levelSize.zw) - 0.5) * 0.5;
        if (pow(r, 0.25) > 0.92 + ragged) discard_fragment();
    }
    float ht = 0.5 * w.params2.y;
    float2 u0 = xz / w.patch.x + ht, u1 = xz / w.patch.y + ht, u2 = xz / w.patch.z + ht;
    float4 sl0 = s0.sample(oceanSampler, u0), sl1 = s1.sample(oceanSampler, u1), sl2 = s2.sample(oceanSampler, u2);
    float2 sl = sl0.xy + sl1.xy + sl2.xy;
    float persistentFoam = saturate(sl0.z + sl1.z * 0.8 + sl2.z * 0.5);
    float3 N = normalize(float3(-sl.x, 1.0, -sl.y));
    float J = j0.sample(oceanSampler, u0).w + j1.sample(oceanSampler, u1).w + j2.sample(oceanSampler, u2).w - 2.0;

    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    if (dot(N, V) < 0.02) N = normalize(N + V * (0.02 - dot(N, V)));
    float NdotV = max(dot(N, V), 1e-3);
    float dist = length(f.cameraPos.xyz - in.worldPos);
    float2 uv = in.position.xy * f.viewport.zw;

    // What lies behind the surface (opaque scene, before the water was drawn).
    float dS = sceneDepth.sample(pointClamp, uv);
    bool skyBehind = dS >= 0.99999;
    float3 sceneP = reconstructWorld(f, uv, dS);
    float2 off = N.xz * (0.03 * w.params.w) / max(1.0, dist * 0.05);
    float2 ruv = clamp(uv + off, float2(0.001), float2(0.999));
    float dR = sceneDepth.sample(pointClamp, ruv);
    float3 rP = reconstructWorld(f, ruv, dR);
    float thick = (dR >= 0.99999) ? 1e4 : dot(rP - in.worldPos, -V);
    if (thick < 0.0) {  // the offset sample is in front of the water: don't refract
        ruv = uv;
        thick = skyBehind ? 1e4 : max(dot(sceneP - in.worldPos, -V), 0.0);
        rP = sceneP;
    }
    float3 refr = sceneTex.sample(linearClamp, ruv).rgb;

    float3 L = -f.sunDir.xyz;
    float3 sunRad = f.sunColor.rgb * f.sunDir.w;
    float ambientK = f.ground.w * 2.0;
    float3 skyIrr = envTex.sample(cubeSampler, float3(0.0, 1.0, 0.0), level(f.extra.z)).rgb;
    float3 absorb = (1.0 - w.shallow.rgb) * (2.2 / max(w.params.x, 0.1)) + 0.012;
    // Light travels down to the submerged surface and back up to the eye: both are absorbed.
    float down = (dR >= 0.99999) ? 0.0 : max(w.levelSize.x - rP.y, 0.0);
    // Caustics: the wavy surface focuses sunlight into moving bright lines on what lies below.
    if (down > 0.02) {
        float3 Lc = -f.sunDir.xyz;
        float2 cp = rP.xz * 0.9 + Lc.xz * down * 0.6;
        float c = waterCaustic(cp, f.cameraPos.w * 0.6) * 0.65 + waterCaustic(cp * 1.7 + 3.1, f.cameraPos.w * 0.75) * 0.35;
        refr *= 1.0 + c * 2.2 * saturate(Lc.y * 2.0) * saturate(down * 3.0) * exp(-down * 0.35) * min(f.sunDir.w, 3.0) * 0.4;
    }
    float3 T = exp(-absorb * min(thick + down, 500.0));
    float3 inscatter = w.deep.rgb * (skyIrr * ambientK * 0.9 + sunRad * saturate(L.y) * 0.14);
    float3 under = refr * T + inscatter * (1.0 - T);

    float rough = mix(w.params2.x, 0.14, saturate(dist / 900.0));
    float3 R = reflect(-V, N);
    R.y = max(abs(R.y), 0.035);  // reflect sky, never the band below the horizon
    R = normalize(R);
    float3 envR = f.sky.x > 1.5 ? panorama(R, f, pano, max(f.hdri.z - 1.5, 0.0) + rough * 9.0)
                                : envTex.sample(cubeSampler, R, level(rough * f.extra.z * 1.5)).rgb;
    float4 ssr = traceSSR(f, in.worldPos, R, sceneTex, sceneDepth);
    float3 refl = mix(envR, ssr.rgb, ssr.a) * f.sky.w;
    float F = saturate((0.02 + 0.98 * pow(1.0 - NdotV, 5.0)) * w.params.z);

    float shadow = L.y > -0.08 ? shadowFactor(in.worldPos, N, in.position.xy, f, shadowAtlas) : 0.0;
    float3 H = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float a2 = max(rough * rough, 0.0006);
    float spec = D_GGX(saturate(dot(N, H)), a2) * V_SmithGGX(NdotV, NdotL, a2) * NdotL;
    float3 color = mix(under, refl, F) + sunRad * spec * F_Schlick(float3(0.02), saturate(dot(V, H))) * shadow;

    // Light through the thin crests of waves rolling toward the viewer.
    float crest = saturate(in.height * 0.45 + 0.2);
    color += w.shallow.rgb * sunRad * pow(saturate(dot(V, -L) * 0.5 + 0.5), 4.0) * crest * 0.16 * shadow * (1.0 - F);

    // Lamps and fires reflect on the water.
    int count = int(f.params.y);
    for (int i = 0; i < count; ++i) {
        float3 Ll;
        float3 rad = pointLightAt(lights[i], in.worldPos, N, Ll);
        float3 Hl = normalize(Ll + V);
        float nl = saturate(dot(N, Ll));
        float sp = D_GGX(saturate(dot(N, Hl)), max(a2, 0.004)) * V_SmithGGX(NdotV, nl, max(a2, 0.004)) * nl;
        color += rad * (sp * 0.6 + nl * 0.02);
    }

    // Foam: whitecaps where the surface folds (Jacobian < 1) and surf where the water is shallow.
    float time = f.cameraPos.w;
    // Surf hugs the waterline: where the view crosses only a little water before hitting ground.
    float through = skyBehind ? 1e4 : max(dot(sceneP - in.worldPos, -V), 0.0);
    float shoreBand = pow(saturate(1.0 - through / 0.45), 1.5);
    float surf = shoreBand * smoothstep(0.45, 0.75, fbm(xz * 3.0 + float2(time * 0.25, -time * 0.15)) + shoreBand * 0.5);
    // Whitecaps: only where crests actually fold (Jacobian well below 1), streaked by noise.
    // Whitecaps: born where crests fold, then linger and break up into streaky lace.
    float lace = fbm(xz * float2(2.6, 1.3) + float2(time * 0.08, 0.0)) * 0.6 + fbm(xz * 6.5 - float2(0.0, time * 0.1)) * 0.4;
    float caps = smoothstep(0.25, 0.8, persistentFoam + (lace - 0.5) * 0.9) * smoothstep(0.1, 0.4, persistentFoam);
    float foam = saturate((surf + caps) * w.params.y);
    float3 foamColor = (sunRad * (NdotL * 0.8 + 0.2) * shadow + skyIrr * ambientK) * 0.8;
    color = mix(color, foamColor, foam * 0.9);

    float fogAmt = fogFactor(f, in.worldPos);
    float3 fogC = f.fog.rgb + f.sunColor.rgb * f.sunDir.w * pow(saturate(dot(-V, L)), 8.0) * 0.25;
    color = mix(color, fogC, fogAmt);
    MainOut o;
    o.color = float4(color, 1.0);
    o.ambient = float4(0.0, 0.0, 0.0, 1.0);
    return o;
}

// ---------------------------------------------------------------------------
// Particles: one back-to-front stream of camera-facing (or velocity-stretched) quads,
// premultiplied alpha (emissive looks output alpha ~0 = additive). Soft particles fade
// against the depth buffer. Smoke, mist, snow and rain are lit by the sun (shadowed),
// the sky and every point light; flames are procedural, animated fire tongues.
// ---------------------------------------------------------------------------

struct ParticleGpu {
    packed_float3 position;
    float size;
    float4 color;
    packed_float3 velocity;
    float rotation;
    float age;
    float look;
    float seed;
    float softness;
};

struct ParticleOut {
    float4 position [[position]];
    float3 worldPos;
    float2 uv;
    float4 color;
    float age;
    float seed;
    float softness;
    float size;
    float look [[flat]];
};

vertex ParticleOut particleVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device ParticleGpu* ps [[buffer(0)]],
                                  constant FrameUniforms& f [[buffer(1)]]) {
    ParticleGpu p = ps[iid];
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(1, 1), float2(-1, 1)};
    float2 c = corners[vid];
    float3 center = float3(p.position);
    float3 toCam = normalize(f.cameraPos.xyz - center);
    float3 fwd = -toCam;
    float3 worldUp = abs(fwd.y) > 0.99 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(fwd, worldUp));
    float3 up = cross(right, fwd);
    float3 v = float3(p.velocity);
    float3 vs = v - fwd * dot(v, fwd);
    float len = length(vs);
    float r = p.size * 0.5;
    int look = int(p.look + 0.5);
    float3 world;
    if (len > r * 0.25 && (look == 0 || look == 3 || look == 4 || look == 7)) {
        float3 along = vs / len;
        float3 across = normalize(cross(along, toCam));
        float3 mid = center - v * 0.5;
        world = mid + along * (c.y * (len * 0.5 + r)) + across * (c.x * r);
    } else {
        float cr = cos(p.rotation), sr = sin(p.rotation);
        float2 rc = look == 1 ? c : float2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);  // flames stay upright
        world = center + (right * rc.x + up * rc.y) * r;
    }
    ParticleOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.uv = c;
    o.color = p.color;
    o.age = p.age;
    o.seed = p.seed;
    o.softness = p.softness;
    o.size = p.size;
    o.look = p.look;
    return o;
}

static float3 particleLighting(constant FrameUniforms& f, constant GPULight* lights, depth2d<float> atlas,
                               texturecube<float> env, float3 pos, float3 n, float2 pixel, float phaseAmount) {
    float3 L = -f.sunDir.xyz;
    float sh = L.y > -0.08 ? shadowFactor(pos, float3(0.0, 1.0, 0.0), pixel, f, atlas) : 0.0;
    float3 V = normalize(f.cameraPos.xyz - pos);
    float wrap = saturate(dot(n, L) * 0.5 + 0.5);
    float g = 0.5;
    float cosT = dot(-V, L);
    float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.0796;  // Henyey-Greenstein
    float3 lit = f.sunColor.rgb * f.sunDir.w * sh * (wrap * 0.32 + hg * phaseAmount * 2.0);
    lit += env.sample(cubeSampler, n, level(f.extra.z)).rgb * f.ground.w * 1.1;
    int count = int(f.params.y);
    for (int i = 0; i < count; ++i) {
        float3 Ll;
        float3 rad = pointLightAt(lights[i], pos, n, Ll);
        lit += rad * (saturate(dot(n, Ll)) * 0.6 + 0.4);
    }
    return lit;
}

fragment MainOut particleFragment(ParticleOut in [[stage_in]],
                                  constant FrameUniforms& f [[buffer(1)]],
                                  constant GPULight* lights [[buffer(2)]],
                                  depth2d<float> shadowAtlas [[texture(1)]],
                                  texturecube<float> envTex [[texture(5)]],
                                  depth2d<float> sceneDepth [[texture(7)]]) {
    float2 suv = in.position.xy * f.viewport.zw;
    float sd = sceneDepth.sample(pointClamp, suv);
    float camDist = distance(f.cameraPos.xyz, in.worldPos);
    float soft = 1.0;
    if (sd < 0.99999) {
        float gap = distance(f.cameraPos.xyz, reconstructWorld(f, suv, sd)) - camDist;
        if (gap <= 0.0) discard_fragment();
        soft = saturate(gap / max(in.softness, 0.005));
    }
    soft *= saturate((camDist - 0.1) / max(in.size, 0.05));  // fade out instead of filling the lens

    int look = int(in.look + 0.5);
    float2 p = in.uv;
    float r2 = dot(p, p);
    float time = f.cameraPos.w;
    float3 rgb = float3(0.0);
    float a = 0.0;
    float3 toCam = normalize(f.cameraPos.xyz - in.worldPos);
    float3 right = normalize(cross(-toCam, abs(toCam.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0)));
    float3 up = cross(right, -toCam);

    if (look == 1) {  // flame tongue: tapering, swaying, noise-eroded; heat ramp from base to tip
        float y01 = p.y * 0.5 + 0.5;
        float flow = time * 2.6 + in.seed * 11.0;
        float n1 = fbm(float2(p.x * 1.6 + in.seed * 7.0, p.y * 0.9 - flow));
        float n2 = fbm(float2(p.x * 3.4 - in.seed * 3.0, p.y * 1.8 - flow * 1.7));
        float sway = (n1 - 0.5) * 0.7 * y01 * y01;
        float width = 0.62 * pow(saturate(1.0 - y01), 0.75) + 0.04;
        float body = saturate(1.0 - abs(p.x + sway) / width);
        float erode = saturate((n2 * 1.25 + (1.0 - y01) * 0.95 - 0.78) * 3.2);
        float d = body * erode * smoothstep(0.0, 0.18, y01) * in.color.a * (1.0 - smoothstep(0.55, 1.0, in.age));
        float heat = saturate(d * (1.15 - y01 * 0.75));
        // ramp: deep red tips -> particle color -> yellow-white core
        float3 tip = in.color.rgb * float3(0.75, 0.32, 0.12);
        float3 col = mix(tip, in.color.rgb, smoothstep(0.08, 0.5, heat));
        col = mix(col, in.color.rgb * float3(1.0, 1.35, 1.9) + dot(in.color.rgb, float3(0.1)), smoothstep(0.55, 1.0, heat) * 0.7);
        a = saturate(d * 1.3) * 0.6;
        rgb = col * a;
    } else if (look == 2 || look == 6) {  // smoke / mist: lit billowing volume
        bool mist = look == 6;
        float2 q = p * (mist ? 0.7 : 1.25) + float2(in.seed * 17.0, in.seed * 5.0 - in.age * 0.7);
        float n = fbm(q * 1.6) * 0.65 + fbm(q * 3.7 + float2(time * 0.05, -time * 0.03)) * 0.35;
        float sphere = pow(saturate(1.0 - r2), mist ? 2.2 : 1.4);
        float density = saturate(sphere * (0.35 + n * 1.25) - 0.12);
        a = density * in.color.a * smoothstep(0.0, mist ? 0.25 : 0.1, in.age);
        float3 n3 = normalize(right * p.x + up * p.y + toCam * sqrt(max(0.0, 1.0 - r2)) + float3(0, (n - 0.5) * 0.6, 0));
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, n3, in.position.xy, mist ? 0.8 : 0.5);
        rgb = in.color.rgb * lit * (0.75 + 0.25 * n) * a;
    } else if (look == 4 || look == 7) {  // rain streak / splash droplet: lit by lamps and sky
        float across = exp(-p.x * p.x * (look == 4 ? 4.0 : 2.0));
        float along = look == 4 ? smoothstep(1.0, 0.55, abs(p.y)) : saturate(1.0 - r2);
        a = in.color.a * across * along * (look == 7 ? 0.8 : 1.0);
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.6);
        rgb = in.color.rgb * (lit * 0.55 + 0.02) * a;
    } else if (look == 5) {  // snow flake
        a = smoothstep(1.0, 0.25, sqrt(r2)) * in.color.a;
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.3);
        rgb = in.color.rgb * lit * 0.8 * a;
    } else if (look == 3) {  // spark streak
        float g = exp(-p.x * p.x * 5.0) * (1.0 - smoothstep(0.6, 1.0, abs(p.y)));
        rgb = in.color.rgb * g * in.color.a;
        a = 0.0;
    } else {  // glow
        float g = exp(-r2 * 5.0) - 0.0067;
        rgb = in.color.rgb * max(g, 0.0) * in.color.a;
        a = 0.0;
    }
    float fogAmt = fogFactor(f, in.worldPos);
    if (look == 2 || look == 4 || look == 5 || look == 6 || look == 7) {
        rgb = mix(rgb, f.fog.rgb * a, fogAmt);
    } else {
        rgb *= 1.0 - fogAmt;
    }
    MainOut o;
    o.color = float4(rgb, a) * soft;
    o.ambient = float4(0.0, 0.0, 0.0, a * soft);
    return o;
}

// ---------------------------------------------------------------------------
// Volumetric fluids: a 3D Eulerian gas solver (compute) and a ray marcher (fragment).
//   scalars: r = smoke (soot / vapour), g = heat, b = fuel
//   velocity: cells per second
// Step: MacCormack advection -> combustion & sources -> vorticity confinement + buoyancy
//       + wind -> divergence -> Jacobi pressure -> projection (incompressible).
// ---------------------------------------------------------------------------

struct FluidParams {
    float4 dims;     // nx, ny, nz, cell size (m)
    float4 step;     // dt, time, seed, burst multiplier
    float4 source;   // xyz source center (cells), radius (cells)
    float4 feed;     // fuel/s, heat, smoke, speed (cells/s)
    float4 physics;  // buoyancy, vorticity, turbulence, burn rate
    float4 decay;    // cooling, smoke fade, emitting, 0
    float4 wind;     // xyz wind (cells/s, box space)
};

constexpr sampler volumeSampler(coord::normalized, filter::linear, address::clamp_to_edge);

static float3 cellUVW(float3 cellPos, constant FluidParams& p) { return (cellPos + 0.5) / p.dims.xyz; }

static float hash31(float3 p) {
    p = fract(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

static float noise3(float3 p) {
    float3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i), n100 = hash31(i + float3(1, 0, 0)), n010 = hash31(i + float3(0, 1, 0)), n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1)), n101 = hash31(i + float3(1, 0, 1)), n011 = hash31(i + float3(0, 1, 1)), n111 = hash31(i + float3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

static bool inside(uint3 g, constant FluidParams& p) { return g.x < uint(p.dims.x) && g.y < uint(p.dims.y) && g.z < uint(p.dims.z); }

static float4 readClamped(texture3d<float, access::read> t, int3 q, int3 m) { return t.read(uint3(clamp(q, int3(0), m))); }

// Pressure outside the box: open sides/top (0, gas escapes), solid floor (mirror).
static float pressureAt(texture3d<float, access::read> t, int3 q, int3 m, float self) {
    if (q.y < 0) return self;
    if (any(q < int3(0)) || any(q > m)) return 0.0;
    return t.read(uint3(q)).x;
}

kernel void fluidAdvect(texture3d<float, access::sample> src [[texture(0)]],
                        texture3d<float, access::sample> vel [[texture(1)]],
                        texture3d<float, access::write> dst [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        constant float& direction [[buffer(1)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float3 pos = float3(g);
    float3 v = vel.read(g).xyz;
    float3 back = pos - direction * p.step.x * v;
    dst.write(src.sample(volumeSampler, cellUVW(back, p)), g);
}

// MacCormack correction: second-order advection with a limiter (crisp flames, no ringing).
kernel void fluidCorrect(texture3d<float, access::sample> src [[texture(0)]],
                         texture3d<float, access::sample> vel [[texture(1)]],
                         texture3d<float, access::read> fwd [[texture(2)]],
                         texture3d<float, access::read> bwd [[texture(3)]],
                         texture3d<float, access::write> dst [[texture(4)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float3 back = float3(g) - p.step.x * vel.read(g).xyz;
    float3 b = clamp(floor(back), float3(0.0), p.dims.xyz - 1.0);
    float4 lo = float4(1e9), hi = float4(-1e9);
    for (int k = 0; k < 8; ++k) {
        float3 o = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float4 v = src.read(uint3(min(b + o, p.dims.xyz - 1.0)));
        lo = min(lo, v);
        hi = max(hi, v);
    }
    float4 r = fwd.read(g) + 0.5 * (src.read(g) - bwd.read(g));
    dst.write(clamp(r, lo, hi), g);
}

kernel void fluidCombust(texture3d<float, access::read> src [[texture(0)]],
                         texture3d<float, access::write> dst [[texture(1)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float4 s = src.read(g);
    float dt = p.step.x, time = p.step.y;
    float3 pos = float3(g) + 0.5;
    // Source: a noisy sphere fed with fuel (fire) or heat + vapour (smoke / steam).
    float r = p.source.w;
    float d = length((pos - p.source.xyz) / float3(1.0, 0.35, 1.0));  // a flat bed of fuel
    // Patchy, flickering fuel: separate tongues instead of one column.
    float flick = noise3(pos * 0.16 + float3(0.0, -time * 1.5, p.step.z)) * noise3(pos * 0.45 + float3(p.step.z, -time * 4.0, 0.0)) * 2.4;
    float inSrc = saturate(1.0 - d / max(r, 0.5)) * flick * p.decay.z;
    float feed = p.feed.x * p.step.w;
    if (feed > 0.0) {
        s.b += inSrc * feed * dt * 9.0;
    } else {
        s.g += inSrc * p.feed.y * dt * 3.0;
        s.r += inSrc * p.feed.z * dt * 3.0;
    }
    // Combustion: fuel burns into heat and soot.
    float burn = min(s.b, s.b * p.physics.w * dt + 0.02 * dt);
    s.b -= burn;
    s.g += burn * p.feed.y * 2.6;
    s.r += burn * p.feed.z * 0.9;
    // Heat radiates away (faster when hotter); smoke thins.
    s.g = max(0.0, s.g - (p.decay.x * dt) * (0.25 + s.g * 0.75));
    s.r = max(0.0, s.r * (1.0 - p.decay.y * dt));
    // Soft walls: everything fades near the sides and top of the box (no hard edges).
    float3 e = min(pos, p.dims.xyz - pos);
    float wall = saturate(min(min(e.x, e.z), e.y + 4.0) / 3.0) * saturate((p.dims.y - pos.y) / 4.0);
    s.rg *= mix(1.0, wall, saturate(dt * 8.0));
    s.b = min(s.b, 3.0);
    dst.write(s, g);
}

kernel void fluidCurl(texture3d<float, access::read> vel [[texture(0)]],
                      texture3d<float, access::write> curl [[texture(1)]],
                      constant FluidParams& p [[buffer(0)]],
                      uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float3 dx = (readClamped(vel, c + int3(1, 0, 0), m).xyz - readClamped(vel, c - int3(1, 0, 0), m).xyz) * 0.5;
    float3 dy = (readClamped(vel, c + int3(0, 1, 0), m).xyz - readClamped(vel, c - int3(0, 1, 0), m).xyz) * 0.5;
    float3 dz = (readClamped(vel, c + int3(0, 0, 1), m).xyz - readClamped(vel, c - int3(0, 0, 1), m).xyz) * 0.5;
    float3 w = float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
    curl.write(float4(w, length(w)), g);
}

kernel void fluidForces(texture3d<float, access::read> vel [[texture(0)]],
                        texture3d<float, access::read> curl [[texture(1)]],
                        texture3d<float, access::read> scal [[texture(2)]],
                        texture3d<float, access::write> dst [[texture(3)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float dt = p.step.x, time = p.step.y;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float3 v = vel.read(g).xyz;
    float4 s = scal.read(g);
    float invCell = 1.0 / p.dims.w;
    // Buoyancy: heat lifts, soot weighs a little.
    v.y += (p.physics.x * s.g * 9.0 - s.r * 0.6) * invCell * dt;
    // Vorticity confinement: feeds back the small swirls numerical diffusion eats.
    float3 grad = float3(readClamped(curl, c + int3(1, 0, 0), m).w - readClamped(curl, c - int3(1, 0, 0), m).w,
                         readClamped(curl, c + int3(0, 1, 0), m).w - readClamped(curl, c - int3(0, 1, 0), m).w,
                         readClamped(curl, c + int3(0, 0, 1), m).w - readClamped(curl, c - int3(0, 0, 1), m).w) * 0.5;
    float gl = length(grad);
    if (gl > 1e-5) {
        float3 N = grad / gl;
        float3 w = curl.read(g).xyz;
        v += cross(N, w) * p.physics.y * 2.2 * dt;
    }
    // Source jet with turbulence, and the wind (where there is gas to carry).
    float3 pos = float3(g) + 0.5;
    float d = length((pos - p.source.xyz) / float3(1.0, 0.35, 1.0));
    float inSrc = saturate(1.0 - d / max(p.source.w, 0.5)) * p.decay.z;
    float3 n = float3(noise3(pos * 0.21 + float3(time * 1.7, 0, p.step.z)), noise3(pos * 0.21 + float3(7.1, time * 1.3, 2.0)),
                      noise3(pos * 0.21 + float3(3.3, 1.9, time * 1.9))) - 0.5;
    v += (float3(0.0, p.feed.w, 0.0) - v) * inSrc * saturate(dt * 6.0);
    v += n * p.physics.z * 40.0 * dt * (inSrc + saturate(s.g) * 0.5);
    float gas = saturate(s.r * 2.0 + s.g);
    v += (p.wind.xyz - v) * gas * saturate(dt * 0.8);
    v *= 1.0 - saturate(dt * 0.05);
    dst.write(float4(v, 0.0), g);
}

kernel void fluidDivergence(texture3d<float, access::read> vel [[texture(0)]],
                            texture3d<float, access::write> div [[texture(1)]],
                            constant FluidParams& p [[buffer(0)]],
                            uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float below = c.y > 0 ? readClamped(vel, c - int3(0, 1, 0), m).y : 0.0;  // solid floor
    float d = 0.5 * ((readClamped(vel, c + int3(1, 0, 0), m).x - readClamped(vel, c - int3(1, 0, 0), m).x) +
                     (readClamped(vel, c + int3(0, 1, 0), m).y - below) +
                     (readClamped(vel, c + int3(0, 0, 1), m).z - readClamped(vel, c - int3(0, 0, 1), m).z));
    div.write(float4(d), g);
}

kernel void fluidJacobi(texture3d<float, access::read> pin [[texture(0)]],
                        texture3d<float, access::read> div [[texture(1)]],
                        texture3d<float, access::write> pout [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float self = pin.read(g).x;
    float sum = pressureAt(pin, c + int3(1, 0, 0), m, self) + pressureAt(pin, c - int3(1, 0, 0), m, self) +
                pressureAt(pin, c + int3(0, 1, 0), m, self) + pressureAt(pin, c - int3(0, 1, 0), m, self) +
                pressureAt(pin, c + int3(0, 0, 1), m, self) + pressureAt(pin, c - int3(0, 0, 1), m, self);
    pout.write(float4((sum - div.read(g).x) / 6.0), g);
}

kernel void fluidProject(texture3d<float, access::read> vel [[texture(0)]],
                         texture3d<float, access::read> pr [[texture(1)]],
                         texture3d<float, access::write> dst [[texture(2)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float self = pr.read(g).x;
    float3 grad = 0.5 * float3(pressureAt(pr, c + int3(1, 0, 0), m, self) - pressureAt(pr, c - int3(1, 0, 0), m, self),
                               pressureAt(pr, c + int3(0, 1, 0), m, self) - pressureAt(pr, c - int3(0, 1, 0), m, self),
                               pressureAt(pr, c + int3(0, 0, 1), m, self) - pressureAt(pr, c - int3(0, 0, 1), m, self));
    float3 v = vel.read(g).xyz - grad;
    if (g.y == 0) v.y = max(v.y, 0.0);
    dst.write(float4(v, 0.0), g);
}

// --- Ray marching ---------------------------------------------------------------------------

struct VolumeUniforms {
    float4x4 model;     // box space (bottom-center origin, meters) -> world
    float4x4 invModel;
    float4 size;        // xyz box size (m)
    float4 flame;       // x intensity, y temperature (K), z smoke density, w steps
    float4 smokeColor;  // rgb linear
    float4 glow;        // rgb light color (linear) * light, w = jitter seed
};

struct VolumeOut {
    float4 position [[position]];
    float3 worldPos;
};

vertex VolumeOut volumeVertex(uint vid [[vertex_id]], const device Vertex* verts [[buffer(0)]],
                              constant VolumeUniforms& u [[buffer(1)]], constant FrameUniforms& f [[buffer(2)]]) {
    float3 lp = float3(verts[vid].position);  // unit cube, -0.5..0.5
    float3 boxP = float3(lp.x * u.size.x, (lp.y + 0.5) * u.size.y, lp.z * u.size.z);
    float4 w = u.model * float4(boxP, 1.0);
    VolumeOut o;
    o.position = f.viewProj * w;
    o.worldPos = w.xyz;
    return o;
}

// Blackbody color (Kelvin -> linear RGB, normalized), after Tanner Helland's fit.
static float3 blackbody(float k) {
    float t = clamp(k, 1000.0, 12000.0) / 100.0;
    float r = t <= 66.0 ? 1.0 : saturate(1.2929 * pow(t - 60.0, -0.1332));
    float g = t <= 66.0 ? saturate(0.3901 * log(t) - 0.6318) : saturate(1.1299 * pow(t - 60.0, -0.0755));
    float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : saturate(0.5432 * log(t - 10.0) - 1.1962));
    float3 c = float3(r, g, b);
    return pow(c, float3(2.2));  // the fit is in sRGB
}

fragment MainOut volumeFragment(VolumeOut in [[stage_in]],
                                constant VolumeUniforms& u [[buffer(0)]],
                                constant FrameUniforms& f [[buffer(1)]],
                                constant GPULight* lights [[buffer(2)]],
                                texture3d<float> scal [[texture(0)]],
                                texturecube<float> envTex [[texture(5)]],
                                depth2d<float> sceneDepth [[texture(7)]]) {
    float3 camW = f.cameraPos.xyz;
    float3 ro = (u.invModel * float4(camW, 1.0)).xyz;
    float3 rdW = normalize(in.worldPos - camW);
    float3 rd = normalize((u.invModel * float4(rdW, 0.0)).xyz);
    float3 bmin = float3(-u.size.x * 0.5, 0.0, -u.size.z * 0.5), bmax = float3(u.size.x * 0.5, u.size.y, u.size.z * 0.5);
    float3 inv = 1.0 / rd;
    float3 t0 = (bmin - ro) * inv, t1 = (bmax - ro) * inv;
    float tn = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float tf = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    tn = max(tn, 0.0);
    // Stop at opaque geometry.
    float2 suv = in.position.xy * f.viewport.zw;
    float sd = sceneDepth.sample(pointClamp, suv);
    if (sd < 0.99999) {
        float3 sw = reconstructWorld(f, suv, sd);
        float sceneT = length((u.invModel * float4(sw, 1.0)).xyz - ro);
        tf = min(tf, sceneT);
    }
    if (tf <= tn) discard_fragment();

    int steps = int(u.flame.w);
    float dt = (tf - tn) / float(steps);
    float jitter = interleavedGradientNoise(in.position.xy + u.glow.w * 37.0);
    float3 L = normalize((u.invModel * float4(-f.sunDir.xyz, 0.0)).xyz);
    float3 sunRad = f.sunColor.rgb * f.sunDir.w;
    float3 amb = envTex.sample(cubeSampler, float3(0, 1, 0), level(f.extra.z)).rgb * f.ground.w * 2.0;
    float cosT = dot(rdW, -f.sunDir.xyz);
    float g = 0.35;
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.0796;
    float3 C = float3(0.0);
    float T = 1.0;
    float tempK = u.flame.y;
    float sunStep = u.size.y * 0.07;
    for (int i = 0; i < steps && T > 0.01; ++i) {
        float t = tn + (float(i) + jitter) * dt;
        float3 pB = ro + rd * t;
        float3 uvw = (pB - bmin) / (bmax - bmin);
        float4 s = scal.sample(volumeSampler, uvw);
        float smoke = s.r * u.flame.z;
        float heat = s.g;
        if (smoke + heat < 0.002) continue;
        float sigma = smoke * 6.0 + smoothstep(0.1, 0.6, heat) * 0.6;
        // Flames: blackbody emission; brightness grows steeply with temperature.
        float h = saturate(heat);
        float k = mix(800.0, tempK, h);
        float3 emit = blackbody(k) * pow(k / tempK, 4.0) * smoothstep(0.1, 0.55, heat) * min(heat, 2.0) * u.flame.x * 14.0;
        // Smoke: sun through a short shadow march, sky light, and the fire's glow from below.
        float3 lit = float3(0.0);
        if (smoke > 0.003) {
            float od = 0.0;
            for (int j = 1; j <= 5; ++j) {
                float3 q = (pB + L * (sunStep * float(j)) - bmin) / (bmax - bmin);
                if (any(q < 0.0) || any(q > 1.0)) break;
                od += scal.sample(volumeSampler, q).r * u.flame.z * 6.0 * sunStep;
            }
            float3 below = (pB - float3(0.0, u.size.y * 0.12, 0.0) - bmin) / (bmax - bmin);
            float fireBelow = saturate(scal.sample(volumeSampler, clamp(below, 0.0, 1.0)).g);
            lit = u.smokeColor.rgb * (sunRad * exp(-od) * (phase * 2.5 + 0.12) + amb * 0.55 + u.glow.rgb * fireBelow * 0.12);
            int count = int(f.params.y);
            float3 pw = (u.model * float4(pB, 1.0)).xyz;
            for (int li = 0; li < count; ++li) {
                float3 Ll;
                lit += u.smokeColor.rgb * pointLightAt(lights[li], pw, -rdW, Ll) * 0.22;
            }
        }
        float a = exp(-sigma * dt);
        float3 src = emit + lit * smoke * 6.0;
        C += T * src * (1.0 - a) / max(sigma, 1e-4);
        T *= a;
    }
    float alpha = 1.0 - T;
    float3 center = (u.model * float4(0.0, u.size.y * 0.5, 0.0, 1.0)).xyz;
    float fogAmt = fogFactor(f, center);
    C = mix(C, f.fog.rgb * alpha, fogAmt);
    MainOut o;
    o.color = float4(C, alpha);
    o.ambient = float4(0.0, 0.0, 0.0, alpha);
    return o;
}

// ---------------------------------------------------------------------------
// Editor grid (procedural, on y = 0)
// ---------------------------------------------------------------------------

struct GridOut {
    float4 position [[position]];
    float3 worldPos;
};

vertex GridOut gridVertex(uint vid [[vertex_id]], constant FrameUniforms& f [[buffer(0)]]) {
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
    float extent = 400.0;
    float2 c = corners[vid] * extent + floor(f.cameraPos.xz);
    GridOut o;
    o.worldPos = float3(c.x, 0.002, c.y);  // lifted slightly to avoid z-fighting with ground planes
    o.position = f.viewProj * float4(o.worldPos, 1.0);
    return o;
}

static float gridLine(float2 p, float spacing) {
    float2 g = abs(fract(p / spacing - 0.5) - 0.5) / fwidth(p / spacing);
    return 1.0 - min(min(g.x, g.y), 1.0);
}

fragment MainOut gridFragment(GridOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]]) {
    float2 p = in.worldPos.xz;
    float minor = gridLine(p, 1.0) * 0.35;
    float major = gridLine(p, 10.0) * 0.6;
    float a = max(minor, major);
    float3 col = float3(0.92, 0.95, 1.0);
    float2 w = fwidth(p);
    if (abs(p.y) < w.y * 1.2) { col = float3(0.95, 0.35, 0.35); a = 0.9; }  // X axis
    if (abs(p.x) < w.x * 1.2) { col = float3(0.35, 0.55, 1.0); a = 0.9; }   // Z axis
    float dist = length(f.cameraPos.xyz - in.worldPos);
    float fade = saturate(1.0 - dist / (40.0 + abs(f.cameraPos.y) * 6.0));
    a *= fade * 0.55;
    if (a < 0.003) discard_fragment();
    MainOut o;
    o.color = float4(col, a);
    o.ambient = float4(0.0, 0.0, 0.0, a);
    return o;
}

// ---------------------------------------------------------------------------
// Present (copy offscreen color to the drawable)
// ---------------------------------------------------------------------------

fragment float4 presentFragment(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    return src.sample(presentSampler, uvOf(in));
}

// ---------------------------------------------------------------------------
// Screen-space ambient occlusion (half resolution) + blur
// ---------------------------------------------------------------------------

constant float3 kAOKernel[12] = {
    float3(0.106, 0.026, 0.192), float3(-0.217, 0.113, 0.288), float3(0.287, -0.261, 0.178), float3(-0.064, -0.302, 0.334),
    float3(0.425, 0.167, 0.214), float3(-0.403, -0.177, 0.391), float3(0.143, 0.471, 0.402), float3(-0.297, 0.462, 0.227),
    float3(0.611, -0.218, 0.305), float3(-0.612, 0.103, 0.413), float3(0.157, -0.683, 0.492), float3(0.082, 0.351, 0.871)};

static float3 viewPos(constant AOUniforms& a, float2 uv, float depth) {
    float4 p = a.invProj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    return p.xyz / p.w;
}

fragment float ssaoFragment(FullscreenOut in [[stage_in]], constant AOUniforms& a [[buffer(0)]],
                            depth2d<float> depth [[texture(0)]]) {
    float2 uv = uvOf(in);
    float d0 = depth.sample(linearClamp, uv);
    if (d0 >= 1.0) return 1.0;
    float3 P = viewPos(a, uv, d0);
    float2 t = a.params.zw;
    float3 Px = viewPos(a, uv + float2(t.x, 0), depth.sample(linearClamp, uv + float2(t.x, 0)));
    float3 Pxn = viewPos(a, uv - float2(t.x, 0), depth.sample(linearClamp, uv - float2(t.x, 0)));
    float3 Py = viewPos(a, uv + float2(0, t.y), depth.sample(linearClamp, uv + float2(0, t.y)));
    float3 Pyn = viewPos(a, uv - float2(0, t.y), depth.sample(linearClamp, uv - float2(0, t.y)));
    float3 dx = abs(Px.z - P.z) < abs(Pxn.z - P.z) ? Px - P : P - Pxn;
    float3 dy = abs(Py.z - P.z) < abs(Pyn.z - P.z) ? Py - P : P - Pyn;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, P) > 0.0) N = -N;
    float ang = interleavedGradientNoise(in.position.xy) * 6.2831853;
    float3 rv = float3(cos(ang), sin(ang), 0.0);
    float3 T = normalize(rv - N * dot(rv, N));
    float3 B = cross(N, T);
    float radius = a.params.x;
    float occ = 0.0;
    for (int i = 0; i < 12; ++i) {
        float3 sp = P + (T * kAOKernel[i].x + B * kAOKernel[i].y + N * kAOKernel[i].z) * radius;
        float4 c = a.proj * float4(sp, 1.0);
        float2 suv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        float sd = depth.sample(linearClamp, suv);
        float3 S = viewPos(a, suv, sd);
        float range = smoothstep(0.0, 1.0, radius / max(abs(P.z - S.z), 1e-4));
        occ += (S.z >= sp.z + 0.02 * radius ? 1.0 : 0.0) * range;
    }
    return saturate(1.0 - occ / 12.0);
}

fragment float aoBlurFragment(FullscreenOut in [[stage_in]], constant PostUniforms& p [[buffer(0)]],
                              texture2d<float> ao [[texture(0)]]) {
    float2 uv = uvOf(in);
    float sum = 0.0;
    for (int y = -2; y < 2; ++y) {
        for (int x = -2; x < 2; ++x) sum += ao.sample(linearClamp, uv + (float2(x, y) + 0.5) * p.texel.xy).r;
    }
    return sum / 16.0;
}

// ---------------------------------------------------------------------------
// Post-processing: bloom (threshold -> downsample chain -> additive upsample) and the
// final composite (AO on indirect light, white balance, exposure, bloom, tonemap,
// saturation/contrast, vignette, dither).
// ---------------------------------------------------------------------------

static float3 sampleBox4(texture2d<float> t, float2 uv, float2 texel) {
    float4 o = texel.xyxy * float4(-1.0, -1.0, 1.0, 1.0);
    return 0.25 * (t.sample(linearClamp, uv + o.xy).rgb + t.sample(linearClamp, uv + o.zy).rgb +
                   t.sample(linearClamp, uv + o.xw).rgb + t.sample(linearClamp, uv + o.zw).rgb);
}

fragment float4 bloomPrefilter(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                               constant PostUniforms& p [[buffer(0)]]) {
    float3 c = sampleBox4(src, uvOf(in), p.texel.xy) * p.params.x;
    float brightness = max(c.r, max(c.g, c.b));
    float knee = p.params.z * 0.5;
    float soft = clamp(brightness - p.params.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contribution = max(soft, brightness - p.params.z) / max(brightness, 1e-4);
    return float4(min(c * contribution, float3(64.0)), 1.0);
}

fragment float4 bloomDown(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                          constant PostUniforms& p [[buffer(0)]]) {
    return float4(sampleBox4(src, uvOf(in), p.texel.xy), 1.0);
}

fragment float4 bloomUp(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                        constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float2 t = p.texel.xy;
    // 9-tap tent filter
    float3 c = src.sample(linearClamp, uv).rgb * 4.0;
    c += (src.sample(linearClamp, uv + float2(-t.x, 0)).rgb + src.sample(linearClamp, uv + float2(t.x, 0)).rgb +
          src.sample(linearClamp, uv + float2(0, -t.y)).rgb + src.sample(linearClamp, uv + float2(0, t.y)).rgb) * 2.0;
    c += src.sample(linearClamp, uv + t).rgb + src.sample(linearClamp, uv - t).rgb +
         src.sample(linearClamp, uv + float2(t.x, -t.y)).rgb + src.sample(linearClamp, uv + float2(-t.x, t.y)).rgb;
    return float4(c / 16.0, 1.0);
}

static float3 tonemapACES(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

static float3 agxContrast(float3 x) {
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

static float3 tonemapAgX(float3 c) {
    const float3x3 inset = float3x3(float3(0.842479, 0.0423282, 0.0423756), float3(0.0784336, 0.878468, 0.0784336),
                                    float3(0.0792237, 0.0791661, 0.879142));
    const float3x3 outset = float3x3(float3(1.19688, -0.0528968, -0.0529716), float3(-0.0980209, 1.15190, -0.0980435),
                                     float3(-0.0990297, -0.0989612, 1.15107));
    c = inset * max(c, 1e-10);
    c = clamp(log2(c), -12.47393, 4.026069);
    c = (c + 12.47393) / 16.5;
    c = agxContrast(c);
    c = outset * c;
    return saturate(pow(max(c, 0.0), float3(2.2)));
}

static float3 tonemapNeutral(float3 c) {  // Khronos PBR Neutral
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, float3(newPeak), g);
}

static float3 hable(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

static float3 tonemapFilmic(float3 c) { return saturate(hable(c * 2.0) / hable(float3(11.2))); }

// ---------------------------------------------------------------------------
// Volumetric light (half resolution): march from the eye to the first surface through
// height-falling haze; in-scatter the sun (shadow-mapped, forward-scattering phase, so the
// shafts shine toward it) and every point/spot light (cones under street lamps).
// ---------------------------------------------------------------------------

struct VolumetricUniforms {
    float4 params;  // x = strength, y = haze density, z = max distance, w = frame
};

fragment float4 volumetricFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                   constant VolumetricUniforms& vu [[buffer(1)]], constant GPULight* lights [[buffer(2)]],
                                   depth2d<float> depthTex [[texture(0)]], depth2d<float> atlas [[texture(1)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float3 eye = f.cameraPos.xyz;
    float3 end = reconstructWorld(f, uv, min(d, 0.9999999));
    float3 ray = end - eye;
    float len = min(length(ray), d >= 0.99999 ? vu.params.z : vu.params.z);
    float3 dir = normalize(ray);
    const int steps = 28;
    float ds = len / float(steps);
    float jitter = interleavedGradientNoise(in.position.xy + vu.params.w * 7.13);
    float3 L = -f.sunDir.xyz;
    float cosT = dot(dir, L);
    float g = 0.62;
    float phaseSun = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.0796;
    float3 sunRad = f.sunColor.rgb * f.sunDir.w * (L.y > -0.05 ? 1.0 : 0.0);
    float falloff = max(f.extra.x, 0.02);
    int count = int(f.params.y);
    float3 acc = float3(0.0);
    float T = 1.0;
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + jitter) * ds;
        float3 p = eye + dir * t;
        float density = vu.params.y * exp(-max(p.y, 0.0) * falloff);
        float3 li = sunRad * sunVisibility(p, f, atlas) * phaseSun;
        for (int k = 0; k < count; ++k) {
            GPULight l = lights[k];
            if (l.kind.x < 0.5) continue;
            float3 Ll;
            float3 rad = pointLightAt(l, p, -dir, Ll);
            li += rad * 0.12;  // near-isotropic for lamps
        }
        float a = exp(-density * ds);
        acc += T * li * density * ds;
        T *= a;
    }
    return float4(acc * vu.params.x, T);
}

fragment float4 compositeFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]], texture2d<float> ambient [[texture(2)]],
                                  texture2d<float> ao [[texture(3)]], texture2d<float> vol [[texture(4)]],
                                  constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float3 c = hdr.sample(linearClamp, uv).rgb;
    if (p.grade.z > 0.0) {  // volumetric light, blurred by sampling a few taps of the half-res buffer
        float2 tx = p.texel.xy * 2.0;
        float3 v = vol.sample(linearClamp, uv).rgb * 0.4 + (vol.sample(linearClamp, uv + float2(tx.x, tx.y)).rgb +
                   vol.sample(linearClamp, uv - float2(tx.x, tx.y)).rgb + vol.sample(linearClamp, uv + float2(-tx.x, tx.y)).rgb +
                   vol.sample(linearClamp, uv + float2(tx.x, -tx.y)).rgb) * 0.15;
        float T = vol.sample(linearClamp, uv).a;
        c = c * T + v;  // haze dims what lies behind it and glows where light crosses it
    }
    if (p.texel.z > 0.0) {
        float occ = mix(1.0, ao.sample(linearClamp, uv).r, saturate(p.texel.z));
        float3 amb = ambient.sample(linearClamp, uv).rgb;
        c = max(c - amb * (1.0 - occ) * max(p.texel.z, 1.0), 0.0);
    }
    // White balance (approximate: warm/cool along blue-orange, tint along green-magenta)
    float3 wb = float3(1.0 + p.grade.x * 0.18 + p.grade.y * 0.06, 1.0 - p.grade.y * 0.12, 1.0 - p.grade.x * 0.22 + p.grade.y * 0.06);
    c *= wb;
    c *= p.params.x;
    c += bloom.sample(linearClamp, uv).rgb * p.params.y;
    int tm = int(p.params2.w + 0.5);
    if (tm == 0) c = tonemapACES(c);
    else if (tm == 1) c = tonemapAgX(c);
    else if (tm == 2) c = saturate(tonemapNeutral(c));
    else if (tm == 3) c = tonemapFilmic(c);
    else c = saturate(c);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(mix(float3(luma), c, p.params.w), 0.0);
    c = saturate((c - 0.5) * p.params2.x + 0.5);
    float2 v = (uv - 0.5) * float2(p.params2.z, 1.0);
    c *= 1.0 - p.params2.y * smoothstep(0.35, 1.05, length(v) * 1.15);
    // Dither before quantizing to 8 bits (removes banding in skies and fog).
    float n = interleavedGradientNoise(in.position.xy + p.texel.w * 5.588238) - 0.5;
    c += n / 255.0;
    return float4(c, 1.0);
}
