// Skywalker standard shaders (Metal Shading Language).
//
// Compiled at runtime from source so that the editor and agents can hot-reload them
// (`render_shader_reload`) and receive compiler diagnostics. Struct layouts must match
// MetalRenderer.mm exactly.

#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4x4 lightViewProj;
    float4 cameraPos;   // xyz, w = time
    float4 sunDir;      // xyz = direction light travels, w = intensity
    float4 sunColor;
    float4 skyTop;
    float4 skyHorizon;
    float4 ground;      // rgb, w = ambient strength
    float4 fog;         // rgb, w = density
    float4 params;      // x = exposure, y = light count, z = shadows on, w = shadow texel size
    float4 viewport;    // x = width, y = height (pixels), z = 1/width, w = 1/height
};

struct DrawUniforms {
    float4x4 model;
    float4x4 normalMatrix;
    float4 color;
    float4 emissive;    // rgb, w = strength
    float4 material;    // x = metallic, y = roughness, z = selected, w = has texture
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

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static float3 skyColor(float3 dir, constant FrameUniforms& f) {
    float t = saturate(dir.y * 1.4 + 0.05);
    float3 sky = mix(f.skyHorizon.rgb, f.skyTop.rgb, pow(t, 0.6));
    float below = saturate(-dir.y * 4.0);
    sky = mix(sky, mix(f.skyHorizon.rgb, f.ground.rgb, 0.45), below);
    float3 toSun = -f.sunDir.xyz;
    float sd = max(dot(dir, toSun), 0.0);
    sky += f.sunColor.rgb * (pow(sd, 900.0) * 6.0 + pow(sd, 12.0) * 0.18) * step(-0.02, toSun.y);
    return sky;
}

static float3 acesTonemap(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

static float3 ggx(float3 N, float3 V, float3 L, float3 radiance, float3 albedo, float metallic, float roughness) {
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (M_PI_F * dd * dd);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 F0 = mix(float3(0.04), albedo, metallic);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float3 spec = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    return (kd * albedo / M_PI_F + spec) * radiance * NdotL * M_PI_F;
}

constexpr sampler shadowSampler(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
constexpr sampler albedoSampler(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
constexpr sampler presentSampler(coord::normalized, filter::linear, address::clamp_to_edge);

static float shadowFactor(float3 worldPos, float3 N, constant FrameUniforms& f, depth2d<float> shadowMap) {
    if (f.params.z < 0.5) return 1.0;
    float3 L = -f.sunDir.xyz;
    float3 p = worldPos + N * 0.04;  // normal offset reduces acne
    float4 lc = f.lightViewProj * float4(p, 1.0);
    float3 ndc = lc.xyz / lc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0) return 1.0;
    float bias = 0.0015 + 0.003 * (1.0 - saturate(dot(N, L)));
    float texel = f.params.w;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += shadowMap.sample_compare(shadowSampler, uv + float2(x, y) * texel, ndc.z - bias);
        }
    }
    return sum / 9.0;
}

// ---------------------------------------------------------------------------
// Sky (fullscreen triangle)
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

fragment float4 skyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]]) {
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float3 dir = normalize(farP.xyz / farP.w - f.cameraPos.xyz);
    float3 c = skyColor(dir, f) * f.params.x;
    return float4(acesTonemap(c), 1.0);
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

fragment float4 meshFragment(MeshOut in [[stage_in]],
                             bool frontFacing [[front_facing]],
                             constant DrawUniforms& d [[buffer(0)]],
                             constant FrameUniforms& f [[buffer(1)]],
                             constant GPULight* lights [[buffer(2)]],
                             texture2d<float> albedoTex [[texture(0)]],
                             depth2d<float> shadowMap [[texture(1)]]) {
    float3 N = normalize(in.normal) * (frontFacing ? 1.0 : -1.0);
    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    float3 albedo = d.color.rgb;
    float alpha = d.color.a;
    if (d.material.w > 0.5) {
        float4 t = albedoTex.sample(albedoSampler, in.uv);
        albedo *= t.rgb;
        alpha *= t.a;
        if (alpha < 0.02) discard_fragment();
    }
    float metallic = d.material.x;
    float roughness = clamp(d.material.y, 0.04, 1.0);

    // Sun
    float3 L = -f.sunDir.xyz;
    float sunVisible = L.y > -0.05 ? shadowFactor(in.worldPos, N, f, shadowMap) : 0.0;
    float3 color = ggx(N, V, L, f.sunColor.rgb * f.sunDir.w * sunVisible, albedo, metallic, roughness);

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
                atten *= smoothstep(l.directionCone.w, mix(l.directionCone.w, 1.0, 0.15), cd);
            }
        }
        color += ggx(N, V, Ll, l.colorIntensity.rgb * l.colorIntensity.w * atten, albedo, metallic, roughness);
    }

    // Hemisphere ambient + cheap environment specular
    float up = N.y * 0.5 + 0.5;
    float3 hemi = mix(f.ground.rgb, mix(f.skyHorizon.rgb, f.skyTop.rgb, 0.5), up);
    float3 F0 = mix(float3(0.04), albedo, metallic);
    float3 R = reflect(-V, N);
    float3 env = skyColor(R, f);
    float fres = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    float3 specAmb = env * (F0 + (1.0 - F0) * fres) * (1.0 - roughness) * 0.6;
    color += f.ground.w * (hemi * albedo * (1.0 - metallic) + specAmb);

    color += d.emissive.rgb * d.emissive.w;

    // Fog
    float dist = length(f.cameraPos.xyz - in.worldPos);
    float fogAmt = 1.0 - exp(-dist * f.fog.w);
    color = mix(color, f.fog.rgb, saturate(fogAmt));

    return float4(acesTonemap(color * f.params.x), alpha);
}

// ---------------------------------------------------------------------------
// Selection outline: inverted hull extruded in clip space (constant pixel width)
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
    const float widthPx = 2.5;
    clip.xy += dir * widthPx * 2.0 * f.viewport.zw * clip.w;
    return clip;
}

fragment float4 outlineFragment() {
    return float4(1.0, 0.32, 0.0, 1.0);  // selection orange (linear; sRGB target encodes it)
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

fragment float4 gridFragment(GridOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]]) {
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
    return float4(col, a);
}

// ---------------------------------------------------------------------------
// Present (copy offscreen color to the drawable)
// ---------------------------------------------------------------------------

fragment float4 presentFragment(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    float2 uv = float2(in.ndc.x * 0.5 + 0.5, 0.5 - in.ndc.y * 0.5);
    return src.sample(presentSampler, uv);
}
