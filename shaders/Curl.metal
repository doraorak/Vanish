#ifndef VN_CURL_METAL
#define VN_CURL_METAL

#include "Common.metal"

// ---------------------------------------------------------------------------
// Page curl
// ---------------------------------------------------------------------------
//
// The window peels away like a sheet of paper, from its right edge to its left: it lies flat up to
// a fold line, rolls over a cylinder, and past the cylinder it is gone. The cylinder shows the page
// twice: the front side where it is still on its way up, and, on top of it, the page's back, lit
// along the curve. A soft shadow falls ahead of the roll, on the page still flat and on what the
// page uncovers. All of it is inside the window's own frame: nothing is drawn beyond it.
//
// This is Core Image's own page curl. CIPageCurlTransition (and the shadow variant) is a Core
// Image kernel shipped in CoreImage.framework's ci_filters.metallib; its algorithm, read off that
// kernel (_pageCurlNoShadowTransition and _pageCurlWithShadowTransition), is:
//
//   t   = how far across the cylinder a pixel is, 0 where it starts to lift, 1 at its far side
//   t'  = t + 0.5625 (sqrt(1 - t^1.5) - 1)^2     an arc length: a fit of asin(t), 0 .. pi/2
//   the front side of the page at that pixel is the page's own picture at arc t',
//   the back side, the same page turned over the top of the cylinder, at arc pi - t'
//   past the cylinder (t >= 1) there is no page; flat (t < 0) it is the pixel itself
//   sheen  k = clamp((t - 0.6) 2.5, 0, 1);  s = k > 0.75 ? 15 (k - 0.82)^2 + 0.4 : (0.375 k + 0.35) k
//          the back's lighting is mix(bright, dark, s), laid over the back, which lies over the front
//   the silhouette of the cylinder is softened by clamp((1 - t) radius, 0, 1)
//   the shadow is a cubic step, 0.5 + 0.64 d - 0.14 d^3, of the distance from the roll
//
// What the kernel gets as uniforms from the CIFilter (the cylinder as two matrices) is worked out
// here from the clone's geometry instead: the fold runs at an angle across the frame and moves
// across it as the animation goes. The page is the window itself: the clone's frame also holds its
// drop shadow when shadows are on (VNShaderExtra params[2..4] say where the window sits in it), and
// the shadow goes as the part of the page beside it does. The roll starts as a small curl at the page's far corner and grows to
// its full radius as the fold crosses the page: a page can only roll over as much of itself as it
// has, so a roll of full size at the edge would have nothing to be made of.
//
// At phase 0 the fold is beyond the far edge and the frame is returned untouched (the identity
// rule); the shadow is scaled in from nothing.

/// The arc, in radians (0 .. pi/2), of the point t (0 .. 1) across the cylinder: Core Image's polynomial fit of asin.
static inline float vn_curl_arc(float t) {
    const float q = sqrt(max(1.0 - pow(t, 1.5), 0.0)) - 1.0;
    return q * q * 0.5625 + t;
}

/// A smooth step from d = -1 (0) to d = 1 (1): the cubic the kernel's shadow is made from.
static inline float vn_curl_step(float d) {
    const float c = clamp(d, -1.0, 1.0);
    return 0.5 + c * (0.64 - 0.14 * c * c);
}

/// The clone's frame at a point of it (0..1), nothing outside it.
static inline float4 vn_curl_frame_sample(texture2d<float> tex2D, sampler samp, float2 uv) {
    if (any(uv < 0.0) || any(uv > 1.0)) return float4(0.0);
    return tex2D.sample(samp, uv, level(0));
}

/// The page at a point of the frame: the window's own pixels, nothing outside its rect (not the shadow round it).
static inline float4 vn_curl_page_sample(texture2d<float> tex2D, sampler samp, float2 uv, float2 w0, float2 wsz) {
    if (any(uv < w0) || any(uv > w0 + wsz)) return float4(0.0);
    return tex2D.sample(samp, uv, level(0));
}

