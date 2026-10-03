// ---------------------------------------------------------------------------
// Volumetric clouds: a curved cloud layer above the ground, shaped by tileable
// Perlin-Worley and Worley noise volumes (generated once on the GPU), coverage from a
// drifting weather field, ray marched with energy-conserving integration, Beer + powder
// lighting, a dual-lobe phase function and an approximation of multiple scattering.
// Also provides cheap cloud shadows for surfaces lit by the sun.
// ---------------------------------------------------------------------------

constant float kEarthRadius = 6360e3;

struct CloudParams {
    float coverage, base, thickness, density;
    float scale, time, speed, mode;
    float2 wind;  // xz direction
};

static CloudParams cloudParams(constant FrameUniforms& f) {
    CloudParams c;
    c.coverage = f.sky.y;
    c.base = f.clouds.y;
    c.thickness = max(f.clouds.z, 100.0);
    c.density = f.clouds.w;
    c.scale = max(f.clouds2.x, 0.05);
    c.speed = f.clouds2.y;
    c.mode = f.clouds2.z;
    c.wind = f.clouds2.w > -1e6 ? float2(sin(f.clouds2.w), cos(f.clouds2.w)) : float2(0, 1);
    c.time = f.cameraPos.w;
    return c;
}

// --- Tileable noise generation (compute, once) ------------------------------------------

static float3 cloudHash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7, 74.7)), dot(p, float3(269.5, 183.3, 246.1)), dot(p, float3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

static float worleyTile(float3 p, float period) {
    float3 id = floor(p), fp = fract(p);
    float minD = 1.0;
    for (int z = -1; z <= 1; ++z) {
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                float3 o = float3(x, y, z);
                float3 cell = fmod(id + o + period, period);
                float3 d = o + cloudHash33(cell) - fp;
                minD = min(minD, dot(d, d));
            }
        }
    }
    return 1.0 - sqrt(minD);
}

static float worleyFbmTile(float3 uvw, float freq) {
    return worleyTile(uvw * freq, freq) * 0.625 + worleyTile(uvw * freq * 2.0, freq * 2.0) * 0.25 +
           worleyTile(uvw * freq * 4.0, freq * 4.0) * 0.125;
}

