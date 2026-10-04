// ---------------------------------------------------------------------------
// Local light shadows (point and spot lights): the shadow atlas, see render/ShadowAtlas.h.
//
// The atlas is a depth2d_array with one slice per quadrant. A light's slot holds one spot view, six
// cube faces (3 x 2 grid) or two paraboloid halves; GPULight::shadow / shadow2 locate it. These
// functions mirror shadows::projectSpot / projectCube / projectParaboloid / shadowClip on the CPU
// (tested in tests/test_shadows.cpp): keep both sides in sync.
// ---------------------------------------------------------------------------

static_assert(sizeof(GPULight) == 128, "GPULight must match MetalRenderer.mm");

constant float kParaboloidSentinel = -2.0;

// Clip position of a world point for a shadow-caster pass: a regular view-projection (sun
// cascades, spot views, cube faces) or an encoded paraboloid hemisphere (m[3].w == sentinel:
// columns 0..2 = basis with w = near, far, uv scale; column 3 = light position).
static float4 shadowClip(float4x4 m, float3 world) {
    if (m[3].w != kParaboloidSentinel) return m * float4(world, 1.0);
    float3 v = world - m[3].xyz;
    float3 lv = float3(dot(v, m[0].xyz), dot(v, m[1].xyz), dot(v, m[2].xyz));
    float dist = max(length(lv), 1e-6);
    float3 d = lv / dist;
    float2 p = d.xy / max(1.0 + d.z, 0.05) / m[2].w;
    float z = (dist - m[0].w) / (m[1].w - m[0].w);
    return float4(p, d.z > -0.2 ? z : d.z + 0.2 - 1e-3, 1.0);  // well behind the hemisphere: clipped
}

struct ShadowBasis {
    float3 s, u, f;
};

static ShadowBasis shadowLightBasis(float3 forward) {
    ShadowBasis b;
    b.f = normalize(forward);
    float3 up = abs(b.f.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0);
    b.s = normalize(cross(b.f, up));
    b.u = cross(b.s, b.f);
    return b;
}

// Cube faces: +X, -X, +Y, -Y, +Z, -Z (shadows::cubeFaceBasis).
constant float3 kShadowCubeS[6] = {float3(0, 0, 1), float3(0, 0, -1), float3(1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0), float3(1, 0, 0)};
constant float3 kShadowCubeU[6] = {float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, 1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0)};
constant float3 kShadowCubeF[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};

static int shadowCubeFace(float3 v) {
    float3 a = abs(v);
    if (a.x >= a.y && a.x >= a.z) return v.x >= 0.0 ? 0 : 1;
    if (a.y >= a.z) return v.y >= 0.0 ? 2 : 3;
    return v.z >= 0.0 ? 4 : 5;
}

// Where a receiver lands: face rect (px inside the slice), uv in the face (0..1, y down), compare depth.
struct LocalShadowCoord {
    float2 rectOrigin, rectSize;
    float2 uv;
    float depth;
    float texelWorld;  // size of one shadow texel at the receiver (m)
    bool inside;
};

