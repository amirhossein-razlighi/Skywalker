// ---------------------------------------------------------------------------
// Character material models (DrawUniforms.material.w = shading id, parameters in
// DrawUniforms.character[3], packed by toSurface() in assets/Material.cpp):
//
//   4 skin       pre-integrated subsurface scattering: per-channel wrapped diffusion driven by the
//                surface curvature and the scatter distance (red travels farthest), per-channel
//                normal blur (normal-map detail softens in red), softened shadow edges, dual-lobe
//                specular (skin's oily sheen over a broad lobe), procedural pore detail that fades
//                with distance, and sun transmission through thin parts from the shadow map's
//                thickness (backlit ears glow red)
//   5 eye        refracted iris (parallax under the cornea), limbus ring, shadowed corners, wet
//                cornea highlight and reflections on the smooth eyeball
//   6 cloth      Charlie sheen lobe (velvet, wool, cotton) + fiber fuzz on silhouettes
//   7 hair_card  anisotropic strand highlights along the card's UV direction (shifted white primary,
//                tinted secondary), wrapped diffuse and backlit transmission; dithered or
//                alpha-to-coverage transparency
//
// Indirect light keeps the standard split-sum form so the screen-space GI / reflection resolve stays
// consistent; skin softens its irradiance per channel.
// ---------------------------------------------------------------------------

constant int kShadeSkin = 4;
constant int kShadeEye = 5;
constant int kShadeCloth = 6;
constant int kShadeHairCard = 7;

static bool isCharacterModel(int shading) { return shading >= kShadeSkin && shading <= kShadeHairCard; }

// Tangent and bitangent along the UV axes from screen-space derivatives (no vertex tangents needed).
static void charCotangentFrame(float3 N, float3 p, float2 uv, thread float3& T, thread float3& B) {
    float3 dp1 = dfdx(p), dp2 = dfdy(p);
    float2 duv1 = dfdx(uv), duv2 = dfdy(uv);
    float3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    T = dp2perp * duv1.x + dp1perp * duv2.x;
    B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    T *= invmax;
    B *= invmax;
}

// Effective surface curvature (1/m) for the skin's diffusion: a head-sized base (1 / 8 cm) plus a
// damped share of the measured curvature. Screen-space curvature is constant per triangle, so it only
// nudges the base: tight features (nose, lips, ears) scatter a little more without faceting.
static float charCurvature(float3 Ngeo, float3 worldPos) {
    float measured = length(fwidth(Ngeo)) / max(length(fwidth(worldPos)), 1e-5);
    return 12.5 + 0.15 * min(measured, 120.0);
}

// Sun transmittance through the object behind this point: the light path length from the cascade's
// stored depth, filtered over a few shadow texels (the depth map is coarse next to a face) and turned
// into per-channel attenuation exp(-thickness / mean free path).
static float3 sunTransmittance(float3 worldPos, float3 Ngeo, float3 mfpMeters, constant FrameUniforms& f, depth2d<float> atlas) {
    if (f.params.z < 0.5) return float3(0.0);
    float viewDepth = dot(worldPos - f.cameraPos.xyz, f.cameraForward.xyz);
    int c = 0;
    if (viewDepth > f.cascadeSplits.x) c = 1;
    if (viewDepth > f.cascadeSplits.y) c = 2;
    if (viewDepth > f.cascadeSplits.z) c = 3;
    if (viewDepth > f.cascadeSplits.w) return 1e3;
    float3 p = worldPos - Ngeo * 0.004;  // just inside the surface
    float4 lc = f.cascadeViewProj[c] * float4(p, 1.0);
    float3 ndc = lc.xyz / lc.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0)) return float3(0.0);
    float2 tile = float2(c % 2, c / 2) * 0.5;
    float2 auv = tile + uv * 0.5;
    float4x4 m = f.cascadeViewProj[c];
    float zPerMeter = max(length(float3(m[0][2], m[1][2], m[2][2])), 1e-6);  // orthographic cascades: linear depth
    float2 lo = tile + f.params.w, hi = tile + 0.5 - f.params.w;
    float3 sum = 0.0;
    for (int k = 0; k < 5; ++k) {
        float2 o = (k == 0 ? float2(0) : kPoisson[k * 2] * 1.5) * f.params.w;
        float stored = atlas.sample(pointClamp, clamp(auv + o, lo, hi));
        float thickness = max(ndc.z - stored, 0.0) / zPerMeter;
        sum += exp(-thickness / mfpMeters);
    }
    return sum / 5.0;
}

