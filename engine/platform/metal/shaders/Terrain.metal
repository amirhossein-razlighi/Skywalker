// ---------------------------------------------------------------------------
// Terrain: continuous-LOD heightfield (CDLOD). A single grid patch is instanced once per
// selected quadtree node; vertices are displaced from the height texture and morph toward
// the next coarser grid with distance, so LOD transitions never crack or pop. Up to 8
// material layers are blended by painted weights, sharpened by a height-blend, with two
// tiling scales against repetition, triplanar projection for cliffs, and wet sand/soil
// along the water line.
// ---------------------------------------------------------------------------

struct TerrainUniforms {
    float4 origin;           // xyz = world position of the terrain center, w = size (m)
    float4 grid;             // x = resolution, y = 1/resolution, z = cell size (m), w = layer count
    float4 water;            // x = water level (world y), y = wet band (m), z = selected, w = unused
    float4 layerParams[8];   // x = tiling (repeats per m), y = normal strength, z = roughness, w = flags
    float4 layerColor[8];    // rgb = tint (linear), a = metallic
};

struct TerrainNode {
    float4 node;   // xy = min corner (local m), z = size (m), w = lod level
    float4 morph;  // x = morph start distance, y = morph end distance, z = grid cells per side
};

struct TerrainOut {
    float4 position [[position]];
    float3 worldPos;
    float2 uv;        // 0..1 over the terrain
};

static float terrainHeight(texture2d<float> heightTex, float2 uv, float res) {
    float2 p = clamp(uv, 0.0, 1.0) * (res - 1.0);
    uint2 i0 = uint2(min(floor(p), float2(res - 2.0)));
    float2 t = p - float2(i0);
    float h00 = heightTex.read(i0).r, h10 = heightTex.read(i0 + uint2(1, 0)).r;
    float h01 = heightTex.read(i0 + uint2(0, 1)).r, h11 = heightTex.read(i0 + uint2(1, 1)).r;
    return mix(mix(h00, h10, t.x), mix(h01, h11, t.x), t.y);
}

vertex TerrainOut terrainVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                const device float2* grid [[buffer(0)]],
                                constant TerrainUniforms& tu [[buffer(1)]],
                                constant FrameUniforms& f [[buffer(2)]],
                                const device TerrainNode* nodes [[buffer(3)]],
                                texture2d<float> heightTex [[texture(0)]]) {
    TerrainNode n = nodes[iid];
    float2 g = grid[vid];
    float size = tu.origin.w;
    float2 local = n.node.xy + g * n.node.z;
    float2 uv = local / size + 0.5;
    float h = terrainHeight(heightTex, uv, tu.grid.x);
    float3 world = float3(tu.origin.x + local.x, tu.origin.y + h, tu.origin.z + local.y);
    // CDLOD morph: odd vertices slide onto the coarser grid as the node nears its LOD range end.
    float dist = distance(world, f.cameraPos.xyz);
    float k = saturate((dist - n.morph.x) / max(n.morph.y - n.morph.x, 1e-3));
    float cells = n.morph.z;
    float2 fracPart = fract(g * cells * 0.5) * 2.0 / cells;
    g -= fracPart * k;
    local = n.node.xy + g * n.node.z;
    uv = local / size + 0.5;
    h = terrainHeight(heightTex, uv, tu.grid.x);
    world = float3(tu.origin.x + local.x, tu.origin.y + h, tu.origin.z + local.y);
    TerrainOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.uv = uv;
    return o;
}

// Depth-only terrain for the sun shadow cascades.
vertex float4 terrainShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device float2* grid [[buffer(0)]],
                                  constant TerrainUniforms& tu [[buffer(1)]],
                                  constant float4x4& lightViewProj [[buffer(2)]],
                                  const device TerrainNode* nodes [[buffer(3)]],
                                  texture2d<float> heightTex [[texture(0)]]) {
    TerrainNode n = nodes[iid];
    float2 local = n.node.xy + grid[vid] * n.node.z;
    float2 uv = local / tu.origin.w + 0.5;
    float h = terrainHeight(heightTex, uv, tu.grid.x);
    return lightViewProj * float4(tu.origin.x + local.x, tu.origin.y + h, tu.origin.z + local.y, 1.0);
}

struct LayerSample {
    float3 albedo;
    float3 normalTS;  // tangent space (x along world X, y along world Z)
    float3 orm;
};

constexpr sampler terrainSampler(coord::normalized, filter::linear, mip_filter::linear, address::repeat, max_anisotropy(16));

static LayerSample sampleLayer(int i, float3 wp, float3 N, float dist, constant TerrainUniforms& tu,
                               texture2d<float> albedoTex, texture2d<float> normalTex, texture2d<float> ormTex) {
    float4 lp = tu.layerParams[i];
    int flags = int(lp.w + 0.5);
    bool hasA = (flags & 1) != 0, hasN = (flags & 2) != 0, hasO = (flags & 4) != 0, tri = (flags & 8) != 0;
    LayerSample s;
    s.albedo = tu.layerColor[i].rgb;
    s.normalTS = float3(0, 0, 1);
    s.orm = float3(1.0, lp.z, tu.layerColor[i].a);
    float tiling = lp.x;
    // Two scales: the detail scale up close, a larger scale far away (hides tiling).
    float farMix = smoothstep(18.0, 90.0, dist);
    if (tri) {
        float3 w = pow(abs(N), float3(6.0));
        w /= (w.x + w.y + w.z);
        float2 uvX = wp.zy * tiling, uvY = wp.xz * tiling, uvZ = wp.xy * tiling;
        if (hasA) s.albedo *= (albedoTex.sample(terrainSampler, uvX).rgb * w.x + albedoTex.sample(terrainSampler, uvY).rgb * w.y +
                               albedoTex.sample(terrainSampler, uvZ).rgb * w.z);
        if (hasO) s.orm = (ormTex.sample(terrainSampler, uvX).rgb * w.x + ormTex.sample(terrainSampler, uvY).rgb * w.y +
                           ormTex.sample(terrainSampler, uvZ).rgb * w.z) * float3(1.0, lp.z * 2.0, 1.0);
        if (hasN) s.normalTS = normalize(normalTex.sample(terrainSampler, uvY).xyz * 2.0 - 1.0);
        s.normalTS.xy *= lp.y;
        return s;
    }
    float2 uv1 = wp.xz * tiling;
    float2 uv2 = float2(wp.x * 0.7071 - wp.z * 0.7071, wp.x * 0.7071 + wp.z * 0.7071) * tiling * 0.23;
    if (hasA) {
        float3 a1 = albedoTex.sample(terrainSampler, uv1).rgb;
        float3 a2 = albedoTex.sample(terrainSampler, uv2).rgb;
        s.albedo *= mix(a1, a2, farMix * 0.65);
    }
    if (hasN) {
        float3 n1 = normalTex.sample(terrainSampler, uv1).xyz * 2.0 - 1.0;
        float3 n2 = normalTex.sample(terrainSampler, uv2).xyz * 2.0 - 1.0;
        s.normalTS = normalize(mix(n1, n2, farMix * 0.5));
        s.normalTS.xy *= lp.y;
    }
    if (hasO) s.orm = ormTex.sample(terrainSampler, uv1).rgb * float3(1.0, lp.z * 2.0, 1.0);
    return s;
}

