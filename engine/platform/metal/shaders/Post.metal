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
                               texture2d<float> exposureTex [[texture(1)]], constant PostUniforms& p [[buffer(0)]]) {
    float autoExp = p.grade.w > 0.5 ? exposureTex.read(uint2(0, 0)).r : 1.0;
    float3 c = sampleBox4(src, uvOf(in), p.texel.xy) * p.params.x * autoExp;
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

// Camera motion vectors (uv units, current -> previous) for MetalFX temporal upscaling.
fragment float2 motionVectorFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                     depth2d<float> depthTex [[texture(0)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float3 p = reconstructWorld(f, uv, min(d, 0.999999));
    float4 pc = f.prevViewProj * float4(p, 1.0);
    if (pc.w <= 0.0) return float2(0.0);
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5) + f.temporal.xy * float2(0.5, -0.5);
    return puv - uv;
}

fragment float4 temporalFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                 constant TemporalUniforms& t [[buffer(1)]], texture2d<float> lit [[texture(0)]],
                                 texture2d<float> vol [[texture(1)]], texture2d<float> history [[texture(2)]],
                                 depth2d<float> depthTex [[texture(3)]], texture2d<float> reactive [[texture(4)]]) {
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
    // Reactive mask (GPU particles): trust the current frame where effects move over the scene,
    // so sparks and rain don't leave ghost trails.
    feedback *= 1.0 - saturate(reactive.sample(linearClamp, uv).r) * 0.85;
    float wc = (1.0 - feedback) / (1.0 + c.x), wh = feedback / (1.0 + h.x);
    float3 outC = (c * wc + h * wh) / (wc + wh);
    return float4(max(fromYCoCg(outC), 0.0), 1.0);
}

// ---------------------------------------------------------------------------
// Camera: auto exposure (center-weighted log-average metering + eye adaptation), motion
// blur (camera motion from depth reprojection), and bokeh depth of field (thin-lens circle
// of confusion, half-resolution golden-angle gather that keeps sharp foregrounds clean).
// ---------------------------------------------------------------------------

struct LensUniforms {
    float4 lens;    // x = f-stop (0 = off), y = focus distance (m, 0 = auto), z = focal length (mm), w = max CoC (half-res px)
    float4 motion;  // x = shutter (0 = off), yzw = unused
    float4 texel;   // xy = full-res texel, zw = half-res texel
    float4 view;    // x = image height (px), yzw = unused
};

fragment float2 lumaFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]]) {
    float2 uv = uvOf(in);
    float lum = dot(hdr.sample(linearClamp, uv).rgb, float3(0.2126, 0.7152, 0.0722));
    float2 d = uv - 0.5;
    float w = exp(-dot(d, d) * 5.0);  // center-weighted metering
    return float2(clamp(log2(max(lum, 1e-5)), -16.0, 16.0) * w, w);
}

fragment float exposureFragment(FullscreenOut in [[stage_in]], constant float4& p [[buffer(0)]],
                                texture2d<float> lum [[texture(0)]], texture2d<float> prev [[texture(1)]]) {
    uint last = lum.get_num_mip_levels() - 1;
    float2 v = lum.read(uint2(0, 0), last).rg;
    float avgLum = exp2(v.r / max(v.g, 1e-5));
    float target = clamp(0.16 / max(avgLum, 1e-5), exp2(p.z), exp2(p.w));
    if (p.x < 0.0) return target;
    float cur = prev.read(uint2(0, 0)).r;
    if (!(cur > 0.0) || !isfinite(cur)) return target;
    return exp2(mix(log2(cur), log2(target), 1.0 - exp(-p.x)));
}

fragment float4 motionBlurFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                   constant LensUniforms& l [[buffer(1)]], texture2d<float> src [[texture(0)]],
                                   depth2d<float> depthTex [[texture(1)]]) {
    float2 uv = uvOf(in);
    float d = depthTex.sample(pointClamp, uv);
    float3 p = reconstructWorld(f, uv, min(d, 0.999999));
    float4 pc = f.prevViewProj * float4(p, 1.0);
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    float2 vel = (uv - puv) * l.motion.x;
    float len = length(vel);
    if (len < l.texel.x * 0.5 || pc.w <= 0.0) return src.sample(pointClamp, uv);
    vel *= min(1.0, 0.06 / len);  // cap the streak
    float jitter = interleavedGradientNoise(in.position.xy) - 0.5;
    float3 sum = 0.0;
    const int N = 12;
    for (int i = 0; i < N; ++i) {
        float t = (float(i) + 0.5 + jitter) / float(N) - 0.5;
        sum += src.sample(linearClamp, uv + vel * t).rgb;
    }
    return float4(sum / float(N), 1.0);
}