static float perlinTile(float3 p, float period) {
    float3 i = floor(p), f = fract(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float r = 0.0;
    float n[8];
    for (int k = 0; k < 8; ++k) {
        float3 o = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        float3 g = normalize(cloudHash33(fmod(i + o, period)) * 2.0 - 1.0);
        n[k] = dot(g, f - o);
    }
    float x00 = mix(n[0], n[1], u.x), x10 = mix(n[2], n[3], u.x), x01 = mix(n[4], n[5], u.x), x11 = mix(n[6], n[7], u.x);
    r = mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
    return r;
}

static float remap(float v, float lo, float hi, float nlo, float nhi) { return nlo + (v - lo) / max(hi - lo, 1e-5) * (nhi - nlo); }

kernel void cloudShapeKernel(texture3d<float, access::write> out [[texture(0)]], uint3 gid [[thread_position_in_grid]]) {
    float size = float(out.get_width());
    if (any(float3(gid) >= size)) return;
    float3 uvw = (float3(gid) + 0.5) / size;
    float pf = 0.0, amp = 0.5, freq = 4.0;
    for (int o = 0; o < 5; ++o) {
        pf += perlinTile(uvw * freq, freq) * amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    pf = saturate(pf * 0.9 + 0.5);
    float w = worleyFbmTile(uvw, 4.0);
    float perlinWorley = remap(pf, 0.0, 1.0, w, 1.0);  // billowy: Perlin carved by Worley cells
    out.write(float4(saturate(perlinWorley), worleyFbmTile(uvw, 4.0), worleyFbmTile(uvw, 8.0), worleyFbmTile(uvw, 16.0)), gid);
}

kernel void cloudDetailKernel(texture3d<float, access::write> out [[texture(0)]], uint3 gid [[thread_position_in_grid]]) {
    float size = float(out.get_width());
    if (any(float3(gid) >= size)) return;
    float3 uvw = (float3(gid) + 0.5) / size;
    out.write(float4(worleyFbmTile(uvw, 2.0), worleyFbmTile(uvw, 4.0), worleyFbmTile(uvw, 8.0), 1.0), gid);
}

// --- Density, lighting, marching ----------------------------------------------------------

constexpr sampler cloudSampler(coord::normalized, filter::linear, address::repeat);

static float cloudHeight01(float3 p, CloudParams c) {
    float r = length(p + float3(0, kEarthRadius, 0)) - kEarthRadius;
    return (r - c.base) / c.thickness;
}

static float cloudDensity(float3 p, float h, CloudParams c, texture3d<float> shape, texture3d<float> detail, bool cheap) {
    if (h <= 0.0 || h >= 1.0) return 0.0;
    float2 drift = c.wind * c.speed * c.time;
    float3 q = p + float3(drift.x, 0.0, drift.y) + float3(c.wind.x, 0.0, c.wind.y) * h * 400.0;  // tops lean downwind
    // Weather: large patches of cloud and clear sky.
    float2 wuv = (p.xz + drift * 0.6) / (11000.0 * c.scale);
    float weather = valueNoise(wuv) * 0.6 + valueNoise(wuv * 2.7 + 13.1) * 0.4;
    // Coverage maps to the fraction of sky covered: weather decides where the cloud fields are.
    float cov = saturate((weather - (1.0 - c.coverage) * 0.75) * 2.2) * saturate(c.coverage * 1.6);
    // Cumulus profile: rounded base, billowing towers that reach higher where coverage is high.
    float top = mix(0.45, 1.0, saturate(weather * 1.3));
    float grad = saturate(remap(h, 0.0, 0.1, 0.0, 1.0)) * saturate(remap(h, top * 0.55, top, 1.0, 0.0));
    float4 s = shape.sample(cloudSampler, q / (4200.0 * c.scale));
    float fbm = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
    float base = remap(s.r, fbm - 1.0, 1.0, 0.0, 1.0) * grad;
    base = saturate(remap(base, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    if (base <= 0.0 || cheap) return base * c.density;
    float3 d = detail.sample(cloudSampler, q / (700.0 * c.scale) + float3(0.0, c.time * 0.01, 0.0)).rgb;
    float dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    float erode = mix(dfbm, 1.0 - dfbm, saturate(h * 6.0));  // wispy bottoms, cauliflower tops
    base = saturate(remap(base, erode * 0.38, 1.0, 0.0, 1.0));
    return base * c.density;
}

static float hgPhase(float cosT, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * M_PI_F * pow(1.0 + g2 - 2.0 * g * cosT, 1.5));
}

static float2 cloudShell(float3 ro, float3 rd, float radius) {
    float3 o = ro + float3(0, kEarthRadius, 0);
    float b = dot(o, rd), cc = dot(o, o) - radius * radius;
    float h = b * b - cc;
    if (h < 0.0) return float2(-1.0);
    h = sqrt(h);
    return float2(-b - h, -b + h);
}

/// Marches the cloud layer along a view ray. Returns (in-scattered light, transmittance).
static float4 marchClouds(float3 ro, float3 rd, CloudParams c, float3 toSun, float3 sunRad, float3 ambTop, float3 ambBottom,
                          texture3d<float> shape, texture3d<float> detail, int steps, float jitter, float maxDist) {
    if (c.coverage <= 0.001) return float4(0, 0, 0, 1);
    float rIn = kEarthRadius + c.base, rOut = kEarthRadius + c.base + c.thickness;
    float camR = length(ro + float3(0, kEarthRadius, 0));
    float t0, t1;
    float2 sIn = cloudShell(ro, rd, rIn), sOut = cloudShell(ro, rd, rOut);
    if (camR < rIn) {  // below the layer
        if (sIn.y < 0.0) return float4(0, 0, 0, 1);
        t0 = sIn.y;
        t1 = sOut.y;
    } else if (camR < rOut) {  // inside
        t0 = 0.0;
        t1 = sIn.x > 0.0 ? sIn.x : sOut.y;
    } else {  // above
        if (sOut.x < 0.0) return float4(0, 0, 0, 1);
        t0 = sOut.x;
        t1 = sIn.x > 0.0 ? sIn.x : sOut.y;
    }
    t1 = min(t1, t0 + c.thickness * 6.0);
    if (t0 > maxDist) return float4(0, 0, 0, 1);
    t1 = min(t1, maxDist);
    float ds = (t1 - t0) / float(steps);
    float cosT = dot(rd, toSun);
    float3 L = 0.0;
    float T = 1.0;
    float firstHit = -1.0;
    const float sigmaK = 0.02;  // extinction per meter at density 1
    for (int i = 0; i < steps; ++i) {
        float t = t0 + (float(i) + jitter) * ds;
        float3 p = ro + rd * t;
        float h = cloudHeight01(p, c);
        float dens = cloudDensity(p, h, c, shape, detail, false);
        if (dens <= 0.002) continue;
        if (firstHit < 0.0) firstHit = t;
        // Light march toward the sun (growing steps, cheap density far away).
        // Distances grow geometrically: fine detail near the sample, coarse self-shadow far away.
        const float lightT[6] = {12.0, 40.0, 95.0, 200.0, 420.0, 900.0};
        float od = 0.0, prevT = 0.0;
        for (int j = 0; j < 6; ++j) {
            float3 q = p + toSun * lightT[j];
            od += cloudDensity(q, cloudHeight01(q, c), c, shape, detail, j > 2) * (lightT[j] - prevT);
            prevT = lightT[j];
        }
        // Multiple scattering (octaves with weaker extinction and broader phase).
        float3 sun = 0.0;
        float a = 1.0, b = 1.0, cg = 1.0;
        for (int o = 0; o < 3; ++o) {
            float phase = mix(hgPhase(cosT, 0.78 * cg), hgPhase(cosT, -0.25 * cg), 0.25);
            sun += sunRad * a * phase * exp(-od * sigmaK * b);
            a *= 0.5;
            b *= 0.3;
            cg *= 0.5;
        }
        float powder = 1.0 - exp(-od * sigmaK * 2.0);
        float3 amb = mix(ambBottom, ambTop, saturate(h)) * (0.6 + 0.4 * h);
        float sigma = max(dens * sigmaK, 1e-6);
        float3 S = (sun * 4.0 * M_PI_F * mix(1.0, powder, 0.6) + amb) * sigma;
        float Ti = exp(-sigma * ds);
        L += T * (S - S * Ti) / sigma;
        T *= Ti;
        if (T < 0.01) break;
    }
    // Aerial perspective: distant clouds fade into the horizon haze.
    if (firstHit > 0.0) {
        float fade = 1.0 - exp(-firstHit / 38000.0);
        L = mix(L, ambBottom * 1.2 * (1.0 - T), fade * 0.6);
        T = mix(T, 1.0, fade * fade * 0.5);
    }
    return float4(L, T);
}

/// Fraction of sunlight reaching `p` through the cloud layer (cheap: three density taps).
static float cloudShadow(float3 p, float3 toSun, constant FrameUniforms& f, texture3d<float> shape) {
    CloudParams c = cloudParams(f);
    if (c.coverage <= 0.001 || c.mode > 0.5 || toSun.y < 0.02) return 1.0;
    float od = 0.0;
    for (int i = 0; i < 3; ++i) {
        float h = (float(i) + 0.5) / 3.0;
        float tt = (c.base + c.thickness * h - p.y) / toSun.y;
        if (tt < 0.0) continue;
        float3 q = p + toSun * tt;
        od += cloudDensity(q, h, c, shape, shape, true) * c.thickness / 3.0;
    }
    return mix(1.0, exp(-od * 0.004), 0.85);
}
