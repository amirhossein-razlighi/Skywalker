// ---------------------------------------------------------------------------
// Reflection probes (render/ReflectionProbes.h, MetalProbes.mm). Captured cubemaps live in one
// cube-array atlas (one cube per slot, kMips GGX-prefiltered mips; the roughest one doubles as the
// diffuse ambient). Surfaces find the probes of their light cluster in a bit mask, weight them by
// influence (front to back, shading order) and box-project the reflection vector; the sky fills
// the rest, except inside interior probes. The C++ mirror of this math is unit-tested.
//
// Bindings wherever surfaces are lit: fragment buffer 10 = ProbeBlock, 11 = per-cluster probe
// masks, texture 33 = the atlas.
// ---------------------------------------------------------------------------

struct GPUProbe {
    float4x4 worldToLocal;  // world -> volume frame (meters)
    float4 extents;         // xyz half extents (sphere: x = radius), w = blend distance
    float4 capture;         // xyz capture point (world), w = intensity
    float4 params;          // x = atlas cube, y = base mip, z = flags (1 box projection, 2 interior, 4 sphere), w = ambient (0 probe, 1 sky, 2 color)
    float4 ambient;         // rgb = ambient color (linear x energy), w = debug color index
    float4 projection;      // xyz = projection box center (volume frame), w = 1: project onto it instead of the volume
    float4 projectionHalf;  // xyz = projection box half size
};
static_assert(sizeof(GPUProbe) == 160, "GPUProbe must match probes::GpuProbe");

constant int kProbeMax = 32;
constant int kProbeMaxPerPixel = 8;

struct ProbeBlock {
    float4 info;     // x = probe count (0 = off), y = atlas max mip, z = flags (1 capture pass, 2 interior capture)
    float4 ambient;  // interior capture: the ambient that replaces the sky's light
    GPUProbe probes[kProbeMax];
};

static bool probeFlag(GPUProbe p, int bit) { return (int(p.params.z + 0.5) & bit) != 0; }

// Distance (m) to the nearest face of the volume: > 0 inside. `local` = the point in the volume frame.
static float probeEdgeDistance(GPUProbe p, float3 local) {
    if (probeFlag(p, 4)) return p.extents.x - length(local);
    float3 q = p.extents.xyz - abs(local);
    return min(q.x, min(q.y, q.z));
}

constant float kProbeInteriorMargin = 0.1;     // probes::kInteriorMargin
constant float kProbeInteriorMinWeight = 1e-3;  // probes::kInteriorMinWeight

static float probeInfluence(GPUProbe p, float3 local) {
    float d = probeEdgeDistance(p, local);
    if (probeFlag(p, 2)) {  // interior: the room's walls (on or just outside the faces) still belong to it
        return d <= -kProbeInteriorMargin ? 0.0 : clamp(d / max(p.extents.w, 1e-3), kProbeInteriorMinWeight, 1.0);
    }
    return d <= 0.0 ? 0.0 : saturate(d / max(p.extents.w, 1e-3));
}

// Cubemap lookup direction for reflection vector R at `world` (probes::lookupDir).
static float3 probeLookup(GPUProbe p, float3 world, float3 local, float3 R) {
    if (!probeFlag(p, 1)) return R;
    float3 rl = (p.worldToLocal * float4(R, 0.0)).xyz;
    const bool proxy = p.projection.w > 0.5;  // a projection box other than the volume
    const float3 l = proxy ? local - p.projection.xyz : local;
    const float3 ext = proxy ? p.projectionHalf.xyz : p.extents.xyz;
    float t;
    if (probeFlag(p, 4) && !proxy) {
        float b = dot(l, rl), c = dot(l, l) - ext.x * ext.x, rr = max(dot(rl, rl), 1e-8);
        t = (-b + sqrt(max(b * b - rr * c, 0.0))) / rr;
    } else {
        float3 safeR = select(rl, float3(1e-6), abs(rl) < 1e-6);
        float3 ta = (ext - l) / safeR, tb = (-ext - l) / safeR;
        float3 tmax = max(ta, tb);
        t = min(tmax.x, min(tmax.y, tmax.z));
    }
    return world + R * max(t, 0.0) - p.capture.xyz;
}