// Signed circle of confusion in half-resolution pixels (negative = in front of focus).
static float circleOfConfusion(constant FrameUniforms& f, constant LensUniforms& l, float2 uv, float depth, float focus) {
    float3 p = reconstructWorld(f, uv, min(depth, 0.999999));
    float z = max(dot(p - f.cameraPos.xyz, f.cameraForward.xyz), 0.01) * 1000.0;  // mm
    float zf = focus * 1000.0;
    float fl = l.lens.z;
    float A = fl / max(l.lens.x, 0.5);
    float cocMM = A * fl * (z - zf) / (z * max(zf - fl, 1.0));
    float px = cocMM / 24.0 * l.view.x * 0.5;  // 24 mm sensor height, half resolution
    return clamp(px, -l.lens.w, l.lens.w);
}

static float focusDistance(constant FrameUniforms& f, constant LensUniforms& l, depth2d<float> depthTex) {
    if (l.lens.y > 0.0) return l.lens.y;
    float sum = 0.0;
    for (int i = 0; i < 5; ++i) {
        float2 o = float2(i == 1 ? 0.02 : (i == 2 ? -0.02 : 0.0), i == 3 ? 0.02 : (i == 4 ? -0.02 : 0.0));
        float d = depthTex.sample(pointClamp, float2(0.5) + o);
        float3 p = reconstructWorld(f, float2(0.5) + o, min(d, 0.999999));
        sum += min(dot(p - f.cameraPos.xyz, f.cameraForward.xyz), 5000.0);
    }
    return max(sum / 5.0, 0.2);
}

fragment float4 dofCocFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                               constant LensUniforms& l [[buffer(1)]], texture2d<float> src [[texture(0)]],
                               depth2d<float> depthTex [[texture(1)]]) {
    float2 uv = uvOf(in);
    float focus = focusDistance(f, l, depthTex);
    // Nearest depth of the 2x2 footprint: foreground edges keep their blur.
    float d = 1.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) d = min(d, depthTex.sample(pointClamp, uv + (float2(x, y) - 0.5) * l.texel.xy));
    }
    return float4(src.sample(linearClamp, uv).rgb, circleOfConfusion(f, l, uv, d, focus));
}

fragment float4 dofBlurFragment(FullscreenOut in [[stage_in]], constant LensUniforms& l [[buffer(1)]],
                                texture2d<float> cocTex [[texture(0)]]) {
    float2 uv = uvOf(in);
    float4 center = cocTex.sample(pointClamp, uv);
    float centerSize = abs(center.a);
    float3 color = center.rgb;
    float tot = 1.0;
    const float kGolden = 2.39996323;
    float radius = 0.5;
    float maxR = l.lens.w;
    for (float ang = 0.0; radius < maxR; ang += kGolden) {
        float2 tc = uv + float2(cos(ang), sin(ang)) * l.texel.zw * radius;
        float4 s = cocTex.sample(linearClamp, tc);
        float size = abs(s.a);
        if (s.a > center.a) size = clamp(size, 0.0, centerSize * 2.0);  // background never blurs over a sharper foreground
        float m = smoothstep(radius - 0.5, radius + 0.5, size);
        color += mix(color / tot, s.rgb, m);
        tot += 1.0;
        radius += 0.85 / radius;
    }
    return float4(color / tot, center.a);
}

fragment float4 dofCombineFragment(FullscreenOut in [[stage_in]], constant FrameUniforms& f [[buffer(0)]],
                                   constant LensUniforms& l [[buffer(1)]], texture2d<float> sharp [[texture(0)]],
                                   texture2d<float> blurred [[texture(1)]], depth2d<float> depthTex [[texture(2)]]) {
    float2 uv = uvOf(in);
    float focus = focusDistance(f, l, depthTex);
    float coc = abs(circleOfConfusion(f, l, uv, depthTex.sample(pointClamp, uv), focus));
    float4 b = blurred.sample(linearClamp, uv);
    float amount = saturate(max(coc, abs(b.a)) - 0.6);
    return float4(mix(sharp.sample(pointClamp, uv).rgb, b.rgb, amount), 1.0);
}

struct GradeUniforms {
    float4 params;  // x = auto exposure on, y = look/LUT strength, z = grain, w = chromatic aberration
};