fragment MainOut terrainFragment(TerrainOut in [[stage_in]],
                                 constant TerrainUniforms& tu [[buffer(0)]],
                                 constant FrameUniforms& f [[buffer(1)]],
                                 const device GPULight* lights [[buffer(2)]],
                                 const device uint2* clusterCells [[buffer(3)]],
                                 const device uint* clusterIndices [[buffer(4)]],
                                 texture2d<float> normalMap [[texture(0)]],
                                 depth2d<float> shadowAtlas [[texture(1)]],
                                 texture2d<float> weights0 [[texture(2)]],
                                 texture2d<float> weights1 [[texture(3)]],
                                 texture3d<float> cloudShape [[texture(4)]],
                                 texturecube<float> envTex [[texture(5)]],
                                 texture2d<float> brdfLut [[texture(6)]],
                                 texture2d<float> l0a [[texture(7)]], texture2d<float> l0n [[texture(8)]], texture2d<float> l0o [[texture(9)]],
                                 texture2d<float> l1a [[texture(10)]], texture2d<float> l1n [[texture(11)]], texture2d<float> l1o [[texture(12)]],
                                 texture2d<float> l2a [[texture(13)]], texture2d<float> l2n [[texture(14)]], texture2d<float> l2o [[texture(15)]],
                                 texture2d<float> l3a [[texture(16)]], texture2d<float> l3n [[texture(17)]], texture2d<float> l3o [[texture(18)]],
                                 texture2d<float> l4a [[texture(19)]], texture2d<float> l4n [[texture(20)]], texture2d<float> l4o [[texture(21)]],
                                 texture2d<float> l5a [[texture(22)]], texture2d<float> l5n [[texture(23)]], texture2d<float> l5o [[texture(24)]],
                                 texture2d<float> l6a [[texture(25)]], texture2d<float> l6n [[texture(26)]], texture2d<float> l6o [[texture(27)]],
                                 texture2d<float> l7a [[texture(28)]], texture2d<float> l7n [[texture(29)]], texture2d<float> l7o [[texture(30)]]) {
    float3 wp = in.worldPos;
    float3 V = normalize(f.cameraPos.xyz - wp);
    if (f.cameraForward.w > 0.5) V = -f.cameraForward.xyz;
    float dist = distance(f.cameraPos.xyz, wp);
    // Geometric normal from the precomputed normal map (xz packed, y reconstructed).
    float2 nxz = normalMap.sample(linearClamp, in.uv).xy * 2.0 - 1.0;
    float3 Ngeo = normalize(float3(nxz.x, sqrt(saturate(1.0 - dot(nxz, nxz))), nxz.y));
    float4 w0 = weights0.sample(linearClamp, in.uv), w1 = weights1.sample(linearClamp, in.uv);
    float weights[8] = {w0.x, w0.y, w0.z, w0.w, w1.x, w1.y, w1.z, w1.w};
    int layerCount = int(tu.grid.w + 0.5);

    LayerSample acc;
    acc.albedo = 0.0;
    acc.normalTS = 0.0;
    acc.orm = 0.0;
    // Height blend: each layer's "height" (from its albedo + AO) competes with its weight,
    // so sand fills the gaps between stones instead of fading through them.
    float hb[8];
    LayerSample ls[8];
    float best = -1.0;
    for (int i = 0; i < 8; ++i) {
        hb[i] = -1.0;
        if (i >= layerCount || weights[i] < 0.004) continue;
        LayerSample s;
        switch (i) {
            case 0: s = sampleLayer(0, wp, Ngeo, dist, tu, l0a, l0n, l0o); break;
            case 1: s = sampleLayer(1, wp, Ngeo, dist, tu, l1a, l1n, l1o); break;
            case 2: s = sampleLayer(2, wp, Ngeo, dist, tu, l2a, l2n, l2o); break;
            case 3: s = sampleLayer(3, wp, Ngeo, dist, tu, l3a, l3n, l3o); break;
            case 4: s = sampleLayer(4, wp, Ngeo, dist, tu, l4a, l4n, l4o); break;
            case 5: s = sampleLayer(5, wp, Ngeo, dist, tu, l5a, l5n, l5o); break;
            case 6: s = sampleLayer(6, wp, Ngeo, dist, tu, l6a, l6n, l6o); break;
            default: s = sampleLayer(7, wp, Ngeo, dist, tu, l7a, l7n, l7o); break;
        }
        ls[i] = s;
        float height = dot(s.albedo, float3(0.3, 0.5, 0.2)) * 0.5 + s.orm.x * 0.5;
        hb[i] = weights[i] + height * 0.35;
        best = max(best, hb[i]);
    }
    float wsum = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (hb[i] < 0.0) continue;
        float w = max(hb[i] - best + 0.12, 0.0);
        acc.albedo += ls[i].albedo * w;
        acc.normalTS += ls[i].normalTS * w;
        acc.orm += ls[i].orm * w;
        wsum += w;
    }
    wsum = max(wsum, 1e-4);
    acc.albedo /= wsum;
    acc.orm /= wsum;
    float3 nTS = normalize(acc.normalTS / wsum + float3(0, 0, 1e-4));
    // Tangent frame on the heightfield: T along +X, B along +Z, bent onto the surface.
    float3 T = normalize(float3(1, 0, 0) - Ngeo * Ngeo.x);
    float3 B = normalize(cross(T, Ngeo)) * -1.0;
    float3 N = normalize(T * nTS.x + B * nTS.y + Ngeo * nTS.z);

    SurfaceData s;
    s.albedo = acc.albedo;
    s.alpha = 1.0;
    s.ao = acc.orm.x;
    s.roughness = clamp(acc.orm.y, 0.04, 1.0);
    s.metallic = acc.orm.z;
    // Wet shoreline: darker, glossy, smoother where waves run up (porous sand soaks up water).
    float wl = tu.water.x;
    if (wl > -99999.0) {
        float n = valueNoise(wp.xz * 0.35) * 0.6 + valueNoise(wp.xz * 1.7) * 0.4;
        float wet = 1.0 - smoothstep(wl, wl + tu.water.y * (0.7 + 0.6 * n), wp.y);
        float porous = saturate(1.0 - s.metallic);
        s.albedo *= mix(1.0, 0.52, wet * porous);
        s.roughness = mix(s.roughness, 0.07, wet * 0.92);
        N = normalize(mix(N, Ngeo, wet * 0.8));
    }
    s.clearcoat = 0.0;
    s.subsurface = 0.0;
    float3 color = shadeSurface(s, Ngeo, wp, in.position.xy, V, false, 0.0, f, lights, clusterCells, clusterIndices, shadowAtlas,
                                envTex, brdfLut, cloudShape);
    if (tu.water.z > 0.5) color = mix(color, float3(1.0, 0.5, 0.1), 0.15);  // selection tint
    color = applyFog(color, wp, V, f);
    return mainOut(float4(color, 1.0), s.albedo, s.ao, N, s.roughness, s.metallic);
}

