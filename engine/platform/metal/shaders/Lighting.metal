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
// Screen-space global illumination and reflections (half resolution).
//
// Both trace rays through the depth buffer. A hit gathers the lit scene color (the previous
// frame's anti-aliased output reprojected, so light keeps bouncing frame after frame; or the
// current frame's direct-lit color when there is no history). A miss falls back to the sky
// probe. Results are accumulated over time (reprojection + disocclusion test) and denoised
// by the bilateral upsample in the lighting resolve.
// ---------------------------------------------------------------------------

struct SSUniforms {
    float4 params;   // x = GI distance (m), y = thickness (m), z = rays, w = steps
    float4 params2;  // x = radiance is history (reproject), y = temporal blend (0 = off), z = SSR max roughness, w = seed
    float4 texel;    // xy = full-res texel, zw = half-res texel
};

static float3 cosineHemisphere(float2 xi, float3 N) {
    float phi = 6.2831853 * xi.x;
    float r = sqrt(xi.y);
    float3 up = abs(N.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - xi.y)));
}

static float2 projectUV(constant FrameUniforms& f, float3 p, thread float& ndcZ) {
    float4 c = f.viewProj * float4(p, 1.0);
    ndcZ = c.z / c.w;
    return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
}

static float2 prevUV(constant FrameUniforms& f, float3 p) {
    float4 c = f.prevViewProj * float4(p, 1.0);
    return float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
}

// Marches `dir` from `p` in screen space. Returns the hit uv (current frame) or (-1).
static float2 traceScreen(constant FrameUniforms& f, depth2d<float> depthTex, float3 p, float3 dir, float maxDist,
                          float thickness, int steps, float jitter) {
    float3 eye = f.cameraPos.xyz;
    float prevT = 0.0;
    for (int i = 1; i <= steps; ++i) {
        float s = (float(i) - 1.0 + jitter) / float(steps);
        float t = maxDist * s * s + 0.02;
        float3 q = p + dir * t;
        float qz;
        float2 uv = projectUV(f, q, qz);
        if (any(uv < 0.0) || any(uv > 1.0) || qz < 0.0 || qz > 1.0) return float2(-1.0);
        float sd = depthTex.sample(pointClamp, uv);
        if (sd >= 1.0) { prevT = t; continue; }
        float sceneDist = distance(reconstructWorld(f, uv, sd), eye);
        float qDist = distance(q, eye);
        float th = max(thickness, (t - prevT) * 1.5);
        if (qDist > sceneDist + 0.01 && qDist - sceneDist < th) {
            // Refine between the last two samples.
            float a = prevT, b = t;
            for (int k = 0; k < 4; ++k) {
                float m = 0.5 * (a + b);
                float3 qm = p + dir * m;
                float mz;
                float2 muv = projectUV(f, qm, mz);
                float md = depthTex.sample(pointClamp, muv);
                if (distance(qm, eye) > distance(reconstructWorld(f, muv, md), eye)) b = m; else a = m;
            }
            float bz;
            return projectUV(f, p + dir * b, bz);
        }
        prevT = t;
    }
    return float2(-1.0);
}

// Radiance at a hit: previous anti-aliased frame (reprojected) or the current frame.
static float3 hitRadiance(constant FrameUniforms& f, constant SSUniforms& u, texture2d<float> radiance,
                          depth2d<float> depthTex, float2 uv) {
    float2 ruv = uv;
    if (u.params2.x > 0.5) {
        float3 hp = reconstructWorld(f, uv, depthTex.sample(pointClamp, uv));
        ruv = prevUV(f, hp);
        if (any(ruv < 0.0) || any(ruv > 1.0)) ruv = uv;
    }
    return min(radiance.sample(linearClamp, ruv).rgb, float3(64.0));  // tame fireflies
}

