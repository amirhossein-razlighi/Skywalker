// ---------------------------------------------------------------------------
// Lit meshes
// ---------------------------------------------------------------------------

// `prevVerts` holds the previous frame's posed vertices of skinned meshes (the same buffer as
// `verts` otherwise); with `prevModel` it gives the velocity buffer per-object motion.
vertex MeshOut meshVertex(uint vid [[vertex_id]],
                          const device Vertex* verts [[buffer(0)]],
                          constant DrawUniforms& d [[buffer(1)]],
                          constant FrameUniforms& f [[buffer(2)]],
                          const device Vertex* prevVerts [[buffer(3)]]) {
    Vertex v = verts[vid];
    float4 world = d.model * float4(float3(v.position), 1.0);
    MeshOut o;
    o.position = f.viewProj * world;
    o.worldPos = world.xyz;
    o.prevWorldPos = d.motion.x > 0.5 ? (d.prevModel * float4(float3(prevVerts[vid].position), 1.0)).xyz : world.xyz;
    o.normal = (d.normalMatrix * float4(float3(v.normal), 0.0)).xyz;
    o.uv = float2(v.uv);
    o.color = float4(v.color);
    o.fade = 0.0;
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

// `mipBias` sharpens textures when the frame renders below output resolution and MetalFX
// upscales it (FrameUniforms.extra.w = log2(renderScale)); 0 otherwise.
static float4 sampleTri(texture2d<float> tex, Triplanar t, float mipBias) {
    return tex.sample(materialSampler, t.uvX, bias(mipBias)) * t.w.x + tex.sample(materialSampler, t.uvY, bias(mipBias)) * t.w.y +
           tex.sample(materialSampler, t.uvZ, bias(mipBias)) * t.w.z;
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

static float3 triplanarNormal(texture2d<float> tex, Triplanar t, float3 N, float strength, float mipBias) {
    // Whiteout blend of three tangent-space normals into world space.
    float3 tx = tex.sample(materialSampler, t.uvX, bias(mipBias)).xyz * 2.0 - 1.0;
    float3 ty = tex.sample(materialSampler, t.uvY, bias(mipBias)).xyz * 2.0 - 1.0;
    float3 tz = tex.sample(materialSampler, t.uvZ, bias(mipBias)).xyz * 2.0 - 1.0;
    tx.xy *= strength;
    ty.xy *= strength;
    tz.xy *= strength;
    float3 nx = float3(tx.xy + N.zy, abs(tx.z) * N.x);
    float3 ny = float3(ty.xy + N.xz, abs(ty.z) * N.y);
    float3 nz = float3(tz.xy + N.xy, abs(tz.z) * N.z);
    return normalize(nx.zyx * t.w.x + ny.xzy * t.w.y + nz.xyz * t.w.z);
}

// Direct (sun + punctual lights) and image-based lighting of a PBR / toon surface, before
// emission and fog. Shared by meshes, instanced foliage and terrain.
static float3 shadeSurface(SurfaceData s, float3 Ngeo, float3 worldPos, float2 fragXY, float3 V, bool toon, float rim,
                           constant FrameUniforms& f, const device GPULight* lights, const device uint2* clusterCells,
                           const device uint* clusterIndices, depth2d<float> shadowAtlas, texturecube<float> envTex,
                           texture2d<float> brdfLut, texture3d<float> cloudShape, uint layers = 1u) {
    // Sun
    float3 L = -f.sunDir.xyz;
    float sunVisible = L.y > -0.08 ? shadowFactor(worldPos, Ngeo, fragXY, f, shadowAtlas) : 0.0;
    if (sunVisible > 0.0) sunVisible *= cloudShadow(worldPos, L, f, cloudShape);  // drifting cloud shadows
    float3 sunRad = f.sunColor.rgb * f.sunDir.w * sunVisible;
    float3 color = toon ? toonLight(s, V, L, sunRad) : directLight(s, V, L, sunRad);

    // Punctual lights: directional ones everywhere, point/spot lights from this pixel's cluster.
    int dirCount = int(f.cluster2.y);
    uint2 cell = clusterCells[clusterOf(f, fragXY, worldPos)];
    int total = dirCount + int(cell.y);
    for (int k = 0; k < total; ++k) {
        int i = k < dirCount ? k : int(clusterIndices[cell.x + uint(k - dirCount)]);
        GPULight l = lights[i];
        if (!lightAffects(l, layers)) continue;  // render layers: the light's cullMask excludes this surface
        float3 Ll;
        float3 rad = lightRadiance(l, worldPos, Ll);
        color += toon ? toonLight(s, V, Ll, rad, l.params.x) : directLight(s, V, Ll, rad, l.params.x);
    }
    color = max(color, 0.0);  // negative lights darken, never below black

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
        if (rim > 0.0) {
        float r = pow(1.0 - NdotV, 3.0) * rim;
        if (toon) r = smoothstep(0.35, 0.4, r);
        indirect += r * (mix(f.skyHorizon.rgb, f.sunColor.rgb, 0.5) + s.albedo * 0.3) * 0.6;
    }
    return color + indirect;
}

// Height fog with a warm in-scatter toward the sun.
static float3 applyFog(float3 color, float3 worldPos, float3 V, constant FrameUniforms& f) {
    float fogAmt = fogFactor(f, worldPos);
    float3 fogC = f.fog.rgb + f.sunColor.rgb * f.sunDir.w * pow(saturate(dot(-V, -f.sunDir.xyz)), 8.0) * 0.25;
    return mix(color, fogC, fogAmt);
}

// Material evaluation shared by every lit surface path (meshes, instanced foliage) and the
// impostor bake, so impostors match their meshes exactly: base color x vertex color x maps,
// the alpha test, ORM and normal maps (tangent space or triplanar). `hardAlphaTest` cuts at the
// cutoff (bakes); otherwise the edge is sharpened to a ~1 px ramp for alpha-to-coverage.
struct MaterialSample {
    SurfaceData s;
    float3 emissive;
    float3 Ngeo;
};

static MaterialSample evaluateMaterial(float3 worldPos, float3 normal, float2 uvIn, float4 vertexColor, bool frontFacing,
                                       constant DrawUniforms& d, texture2d<float> albedoTex, texture2d<float> normalTex,
                                       texture2d<float> ormTex, texture2d<float> emissiveTex, bool hardAlphaTest,
                                       float mipBias = 0.0) {
    MaterialSample m;
    m.Ngeo = normalize(normal) * (frontFacing ? 1.0 : -1.0);
    bool tri = d.material2.w > 0.5;
    float2 uv = uvIn * d.material2.xy;
    Triplanar tp = triplanar(worldPos, m.Ngeo, d.material2.xy);

    SurfaceData s;
    s.albedo = d.color.rgb * vertexColor.rgb;
    s.alpha = d.color.a * vertexColor.a;
    if (d.maps.x > 0.5) {
        float4 t = tri ? sampleTri(albedoTex, tp, mipBias) : albedoTex.sample(materialSampler, uv, bias(mipBias));
        if (d.material4.y < 0.5) s.albedo *= t.rgb;
        s.alpha *= t.a;
        if (d.material4.x > 0.0) {
            if (hardAlphaTest) {
                if (s.alpha < d.material4.x) discard_fragment();
                s.alpha = 1.0;
            } else {
                // Alpha test, sharpened to a ~1 px ramp so alpha-to-coverage antialiases the edge.
                s.alpha = saturate((s.alpha - d.material4.x) / max(fwidth(s.alpha), 1e-4) + 0.5);
                if (s.alpha <= 0.0) discard_fragment();
            }
        } else if (s.alpha < 0.02) {
            discard_fragment();
        }
    }
    m.emissive = d.emissive.rgb * d.emissive.w;
    if (d.maps.w > 0.5) m.emissive *= (tri ? sampleTri(emissiveTex, tp, mipBias) : emissiveTex.sample(materialSampler, uv, bias(mipBias))).rgb;

    s.metallic = d.material.x;
    s.roughness = d.material.y;
    s.ao = 1.0;
    if (d.maps.z > 0.5) {
        float3 orm = (tri ? sampleTri(ormTex, tp, mipBias) : ormTex.sample(materialSampler, uv, bias(mipBias))).rgb;
        s.ao = mix(1.0, orm.r, saturate(d.maps.z - 1.0));
        s.roughness *= orm.g;
        s.metallic *= orm.b;
    }
    s.N = m.Ngeo;
    if (d.maps.y > 0.5) {
        if (tri) {
            s.N = triplanarNormal(normalTex, tp, m.Ngeo, d.material2.z, mipBias);
        } else {
            float3 mapN = normalTex.sample(materialSampler, uv, bias(mipBias)).xyz * 2.0 - 1.0;
            mapN.xy *= d.material2.z;
            s.N = perturbNormal(m.Ngeo, worldPos, uv, normalize(mapN));
        }
    }
    s.clearcoat = d.material3.x;
    s.subsurface = d.material3.y;
    m.s = s;
    return m;
}

fragment MainOut meshFragment(MeshOut in [[stage_in]],
                              bool frontFacing [[front_facing]],
                              constant DrawUniforms& d [[buffer(0)]],
                              constant FrameUniforms& f [[buffer(1)]],
                              const device GPULight* lights [[buffer(2)]],
                              const device uint2* clusterCells [[buffer(3)]],
                              const device uint* clusterIndices [[buffer(4)]],
                              texture2d<float> albedoTex [[texture(0)]],
                              depth2d<float> shadowAtlas [[texture(1)]],
                              texture2d<float> normalTex [[texture(2)]],
                              texture2d<float> ormTex [[texture(3)]],
                              texture2d<float> emissiveTex [[texture(4)]],
                              texturecube<float> envTex [[texture(5)]],
                              texture2d<float> brdfLut [[texture(6)]],
                              texture3d<float> cloudShape [[texture(7)]]) {
    // Foliage crossfading into its impostor: complementary dither (the impostor keeps the rest).
    if (in.fade > 0.0 && ditherNoise(in.position.xy, f.temporal) < in.fade) discard_fragment();
    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    if (f.cameraForward.w > 0.5) V = -f.cameraForward.xyz;
    MaterialSample m = evaluateMaterial(in.worldPos, in.normal, in.uv, in.color, frontFacing, d, albedoTex, normalTex, ormTex,
                                        emissiveTex, false, f.extra.w);
    SurfaceData s = m.s;
    float3 Ngeo = m.Ngeo;
    float3 emissive = m.emissive;
    // [debug views] lighting_only: a white diffuse material; other surface views replace the color.
    const int dbg = debugMode(f);
    if (dbg == kDbgLightingOnly) {
        s.albedo = float3(1.0);
        s.metallic = 0.0;
        emissive = 0.0;
    } else if (debugReplacesColor(dbg)) {
        DebugSurface ds;
        ds.worldPos = in.worldPos;
        ds.N = s.N;
        ds.uv = in.uv;
        ds.texUV = in.uv * d.material2.xy;
        ds.texSize = d.maps.x > 0.5 ? float(albedoTex.get_width()) : 0.0;
        ds.albedo = s.albedo;
        ds.emissive = emissive;
        ds.metallic = s.metallic;
        ds.roughness = s.roughness;
        ds.lod = max(d.material4.w - 1.0, 0.0);
        ds.lights = float(clusterCells[clusterOf(f, in.position.xy, in.worldPos)].y);
        return mainOut(float4(debugSurfaceColor(dbg, ds, f), s.alpha), s.albedo, 1.0, Ngeo, 1.0, kGbufNoLighting);
    }

    int shading = int(d.material.w + 0.5);
    if (shading == 2) {  // unlit: flat color, still emissive
        MainOut o = mainOut(float4(s.albedo + emissive, s.alpha), s.albedo, 1.0, Ngeo, 1.0, kGbufNoLighting);
        o.velocity = objectMotion(f, in.worldPos, in.prevWorldPos);
        return o;
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
    bool toon = shading == 1;

    // Render layers of this draw (DrawUniforms.motion.y; 0 = unset, e.g. mesh particles: layer 1).
    const uint layers = d.motion.y > 0.5 ? uint(d.motion.y) : 1u;
    float3 color = shadeSurface(s, Ngeo, in.worldPos, in.position.xy, V, toon, d.material3.z, f, lights, clusterCells,
                                clusterIndices, shadowAtlas, envTex, brdfLut, cloudShape, layers) + emissive;
    color = applyFog(color, in.worldPos, V, f);


    // Toon and legacy water keep their stylized ambient: no screen-space GI/reflections.
    bool screenSpace = !toon && shading != 3;
    MainOut o = mainOut(float4(color, s.alpha), s.albedo, s.ao, s.N, s.roughness, screenSpace ? s.metallic : kGbufNoLighting);
    o.velocity = objectMotion(f, in.worldPos, in.prevWorldPos);
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
    return mainOutFlat(d.outlineColor);
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
    return mainOutFlat(float4(col, a));
}
