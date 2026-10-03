// ---------------------------------------------------------------------------
// Strand hair and fur.
//
// Simulation (compute)
//   hairReset        guides = rest pose transformed by the model matrix
//   hairSimulate     one thread per guide strand: Verlet integration (gravity, gusting wind
//                    drag), global + local shape stiffness, follow-the-leader length
//                    constraints, sphere / capsule / plane collisions
//   hairInterpolate  one thread per rendered strand: base curve from 3 guides, parallel-
//                    transport frames, stored offsets (clumps, curls, waves, frizz)
// Rendering
//   hairVertex       instanced triangle strips: segments expanded into camera-facing ribbons;
//                    sub-pixel strands are widened to ~1 px with proportionally lower coverage
//   hairFragment     stochastic coverage -> MSAA sample mask (smooth under TAA / accumulation),
//                    Marschner shading (R, TT, TRT; Karis 2016 real-time fit) with melanin
//                    absorption, multiple-scattering approximation, deep-opacity self-shadowing,
//                    external shadows from the cascades, sky ambient; writes the G-buffer
//                    with a hair flag (screen-space GI/SSR leave it alone)
//   hairShadow*      stochastic hair coverage into the sun's cascades (shadows on the face)
//   hairDom*         deep opacity map: front depth of the hair + 4 cumulative density layers
//                    (and the depth of opaque meshes around the groom) from the sun
//   hairCard*        distant LOD: guide strands as wide cards with procedural strand alpha
// ---------------------------------------------------------------------------

struct HairParams {
    float4x4 model;
    float4x4 domViewProj;  // world -> deep opacity map clip space (orthographic)
    float4 dims;           // x points per strand, y strands drawn, z guides, w model scale
    float4 width;          // x root width (m), y tip width (m), z density, w coverage boost (LOD)
    float4 sigma;          // xyz absorption per strand diameter, w color variation
    float4 dye;            // rgb dye, w specular
    float4 rootColor;      // rgb, w longitudinal roughness
    float4 tipColor;       // rgb, w azimuthal roughness
    float4 shade;          // x scatter, y cuticle tilt (rad), z ambient occlusion strength, w 0
    float4 dom;            // x layer thickness (m), y depth range (m), z texel size (m), w has DOM
    float4 view;           // x pixel size at 1 m (or per texel), y orthographic, z frame, w 0
    float4 sim;            // x dt, y time, z damping, w stiffness
    float4 sim2;           // x root stiffness, y gravity (m/s^2), z collider count, w wind drag
    float4 wind;           // xyz wind (m/s), w gust
    float4 cards;          // x card width (m), y strands per card, z strand stride (shadow passes), w 0
    float4 colliders[16];  // [a.xyz, kind], [b.xyz, radius]
};

struct HairChild {
    float4 root;     // xyz mesh-local root, w length scale
    float4 weights;  // xyz guide weights, w random
    uint4 guides;    // xyz guide indices, w width multiplier (float bits)
};

// Large so that even one hair sample in a pixel keeps the MSAA-resolved flag above 1.5: screen-space
// GI / reflections then leave hair pixels alone (hair has its own ambient and scattering).
constant float kGbufHair = 12.0;