// --- per-light terms ---------------------------------------------------------------------------

struct CharacterInputs {
    int model;
    float3 Ngeo;
    float curvature;
    float3 hairT;  // hair cards: strand direction (world)
};

static float3 skinLight(SurfaceData s, CharacterInputs ci, constant DrawUniforms& d, float3 V, float3 L, float3 radiance,
                        float shadow, float specScale) {
    const float3 mfp = d.character[0].rgb * d.character[0].w;  // mm the light travels, per channel
    const float3 blur = saturate(mfp / 3.0);
    float3 nr = normalize(mix(s.N, ci.Ngeo, blur.r)), ng = normalize(mix(s.N, ci.Ngeo, blur.g)), nb = normalize(mix(s.N, ci.Ngeo, blur.b));
    float3 ndl = float3(dot(nr, L), dot(ng, L), dot(nb, L));
    // Pre-integrated diffusion: light wraps past the terminator by about the scatter distance times the
    // curvature (mm x 1/m), doubled for the profile's long tail; curved, thin features wrap further.
    float3 w = saturate(ci.curvature * mfp * 0.002);
    float3 diffuse = saturate((ndl + w) / ((1.0 + w) * (1.0 + w))) * (1.0 + w * 0.5);
    // Shadow edges glow slightly warm: light diffuses a few millimeters into the penumbra (which is
    // centimeters wide in the shadow map, so only a gentle tint).
    float3 shadowC = pow(max(shadow, 0.0), 1.0 / (1.0 + 0.25 * blur));
    float NdotV = max(dot(s.N, V), 1e-4);
    float nl = saturate(dot(s.N, L));
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(s.N, H)), VdotH = saturate(dot(V, H));
    float F = 0.028 + (1.0 - 0.028) * pow(1.0 - VdotH, 5.0);
    float r1 = clamp(s.roughness * d.character[1].x, 0.08, 1.0), r2 = clamp(s.roughness * d.character[1].y, 0.08, 1.0);
    float a1 = r1 * r1, a2 = r2 * r2;
    float spec = mix(D_GGX(NdotH, a1) * V_SmithGGX(NdotV, max(nl, 1e-4), a1), D_GGX(NdotH, a2) * V_SmithGGX(NdotV, max(nl, 1e-4), a2),
                     saturate(d.character[1].z)) * F * specScale;
    return (s.albedo / M_PI_F * diffuse * shadowC * (1.0 - F) + spec * nl * shadow) * radiance * M_PI_F;
}

static float3 eyeLight(SurfaceData s, CharacterInputs ci, constant DrawUniforms& d, float3 V, float3 L, float3 radiance, float shadow,
                       float specScale) {
    float NdotL = dot(ci.Ngeo, L);
    float diff = saturate((NdotL + 0.25) / 1.25);  // a little scattering in the sclera
    float3 H = normalize(V + L);
    float NdotV = max(dot(ci.Ngeo, V), 1e-4), NdotH = saturate(dot(ci.Ngeo, H)), VdotH = saturate(dot(V, H));
    float a = max(d.character[1].x, 0.01);
    a *= a;
    float F = 0.025 + 0.975 * pow(1.0 - VdotH, 5.0);
    float spec = D_GGX(NdotH, a) * V_SmithGGX(NdotV, max(saturate(NdotL), 1e-4), a) * F * specScale;
    return (s.albedo / M_PI_F * diff * (1.0 - F) + spec * saturate(NdotL)) * radiance * shadow * M_PI_F;
}