// ---------------------------------------------------------------------------
// Instanced foliage, GPU-driven. Per chunk, `foliageCullKernel` tests every instance against
// the camera and the 4 sun cascades and picks a distance band (mesh LOD) and/or the impostor;
// it writes compact instance lists plus indirect draw arguments. Mesh instances bend in the
// wind (gusts travel across the field) and thin out toward the cull distance; shading reuses
// meshFragment (vertex color carries the instance tint). Far instances are octahedral
// impostors (render/Impostor.h): camera-facing cards that blend the 3 nearest baked views,
// correct parallax with the baked depth and write a real G-buffer and depth.
// ---------------------------------------------------------------------------

struct FoliageInstanceGpu {
    float4 row0, row1, row2;
    float4 params;  // x = tint, y = wind phase, z = height (m), w = fade random
};

struct FoliageUniforms {
    float4 wind;    // xy = direction (xz), z = speed (m/s), w = layer bend strength
    float4 params;  // x = cull distance, y = mesh height (m), z = debug tint (impostor view), w = time (s)
    float4 fade;    // x = impostor transition distance (0 = none), y = crossfade width (m)
    float4x4 part;  // the part's transform inside a multi-part model (trunk, leaves...)
};

static float3x3 instanceBasis(FoliageInstanceGpu inst) {
    return float3x3(float3(inst.row0.x, inst.row1.x, inst.row2.x), float3(inst.row0.y, inst.row1.y, inst.row2.y),
                    float3(inst.row0.z, inst.row1.z, inst.row2.z));
}

static float3 instanceOrigin(FoliageInstanceGpu inst) { return float3(inst.row0.w, inst.row1.w, inst.row2.w); }

static float3 instanceTint(float3 c, float tint) {
    c *= 1.0 + tint * 0.22;
    return mix(c, c * float3(1.18, 1.06, 0.62), saturate(tint) * 0.6);  // some blades yellow, some lush
}

// Fraction of an instance drawn as its impostor at camera distance `dist` (0 = mesh only).
static float impostorWeight(float dist, float4 fade) {
    return fade.x > 0.0 ? saturate((dist - (fade.x - fade.y)) / max(fade.y, 1e-3)) : 0.0;
}

static float3 foliageWorld(float3 p, FoliageInstanceGpu inst, constant FoliageUniforms& fu, thread float& h01) {
    float3 base = instanceOrigin(inst);
    float3 world = float3(dot(inst.row0.xyz, p), dot(inst.row1.xyz, p), dot(inst.row2.xyz, p)) + base;
    h01 = saturate(p.y / max(fu.params.y, 1e-3));
    float bend = h01 * h01;
    float t = fu.params.w;
    float2 dir = fu.wind.xy;
    float speed = fu.wind.z;
    float travel = dot(base.xz, dir);
    float gust = 0.55 + 0.45 * sin(t * 0.55 - travel * 0.045) * sin(t * 0.9 - travel * 0.11 + 1.3);
    float sway = sin(t * (1.7 + speed * 0.12) + inst.params.y * 6.2831853 - travel * 0.35);
    float flutter = sin(t * 7.3 + inst.params.y * 40.0 + p.y * 12.0) * 0.15;
    float amp = fu.wind.w * (0.06 + speed * 0.035) * gust * inst.params.z;
    float2 off = dir * amp * bend * (0.65 + 0.35 * sway) + float2(-dir.y, dir.x) * amp * bend * flutter;
    world.xz += off;
    world.y -= length(off) * 0.35 * bend;  // keep the blade length roughly constant
    return world;
}