struct ProbeSample {
    float3 spec;   // weighted (premultiplied) reflections
    float3 irr;    // weighted diffuse ambient
    float specW;   // accumulated weights 0..1
    float irrW;
    bool interior; // inside an interior probe: no sky remainder
};

static ProbeSample sampleProbes(constant ProbeBlock& pb, const device uint* clusters, texturecube_array<float> atlas, uint cluster,
                                float3 world, float3 R, float3 N, float rough, bool wantIrradiance) {
    ProbeSample s;
    s.spec = 0.0;
    s.irr = 0.0;
    s.specW = 0.0;
    s.irrW = 0.0;
    s.interior = false;
    const int count = min(int(pb.info.x), kProbeMax);
    if (count <= 0) return s;
    uint mask = clusters[cluster];
    const float maxMip = pb.info.y;
    int used = 0;
    for (int guard = 0; guard < kProbeMax && mask != 0u && used < kProbeMaxPerPixel; ++guard) {
        const uint i = ctz(mask);
        mask &= mask - 1u;
        if (int(i) >= count) break;
        GPUProbe p = pb.probes[i];
        float3 local = (p.worldToLocal * float4(world, 1.0)).xyz;
        float w = probeInfluence(p, local);
        if (w <= 0.0) continue;
        ++used;
        if (probeFlag(p, 2)) s.interior = true;
        const uint slice = uint(p.params.x + 0.5);
        const float base = p.params.y;
        float3 dir = probeLookup(p, world, local, R);
        float a = w * (1.0 - s.specW);
        s.spec += atlas.sample(cubeSampler, dir, slice, level(base + rough * (maxMip - base))).rgb * (p.capture.w * a);
        s.specW += a;
        const int ambientMode = int(p.params.w + 0.5);
        if (wantIrradiance && ambientMode != 1) {
            float3 irr = ambientMode == 2 ? p.ambient.rgb : atlas.sample(cubeSampler, N, slice, level(maxMip)).rgb * p.capture.w;
            float ai = w * (1.0 - s.irrW);
            s.irr += irr * ai;
            s.irrW += ai;
        }
        if (s.specW > 0.999 && (!wantIrradiance || s.irrW > 0.999)) break;
    }
    return s;
}

// Probe light over the sky: the probes' weighted light plus the sky for the rest, or (interior) the
// probes renormalized to full weight.
static float3 probeOverSky(float3 probeLight, float weight, bool interior, float3 sky) {
    if (interior && weight > 1e-4) return probeLight / weight;
    return probeLight + sky * (1.0 - weight);
}

// Image-based light of a surface point: diffuse ambient (`irr`) and the prefiltered reflection
// (`spec`), both already scaled (Environment.ambient / reflections for the sky part). Used by lit
// surfaces in the main pass and by the lighting resolve, so screen-space reflections replace exactly
// what the surface added.
struct EnvLight {
    float3 irr;
    float3 spec;
};

static EnvLight environmentLight(constant FrameUniforms& f, constant ProbeBlock& pb, const device uint* probeClusters,
                                 texturecube_array<float> probeAtlas, texturecube<float> envTex, uint cluster, float3 world,
                                 float3 N, float3 R, float rough) {
    const float maxMip = f.extra.z;
    EnvLight e;
    if ((int(pb.info.z + 0.5) & 2) != 0) {  // interior capture: a constant ambient instead of the sky
        e.irr = pb.ambient.rgb;
        e.spec = pb.ambient.rgb;
    } else {
        e.irr = envTex.sample(cubeSampler, N, level(maxMip)).rgb * f.ground.w;
        e.spec = envTex.sample(cubeSampler, R, level(rough * maxMip)).rgb * f.sky.w * f.ground.w;
    }
    if (pb.info.x > 0.5) {
        ProbeSample ps = sampleProbes(pb, probeClusters, probeAtlas, cluster, world, R, N, rough, true);
        e.irr = probeOverSky(ps.irr, ps.irrW, ps.interior, e.irr);
        e.spec = probeOverSky(ps.spec * f.sky.w, ps.specW, ps.interior, e.spec);
    }
    return e;
}