fragment float4 vn_uber_curl(VNUberStage in [[stage_in]],
                             texture2d<float> tex2D [[texture(0)]],
                             constant VNUberArgs &args [[buffer(0)]],
                             constant VNShaderExtra &extra [[buffer(kVNShaderExtraIndex)]],
                             sampler samp [[sampler(0)]]) {
    const float2 fuv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6), extra);
    const float  p   = vn_phase(args, extra);
    const float2 px  = max(fwidth(fuv), float2(1e-6));       // before any early return
    const float4 flat_colour = vn_curl_frame_sample(tex2D, samp, fuv);
    if (p <= 0.0) return flat_colour;

    // The fold's direction, a little different on every close: mostly from the right, tilted down.
    const float2 seed = vn_close_seed(extra);
    const float  tilt = extra.bound > 0.5 ? mix(0.30, 0.65, fract(seed.x * 0.37)) : 0.45;
    const float2 u = float2(cos(tilt), sin(tilt));          // the way the page goes, towards its far corner
    const float2 a = float2(-u.y, u.x);                     // along the fold

    // Everything below in pixels from the window's top left; the page is the window.
    float2 w0, wsz;
    vn_window_rect(extra, w0, wsz);
    const float2 page = wsz / px;
    const float2 pos  = (fuv - w0) / px;
    const bool   on_page = all(pos >= 0.0) && all(pos <= page);
    const float  far_s = dot(page, u);                      // the far corner's place along u
    const float  full_radius = clamp(0.045 * length(page), 36.0, 130.0);
    const float  radius = full_radius * mix(0.1, 1.0, smoothstep(0.0, 0.3, p));

    // The fold crosses the page from just past its far corner to just past the near edge, so the
    // roll, which lies beyond the fold, has left the page by the end.
    const float e = p * (1.4 - 0.4 * p);
    const float fold = mix(far_s + 2.0, -(full_radius + 2.0), e);
    // A pixel of the shadow round the window goes when the nearest part of the page does.
    const float s = dot(clamp(pos, 0.0, page), u);
    const float t = (s - fold) / radius;
    const float along = dot(pos, a);

    // The shadow grows with the fold and goes as the page does.
    const float amount = 0.55 * smoothstep(0.0, 0.08, p) * (1.0 - smoothstep(0.85, 1.0, p));

    // Flat: the page as it is, darkened ahead of the roll by what the roll casts.
    if (t < 0.0) {
        const float x = saturate(-t * radius / (1.8 * radius));
        const float shade = amount * 0.5 * (1.0 - vn_curl_step(x * 2.0 - 1.0));
        float4 colour = float4(flat_colour.rgb * (1.0 - shade), flat_colour.a);
        // The shadow round the window thins out as the fold nears the part of the page beside it.
        if (!on_page) colour *= mix(1.0, smoothstep(0.0, 24.0, -t * radius), smoothstep(0.0, 0.1, p));
        return colour;
    }

    // The roll and what lies past it are the page's own: nothing hangs outside the window, and the window's
    // rounded corners are the shape's, wherever the roll has left the page.
    if (!on_page) return float4(0.0);
    const float corners = vn_window_shape(pos / page, page, vn_window_radius(extra, page));

    // Past the roll: no page, only the shadow it throws on what it uncovers.
    if (t >= 1.0) {
        const float x = saturate((t - 1.0) * radius / (1.5 * radius));
        const float shade = amount * 0.7 * (1.0 - vn_curl_step(x * 2.0 - 1.0));
        return float4(0.0, 0.0, 0.0, shade * corners);
    }

    // On the roll.
    const float arc = vn_curl_arc(t);
    const float2 front_at = w0 + (u * (fold + radius * arc) + a * along) * px;
    const float2 back_at  = w0 + (u * (fold + radius * (3.14159265 - arc)) + a * along) * px;
    const float  soft = saturate((1.0 - t) * radius / 1.5);

    // The front of the page, darker as it turns away from the light.
    float4 front = vn_curl_page_sample(tex2D, samp, front_at, w0, wsz) * soft;
    front.rgb *= 1.0 - amount * min(0.5, t * t * 0.9);

    // The back: the same page seen from behind, paper with the content faint through it.
    float4 back = vn_curl_page_sample(tex2D, samp, back_at, w0, wsz);
    const float3 paper = float3(0.93, 0.93, 0.95);
    const float3 through = back.a > 1e-4 ? back.rgb / back.a : paper;
    back = float4(mix(paper, through, 0.22) * back.a, back.a) * soft;

    // Light along the curve: bright where the roll begins, dark over its top.
    const float k = clamp((t - 0.6) * 2.5, 0.0, 1.0);
    const float sheen_at = k > 0.75 ? (k - 0.82) * (k - 0.82) * 15.0 + 0.4 : (k * 0.375 + 0.35) * k;
    const float4 bright = float4(0.35);
    const float4 dark   = float4(0.0, 0.0, 0.0, 0.55);
    const float4 lit = back.a * mix(bright, dark, sheen_at);
    const float4 back_layer = (1.0 - lit.a) * back + lit;

    return (front * (1.0 - back_layer.a) + back_layer) * corners;
}

#endif // VN_CURL_METAL
