// ---------------------------------------------------------------------------
// Water: FFT ocean cascades (displacement in the vertex stage, slopes + Jacobian per
// pixel), refraction with depth absorption and in-scattering, screen-space reflections
// over the sky cubemap, GGX sun glints, crest subsurface scattering, whitecaps and shore foam.
// ---------------------------------------------------------------------------

struct WaterUniforms {
    float4 levelSize;  // x = level, y = size (0 endless), zw = center
    float4 deep;
    float4 shallow;
    float4 params;     // x = clarity, y = foam, z = reflections, w = refraction
    float4 params2;    // x = roughness, y = 1/N, z = endless
    float4 patch;      // xyz = cascade tile sizes
    float4 origin;     // xz = grid origin
    float4 shore;      // xyz = terrain center, w = terrain size (m); the seabed under the water
    float4 shore2;     // x = heightmap resolution, y = 1 if a terrain is bound, z = shoaling depth (m)
};

struct WaterOut {
    float4 position [[position]];
    float3 worldPos;
    float2 baseXZ;
    float height;
    float calm;  // 0 at the waterline .. 1 in open water (waves shoal and die in the shallows)
};

// Waves shoal toward the shore: they lose height as the (smoothed) seabed rises, still run up
// the beach face, and die out just above the waterline. The seabed is low-passed (~16 m), so
// hollows behind a beach berm read as land: without this the FFT surface rises through gently
// sloping sand and floods dry hollows with every swell.
static float shoreDamping(constant WaterUniforms& w, texture2d<float> seabed, float2 xz) {
    if (w.shore2.y < 0.5) return 1.0;
    float2 uv = (xz - w.shore.xz) / w.shore.w + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float h = seabed.sample(linearClamp, uv).r;
    float depth = w.levelSize.x - (w.shore.y + h);
    float open = smoothstep(0.0, max(w.shore2.z, 0.1), depth);   // shoaling: 0 at the waterline, 1 offshore
    float runup = smoothstep(-1.2, 0.0, depth);                  // the beach face above the waterline
    return runup * mix(0.4, 1.0, open);
}

vertex WaterOut waterVertex(uint vid [[vertex_id]],
                            const device float2* grid [[buffer(0)]],
                            constant WaterUniforms& w [[buffer(1)]],
                            constant FrameUniforms& f [[buffer(2)]],
                            texture2d<float> d0 [[texture(0)]],
                            texture2d<float> d1 [[texture(1)]],
                            texture2d<float> d2 [[texture(2)]],
                            texture2d<float> seabed [[texture(3)]]) {
    float2 g = grid[vid];
    bool endless = w.params2.z > 0.5;
    float2 xz = endless ? g + w.origin.xz : w.levelSize.zw + g * w.levelSize.y;
    float dist = length(xz - f.cameraPos.xz) + abs(f.cameraPos.y - w.levelSize.x);
    float edge = 1.0;
    if (!endless) {
        float2 d = abs(xz - w.levelSize.zw);
        edge = saturate((w.levelSize.y * 0.5 - max(d.x, d.y)) / 1.5);
    }
    float ht = 0.5 * w.params2.y;
    float texel0 = w.patch.x * w.params2.y, texel1 = w.patch.y * w.params2.y, texel2 = w.patch.z * w.params2.y;
    float3 disp = d0.sample(oceanSampler, xz / w.patch.x + ht, level(clamp(log2(max(dist, 1.0) / (texel0 * 60.0)), 0.0, 6.0))).xyz;
    disp += d1.sample(oceanSampler, xz / w.patch.y + ht, level(clamp(log2(max(dist, 1.0) / (texel1 * 60.0)), 0.0, 6.0))).xyz *
            saturate(1.5 - dist / (w.patch.y * 12.0));
    disp += d2.sample(oceanSampler, xz / w.patch.z + ht, level(clamp(log2(max(dist, 1.0) / (texel2 * 60.0)), 0.0, 6.0))).xyz *
            saturate(1.5 - dist / (w.patch.z * 12.0));
    float calm = shoreDamping(w, seabed, xz);
    disp *= edge * calm;
    float3 world = float3(xz.x + disp.x, w.levelSize.x + disp.y, xz.y + disp.z);
    WaterOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.position.z = min(o.position.z, o.position.w * 0.999999);  // the endless sea reaches the horizon
    o.worldPos = world;
    o.baseXZ = xz;
    o.height = disp.y;
    o.calm = calm;
    return o;
}

// Animated caustic pattern (iterated trigonometric warping, after the well-known "tileable water caustic").
static float waterCaustic(float2 p, float t) {
    float2 i = p;
    float c = 1.0;
    const float inten = 0.005;
    for (int n = 0; n < 4; ++n) {
        float t2 = t * (1.0 - 3.5 / float(n + 1));
        i = p + float2(cos(t2 - i.x) + sin(t2 + i.y), sin(t2 - i.y) + cos(t2 + i.x));
        c += 1.0 / length(float2(p.x / (sin(i.x + t2) / inten), p.y / (cos(i.y + t2) / inten)));
    }
    c /= 4.0;
    c = 1.17 - pow(c, 1.4);
    return saturate(pow(abs(c), 8.0));
}

