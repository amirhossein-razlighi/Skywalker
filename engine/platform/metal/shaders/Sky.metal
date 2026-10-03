// ---------------------------------------------------------------------------
// Sky: artist gradient or a physically inspired single-scattering atmosphere,
// plus procedural clouds and stars.
// ---------------------------------------------------------------------------

static float2 raySphere(float3 ro, float3 rd, float radius) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return float2(-1.0);
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

static float3 atmosphere(float3 rd, float3 toSun, float sunIntensity) {
    const float Rg = 6360e3, Ra = 6420e3, Hr = 8e3, Hm = 1.2e3;
    const float3 betaR = float3(5.5e-6, 13.0e-6, 22.4e-6);
    const float betaM = 21e-6;
    float3 ro = float3(0.0, Rg + 50.0, 0.0);
    rd.y = max(rd.y, -0.02);
    rd = normalize(rd);
    float2 t = raySphere(ro, rd, Ra);
    float tMax = t.y;
    const int N = 12, M = 4;
    float seg = tMax / float(N);
    float odR = 0.0, odM = 0.0;
    float3 sumR = 0.0, sumM = 0.0;
    for (int i = 0; i < N; ++i) {
        float3 p = ro + rd * (seg * (float(i) + 0.5));
        float h = length(p) - Rg;
        float hr = exp(-h / Hr) * seg, hm = exp(-h / Hm) * seg;
        odR += hr;
        odM += hm;
        float2 ts = raySphere(p, toSun, Ra);
        float segL = ts.y / float(M);
        float odRL = 0.0, odML = 0.0;
        for (int j = 0; j < M; ++j) {
            float3 q = p + toSun * (segL * (float(j) + 0.5));
            float hl = max(length(q) - Rg, 0.0);
            odRL += exp(-hl / Hr) * segL;
            odML += exp(-hl / Hm) * segL;
        }
        float3 tau = betaR * (odR + odRL) + betaM * 1.1 * (odM + odML);
        float3 att = exp(-tau);
        sumR += att * hr;
        sumM += att * hm;
    }
    float mu = dot(rd, toSun);
    float phaseR = 3.0 / (16.0 * M_PI_F) * (1.0 + mu * mu);
    const float g = 0.76;
    float phaseM = 3.0 / (8.0 * M_PI_F) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    return (sumR * betaR * phaseR + sumM * betaM * phaseM) * sunIntensity * 9.0;
}

static float3 starField(float3 dir, float time) {
    float3 a = abs(dir);
    float2 uv = a.x > a.y && a.x > a.z ? dir.yz / a.x : (a.y > a.z ? dir.xz / a.y : dir.xy / a.z);
    float face = a.x > a.y && a.x > a.z ? (dir.x > 0 ? 0.0 : 1.0) : (a.y > a.z ? (dir.y > 0 ? 2.0 : 3.0) : (dir.z > 0 ? 4.0 : 5.0));
    float2 cell = floor(uv * 180.0);
    float2 f = fract(uv * 180.0) - 0.5;
    float h = hash12(cell + face * 131.7);
    if (h < 0.965) return float3(0.0);
    float2 off = float2(hash12(cell * 1.7 + 3.1), hash12(cell * 2.3 + 7.7)) - 0.5;
    float d = length(f - off * 0.6);
    float b = smoothstep(0.12, 0.0, d) * (h - 0.965) * 28.0;
    float tw = 0.7 + 0.3 * sin(time * (1.5 + h * 4.0) + h * 40.0);
    float3 tintC = mix(float3(0.75, 0.82, 1.0), float3(1.0, 0.86, 0.7), hash12(cell + 9.0));
    return tintC * b * tw;
}

