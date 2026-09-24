#ifndef VN_CRT_METAL
#define VN_CRT_METAL

#include "Common.metal"

/// CRT off: the picture collapses to a bright line, then a dot, and snaps out.
///
/// Two stages off one phase -- vertical collapse, then horizontal -- with the
/// brightness lifting as the energy concentrates.
///
/// `brightness` is the phase, 1 -> 0. At 1 both scales are 1, the sample is the
/// pixel itself and the lift is zero, so the frame is the untouched window.
fragment float4 vn_uber_crt(VNUberStage in [[stage_in]],
                            texture2d<float> tex2D [[texture(0)]],
                            constant VNUberArgs &args [[buffer(0)]],
                            constant VNShaderExtra &extra [[buffer(kVNShaderExtraIndex)]],
                            sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6), extra);
    const float  t  = vn_phase(args, extra);

    const float vert  = clamp(t / 0.62, 0.0, 1.0);          // squeeze to a line
    const float horiz = clamp((t - 0.62) / 0.38, 0.0, 1.0); // then to a dot

    // Eased so the collapse accelerates into the line.
    const float sy = max(1.0 - vert  * vert  * 0.995, 0.0012);
    const float sx = max(1.0 - horiz * horiz * 0.995, 0.0012);

    const float2 src = (uv - 0.5) / float2(sx, sy) + 0.5;
    if (any(src < 0.0) || any(src > 1.0)) return float4(0.0);

    float4 colour = tex2D.sample(samp, src);

    // Brighter as it is squeezed into less space, then the dot burns out.
    //
    // Scaled by alpha because the content is premultiplied. Adding a flat
    // amount instead lights up every transparent texel as well -- and with
    // shadows enabled the clone is built at the window's BOUNDS, so the whole
    // shadow border is low-alpha texels inside [0,1]. That is what put a white
    // wash behind the window for the length of the animation.
    colour.rgb += colour.a * (vert * 0.45 + horiz * 1.10);
    colour *= 1.0 - horiz * horiz;

    return colour;
}

#endif // VN_CRT_METAL