static float3 hairRotateFromTo(float3 a, float3 b, float3 v) {
    a = normalize(a);
    b = normalize(b);
    float3 axis = cross(a, b);
    float s = length(axis), c = dot(a, b);
    if (s < 1e-6) return c > 0.0 ? v : -v;
    axis /= s;
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

static bool hairPushOut(float4 a, float4 b, thread float3& p, float margin) {
    int kind = int(a.w + 0.5);
    if (kind == 2) {
        float d = dot(p - a.xyz, b.xyz);
        if (d < margin) {
            p += b.xyz * (margin - d);
            return true;
        }
        return false;
    }
    float3 c = a.xyz;
    if (kind == 1) {
        float3 ab = b.xyz - a.xyz;
        float t = clamp(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
        c = a.xyz + ab * t;
    }
    float3 d = p - c;
    float l = length(d), r = b.w + margin;
    if (l < r) {
        p = c + (l > 1e-6 ? d / l : float3(0, 1, 0)) * r;
        return true;
    }
    return false;
}

kernel void hairReset(uint id [[thread_position_in_grid]], constant HairParams& H [[buffer(0)]],
                      device const float4* rest [[buffer(1)]], device float4* pos [[buffer(2)]],
                      device float4* prev [[buffer(3)]]) {
    uint n = uint(H.dims.z) * uint(H.dims.x);
    if (id >= n) return;
    float4 w = float4((H.model * float4(rest[id].xyz, 1.0)).xyz, 0.0);
    pos[id] = w;
    prev[id] = w;
}

kernel void hairSimulate(uint g [[thread_position_in_grid]], constant HairParams& H [[buffer(0)]],
                         device const float4* rest [[buffer(1)]], device float4* pos [[buffer(2)]],
                         device float4* prev [[buffer(3)]]) {
    uint G = uint(H.dims.z), P = uint(H.dims.x);
    if (g >= G) return;
    device const float4* r = rest + g * P;
    device float4* x = pos + g * P;
    device float4* xp = prev + g * P;
    float dt = max(H.sim.x, 1e-4);
    float3 target[32], cur[32];
    for (uint k = 0; k < P; ++k) target[k] = (H.model * float4(r[k].xyz, 1.0)).xyz;
    cur[0] = target[0];
    x[0] = float4(target[0], 0.0);
    xp[0] = float4(target[0], 0.0);
    float damping = H.sim.z, stiffness = H.sim.w, rootStiff = H.sim2.x;
    float phase = float(g) * 0.6180339;
    float gust = 1.0 + H.wind.w * (0.55 * sin(H.sim.y * 1.3 + phase * 6.0) + 0.45 * sin(H.sim.y * 3.7 + phase * 11.0));
    for (uint k = 1; k < P; ++k) {
        float tk = float(k) / float(P - 1);
        float3 pk = x[k].xyz, pp = xp[k].xyz;
        float3 v = (pk - pp) * (1.0 - damping);
        float3 windV = H.wind.xyz * gust;
        float3 acc = float3(0.0, -H.sim2.y, 0.0) + (windV - v / dt) * H.sim2.w * tk;
        float3 nx = pk + v + acc * dt * dt;
        // Global shape: pull toward the groomed (rest) pose, strongly at the roots.
        float sg = mix(rootStiff, stiffness * 0.35, sqrt(tk));
        nx = mix(nx, target[k], 1.0 - pow(1.0 - sg * 0.3, dt * 60.0));
        cur[k] = nx;
        xp[k] = float4(pk, 0.0);
    }
    int nc = int(H.sim2.z + 0.5);
    for (uint k = 1; k < P; ++k) {
        float tk = float(k) / float(P - 1);
        float3 restSeg = target[k] - target[k - 1];
        float restLen = length(restSeg);
        // Local shape: the rest segment carried along by the parent segment's rotation.
        float3 desired = k >= 2 ? hairRotateFromTo(target[k - 1] - target[k - 2], cur[k - 1] - cur[k - 2], restSeg) : restSeg;
        float ls = mix(rootStiff, stiffness, tk) * 0.6;
        float3 d = mix(cur[k] - cur[k - 1], desired, ls);
        float3 np = cur[k - 1] + normalize(d + 1e-9) * restLen;
        float margin = 0.002;
        for (int c = 0; c < nc; ++c) hairPushOut(H.colliders[c * 2], H.colliders[c * 2 + 1], np, margin);
        np = cur[k - 1] + normalize(np - cur[k - 1] + 1e-9) * restLen;  // inextensible
        for (int c = 0; c < nc; ++c) hairPushOut(H.colliders[c * 2], H.colliders[c * 2 + 1], np, margin * 0.5);
        cur[k] = np;
        x[k] = float4(np, 0.0);
    }
}

// Base curve of a child at point k: its root plus the weighted displacement of its guides.
static float3 hairBase(HairChild c, float3 root, device const float4* guides, uint P, uint k) {
    float u = float(k) * c.root.w;
    uint i0 = min(uint(u), P - 2);
    float f = u - float(i0);
    float3 b = root;
    for (uint j = 0; j < 3; ++j) {
        float w = c.weights[j];
        if (w == 0.0) continue;
        device const float4* gp = guides + c.guides[j] * P;
        b += (mix(gp[i0].xyz, gp[i0 + 1].xyz, f) - gp[0].xyz) * w;
    }
    return b;
}

// Rebuilds rendered strands from the guides. Parallel-transport frames are computed on the fly
// (a rolling window, no per-thread arrays) exactly like fx::transportFrames on the CPU.
kernel void hairInterpolate(uint i [[thread_position_in_grid]], constant HairParams& H [[buffer(0)]],
                            device const HairChild* children [[buffer(1)]], device const half4* offsets [[buffer(2)]],
                            device const float4* guides [[buffer(3)]], device float4* out [[buffer(4)]]) {
    uint N = uint(H.dims.y), P = uint(H.dims.x);
    if (i >= N) return;
    HairChild c = children[i];
    float3 root = (H.model * float4(c.root.xyz, 1.0)).xyz;
    float3 ref = normalize((H.model * float4(1.0, 0.0, 0.0, 0.0)).xyz);
    float scale = H.dims.w;
    // w packs the strand's width multiplier (integer part, 1/256 steps) and random value (fraction),
    // so drawing reads one float4 per point and nothing else.
    float packed = floor(as_type<float>(c.guides.w) * 256.0 + 0.5) + c.weights.w * 0.999;
    float3 bPrev = root, bCur = hairBase(c, root, guides, P, 0), bNext = hairBase(c, root, guides, P, 1);
    float3 prevT = float3(0, 1, 0), Nk = float3(1, 0, 0);
    for (uint k = 0; k < P; ++k) {
        float3 d = k + 1 < P ? bNext - bCur : bCur - bPrev;
        float l = length(d);
        float3 T = l > 1e-12 ? d / l : prevT;
        prevT = T;
        if (k == 0) {
            float3 n0 = ref - T * dot(ref, T);
            if (length(n0) < 0.1) {
                float3 alt = normalize((H.model * float4(0.0, 0.0, 1.0, 0.0)).xyz);
                n0 = alt - T * dot(alt, T);
            }
            float l0 = length(n0);
            Nk = l0 > 1e-12 ? n0 / l0 : float3(1, 0, 0);
        } else {
            float3 n = Nk - T * dot(Nk, T);
            float ln = length(n);
            Nk = ln > 1e-12 ? n / ln : Nk;
        }
        float3 o = float3(offsets[i * P + k].xyz);
        out[i * P + k] = float4(bCur + (T * o.x + Nk * o.y + cross(T, Nk) * o.z) * scale, packed);
        bPrev = bCur;
        bCur = bNext;
        if (k + 2 < P) bNext = hairBase(c, root, guides, P, k + 2);
    }
}

// --- shading ----------------------------------------------------------------------------------

static float hairGauss(float B, float x) { return exp(-0.5 * x * x / (B * B)) / (2.5066283 * B); }

static float hairFresnel(float cosT) {
    const float F0 = 0.04652;  // ((1 - 1.55) / (1 + 1.55))^2
    float m = 1.0 - saturate(cosT);
    return F0 + (1.0 - F0) * m * m * m * m * m;
}

// Marschner hair BSDF, real-time fit (Karis 2016). T points from the tip toward the root.
static float3 hairBSDF(float3 T, float3 V, float3 L, float3 C, float rough, float radial, float tilt, float specular,
                       float scatter, float shadow) {
    float VoL = dot(V, L);
    float sinL = clamp(dot(T, L), -1.0, 1.0), sinV = clamp(dot(T, V), -1.0, 1.0);
    float cosThetaD = max(cos(0.5 * abs(asin(sinV) - asin(sinL))), 0.05);
    float3 Lp = L - sinL * T, Vp = V - sinV * T;
    float cosPhi = dot(Lp, Vp) * rsqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
    float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));
    float nPrime = 1.19 / cosThetaD + 0.36 * cosThetaD;
    float r2 = rough * rough;
    float B0 = r2, B1 = r2 * 0.5, B2 = r2 * 2.0;
    float3 S = 0.0;
    // R: white primary highlight, shifted toward the tip by the cuticle tilt.
    {
        float sa = sin(-tilt * 2.0), ca = cos(-tilt * 2.0);
        float shift = 2.0 * sa * (ca * cosHalfPhi * sqrt(max(0.0, 1.0 - sinV * sinV)) + sa * sinV);
        float Mp = hairGauss(B0 * 1.4142 * cosHalfPhi + 1e-3, sinL + sinV - shift);
        float Np = 0.25 * cosHalfPhi;
        float Fp = hairFresnel(sqrt(saturate(0.5 + 0.5 * VoL)));
        S += Mp * Np * Fp * specular;
    }
    // TT: light through the strand (backlit glow), colored by absorption.
    {
        float Mp = hairGauss(B1, sinL + sinV - tilt);
        float a = 1.0 / nPrime;
        float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
        float f = hairFresnel(cosThetaD * sqrt(saturate(1.0 - h * h)));
        float Fp = (1.0 - f) * (1.0 - f);
        float3 Tp = pow(C, 0.5 * sqrt(max(0.0, 1.0 - h * h * a * a)) / cosThetaD);
        float Np = exp((-3.65 * cosPhi - 3.98) * (1.25 - radial * 0.5));
        S += Mp * Np * Fp * Tp;
    }
    // TRT: colored secondary highlight, shifted toward the root.
    {
        float Mp = hairGauss(B2, sinL + sinV - tilt * 4.0);
        float f = hairFresnel(cosThetaD * 0.5);
        float Fp = (1.0 - f) * (1.0 - f) * f;
        float3 Tp = pow(C, 0.8 / cosThetaD);
        float Np = exp(17.0 * cosPhi - 16.78) * (1.0 - radial * 0.4) + 0.02 * radial;
        S += Mp * Np * Fp * Tp;
    }
    // Multiple scattering: soft Kajiya-like diffusion through the hair volume, tinted more
    // strongly where the light has travelled through many strands (light hair glows).
    {
        float kajiya = 1.0 - abs(sinL);
        float3 Nf = normalize(V - T * sinV + 1e-5);
        float wrapNoL = saturate((dot(Nf, L) + 1.0) * 0.25);
        float diffuse = (1.0 / M_PI_F) * mix(wrapNoL, kajiya, 0.33) * scatter;
        float luma = max(dot(C, float3(0.2126, 0.7152, 0.0722)), 1e-4);
        float3 tint = pow(C / luma, 1.0 - shadow);
        S += sqrt(C) * diffuse * tint;
    }
    return max(S, 0.0);
}