fragment float4 ssgiFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                             constant SSUniforms& u [[buffer(1)]], depth2d<float> depthTex [[texture(0)]],
                             texture2d<float> gbufB [[texture(1)]], texture2d<float> radiance [[texture(2)]],
                             texturecube<float> envTex [[texture(3)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float4 gb = gbufB.sample(pointClamp, uv);
    if (d >= 1.0 || gb.w >= 1.5) return float4(0.0, 0.0, 0.0, 1.0);
    float3 p = reconstructWorld(f, uv, d);
    float3 N = octDecode(gb.xy);
    float dist = distance(p, f.cameraPos.xyz);
    float maxDist = u.params.x * clamp(dist * 0.12, 1.0, 4.0);
    int rays = int(u.params.z), steps = int(u.params.w);
    float2 noise = float2(interleavedGradientNoise(in.position.xy + u.params2.w * 5.588238),
                          interleavedGradientNoise(in.position.yx * 1.31 + u.params2.w * 3.1));
    float3 origin = p + N * (0.015 + dist * 0.002);
    float lod = max(f.extra.z - 1.0, 0.0);
    float3 sum = 0.0;
    float hits = 0.0;
    for (int r = 0; r < rays; ++r) {
        float2 xi = fract(noise + float2(r) * float2(0.7548776662, 0.5698402910));  // R2 sequence
        float3 dir = cosineHemisphere(xi, N);
        float2 huv = traceScreen(f, depthTex, origin, dir, maxDist, u.params.y, steps, fract(noise.x + float(r) * 0.37));
        if (huv.x >= 0.0) {
            float3 hn = octDecode(gbufB.sample(pointClamp, huv).xy);
            float facing = saturate(-dot(hn, dir) * 4.0 + 0.25);  // back faces leak light: fade them
            sum += hitRadiance(f, u, radiance, depthTex, huv) * facing;
            hits += 1.0;
        } else {
            sum += envTex.sample(cubeSampler, dir, level(lod)).rgb * f.ground.w;
        }
    }
    return float4(sum / float(rays), 1.0 - hits / float(rays));
}

fragment float4 ssrFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                            constant SSUniforms& u [[buffer(1)]], depth2d<float> depthTex [[texture(0)]],
                            texture2d<float> gbufB [[texture(1)]], texture2d<float> radiance [[texture(2)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float4 gb = gbufB.sample(pointClamp, uv);
    float rough = gb.z;
    if (d >= 1.0 || gb.w >= 1.5 || rough > u.params2.z) return float4(0.0);
    float3 p = reconstructWorld(f, uv, d);
    float3 N = octDecode(gb.xy);
    float3 V = normalize(f.cameraPos.xyz - p);
    float2 xi = float2(interleavedGradientNoise(in.position.xy + u.params2.w * 5.588238),
                       interleavedGradientNoise(in.position.yx * 1.7 + u.params2.w * 2.3));
    // GGX-importance-sampled microfacet normal: glossy surfaces get blurry reflections.
    float a = max(rough * rough, 0.002);
    float3 H = importanceGGX(xi * float2(1.0, 0.85), N, a);
    float3 R = reflect(-V, H);
    if (dot(R, N) <= 0.0) R = reflect(-V, N);
    float dist = distance(p, f.cameraPos.xyz);
    float maxDist = max(dist * 1.5, 30.0);
    float2 huv = traceScreen(f, depthTex, p + N * (0.01 + dist * 0.001), R, maxDist, max(0.25, dist * 0.02),
                             int(u.params.w) * 2, xi.x);
    if (huv.x < 0.0) return float4(0.0);
    float3 hn = octDecode(gbufB.sample(pointClamp, huv).xy);
    if (dot(hn, R) > 0.3) return float4(0.0);  // hit a back face
    float2 edge = smoothstep(0.0, 0.08, huv) * smoothstep(1.0, 0.92, huv);
    float conf = edge.x * edge.y * (1.0 - smoothstep(u.params2.z * 0.6, u.params2.z, rough));
    return float4(hitRadiance(f, u, radiance, depthTex, huv), conf);
}

// Temporal accumulation for the half-resolution GI / SSR buffers.
fragment float4 ssTemporalFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                   constant SSUniforms& u [[buffer(1)]], texture2d<float> current [[texture(0)]],
                                   texture2d<float> history [[texture(1)]], depth2d<float> depthTex [[texture(2)]],
                                   depth2d<float> prevDepth [[texture(3)]]) {
    float2 uv = uvOf(in);
    float4 c = current.sample(pointClamp, uv);
    float blend = u.params2.y;
    if (blend <= 0.0) return c;
    float d = depthTex.sample(pointClamp, uv);
    if (d >= 1.0) return c;
    float3 p = reconstructWorld(f, uv, d);
    float2 puv = prevUV(f, p);
    if (any(puv < 0.0) || any(puv > 1.0)) return c;
    // Disocclusion: the previous frame must have seen (about) the same surface there.
    float pd = prevDepth.sample(pointClamp, puv);
    float4 pc = f.prevViewProj * float4(p, 1.0);
    float expected = pc.z / pc.w;
    float dist = distance(p, f.cameraPos.xyz);
    float tol = 0.0004 + 0.002 / max(dist, 0.5);
    if (abs(pd - expected) > tol * 8.0) return c;
    float4 h = history.sample(linearClamp, puv);
    // Clamp the history to the local neighborhood (wide, the input is noisy).
    float4 mn = c, mx = c;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float4 n = current.sample(pointClamp, uv + float2(x, y) * u.texel.zw);
            mn = min(mn, n);
            mx = max(mx, n);
        }
    }
    float4 range = mx - mn;
    h = clamp(h, mn - range * 0.5, mx + range * 0.5);
    return mix(c, h, blend);
}

