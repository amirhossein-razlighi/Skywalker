// ---------------------------------------------------------------------------
// Debug views (render/DebugViews.h). Surface views replace a lit surface's color with a
// diagnostic one (FrameUniforms.debug.x = view id); the renderer then shows the main pass
// color untonemapped, so the colors here are exactly what the image shows. Overdraw and the
// wireframe lines use their own pipelines (overdrawFragment, wireframeFragment).
// ---------------------------------------------------------------------------

constant int kDbgWireframe = 11;
constant int kDbgOverdraw = 12;
constant int kDbgUnshaded = 13;
constant int kDbgLightingOnly = 14;
constant int kDbgCascades = 15;
constant int kDbgLightComplexity = 16;
constant int kDbgLod = 17;
constant int kDbgEmission = 18;
constant int kDbgSpecular = 19;
constant int kDbgUvChecker = 20;
constant int kDbgTexelDensity = 21;

static int debugMode(constant FrameUniforms& f) { return int(f.debug.x + 0.5); }

// Surface views that replace the color entirely (lighting_only only changes the material).
static bool debugReplacesColor(int mode) { return mode >= kDbgWireframe && mode != kDbgLightingOnly && mode != kDbgOverdraw; }

// Discrete heat ramp shared by overdraw and light complexity (counts): 0 black, 1 dark blue,
// 2 blue, 3 cyan, 4 green, 5-6 yellow, 7-9 orange, 10-15 red, 16+ white.
static float3 debugHeat(float n) {
    if (n < 0.5) return float3(0.02);
    if (n < 1.5) return float3(0.03, 0.06, 0.40);
    if (n < 2.5) return float3(0.08, 0.30, 0.95);
    if (n < 3.5) return float3(0.10, 0.80, 0.85);
    if (n < 4.5) return float3(0.12, 0.80, 0.20);
    if (n < 6.5) return float3(0.95, 0.85, 0.10);
    if (n < 9.5) return float3(0.98, 0.45, 0.05);
    if (n < 15.5) return float3(0.90, 0.08, 0.06);
    return float3(1.0);
}

// LOD colors: green 0, yellow 1, orange 2, red 3, magenta 4+, purple = impostor (lod < 0).
static float3 debugLodColor(float lod) {
    if (lod < -0.5) return float3(0.45, 0.20, 0.95);
    int l = int(lod + 0.5);
    if (l <= 0) return float3(0.15, 0.85, 0.20);
    if (l == 1) return float3(0.95, 0.90, 0.15);
    if (l == 2) return float3(0.98, 0.55, 0.10);
    if (l == 3) return float3(0.95, 0.15, 0.10);
    return float3(0.95, 0.15, 0.85);
}

struct DebugSurface {
    float3 worldPos;
    float3 N;          // shading normal
    float2 uv;         // UV0 (untiled)
    float2 texUV;      // the UVs the base color texture is sampled with (tiled)
    float texSize;     // base color texture width in texels (0 = untextured)
    float3 albedo;
    float3 emissive;
    float metallic;
    float roughness;
    float lod;         // mesh LOD (0 = finest), -1 = impostor
    float lights;      // point/spot lights evaluated in this pixel's cluster
};

// Gentle form shading so heat maps still read as geometry.
static float debugShade(DebugSurface s, constant FrameUniforms& f) {
    float3 V = f.cameraForward.w > 0.5 ? -f.cameraForward.xyz : normalize(f.cameraPos.xyz - s.worldPos);
    float3 L = -f.sunDir.xyz;
    return 0.45 + 0.35 * saturate(dot(s.N, L)) + 0.2 * saturate(dot(s.N, V));
}

static float3 debugSurfaceColor(int mode, DebugSurface s, constant FrameUniforms& f) {
    float shade = debugShade(s, f);
    if (mode == kDbgWireframe) return float3(0.045, 0.05, 0.06) * shade;
    if (mode == kDbgUnshaded) return s.albedo + s.emissive;
    if (mode == kDbgCascades) {
        float d = dot(s.worldPos - f.cameraPos.xyz, f.cameraForward.xyz);
        const float3 tint[4] = {float3(0.95, 0.25, 0.20), float3(0.25, 0.85, 0.30), float3(0.25, 0.45, 0.95), float3(0.95, 0.85, 0.20)};
        for (int i = 0; i < 4; ++i) {
            if (d <= f.cascadeSplits[i]) return tint[i] * shade;
        }
        return float3(0.35) * shade;
    }
    if (mode == kDbgLightComplexity) return debugHeat(s.lights) * (0.55 + 0.45 * shade);
    if (mode == kDbgLod) return debugLodColor(s.lod) * shade;
    if (mode == kDbgEmission) return s.emissive / (1.0 + s.emissive) + float3(0.02) * shade;
    if (mode == kDbgSpecular) {
        float3 F0 = mix(float3(0.04), s.albedo, saturate(s.metallic));
        return F0 * mix(0.25, 1.0, 1.0 - saturate(s.roughness)) * (0.6 + 0.4 * shade);
    }
    if (mode == kDbgUvChecker) {
        float2 c = floor(s.uv * 8.0);
        float check = fmod(abs(c.x + c.y), 2.0) > 0.5 ? 1.0 : 0.3;
        float2 w = fwidth(s.uv * 8.0);
        check = mix(0.65, check, saturate(1.5 - max(w.x, w.y) * 1.5));  // fade to gray where cells go sub-pixel
        float2 uvf = fract(s.uv);
        float3 tint = float3(0.35 + 0.65 * uvf.x, 0.35 + 0.65 * uvf.y, 0.6);
        return tint * check * shade;
    }
    if (mode == kDbgTexelDensity) {
        if (s.texSize <= 1.5) return float3(0.4) * shade;
        float dw = length(fwidth(s.worldPos));
        float du = length(fwidth(s.texUV)) * s.texSize;
        float density = du / max(dw, 1e-6);  // texels per meter
        float t = clamp(log2(max(density, 1e-3) / 512.0), -2.0, 2.0);  // 128 .. 2048
        const float3 ramp[5] = {float3(0.15, 0.25, 0.95), float3(0.15, 0.80, 0.90), float3(0.15, 0.85, 0.25),
                                float3(0.95, 0.85, 0.15), float3(0.95, 0.15, 0.10)};
        float x = t + 2.0;
        int i = min(int(floor(x)), 3);
        return mix(ramp[i], ramp[i + 1], x - float(i)) * shade;
    }
    return s.albedo;
}

// Wireframe lines (re-drawn over the opaque meshes with fill mode lines, depth-tested).
fragment MainOut wireframeFragment() {
    return mainOut(float4(0.25, 0.85, 1.0, 1.0), float3(0.0), 1.0, float3(0, 1, 0), 1.0, kGbufNoLighting);
}

// Overdraw: every fragment adds 1 (additive blend, no depth test); the debug pass maps counts to heat.
fragment MainOut overdrawFragment() {
    return mainOut(float4(1.0, 1.0, 1.0, 1.0), float3(0.0), 1.0, float3(0, 1, 0), 1.0, kGbufNoLighting);
}