static float4 traceSSR(constant FrameUniforms& f, float3 p, float3 R, texture2d<float> sceneTex, depth2d<float> depthTex) {
    float stepLen = 0.35;
    float3 pos = p + R * 0.15;
    for (int i = 0; i < 40; ++i) {
        pos += R * stepLen;
        stepLen *= 1.14;
        float4 c = f.viewProj * float4(pos, 1.0);
        if (c.w <= 1e-4) break;
        float3 ndc = c.xyz / c.w;
        float2 suv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (any(suv < 0.0) || any(suv > 1.0)) break;
        float sd = depthTex.sample(pointClamp, suv);
        if (sd >= 0.99999 || ndc.z <= sd) continue;
        float3 sp = reconstructWorld(f, suv, sd);
        float behind = distance(f.cameraPos.xyz, pos) - distance(f.cameraPos.xyz, sp);
        if (behind > stepLen * 1.6 + 0.4) break;  // passed behind a thick object: no reliable hit
        float2 e = smoothstep(0.0, 0.07, suv) * smoothstep(1.0, 0.93, suv);
        float conf = e.x * e.y * (1.0 - float(i) / 40.0);
        return float4(sceneTex.sample(linearClamp, suv).rgb, conf);
    }
    return float4(0.0);
}

fragment EffectOut waterFragment(WaterOut in [[stage_in]],
                               constant WaterUniforms& w [[buffer(0)]],
                               constant FrameUniforms& f [[buffer(1)]],
                               constant GPULight* lights [[buffer(2)]],
                               depth2d<float> shadowAtlas [[texture(1)]],
                               texturecube<float> envTex [[texture(5)]],
                               texture2d<float> sceneTex [[texture(6)]],
                               depth2d<float> sceneDepth [[texture(7)]],
                               texture2d<float> s0 [[texture(8)]],
                               texture2d<float> s1 [[texture(9)]],
                               texture2d<float> s2 [[texture(10)]],
                               texture2d<float> j0 [[texture(11)]],
                               texture2d<float> j1 [[texture(12)]],
                               texture2d<float> j2 [[texture(13)]],
                               texture2d<float> pano [[texture(14)]]) {
    bool endless = w.params2.z > 0.5;
    float2 xz = in.baseXZ;
    if (!endless) {
        // Bounded water (lakes, ponds, puddles) gets an organic, ragged outline.
        float2 rel = (xz - w.levelSize.zw) / (w.levelSize.y * 0.5);
        float r = length(rel * rel * rel * rel);  // squircle
        float ragged = (fbm(xz * (6.0 / max(w.levelSize.y, 0.5)) + w.levelSize.zw) - 0.5) * 0.5;
        if (pow(r, 0.25) > 0.92 + ragged) discard_fragment();
    }
    float ht = 0.5 * w.params2.y;
    float2 u0 = xz / w.patch.x + ht, u1 = xz / w.patch.y + ht, u2 = xz / w.patch.z + ht;
    float4 sl0 = s0.sample(oceanSampler, u0), sl1 = s1.sample(oceanSampler, u1), sl2 = s2.sample(oceanSampler, u2);
    float2 sl = sl0.xy + sl1.xy + sl2.xy;
    float persistentFoam = saturate(sl0.z + sl1.z * 0.8 + sl2.z * 0.5);
    float3 N = normalize(float3(-sl.x, 1.0, -sl.y) * float3(mix(0.3, 1.0, in.calm), 1.0, mix(0.3, 1.0, in.calm)));
    float J = j0.sample(oceanSampler, u0).w + j1.sample(oceanSampler, u1).w + j2.sample(oceanSampler, u2).w - 2.0;

    float3 V = normalize(f.cameraPos.xyz - in.worldPos);
    if (dot(N, V) < 0.02) N = normalize(N + V * (0.02 - dot(N, V)));
    float NdotV = max(dot(N, V), 1e-3);
    float dist = length(f.cameraPos.xyz - in.worldPos);
    float2 uv = in.position.xy * f.viewport.zw;

    // What lies behind the surface (opaque scene, before the water was drawn).
    float dS = sceneDepth.sample(pointClamp, uv);
    bool skyBehind = dS >= 0.99999;
    float3 sceneP = reconstructWorld(f, uv, dS);
    float2 off = N.xz * (0.03 * w.params.w) / max(1.0, dist * 0.05);
    float2 ruv = clamp(uv + off, float2(0.001), float2(0.999));
    float dR = sceneDepth.sample(pointClamp, ruv);
    float3 rP = reconstructWorld(f, ruv, dR);
    float thick = (dR >= 0.99999) ? 1e4 : dot(rP - in.worldPos, -V);
    if (thick < 0.0) {  // the offset sample is in front of the water: don't refract
        ruv = uv;
        thick = skyBehind ? 1e4 : max(dot(sceneP - in.worldPos, -V), 0.0);
        rP = sceneP;
    }
    float3 refr = sceneTex.sample(linearClamp, ruv).rgb;

    float3 L = -f.sunDir.xyz;
    float3 sunRad = f.sunColor.rgb * f.sunDir.w;
    float ambientK = f.ground.w * 2.0;
    float3 skyIrr = envTex.sample(cubeSampler, float3(0.0, 1.0, 0.0), level(f.extra.z)).rgb;
    float3 absorb = (1.0 - w.shallow.rgb) * (2.2 / max(w.params.x, 0.1)) + 0.012;
    // Light travels down to the submerged surface and back up to the eye: both are absorbed.
    float down = (dR >= 0.99999) ? 0.0 : max(w.levelSize.x - rP.y, 0.0);
    // Caustics: the wavy surface focuses sunlight into moving bright lines on what lies below.
    if (down > 0.02) {
        float3 Lc = -f.sunDir.xyz;
        float2 cp = rP.xz * 0.9 + Lc.xz * down * 0.6;
        float c = waterCaustic(cp, f.cameraPos.w * 0.6) * 0.65 + waterCaustic(cp * 1.7 + 3.1, f.cameraPos.w * 0.75) * 0.35;
        refr *= 1.0 + c * 2.2 * saturate(Lc.y * 2.0) * saturate(down * 3.0) * exp(-down * 0.35) * min(f.sunDir.w, 3.0) * 0.4;
    }
    float3 T = exp(-absorb * min(thick + down, 500.0));
    float3 inscatter = w.deep.rgb * (skyIrr * ambientK * 0.9 + sunRad * saturate(L.y) * 0.14);
    float3 under = refr * T + inscatter * (1.0 - T);

    float rough = mix(w.params2.x, 0.14, saturate(dist / 900.0));
    float3 R = reflect(-V, N);
    R.y = max(abs(R.y), 0.035);  // reflect sky, never the band below the horizon
    R = normalize(R);
    float3 envR = f.sky.x > 1.5 ? panorama(R, f, pano, max(f.hdri.z - 1.5, 0.0) + rough * 9.0)
                                : envTex.sample(cubeSampler, R, level(rough * f.extra.z * 1.5)).rgb;
    float4 ssr = traceSSR(f, in.worldPos, R, sceneTex, sceneDepth);
    float3 refl = mix(envR, ssr.rgb, ssr.a) * f.sky.w;
    float F = saturate((0.02 + 0.98 * pow(1.0 - NdotV, 5.0)) * w.params.z);

    float shadow = L.y > -0.08 ? shadowFactor(in.worldPos, N, in.position.xy, f, shadowAtlas) : 0.0;
    float3 H = normalize(L + V);
    float NdotL = saturate(dot(N, L));
    float a2 = max(rough * rough, 0.0006);
    float spec = D_GGX(saturate(dot(N, H)), a2) * V_SmithGGX(NdotV, NdotL, a2) * NdotL;
    float3 color = mix(under, refl, F) + sunRad * spec * F_Schlick(float3(0.02), saturate(dot(V, H))) * shadow;

    // Light through the thin crests of waves rolling toward the viewer.
    float crest = saturate(in.height * 0.45 + 0.2);
    color += w.shallow.rgb * sunRad * pow(saturate(dot(V, -L) * 0.5 + 0.5), 4.0) * crest * 0.16 * shadow * (1.0 - F);

    // Lamps and fires reflect on the water.
    int count = int(f.params.y);
    for (int i = 0; i < count; ++i) {
        float3 Ll;
        float3 rad = pointLightAt(lights[i], in.worldPos, N, Ll);
        float3 Hl = normalize(Ll + V);
        float nl = saturate(dot(N, Ll));
        float sp = D_GGX(saturate(dot(N, Hl)), max(a2, 0.004)) * V_SmithGGX(NdotV, nl, max(a2, 0.004)) * nl;
        color += rad * (sp * 0.6 + nl * 0.02);
    }

    // Foam: whitecaps where the surface folds (Jacobian < 1) and surf where the water is shallow.
    float time = f.cameraPos.w;
    // Surf hugs the waterline: where the view crosses only a little water before hitting ground.
    float through = skyBehind ? 1e4 : max(dot(sceneP - in.worldPos, -V), 0.0);
    float shoreBand = pow(saturate(1.0 - through / 0.45), 1.5);
    float surf = shoreBand * smoothstep(0.45, 0.75, fbm(xz * 3.0 + float2(time * 0.25, -time * 0.15)) + shoreBand * 0.5);
    // Whitecaps: only where crests actually fold (Jacobian well below 1), streaked by noise.
    // Whitecaps: born where crests fold, then linger and break up into streaky lace.
    float lace = fbm(xz * float2(2.6, 1.3) + float2(time * 0.08, 0.0)) * 0.6 + fbm(xz * 6.5 - float2(0.0, time * 0.1)) * 0.4;
    float caps = smoothstep(0.25, 0.8, persistentFoam + (lace - 0.5) * 0.9) * smoothstep(0.1, 0.4, persistentFoam);
    float foam = saturate((surf + caps) * w.params.y);
    float3 foamColor = (sunRad * (NdotL * 0.8 + 0.2) * shadow + skyIrr * ambientK) * 0.8;
    color = mix(color, foamColor, foam * 0.9);

    // Far away the sea becomes a mirror of the horizon sky (grazing Fresnel), so it meets the
    // sky without a seam where the simulated grid ends.
    float far = smoothstep(1500.0, 18000.0, length(in.worldPos.xz - f.cameraPos.xz));
    if (far > 0.0) {
        float3 hd = normalize(float3(-V.x, 0.04, -V.z));  // just above the probe's horizon (below it is ground)
        color = mix(color, envTex.sample(cubeSampler, hd, level(0.0)).rgb * mix(1.0, 0.85, saturate(w.params.z)), far * 0.9);
    }
    float fogAmt = fogFactor(f, in.worldPos);
    float3 fogC = f.fog.rgb + f.sunColor.rgb * f.sunDir.w * pow(saturate(dot(-V, L)), 8.0) * 0.25;
    color = mix(color, fogC, fogAmt);
    EffectOut o;
    o.color = float4(color, 1.0);
    return o;
}