struct HairShadeIn {
    float3 worldPos;
    float3 tangent;  // root -> tip
    float t;         // 0 root .. 1 tip
    float rand;
};

// Chiang et al. 2016: absorption <-> multiple-scattered hair color (azimuthal roughness 0.3).
constant float kHairColorD = 5.889;
static float3 hairColorToAbsorption(float3 c) {
    float3 l = log(clamp(c, 1e-4, 1.0)) / kHairColorD;
    return l * l;
}

// Hair color for the Karis lobes: melanin absorption (+ per-strand variation) plus dye and
// root/tip tints expressed as extra absorption, so dyed hair keeps a white primary highlight.
static float3 hairColor(constant HairParams& H, float t, float rand) {
    float3 sigma = H.sigma.xyz * max(0.0, 1.0 + H.sigma.w * (rand - 0.5) * 2.0);
    sigma += hairColorToAbsorption(H.dye.rgb) + hairColorToAbsorption(mix(H.rootColor.rgb, H.tipColor.rgb, smoothstep(0.0, 1.0, t)));
    return clamp(exp(-sqrt(sigma) * kHairColorD), 0.0005, 0.999);
}

// Deep opacity transmittance through the hair toward the sun, opaque visibility around
// the groom, and the depth (m) of the point behind the groom's front surface.
static float3 hairDomVisibility(constant HairParams& H, float3 worldPos, depth2d<float> domDepth, depth2d<float> domOpaque,
                                texture2d<float> domDensity, float3 C, float scatter, thread float& depthBehind,
                                thread float& opaqueVis) {
    depthBehind = 0.0;
    opaqueVis = 1.0;
    if (H.dom.w < 0.5) return 1.0;
    float4 lc = H.domViewProj * float4(worldPos, 1.0);
    float3 ndc = lc.xyz / lc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float range = H.dom.y;
    float zf = domDepth.sample(pointClamp, uv);
    float zo = domOpaque.sample(pointClamp, uv);
    float zFront = min(zf, zo);
    depthBehind = max(0.0, (ndc.z - zFront) * range);
    // Opaque occluders around the groom (head, shoulders): 4-tap PCF.
    float texel = 1.0 / float(domOpaque.get_width());
    float vis = 0.0;
    for (int i = 0; i < 4; ++i) {
        float2 o = float2((i & 1) ? 0.75 : -0.75, (i & 2) ? 0.75 : -0.75) * texel;
        vis += domOpaque.sample_compare(shadowSampler, uv + o, ndc.z - 0.004 / range) ;
    }
    opaqueVis = vis * 0.25;
    float d = (ndc.z - zf) * range;
    if (d <= 0.0) return 1.0;
    float4 cum = domDensity.sample(linearClamp, uv);
    float b = H.dom.x;
    float4 edges = float4(1.0, 3.0, 7.0, 15.0) * b;
    float O;
    if (d < edges.x) O = cum.x * d / edges.x;
    else if (d < edges.y) O = mix(cum.x, cum.y, (d - edges.x) / (edges.y - edges.x));
    else if (d < edges.z) O = mix(cum.y, cum.z, (d - edges.y) / (edges.z - edges.y));
    else if (d < edges.w) O = mix(cum.z, cum.w, (d - edges.z) / (edges.w - edges.z));
    else O = cum.w;
    // Forward scattering lets colored light through light hair (dual-scattering approximation).
    float3 ext = 1.0 - saturate(scatter * 0.45) * sqrt(C);
    return exp(-O * ext);
}