static LocalShadowCoord localShadowCoord(GPULight l, int proj, float slotPx, float3 p, float bias) {
    LocalShadowCoord c;
    c.inside = false;
    c.depth = 1.0;
    c.uv = float2(0.5);
    float far = l.positionRange.w;
    float near = clamp(far * 0.004, 0.02, 0.2);
    float t = l.shadow2.w;
    float3 v = p - l.positionRange.xyz;
    if (proj == 3) {  // dual paraboloid
        ShadowBasis b = shadowLightBasis(l.directionCone.xyz);
        float3 lv = float3(dot(v, b.s), dot(v, b.u), dot(v, b.f));
        float dist = max(length(lv), 1e-6);
        float3 d = lv / dist;
        int hemi = d.z >= 0.0 ? 0 : 1;
        if (hemi == 1) d.xz = -d.xz;  // back hemisphere basis: (-s, u, -f)
        float2 pp = d.xy / (1.0 + d.z) / t;
        c.rectSize = float2(floor(slotPx * 0.5), slotPx);
        c.rectOrigin = float2(float(hemi) * c.rectSize.x, 0.0);
        c.uv = float2(pp.x * 0.5 + 0.5, 0.5 - pp.y * 0.5);
        c.inside = all(abs(pp) <= 1.0);
        c.depth = (dist - bias - near) / (far - near);
        c.texelWorld = 4.0 * dist * t / max(c.rectSize.x, 1.0);
        return c;
    }
    float3 S, U, F;
    if (proj == 1) {  // spot
        ShadowBasis b = shadowLightBasis(l.directionCone.xyz);
        S = b.s, U = b.u, F = b.f;
        c.rectSize = float2(slotPx);
        c.rectOrigin = float2(0.0);
    } else {  // cube
        int face = shadowCubeFace(v);
        S = kShadowCubeS[face], U = kShadowCubeU[face], F = kShadowCubeF[face];
        c.rectSize = float2(floor(slotPx / 3.0), floor(slotPx * 0.5));
        c.rectOrigin = float2(float(face % 3), float(face / 3)) * c.rectSize;
    }
    float d = dot(v, F);
    if (d <= near * 0.5) return c;
    float2 xy = float2(dot(v, S), dot(v, U)) / (d * t);
    c.uv = float2(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);
    c.inside = all(abs(xy) <= 1.0);
    float db = max(d - bias, near);
    c.depth = far * (db - near) / ((far - near) * db);
    c.texelWorld = 2.0 * d * t / max(min(c.rectSize.x, c.rectSize.y), 1.0);
    return c;
}

// Visibility (0..1) of local light `l` at `worldPos`: `taps` = 1 (one bilinear compare) or a Poisson
// disc of 4 / 8 / 12 bilinear taps rotated by `noise` (0..1; vary it per frame so TAA and still
// accumulation smooth the penumbra), radius scaled by `softness` (Environment::shadowSoftness).
static float localShadow(GPULight l, float3 worldPos, float3 N, float noise, depth2d_array<float> atlas, int taps,
                         float softness) {
    int code = int(l.shadow.w + 0.5);
    int proj = code & 3;
    if (proj == 0) return 1.0;
    uint slice = uint(code >> 2);
    float W = float(atlas.get_width());
    float slotPx = l.shadow.z * W;
    if (slotPx < 8.0) return 1.0;
    float bias = l.shadow2.y;
    // Normal offset: push the lookup along the geometric normal by a few texels at this distance
    // (surfaces only; volumes and particles pass N = 0 and skip it).
    float3 p = worldPos;
    if (dot(N, N) > 0.0 && l.shadow2.z > 0.0) p += N * (localShadowCoord(l, proj, slotPx, worldPos, 0.0).texelWorld * l.shadow2.z);
    LocalShadowCoord c = localShadowCoord(l, proj, slotPx, p, bias);
    if (!c.inside) return 1.0;  // outside a spot's cone (its light is zero there anyway)
    float2 base = l.shadow.xy * W + c.rectOrigin;
    float2 px = base + c.uv * c.rectSize;
    float2 lo = base + 1.0, hi = base + c.rectSize - 1.0;
    float vis;
    if (taps <= 1) {
        vis = atlas.sample_compare(shadowSampler, clamp(px, lo, hi) / W, slice, c.depth);
    } else {
        float radius = 1.0 + 1.5 * softness;  // texels
        float ang = noise * 6.2831853;
        float2x2 rot = float2x2(float2(cos(ang), sin(ang)), float2(-sin(ang), cos(ang)));
        int n = min(taps, 12);
        float sum = 0.0;
        for (int i = 0; i < n; ++i) {
            float2 s = clamp(px + rot * kPoisson[i] * radius, lo, hi);
            sum += atlas.sample_compare(shadowSampler, s / W, slice, c.depth);
        }
        vis = sum / float(n);
    }
    return mix(1.0, vis, saturate(l.shadow2.x));
}