vertex MeshOut foliageVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                             const device Vertex* verts [[buffer(0)]],
                             constant DrawUniforms& d [[buffer(1)]],
                             constant FrameUniforms& f [[buffer(2)]],
                             const device FoliageInstanceGpu* instances [[buffer(3)]],
                             constant FoliageUniforms& fu [[buffer(4)]],
                             const device uint* visible [[buffer(5)]]) {
    Vertex v = verts[vid];
    FoliageInstanceGpu inst = instances[visible[iid]];
    float3 base = instanceOrigin(inst);
    MeshOut o;
    // Thin out smoothly toward the cull distance (random per instance), then vanish.
    float dist = distance(base, f.cameraPos.xyz);
    float limit = fu.params.x * (0.72 + 0.28 * inst.params.w);
    float shrink = fu.fade.x > 0.0 ? 1.0 : 1.0 - smoothstep(limit * 0.85, limit, dist);  // impostors thin out themselves
    float h01;
    float3 local = (fu.part * float4(float3(v.position), 1.0)).xyz;
    float3 world = foliageWorld(local * float3(1.0, shrink, 1.0), inst, fu, h01);
    float3 ln = (fu.part * float4(float3(v.normal), 0.0)).xyz;
    float3 n = float3(dot(inst.row0.xyz, ln), dot(inst.row1.xyz, ln), dot(inst.row2.xyz, ln));
    o.position = shrink <= 0.0 ? float4(0.0, 0.0, -2.0, 1.0) : f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.normal = normalize(n);
    o.uv = float2(v.uv);
    o.color = float4(instanceTint(float4(v.color).rgb, inst.params.x), float4(v.color).a);
    if (fu.params.z > 0.5) o.color.rgb = mix(o.color.rgb, float3(0.15, 0.95, 0.1), 0.7);  // debug view: meshes green
    o.fade = impostorWeight(dist, fu.fade);  // crossfading into the impostor
    return o;
}

vertex float4 foliageShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device Vertex* verts [[buffer(0)]],
                                  constant float4x4& lightViewProj [[buffer(2)]],
                                  const device FoliageInstanceGpu* instances [[buffer(3)]],
                                  constant FoliageUniforms& fu [[buffer(4)]],
                                  const device uint* visible [[buffer(5)]]) {
    float h01;
    float3 world = foliageWorld((fu.part * float4(float3(verts[vid].position), 1.0)).xyz, instances[visible[iid]], fu, h01);
    return lightViewProj * float4(world, 1.0);
}

// Alpha-tested foliage (leaf cards, grass cards) casts shadows only where it is solid.
vertex ShadowAlphaOut foliageShadowAlphaVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                               const device Vertex* verts [[buffer(0)]],
                                               constant DrawUniforms& d [[buffer(1)]],
                                               constant float4x4& lightViewProj [[buffer(2)]],
                                               const device FoliageInstanceGpu* instances [[buffer(3)]],
                                               constant FoliageUniforms& fu [[buffer(4)]],
                                               const device uint* visible [[buffer(5)]]) {
    float h01;
    float3 world = foliageWorld((fu.part * float4(float3(verts[vid].position), 1.0)).xyz, instances[visible[iid]], fu, h01);
    ShadowAlphaOut o;
    o.position = lightViewProj * float4(world, 1.0);
    o.uv = float2(verts[vid].uv) * d.material2.xy;
    return o;
}

// --- GPU culling and LOD selection ------------------------------------------------------------

constant uint kFoliageViews = 5;  // camera + 4 sun cascades
constant uint kFoliageBins = 5;   // 4 mesh distance bands + the impostor

struct FoliageCullParams {
    float4 planes[30];  // [view][6] normalized frustum planes (view 0 = camera, 1..4 = sun cascades)
    float4 eye;         // xyz = camera position, w = impostor transition distance (0 = none)
    float4 params;      // x = crossfade width (m), y = cull distance, z = bounding radius (model units),
                        // w = shadow-casting camera distance (0 = no shadows)
    float4 center;      // xyz = model-space bounds center, w = first impostor-only cascade (4 = none)
    float4 bands;       // xyz = far edges (m) of mesh bands 0..2; band 3 runs to the transition
    uint4 counts;       // x = instances, y = parts, z = views, w = list stride per view (2 x instances)
    uint2 lods[64];     // [table: camera, shadows][part (8)][band (4)] -> (index start, index count)
};

// Which bins (bit b: mesh band b, bit 4: impostor) an instance lands in for one view.
static uint foliageBinsFor(constant FoliageCullParams& p, uint view, float d, float3 center, float radius) {
    for (uint k = 0; k < 6; ++k) {
        float4 pl = p.planes[view * 6 + k];
        if (dot(pl.xyz, center) + pl.w < -radius) return 0u;
    }
    float D = p.eye.w;
    uint band = d < p.bands.x ? 0u : (d < p.bands.y ? 1u : (d < p.bands.z ? 2u : 3u));
    if (view == 0) {
        if (D <= 0.0) return 1u << band;
        float t = saturate((d - (D - p.params.x)) / max(p.params.x, 1e-3));
        return (t < 1.0 ? (1u << band) : 0u) | (t > 0.0 ? 16u : 0u);
    }
    if (d > p.params.w) return 0u;  // beyond the shadow-casting distance
    // Shadows switch without a crossfade; far cascades take impostors only.
    if (D > 0.0 && (d >= D || float(view - 1) >= p.center.w)) return 16u;
    return 1u << band;
}