static float3 hairShade(constant HairParams& H, constant FrameUniforms& f, const device GPULight* lights,
                        const device uint2* clusterCells, const device uint* clusterIndices, HairShadeIn s,
                        float2 pixel, depth2d<float> shadowAtlas, texturecube<float> envTex, depth2d<float> domDepth,
                        depth2d<float> domOpaque, texture2d<float> domDensity, thread float3& albedoOut,
                        thread float3& normalOut) {
    float3 V = normalize(f.cameraPos.xyz - s.worldPos);
    if (f.cameraForward.w > 0.5) V = -f.cameraForward.xyz;
    float3 Tt = normalize(s.tangent);
    float3 T = -Tt;  // toward the root (cuticle scales point to the tip)
    float3 C = hairColor(H, s.t, s.rand);
    float rough = H.rootColor.w, radial = H.tipColor.w, tilt = H.shade.y, specular = H.dye.w, scatter = H.shade.x;
    float3 L = -f.sunDir.xyz;
    float depthBehind, opaqueVis;
    float3 domT = hairDomVisibility(H, s.worldPos, domDepth, domOpaque, domDensity, C, scatter, depthBehind, opaqueVis);
    // External occluders (trees, walls): look up the cascades at the groom's sun-facing surface,
    // so hair and head (already in the deep opacity map) don't shadow themselves twice.
    float ext = L.y > -0.08 ? shadowFactor(s.worldPos + L * (depthBehind + 0.03), L, pixel, f, shadowAtlas) : 0.0;
    float3 vis = domT * opaqueVis * ext;
    float shadowAmt = dot(vis, float3(0.333));
    float3 color = hairBSDF(T, V, L, C, rough, radial, tilt, specular, scatter, shadowAmt) * f.sunColor.rgb * f.sunDir.w * vis;
    // Punctual lights: directional ones everywhere, point / spot lights from this pixel's cluster.
    int dirCount = int(f.cluster2.y);
    uint2 cell = clusterCells[clusterOf(f, pixel, s.worldPos)];
    int total = dirCount + int(cell.y);
    for (int k = 0; k < total; ++k) {
        int i = k < dirCount ? k : int(clusterIndices[cell.x + uint(k - dirCount)]);
        float3 Ll;
        float3 rad = pointLightAt(lights[i], s.worldPos, T, Ll);
        color += hairBSDF(T, V, Ll, C, rough, radial, tilt, specular, scatter, 1.0) * rad;
    }
    // Sky light: diffuse around the strand plus a glossy reflection, occluded inside the groom.
    float3 Nf = normalize(V - T * dot(V, T) + 1e-5);
    float ambientK = f.ground.w;
    float maxMip = f.extra.z;
    float occ = mix(1.0, saturate(dot(domT, float3(0.333)) * 0.7 + 0.3), H.shade.z) * mix(0.45, 1.0, smoothstep(0.0, 0.4, s.t));
    float3 albedo = sqrt(C) * 0.8;
    float3 irr = (envTex.sample(cubeSampler, Nf, level(maxMip)).rgb + envTex.sample(cubeSampler, float3(0, 1, 0), level(maxMip)).rgb) * 0.5;
    float3 spec = envTex.sample(cubeSampler, reflect(-V, Nf), level(clamp(rough * 1.5, 0.0, 1.0) * maxMip)).rgb;
    color += (irr * albedo * scatter + spec * 0.06 * specular * f.sky.w) * ambientK * occ;
    float fogAmt = fogFactor(f, s.worldPos);
    float3 fogC = f.fog.rgb + f.sunColor.rgb * f.sunDir.w * pow(saturate(dot(-V, L)), 8.0) * 0.25;
    color = mix(color, fogC, fogAmt);
    albedoOut = albedo;
    normalOut = Nf;
    return color;
}