// Radiance along one direction (clearcoat and water reflections, missed GI rays): the probes over the
// caller's sky value `sky`, the probe part scaled by `scale` (Environment.reflections for reflections).
static float3 probeRadiance(constant ProbeBlock& pb, const device uint* probeClusters, texturecube_array<float> probeAtlas,
                            uint cluster, float3 world, float3 R, float rough, float3 sky, float scale) {
    if (pb.info.x < 0.5) return sky;
    ProbeSample ps = sampleProbes(pb, probeClusters, probeAtlas, cluster, world, R, float3(0, 1, 0), rough, false);
    return probeOverSky(ps.spec * scale, ps.specW, ps.interior, sky);
}

// ---------------------------------------------------------------------------
// Capture: the sky behind the captured geometry (the environment's sky cube, scaled like the sky's
// image-based light so probes and the sky match where they blend).
// ---------------------------------------------------------------------------

fragment MainOut probeSkyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                  texturecube<float> skyCube [[texture(0)]]) {
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float4 nearP = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    return mainOutFlat(float4(skyCube.sample(cubeSampler, dir, level(0.0)).rgb * f.ground.w, 1.0));
}

// ---------------------------------------------------------------------------
// Filter: one atlas mip of one probe (6 faces) from the captured cube's mip chain. The probe's base
// mip is a copy (roughness 0); each further mip is GGX-prefiltered (filtered importance sampling).
// ---------------------------------------------------------------------------

struct ProbeFilterParams {
    float4 params;  // x = roughness, y = destination face size (px), z = source face size (px), w = source mip count
};

kernel void probeFilterKernel(uint3 gid [[thread_position_in_grid]], constant ProbeFilterParams& p [[buffer(0)]],
                              texturecube<float> src [[texture(0)]], texture2d_array<float, access::write> dst [[texture(1)]]) {
    const uint size = uint(p.params.y);
    if (gid.x >= size || gid.y >= size || gid.z >= 6u) return;  // bounds-checked writes
    float3 N = cubeDir(float(gid.z), (float2(gid.xy) + 0.5) / float(size));
    float rough = p.params.x;
    if (rough < 0.01) {
        dst.write(float4(src.sample(cubeSampler, N, level(0.0)).rgb, 1.0), gid.xy, gid.z);
        return;
    }
    float a = rough * rough;
    const uint COUNT = 64;
    const float maxLevel = max(p.params.w - 1.0, 0.0);
    float saTexel = 4.0 * M_PI_F / (6.0 * p.params.z * p.params.z);
    float3 sum = 0.0;
    float wsum = 0.0;
    for (uint i = 0; i < COUNT; ++i) {
        float3 H = importanceGGX(hammersley(i, COUNT), N, a);
        float3 L = normalize(2.0 * dot(N, H) * H - N);
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float pdf = D_GGX(saturate(dot(N, H)), a) * 0.25 + 1e-4;
        float mip = clamp(0.5 * log2(1.0 / (float(COUNT) * pdf) / saTexel) + 1.0, 0.0, maxLevel);
        sum += min(src.sample(cubeSampler, L, level(mip)).rgb, float3(512.0)) * NdotL;
        wsum += NdotL;
    }
    dst.write(float4(sum / max(wsum, 1e-4), 1.0), gid.xy, gid.z);
}

// ---------------------------------------------------------------------------
// Debug view `reflection_probes`: tints the final image with the color of the probe(s) lighting each
// pixel (gray = sky only), outlines every influence volume (dashed where hidden) and marks capture
// points. Drawn with alpha blending over the finished image.
// ---------------------------------------------------------------------------

struct ProbeDebugParams {
    float4 params;  // x = pixel angle (radians per px of the internal viewport), y = output height, zw = unused
};

static float3 probeDebugColor(float index) {
    const float3 kPalette[12] = {float3(0.95, 0.30, 0.25), float3(0.25, 0.65, 0.98), float3(0.35, 0.90, 0.35), float3(0.98, 0.80, 0.20),
                                 float3(0.85, 0.35, 0.95), float3(0.20, 0.90, 0.85), float3(0.98, 0.55, 0.15), float3(0.60, 0.45, 0.98),
                                 float3(0.95, 0.45, 0.65), float3(0.70, 0.95, 0.25), float3(0.40, 0.55, 0.75), float3(0.85, 0.70, 0.50)};
    int i = int(index + 0.5) % 12;
    return kPalette[max(i, 0)];
}