static float3 clothLight(SurfaceData s, constant DrawUniforms& d, float3 V, float3 L, float3 radiance, float shadow, float specScale) {
    float3 N = s.N;
    float NdotL = saturate(dot(N, L)), NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H)), VdotH = saturate(dot(V, H));
    // Charlie sheen (Estevez & Kulla) with Neubelt's visibility.
    float alpha = max(d.character[0].w * d.character[0].w, 0.005);
    float sin2h = max(1.0 - NdotH * NdotH, 1e-4);
    float D = (2.0 + 1.0 / alpha) * pow(sin2h, 0.5 / alpha) / (2.0 * M_PI_F);
    float Vis = 1.0 / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 1e-4);
    float3 sheen = d.character[0].rgb * D * Vis * NdotL;
    float a = s.roughness * s.roughness;
    float F = 0.04 + 0.96 * pow(1.0 - VdotH, 5.0);
    float spec = D_GGX(NdotH, a) * V_SmithGGX(NdotV, max(NdotL, 1e-4), a) * F * 0.5 * specScale;
    float sheenMax = max(d.character[0].r, max(d.character[0].g, d.character[0].b));
    float3 diffuse = s.albedo / M_PI_F * NdotL * (1.0 - 0.2 * sheenMax);
    return (diffuse + spec * NdotL + sheen * specScale) * radiance * shadow * M_PI_F;
}

static float3 hairCardLight(SurfaceData s, CharacterInputs ci, constant DrawUniforms& d, float3 V, float3 L, float3 radiance, float shadow,
                            float specScale) {
    float3 N = s.N, T = ci.hairT;
    float shift = d.character[0].x;
    float3 T1 = normalize(T + N * shift), T2 = normalize(T + N * (shift - 0.15));
    float3 H = normalize(V + L);
    float e1 = mix(320.0, 24.0, saturate(s.roughness)), e2 = e1 * 0.45;
    float th1 = dot(T1, H), th2 = dot(T2, H);
    float spec1 = pow(sqrt(saturate(1.0 - th1 * th1)), e1) * (e1 + 2.0) / (8.0 * M_PI_F);
    float spec2 = pow(sqrt(saturate(1.0 - th2 * th2)), e2) * (e2 + 2.0) / (8.0 * M_PI_F);
    float facing = smoothstep(-0.15, 0.25, dot(N, L));
    float3 spec = (float3(spec1) + s.albedo * spec2 * 1.6) * d.character[0].y * facing * specScale;
    float3 diffuse = s.albedo / M_PI_F * saturate(mix(0.3, 1.0, dot(N, L) * 0.5 + 0.5));
    float3 through = s.albedo * pow(saturate(dot(V, -L)), 4.0) * 0.35;  // backlit hair glows
    return (diffuse + spec * 0.25 + through) * radiance * shadow * M_PI_F;
}

static float3 characterLight(SurfaceData s, CharacterInputs ci, constant DrawUniforms& d, float3 V, float3 L, float3 radiance, float shadow,
                             float specScale) {
    if (ci.model == kShadeSkin) return skinLight(s, ci, d, V, L, radiance, shadow, specScale);
    if (ci.model == kShadeEye) return eyeLight(s, ci, d, V, L, radiance, shadow, specScale);
    if (ci.model == kShadeCloth) return clothLight(s, d, V, L, radiance, shadow, specScale);
    return hairCardLight(s, ci, d, V, L, radiance, shadow, specScale);
}

// --- surface preparation ---------------------------------------------------------------------------

