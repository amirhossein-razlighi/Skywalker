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
fragment float4 compositeFragment(FullscreenOut in [[stage_in]], texture2d<float> hdr [[texture(0)]],
                                  texture2d<float> bloom [[texture(1)]], texture2d<float> ambient [[texture(2)]],
                                  texture2d<float> ao [[texture(3)]], texture2d<float> vol [[texture(4)]],
                                  constant PostUniforms& p [[buffer(0)]]) {
    float2 uv = uvOf(in);
    float3 c = hdr.sample(linearClamp, uv).rgb;
    if (p.grade.z > 0.0) {  // volumetric light, blurred by sampling a few taps of the half-res buffer
        float2 tx = p.texel.xy * 2.0;
        float3 v = vol.sample(linearClamp, uv).rgb * 0.4 + (vol.sample(linearClamp, uv + float2(tx.x, tx.y)).rgb +
                   vol.sample(linearClamp, uv - float2(tx.x, tx.y)).rgb + vol.sample(linearClamp, uv + float2(-tx.x, tx.y)).rgb +
                   vol.sample(linearClamp, uv + float2(tx.x, -tx.y)).rgb) * 0.15;
        float T = vol.sample(linearClamp, uv).a;
        c = c * T + v;  // haze dims what lies behind it and glows where light crosses it
    }
    if (p.texel.z > 0.0) {
        float occ = mix(1.0, ao.sample(linearClamp, uv).r, saturate(p.texel.z));
        float3 amb = ambient.sample(linearClamp, uv).rgb;
        c = max(c - amb * (1.0 - occ) * max(p.texel.z, 1.0), 0.0);
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
