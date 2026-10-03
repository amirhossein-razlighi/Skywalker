// Skywalker 2D world shaders: sprites, tiles, SDF world text, 2D light halos and shadow occluders.
//
// Instances are world-space parallelograms (origin + s * axisX + t * axisY) drawn as two triangles in
// painter's order inside the scene pass (depth-tested against 3D geometry, no depth writes). Lit
// sprites gather an ambient term plus point/spot 2D lights with optional normal maps and screen-space
// soft shadows marched through an occluder mask. Output is linear HDR, so bloom, tonemapping and
// grading from the post stack apply to 2D exactly as to 3D.

#include <metal_stdlib>
using namespace metal;

struct SpriteInstance {
    float4 origin;    // xyz, w = fog factor
    float4 axisX;
    float4 axisY;
    float4 uv;        // u0 v0 u1 v1
    float4 color;     // linear rgba
    float4 emission;  // Color: rgb glow; Sdf: outline rgb + width
    float4 params;    // x = mode (0 color, 1 sdf, 2 halo), y = lit, z = normal map, w = sdf dilation
    float4 extra;     // x = casts shadows, y = alpha cutoff
};

struct Sprite2DUniforms {
    float4x4 viewProj;
    float4 cameraPos;   // xyz
    float4 fog;         // rgb linear, a = density
    float4 ambient;     // rgb, a = 1 when 2D lighting is on
    float4 params;      // x = light count, y = time, z = shadows available, w = unused
    float4 viewport;    // w, h, 1/w, 1/h
};

struct Light2D {
    float4 positionRadius;  // xyz, radius
    float4 colorFalloff;    // rgb (intensity applied), falloff exponent
    float4 directionCone;   // xyz spot axis, w = kind (1 point, 2 spot)
    float4 extra;           // x = cos inner, y = cos outer, z = height, w = shadows (softness if > 0, -1 = none)
};

struct VOut {
    float4 position [[position]];
    float3 world;
    float2 uv;
    float2 local;  // 0..1 across the quad (halos)
    uint instance [[flat]];
};

// The scene pass writes a G-buffer (Common.metal MainOut): 2D quads mark their pixels "no lighting"
// (flag 4 in gbufB.w) so screen-space GI and reflections leave sprites exactly as shaded here.
struct SpriteOut {
    float4 color [[color(0)]];
    float4 gbufA [[color(1)]];
    float4 gbufB [[color(2)]];
};

static SpriteOut spriteOut(float4 color, float3 albedo) {
    SpriteOut o;
    o.color = color;
    o.gbufA = float4(albedo, 1.0);
    o.gbufB = float4(0.0, 0.0, 1.0, 4.0);  // octahedral normal (0,0,1), roughness 1, kGbufNoLighting
    return o;
}

constant float2 kCorners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};

vertex VOut spriteVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                         const device SpriteInstance* instances [[buffer(0)]],
                         constant Sprite2DUniforms& u [[buffer(1)]]) {
    SpriteInstance s = instances[iid];
    float2 c = kCorners[vid];
    float3 world = s.origin.xyz + s.axisX.xyz * c.x + s.axisY.xyz * c.y;
    VOut o;
    o.position = u.viewProj * float4(world, 1.0);
    o.world = world;
    o.uv = mix(s.uv.xy, s.uv.zw, c);
    o.local = c;
    o.instance = iid;
    return o;
}

static float shadowAt(float2 fragUv, float3 lightPos, constant Sprite2DUniforms& u, texture2d<float> occluders, sampler smp,
                      float softness) {
    float4 clip = u.viewProj * float4(lightPos, 1.0);
    if (clip.w <= 1e-4) return 1.0;
    float2 lightUv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    // Pixels that are themselves occluders are lit (no self-shadowing of casters).
    if (occluders.sample(smp, fragUv).r > 0.5) return 1.0;
    const int kSteps = 28;
    float occlusion = 0.0;
    for (int i = 1; i < kSteps; ++i) {
        float t = float(i) / float(kSteps);
        float o = occluders.sample(smp, mix(fragUv, lightUv, t)).r;
        // Soft edges: occluders far from the receiver blur more.
        occlusion = max(occlusion, o * mix(1.0, 0.6, softness * t));
    }
    return 1.0 - saturate(occlusion);
}

