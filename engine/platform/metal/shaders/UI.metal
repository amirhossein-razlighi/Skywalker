// Skywalker UI shaders: rounded rectangles with borders and gradients, soft drop shadows, images
// (9-slices arrive as separate quads) and SDF text, drawn after post-processing at output resolution.
// Colors arrive in sRGB (as authored) and are converted to linear; the sRGB render target encodes
// them back, so the UI shows exactly the colors in the style sheet.

#include <metal_stdlib>
using namespace metal;

struct UIQuad {
    float4 rect;         // x, y, w, h (canvas units)
    float4 uv;
    float4 color;
    float4 color2;
    float4 borderColor;
    float4 params;       // x = kind (0 rect, 1 image, 2 glyph, 3 shadow), y = radius, z = border width, w = opacity
    float4 params2;      // rect/shadow: x = blur; glyph: x = dilation, y = outline, z = italic shear
    float4 clip;         // x0 y0 x1 y1 (canvas units); x1 < x0 = none
};

struct UIUniforms {
    float4x4 transform;  // canvas units -> clip space
    float4 info;         // x = output pixels per canvas unit
};

struct VOut {
    float4 position [[position]];
    float2 local;   // position inside the rect (canvas units, from its top-left)
    float2 canvas;  // canvas position (clipping)
    float2 uv;
    uint quad [[flat]];
};

constant float2 kCorners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 0), float2(1, 1), float2(0, 1)};

static float3 toLinear(float3 c) {
    return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

static float sdRoundRect(float2 p, float2 hs, float r) {
    r = min(r, min(hs.x, hs.y));
    float2 q = abs(p) - hs + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

vertex VOut uiVertex(uint vid [[vertex_id]], uint iid [[instance_id]], const device UIQuad* quads [[buffer(0)]],
                     constant UIUniforms& u [[buffer(1)]]) {
    UIQuad q = quads[iid];
    float2 c = kCorners[vid];
    int kind = int(q.params.x + 0.5);
    // Rects grow by a pixel so their anti-aliased edge is not cut by rasterization.
    float pad = (kind == 0) ? 1.0 / max(u.info.x, 1e-3) : 0.0;
    float2 size = q.rect.zw + pad * 2.0;
    float2 local = c * size - pad;
    float2 pos = q.rect.xy + local;
    if (kind == 2) pos.x += q.params2.z * (1.0 - c.y);  // italic: lean the top right
    VOut o;
    o.position = u.transform * float4(pos, 0.0, 1.0);
    o.local = local;
    o.canvas = pos;
    o.uv = mix(q.uv.xy, q.uv.zw, c);
    o.quad = iid;
    return o;
}

fragment float4 uiFragment(VOut in [[stage_in]], const device UIQuad* quads [[buffer(0)]], constant UIUniforms& u [[buffer(1)]],
                           texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    UIQuad q = quads[in.quad];
    if (q.clip.z >= q.clip.x &&
        (in.canvas.x < q.clip.x || in.canvas.y < q.clip.y || in.canvas.x > q.clip.z || in.canvas.y > q.clip.w)) {
        discard_fragment();
    }
    int kind = int(q.params.x + 0.5);
    float opacity = q.params.w;
    float2 hs = q.rect.zw * 0.5;
    float2 p = in.local - hs;
    float aa = 0.5 / max(u.info.x, 1e-3);
    float4 color = float4(toLinear(q.color.rgb), q.color.a);
    float4 outc = float4(0.0);
    if (kind == 0) {  // rounded rect: gradient fill + border
        float d = sdRoundRect(p, hs, q.params.y);
        float cover = saturate(0.5 - d / (2.0 * aa));
        float v = saturate(in.local.y / max(q.rect.w, 1e-3));
        float4 fill = mix(q.color, q.color2, v);
        fill.rgb = toLinear(fill.rgb);
        float bw = q.params.z;
        float4 border = float4(toLinear(q.borderColor.rgb), q.borderColor.a);
        float4 c = fill;
        if (bw > 0.0 && border.a > 0.0) {
            float inner = saturate(0.5 - (d + bw) / (2.0 * aa));
            float a = border.a * (1.0 - inner) + fill.a * inner;
            c.rgb = a > 0.0 ? (border.rgb * border.a * (1.0 - inner) + fill.rgb * fill.a * inner) / a : fill.rgb;
            c.a = a;
        }
        outc = float4(c.rgb, c.a * cover);
    } else if (kind == 3) {  // soft shadow
        float blur = max(q.params2.x, 0.5);
        float d = sdRoundRect(p, max(hs - blur, float2(0.0)), q.params.y);
        float a = 1.0 - smoothstep(-blur, blur, d);
        outc = float4(color.rgb, color.a * a);
    } else if (kind == 1) {  // image (rounded corners clip it)
        float4 t = tex.sample(smp, in.uv);
        float mask = q.params.y > 0.0 ? saturate(0.5 - sdRoundRect(p, hs, q.params.y) / (2.0 * aa)) : 1.0;
        outc = float4(t.rgb * color.rgb, t.a * color.a * mask);
    } else {  // SDF glyph
        float d = tex.sample(smp, in.uv).r + q.params2.x;
        float fw = fwidth(d);
        d += min(0.06, fw * 0.3);  // stem darkening: small text keeps its weight
        float w = max(fw * 0.55, 1e-3);
        float fill = smoothstep(0.5 - w, 0.5 + w, d);
        float outline = q.params2.y;
        if (outline > 0.0) {
            float outer = smoothstep(0.5 - outline - w, 0.5 - outline + w, d);
            float4 oc = float4(toLinear(q.color2.rgb), q.color2.a);
            outc = float4(mix(oc.rgb, color.rgb, fill), mix(oc.a, color.a, fill) * outer);
        } else {
            outc = float4(color.rgb, color.a * fill);
        }
    }
    outc.a *= opacity;
    if (outc.a <= 0.002) discard_fragment();
    return outc;
}