fragment float4 compositeFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]], texture2d<float> exposureTex [[texture(2)]],
                                  texture3d<float> lut [[texture(3)]], constant PostUniforms& p [[buffer(0)]],
                                  constant GradeUniforms& g [[buffer(1)]]) {
    float2 uv = uvOf(in);
    float3 c = hdr.sample(linearClamp, uv).rgb;
    if (g.params.w > 0.0) {  // chromatic aberration: red and blue focus at slightly different scales
        float2 off = (uv - 0.5) * g.params.w * 0.012;
        c.r = hdr.sample(linearClamp, uv - off).r;
        c.b = hdr.sample(linearClamp, uv + off).b;
    }
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
    float exposure = p.params.x * (g.params.x > 0.5 ? exposureTex.read(uint2(0, 0)).r : 1.0);
    c *= exposure;
    c += bloom.sample(linearClamp, uv).rgb * p.params.y;  // bloom is already exposed (prefilter)
    int tm = int(p.params2.w + 0.5);
    if (tm == 0) c = tonemapACES(c);
    else if (tm == 1) c = tonemapAgX(c);
    else if (tm == 2) c = saturate(tonemapNeutral(c));
    else if (tm == 3) c = tonemapFilmic(c);
    else c = saturate(c);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(mix(float3(luma), c, p.params.w), 0.0);
    c = saturate((c - 0.5) * p.params2.x + 0.5);
    if (g.params.y > 0.0) {  // look / 3D LUT (in display space)
        float n = float(lut.get_width());
        float3 l = lut.sample(linearClamp, saturate(c) * ((n - 1.0) / n) + 0.5 / n).rgb;
        c = mix(c, l, g.params.y);
    }
    float2 v = (uv - 0.5) * float2(p.params2.z, 1.0);
    c *= 1.0 - p.params2.y * smoothstep(0.35, 1.05, length(v) * 1.15);
    // Dither before quantizing to 8 bits (removes banding in skies and fog).
    if (g.params.z > 0.0) {  // film grain: strongest in the mid-tones
        float gn = hash12(floor(in.position.xy) + float2(p.texel.w * 17.0, p.texel.w * 7.0)) - 0.5;
        float lumC = dot(c, float3(0.2126, 0.7152, 0.0722));
        c = saturate(c + gn * g.params.z * 0.11 * (1.0 - abs(lumC - 0.5) * 1.2));
    }
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
    else if (mode == 9) {
        // Sketch: pencil contours from depth/normal discontinuities + cross-hatching by light.
        float2 tx = p.texel.xy;
        float d0 = depthTex.sample(pointClamp, uv);
        float3 n0 = b.w >= 1.5 ? float3(0.0) : octDecode(b.xy);
        float edge = 0.0;
        for (int k = 0; k < 4; ++k) {
            float2 o = (k == 0 ? float2(1, 0) : k == 1 ? float2(-1, 0) : k == 2 ? float2(0, 1) : float2(0, -1)) * tx * 1.2;
            float d1 = depthTex.sample(pointClamp, uv + o);
            float4 b1 = gbufB.sample(pointClamp, uv + o);
            float3 n1 = b1.w >= 1.5 ? float3(0.0) : octDecode(b1.xy);
            // Depth: relative jump in linear-ish depth; normals: crease angle.
            float z0 = 1.0 / max(1.0 - d0, 1e-5), z1 = 1.0 / max(1.0 - d1, 1e-5);
            edge = max(edge, smoothstep(0.015, 0.05, abs(z1 - z0) / max(min(z0, z1), 1e-3)));
            edge = max(edge, smoothstep(0.5, 0.9, 1.0 - dot(n0, n1)) * (d0 < 1.0 ? 0.7 : 0.0));
        }
        float lum = dot(tonemapACES(hdr.sample(linearClamp, uv).rgb), float3(0.3, 0.55, 0.15));
        float2 px = in.position.xy;
        float h1 = step(0.5, fract((px.x + px.y) * 0.125));
        float h2 = step(0.5, fract((px.x - px.y) * 0.125));
        float hatch = (1.0 - smoothstep(0.2, 0.32, lum)) * (1.0 - h1) * 0.28 + (1.0 - smoothstep(0.06, 0.14, lum)) * (1.0 - h2) * 0.3;
        float paperGrain = hash12(floor(px)) * 0.04;
        float3 paper = float3(0.965, 0.955, 0.93) - paperGrain;
        float3 ink = float3(0.16, 0.17, 0.2);
        c = mix(paper, ink, saturate(edge * 0.95 + (d0 < 1.0 ? hatch : 0.0)));
    }
    else c = tonemapACES(hdr.sample(linearClamp, uv).rgb);
    return float4(c, 1.0);
}
