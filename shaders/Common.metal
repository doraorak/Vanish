#ifndef VN_COMMON_METAL
#define VN_COMMON_METAL

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

// Per-window arguments, bound by Vanish at buffer(kVNShaderExtraIndex) whenever
// one of our pipelines is set. Must match VNShaderExtra and kVNShaderExtraIndex
// in Vanish.c. `params` are the five floats Vanish puts on the clone's filter;
// `bound` is 1 when they came from the layer being drawn, and 0 means use
// defaults.
#define kVNShaderExtraIndex 8

struct VNShaderExtra {
    float params[5];
    float bound;
    float phase;    // the animation's phase when this frame is drawn; -1 = none
};

// The animation's progress, 0..1, for the frame being drawn. Vanish computes it
// from the clock at draw time, so every composited frame shows its own moment;
// before the animation starts, or unbound, it falls back to the phase the last
// tick left in `brightness`.
static inline float vn_phase(constant VNUberArgs &args, constant VNShaderExtra &extra) {
    return extra.phase >= 0.0 ? extra.phase : clamp(1.0 - args.brightness, 0.0, 1.0);
}

// Where the window itself sits within the clone's frame, which also holds the
// drop shadow when shadows are on: VNShaderExtra params[2] is the inset of its
// left and right edges (the shadow is centred horizontally), params[3] and
// params[4] its top and bottom. Unbound, the window is the whole frame.
static inline void vn_window_rect(constant VNShaderExtra &extra, thread float2 &w0, thread float2 &wsz) {
    const bool bound = extra.bound > 0.5;
    w0 = bound ? float2(extra.params[2], extra.params[3]) : float2(0.0);
    const float2 w1 = bound ? float2(1.0 - extra.params[2], extra.params[4]) : float2(1.0);
    wsz = max(w1 - w0, float2(1e-4));
}

// The window's corner radius in pixels (params[1] is a fraction of its height),
// for a window `size` pixels big. Zero unbound.
static inline float vn_window_radius(constant VNShaderExtra &extra, float2 size) {
    return extra.bound > 0.5 ? extra.params[1] * size.y : 0.0;
}

// How much of the window's rounded shape covers a point given in the window's
// unit square: 1 inside, 0 outside, antialiased over a pixel.
static inline float vn_window_shape(float2 uv, float2 size, float radius) {
    const float2 half_size = 0.5 * size;
    const float  r   = min(radius, min(half_size.x, half_size.y));
    const float2 d   = abs(uv * size - half_size) - (half_size - r);
    const float  sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    return saturate(0.5 - sdf);
}

// An offset for hash inputs that is different on every close, so a shader's
// randomness -- where pieces break, how flecks drift -- is new each time.
// params[0] is a fresh random value per close (Burn reads it as its ignition
// point); unbound, the offset is zero and the shader keeps its fixed pattern.
// Kept under 100 so the sin-based vn_hash stays precise.
static inline float2 vn_close_seed(constant VNShaderExtra &extra) {
    if (extra.bound <= 0.5) return float2(0.0);
    const float s = extra.params[0];
    return float2(fract(s * 0.6180339), fract(s * 0.4142136)) * 97.0;
}

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

// Must match kVNShaderMargin in Vanish.c.
//
// Vanish gives the clone a shape that much larger than the window on each side
// so flecks are not cut off at its edge. The texture coordinates that arrive
// here are normalised to the TEXTURE, not to the quad -- so across a quad
// (1 + 2m) times wider they run 0 .. (1 + 2m), and the window's own pixels are
// exactly the [0,1] part. Nothing needs rescaling; texel-to-pixel is already
// 1:1.
//
// What does need correcting is the origin: coordinate 0 sits at the quad's
// top-left corner, which the widened shape moved up and left by m. Subtracting
// m puts the window back where it was, centred in the quad with margin all
// round, and makes anything outside [0,1] the margin.
// One margin for every shader animation, deliberately.
//
// Effects that never leave the window could ask for far less, and the quad's
// area is what they cost -- but a small margin exposes a one-or-two-frame
// artifact at the start of the close that has resisted five attempts to fix
// (a displaced copy of the window peeking out from behind the original). At
// this width the displaced frame lands entirely behind the original window and
// is never seen. That is a workaround, not a fix: the underlying mismatch is
// still there, it is simply covered.
constant float kVNShaderMargin = 0.50;

static inline float2 vn_window_uv(float2 tex) {
    return tex - kVNShaderMargin;
}

// Dave Hoskins' hash. Multiply-add based.
static inline float vn_hash(float2 p) {
    return fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

static inline float2 vn_hash22(float2 p) {
    float3 p3 = fract(float3(p.x, p.y, p.x) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Procedural 2D value noise with cubic Hermite smoothing
static inline float vn_fast_noise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = vn_hash22(i).x;
    float b = vn_hash22(i + float2(1.0, 0.0)).x;
    float c = vn_hash22(i + float2(0.0, 1.0)).x;
    float d = vn_hash22(i + float2(1.0, 1.0)).x;
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

#endif // VN_COMMON_METAL