// Skin: procedural pores / micro detail on top of the normal map, faded out where it would alias.
static float3 skinMicroNormal(float3 N, float3 worldPos, float2 uv, constant DrawUniforms& d) {
    float strength = d.character[2].x;
    if (strength <= 0.0) return N;
    float2 p = uv * d.character[2].y;
    float fade = 1.0 - smoothstep(0.35, 0.9, length(fwidth(p)));  // cycles per pixel
    if (fade <= 0.0) return N;
    float e = 0.35;
    float h = valueNoise(p) * 0.7 + valueNoise(p * 2.7 + 11.3) * 0.3;
    float hx = valueNoise(p + float2(e, 0.0)) * 0.7 + valueNoise((p + float2(e, 0.0)) * 2.7 + 11.3) * 0.3;
    float hy = valueNoise(p + float2(0.0, e)) * 0.7 + valueNoise((p + float2(0.0, e)) * 2.7 + 11.3) * 0.3;
    float3 T, B;
    charCotangentFrame(N, worldPos, uv, T, B);
    float2 g = float2(hx - h, hy - h) / e * strength * fade * 0.35;
    return normalize(N - (T * g.x + B * g.y));
}

// Eye: refracted iris (parallax under the cornea), limbus ring and shadowed corners.
static void eyeSurface(thread SurfaceData& s, float3 Ngeo, float3 worldPos, float2 uv, float3 V, constant DrawUniforms& d,
                       texture2d<float> albedoTex, float mipBias) {
    float2 center = d.character[0].xy;
    if (d.character[2].x >= 0.0 && length(uv - d.character[2].xy) < length(uv - center)) center = d.character[2].xy;  // second eye
    float radius = max(d.character[0].z, 1e-3), depth = d.character[0].w;
    float dist = length(uv - center);
    float iris = 1.0 - smoothstep(radius * 0.92, radius, dist);
    if (d.maps.x > 0.5 && iris > 0.0 && depth > 0.0) {
        float3 T, B;
        charCotangentFrame(Ngeo, worldPos, uv, T, B);
        float3 Vt = float3(dot(V, normalize(T)), dot(V, normalize(B)), dot(V, Ngeo));
        float2 offset = Vt.xy / max(Vt.z, 0.3) * depth;
        float3 refracted = albedoTex.sample(materialSampler, uv - offset * iris, bias(mipBias)).rgb * d.color.rgb;
        s.albedo = mix(s.albedo, refracted, iris);
    }
    float ring = exp(-pow((dist - radius) / (radius * 0.16), 2.0));
    s.albedo *= 1.0 - d.character[1].z * ring;
    s.ao *= 1.0 - d.character[1].y * smoothstep(radius * 1.5, radius * 3.2, dist);
    s.N = Ngeo;  // the cornea is smooth
    s.roughness = clamp(d.character[1].x, 0.02, 1.0);
}

// Hair cards: the strand direction along the card's UV axis (world space).
static float3 hairCardTangent(float3 N, float3 worldPos, float2 uv, constant DrawUniforms& d) {
    float3 T, B;
    charCotangentFrame(N, worldPos, uv, T, B);
    float3 dir = (d.character[0].z > 0.5 ? B : T) * d.character[0].w;
    dir -= N * dot(dir, N);
    return length(dir) > 1e-6 ? normalize(dir) : float3(0.0, 1.0, 0.0);
}