// Stochastic transparency: turns fractional coverage into an MSAA sample mask that changes
// every frame (TAA) and every sub-sample (stills), so thin strands resolve smoothly.
static uint hairSampleMask(float coverage, float2 pixel, float strandRand, float frame) {
    float c = saturate(coverage);
    if (c >= 0.999) return 0xFu;
    float noise = fract(interleavedGradientNoise(pixel + float2(frame * 5.588238, frame * 3.1)) + strandRand * 0.618);
    uint n = uint(floor(c * 4.0 + noise));
    if (n == 0) return 0u;
    uint bits = (1u << n) - 1u;
    uint rot = uint(fract(noise * 7.31 + strandRand * 3.7) * 4.0) & 3u;
    return ((bits << rot) | (bits >> (4u - rot))) & 0xFu;
}

struct HairVOut {
    float4 position [[position]];
    float3 worldPos;
    float3 tangent;
    float coverage;
    float t;
    float rand [[flat]];
    float across;
};

struct HairOut {
    float4 color [[color(0)]];
    float4 gbufA [[color(1)]];
    float4 gbufB [[color(2)]];
    uint mask [[sample_mask]];
};

static float3 hairPoint(device const float4* pos, uint base, uint k) { return pos[base + k].xyz; }

// Common strand expansion: returns the world position of this strip vertex.
static float3 hairExpand(constant HairParams& H, device const float4* pos, device const HairChild* children, uint strand,
                         uint vid, float3 eye, float3 fwd, bool ortho, float pixelAt1m, thread float3& tangent,
                         thread float& coverage, thread float& t, thread float& rand) {
    uint P = uint(H.dims.x);
    uint k = min(vid >> 1, P - 1);
    float side = (vid & 1) ? 1.0 : -1.0;
    uint base = strand * P;
    float4 p4 = pos[base + k];
    float3 p = p4.xyz;
    float3 T = k + 1 < P ? pos[base + k + 1].xyz - p : p - pos[base + k - 1].xyz;
    T = dot(T, T) > 1e-14 ? normalize(T) : float3(0, 1, 0);
    float3 V = ortho ? -fwd : normalize(eye - p);
    float3 B = cross(T, V);
    B = dot(B, B) > 1e-12 ? normalize(B) : normalize(cross(T, float3(0.31, 0.92, 0.23)));
    t = float(k) / float(P - 1);
    float widthMul = floor(p4.w) * (1.0 / 256.0);
    float wPhys = mix(H.width.x, H.width.y, t) * widthMul * H.dims.w;
    float pix = ortho ? pixelAt1m : pixelAt1m * max(dot(p - eye, fwd), 1e-3);
    float wDraw = max(wPhys, pix * 0.9);
    coverage = wPhys / wDraw * H.width.z * H.width.w * (1.0 - smoothstep(0.9, 1.0, t) * 0.5);
    tangent = T;
    rand = fract(p4.w) / 0.999;
    return p + B * side * wDraw * 0.5;
}

