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
    s.N = N;
    float3 color = shadeSurface(s, Ngeo, wp, in.position.xy, V, false, 0.0, f, lights, clusterCells, clusterIndices, shadowAtlas,
                                envTex, brdfLut, cloudShape);
    if (tu.water.z > 0.5) color = mix(color, float3(1.0, 0.5, 0.1), 0.15);  // selection tint
    color = applyFog(color, wp, V, f);
    return mainOut(float4(color, 1.0), s.albedo, s.ao, N, s.roughness, s.metallic);
}

// ---------------------------------------------------------------------------
// Instanced foliage: per-instance 3x4 transforms; wind bends vertices by their height
// above the mesh base (gusts travel across the field); thinning toward the cull distance.
// Shading reuses meshFragment (vertex color carries the instance tint).
// ---------------------------------------------------------------------------

struct FoliageInstanceGpu {
    float4 row0, row1, row2;
    float4 params;  // x = tint, y = wind phase, z = height (m), w = fade random
};

struct FoliageUniforms {
    float4 wind;    // xy = direction (xz), z = speed (m/s), w = layer bend strength
    float4 params;  // x = cull distance, y = mesh height (m), z = unused, w = time (s)
    float4x4 part;  // the part's transform inside a multi-part model (trunk, leaves...)
};

static float3 foliageWorld(float3 p, FoliageInstanceGpu inst, constant FoliageUniforms& fu, thread float& h01) {
    float3 base = float3(inst.row0.w, inst.row1.w, inst.row2.w);
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
                             constant FoliageUniforms& fu [[buffer(4)]]) {
    Vertex v = verts[vid];
    FoliageInstanceGpu inst = instances[iid];
    float3 base = float3(inst.row0.w, inst.row1.w, inst.row2.w);
    MeshOut o;
    // Thin out smoothly toward the cull distance (random per instance), then vanish.
    float dist = distance(base, f.cameraPos.xyz);
    float limit = fu.params.x * (0.72 + 0.28 * inst.params.w);
    float shrink = 1.0 - smoothstep(limit * 0.85, limit, dist);
    if (shrink <= 0.0) {
        o.position = float4(0.0, 0.0, -2.0, 1.0);  // degenerate, culled
        o.worldPos = 0.0;
        o.normal = float3(0, 1, 0);
        o.uv = 0.0;
        o.color = 0.0;
        return o;
    }
    float h01;
    float3 local = (fu.part * float4(float3(v.position), 1.0)).xyz;
    float3 world = foliageWorld(local * float3(1.0, shrink, 1.0), inst, fu, h01);
    float3 ln = (fu.part * float4(float3(v.normal), 0.0)).xyz;
    float3 n = float3(dot(inst.row0.xyz, ln), dot(inst.row1.xyz, ln), dot(inst.row2.xyz, ln));
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.normal = normalize(n);
    o.uv = float2(v.uv);
    float tint = inst.params.x;
    float3 c = float4(v.color).rgb * (1.0 + tint * 0.22);
    c = mix(c, c * float3(1.18, 1.06, 0.62), saturate(tint) * 0.6);  // some blades yellow, some lush
    o.color = float4(c, float4(v.color).a);
    return o;
}

vertex float4 foliageShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device Vertex* verts [[buffer(0)]],
                                  constant float4x4& lightViewProj [[buffer(2)]],
                                  const device FoliageInstanceGpu* instances [[buffer(3)]],
                                  constant FoliageUniforms& fu [[buffer(4)]]) {
    float h01;
    float3 world = foliageWorld((fu.part * float4(float3(verts[vid].position), 1.0)).xyz, instances[iid], fu, h01);
    return lightViewProj * float4(world, 1.0);
}

// Alpha-tested foliage (leaf cards, grass cards) casts shadows only where it is solid.
vertex ShadowAlphaOut foliageShadowAlphaVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                               const device Vertex* verts [[buffer(0)]],
                                               constant DrawUniforms& d [[buffer(1)]],
                                               constant float4x4& lightViewProj [[buffer(2)]],
                                               const device FoliageInstanceGpu* instances [[buffer(3)]],
                                               constant FoliageUniforms& fu [[buffer(4)]]) {
    float h01;
    float3 world = foliageWorld((fu.part * float4(float3(verts[vid].position), 1.0)).xyz, instances[iid], fu, h01);
    ShadowAlphaOut o;
    o.position = lightViewProj * float4(world, 1.0);
    o.uv = float2(verts[vid].uv) * d.material2.xy;
    return o;
}
