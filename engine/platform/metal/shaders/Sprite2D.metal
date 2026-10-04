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
    float4 extra;     // x = casts shadows, y = alpha cutoff, z = sway pin (0 bottom, 1 top), w = additive
    float4 fx;        // x = blur radius (texels), y = sway amplitude (fraction of width), z = sway Hz, w = sway waves
    float4 flash;     // rgb linear, a = amount
};

struct Sprite2DUniforms {
    float4x4 viewProj;
    float4 cameraPos;   // xyz
    float4 fog;         // rgb linear, a = density
    float4 ambient;     // rgb, a = 1 when 2D lighting is on
    float4 params;      // x = light count, y = time, z = shadows available, w = world units per art texel (0 = screen pixels)
    float4 viewport;    // w, h, 1/w, 1/h
};

struct Light2D {
    float4 positionRadius;  // xyz, radius
    float4 colorFalloff;    // rgb (intensity applied), falloff exponent
    float4 directionCone;   // xyz spot axis, w = kind (1 point, 2 spot) + 4 * bands (stepped, dithered falloff)
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
    float2 velocity [[color(3)]];  // velocity buffer: sprites are static (camera motion only)
};

static SpriteOut spriteOut(float4 color, float3 albedo) {
    SpriteOut o;
    o.color = color;
    o.gbufA = float4(albedo, 1.0);
    o.gbufB = float4(0.0, 0.0, 1.0, 4.0);  // octahedral normal (0,0,1), roughness 1, kGbufNoLighting
    o.velocity = float2(0.0);
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

// 4x4 ordered (Bayer) dither threshold in [0, 1) for a cell of the art's texel grid (or the screen).
static float bayer4(float2 world, float2 screen, constant Sprite2DUniforms& u) {
    float2 cell = u.params.w > 0.0 ? floor(world / u.params.w) : floor(screen);
    int x = int(fmod(fmod(cell.x, 4.0) + 4.0, 4.0)), y = int(fmod(fmod(cell.y, 4.0) + 4.0, 4.0));
    const float m[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (m[y * 4 + x] + 0.5) / 16.0;
}

// Pixel-art falloff: `bands` flat steps; the last third of each step is an ordered-dither fringe into
// the next one (flat pools of light with crisp, stippled rims instead of a smooth 8-bit ramp).
static float stepped(float v, float bands, float threshold) {
    if (bands < 0.5) return v;
    float t = saturate(v) * bands;
    float f = floor(t);
    float fringe = saturate((t - f - 0.66) / 0.34);
    return (f + (fringe > threshold ? 1.0 : 0.0)) / bands;
}

// Wind / cloth sway: the image bends sideways inside its quad, more toward the free edge (in.local.y = 0 is
// the top of the frame). Each quad gets its own phase from its position, so neighbours do not move in lockstep.
static float2 swayUv(SpriteInstance s, VOut in, constant Sprite2DUniforms& u) {
    float v = in.local.y;                                  // 0 top .. 1 bottom
    float free = s.extra.z > 0.5 ? v : 1.0 - v;            // distance from the pinned edge
    float weight = free * free * (3.0 - 2.0 * free);
    float phase = s.origin.x * 0.73 + s.origin.y * 0.31;
    float t = u.params.y * s.fx.z * 6.2831853;
    float wave = sin(t + phase + v * s.fx.w * 6.2831853) * 0.75 + sin(t * 0.53 + phase * 1.7 + v * s.fx.w * 3.1) * 0.25;
    float du = s.fx.y * weight * wave * (s.uv.z - s.uv.x);
    return float2(in.uv.x + du, in.uv.y);
}

// Depth-of-field blur: a golden-angle disk of taps on a coarser mip, averaged in premultiplied alpha so
// soft edges do not pick up the dark color of transparent texels.
static float4 blurred(texture2d<float> tex, sampler smp, float2 uv, float radius) {
    float2 texel = 1.0 / float2(tex.get_width(), tex.get_height());
    float lod = log2(max(radius * 0.35, 1.0));
    float4 acc = float4(0.0);
    const int kTaps = 24;
    for (int i = 0; i < kTaps; ++i) {
        float r = sqrt((float(i) + 0.5) / float(kTaps)) * radius;
        float a = float(i) * 2.39996323;
        float4 t = tex.sample(smp, uv + float2(cos(a), sin(a)) * r * texel, level(lod));
        acc += float4(t.rgb * t.a, t.a);
    }
    acc /= float(kTaps);
    return acc.a > 1e-4 ? float4(acc.rgb / acc.a, acc.a) : float4(0.0);
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
    if (mode == 2) {  // halo: soft radial glow, additive (stepped + dithered for banded lights)
        float r = saturate(1.0 - length(in.local * 2.0 - 1.0));
        float g = stepped(r * r, s.params.y, bayer4(in.world.xy, in.position.xy, u));
        return spriteOut(float4(s.color.rgb * g, 0.0), float3(0.0));
    }
    float2 uv = in.uv;
    if (mode == 0 && s.fx.y != 0.0) uv = swayUv(s, in, u);
    float4 tex = (mode == 0 && s.fx.x > 0.25) ? blurred(albedo, smp, uv, s.fx.x) : albedo.sample(smp, uv);
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
            float3 m = normalMap.sample(smp, uv).xyz * 2.0 - 1.0;
            n = normalize(T * m.x + B * m.y + N0 * max(m.z, 0.05));
        }
        float3 light = u.ambient.rgb;
        float2 fragUv = in.position.xy * u.viewport.zw;
        float dither = bayer4(in.world.xy, in.position.xy, u);
        int count = int(u.params.x);
        for (int i = 0; i < count; ++i) {
            Light2D l = lights[i];
            float3 toLight = l.positionRadius.xyz - in.world;
            float planar = length(toLight - N0 * dot(toLight, N0));
            float atten = pow(saturate(1.0 - planar / max(l.positionRadius.w, 1e-3)), l.colorFalloff.w);
            if (atten <= 0.0) continue;
            float kind = fmod(l.directionCone.w, 4.0);
            atten = stepped(atten, floor(l.directionCone.w / 4.0), dither);
            if (atten <= 0.0) continue;
            if (kind > 1.5) {  // spot
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
    rgb = mix(rgb, s.flash.rgb, saturate(s.flash.a));  // hit flash
    // Atmospheric fog by distance (far parallax layers fade into the haze).
    float dist = length(in.world - u.cameraPos.xyz);
    float fog = (1.0 - exp(-u.fog.a * dist)) * s.origin.w;
    rgb = mix(rgb, u.fog.rgb, saturate(fog));
    if (s.extra.w > 0.5) return spriteOut(float4(rgb * c.a, 0.0), float3(0.0));  // additive: premultiplied, dst kept
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