// Lighting resolve (full resolution): replaces the sky-probe indirect light of PBR surfaces
// with screen-space GI and reflections, and applies SSAO to indirect diffuse.
struct ResolveUniforms {
    float4 params;  // x = GI strength, y = SSR strength, z = SSAO strength, w = has GI
    float4 params2; // x = has SSR, y = has AO
    float4 texel;   // xy = full-res texel, zw = half-res texel
};

static float4 bilateralHalf(texture2d<float> t, float2 uv, float2 halfTexel, depth2d<float> depthTex,
                            constant FrameUniforms& f, float3 p, float3 N, texture2d<float> gbufB) {
    float4 sum = 0.0;
    float wsum = 0.0;
    float dist = distance(p, f.cameraPos.xyz);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 suv = uv + float2(x, y) * halfTexel * 1.5;
            float sd = depthTex.sample(pointClamp, suv);
            float3 sp = reconstructWorld(f, suv, sd);
            float3 sn = octDecode(gbufB.sample(pointClamp, suv).xy);
            float wz = exp(-abs(dot(sp - p, N)) / (0.02 + dist * 0.01) * 2.0);
            float wn = pow(saturate(dot(sn, N)), 8.0);
            float w = wz * wn * (x == 0 && y == 0 ? 2.0 : 1.0) + 1e-4;
            sum += t.sample(linearClamp, suv) * w;
            wsum += w;
        }
    }
    return sum / wsum;
}

fragment float4 lightingResolveFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                        constant ResolveUniforms& r [[buffer(1)]],
                                        texture2d<float> color [[texture(0)]], texture2d<float> gbufA [[texture(1)]],
                                        texture2d<float> gbufB [[texture(2)]], depth2d<float> depthTex [[texture(3)]],
                                        texture2d<float> aoTex [[texture(4)]], texture2d<float> giTex [[texture(5)]],
                                        texture2d<float> ssrTex [[texture(6)]], texturecube<float> envTex [[texture(7)]],
                                        texture2d<float> brdfLut [[texture(8)]]) {
    float2 uv = uvOf(in);
    float4 c = color.sample(pointClamp, uv);
    float4 gb = gbufB.sample(pointClamp, uv);
    float d = depthTex.sample(pointClamp, uv);
    if (d >= 1.0 || gb.w >= 1.5) return c;
    float4 ga = gbufA.sample(pointClamp, uv);
    float3 p = reconstructWorld(f, uv, d);
    float3 N = octDecode(gb.xy);
    float3 V = normalize(f.cameraPos.xyz - p);
    if (f.cameraForward.w > 0.5) V = -f.cameraForward.xyz;
    float rough = gb.z, metal = saturate(gb.w);
    float3 albedo = ga.rgb;
    float aoMat = ga.a;
    float fogT = 1.0 - fogFactor(f, p);
    float ambientK = f.ground.w;
    float maxMip = f.extra.z;
    float NdotV = max(dot(N, V), 1e-4);
    float3 F0 = mix(float3(0.04), albedo, metal);
    float2 ab = brdfLut.sample(linearClamp, float2(NdotV, 1.0 - rough)).rg;
    float3 Fr = F0 * ab.x + ab.y;
    float3 kd = (1.0 - Fr) * (1.0 - metal);

    // Diffuse: sky probe -> screen-space GI (blended by strength), times SSAO.
    float3 envIrr = envTex.sample(cubeSampler, N, level(maxMip)).rgb * ambientK;
    float3 irr = envIrr;
    if (r.params.w > 0.5) {
        float4 gi = bilateralHalf(giTex, uv, r.texel.zw, depthTex, f, p, N, gbufB);
        irr = mix(envIrr, gi.rgb, r.params.x);
    }
    float ssao = r.params2.y > 0.5 ? mix(1.0, aoTex.sample(linearClamp, uv).r, saturate(r.params.z)) : 1.0;
    float3 diffuseW = albedo * kd * aoMat * fogT;
    float3 delta = diffuseW * (irr * ssao - envIrr);

    // Specular: sky probe -> screen-space reflections where the trace found something.
    if (r.params2.x > 0.5) {
        float3 R = reflect(-V, N);
        float3 prefiltered = envTex.sample(cubeSampler, R, level(rough * maxMip)).rgb * f.sky.w * ambientK;
        float specOcc = saturate(pow(NdotV + aoMat, exp2(-16.0 * rough - 1.0)) - 1.0 + aoMat);
        float4 ssr = bilateralHalf(ssrTex, uv, r.texel.zw, depthTex, f, p, N, gbufB);
        float conf = saturate(ssr.a * r.params.y);
        float3 hit = conf > 1e-3 ? ssr.rgb / max(ssr.a, 1e-3) : prefiltered;
        delta += Fr * specOcc * fogT * (mix(prefiltered, hit, conf) - prefiltered) * ssao;
    }
    return float4(max(c.rgb + delta, 0.0), c.a);
}

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
