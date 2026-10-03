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
    float4 sky;            // x = mode (0 gradient, 1 atmosphere), y = clouds, z = stars, w = reflections
    float4 extra;          // x = fog height falloff, y = shadow softness, z = env max mip, w = unused
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
};

struct MeshOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv;
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

fragment MainOut skyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]]) {
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float4 nearP = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    float3 c = skyColor(dir, f, true);
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
                               constant EnvUniforms& e [[buffer(1)]]) {
    float3 dir = cubeDir(e.face.x, uvOf(in));
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
    s.albedo = d.color.rgb;
    s.alpha = d.color.a;
    if (d.maps.x > 0.5) {
        float4 t = tri ? sampleTri(albedoTex, tp) : albedoTex.sample(materialSampler, uv);
        s.albedo *= t.rgb;
        s.alpha *= t.a;
        if (s.alpha < 0.02) discard_fragment();
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

fragment float4 compositeFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]], texture2d<float> ambient [[texture(2)]],
                                  texture2d<float> ao [[texture(3)]], constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float3 c = hdr.sample(linearClamp, uv).rgb;
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