// ---------------------------------------------------------------------------
// Particles: one back-to-front stream of camera-facing (or velocity-stretched) quads,
// premultiplied alpha (emissive looks output alpha ~0 = additive). Soft particles fade
// against the depth buffer. Smoke, mist, snow and rain are lit by the sun (shadowed),
// the sky and every point light; flames are procedural, animated fire tongues.
// ---------------------------------------------------------------------------

struct ParticleGpu {
    packed_float3 position;
    float size;
    float4 color;
    packed_float3 velocity;
    float rotation;
    float age;
    float look;
    float seed;
    float softness;
};

struct ParticleOut {
    float4 position [[position]];
    float3 worldPos;
    float2 uv;
    float4 color;
    float age;
    float seed;
    float softness;
    float size;
    float look [[flat]];
};

vertex ParticleOut particleVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                  const device ParticleGpu* ps [[buffer(0)]],
                                  constant FrameUniforms& f [[buffer(1)]]) {
    ParticleGpu p = ps[iid];
    const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(1, 1), float2(-1, 1)};
    float2 c = corners[vid];
    float3 center = float3(p.position);
    float3 toCam = normalize(f.cameraPos.xyz - center);
    float3 fwd = -toCam;
    float3 worldUp = abs(fwd.y) > 0.99 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(fwd, worldUp));
    float3 up = cross(right, fwd);
    float3 v = float3(p.velocity);
    float3 vs = v - fwd * dot(v, fwd);
    float len = length(vs);
    float r = p.size * 0.5;
    int look = int(p.look + 0.5);
    float3 world;
    if (len > r * 0.25 && (look == 0 || look == 3 || look == 4 || look == 7)) {
        float3 along = vs / len;
        float3 across = normalize(cross(along, toCam));
        float3 mid = center - v * 0.5;
        world = mid + along * (c.y * (len * 0.5 + r)) + across * (c.x * r);
    } else {
        float cr = cos(p.rotation), sr = sin(p.rotation);
        float2 rc = look == 1 ? c : float2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);  // flames stay upright
        world = center + (right * rc.x + up * rc.y) * r;
    }
    ParticleOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.uv = c;
    o.color = p.color;
    o.age = p.age;
    o.seed = p.seed;
    o.softness = p.softness;
    o.size = p.size;
    o.look = p.look;
    return o;
}