static float3 skyColor(float3 dir, constant FrameUniforms& f, bool withClouds) {
    float3 toSun = -f.sunDir.xyz;
    float3 sky;
    if (f.sky.x > 0.5) {
        sky = atmosphere(dir, toSun, max(f.sunDir.w, 0.05));
        float below = saturate(-dir.y * 6.0);
        sky = mix(sky, f.ground.rgb * max(f.sunDir.w, 0.05) * 0.25 * saturate(toSun.y + 0.2), below);
    } else {
        float t = saturate(dir.y * 1.4 + 0.05);
        sky = mix(f.skyHorizon.rgb, f.skyTop.rgb, pow(t, 0.6));
        float below = saturate(-dir.y * 4.0);
        sky = mix(sky, mix(f.skyHorizon.rgb, f.ground.rgb, 0.45), below);
    }
    // Night: stars fade in as the sky darkens.
    if (f.sky.z > 0.0 && dir.y > -0.05) {
        float dark = saturate(1.0 - dot(sky, float3(0.3, 0.5, 0.2)) * 3.0);
        sky += starField(dir, f.cameraPos.w) * f.sky.z * dark * saturate(dir.y * 8.0 + 0.4);
    }
    // Sun disc + glow
    float sd = max(dot(dir, toSun), 0.0);
    float size = max(f.sunColor.w, 0.1);
    float disc = smoothstep(cos(0.0095 * size), cos(0.0075 * size), sd);
    float above = smoothstep(-0.03, 0.01, toSun.y);
    sky += f.sunColor.rgb * (disc * 14.0 + pow(sd, 12.0 / size) * 0.18) * above * (f.sky.x > 0.5 ? 0.6 : 1.0);
    // Clouds on a virtual plane
    if (withClouds && f.sky.y > 0.0 && dir.y > 0.0) {
        float2 p = dir.xz / (dir.y + 0.08) * 0.55 + float2(f.cameraPos.w * 0.006, f.cameraPos.w * 0.002);
        float n = fbm(p * 1.6);
        float cover = f.sky.y;
        float density = smoothstep(1.0 - cover, 1.0 - cover + 0.32, n);
        float shade = fbm(p * 1.6 + toSun.xz * 0.12);
        float3 sunLit = f.sunColor.rgb * max(f.sunDir.w, 0.05) * (0.55 + 0.6 * saturate(n - shade + 0.3));
        float3 ambient = mix(f.skyHorizon.rgb, f.skyTop.rgb, 0.5) * 0.7 + float3(0.06);
        float3 cloud = (ambient + sunLit * 0.6) * mix(1.0, 0.65, density);
        float horizonFade = smoothstep(0.0, 0.18, dir.y);
        sky = mix(sky, cloud, density * horizonFade * 0.92);
    }
    return sky;
}

// Equirectangular panorama (skyMode "hdri"); `lod` < 0 samples with automatic mip selection.
static float3 panorama(float3 dir, constant FrameUniforms& f, texture2d<float> pano, float lod) {
    float phi = atan2(dir.x, -dir.z) + f.hdri.x;
    float2 uv = float2(phi * (0.5 / M_PI_F) + 0.5, acos(clamp(dir.y, -1.0, 1.0)) / M_PI_F);
    float3 c = lod < 0.0 ? pano.sample(panoSampler, uv).rgb : pano.sample(panoSampler, uv, level(lod)).rgb;
    return c * f.hdri.y;
}

fragment MainOut skyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                             texture2d<float> pano [[texture(0)]]) {
    float4 farP = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float4 nearP = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);
    float3 c = f.sky.x > 1.5 ? panorama(dir, f, pano, 0.0) : skyColor(dir, f, true);
    // Height fog veils the horizon when the fog is dense.
    float fogAmt = saturate(f.fog.w * 60.0) * (1.0 - smoothstep(0.0, 0.35, dir.y));
    c = mix(c, f.fog.rgb, fogAmt);
    return mainOutFlat(float4(c, 1.0));  // linear HDR; tonemapped in the composite pass
}

// ---------------------------------------------------------------------------
// Environment cubemap: sky -> cube faces, GGX prefilter per mip, BRDF LUT
// ---------------------------------------------------------------------------