vertex HairVOut hairVertex(uint vid [[vertex_id]], uint iid [[instance_id]], constant FrameUniforms& f [[buffer(2)]],
                           constant HairParams& H [[buffer(3)]], device const float4* pos [[buffer(4)]],
                           device const HairChild* children [[buffer(5)]]) {
    HairVOut o;
    float3 T;
    float cov, t, rnd;
    bool ortho = f.cameraForward.w > 0.5;
    float3 world = hairExpand(H, pos, children, iid, vid, f.cameraPos.xyz, f.cameraForward.xyz, ortho, H.view.x, T, cov, t, rnd);
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.tangent = T;
    o.coverage = cov;
    o.t = t;
    o.rand = rnd;
    o.across = (vid & 1) ? 1.0 : -1.0;
    return o;
}

fragment HairOut hairFragment(HairVOut in [[stage_in]], constant HairParams& H [[buffer(5)]],
                              constant FrameUniforms& f [[buffer(1)]], const device GPULight* lights [[buffer(2)]],
                              const device uint2* clusterCells [[buffer(3)]],
                              const device uint* clusterIndices [[buffer(4)]],
                              depth2d<float> shadowAtlas [[texture(1)]], texturecube<float> envTex [[texture(5)]],
                              depth2d<float> domDepth [[texture(9)]], depth2d<float> domOpaque [[texture(10)]],
                              texture2d<float> domDensity [[texture(11)]]) {
    uint mask = hairSampleMask(in.coverage, in.position.xy, in.rand, f.temporal.z * 7.0 + f.temporal.w);
    if (mask == 0u) discard_fragment();
    HairShadeIn s{in.worldPos, in.tangent, in.t, in.rand};
    float3 albedo, N;
    float3 c = hairShade(H, f, lights, clusterCells, clusterIndices, s, in.position.xy, shadowAtlas, envTex, domDepth, domOpaque, domDensity, albedo, N);
    HairOut o;
    o.color = float4(c, 1.0);
    o.gbufA = float4(albedo, 1.0);
    o.gbufB = float4(octEncode(N), H.rootColor.w, kGbufHair);
    o.mask = mask;
    return o;
}

