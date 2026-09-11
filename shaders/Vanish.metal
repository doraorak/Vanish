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
// One margin for every shader animation, deliberately.
//
// Effects that never leave the window could ask for far less, and the quad's
// area is what they cost -- but a small margin exposes a one-or-two-frame
// artifact at the start of the close that has resisted five attempts to fix
// (a displaced copy of the window peeking out from behind the original). At
// this width the displaced frame lands entirely behind the original window and
// is never seen. That is a workaround, not a fix: the underlying mismatch is
// still there, it is simply covered.
constant float kVNShaderMargin = 0.36;

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
                            sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6));
    const float  t  = clamp(1.0 - args.brightness, 0.0, 1.0);

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

// Dave Hoskins' hash. The sin-based vn_hash above costs a transcendental per
// call, which is fine at the two the dissolve makes and not fine at the twenty
// odd this makes -- this one is pure multiply-add.
static float2 vn_hash22(float2 p) {
    float3 p3 = fract(float3(p.x, p.y, p.x) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

// Sites are the lattice jittered by up to half a cell: irregular and angular,
// the way glass actually breaks, without the near-degenerate slivers that full
// jitter produces.
static float2 vn_shard_site(float2 g) {
    return g + 0.5 + (vn_hash22(g) - 0.5) * 0.95;
}

/// Which shard owns a point, and where that shard's centroid is. Cell space.
static void vn_shard_at(float2 c, thread float2 &id, thread float2 &site) {
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
