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
