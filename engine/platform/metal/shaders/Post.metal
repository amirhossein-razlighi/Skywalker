// ---------------------------------------------------------------------------
// Present (copy offscreen color to the drawable)
// ---------------------------------------------------------------------------

fragment float4 presentFragment(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    return src.sample(presentSampler, uvOf(in));
}

// ---------------------------------------------------------------------------
// Post-processing: bloom (threshold -> downsample chain -> additive upsample) and the
// final composite (AO on indirect light, white balance, exposure, bloom, tonemap,
// saturation/contrast, vignette, dither).
// ---------------------------------------------------------------------------

static float3 sampleBox4(texture2d<float> t, float2 uv, float2 texel) {
    float4 o = texel.xyxy * float4(-1.0, -1.0, 1.0, 1.0);
    return 0.25 * (t.sample(linearClamp, uv + o.xy).rgb + t.sample(linearClamp, uv + o.zy).rgb +
                   t.sample(linearClamp, uv + o.xw).rgb + t.sample(linearClamp, uv + o.zw).rgb);
}

fragment float4 bloomPrefilter(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                               constant PostUniforms& p [[buffer(0)]]) {
    float3 c = sampleBox4(src, uvOf(in), p.texel.xy) * p.params.x;
    float brightness = max(c.r, max(c.g, c.b));
    float knee = p.params.z * 0.5;
    float soft = clamp(brightness - p.params.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contribution = max(soft, brightness - p.params.z) / max(brightness, 1e-4);
    return float4(min(c * contribution, float3(64.0)), 1.0);
}

fragment float4 bloomDown(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                          constant PostUniforms& p [[buffer(0)]]) {
    return float4(sampleBox4(src, uvOf(in), p.texel.xy), 1.0);
}

fragment float4 bloomUp(FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]],
                        constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float2 t = p.texel.xy;
    // 9-tap tent filter
    float3 c = src.sample(linearClamp, uv).rgb * 4.0;
    c += (src.sample(linearClamp, uv + float2(-t.x, 0)).rgb + src.sample(linearClamp, uv + float2(t.x, 0)).rgb +
          src.sample(linearClamp, uv + float2(0, -t.y)).rgb + src.sample(linearClamp, uv + float2(0, t.y)).rgb) * 2.0;
    c += src.sample(linearClamp, uv + t).rgb + src.sample(linearClamp, uv - t).rgb +
         src.sample(linearClamp, uv + float2(t.x, -t.y)).rgb + src.sample(linearClamp, uv + float2(-t.x, t.y)).rgb;
    return float4(c / 16.0, 1.0);
}