fragment float4 probeDebugFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                   constant ProbeBlock& volumes [[buffer(1)]], constant ProbeDebugParams& dp [[buffer(2)]],
                                   depth2d<float> depthTex [[texture(0)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float4 nearP = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float3 ro = nearP.xyz / nearP.w;
    float3 rd = normalize(farP.xyz / farP.w - ro);
    if (f.cameraForward.w < 0.5) ro = f.cameraPos.xyz;
    const bool surface = d < 1.0;
    const float3 world = surface ? reconstructWorld(f, uv, d) : ro + rd * 1e6;
    const float sceneT = surface ? dot(world - ro, rd) : 1e30;
    const int count = min(int(volumes.info.x), kProbeMax);

    // Which probes light this pixel (front to back, as the surfaces blend them; ready probes only).
    float3 tint = 0.0;
    float acc = 0.0;
    bool interior = false;
    if (surface) {
        for (int i = 0; i < count && acc < 0.999; ++i) {
            GPUProbe p = volumes.probes[i];
            if (p.params.x < -0.5) continue;  // no capture yet
            float w = probeInfluence(p, (p.worldToLocal * float4(world, 1.0)).xyz);
            if (w <= 0.0) continue;
            interior = interior || probeFlag(p, 2);
            float a = w * (1.0 - acc);
            tint += probeDebugColor(p.ambient.w) * a;
            acc += a;
        }
        if (interior && acc > 0.0) tint /= acc;
        else tint += float3(0.35) * (1.0 - acc);
    }
    float4 o = surface ? float4(tint, 0.55) : float4(0.0);

    // Volume outlines and capture points (constant pixel width).
    const float px = max(dp.params.x, 1e-5);
    for (int i = 0; i < count; ++i) {
        GPUProbe p = volumes.probes[i];
        float3 col = probeDebugColor(p.ambient.w);
        float3 lo = (p.worldToLocal * float4(ro, 1.0)).xyz;
        float3 ld = (p.worldToLocal * float4(rd, 0.0)).xyz;
        // Capture point: a dot ~5 px wide.
        float3 toC = p.capture.xyz - ro;
        float tc = dot(toC, rd);
        if (tc > 0.0 && length(toC - rd * tc) < px * tc * 3.0) {
            float hidden = tc > sceneT + 0.05 ? 0.5 : 1.0;
            o = float4(mix(col, float3(1.0), 0.6) * hidden, 1.0);
            continue;
        }
        if (probeFlag(p, 4)) {  // sphere: the silhouette ring
            float tm = dot(-lo, ld);
            float dist = length(lo + ld * tm);
            if (tm > 0.0 && abs(dist - p.extents.x) < px * tm * 1.2) {
                bool hidden = tm > sceneT;
                if (!hidden || fmod(floor(in.position.x * 0.25) + floor(in.position.y * 0.25), 2.0) < 1.0) {
                    o = float4(col * (hidden ? 0.6 : 1.0), hidden ? 0.7 : 1.0);
                }
            }
            continue;
        }
        // Box: the entry and exit points of the ray; an edge where two coordinates sit on faces.
        float3 safeD = select(ld, float3(1e-6), abs(ld) < 1e-6);
        float3 t1 = (-p.extents.xyz - lo) / safeD, t2 = (p.extents.xyz - lo) / safeD;
        float3 tn = min(t1, t2), tf = max(t1, t2);
        float tEnter = max(max(tn.x, tn.y), tn.z), tExit = min(min(tf.x, tf.y), tf.z);
        if (tExit < max(tEnter, 0.0)) continue;
        for (int k = 0; k < 2; ++k) {
            float t = k == 0 ? tEnter : tExit;
            if (t <= 0.0) continue;
            float3 h = lo + ld * t;
            float3 near = step(p.extents.xyz - abs(h), float3(px * t * 1.5));
            if (near.x + near.y + near.z < 1.5) continue;
            bool hidden = t > sceneT + 0.02;
            if (hidden && fmod(floor(in.position.x * 0.25) + floor(in.position.y * 0.25), 2.0) >= 1.0) continue;
            o = float4(col * (hidden ? 0.6 : 1.0), hidden ? 0.7 : 1.0);
            break;
        }
    }
    return o;
}
