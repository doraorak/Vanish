#ifndef VN_BURN_METAL
#define VN_BURN_METAL

#include "Common.metal"

// ---------------------------------------------------------------------------
// Burn (HDR)
// ---------------------------------------------------------------------------
//
// Exploits Apple's EDR (Extended Dynamic Range) compositor pipeline:
// WindowServer renders into an extended-range framebuffer (rgba16float / Display P3).
//
// Color components exceeding 1.0 bypass the standard SDR luminance ceiling and
// drive the display toward its peak physical brightness. The gain is fixed at
// kVNBurnHDR, the largest value whose brightest pixel still fits the half-float
// framebuffer; the display clips to its own peak well before that.
//
// The animation ignites at the window's top-left corner and sweeps across the
// pane as an organic, incandescent burning wavefront:
//
//   AHEAD OF WAVE  : Window is completely intact, unaltered pixels.
//   AT THE WAVE    : Relativistic burning plasma edge with extreme HDR radiance
//                    (amber -> solar gold -> blinding blue-white core) scaled by
//                    kVNBurnHDR * 3.5 with subtle refractive heat shimmer.
//   BEHIND WAVE    : Matter is cleanly consumed and vaporized to void with zero
//                    lingering artifacts or debris.
//
// The flame is strictly confined to the visible window body: any outer margin
// quad texels and low-alpha drop shadow regions are discarded immediately.
//
// When brightness is 1 (t = 0), the displacement and thermal emission are zero,
// preserving the compositor's byte-identical identity rule.

// The brightest pixel is base (<= 1) + fire_col (<= 3.5) * heat factor (<= 3.7)
// * 3.5 * kVNBurnHDR. At 1445 that is about 65500, just under the half-float
// maximum of 65504; anything larger becomes infinity and renders as garbage.
constant float kVNBurnHDR = 1445.0;

fragment float4 vn_uber_burn(VNUberStage in [[stage_in]],
                             texture2d<float> tex2D [[texture(0)]],
                             constant VNUberArgs &args [[buffer(0)]],
                             sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6));
    const float  t  = clamp(1.0 - args.brightness, 0.0, 1.0);

    // Identity rule: before the animation begins, output must be byte-identical
    // to the untouched window.
    if (t <= 0.0) {
        if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0);
        return tex2D.sample(samp, uv);
    }

    // 1. Strictly discard anything outside the [0, 1] window coordinate bounds.
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0);

    // 2. Sample the source window texture.
    // When "shadows enabled" is ON, the clone frame includes the window's drop
    // shadow region in [0, 1]. In that shadow region, alpha is low (< 0.15).
    // Discarding low-alpha shadow regions ensures the flame never burns into the
    // empty drop shadow or beyond rounded window corners.
    const float4 src = tex2D.sample(samp, uv);
    const float content_mask = smoothstep(0.10, 0.28, src.a);
    if (content_mask <= 0.0) return float4(0.0);

    const float2 px     = max(fwidth(uv), float2(1e-6));
    const float2 size   = 1.0 / px;
    const float  aspect = size.x / size.y;

    // Detonation origin anchored directly at the window's top-left corner (0.0, 0.0)
    const float2 p      = uv * float2(aspect, 1.0);
    const float  dist   = length(p);
    const float  span   = length(float2(aspect, 1.0));
    const float  norm_d = dist / max(span, 1e-4);

    // Wavefront expands smoothly from the corner to consume the full window
    const float front = pow(t, 0.88) * 1.25;

    // Multi-octave procedural turbulence for organic burning flame contours
    const float n = vn_fast_noise(p * 5.5 - float2(t * 2.2)) * 0.70
                  + vn_fast_noise(p * 14.0 + float2(t * 3.5)) * 0.30;

    const float delta = norm_d - front + (n - 0.5) * 0.08;

    // Burning front intensity profile: sharp crest right at the wave, zero behind and ahead
    const float fire_crest = smoothstep(0.02, -0.01, delta) * smoothstep(-0.07, -0.01, delta);
    const float heat = pow(fire_crest, 0.75) * content_mask;

    // Matter burn mask: 1.0 ahead of the wave, falling sharply to 0.0 across the flame
    const float burn_mask = smoothstep(-0.04, 0.01, delta);

    // Sample window content with slight refractive heat shimmer at the flame
    const float2 shock_dir = dist > 1e-4 ? normalize(uv) : float2(0.7071, 0.7071);
    const float2 sample_uv = uv - shock_dir * (heat * 0.020);
    float4 base = tex2D.sample(samp, clamp(sample_uv, 0.0, 1.0));
    base *= burn_mask * content_mask;

    const float hdr = kVNBurnHDR;

    // Planckian / stellar plasma temperature ramp
    const float3 plasma_amber = float3(1.5, 0.45, 0.05);   // Molten gold/orange flame
    const float3 plasma_gold  = float3(1.9, 1.40, 0.35);   // Incandescent solar photosphere
    const float3 plasma_blue  = float3(1.1, 1.75, 3.50);   // Blinding relativistic blue-white core

    float3 fire_col = mix(plasma_amber, plasma_gold, smoothstep(0.08, 0.50, heat));
    fire_col = mix(fire_col, plasma_blue, smoothstep(0.50, 0.95, heat));

    // Content luminance drives thermal conductivity (brighter elements glow hotter)
    const float lum = dot(base.rgb, float3(0.2126, 0.7152, 0.0722));

    // HDR radiant emission along the flame crest: strictly locked to window content
    float3 emission = fire_col * (heat * 2.2 + lum * 1.5 * heat) * (hdr * 3.5) * content_mask;

    // Assemble final color: pure burning wave, completely clean wake, zero overflow
    float4 result;
    result.rgb = base.rgb + emission;
    result.a   = clamp(base.a + heat, 0.0, 1.0);

    // Smooth exit into void at end of animation
    result *= (1.0 - smoothstep(0.92, 1.0, t));
    return result;
}

#endif // VN_BURN_METAL