// Direct (sun + punctual lights) and image-based lighting of a character surface: shadeSurface()
// with the character BRDFs, the skin's softened shadows and the sun's transmission through thin parts.
static float3 shadeCharacter(SurfaceData s, CharacterInputs ci, float3 worldPos, float2 fragXY, float3 V, constant DrawUniforms& d,
                             constant FrameUniforms& f, const device GPULight* lights, const device uint2* clusterCells,
                             const device uint* clusterIndices, depth2d<float> shadowAtlas, texturecube<float> envTex,
                             texture2d<float> brdfLut, texture3d<float> cloudShape, depth2d_array<float> localShadows, uint layers) {
    float3 L = -f.sunDir.xyz;
    // Skin wraps light past the terminator, where the shadow map's self-shadowing would cut it off with a
    // jagged edge: its lookup point moves a little toward the sun (a light-space bias of a few mm).
    float3 shadowPos = ci.model == kShadeSkin ? worldPos + L * 0.006 + ci.Ngeo * 0.002 : worldPos;
    float sunVisible = L.y > -0.08 ? shadowFactor(shadowPos, ci.Ngeo, fragXY, f, shadowAtlas) : 0.0;
    float clouds = L.y > -0.08 ? cloudShadow(worldPos, L, f, cloudShape) : 0.0;
    float3 sunRad = f.sunColor.rgb * f.sunDir.w * clouds;
    float3 color = characterLight(s, ci, d, V, L, sunRad, sunVisible, 1.0);
    if (ci.model == kShadeSkin && d.character[1].w > 0.0 && L.y > -0.08) {
        // Transmission: light that entered the far side of a thin part (ear, nostril, fingers). Only
        // surfaces facing away from the sun pass it (silhouettes, where the depth map is unreliable, do not).
        float back = smoothstep(0.2, 0.75, -dot(ci.Ngeo, L));
        if (back > 0.0) {
            float3 mfp = max(d.character[0].rgb * d.character[0].w, float3(1e-3)) * 0.001;  // meters
            float3 through = sunTransmittance(worldPos, ci.Ngeo, mfp, f, shadowAtlas);
            color += s.albedo * through * back * sunRad * d.character[1].w * 0.8;
        }
    }
    int dirCount = int(f.cluster2.y);
    uint2 cell = clusterCells[clusterOf(f, fragXY, worldPos)];
    int total = dirCount + int(cell.y);
    const float shadowNoise = ditherNoise(fragXY, f.temporal);
    for (int k = 0; k < total; ++k) {
        int i = k < dirCount ? k : int(clusterIndices[cell.x + uint(k - dirCount)]);
        GPULight l = lights[i];
        if (!lightAffects(l, layers)) continue;
        float3 Ll;
        float3 rad = lightRadiance(l, worldPos, Ll);
        float sh = 1.0;
        if (l.kind.x > 0.5 && l.shadow.w > 0.5 && any(rad != 0.0)) sh = localShadow(l, worldPos, ci.Ngeo, shadowNoise, localShadows, 4, f.extra.y);
        color += characterLight(s, ci, d, V, Ll, rad, sh, l.params.x);
    }
    color = max(color, 0.0);

    // Image-based lighting (the split-sum form shadeSurface uses).
    float ambientK = f.ground.w * 2.0;
    float maxMip = f.extra.z;
    float NdotV = max(dot(s.N, V), 1e-4);
    float3 irradiance = envTex.sample(cubeSampler, s.N, level(maxMip)).rgb;
    if (ci.model == kShadeSkin) {  // red light averages over the smoother, geometric normal
        float3 smooth = envTex.sample(cubeSampler, ci.Ngeo, level(maxMip)).rgb;
        irradiance = float3(mix(irradiance.r, smooth.r, 0.7), mix(irradiance.g, smooth.g, 0.35), irradiance.b);
    }
    float rough = s.roughness;
    float F0v = ci.model == kShadeSkin ? 0.028 : (ci.model == kShadeEye ? 0.025 : 0.04);
    float3 R = reflect(-V, s.N);
    float3 prefiltered = envTex.sample(cubeSampler, R, level(rough * maxMip)).rgb;
    float2 ab = brdfLut.sample(linearClamp, float2(NdotV, 1.0 - rough)).rg;
    float3 Fr = float3(F0v) * ab.x + ab.y;
    float3 diffuse = irradiance * s.albedo * (1.0 - Fr);
    float specOcclusion = saturate(pow(NdotV + s.ao, exp2(-16.0 * rough - 1.0)) - 1.0 + s.ao);
    float3 specular = prefiltered * Fr * f.sky.w * specOcclusion;
    if (ci.model == kShadeCloth) {
        specular *= 0.5;
        float rimF = pow(1.0 - NdotV, 3.0);
        diffuse += irradiance * d.character[0].rgb * (d.character[1].x * rimF * 0.6 + 0.08);
    } else if (ci.model == kShadeHairCard) {
        specular *= 0.35 * d.character[0].y;
    }
    return color + (diffuse * s.ao + specular) * ambientK * 0.5;
}