// --- cards (distant LOD): guides as wide ribbons with procedural strands -----------------

struct HairCardOut {
    float4 position [[position]];
    float3 worldPos;
    float3 tangent;
    float t;
    float u;
    float rand [[flat]];
};

vertex HairCardOut hairCardVertex(uint vid [[vertex_id]], uint iid [[instance_id]], constant FrameUniforms& f [[buffer(2)]],
                                  constant HairParams& H [[buffer(3)]], device const float4* guides [[buffer(4)]]) {
    uint P = uint(H.dims.x);
    uint k = min(vid >> 1, P - 1);
    float side = (vid & 1) ? 1.0 : -1.0;
    uint base = iid * P;
    float3 p = guides[base + k].xyz;
    float3 T = guides[base + min(k + 1, P - 1)].xyz - guides[base + (k > 0 ? k - 1 : 0)].xyz;
    T = dot(T, T) > 1e-14 ? normalize(T) : float3(0, 1, 0);
    float3 V = f.cameraForward.w > 0.5 ? -f.cameraForward.xyz : normalize(f.cameraPos.xyz - p);
    float3 B = normalize(cross(T, V) + 1e-6);
    float t = float(k) / float(P - 1);
    float w = H.cards.x * mix(1.0, 0.45, t);
    float3 world = p + B * side * w * 0.5;
    HairCardOut o;
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.tangent = T;
    o.t = t;
    o.u = side * 0.5 + 0.5;
    o.rand = fract(float(iid) * 0.61803);
    return o;
}

