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
constant float kVNShaderMargin = 0.35;

static float2 vn_window_uv(float2 tex) {
    return tex - kVNShaderMargin;
}

static float vn_hash(float2 p) {
    return fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

/// Dissolve: the window comes apart into drifting flecks of its own pixels.
///
/// Modelled on Aghajari's ThanosEffect, which is a real particle system: every
/// particle carries a colour sampled from the source, spreads radially, gets
/// accelerated upward by `pow(fraction * vy, 2)`, shrinks, fades out over
/// `1.2 - fraction`, and is drawn as a round point.
///
/// We cannot emit points -- the compositor gives us one quad and a fragment
/// shader over it -- so the same effect is built inside out. For each output
/// pixel we walk BACK along the drift to find which particle is covering it,
/// then sample the window's real colour at that particle's origin. That is the
/// part the first attempt was missing: it only punched alpha to zero, so pixels
/// disappeared where they stood instead of travelling.
///
/// `brightness` is the phase, driven from CGXWindow::brightness each tick and
/// running 1 -> 0. At 1 every particle's age is 0, the drift is zero and the
/// cell mask covers whole cells, so the frame is byte-identical to the
/// untouched window -- the registry's identity rule, which matters because the
/// clone composites once with the tag attached before the animation starts.
fragment float4 vn_uber_dissolve(VNUberStage in [[stage_in]],
                                 texture2d<float> tex2D [[texture(0)]],
                                 constant VNUberArgs &args [[buffer(0)]],
                                 sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6));
    const float  t  = clamp(1.0 - args.brightness, 0.0, 1.0);

    // Fleck size fixed in screen pixels, not texture coordinates, so it does not
    // scale with the window.
    const float2 px    = max(fwidth(uv), float2(1e-6));
    const float2 grain = px * 3.0;

    // Two fixed-point steps are enough to invert the drift while it stays
    // modest; the first guesses with the displacement at the output pixel, the
    // second corrects it.
    float2 src = uv;
    float  age = 0.0;
    for (int i = 0; i < 2; ++i) {
        const float2 cell = floor(src / grain);
        const float  r    = vn_hash(cell);

        // The dissolve begins at the top-left and spreads outward. That corner
        // is where the close button is, so it is where the pointer is and where
        // the eye already is -- the effect starts under the cursor and sweeps
        // away from it.
        //
        // Starting anywhere else costs far more than it looks. A front that
        // begins at the far edge leaves the region being watched untouched for
        // its whole spread, which reads as lag even though the animation is
        // already running.
        //
        // Distance is measured in pixels, not texture coordinates, so the front
        // is a circle on screen rather than an ellipse in a window that is not
        // square. 1/px is the window's size in pixels; its length is the
        // diagonal, which normalises the distance to 0..1.
        const float2 size  = 1.0 / px;
        const float  reach = length(src * size) / length(size);
        const float  start = reach * 0.50 + r * 0.15;
        age = clamp((t - start) / 0.55, 0.0, 1.0);

        const float2 rel = src - float2(0.5);
        float2 drift;
        drift.x = rel.x * 0.35 * age + (r - 0.5) * 0.12 * age;
        drift.y = rel.y * 0.15 * age - age * age * 0.45;   // rises, accelerating
        src = uv - drift;
    }

    if (age >= 1.0) return float4(0.0);

    // A fleck can drift into the margin, but it cannot originate there -- there
    // is no window pixel to carry.
    if (any(src < 0.0) || any(src > 1.0)) return float4(0.0);

    // Round flecks, shrinking with age -- the reference's circular points. At
    // age 0 the radius covers a whole cell (its far corner is at 1.41), so
    // nothing is cut out of a window that has not started dissolving.
    const float2 f = fract(src / grain) - 0.5;
    if (length(f) * 2.0 > mix(1.5, 0.0, age)) return float4(0.0);

    float4 colour = tex2D.sample(samp, src);
    colour *= clamp(1.2 - age, 0.0, 1.0);
    return colour;
}
