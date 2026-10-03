// Skywalker GPU skinning (Metal Shading Language), compiled at runtime like Standard.metal.
//
// A compute pre-pass: every skinned draw writes its posed vertices (same 12-float layout
// as static meshes) into a per-instance buffer, so the standard vertex functions — main
// pass, shadows, outlines, selection — draw animated characters unchanged. Linear blend
// skinning with up to four joints per vertex. Layouts must match MetalRenderer.mm.

#include <metal_stdlib>
using namespace metal;

struct SkinVertex {        // 48 bytes
    packed_float3 position;  // bind (skin) space
    packed_float3 normal;
    ushort4 joints;          // palette slots
    float4 weights;          // sum to 1
};

struct SkinParams {
    uint vertexCount;
    uint jointCount;
    uint pad0;
    uint pad1;
};

kernel void skinVertices(device const float* base [[buffer(0)]],         // static vertices: uv + color are copied
                         device const SkinVertex* skin [[buffer(1)]],
                         device const float4x4* palette [[buffer(2)]],    // mesh-space joint matrices
                         constant SkinParams& params [[buffer(3)]],
                         device float* out [[buffer(4)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= params.vertexCount) return;
    SkinVertex s = skin[id];
    float4x4 m = float4x4(0.0);
    for (int k = 0; k < 4; ++k) {
        float w = s.weights[k];
        if (w > 0.0) m += palette[min(uint(s.joints[k]), params.jointCount - 1)] * w;
    }
    float3 p = (m * float4(float3(s.position), 1.0)).xyz;
    float3 n = (m * float4(float3(s.normal), 0.0)).xyz;
    float len = length(n);
    n = len > 1e-12 ? n / len : float3(0.0, 1.0, 0.0);
    uint o = id * 12;
    out[o + 0] = p.x;
    out[o + 1] = p.y;
    out[o + 2] = p.z;
    out[o + 3] = n.x;
    out[o + 4] = n.y;
    out[o + 5] = n.z;
    for (uint k = 6; k < 12; ++k) out[o + k] = base[o + k];
}