static float3 tonemapACES(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

static float3 agxContrast(float3 x) {
    float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

static float3 tonemapAgX(float3 c) {
    const float3x3 inset = float3x3(float3(0.842479, 0.0423282, 0.0423756), float3(0.0784336, 0.878468, 0.0784336),
                                    float3(0.0792237, 0.0791661, 0.879142));
    const float3x3 outset = float3x3(float3(1.19688, -0.0528968, -0.0529716), float3(-0.0980209, 1.15190, -0.0980435),
                                     float3(-0.0990297, -0.0989612, 1.15107));
    c = inset * max(c, 1e-10);
    c = clamp(log2(c), -12.47393, 4.026069);
    c = (c + 12.47393) / 16.5;
    c = agxContrast(c);
    c = outset * c;
    return saturate(pow(max(c, 0.0), float3(2.2)));
}

static float3 tonemapNeutral(float3 c) {  // Khronos PBR Neutral
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, float3(newPeak), g);
}

static float3 hable(float3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

static float3 tonemapFilmic(float3 c) { return saturate(hable(c * 2.0) / hable(float3(11.2))); }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Temporal resolve: applies the volumetric light, then
//   mode 0: passes the frame through (no history),
//   mode 1: TAA (reprojected history, Catmull-Rom, YCoCg variance clipping),
//   mode 2: accumulation of jittered sub-samples (stills / cinematics: supersampling).
// ---------------------------------------------------------------------------

struct TemporalUniforms {
    float4 params;  // x = mode, y = accumulation weight (1/(n+1)), z = TAA feedback, w = has volumetric
    float4 texel;   // xy = full-res texel, zw = volumetric texel
};

static float3 toYCoCg(float3 c) {
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}
static float3 fromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

static float3 sceneAt(texture2d<float> lit, texture2d<float> vol, constant TemporalUniforms& t, float2 uv) {
    float3 c = lit.sample(pointClamp, uv).rgb;
    if (t.params.w > 0.5) {
        float4 v = vol.sample(linearClamp, uv);
        c = c * v.a + v.rgb;  // haze dims what lies behind it and glows where light crosses it
    }
    return min(c, float3(65000.0));
}

// 5-tap Catmull-Rom history fetch (sharper than bilinear, no ghosting blur).
static float3 sampleCatmullRom(texture2d<float> tex, float2 uv, float2 texel) {
    float2 size = 1.0 / texel;
    float2 pos = uv * size;
    float2 c = floor(pos - 0.5) + 0.5;
    float2 f = pos - c;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc12 = (c + w2 / w12) * texel;
    float2 tc0 = (c - 1.0) * texel, tc3 = (c + 2.0) * texel;
    float3 r = tex.sample(linearClamp, float2(tc12.x, tc0.y)).rgb * (w12.x * w0.y) +
               tex.sample(linearClamp, float2(tc0.x, tc12.y)).rgb * (w0.x * w12.y) +
               tex.sample(linearClamp, tc12).rgb * (w12.x * w12.y) +
               tex.sample(linearClamp, float2(tc3.x, tc12.y)).rgb * (w3.x * w12.y) +
               tex.sample(linearClamp, float2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, 0.0);
}

fragment float4 temporalFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                 constant TemporalUniforms& t [[buffer(1)]], texture2d<float> lit [[texture(0)]],
                                 texture2d<float> vol [[texture(1)]], texture2d<float> history [[texture(2)]],
                                 depth2d<float> depthTex [[texture(3)]]) {
    float2 uv = uvOf(in);
    float3 cur = sceneAt(lit, vol, t, uv);
    int mode = int(t.params.x + 0.5);
    if (mode == 0) return float4(cur, 1.0);
    if (mode == 2) {
        if (t.params.y >= 1.0) return float4(cur, 1.0);  // first sub-sample: ignore (uninitialized) history
        float3 h = history.sample(pointClamp, uv).rgb;
        return float4(mix(h, cur, t.params.y), 1.0);
    }
    // TAA: reproject with the nearest depth of the 3x3 neighborhood (keeps edges sharp).
    float3 m1 = 0.0, m2 = 0.0;
    float3 mn = float3(1e9), mx = float3(-1e9);
    float nearest = 1.0;
    float2 nearestUV = uv;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 suv = uv + float2(x, y) * t.texel.xy;
            float3 c = toYCoCg(sceneAt(lit, vol, t, suv));
            m1 += c;
            m2 += c * c;
            mn = min(mn, c);
            mx = max(mx, c);
            float d = depthTex.sample(pointClamp, suv);
            if (d < nearest) {
                nearest = d;
                nearestUV = suv;
            }
        }
    }
    float3 p = reconstructWorld(f, nearestUV, nearest);
    float4 pc = f.prevViewProj * float4(p, 1.0);
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5) + (uv - nearestUV);
    // The history is (on average) unjittered: undo this frame's sub-pixel offset. Sky pixels
    // reproject through the far plane, i.e. by camera rotation only.
    puv += f.temporal.xy * float2(0.5, -0.5);
    if (any(puv < 0.0) || any(puv > 1.0)) return float4(cur, 1.0);
    float3 h = toYCoCg(sampleCatmullRom(history, puv, t.texel.xy));
    // Variance clipping (Salvi): clip the history toward the neighborhood mean.
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 bmin = max(mean - sigma * 1.25, mn), bmax = min(mean + sigma * 1.25, mx);
    float3 center = 0.5 * (bmax + bmin), extent = 0.5 * (bmax - bmin) + 1e-4;
    float3 off = h - center;
    float3 ts = abs(off / extent);
    float tmax = max(ts.x, max(ts.y, ts.z));
    if (tmax > 1.0) h = center + off / tmax;
    float3 c = toYCoCg(cur);
    // Luma-weighted blend: fireflies cannot dominate the history.
    float feedback = t.params.z;
    float wc = (1.0 - feedback) / (1.0 + c.x), wh = feedback / (1.0 + h.x);
    float3 outC = (c * wc + h * wh) / (wc + wh);
    return float4(max(fromYCoCg(outC), 0.0), 1.0);
}