static float3 particleLighting(constant FrameUniforms& f, constant GPULight* lights, depth2d<float> atlas,
                               texturecube<float> env, float3 pos, float3 n, float2 pixel, float phaseAmount) {
    float3 L = -f.sunDir.xyz;
    float sh = L.y > -0.08 ? shadowFactor(pos, float3(0.0, 1.0, 0.0), pixel, f, atlas) : 0.0;
    float3 V = normalize(f.cameraPos.xyz - pos);
    float wrap = saturate(dot(n, L) * 0.5 + 0.5);
    float g = 0.5;
    float cosT = dot(-V, L);
    float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.0796;  // Henyey-Greenstein
    float3 lit = f.sunColor.rgb * f.sunDir.w * sh * (wrap * 0.32 + hg * phaseAmount * 2.0);
    lit += env.sample(cubeSampler, n, level(f.extra.z)).rgb * f.ground.w * 1.1;
    int count = int(f.params.y);
    for (int i = 0; i < count; ++i) {
        float3 Ll;
        float3 rad = pointLightAt(lights[i], pos, n, Ll);
        lit += rad * (saturate(dot(n, Ll)) * 0.6 + 0.4);
    }
    return lit;
}

fragment EffectOut particleFragment(ParticleOut in [[stage_in]],
                                  constant FrameUniforms& f [[buffer(1)]],
                                  constant GPULight* lights [[buffer(2)]],
                                  depth2d<float> shadowAtlas [[texture(1)]],
                                  texturecube<float> envTex [[texture(5)]],
                                  depth2d<float> sceneDepth [[texture(7)]]) {
    float2 suv = in.position.xy * f.viewport.zw;
    float sd = sceneDepth.sample(pointClamp, suv);
    float camDist = distance(f.cameraPos.xyz, in.worldPos);
    float soft = 1.0;
    if (sd < 0.99999) {
        float gap = distance(f.cameraPos.xyz, reconstructWorld(f, suv, sd)) - camDist;
        if (gap <= 0.0) discard_fragment();
        soft = saturate(gap / max(in.softness, 0.005));
    }
    soft *= saturate((camDist - 0.1) / max(in.size, 0.05));  // fade out instead of filling the lens

    int look = int(in.look + 0.5);
    float2 p = in.uv;
    float r2 = dot(p, p);
    float time = f.cameraPos.w;
    float3 rgb = float3(0.0);
    float a = 0.0;
    float3 toCam = normalize(f.cameraPos.xyz - in.worldPos);
    float3 right = normalize(cross(-toCam, abs(toCam.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0)));
    float3 up = cross(right, -toCam);

    if (look == 1) {  // flame tongue: tapering, swaying, noise-eroded; heat ramp from base to tip
        float y01 = p.y * 0.5 + 0.5;
        float flow = time * 2.6 + in.seed * 11.0;
        float n1 = fbm(float2(p.x * 1.6 + in.seed * 7.0, p.y * 0.9 - flow));
        float n2 = fbm(float2(p.x * 3.4 - in.seed * 3.0, p.y * 1.8 - flow * 1.7));
        float sway = (n1 - 0.5) * 0.7 * y01 * y01;
        float width = 0.62 * pow(saturate(1.0 - y01), 0.75) + 0.04;
        float body = saturate(1.0 - abs(p.x + sway) / width);
        float erode = saturate((n2 * 1.25 + (1.0 - y01) * 0.95 - 0.78) * 3.2);
        float d = body * erode * smoothstep(0.0, 0.18, y01) * in.color.a * (1.0 - smoothstep(0.55, 1.0, in.age));
        float heat = saturate(d * (1.15 - y01 * 0.75));
        // ramp: deep red tips -> particle color -> yellow-white core
        float3 tip = in.color.rgb * float3(0.75, 0.32, 0.12);
        float3 col = mix(tip, in.color.rgb, smoothstep(0.08, 0.5, heat));
        col = mix(col, in.color.rgb * float3(1.0, 1.35, 1.9) + dot(in.color.rgb, float3(0.1)), smoothstep(0.55, 1.0, heat) * 0.7);
        a = saturate(d * 1.3) * 0.6;
        rgb = col * a;
    } else if (look == 2 || look == 6) {  // smoke / mist: lit billowing volume
        bool mist = look == 6;
        float2 q = p * (mist ? 0.7 : 1.25) + float2(in.seed * 17.0, in.seed * 5.0 - in.age * 0.7);
        float n = fbm(q * 1.6) * 0.65 + fbm(q * 3.7 + float2(time * 0.05, -time * 0.03)) * 0.35;
        float sphere = pow(saturate(1.0 - r2), mist ? 2.2 : 1.4);
        float density = saturate(sphere * (0.35 + n * 1.25) - 0.12);
        a = density * in.color.a * smoothstep(0.0, mist ? 0.25 : 0.1, in.age);
        float3 n3 = normalize(right * p.x + up * p.y + toCam * sqrt(max(0.0, 1.0 - r2)) + float3(0, (n - 0.5) * 0.6, 0));
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, n3, in.position.xy, mist ? 0.8 : 0.5);
        rgb = in.color.rgb * lit * (0.75 + 0.25 * n) * a;
    } else if (look == 4 || look == 7) {  // rain streak / splash droplet: lit by lamps and sky
        float across = exp(-p.x * p.x * (look == 4 ? 4.0 : 2.0));
        float along = look == 4 ? smoothstep(1.0, 0.55, abs(p.y)) : saturate(1.0 - r2);
        a = in.color.a * across * along * (look == 7 ? 0.8 : 1.0);
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.6);
        rgb = in.color.rgb * (lit * 0.55 + 0.02) * a;
    } else if (look == 5) {  // snow flake
        a = smoothstep(1.0, 0.25, sqrt(r2)) * in.color.a;
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.3);
        rgb = in.color.rgb * lit * 0.8 * a;
    } else if (look == 3) {  // spark streak
        float g = exp(-p.x * p.x * 5.0) * (1.0 - smoothstep(0.6, 1.0, abs(p.y)));
        rgb = in.color.rgb * g * in.color.a;
        a = 0.0;
    } else {  // glow
        float g = exp(-r2 * 5.0) - 0.0067;
        rgb = in.color.rgb * max(g, 0.0) * in.color.a;
        a = 0.0;
    }
    float fogAmt = fogFactor(f, in.worldPos);
    if (look == 2 || look == 4 || look == 5 || look == 6 || look == 7) {
        rgb = mix(rgb, f.fog.rgb * a, fogAmt);
    } else {
        rgb *= 1.0 - fogAmt;
    }
    EffectOut o;
    o.color = float4(rgb, a) * soft;
    return o;
}