fragment HairOut hairCardFragment(HairCardOut in [[stage_in]], constant HairParams& H [[buffer(5)]],
                                  constant FrameUniforms& f [[buffer(1)]], const device GPULight* lights [[buffer(2)]],
                                  const device uint2* clusterCells [[buffer(3)]],
                                  const device uint* clusterIndices [[buffer(4)]],
                                  depth2d<float> shadowAtlas [[texture(1)]], texturecube<float> envTex [[texture(5)]],
                                  depth2d<float> domDepth [[texture(9)]], depth2d<float> domOpaque [[texture(10)]],
                                  texture2d<float> domDensity [[texture(11)]]) {
    // Procedural strand pattern across the card, thinning toward the tip and the edges.
    float strands = H.cards.y;
    float x = in.u * strands + in.rand * 13.0;
    float lane = fract(x + valueNoise(float2(floor(x), in.t * 3.0)) * 0.3);
    float strand = smoothstep(0.5, 0.15, abs(lane - 0.5));
    float edge = smoothstep(0.0, 0.25, in.u) * smoothstep(1.0, 0.75, in.u);
    float alpha = strand * mix(1.0, 0.35, in.t) * mix(0.6, 1.0, edge) * H.width.z;
    uint mask = hairSampleMask(alpha, in.position.xy, in.rand, f.temporal.z * 7.0 + f.temporal.w);
    if (mask == 0u) discard_fragment();
    HairShadeIn s{in.worldPos, in.tangent, in.t, fract(floor(x) * 0.618)};
    float3 albedo, N;
    float3 c = hairShade(H, f, lights, clusterCells, clusterIndices, s, in.position.xy, shadowAtlas, envTex, domDepth, domOpaque, domDensity, albedo, N);
    HairOut o;
    o.color = float4(c, 1.0);
    o.gbufA = float4(albedo, 1.0);
    o.gbufB = float4(octEncode(N), H.rootColor.w, kGbufHair);
    o.mask = mask;
    return o;
}

// --- sun cascades (stochastic coverage, so shadows on the face are soft and fine) ------------

struct HairShadowOut {
    float4 position [[position]];
    float coverage;
    float rand [[flat]];
};

vertex HairShadowOut hairShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                      constant float4x4& lightViewProj [[buffer(2)]], constant HairParams& H [[buffer(3)]],
                                      device const float4* pos [[buffer(4)]], device const HairChild* children [[buffer(5)]],
                                      constant float4& lightInfo [[buffer(6)]]) {
    // lightInfo: xyz = direction the light travels, w = texel size (m)
    float3 T;
    float cov, t, rnd;
    // Every `stride`-th strand with `stride` times the coverage: same opacity, fraction of the cost.
    uint stride = max(uint(H.cards.z), 1u);
    float3 world = hairExpand(H, pos, children, iid * stride, vid, float3(0.0), lightInfo.xyz, true, lightInfo.w, T, cov, t, rnd);
    cov *= float(stride);
    HairShadowOut o;
    o.position = lightViewProj * float4(world, 1.0);
    o.coverage = cov;
    o.rand = rnd;
    return o;
}

fragment void hairShadowFragment(HairShadowOut in [[stage_in]]) {
    float n = fract(interleavedGradientNoise(in.position.xy) + in.rand * 0.618);
    if (n >= saturate(in.coverage)) discard_fragment();
}

// --- deep opacity map --------------------------------------------------------------------------

struct HairDomOut {
    float4 position [[position]];
    float coverage;
};

vertex HairDomOut hairDomVertex(uint vid [[vertex_id]], uint iid [[instance_id]], constant HairParams& H [[buffer(3)]],
                                device const float4* pos [[buffer(4)]], device const HairChild* children [[buffer(5)]],
                                constant float4& lightInfo [[buffer(6)]]) {
    float3 T;
    float cov, t, rnd;
    uint stride = max(uint(H.cards.z), 1u);
    float3 world = hairExpand(H, pos, children, iid * stride, vid, float3(0.0), lightInfo.xyz, true, lightInfo.w, T, cov, t, rnd);
    HairDomOut o;
    o.position = H.domViewProj * float4(world, 1.0);
    o.coverage = cov * float(stride);
    return o;
}

fragment void hairDomDepthFragment(HairDomOut in [[stage_in]]) {}

fragment float4 hairDomDensityFragment(HairDomOut in [[stage_in]], constant HairParams& H [[buffer(3)]],
                                       depth2d<float> domDepth [[texture(0)]]) {
    float zf = domDepth.read(uint2(in.position.xy));
    float d = (in.position.z - zf) * H.dom.y;
    float4 edges = float4(1.0, 3.0, 7.0, 15.0) * H.dom.x;
    float c = saturate(in.coverage);
    return float4(d < edges.x ? c : 0.0, d < edges.y ? c : 0.0, d < edges.z ? c : 0.0, d < edges.w ? c : 0.0);
}

vertex float4 hairDomMeshVertex(uint vid [[vertex_id]], const device Vertex* verts [[buffer(0)]],
                                constant float4x4& mvp [[buffer(1)]]) {
    return mvp * float4(float3(verts[vid].position), 1.0);
}