// One threadgroup per chunk: count each bin, prefix-sum the counts into list offsets, write the
// indirect arguments, then scatter instance indices into the lists.
kernel void foliageCullKernel(const device FoliageInstanceGpu* instances [[buffer(0)]],
                              constant FoliageCullParams& p [[buffer(1)]],
                              device uint* lists [[buffer(2)]],
                              device uint* args [[buffer(3)]],
                              device atomic_uint* stats [[buffer(4)]],
                              uint tid [[thread_position_in_threadgroup]],
                              uint tcount [[threads_per_threadgroup]]) {
    threadgroup atomic_uint counters[kFoliageViews * kFoliageBins];
    threadgroup uint offsets[kFoliageViews * kFoliageBins];
    const uint n = p.counts.x, views = min(p.counts.z, kFoliageViews), stride = p.counts.w;
    if (tid < kFoliageViews * kFoliageBins) atomic_store_explicit(&counters[tid], 0u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint pass = 0; pass < 2; ++pass) {
        for (uint i = tid; i < n; i += tcount) {
            FoliageInstanceGpu inst = instances[i];
            float3 T = instanceOrigin(inst);
            float d = distance(T, p.eye.xyz);
            if (d > p.params.y * (0.72 + 0.28 * inst.params.w)) continue;  // thinned out (see foliageVertex)
            float3x3 M = instanceBasis(inst);
            float3 C = M * p.center.xyz + T;
            float r = p.params.z * length(M[0]);
            for (uint v = 0; v < views; ++v) {
                uint mask = foliageBinsFor(p, v, d, C, r);
                for (uint b = 0; b < kFoliageBins; ++b) {
                    if (!(mask & (1u << b))) continue;
                    uint slot = atomic_fetch_add_explicit(&counters[v * kFoliageBins + b], 1u, memory_order_relaxed);
                    uint at = offsets[v * kFoliageBins + b] + slot;
                    if (pass == 1 && at < stride) lists[v * stride + at] = i;  // bounds-checked
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (pass > 0) break;
        if (tid == 0) {
            const uint parts = min(p.counts.y, 8u), viewArgs = parts * 20u + 4u;
            float kiloTris = 0.0, shadowKiloTris = 0.0;
            uint meshInstances = 0, impostors = 0, shadowImpostors = 0;
            for (uint v = 0; v < views; ++v) {
                uint off = 0;
                for (uint b = 0; b < kFoliageBins; ++b) {
                    offsets[v * kFoliageBins + b] = off;
                    off += atomic_load_explicit(&counters[v * kFoliageBins + b], memory_order_relaxed);
                }
                const uint table = v == 0 ? 0u : 1u;
                device uint* a = args + v * viewArgs;
                for (uint part = 0; part < parts; ++part) {
                    for (uint b = 0; b < 4; ++b) {
                        uint2 lod = p.lods[table * 32u + part * 4u + b];
                        uint count = min(atomic_load_explicit(&counters[v * kFoliageBins + b], memory_order_relaxed), n);
                        device uint* x = a + (part * 4u + b) * 5u;  // MTLDrawIndexedPrimitivesIndirectArguments
                        x[0] = lod.y;
                        x[1] = count;
                        x[2] = lod.x;
                        x[3] = 0u;
                        x[4] = offsets[v * kFoliageBins + b];
                        float k = float(count) * float(lod.y / 3u) / 1024.0;
                        if (v == 0) kiloTris += k; else shadowKiloTris += k;
                        if (v == 0 && part == 0) meshInstances += count;
                    }
                }
                uint imp = min(atomic_load_explicit(&counters[v * kFoliageBins + 4], memory_order_relaxed), n);
                device uint* x = a + parts * 20u;  // MTLDrawPrimitivesIndirectArguments (a 4-vertex strip)
                x[0] = 4u;
                x[1] = imp;
                x[2] = 0u;
                x[3] = offsets[v * kFoliageBins + 4];
                if (v == 0) impostors += imp; else shadowImpostors += imp;
            }
            atomic_fetch_add_explicit(&stats[0], meshInstances, memory_order_relaxed);
            atomic_fetch_add_explicit(&stats[1], impostors, memory_order_relaxed);
            atomic_fetch_add_explicit(&stats[2], uint(kiloTris + 0.5), memory_order_relaxed);
            atomic_fetch_add_explicit(&stats[3], uint(shadowKiloTris + 0.5), memory_order_relaxed);
            atomic_fetch_add_explicit(&stats[4], shadowImpostors, memory_order_relaxed);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (tid < kFoliageViews * kFoliageBins) atomic_store_explicit(&counters[tid], 0u, memory_order_relaxed);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
}

// --- Octahedral impostors -------------------------------------------------------------------
// Direction math mirrors render/Impostor.cpp exactly (encodings, frame directions and bases,
// 3-frame blend), so the runtime samples what the bake captured.

static float2 impHemiOctEncode(float3 d) {
    d.y = max(d.y, 0.0);
    float s = abs(d.x) + d.y + abs(d.z);
    if (s < 1e-12) return float2(0.0);
    float3 n = d / s;
    return float2(n.x + n.z, n.x - n.z);
}

static float3 impHemiOctDecode(float2 e) {
    float qx = (e.x + e.y) * 0.5, qz = (e.x - e.y) * 0.5;
    return normalize(float3(qx, 1.0 - abs(qx) - abs(qz), qz));
}

static float2 impOctEncode(float3 d) {
    float s = abs(d.x) + abs(d.y) + abs(d.z);
    if (s < 1e-12) return float2(0.0);
    float3 n = d / s;
    float2 e = float2(n.x, n.z);
    if (n.y < 0.0) e = float2((1.0 - abs(n.z)) * (n.x >= 0.0 ? 1.0 : -1.0), (1.0 - abs(n.x)) * (n.z >= 0.0 ? 1.0 : -1.0));
    return e;
}

static float3 impOctDecode(float2 e) {
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    float t = max(-n.y, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.z += n.z >= 0.0 ? -t : t;
    return normalize(n);
}

static float3 impFrameDirection(uint fx, uint fy, float frames, bool hemi) {
    float n = max(frames - 1.0, 1.0);
    float2 e = float2(float(fx), float(fy)) / n * 2.0 - 1.0;
    return hemi ? impHemiOctDecode(e) : impOctDecode(e);
}

struct ImpBasis {
    float3 right, up, forward;
};

static ImpBasis impFrameBasis(float3 fwd) {
    ImpBasis b;
    b.forward = normalize(fwd);
    float3 ref = abs(b.forward.y) > 0.999 ? float3(0, 0, -1) : float3(0, 1, 0);
    b.right = normalize(cross(ref, b.forward));
    b.up = cross(b.forward, b.right);
    return b;
}

// The 3 frames around a view direction (packed as y * frames + x) and their weights.
static void impBlend(float3 viewDir, float frames, bool hemi, thread uint3& idx, thread float3& w) {
    float2 e = hemi ? impHemiOctEncode(viewDir) : impOctEncode(viewDir);
    float n = max(frames - 1.0, 1.0);
    float2 g = saturate(e * 0.5 + 0.5) * n;
    float2 cell = clamp(floor(g), float2(0.0), float2(max(frames - 2.0, 0.0)));
    float2 fr = saturate(g - cell);
    uint F = uint(frames), cx = uint(cell.x), cy = uint(cell.y);
    if (fr.x + fr.y <= 1.0) {
        idx = uint3(cy * F + cx, cy * F + cx + 1, (cy + 1) * F + cx);
        w = float3(1.0 - fr.x - fr.y, fr.x, fr.y);
    } else {
        idx = uint3((cy + 1) * F + cx + 1, cy * F + cx + 1, (cy + 1) * F + cx);
        w = float3(fr.x + fr.y - 1.0, 1.0 - fr.y, 1.0 - fr.x);
    }
    w = max(w, float3(0.0));
}

struct ImpostorUniforms {
    float4 center;   // xyz = model-space bounds center, w = bounding radius (model units)
    float4 grid;     // x = frames per side, y = 1 / frames, z = hemi (1) / full sphere (0), w = 1 / tile texels
    float4 surface;  // x = roughness, y = debug tint, z = shadow depth bias (m), w = unused
    float4 light;    // xyz = direction sunlight travels (shadow cards)
};

constexpr sampler impostorSampler(coord::normalized, filter::linear, mip_filter::linear, address::clamp_to_edge);

struct ImpostorSample {
    float alpha;
    float3 albedo;
    float3 normal;    // model space
    float3 position;  // model space, on the reconstructed surface
    float subsurface;
};

// Atlas coordinates of a point on a frame's image plane (relative to the model center).
static float2 impAtlasUV(float3 x, ImpBasis b, float R, uint fx, uint fy, float invFrames, float inset, thread bool& inside) {
    float2 uv = float2(dot(x, b.right), dot(x, b.up)) / (2.0 * R) + 0.5;
    inside = all(uv >= float2(0.0)) && all(uv <= float2(1.0));
    uv = clamp(uv, float2(inset), float2(1.0 - inset));
    return (float2(float(fx), float(fy)) + float2(uv.x, 1.0 - uv.y)) * invFrames;
}

// A ray (model space: through p, direction r away from the viewer) through the 3 blended frames,
// with depth-based parallax (two fixed-point steps per frame).
static ImpostorSample sampleImpostor(texture2d<float> atlasA, texture2d<float> atlasB, constant ImpostorUniforms& ic, float3 p,
                                     float3 r, uint3 frames, float3 weights) {
    ImpostorSample o;
    o.alpha = 0.0;
    o.albedo = 0.0;
    o.normal = 0.0;
    o.position = 0.0;
    o.subsurface = 0.0;
    const float3 c = ic.center.xyz;
    const float R = ic.center.w, F = ic.grid.x, invF = ic.grid.y;
    const uint Fi = uint(F);
    const bool hemi = ic.grid.z > 0.5;
    const float inset = 0.5 * ic.grid.w;
    float acc = 0.0;
    for (int k = 0; k < 3; ++k) {
        uint fx = frames[k] % Fi, fy = frames[k] / Fi;
        ImpBasis b = impFrameBasis(impFrameDirection(fx, fy, F, hemi));
        float denom = min(dot(r, b.forward), -1e-3);
        float3 q = p + r * (dot(c - p, b.forward) / denom) - c;  // on the frame plane through the center
        bool inside;
        float2 uv0 = impAtlasUV(q, b, R, fx, fy, invF, inset, inside);
        float2 gx = dfdx(uv0), gy = dfdy(uv0);  // un-displaced gradients: stable mip selection
        if (weights[k] <= 1e-3) continue;
        float h = (atlasB.sample(impostorSampler, uv0, gradient2d(gx, gy)).z * 2.0 - 1.0) * R;
        float2 uv1 = impAtlasUV(q + r * (h / denom), b, R, fx, fy, invF, inset, inside);
        h = (atlasB.sample(impostorSampler, uv1, gradient2d(gx, gy)).z * 2.0 - 1.0) * R;
        float3 q2 = q + r * (h / denom);  // the ray point at the baked depth
        float2 uv2 = impAtlasUV(q2, b, R, fx, fy, invF, inset, inside);
        float4 A = atlasA.sample(impostorSampler, uv2, gradient2d(gx, gy));
        float4 B = atlasB.sample(impostorSampler, uv2, gradient2d(gx, gy));
        float wa = weights[k] * A.a * (inside ? 1.0 : 0.0);
        o.alpha += wa;
        o.albedo += A.rgb * wa;
        o.normal += octDecode(B.xy * 2.0 - 1.0) * wa;
        o.position += (c + q2) * wa;
        o.subsurface += B.w * wa;
        acc += wa;
    }
    if (acc > 1e-5) {
        o.albedo /= acc;
        o.position /= acc;
        o.subsurface /= acc;
        o.normal = normalize(o.normal + float3(0.0, 1e-5, 0.0));
    } else {
        o.normal = float3(0.0, 1.0, 0.0);
        o.position = p;
    }
    return o;
}

struct ImpostorOut {
    float4 position [[position]];
    float3 modelPos;                 // on the card, model space
    float3 eyeModel [[flat]];        // camera position (perspective) or view direction (orthographic), model space
    uint inst [[flat]];
    uint3 frames [[flat]];
    float3 weights [[flat]];
    float fade [[flat]];             // fraction of pixels kept (crossfade with the mesh, cull-distance thinning)
};

vertex ImpostorOut impostorVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  constant FrameUniforms& f [[buffer(2)]],
                                  const device FoliageInstanceGpu* instances [[buffer(3)]],
                                  constant FoliageUniforms& fu [[buffer(4)]],
                                  const device uint* visible [[buffer(5)]],
                                  constant ImpostorUniforms& ic [[buffer(6)]]) {
    const uint idx = visible[iid];
    FoliageInstanceGpu inst = instances[idx];
    float3x3 M = instanceBasis(inst);
    float3 T = instanceOrigin(inst);
    float s2 = max(dot(M[0], M[0]), 1e-8);
    float3x3 Minv = transpose(M) * (1.0 / s2);  // uniform scale x rotation
    float3 C = M * ic.center.xyz + T;
    float R = ic.center.w * sqrt(s2);
    bool ortho = f.cameraForward.w > 0.5;
    float3 toCam = ortho ? -f.cameraForward.xyz : normalize(f.cameraPos.xyz - C);
    float3 ref = abs(toCam.y) > 0.999 ? float3(0, 0, -1) : float3(0, 1, 0);
    float3 right = normalize(cross(ref, toCam)), up = cross(toCam, right);
    float2 corner = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    // A card tangent to the bounding sphere on the camera side covers its silhouette, and every
    // reconstructed surface point lies behind it (conservative depth).
    float3 P = C + toCam * R + (right * corner.x + up * corner.y) * R;
    ImpostorOut o;
    o.position = f.viewProj * float4(P, 1.0);
    o.modelPos = Minv * (P - T);
    o.eyeModel = ortho ? normalize(Minv * f.cameraForward.xyz) : Minv * (f.cameraPos.xyz - T);
    o.inst = idx;
    float3 viewDir = ortho ? -o.eyeModel : normalize(o.eyeModel - ic.center.xyz);
    impBlend(viewDir, ic.grid.x, ic.grid.z > 0.5, o.frames, o.weights);
    float dist = distance(T, f.cameraPos.xyz);
    float limit = fu.params.x * (0.72 + 0.28 * inst.params.w);
    o.fade = min(impostorWeight(dist, fu.fade), 1.0 - smoothstep(limit * 0.85, limit, dist));
    return o;
}

struct ImpostorFragmentOut {
    float4 color [[color(0)]];
    float4 gbufA [[color(1)]];
    float4 gbufB [[color(2)]];
    float depth [[depth(greater)]];
};

fragment ImpostorFragmentOut impostorFragment(ImpostorOut in [[stage_in]],
                                              constant ImpostorUniforms& ic [[buffer(0)]],
                                              constant FrameUniforms& f [[buffer(1)]],
                                              const device GPULight* lights [[buffer(2)]],
                                              const device uint2* clusterCells [[buffer(3)]],
                                              const device uint* clusterIndices [[buffer(4)]],
                                              const device FoliageInstanceGpu* instances [[buffer(5)]],
                                              texture2d<float> atlasA [[texture(0)]],
                                              depth2d<float> shadowAtlas [[texture(1)]],
                                              texture2d<float> atlasB [[texture(2)]],
                                              texturecube<float> envTex [[texture(5)]],
                                              texture2d<float> brdfLut [[texture(6)]],
                                              texture3d<float> cloudShape [[texture(7)]]) {
    const bool ortho = f.cameraForward.w > 0.5;
    const float3 ray = ortho ? in.eyeModel : normalize(in.modelPos - in.eyeModel);
    ImpostorSample smp = sampleImpostor(atlasA, atlasB, ic, in.modelPos, ray, in.frames, in.weights);
    // Alpha test sharpened to a ~1 px ramp: alpha-to-coverage antialiases the silhouette.
    float alpha = saturate((smp.alpha - 0.5) / max(fwidth(smp.alpha), 1e-4) + 0.5);
    FoliageInstanceGpu inst = instances[in.inst];
    float3x3 M = instanceBasis(inst);
    float3 worldPos = M * smp.position + instanceOrigin(inst);
    float3 N = normalize(M * smp.normal);
    float3 dn = fwidth(N);  // specular anti-aliasing (as meshFragment)
    if (alpha <= 0.0 || ditherNoise(in.position.xy, f.temporal) >= in.fade) discard_fragment();

    SurfaceData s;
    s.albedo = instanceTint(smp.albedo, inst.params.x);
    if (ic.surface.y > 0.5) s.albedo = mix(s.albedo, float3(0.95, 0.1, 0.85), 0.7);  // debug view: impostors magenta
    s.alpha = alpha;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.clearcoat = 0.0;
    s.subsurface = smp.subsurface;
    s.N = N;
    float a = ic.surface.x * ic.surface.x;
    a = sqrt(a * a + min(0.5 * dot(dn, dn), 0.18));
    s.roughness = clamp(sqrt(a), 0.045, 1.0);
    float3 V = ortho ? -f.cameraForward.xyz : normalize(f.cameraPos.xyz - worldPos);
    float3 color = shadeSurface(s, N, worldPos, in.position.xy, V, false, 0.0, f, lights, clusterCells, clusterIndices, shadowAtlas,
                                envTex, brdfLut, cloudShape);
    color = applyFog(color, worldPos, V, f);
    MainOut m = mainOut(float4(color, alpha), s.albedo, 1.0, N, s.roughness, 0.0);
    ImpostorFragmentOut o;
    o.color = m.color;
    o.gbufA = m.gbufA;
    o.gbufB = m.gbufB;
    float4 clip = f.viewProj * float4(worldPos, 1.0);  // pixel depth offset: real depth for SSAO and intersections
    o.depth = max(clip.z / clip.w, in.position.z);
    return o;
}

// Shadow casting: a card facing the sun, sampled from the frames nearest to the light direction.
struct ImpostorShadowOut {
    float4 position [[position]];
    float3 modelPos;
    float3 ray [[flat]];  // sunlight direction, model space
    uint inst [[flat]];
    uint3 frames [[flat]];
    float3 weights [[flat]];
};

vertex ImpostorShadowOut impostorShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                              constant float4x4& lightViewProj [[buffer(2)]],
                                              const device FoliageInstanceGpu* instances [[buffer(3)]],
                                              const device uint* visible [[buffer(5)]],
                                              constant ImpostorUniforms& ic [[buffer(6)]]) {
    const uint idx = visible[iid];
    FoliageInstanceGpu inst = instances[idx];
    float3x3 M = instanceBasis(inst);
    float3 T = instanceOrigin(inst);
    float s2 = max(dot(M[0], M[0]), 1e-8);
    float3x3 Minv = transpose(M) * (1.0 / s2);
    float3 C = M * ic.center.xyz + T;
    float R = ic.center.w * sqrt(s2);
    float3 toLight = -ic.light.xyz;
    float3 ref = abs(toLight.y) > 0.999 ? float3(0, 0, -1) : float3(0, 1, 0);
    float3 right = normalize(cross(ref, toLight)), up = cross(toLight, right);
    float2 corner = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    float3 P = C + toLight * R + (right * corner.x + up * corner.y) * R;
    ImpostorShadowOut o;
    o.position = lightViewProj * float4(P, 1.0);
    o.modelPos = Minv * (P - T);
    o.ray = normalize(Minv * ic.light.xyz);
    o.inst = idx;
    impBlend(-o.ray, ic.grid.x, ic.grid.z > 0.5, o.frames, o.weights);
    return o;
}

struct ImpostorShadowDepth {
    float depth [[depth(greater)]];
};

fragment ImpostorShadowDepth impostorShadowFragment(ImpostorShadowOut in [[stage_in]],
                                                    constant ImpostorUniforms& ic [[buffer(0)]],
                                                    constant float4x4& lightViewProj [[buffer(1)]],
                                                    const device FoliageInstanceGpu* instances [[buffer(5)]],
                                                    texture2d<float> atlasA [[texture(0)]],
                                                    texture2d<float> atlasB [[texture(2)]]) {
    ImpostorSample smp = sampleImpostor(atlasA, atlasB, ic, in.modelPos, in.ray, in.frames, in.weights);
    if (smp.alpha < 0.5) discard_fragment();
    FoliageInstanceGpu inst = instances[in.inst];
    // Depth of the reconstructed surface, pushed slightly away from the light (shader-written
    // depth gets no rasterizer slope bias).
    float3 worldPos = instanceBasis(inst) * smp.position + instanceOrigin(inst) + ic.light.xyz * ic.surface.z;
    ImpostorShadowDepth o;
    o.depth = max((lightViewProj * float4(worldPos, 1.0)).z, in.position.z);
    return o;
}

// --- Impostor bake -------------------------------------------------------------------------------
// One orthographic view per atlas frame. Shading goes through evaluateMaterial (the exact
// material path of the meshes); the alpha test runs per MSAA sample so leaf edges resolve to
// true coverage. Outputs: albedo + coverage, model-space normal + depth + subsurface, roughness.

struct ImpostorBakeUniforms {
    float4 center;   // xyz = model-space bounds center, w = bounding radius
    float4 right, up, forward;  // capture basis of the frame (forward points toward the camera)
};

struct ImpostorBakeOut {
    float4 position [[position]];
    float3 modelPos [[sample_perspective]];
    float3 normal [[sample_perspective]];
    float2 uv [[sample_perspective]];
    float4 color [[sample_perspective]];
};

vertex ImpostorBakeOut impostorBakeVertex(uint vid [[vertex_id]],
                                          const device Vertex* verts [[buffer(0)]],
                                          constant DrawUniforms& d [[buffer(1)]],
                                          constant ImpostorBakeUniforms& b [[buffer(2)]]) {
    Vertex v = verts[vid];
    float3 p = (d.model * float4(float3(v.position), 1.0)).xyz;
    float3 q = p - b.center.xyz;
    float R = b.center.w;
    ImpostorBakeOut o;
    o.position = float4(dot(q, b.right.xyz) / R, dot(q, b.up.xyz) / R, 0.5 - dot(q, b.forward.xyz) / (2.0 * R), 1.0);
    o.modelPos = p;
    o.normal = (d.normalMatrix * float4(float3(v.normal), 0.0)).xyz;
    o.uv = float2(v.uv);
    o.color = float4(v.color);
    return o;
}

struct ImpostorBakeTargets {
    float4 albedo [[color(0)]];
    float4 normal [[color(1)]];
    float roughness [[color(2)]];
};

fragment ImpostorBakeTargets impostorBakeFragment(ImpostorBakeOut in [[stage_in]],
                                                  bool frontFacing [[front_facing]],
                                                  uint sampleId [[sample_id]],
                                                  constant DrawUniforms& d [[buffer(0)]],
                                                  constant ImpostorBakeUniforms& b [[buffer(2)]],
                                                  texture2d<float> albedoTex [[texture(0)]],
                                                  texture2d<float> normalTex [[texture(2)]],
                                                  texture2d<float> ormTex [[texture(3)]],
                                                  texture2d<float> emissiveTex [[texture(4)]]) {
    (void)sampleId;  // per-sample shading: the alpha test resolves to coverage
    MaterialSample m = evaluateMaterial(in.modelPos, in.normal, in.uv, in.color, frontFacing, d, albedoTex, normalTex, ormTex,
                                        emissiveTex, true);
    float depth = dot(in.modelPos - b.center.xyz, b.forward.xyz) / b.center.w;
    ImpostorBakeTargets o;
    o.albedo = float4(m.s.albedo, 1.0);
    o.normal = float4(octEncode(normalize(m.s.N)) * 0.5 + 0.5, saturate(depth * 0.5 + 0.5), saturate(m.s.subsurface));
    o.roughness = saturate(m.s.roughness);
    return o;
}