// ---------------------------------------------------------------------------
// Volumetric fluids: a 3D Eulerian gas solver (compute) and a ray marcher (fragment).
//   scalars: r = smoke (soot / vapour), g = heat, b = fuel
//   velocity: cells per second
// Step: MacCormack advection -> combustion & sources -> vorticity confinement + buoyancy
//       + wind -> divergence -> Jacobi pressure -> projection (incompressible).
// ---------------------------------------------------------------------------

struct FluidParams {
    float4 dims;     // nx, ny, nz, cell size (m)
    float4 step;     // dt, time, seed, burst multiplier
    float4 source;   // xyz source center (cells), radius (cells)
    float4 feed;     // fuel/s, heat, smoke, speed (cells/s)
    float4 physics;  // buoyancy, vorticity, turbulence, burn rate
    float4 decay;    // cooling, smoke fade, emitting, 0
    float4 wind;     // xyz wind (cells/s, box space)
};

constexpr sampler volumeSampler(coord::normalized, filter::linear, address::clamp_to_edge);

static float3 cellUVW(float3 cellPos, constant FluidParams& p) { return (cellPos + 0.5) / p.dims.xyz; }

static float hash31(float3 p) {
    p = fract(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

static float noise3(float3 p) {
    float3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i), n100 = hash31(i + float3(1, 0, 0)), n010 = hash31(i + float3(0, 1, 0)), n110 = hash31(i + float3(1, 1, 0));
    float n001 = hash31(i + float3(0, 0, 1)), n101 = hash31(i + float3(1, 0, 1)), n011 = hash31(i + float3(0, 1, 1)), n111 = hash31(i + float3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

static bool inside(uint3 g, constant FluidParams& p) { return g.x < uint(p.dims.x) && g.y < uint(p.dims.y) && g.z < uint(p.dims.z); }

static float4 readClamped(texture3d<float, access::read> t, int3 q, int3 m) { return t.read(uint3(clamp(q, int3(0), m))); }

// Pressure outside the box: open sides/top (0, gas escapes), solid floor (mirror).
static float pressureAt(texture3d<float, access::read> t, int3 q, int3 m, float self) {
    if (q.y < 0) return self;
    if (any(q < int3(0)) || any(q > m)) return 0.0;
    return t.read(uint3(q)).x;
}

kernel void fluidAdvect(texture3d<float, access::sample> src [[texture(0)]],
                        texture3d<float, access::sample> vel [[texture(1)]],
                        texture3d<float, access::write> dst [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        constant float& direction [[buffer(1)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float3 pos = float3(g);
    float3 v = vel.read(g).xyz;
    float3 back = pos - direction * p.step.x * v;
    dst.write(src.sample(volumeSampler, cellUVW(back, p)), g);
}

// MacCormack correction: second-order advection with a limiter (crisp flames, no ringing).
kernel void fluidCorrect(texture3d<float, access::sample> src [[texture(0)]],
                         texture3d<float, access::sample> vel [[texture(1)]],
                         texture3d<float, access::read> fwd [[texture(2)]],
                         texture3d<float, access::read> bwd [[texture(3)]],
                         texture3d<float, access::write> dst [[texture(4)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float3 back = float3(g) - p.step.x * vel.read(g).xyz;
    float3 b = clamp(floor(back), float3(0.0), p.dims.xyz - 1.0);
    float4 lo = float4(1e9), hi = float4(-1e9);
    for (int k = 0; k < 8; ++k) {
        float3 o = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float4 v = src.read(uint3(min(b + o, p.dims.xyz - 1.0)));
        lo = min(lo, v);
        hi = max(hi, v);
    }
    float4 r = fwd.read(g) + 0.5 * (src.read(g) - bwd.read(g));
    dst.write(clamp(r, lo, hi), g);
}

kernel void fluidCombust(texture3d<float, access::read> src [[texture(0)]],
                         texture3d<float, access::write> dst [[texture(1)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float4 s = src.read(g);
    float dt = p.step.x, time = p.step.y;
    float3 pos = float3(g) + 0.5;
    // Source: a noisy sphere fed with fuel (fire) or heat + vapour (smoke / steam).
    float r = p.source.w;
    float d = length((pos - p.source.xyz) / float3(1.0, 0.35, 1.0));  // a flat bed of fuel
    // Patchy, flickering fuel: separate tongues instead of one column.
    float flick = noise3(pos * 0.16 + float3(0.0, -time * 1.5, p.step.z)) * noise3(pos * 0.45 + float3(p.step.z, -time * 4.0, 0.0)) * 2.4;
    float inSrc = saturate(1.0 - d / max(r, 0.5)) * flick * p.decay.z;
    float feed = p.feed.x * p.step.w;
    if (p.step.w > 1.5) inSrc = max(inSrc, saturate(1.0 - d / max(r, 0.5)));  // a blast fills its whole source
    if (feed > 0.0) {
        s.b += inSrc * feed * dt * 9.0;
    } else {
        s.g += inSrc * p.feed.y * dt * 3.0;
        s.r += inSrc * p.feed.z * dt * 3.0;
    }
    // Combustion: fuel burns into heat and soot.
    float burn = min(s.b, s.b * p.physics.w * dt + 0.02 * dt);
    s.b -= burn;
    s.g += burn * p.feed.y * 2.6;
    s.r += burn * p.feed.z * 0.9;
    // Heat radiates away (faster when hotter); smoke thins.
    s.g = max(0.0, s.g - (p.decay.x * dt) * (0.25 + s.g * 0.75));
    s.r = max(0.0, s.r * (1.0 - p.decay.y * dt));
    // Soft walls: everything fades near the sides and top of the box (no hard edges).
    float3 e = min(pos, p.dims.xyz - pos);
    float wall = saturate(min(min(e.x, e.z), e.y + 4.0) / 3.0) * saturate((p.dims.y - pos.y) / 4.0);
    s.rg *= mix(1.0, wall, saturate(dt * 8.0));
    s.b = min(s.b, p.step.w > 1.5 ? 8.0 : 3.0);
    s.rg = min(s.rg, float2(6.0, 4.0));
    if (any(isnan(s)) || any(isinf(s))) s = float4(0.0);
    dst.write(s, g);
}

kernel void fluidCurl(texture3d<float, access::read> vel [[texture(0)]],
                      texture3d<float, access::write> curl [[texture(1)]],
                      constant FluidParams& p [[buffer(0)]],
                      uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float3 dx = (readClamped(vel, c + int3(1, 0, 0), m).xyz - readClamped(vel, c - int3(1, 0, 0), m).xyz) * 0.5;
    float3 dy = (readClamped(vel, c + int3(0, 1, 0), m).xyz - readClamped(vel, c - int3(0, 1, 0), m).xyz) * 0.5;
    float3 dz = (readClamped(vel, c + int3(0, 0, 1), m).xyz - readClamped(vel, c - int3(0, 0, 1), m).xyz) * 0.5;
    float3 w = float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
    curl.write(float4(w, length(w)), g);
}

kernel void fluidForces(texture3d<float, access::read> vel [[texture(0)]],
                        texture3d<float, access::read> curl [[texture(1)]],
                        texture3d<float, access::read> scal [[texture(2)]],
                        texture3d<float, access::write> dst [[texture(3)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    float dt = p.step.x, time = p.step.y;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float3 v = vel.read(g).xyz;
    float4 s = scal.read(g);
    float invCell = 1.0 / p.dims.w;
    // Buoyancy: heat lifts, soot weighs a little.
    v.y += (p.physics.x * s.g * 9.0 - s.r * 0.6) * invCell * dt;
    // Vorticity confinement: feeds back the small swirls numerical diffusion eats.
    float3 grad = float3(readClamped(curl, c + int3(1, 0, 0), m).w - readClamped(curl, c - int3(1, 0, 0), m).w,
                         readClamped(curl, c + int3(0, 1, 0), m).w - readClamped(curl, c - int3(0, 1, 0), m).w,
                         readClamped(curl, c + int3(0, 0, 1), m).w - readClamped(curl, c - int3(0, 0, 1), m).w) * 0.5;
    float gl = length(grad);
    if (gl > 1e-5) {
        float3 N = grad / gl;
        float3 w = curl.read(g).xyz;
        v += cross(N, w) * p.physics.y * 2.2 * dt;
    }
    // Source jet with turbulence, and the wind (where there is gas to carry).
    float3 pos = float3(g) + 0.5;
    float d = length((pos - p.source.xyz) / float3(1.0, 0.35, 1.0));
    float inSrc = saturate(1.0 - d / max(p.source.w, 0.5)) * p.decay.z;
    float3 n = float3(noise3(pos * 0.21 + float3(time * 1.7, 0, p.step.z)), noise3(pos * 0.21 + float3(7.1, time * 1.3, 2.0)),
                      noise3(pos * 0.21 + float3(3.3, 1.9, time * 1.9))) - 0.5;
    v += (float3(0.0, p.feed.w, 0.0) - v) * inSrc * saturate(dt * 6.0);
    if (p.step.w > 1.5) {  // burst (explosions): a violent radial blast from the source
        float3 out = pos - p.source.xyz;
        float od = length(out);
        float blast = saturate(1.0 - od / max(p.source.w * 2.5, 1.0));
        v += (out / max(od, 1e-3)) * p.feed.w * blast * saturate(dt * 12.0) * 1.5;
    }
    v += n * p.physics.z * 40.0 * dt * (inSrc + saturate(s.g) * 0.5);
    float gas = saturate(s.r * 2.0 + s.g);
    v += (p.wind.xyz - v) * gas * saturate(dt * 0.8);
    v *= 1.0 - saturate(dt * 0.05);
    // Stability: never move more than a few cells per step (CFL) and never keep a NaN.
    float vmax = 2.5 / max(dt, 1e-4);
    float vl = length(v);
    if (vl > vmax) v *= vmax / vl;
    if (any(isnan(v)) || any(isinf(v))) v = float3(0.0);
    dst.write(float4(v, 0.0), g);
}

kernel void fluidDivergence(texture3d<float, access::read> vel [[texture(0)]],
                            texture3d<float, access::write> div [[texture(1)]],
                            constant FluidParams& p [[buffer(0)]],
                            uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float below = c.y > 0 ? readClamped(vel, c - int3(0, 1, 0), m).y : 0.0;  // solid floor
    float d = 0.5 * ((readClamped(vel, c + int3(1, 0, 0), m).x - readClamped(vel, c - int3(1, 0, 0), m).x) +
                     (readClamped(vel, c + int3(0, 1, 0), m).y - below) +
                     (readClamped(vel, c + int3(0, 0, 1), m).z - readClamped(vel, c - int3(0, 0, 1), m).z));
    div.write(float4(d), g);
}

kernel void fluidJacobi(texture3d<float, access::read> pin [[texture(0)]],
                        texture3d<float, access::read> div [[texture(1)]],
                        texture3d<float, access::write> pout [[texture(2)]],
                        constant FluidParams& p [[buffer(0)]],
                        uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float self = pin.read(g).x;
    float sum = pressureAt(pin, c + int3(1, 0, 0), m, self) + pressureAt(pin, c - int3(1, 0, 0), m, self) +
                pressureAt(pin, c + int3(0, 1, 0), m, self) + pressureAt(pin, c - int3(0, 1, 0), m, self) +
                pressureAt(pin, c + int3(0, 0, 1), m, self) + pressureAt(pin, c - int3(0, 0, 1), m, self);
    pout.write(float4((sum - div.read(g).x) / 6.0), g);
}

kernel void fluidProject(texture3d<float, access::read> vel [[texture(0)]],
                         texture3d<float, access::read> pr [[texture(1)]],
                         texture3d<float, access::write> dst [[texture(2)]],
                         constant FluidParams& p [[buffer(0)]],
                         uint3 g [[thread_position_in_grid]]) {
    if (!inside(g, p)) return;
    int3 m = int3(p.dims.xyz) - 1;
    int3 c = int3(g);
    float self = pr.read(g).x;
    float3 grad = 0.5 * float3(pressureAt(pr, c + int3(1, 0, 0), m, self) - pressureAt(pr, c - int3(1, 0, 0), m, self),
                               pressureAt(pr, c + int3(0, 1, 0), m, self) - pressureAt(pr, c - int3(0, 1, 0), m, self),
                               pressureAt(pr, c + int3(0, 0, 1), m, self) - pressureAt(pr, c - int3(0, 0, 1), m, self));
    float3 v = vel.read(g).xyz - grad;
    if (g.y == 0) v.y = max(v.y, 0.0);
    float vmax = 2.5 / max(p.step.x, 1e-4);
    float vl = length(v);
    if (vl > vmax) v *= vmax / vl;
    if (any(isnan(v)) || any(isinf(v))) v = float3(0.0);
    dst.write(float4(v, 0.0), g);
}

// --- Ray marching ---------------------------------------------------------------------------

struct VolumeUniforms {
    float4x4 model;     // box space (bottom-center origin, meters) -> world
    float4x4 invModel;
    float4 size;        // xyz box size (m)
    float4 flame;       // x intensity, y temperature (K), z smoke density, w steps
    float4 smokeColor;  // rgb linear
    float4 glow;        // rgb light color (linear) * light, w = jitter seed
};

struct VolumeOut {
    float4 position [[position]];
    float3 worldPos;
};

vertex VolumeOut volumeVertex(uint vid [[vertex_id]], const device Vertex* verts [[buffer(0)]],
                              constant VolumeUniforms& u [[buffer(1)]], constant FrameUniforms& f [[buffer(2)]]) {
    float3 lp = float3(verts[vid].position);  // unit cube, -0.5..0.5
    float3 boxP = float3(lp.x * u.size.x, (lp.y + 0.5) * u.size.y, lp.z * u.size.z);
    float4 w = u.model * float4(boxP, 1.0);
    VolumeOut o;
    o.position = f.viewProj * w;
    o.worldPos = w.xyz;
    return o;
}

// Blackbody color (Kelvin -> linear RGB, normalized), after Tanner Helland's fit.
static float3 blackbody(float k) {
    float t = clamp(k, 1000.0, 12000.0) / 100.0;
    float r = t <= 66.0 ? 1.0 : saturate(1.2929 * pow(t - 60.0, -0.1332));
    float g = t <= 66.0 ? saturate(0.3901 * log(t) - 0.6318) : saturate(1.1299 * pow(t - 60.0, -0.0755));
    float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : saturate(0.5432 * log(t - 10.0) - 1.1962));
    float3 c = float3(r, g, b);
    return pow(c, float3(2.2));  // the fit is in sRGB
}

fragment EffectOut volumeFragment(VolumeOut in [[stage_in]],
                                constant VolumeUniforms& u [[buffer(0)]],
                                constant FrameUniforms& f [[buffer(1)]],
                                constant GPULight* lights [[buffer(2)]],
                                texture3d<float> scal [[texture(0)]],
                                texturecube<float> envTex [[texture(5)]],
                                depth2d<float> sceneDepth [[texture(7)]]) {
    float3 camW = f.cameraPos.xyz;
    float3 ro = (u.invModel * float4(camW, 1.0)).xyz;
    float3 rdW = normalize(in.worldPos - camW);
    float3 rd = normalize((u.invModel * float4(rdW, 0.0)).xyz);
    float3 bmin = float3(-u.size.x * 0.5, 0.0, -u.size.z * 0.5), bmax = float3(u.size.x * 0.5, u.size.y, u.size.z * 0.5);
    float3 inv = 1.0 / rd;
    float3 t0 = (bmin - ro) * inv, t1 = (bmax - ro) * inv;
    float tn = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
    float tf = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
    tn = max(tn, 0.0);
    // Stop at opaque geometry.
    float2 suv = in.position.xy * f.viewport.zw;
    float sd = sceneDepth.sample(pointClamp, suv);
    if (sd < 0.99999) {
        float3 sw = reconstructWorld(f, suv, sd);
        float sceneT = length((u.invModel * float4(sw, 1.0)).xyz - ro);
        tf = min(tf, sceneT);
    }
    if (tf <= tn) discard_fragment();

    int steps = int(u.flame.w);
    float dt = (tf - tn) / float(steps);
    float jitter = interleavedGradientNoise(in.position.xy + u.glow.w * 37.0);
    float3 L = normalize((u.invModel * float4(-f.sunDir.xyz, 0.0)).xyz);
    float3 sunRad = f.sunColor.rgb * f.sunDir.w;
    float3 amb = envTex.sample(cubeSampler, float3(0, 1, 0), level(f.extra.z)).rgb * f.ground.w * 2.0;
    float cosT = dot(rdW, -f.sunDir.xyz);
    float g = 0.35;
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosT, 1.5) * 0.0796;
    float3 C = float3(0.0);
    float T = 1.0;
    float tempK = u.flame.y;
    float sunStep = u.size.y * 0.07;
    for (int i = 0; i < steps && T > 0.01; ++i) {
        float t = tn + (float(i) + jitter) * dt;
        float3 pB = ro + rd * t;
        float3 uvw = (pB - bmin) / (bmax - bmin);
        float4 s = scal.sample(volumeSampler, uvw);
        float smoke = s.r * u.flame.z;
        float heat = s.g;
        if (smoke + heat < 0.002) continue;
        float sigma = smoke * 6.0 + smoothstep(0.1, 0.6, heat) * 0.6;
        // Flames: blackbody emission; brightness grows steeply with temperature.
        float h = saturate(heat);
        float k = mix(800.0, tempK, h);
        float3 emit = blackbody(k) * pow(k / tempK, 4.0) * smoothstep(0.1, 0.55, heat) * min(heat, 2.0) * u.flame.x * 14.0;
        // Smoke: sun through a short shadow march, sky light, and the fire's glow from below.
        float3 lit = float3(0.0);
        if (smoke > 0.003) {
            float od = 0.0;
            for (int j = 1; j <= 5; ++j) {
                float3 q = (pB + L * (sunStep * float(j)) - bmin) / (bmax - bmin);
                if (any(q < 0.0) || any(q > 1.0)) break;
                od += scal.sample(volumeSampler, q).r * u.flame.z * 6.0 * sunStep;
            }
            float3 below = (pB - float3(0.0, u.size.y * 0.12, 0.0) - bmin) / (bmax - bmin);
            float fireBelow = saturate(scal.sample(volumeSampler, clamp(below, 0.0, 1.0)).g);
            lit = u.smokeColor.rgb * (sunRad * exp(-od) * (phase * 2.5 + 0.12) + amb * 0.55 + u.glow.rgb * fireBelow * 0.12);
            int count = int(f.params.y);
            float3 pw = (u.model * float4(pB, 1.0)).xyz;
            for (int li = 0; li < count; ++li) {
                float3 Ll;
                lit += u.smokeColor.rgb * pointLightAt(lights[li], pw, -rdW, Ll) * 0.22;
            }
        }
        float a = exp(-sigma * dt);
        float3 src = emit + lit * smoke * 6.0;
        C += T * src * (1.0 - a) / max(sigma, 1e-4);
        T *= a;
    }
    float alpha = 1.0 - T;
    float3 center = (u.model * float4(0.0, u.size.y * 0.5, 0.0, 1.0)).xyz;
    float fogAmt = fogFactor(f, center);
    C = mix(C, f.fog.rgb * alpha, fogAmt);
    EffectOut o;
    o.color = float4(C, alpha);
    return o;
}