fragment SpriteOut spriteFragment(VOut in [[stage_in]],
                               const device SpriteInstance* instances [[buffer(0)]],
                               constant Sprite2DUniforms& u [[buffer(1)]],
                               constant Light2D* lights [[buffer(2)]],
                               texture2d<float> albedo [[texture(0)]],
                               texture2d<float> normalMap [[texture(1)]],
                               texture2d<float> occluders [[texture(2)]],
                               sampler smp [[sampler(0)]]) {
    SpriteInstance s = instances[in.instance];
    int mode = int(s.params.x + 0.5);
    if (mode == 2) {  // halo: soft radial glow, additive
        float r = saturate(1.0 - length(in.local * 2.0 - 1.0));
        return spriteOut(float4(s.color.rgb * r * r, 0.0), float3(0.0));
    }
    float4 tex = albedo.sample(smp, in.uv);
    float4 c;
    if (mode == 1) {  // SDF glyph
        float d = tex.r + s.params.w;
        float fw = fwidth(d);
        d += min(0.06, fw * 0.3);  // stem darkening: small text keeps its weight
        float w = max(fw * 0.55, 1e-3);
        float fill = smoothstep(0.5 - w, 0.5 + w, d);
        float outline = s.emission.w;
        c = float4(s.color.rgb, s.color.a * fill);
        if (outline > 0.0) {
            float outer = smoothstep(0.5 - outline - w, 0.5 - outline + w, d);
            c.rgb = mix(s.emission.rgb, s.color.rgb, fill);
            c.a = s.color.a * outer;
        }
        if (c.a <= 0.002) discard_fragment();
        return spriteOut(c, c.rgb);
    }
    c = tex * s.color;
    if (s.extra.y > 0.0) {
        if (tex.a < s.extra.y) discard_fragment();
        c.a = s.color.a;
    }
    if (c.a <= 0.002) discard_fragment();
    float3 rgb = c.rgb;
    if (u.ambient.a > 0.5 && s.params.y > 0.5) {
        float3 T = normalize(s.axisX.xyz), B = normalize(-s.axisY.xyz);
        float3 N0 = normalize(cross(T, B));
        float3 n = N0;
        bool mapped = s.params.z > 0.5;
        if (mapped) {
            float3 m = normalMap.sample(smp, in.uv).xyz * 2.0 - 1.0;
            n = normalize(T * m.x + B * m.y + N0 * max(m.z, 0.05));
        }
        float3 light = u.ambient.rgb;
        float2 fragUv = in.position.xy * u.viewport.zw;
        int count = int(u.params.x);
        for (int i = 0; i < count; ++i) {
            Light2D l = lights[i];
            float3 toLight = l.positionRadius.xyz - in.world;
            float planar = length(toLight - N0 * dot(toLight, N0));
            float atten = pow(saturate(1.0 - planar / max(l.positionRadius.w, 1e-3)), l.colorFalloff.w);
            if (atten <= 0.0) continue;
            if (l.directionCone.w > 1.5) {  // spot
                float3 dir = normalize(-toLight + N0 * dot(toLight, N0) + float3(1e-6));
                atten *= smoothstep(l.extra.y, l.extra.x, dot(dir, normalize(l.directionCone.xyz)));
            }
            float ndl = 1.0;
            if (mapped) {
                float3 L = normalize(toLight + N0 * l.extra.z);
                ndl = saturate(dot(n, L)) * 1.25;
            }
            float sh = 1.0;
            if (l.extra.w >= 0.0 && u.params.z > 0.5) sh = shadowAt(fragUv, l.positionRadius.xyz, u, occluders, smp, l.extra.w);
            light += l.colorFalloff.rgb * atten * ndl * sh;
        }
        rgb *= light;
    }
    rgb += s.emission.rgb * tex.a;
    // Atmospheric fog by distance (far parallax layers fade into the haze).
    float dist = length(in.world - u.cameraPos.xyz);
    float fog = (1.0 - exp(-u.fog.a * dist)) * s.origin.w;
    rgb = mix(rgb, u.fog.rgb, saturate(fog));
    return spriteOut(float4(rgb, c.a), c.rgb);
}

// Shadow casters write their coverage into the occluder mask.
fragment float occluderFragment(VOut in [[stage_in]], const device SpriteInstance* instances [[buffer(0)]],
                                texture2d<float> albedo [[texture(0)]], sampler smp [[sampler(0)]]) {
    SpriteInstance s = instances[in.instance];
    if (s.extra.x < 0.5 || int(s.params.x + 0.5) != 0) discard_fragment();
    float a = albedo.sample(smp, in.uv).a * s.color.a;
    if (a < 0.4) discard_fragment();
    return 1.0;
}