// pointLightAt with the light's shadow (effects, water, hair, volumetric light).
static float3 localLightAt(GPULight l, float3 pos, float3 n, thread float3& dirOut, depth2d_array<float> atlas, float2 pixel,
                           int taps, float softness) {
    float3 rad = pointLightAt(l, pos, n, dirOut);
    if (l.shadow.w > 0.5 && l.kind.x > 0.5 && any(rad != 0.0)) {
        rad *= localShadow(l, pos, n, interleavedGradientNoise(pixel), atlas, taps, softness);
    }
    return rad;
}

// --- Atlas maintenance and the shadow_atlas debug view ------------------------------------------

// A slot about to be re-rendered is cleared to the far plane by a full-viewport triangle.
vertex float4 shadowClearVertex(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    return float4(p, 1.0, 1.0);
}

struct ShadowDebugParams {
    float4 params;  // x = slot rects, y = output aspect (w / h), z = frame, w = unused
};

// Per shadowed light two float4: (slot origin xy and size z in uv of the 2 x 2 quadrant mosaic, state w:
// 0 cached, 1 re-rendered this frame, 2 stale: waiting for the update budget) and (near, far,
// 1 = linear paraboloid depth, 0).
fragment float4 shadowAtlasDebugFragment(FullscreenOut in [[stage_in]], constant ShadowDebugParams& p [[buffer(0)]],
                                         const device float4* rects [[buffer(1)]], depth2d_array<float> atlas [[texture(0)]]) {
    float2 uv = uvOf(in);
    // A square mosaic of the 4 quadrant slices, centered (letterboxed) in the output.
    float aspect = max(p.params.y, 1e-3);
    float2 q = uv;
    if (aspect >= 1.0) q.x = (uv.x - 0.5) * aspect + 0.5;
    else q.y = (uv.y - 0.5) / aspect + 0.5;
    if (any(q < 0.0) || any(q > 1.0)) return float4(0.04, 0.045, 0.05, 1.0);
    uint slice = uint(q.x >= 0.5) + 2u * uint(q.y >= 0.5);
    float2 local = fract(q * 2.0);
    float d = atlas.sample(pointClamp, local, slice);
    float3 c = float3(0.05, 0.05, 0.06);  // unused atlas space
    float2 fw = fwidth(q);
    int n = min(int(p.params.x), 256);
    for (int i = 0; i < n; ++i) {
        float4 r = rects[i * 2], z = rects[i * 2 + 1];
        if (all(q >= r.xy) && all(q <= r.xy + r.z)) {
            // Distance from the light: white = near, dark blue = the light's range.
            float dist = z.z > 0.5 ? mix(z.x, z.y, d) : z.x * z.y / max(z.y - d * (z.y - z.x), 1e-5);
            float v = d >= 1.0 ? 0.0 : pow(saturate(1.0 - dist / max(z.y, 1e-3)), 1.5);
            c = mix(float3(0.02, 0.03, 0.08), float3(1.0, 0.97, 0.9), v);
        }
    }
    // Quadrant borders.
    float2 e = abs(q - 0.5);
    if (min(e.x, e.y) < fw.x * 1.5) c = float3(0.5, 0.5, 0.55);
    for (int i = 0; i < n; ++i) {
        float4 r = rects[i * 2];
        int state = int(r.w + 0.5);
        float2 a = r.xy, b = r.xy + r.z;
        bool inside = all(q >= a) && all(q <= b);
        float2 dd = min(q - a, b - q);
        if (inside && min(dd.x, dd.y) < fw.x * 2.0) {
            c = state == 1 ? float3(0.2, 1.0, 0.35) : (state == 2 ? float3(1.0, 0.55, 0.1) : float3(0.25, 0.55, 1.0));
        }
    }
    return float4(c, 1.0);
}
