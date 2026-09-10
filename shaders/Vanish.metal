// Shaders for Vanish's VN_ANIM_SHADER animation kind.
//
// This is compiled to Vanish.metallib by package.sh and shipped inside the
// tweak bundle. WindowServer only ever loads the finished library -- it never
// sees this source, never runs the Metal compiler, and never talks to the
// compiler service. Every error here is a build error.

#include <metal_stdlib>
using namespace metal;

struct VNVertexOut {
    float4 position [[position]];
    float2 uv;
};

// Passthrough pair, used only by the load-time probe that proves a shipped
// library can become a render pipeline state inside the server. It draws a
// full-target triangle strip from vertex_id alone, so it needs no vertex
// buffer and no vertex descriptor.
vertex VNVertexOut vn_passthrough_vertex(uint vid [[vertex_id]]) {
    const float2 corners[4] = {
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  1.0), float2(1.0,  1.0),
    };
    float2 p = corners[vid];
    VNVertexOut out;
    out.position = float4(p, 0.0, 1.0);
    out.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
    return out;
}

fragment float4 vn_passthrough_fragment(VNVertexOut in [[stage_in]],
                                        texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}