fragment float4 compositeFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]], constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float3 c = hdr.sample(linearClamp, uv).rgb;
    // Contrast-adaptive sharpening (restores detail softened by temporal filtering).
    float sharp = p.grade.z;
    if (sharp > 0.0) {
        float2 tx = p.texel.xy;
        float3 n = hdr.sample(pointClamp, uv + float2(0, -tx.y)).rgb, s2 = hdr.sample(pointClamp, uv + float2(0, tx.y)).rgb;
        float3 e = hdr.sample(pointClamp, uv + float2(tx.x, 0)).rgb, w = hdr.sample(pointClamp, uv - float2(tx.x, 0)).rgb;
        float3 mnC = min(c, min(min(n, s2), min(e, w))), mxC = max(c, max(max(n, s2), max(e, w)));
        float3 amp = sqrt(saturate(min(mnC, 2.0 - mxC) / max(mxC, 1e-4)));
        float3 wgt = -amp * mix(0.125, 0.2, saturate(sharp));
        c = max((c + (n + s2 + e + w) * wgt) / (1.0 + 4.0 * wgt), 0.0);
    }
    // White balance (approximate: warm/cool along blue-orange, tint along green-magenta)
    float3 wb = float3(1.0 + p.grade.x * 0.18 + p.grade.y * 0.06, 1.0 - p.grade.y * 0.12, 1.0 - p.grade.x * 0.22 + p.grade.y * 0.06);
    c *= wb;
    c *= p.params.x;
    c += bloom.sample(linearClamp, uv).rgb * p.params.y;
    int tm = int(p.params2.w + 0.5);
    if (tm == 0) c = tonemapACES(c);
    else if (tm == 1) c = tonemapAgX(c);
    else if (tm == 2) c = saturate(tonemapNeutral(c));
    else if (tm == 3) c = tonemapFilmic(c);
    else c = saturate(c);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(mix(float3(luma), c, p.params.w), 0.0);
    c = saturate((c - 0.5) * p.params2.x + 0.5);
    float2 v = (uv - 0.5) * float2(p.params2.z, 1.0);
    c *= 1.0 - p.params2.y * smoothstep(0.35, 1.05, length(v) * 1.15);
    // Dither before quantizing to 8 bits (removes banding in skies and fog).
    float n = interleavedGradientNoise(in.position.xy + p.texel.w * 5.588238) - 0.5;
    c += n / 255.0;
    return float4(c, 1.0);
}

// Buffer visualization (agents and artists debugging lighting): 1 albedo, 2 normals,
// 3 roughness/metallic, 4 GI, 5 reflections, 6 ambient occlusion, 7 depth, 8 direct+sky lighting.
fragment float4 debugViewFragment(FullscreenOut in [[stage_in]], constant PostUniforms& p [[buffer(0)]],
                                  texture2d<float> gbufA [[texture(0)]], texture2d<float> gbufB [[texture(1)]],
                                  texture2d<float> gi [[texture(2)]], texture2d<float> ssr [[texture(3)]],
                                  texture2d<float> ao [[texture(4)]], depth2d<float> depthTex [[texture(5)]],
                                  texture2d<float> hdr [[texture(6)]]) {
    float2 uv = uvOf(in);
    int mode = int(p.params.x + 0.5);
    float4 b = gbufB.sample(pointClamp, uv);
    float3 c = 0.0;
    if (mode == 1) c = gbufA.sample(pointClamp, uv).rgb;
    else if (mode == 2) c = b.w >= 1.5 ? float3(0.0) : octDecode(b.xy) * 0.5 + 0.5;
    else if (mode == 3) c = b.w >= 1.5 ? float3(0.0) : float3(b.z, saturate(b.w), 0.0);
    else if (mode == 4) c = tonemapACES(gi.sample(linearClamp, uv).rgb);
    else if (mode == 5) { float4 r = ssr.sample(linearClamp, uv); c = tonemapACES(r.rgb) * r.a; }
    else if (mode == 6) c = float3(ao.sample(linearClamp, uv).r);
    else if (mode == 7) { float d = depthTex.sample(pointClamp, uv); c = float3(pow(saturate(1.0 - d), 0.25)); }
    else c = tonemapACES(hdr.sample(linearClamp, uv).rgb);
    return float4(c, 1.0);
}
