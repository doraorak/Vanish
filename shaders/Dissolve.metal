#ifndef VN_DISSOLVE_METAL
#define VN_DISSOLVE_METAL

#include "Common.metal"

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

#endif // VN_DISSOLVE_METAL
