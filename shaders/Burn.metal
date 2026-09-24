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
// `args.hdr_scale` exposes the display's current headroom multiplier (1.0 on SDR
// panels, scaling up to 2.0 - 3.5 on Liquid Retina XDR and Pro Display XDR).
// Color components exceeding 1.0 bypass the standard SDR luminance ceiling and
// directly drive the display's Mini-LED backlights to peak physical brightness
// (1000 - 1600 nits).
//
// The animation ignites at a point Vanish picks per close (VNShaderExtra
// params[0], in the window's unit square; the top-left corner when unbound)
// and sweeps across the pane as an organic, incandescent burning wavefront:
//
//   AHEAD OF WAVE  : Window is completely intact, unaltered pixels.
//   AT THE WAVE    : Relativistic burning plasma edge with extreme HDR radiance
//                    (amber -> solar gold -> blinding blue-white core) scaled by
//                    `hdr_scale * 3.5` with subtle refractive heat shimmer.
//   BEHIND WAVE    : Matter is cleanly consumed and vaporized to void with zero
//                    lingering artifacts or debris.
//
// The flame is strictly confined to the window's own shape: its rect, which
// Vanish passes in VNShaderExtra params[2..4] because the clone's frame also
// holds the drop shadow, with the corner radius from params[1]. The shadow
// beside each part of the window goes when that part burns.
//
// When brightness is 1 (t = 0), the displacement and thermal emission are zero,
// preserving the compositor's byte-identical identity rule.

fragment float4 vn_uber_burn(VNUberStage in [[stage_in]],
                             texture2d<float> tex2D [[texture(0)]],
                             constant VNUberArgs &args [[buffer(0)]],
                             constant VNShaderExtra &extra [[buffer(kVNShaderExtraIndex)]],
                             sampler samp [[sampler(0)]]) {
    // `fuv` spans the clone's frame -- the window plus its shadow when shadows
    // are on -- and is what the texture is sampled with. The window's own rect
    // inside it comes from VNShaderExtra params[2..4]; `uv` spans just that
    // rect, and the flame lives in it. Unbound, the window is the whole frame.
    const bool   bound = extra.bound > 0.5;
    const float2 fuv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6));
    const float  t   = clamp(1.0 - args.brightness, 0.0, 1.0);
    const float2 w0  = bound ? float2(extra.params[2], extra.params[3]) : float2(0.0);
    const float2 w1  = bound ? float2(1.0 - extra.params[2], extra.params[4]) : float2(1.0);
    const float2 wsz = max(w1 - w0, float2(1e-4));
    const float2 uv  = (fuv - w0) / wsz;
    const float2 px  = max(fwidth(fuv) / wsz, float2(1e-6));   // before any early return
    const float2 size   = 1.0 / px;                              // the window, in pixels
    const float  aspect = size.x / size.y;

    // Identity rule: before the animation begins, output must be byte-identical
    // to the untouched window.
    if (any(fuv < 0.0) || any(fuv > 1.0)) return float4(0.0);
    const float4 src = tex2D.sample(samp, fuv);
    if (t <= 0.0) return src;

    // Ignition point (params[0]: whole thousandths of x plus y) and the corner
    // radius (params[1], a fraction of the window's height).
    float2 origin = float2(0.0);
    float  radius = 0.0;
    if (bound) {
        const float xi = floor(extra.params[0]);
        origin = clamp(float2(xi / 1000.0, extra.params[0] - xi), 0.0, 1.0);
        radius = extra.params[1] * size.y;
    }

    // The window's own shape: its rect with rounded corners, antialiased over a
    // pixel. Everything in the frame outside it is shadow.
    const float2 half_size = 0.5 * size;
    const float  r   = min(radius, min(half_size.x, half_size.y));
    const float2 d   = abs(uv * size - half_size) - (half_size - r);
    const float  sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
    const float  window_mask = saturate(0.5 - sdf);

    // The burning front, evaluated at the nearest point of the window, so the
    // shadow beside any part of the window goes exactly when that part does.
    const float2 q      = clamp(uv, 0.0, 1.0);
    const float2 p      = (q - origin) * float2(aspect, 1.0);
    const float2 far    = float2(max(origin.x, 1.0 - origin.x), max(origin.y, 1.0 - origin.y));
    const float  dist   = length(p);
    const float  span   = length(far * float2(aspect, 1.0));
    const float2 np     = q * float2(aspect, 1.0);   // noise stays fixed to the window
    const float  norm_d = dist / max(span, 1e-4);

    // Wavefront expands smoothly from the ignition point to consume the full window
    const float front = pow(t, 0.88) * 1.25;

    // Multi-octave procedural turbulence for organic burning flame contours
    const float n = vn_fast_noise(np * 5.5 - float2(t * 2.2)) * 0.70
                  + vn_fast_noise(np * 14.0 + float2(t * 3.5)) * 0.30;

    const float delta = norm_d - front + (n - 0.5) * 0.08;

    // Matter burn mask: 1.0 ahead of the wave, falling sharply to 0.0 across the flame
    const float burn_mask = smoothstep(-0.04, 0.01, delta);

    // Smooth exit into void at end of animation
    const float exit = 1.0 - smoothstep(0.92, 1.0, t);

    const float4 shadow = src * burn_mask * (1.0 - window_mask);
    if (window_mask <= 0.0) return shadow * exit;

    // Burning front intensity profile: sharp crest right at the wave, zero behind and ahead
    const float fire_crest = smoothstep(0.02, -0.01, delta) * smoothstep(-0.07, -0.01, delta);
    const float heat = pow(fire_crest, 0.75) * window_mask;

    // Sample window content with slight refractive heat shimmer at the flame
    const float2 shock_dir = dist > 1e-4 ? normalize(q - origin) : float2(0.7071, 0.7071);
    const float2 sample_uv = q - shock_dir * (heat * 0.020);
    float4 base = tex2D.sample(samp, w0 + clamp(sample_uv, 0.0, 1.0) * wsz);
    base *= burn_mask * window_mask;

    // Display HDR headroom: 1.0 on SDR, up to 3.5 on Liquid Retina XDR
    const float hdr = max(args.hdr_scale, 1.0);

    // Planckian / stellar plasma temperature ramp
    const float3 plasma_amber = float3(1.5, 0.45, 0.05);   // Molten gold/orange flame
    const float3 plasma_gold  = float3(1.9, 1.40, 0.35);   // Incandescent solar photosphere
    const float3 plasma_blue  = float3(1.1, 1.75, 3.50);   // Blinding relativistic blue-white core

    float3 fire_col = mix(plasma_amber, plasma_gold, smoothstep(0.08, 0.50, heat));
    fire_col = mix(fire_col, plasma_blue, smoothstep(0.50, 0.95, heat));

    // Content luminance drives thermal conductivity (brighter elements glow hotter)
    const float lum = dot(base.rgb, float3(0.2126, 0.7152, 0.0722));

    // HDR radiant emission along the flame crest: strictly locked to the window
    float3 emission = fire_col * (heat * 2.2 + lum * 1.5 * heat) * (hdr * 3.5) * window_mask;

    // Assemble final color: pure burning wave, completely clean wake, zero overflow
    float4 result;
    result.rgb = base.rgb + emission;
    result.a   = clamp(base.a + heat, 0.0, 1.0);

    return (result + shadow) * exit;
}

#endif // VN_BURN_METAL
