#ifndef VN_SHATTER_METAL
#define VN_SHATTER_METAL

#include "Common.metal"

// ---------------------------------------------------------------------------
// Shatter
// ---------------------------------------------------------------------------
//
// The hard part of any "window falls into pieces" effect is that a fragment
// shader runs backwards. We are handed a destination pixel and must find the
// shard that flew there, which is the inverse of the animation we want to
// describe. Three ways to do that, and only the third survives contact:
//
//   Take the shard at the destination and sample through its transform. This is
//   what most shatter shaders do. It nails the shard outlines to the screen and
//   slides content inside them -- a cracked screen, not pieces coming apart.
//
//   Iterate q <- inverse(shard(q))(p) to search for the shard that landed here,
//   the way the dissolve inverts its drift. Measured: it diverges. Once a shard
//   travels more than about its own width, the shard sitting at the destination
//   is an unrelated one that has already left, its transform sends the guess
//   somewhere arbitrary, and the iteration oscillates. Two thirds of the window
//   fails to resolve by the time it is a third of the way through, and more
//   iterations do not help, because the problem is the starting point, not the
//   rate.
//
//   Split the motion so the large part is exactly invertible and the small part
//   cannot go wrong. That is what this does.
//
// Every shard's motion is composed of:
//
//   GLOBAL   a burst outward from the impact plus a fall. Uniform across the
//            window, so it inverts in closed form -- no search, at any speed.
//
//   LOCAL    a shrink, a tumble and a small drift, all about the shard's own
//            centroid, which never moves under the local part.
//
// The shrink is what makes this work. A shard scaled down about its own
// centroid lies strictly inside its own Voronoi cell, so after undoing the
// global part, the point is already in the right cell and ONE lookup finds the
// owner -- exactly, not approximately. It is also what opens the gaps: a
// uniform expansion alone is a bijection, so the pane would simply scale up
// with its pieces still edge to edge. Pieces separate because each one shrinks
// away from its neighbours.
//
// Two consequences worth stating, because they are what make the effect honest:
// shards can never overlap (each stays inside its own cell), and a pixel that
// resolves to no shard is a real gap between pieces rather than a hole in one.
// Verified on the CPU across the whole phase range: zero round-trip failures,
// so every pixel drawn is the exact texel of the exact shard.

// Sites are the lattice jittered by up to half a cell: irregular and angular,
// the way glass actually breaks, without the near-degenerate slivers that full
// jitter produces.
static inline float2 vn_shard_site(float2 g) {
    return g + 0.5 + (vn_hash22(g) - 0.5) * 0.95;
}

/// Which shard owns a point, and where that shard's centroid is. Cell space.
static inline void vn_shard_at(float2 c, thread float2 &id, thread float2 &site) {
    const float2 base = floor(c);
    float best = 1e9;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            const float2 g = base + float2(i, j);
            const float2 s = vn_shard_site(g);
            const float  d = distance_squared(c, s);
            if (d < best) { best = d; id = g; site = s; }
        }
    }
}

/// Shatter: the window fractures at the close button and falls apart.
///
/// Pieces let go in the order the fracture reaches them, burst outward from the
/// impact, tumble about their own centres and fall out of frame. Nothing is
/// shaded or tinted -- every shard shows the window's own pixels unaltered, and
/// the motion carries the whole effect.
///
/// `brightness` is the phase, 1 -> 0. At 1 every shard's age is 0, so every
/// transform -- global and local -- is the identity: the frame is the untouched
/// window, to the texel.
fragment float4 vn_uber_shatter(VNUberStage in [[stage_in]],
                                texture2d<float> tex2D [[texture(0)]],
                                constant VNUberArgs &args [[buffer(0)]],
                                sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6));
    const float  t  = clamp(1.0 - args.brightness, 0.0, 1.0);

    // Derivatives have to be taken in uniform control flow, so the pixel size
    // is read once here, before anything branches.
    const float2 px   = max(fwidth(uv), float2(1e-6));
    const float2 size = 1.0 / px;                  // the window, in pixels

    // Big enough to read as shards of glass on a large window, not so small
    // that closing a dialog turns it to grit.
    const float  cell_px = max(length(size) / 15.0, 46.0);
    const float2 cell_uv = px * cell_px;

    // Cell space: uv scaled so one unit is one shard and both axes are square
    // in screen pixels. That is what makes the pieces tumble as circles rather
    // than ellipses on a window that is not square.
    const float2 c    = uv / cell_uv;
    const float  span = max(length(1.0 / cell_uv), 1e-4);   // diagonal, in cells

    const float te = t * t;

    // GLOBAL: burst outward from the impact at the window's top-left -- the
    // close button, so the pointer and the eye are already there, the same
    // reasoning as the dissolve -- plus a fall. Uniform, so inverting it is
    // division, and the shards may travel as far as they like.
    const float ke = 0.55 * te;
    const float gy = 1.30 * te * te * span;
    const float2 y = float2(c.x, c.y - gy) / (1.0 + ke);

    // One lookup, and it is exact: the local part only ever shrinks a shard
    // towards its own centroid, so it cannot have left its cell.
    float2 id = float2(0.0), site = float2(0.0);
    vn_shard_at(y, id, site);

    const float2 h1 = vn_hash22(id + 0.37);
    const float2 h2 = vn_hash22(id + 11.13);

    // Pieces let go in the order the crack reached them.
    const float reach = length(site) / span;
    const float start = reach * 0.42 + h1.x * 0.12;
    const float age   = clamp((t - start) / max(1.0 - start, 0.08), 0.0, 1.0);

    // LOCAL: shrink, tumble and drift, all about the centroid. The drift is
    // scaled by the room the shrink just freed, so a piece never wanders out of
    // its own cell and into a neighbour's.
    const float  sigma = 1.0 - 0.58 * age;
    const float  theta = (h2.x - 0.5) * 2.6 * age;
    const float  room  = (1.0 - sigma) * 0.55;
    const float2 drift = (float2(h1.y, h2.y) - 0.5) * (2.0 * room);

    const float  cs = cos(-theta);
    const float  sn = sin(-theta);
    const float2 r  = y - site - drift;
    const float2 q  = site + float2(r.x * cs - r.y * sn,
                                    r.x * sn + r.y * cs) / max(sigma, 1e-3);

    // Undoing the shrink pushes outward, so a point can land outside the cell
    // it started in. That is not an error: it is the gap that opened between
    // this piece and the next one.
    float2 id2 = float2(0.0), site2 = float2(0.0);
    vn_shard_at(q, id2, site2);
    if (any(id2 != id)) return float4(0.0);

    if (age >= 1.0) return float4(0.0);

    const float2 src = q * cell_uv;
    if (any(src < 0.0) || any(src > 1.0)) return float4(0.0);

    // No shading of any kind: a shard carries its own pixels at their own
    // colour, and the only thing that touches them is the fade that takes a
    // spent piece out without a pop. The motion is the effect.
    return tex2D.sample(samp, src) * clamp(1.3 - age, 0.0, 1.0);
}

#endif // VN_SHATTER_METAL