static float3 cubeDir(float face, float2 uv) {
    float2 p = uv * 2.0 - 1.0;
    int fi = int(face + 0.5);
    float3 d;
    if (fi == 0) d = float3(1.0, -p.y, -p.x);
    else if (fi == 1) d = float3(-1.0, -p.y, p.x);
    else if (fi == 2) d = float3(p.x, 1.0, p.y);
    else if (fi == 3) d = float3(p.x, -1.0, -p.y);
    else if (fi == 4) d = float3(p.x, -p.y, 1.0);
    else d = float3(-p.x, -p.y, -1.0);
    return normalize(d);
}

fragment float4 envSkyFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                               constant EnvUniforms& e [[buffer(1)]], texture2d<float> pano [[texture(0)]]) {
    float3 dir = cubeDir(e.face.x, uvOf(in));
    if (f.sky.x > 1.5) {  // a photographed panorama already contains its ground
        float3 p = panorama(dir, f, pano, f.hdri.z);
        return float4(min(p, float3(48.0)), 1.0);
    }
    float3 c = skyColor(dir, f, true);
    // Below the horizon, reflect a ground tinted by the ambient color and fog.
    if (dir.y < 0.0) {
        float3 groundC = f.ground.rgb * (max(f.sunDir.w, 0.0) * saturate(-f.sunDir.y) * 0.35 + 0.4);
        c = mix(c, groundC, saturate(-dir.y * 5.0));
    }
    c = mix(c, f.fog.rgb, saturate(f.fog.w * 30.0) * (1.0 - saturate(abs(dir.y) * 3.0)));
    return float4(min(c, float3(48.0)), 1.0);
}

static float2 hammersley(uint i, uint n) {
    uint b = i;
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float2(float(i) / float(n), float(b) * 2.3283064365386963e-10);
}

static float3 importanceGGX(float2 xi, float3 N, float a) {
    float phi = 2.0 * M_PI_F * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    float3 H = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

fragment float4 envPrefilterFragment(FullscreenOut in [[stage_in]], constant EnvUniforms& e [[buffer(0)]],
                                     texturecube<float> src [[texture(0)]]) {
    float3 N = cubeDir(e.face.x, uvOf(in));
    float rough = e.face.y;
    if (rough < 0.01) return float4(src.sample(cubeSampler, N, level(0.0)).rgb, 1.0);
    float a = rough * rough;
    const uint COUNT = 96;
    float3 sum = 0.0;
    float wsum = 0.0;
    float saTexel = 4.0 * M_PI_F / (6.0 * e.face.z * e.face.z);
    for (uint i = 0; i < COUNT; ++i) {
        float3 H = importanceGGX(hammersley(i, COUNT), N, a);
        float3 L = normalize(2.0 * dot(N, H) * H - N);
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;
        float NdotH = saturate(dot(N, H));
        float pdf = D_GGX(NdotH, a) * 0.25 + 1e-4;
        float saSample = 1.0 / (float(COUNT) * pdf);
        float mip = rough < 0.02 ? 0.0 : max(0.5 * log2(saSample / saTexel) + 1.0, 0.0);
        sum += src.sample(cubeSampler, L, level(mip)).rgb * NdotL;
        wsum += NdotL;
    }
    return float4(sum / max(wsum, 1e-4), 1.0);
}

fragment float2 brdfLutFragment(FullscreenOut in [[stage_in]]) {
    float2 uv = uvOf(in);
    float NdotV = max(uv.x, 1e-3);
    float rough = 1.0 - uv.y;
    float a = rough * rough;
    float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    float A = 0.0, B = 0.0;
    const uint COUNT = 256;
    for (uint i = 0; i < COUNT; ++i) {
        float3 H = importanceGGX(hammersley(i, COUNT), float3(0, 0, 1), a);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = saturate(L.z), NdotH = saturate(H.z), VdotH = saturate(dot(V, H));
        if (NdotL > 0.0) {
            float G = V_SmithGGX(NdotV, NdotL, a) * 4.0 * NdotL;
            float Gv = G * VdotH / max(NdotH * NdotV, 1e-4) * NdotV;
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * Gv;
            B += Fc * Gv;
        }
    }
    return float2(A, B) / float(COUNT);
}
