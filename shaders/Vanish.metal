// Shaders for Vanish's VN_ANIM_SHADER animation kind.
//
// Compiled to Vanish.metallib by package.sh and shipped inside the tweak
// bundle. WindowServer only ever loads the finished library -- it never sees
// this source, never runs the Metal compiler, and never talks to the compiler
// service. Every error here is a build error.
//
// The signatures are not a design choice: they mirror SkyLight's own
// UberCompositeVertex / UberCompositeFragment exactly, because that is the
// pipeline the compositor builds and we only substitute the library it is built
// from. Anything here that disagrees with their ABI produces a pipeline that
// fails to build or samples garbage.
//
// Read off SkyLightShaders.air64.metallib with `metal-objdump --disassemble`:
//
//   UberCompositeVertex    attribute(0) float2 _pos
//                          attribute(1) float2 _tex
//                          attribute(2) float  _perspective_scale
//                          buffer(1)    float4x4 mvp_matrix
//                          -> { float4 pos [[position]], float4 tex }
//
//   UberCompositeFragment  stage_in as above
//                          texture(0) texture2d tex2D   <- the window's pixels
//                          buffer(0)  UberComposite_FragmentArgs (12 bytes)
//                          sampler(0) samp
//
//   UberComposite_FragmentArgs = { float _brightness; float _fade; float _hdr_scale; }
//
// Apple's fragment declares several more arguments behind function constants
// (colour-correction LUT, parametric correction, noise, rotbox). We declare none
// of them, so none are required of the pipeline.

#include <metal_stdlib>
using namespace metal;

struct VNUberIn {
    float2 pos               [[attribute(0)]];
    float2 tex               [[attribute(1)]];
    float  perspective_scale [[attribute(2)]];
};

struct VNUberStage {
    float4 pos [[position]];
    float4 tex;
};

struct VNUberArgs {
    float brightness;   // 0 -- Vanish drives this as the animation's progress
    float fade;         // 4
    float hdr_scale;    // 8
};

vertex VNUberStage vn_uber_vertex(VNUberIn in [[stage_in]],
                                  constant float4x4 &mvp [[buffer(1)]]) {
    VNUberStage out;
    out.pos = mvp * float4(in.pos, 0.0, 1.0);
    // Texcoords travel homogeneous so perspective interpolates correctly; the
    // fragment divides through. For an axis-aligned clone the scale is 1 and
    // this is the identity, but matching the stock vertex costs nothing.
    out.tex = float4(in.tex * in.perspective_scale, 0.0, in.perspective_scale);
    return out;
}

/// Dissolve: pixels drop out in a fixed random order as the close progresses.
///
/// Chosen as the first shader deliberately -- it is the simplest effect a mesh
/// warp cannot express at all. A warp only moves pixels it already has; this
/// decides, per pixel, whether there is a pixel.
///
/// `brightness` is the phase, driven from CGXWindow::brightness each tick.
/// Vanish sets it to 1 - t, so it starts at 1 and falls to 0. At phase 1 the
/// threshold is below every hash value and nothing is removed, which is the
/// registry's identity rule: the clone composites once with the tag attached
/// before the animation starts.
fragment float4 vn_uber_dissolve(VNUberStage in [[stage_in]],
                                 texture2d<float> tex2D [[texture(0)]],
                                 constant VNUberArgs &args [[buffer(0)]],
                                 sampler samp [[sampler(0)]]) {
    float2 uv = in.tex.xy / max(in.tex.w, 1e-6);
    float4 colour = tex2D.sample(samp, uv);

    // Value hash on a coarse grid, so the dissolve reads as flecks rather than
    // per-pixel noise at retina density.
    float2 cell = floor(uv * 420.0);
    float  n    = fract(sin(dot(cell, float2(12.9898, 78.233))) * 43758.5453);

    colour *= step(1.0 - args.brightness, n);
    return colour;
}
