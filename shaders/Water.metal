#ifndef VN_WATER_METAL
#define VN_WATER_METAL

#include "Common.metal"

// Water: the window turns to liquid and pours down the screen.
//
// This is a real fluid, not particles under gravity. The solver is Position
// Based Fluids (Macklin & Muller, ACM TOG 32(4), 2013), which is the method
// worth having here for one reason: it enforces incompressibility as a
// positional constraint solved by Jacobi iteration rather than as a pressure
// force, so it is stable at a whole display frame of 1/60 s. Classical SPH
// pressure forces are stiff and need timesteps an order of magnitude smaller --
// inside a compositor there is exactly one step per frame and no second chance,
// so a method that needs substepping is a method that explodes.
//
// Algorithm 1 of the paper, as implemented here, one kernel per line:
//
//     apply forces, predict positions          vn_pbf_predict
//     find neighbours                          vn_pbf_bin_clear / vn_pbf_bin_fill
//     while iter < solverIterations:
//         calculate lambda_i                   vn_pbf_lambda
//         calculate delta p_i, collide         vn_pbf_delta
//         update positions                     vn_pbf_apply
//     update velocity v = (x* - x)/dt          vn_pbf_velocity
//     vorticity confinement and XSPH           vn_pbf_vorticity / _vel_post / _vel_commit
//
// The three velocity kernels are split because each reads its neighbours'
// velocities and writes its own: doing that in one dispatch would have some
// particles reading a neighbour the same dispatch had already overwritten. The
// post pass writes into `dp`, which the solve has finished with, and a final
// dispatch commits it.
//
// The surface is then drawn the way particle fluids are normally drawn --
// screen space, after van der Laan et al., "Screen space fluid rendering with
// curvature flow" (I3D 2009): build a field from the particles, smooth it, and
// take the surface and its normal from the smoothed field, instead of
// polygonising anything. vn_field_build gathers it, two separable blurs smooth
// it, and vn_uber_water shades the isosurface. Gather, not scatter: each texel
// asks the bins which particles are near it, so the field needs no atomics.
//
// Everything is in RENDER-TARGET PIXELS, the space a fragment's [[position]]
// arrives in, so a particle's coordinates and a pixel's are the same numbers.

/// Must match VNSimParams in Vanish.c. Plain scalars only -- no float2 -- so the
/// two layouts cannot disagree about alignment.
struct VNSimParams {
    float domain_x, domain_y;        // the destination, in render-target pixels
    float spawn_min_x, spawn_min_y;  // the closing window within it
    float spawn_max_x, spawn_max_y;
    float dt;
    float gravity;                   // px/s^2, +y is down
    float h;                         // SPH smoothing radius, px
    float rho0;                      // rest density, summed on the CPU for this h and spacing
    float cfm_eps;                   // the relaxation of eq. 11, scaled to this h and spacing
    float scorr_k;                   // eq. 13, k
    float scorr_w_dq;                // eq. 13, W(|dq|, h), precomputed
    float xsph_c;                    // eq. 17
    float vort_eps;                  // eq. 16
    float radius;                    // collision radius against walls and windows, px
    float iso;                       // isosurface level, as a fraction of rho0
    float seed;
    uint  count;
    uint  spawn;                     // 1 on the first step of a close
    uint  bin_w, bin_h;
    float cell;                      // bin cell size, px; never smaller than h
    uint  bin_slots;
    uint  field_w, field_h;
    uint  obstacles;                 // how many of the obstacle buffer is live
    uint  valid;                     // 0 = no simulation yet, 1 = draw it, 2 = this close's water is gone
    float tint;                      // 0..1, how far it settles toward water blue
    float age;                       // seconds since the fluid was seeded
    uint  drains;                    // bit 0 left corner, 1 middle, 2 right corner
    float fade;                      // 1 while the water is shown, down to 0 as it goes
    uint  fresh;                     // obstacles that appeared this step, a bit per index
    uint  others;                    // other closes' fields bound for coupling, 0..2
};

/// Must match VNParticle in Vanish.c. `uv0` is where in the window this parcel
/// of liquid came from, so the fluid carries the window's own pixels.
struct VNParticle {
    float2 pos;
    float2 vel;
    float2 uv0;
    float2 ppos;    // predicted position, and the solver's working copy
    float2 dp;      // position correction, then the post-processed velocity
    float  lambda;  // the solve's multiplier, then the vorticity
    uint   ignore;  // obstacles this particle began inside; see vn_collide
};

/// A window the fluid has to flow around. Rounded rectangle, in the same pixels.
struct VNObstacle {
    float min_x, min_y, max_x, max_y;
    float corner;
    float _pad;
};

static_assert(sizeof(VNSimParams) == 136, "VNSimParams must match WaterSim.h");
static_assert(sizeof(VNParticle) == 48, "VNParticle must match WaterSim.h");
static_assert(sizeof(VNObstacle) == 24, "VNObstacle must match WaterSim.h");

#pragma mark - SPH kernels (2D)

// The 2D normalisations, not the 3D ones every paper quotes: this fluid lives in
// the plane of the screen, and using the 3D constants would put the rest density
// and the pressure gradient on different scales.
//
//   poly6   W(r,h)  = 4/(pi h^8) (h^2 - r^2)^3
//   spiky   grad W  = -30/(pi h^5) (h - r)^2 * rhat

static inline float vn_poly6(float r2, float h) {
    const float d = h * h - r2;
    if (d <= 0.0) return 0.0;
    const float h2 = h * h, h4 = h2 * h2;
    return (4.0 / M_PI_F) * (d * d * d) / (h4 * h4);
}

static inline float2 vn_spiky_grad(float2 rvec, float r, float h) {
    if (r <= 1e-5 || r >= h) return float2(0.0);
    const float h2 = h * h;
    const float c = -(30.0 / M_PI_F) * (h - r) * (h - r) / (h2 * h2 * h);
    return c * (rvec / r);
}

#pragma mark - Neighbour bins

static inline uint vn_bin_stride(constant VNSimParams &sp) { return 1u + sp.bin_slots; }

static inline int2 vn_bin_of(float2 p, constant VNSimParams &sp) {
    return clamp(int2(floor(p / max(sp.cell, 1e-3))),
                 int2(0), int2(int(sp.bin_w) - 1, int(sp.bin_h) - 1));
}

// The cell is never smaller than h (Vanish enforces it), so the 3x3 block
// around a particle's own cell contains every neighbour within h.
//
// The search happens exactly twice: once in vn_pbf_neighbours, which writes a
// flat list every solver kernel then walks, and once per texel in
// vn_field_build. Recomputing neighbourhoods once per step and re-evaluating
// distances each iteration is what the paper prescribes, and it keeps the five
// kernels that consume them down to a three-line loop each instead of five
// copies of a triple-nested search to get wrong independently.
#define kVNMaxNeighbours 48u

#pragma mark - Collision

/// How much of the display's bottom edge is a drain, as a fraction of its
/// width, centred. Without it the fluid has nowhere to go: it lands, spreads
/// along the floor and sits there for the rest of the close.
constant float kVNDrainFraction = 0.25;

/// The two bottom corners are open as well, for this fraction of the bottom
/// edge in from each corner and the same fraction of each side edge up from it.
/// Water that reaches the floor spreads outward, so the corners are where it
/// ends up -- an L-shaped opening at each one is what stops it banking against
/// the walls once the middle drain is behind it.
constant float kVNCornerDrainFraction = 0.075;

/// Bit 31 of VNParticle::ignore. Obstacles use bits 0..23, so the top of the
/// word is free. A drained particle is parked below the display and left out of
/// the bins, which takes it out of every neighbour list and out of the surface
/// field in one move -- it stops existing for everything downstream without
/// needing a second buffer or a compaction pass.
#define VN_DRAINED (1u << 31)

/// How long the tint takes to run its course, in seconds from the fluid's
/// birth. The length of the close it used to be measured against.
constant float kVNTintSeconds = 1.6;

static inline bool vn_inside_obstacle(float2 p, VNObstacle o, float margin) {
    return p.x > o.min_x - margin && p.x < o.max_x + margin &&
           p.y > o.min_y - margin && p.y < o.max_y + margin;
}

/// Projects a point out of the display edges and out of every window it is not
/// entitled to be inside of.
///
/// The entitlement matters more than it sounds. The fluid is born where the
/// closing window was, and a window BEHIND that one contains the whole of it --
/// so on the first step every particle is inside an obstacle, and pushing each
/// one to its nearest edge throws the entire fluid onto the perimeter of that
/// window in a single frame. With a full-screen window in the list, which is
/// what the desktop is, that perimeter is the edge of the display: the fluid
/// arrives as a thin frame around the screen and never falls at all.
///
/// So each particle carries a bit per obstacle it started inside. That obstacle
/// cannot push it, until it leaves of its own accord -- at which point the bit
/// clears and the window becomes solid to it like any other. Water poured onto
/// a stack of windows runs off the one it started on and lands on the next.
/// The display's edges, with the floor and the lower side walls open wherever
/// a drain is switched on.
static inline float2 vn_collide_walls(float2 p, constant VNSimParams &sp) {
    const float r = sp.radius;
    const float2 lo = float2(r);
    const float2 hi = max(float2(sp.domain_x, sp.domain_y) - r, lo + 1.0);

    p.y = max(p.y, lo.y);                         // the ceiling is always solid

    const float corner_w = kVNCornerDrainFraction * sp.domain_x;
    const float corner_h = kVNCornerDrainFraction * sp.domain_y;

    const bool  left_open   = (sp.drains & 1u) != 0u;
    const bool  middle_open = (sp.drains & 2u) != 0u;
    const bool  right_open  = (sp.drains & 4u) != 0u;

    // A side wall stops short of the floor only where that corner drains, so
    // what spreads into an open corner leaves through it rather than banking
    // up against the edge.
    const bool low = p.y > sp.domain_y - corner_h;
    if (!(low && left_open))  p.x = max(p.x, lo.x);
    if (!(low && right_open)) p.x = min(p.x, hi.x);

    // The bottom edge is open wherever its drain is switched on.
    const float drain = 0.5 * kVNDrainFraction * sp.domain_x;
    const bool over_middle = middle_open && abs(p.x - 0.5 * sp.domain_x) < drain;
    const bool over_corner = (left_open  && p.x < corner_w) ||
                             (right_open && p.x > sp.domain_x - corner_w);
    if (!over_middle && !over_corner) p.y = min(p.y, hi.y);
    return p;
}

static inline float2 vn_collide(float2 p, constant VNSimParams &sp,
                                const device VNObstacle *obstacles, uint ignore) {
    const float r = sp.radius;
    p = vn_collide_walls(p, sp);

    for (uint i = 0; i < sp.obstacles; ++i) {
        if (i < 32u && (ignore & (1u << i)) != 0u) continue;
        const VNObstacle o = obstacles[i];
        const float2 lo = float2(o.min_x, o.min_y);
        const float2 hi = float2(o.max_x, o.max_y);
        if (any(hi <= lo)) continue;

        // Signed distance to the rounded rectangle, grown by the particle radius.
        const float2 half_size = 0.5 * (hi - lo);
        const float  rad = min(o.corner, min(half_size.x, half_size.y));
        const float2 d = abs(p - 0.5 * (lo + hi)) - (half_size - rad);
        const float  sdf = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - rad;
        if (sdf >= r) continue;

        // Outward normal from the same field, by its gradient.
        float2 n = normalize(max(d, 0.0) + 1e-4);
        if (max(d.x, d.y) < 0.0) n = (d.x > d.y) ? float2(sign(p.x - 0.5 * (lo.x + hi.x)), 0.0)
                                                 : float2(0.0, sign(p.y - 0.5 * (lo.y + hi.y)));
        else n *= sign(p - 0.5 * (lo + hi));
        p += n * (r - sdf);
    }
    // The walls again, last, so they win. A window dragged into the edge of
    // the display would otherwise push whatever it traps there straight
    // through it; held here, the solver squeezes it up and out instead.
    return vn_collide_walls(p, sp);
}

constexpr sampler vn_field_sampler(coord::normalized, filter::linear, address::clamp_to_edge);

/// Where another close's water is dense enough to count as water, relative to
/// its own rest density, and how hard its surface pushes back.
constant float kVNCoupleLevel    = 0.15;
constant float kVNCoupleStrength = 2.0;

/// Keeps a particle out of another close's water.
///
/// The closes are separate simulations -- separate particles, separate
/// neighbour searches -- so they cannot feel each other as fluid. Each does
/// have a density field, though, built every frame for drawing, and that is
/// enough for the other to treat it as a soft, moving obstacle: where the
/// other water is denser than kVNCoupleLevel, the particle is pushed down its
/// density gradient, further the deeper it is, never more than half a
/// smoothing radius per substep. Two bodies of water bump and pile against
/// each other; they do not mix.
static inline float2 vn_couple(float2 p, constant VNSimParams &sp, texture2d<float> other) {
    const float2 domain = float2(sp.domain_x, sp.domain_y);
    const float2 uv = p / domain;
    const float  d = other.sample(vn_field_sampler, uv).x;
    if (d <= kVNCoupleLevel) return p;

    const float2 t = 1.0 / float2(float(sp.field_w), float(sp.field_h));
    const float2 g = float2(other.sample(vn_field_sampler, uv + float2(t.x, 0.0)).x -
                            other.sample(vn_field_sampler, uv - float2(t.x, 0.0)).x,
                            other.sample(vn_field_sampler, uv + float2(0.0, t.y)).x -
                            other.sample(vn_field_sampler, uv - float2(0.0, t.y)).x);
    // Flat inside a body of water, where the gradient says nothing: straight up
    // is the way out of a pool.
    const float2 out = length(g) > 1e-4 ? -normalize(g) : float2(0.0, -1.0);
    return p + out * min(kVNCoupleStrength * (d - kVNCoupleLevel) * sp.h, 0.5 * sp.h);
}

#pragma mark - Solver

kernel void vn_pbf_predict(device VNParticle *P          [[buffer(0)]],
                           constant VNSimParams &sp      [[buffer(2)]],
                           const device VNObstacle *obs  [[buffer(3)]],
                           texture2d<float> otherA       [[texture(0)]],
                           texture2d<float> otherB       [[texture(1)]],
                           uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    VNParticle p = P[id];

    if (sp.spawn != 0) {
        // Seeded on a lattice covering the window, not at random: a regular
        // packing is the configuration the rest density was measured from, so
        // the fluid starts at rest instead of exploding on frame one as it
        // resolves its own overlaps.
        const float2 span = float2(sp.spawn_max_x - sp.spawn_min_x, sp.spawn_max_y - sp.spawn_min_y);
        const float  s = sqrt(max(span.x * span.y, 1.0) / float(max(sp.count, 1u)));
        const uint   cols = max(uint(span.x / max(s, 1e-3)), 1u);
        const float2 cell = float2(span.x / float(cols), s);
        const uint2  ij = uint2(id % cols, id / cols);

        // A little jitter breaks the lattice's own symmetry, which would
        // otherwise survive as visible rows for the first few frames.
        const float2 j = (vn_hash22(float2(float(id), sp.seed)) - 0.5) * 0.25;
        p.pos  = float2(sp.spawn_min_x, sp.spawn_min_y) + (float2(ij) + 0.5 + j) * cell;
        p.uv0  = clamp((p.pos - float2(sp.spawn_min_x, sp.spawn_min_y)) / max(span, 1.0), 0.0, 1.0);
        p.vel  = float2(0.0);
        p.ppos = p.pos;
        p.dp   = float2(0.0);
        p.lambda = 0.0;

        // Whatever it is born inside of, it may leave.
        p.ignore = 0u;
        for (uint i = 0; i < sp.obstacles && i < 32u; ++i) {
            if (vn_inside_obstacle(p.pos, obs[i], sp.radius)) p.ignore |= (1u << i);
        }
        P[id] = p;
        return;
    }

    // Parked: it went out through the drain and is no longer part of the fluid.
    if ((p.ignore & VN_DRAINED) != 0u) { P[id] = p; return; }

    // A window that has just become an obstacle may be left by whatever is
    // already inside it, as the ones it is born inside may.
    for (uint i = 0; sp.fresh != 0u && i < sp.obstacles && i < 32u; ++i) {
        if ((sp.fresh & (1u << i)) != 0u && vn_inside_obstacle(p.pos, obs[i], sp.radius)) p.ignore |= (1u << i);
    }

    p.vel.y += sp.gravity * sp.dt;

    // A particle may not cross more than a fraction of the smoothing radius in
    // one substep: past that the 3x3 bin search stops finding the neighbours it
    // is about to hit, and the fluid tunnels through itself and through walls.
    // The fraction is kVNMaxTravel in WaterSim.h and must agree with it.
    const float vmax = 0.35 * sp.h / max(sp.dt, 1e-5);
    const float speed = length(p.vel);
    if (speed > vmax) p.vel *= vmax / speed;

    p.ppos = vn_collide(p.pos + p.vel * sp.dt, sp, obs, p.ignore);
    if (sp.others > 0u) {
        p.ppos = vn_couple(p.ppos, sp, otherA);
        if (sp.others > 1u) p.ppos = vn_couple(p.ppos, sp, otherB);
        p.ppos = vn_collide_walls(p.ppos, sp);   // the walls still win
    }

    // Once it is clear of an obstacle it started in, that obstacle is solid to
    // it again. The margin is generous so a particle grazing the edge does not
    // flicker between the two states.
    for (uint i = 0; i < sp.obstacles && i < 32u; ++i) {
        if ((p.ignore & (1u << i)) != 0u && !vn_inside_obstacle(p.ppos, obs[i], sp.radius * 2.0)) {
            p.ignore &= ~(1u << i);
        }
    }
    // Once it is clear of the bottom of the display it is gone. Parking it
    // rather than leaving it falling forever keeps it out of the bin the clamp
    // would otherwise crowd it into.
    // Out of the domain in any direction the walls no longer hold it.
    if (p.ppos.y > sp.domain_y + 2.0 * sp.h ||
        p.ppos.x < -2.0 * sp.h || p.ppos.x > sp.domain_x + 2.0 * sp.h) {
        p.ignore |= VN_DRAINED;
        p.vel = float2(0.0);
        p.pos = p.ppos;
    }

    P[id] = p;
}

kernel void vn_pbf_bin_clear(device atomic_uint *bins [[buffer(1)]],
                             constant VNSimParams &sp [[buffer(2)]],
                             uint id [[thread_position_in_grid]]) {
    if (id >= sp.bin_w * sp.bin_h) return;
    atomic_store_explicit(&bins[id * vn_bin_stride(sp)], 0u, memory_order_relaxed);
}

kernel void vn_pbf_bin_fill(const device VNParticle *P [[buffer(0)]],
                            device atomic_uint *bins   [[buffer(1)]],
                            constant VNSimParams &sp   [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    if ((P[id].ignore & VN_DRAINED) != 0u) return;   // drained: out of the fluid entirely
    const int2 b = vn_bin_of(P[id].ppos, sp);
    const uint base = (uint(b.y) * sp.bin_w + uint(b.x)) * vn_bin_stride(sp);
    const uint slot = atomic_fetch_add_explicit(&bins[base], 1u, memory_order_relaxed);
    if (slot < sp.bin_slots) {
        atomic_store_explicit(&bins[base + 1u + slot], id, memory_order_relaxed);
    }
}

/// Equation 11: lambda_i = -C_i / (sum_k |grad_k C_i|^2 + eps).
/// The one per-particle neighbour search. Everything downstream walks the list
/// this leaves behind, so the 3x3 bin block is traversed once per step rather
/// than once per kernel per iteration.
kernel void vn_pbf_neighbours(const device VNParticle *P [[buffer(0)]],
                              const device uint *bins    [[buffer(1)]],
                              constant VNSimParams &sp   [[buffer(2)]],
                              device uint *nlist         [[buffer(4)]],
                              uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    const float2 pi = P[id].ppos;
    const int2   b0 = vn_bin_of(pi, sp);
    const uint   stride = vn_bin_stride(sp);
    const float  h2 = sp.h * sp.h;

    uint found = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        const int by = b0.y + dy;
        if (by < 0 || by >= int(sp.bin_h)) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            const int bx = b0.x + dx;
            if (bx < 0 || bx >= int(sp.bin_w)) continue;

            const uint base = (uint(by) * sp.bin_w + uint(bx)) * stride;
            const uint n = min(bins[base], sp.bin_slots);
            for (uint k = 0; k < n && found < kVNMaxNeighbours; ++k) {
                const uint j = bins[base + 1u + k];
                if (j >= sp.count || j == id) continue;
                const float2 r = pi - P[j].ppos;
                if (dot(r, r) >= h2) continue;
                nlist[id * kVNMaxNeighbours + found++] = j;
            }
        }
    }
    // A particle with fewer than the cap keeps the rest of its row untouched,
    // so the count has to be stored rather than inferred.
    nlist[sp.count * kVNMaxNeighbours + id] = found;
}

kernel void vn_pbf_lambda(device VNParticle *P        [[buffer(0)]],
                          constant VNSimParams &sp    [[buffer(2)]],
                          const device uint *nlist    [[buffer(4)]],
                          uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    const float2 pi = P[id].ppos;
    const uint   nn = min(nlist[sp.count * kVNMaxNeighbours + id], kVNMaxNeighbours);

    float  rho = vn_poly6(0.0, sp.h);   // the particle's own contribution
    float2 grad_i = float2(0.0);        // the k == i term of the gradient
    float  sum_grad2 = 0.0;             // the k != i terms

    for (uint t = 0; t < nn; ++t) {
        const uint   j = nlist[id * kVNMaxNeighbours + t];
        const float2 r = pi - P[j].ppos;
        const float  r2 = dot(r, r);
        rho += vn_poly6(r2, sp.h);
        const float2 g = vn_spiky_grad(r, sqrt(r2), sp.h) / sp.rho0;
        grad_i += g;
        sum_grad2 += dot(g, g);
    }

    // Compression only. A particle at the surface of the fluid has barely half
    // a neighbourhood, so its unclamped constraint is around -0.5 -- and a
    // negative constraint is a multiplier that pulls its neighbours INWARD,
    // hard, every iteration. Left in, the surface implodes on frame one and
    // takes the body of the fluid with it; the harness measured 169,000 px/s
    // of it. Cohesion comes from the artificial pressure term and from XSPH
    // instead, which is what they are there for.
    const float C = max(rho / sp.rho0 - 1.0, 0.0);
    sum_grad2 += dot(grad_i, grad_i);
    P[id].lambda = -C / (sum_grad2 + sp.cfm_eps);
}

/// Equations 13 and 14: the position correction, with Monaghan's artificial
/// pressure. That term is what gives the fluid a surface: it keeps particles
/// from clumping where the neighbourhood is thin, and the inward pull it leaves
/// behind reads as surface tension.
kernel void vn_pbf_delta(device VNParticle *P          [[buffer(0)]],
                         constant VNSimParams &sp      [[buffer(2)]],
                         const device VNObstacle *obs  [[buffer(3)]],
                         const device uint *nlist      [[buffer(4)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    const float2 pi = P[id].ppos;
    const float  li = P[id].lambda;
    const uint   nn = min(nlist[sp.count * kVNMaxNeighbours + id], kVNMaxNeighbours);

    float2 dp = float2(0.0);
    for (uint t = 0; t < nn; ++t) {
        const uint   j = nlist[id * kVNMaxNeighbours + t];
        const float2 r = pi - P[j].ppos;
        const float  r2 = dot(r, r);

        const float ratio = vn_poly6(r2, sp.h) / max(sp.scorr_w_dq, 1e-20);
        const float s2 = ratio * ratio;
        const float scorr = -sp.scorr_k * s2 * s2;          // n = 4
        dp += (li + P[j].lambda + scorr) * vn_spiky_grad(r, sqrt(r2), sp.h);
    }

    dp /= sp.rho0;

    // No single iteration may move a particle further than it could have
    // travelled anyway. This is a backstop, not tuning: whatever the density
    // error, a correction larger than the search radius puts the particle
    // somewhere its neighbour list no longer describes.
    const float dpmax = 0.3 * sp.h;
    const float dplen = length(dp);
    if (dplen > dpmax) dp *= dpmax / dplen;

    // Collision response inside the solver loop, as Algorithm 1 line 14 does:
    // a wall met here is resolved by the remaining iterations instead of
    // leaving particles buried in it.
    P[id].dp = vn_collide(pi + dp, sp, obs, P[id].ignore) - pi;
}

kernel void vn_pbf_apply(device VNParticle *P     [[buffer(0)]],
                         constant VNSimParams &sp [[buffer(2)]],
                         uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    P[id].ppos += P[id].dp;
}

kernel void vn_pbf_velocity(device VNParticle *P     [[buffer(0)]],
                            constant VNSimParams &sp [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    VNParticle p = P[id];
    p.vel = (p.ppos - p.pos) / max(sp.dt, 1e-5);

    p.pos = p.ppos;
    P[id] = p;
}

/// Equation 15: the vorticity at a particle, which in the plane is one scalar.
kernel void vn_pbf_vorticity(device VNParticle *P     [[buffer(0)]],
                             constant VNSimParams &sp [[buffer(2)]],
                             const device uint *nlist [[buffer(4)]],
                             uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    const float2 pi = P[id].pos;
    const float2 vi = P[id].vel;
    const uint   nn = min(nlist[sp.count * kVNMaxNeighbours + id], kVNMaxNeighbours);

    float w = 0.0;
    for (uint t = 0; t < nn; ++t) {
        const uint   j = nlist[id * kVNMaxNeighbours + t];
        const float2 r = pi - P[j].pos;
        const float  r2 = dot(r, r);
        const float2 vij = P[j].vel - vi;
        const float2 g = vn_spiky_grad(r, sqrt(r2), sp.h);
        w += vij.x * g.y - vij.y * g.x;
    }
    P[id].lambda = w;
}

/// Equations 16 and 17: vorticity confinement to put back the energy a position
/// based solver damps out, and XSPH viscosity, which is what makes the fluid
/// move as a body rather than as a cloud of independent dots.
kernel void vn_pbf_vel_post(device VNParticle *P     [[buffer(0)]],
                            constant VNSimParams &sp [[buffer(2)]],
                            const device uint *nlist [[buffer(4)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    const float2 pi = P[id].pos;
    const float2 vi = P[id].vel;
    const uint   nn = min(nlist[sp.count * kVNMaxNeighbours + id], kVNMaxNeighbours);

    float2 eta = float2(0.0);
    float2 xsph = float2(0.0);
    for (uint t = 0; t < nn; ++t) {
        const uint   j = nlist[id * kVNMaxNeighbours + t];
        const float2 r = pi - P[j].pos;
        const float  r2 = dot(r, r);
        eta  += abs(P[j].lambda) * vn_spiky_grad(r, sqrt(r2), sp.h);
        xsph += (P[j].vel - vi) * vn_poly6(r2, sp.h);
    }

    float2 v = vi + sp.xsph_c * xsph / sp.rho0;
    const float len = length(eta);
    if (len > 1e-8) {
        const float2 N = eta / len;
        v += sp.vort_eps * float2(N.y * P[id].lambda, -N.x * P[id].lambda) * sp.dt;
    }
    P[id].dp = v;
}

kernel void vn_pbf_vel_commit(device VNParticle *P     [[buffer(0)]],
                              constant VNSimParams &sp [[buffer(2)]],
                              uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    P[id].vel = P[id].dp;
    P[id].dp  = float2(0.0);
}

/// How much of the fluid is still on screen. The CPU reads the count back when
/// the frame's command buffer completes, and a close whose water has all
/// drained ends there instead of running out its duration. The slot is zeroed
/// by the CPU after reading, so nothing here has to clear it.
kernel void vn_pbf_census(const device VNParticle *P [[buffer(0)]],
                          constant VNSimParams &sp   [[buffer(2)]],
                          device atomic_uint *live   [[buffer(5)]],
                          uint id [[thread_position_in_grid]]) {
    if (id >= sp.count) return;
    if ((P[id].ignore & VN_DRAINED) == 0u) atomic_fetch_add_explicit(live, 1u, memory_order_relaxed);
}

#pragma mark - Surface field

/// Gathers the density and the window colour the fluid is carrying into one
/// field, which the blurs smooth and the fragment takes its surface from.
///
/// Gather rather than scatter: a texel asks the bins what is near it, so this
/// needs no atomics and no second buffer to copy out of. `r` is the density
/// relative to rest, so the isosurface is a plain fraction and the half-float
/// field has the precision where it matters. `gb` is the density-weighted
/// window coordinate, which the fragment divides back out.
kernel void vn_field_build(const device VNParticle *P   [[buffer(0)]],
                           const device uint *bins      [[buffer(1)]],
                           constant VNSimParams &sp     [[buffer(2)]],
                           texture2d<float, access::write> field [[texture(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= sp.field_w || gid.y >= sp.field_h) return;

    const float2 texel = float2(sp.domain_x / float(sp.field_w), sp.domain_y / float(sp.field_h));
    const float2 p = (float2(gid) + 0.5) * texel;

    const int2  b0 = vn_bin_of(p, sp);
    const uint  stride = vn_bin_stride(sp);
    const float h2 = sp.h * sp.h;

    float  w = 0.0;
    float2 uv = float2(0.0);
    for (int dy = -1; dy <= 1; ++dy) {
        const int by = b0.y + dy;
        if (by < 0 || by >= int(sp.bin_h)) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            const int bx = b0.x + dx;
            if (bx < 0 || bx >= int(sp.bin_w)) continue;

            const uint base = (uint(by) * sp.bin_w + uint(bx)) * stride;
            const uint n = min(bins[base], sp.bin_slots);
            for (uint t = 0; t < n; ++t) {
                const uint j = bins[base + 1u + t];
                if (j >= sp.count) continue;
                const float2 r = p - P[j].pos;
                const float  r2 = dot(r, r);
                if (r2 >= h2) continue;
                const float k = vn_poly6(r2, sp.h);
                w  += k;
                uv += k * P[j].uv0;
            }
        }
    }
    field.write(float4(w / sp.rho0, uv / sp.rho0, 0.0), gid);
}

// Separable Gaussian, 9 taps. Smoothing the field is what turns a lumpy sum of
// kernels into a surface; without it the fluid reads as beads.
#ifndef VN_BLUR_STRIDE
#define VN_BLUR_STRIDE 1
#endif

constant float kVNBlur[5] = { 0.227027, 0.194595, 0.121622, 0.054054, 0.016216 };

static inline void vn_field_blur(texture2d<float, access::read> src,
                                 texture2d<float, access::write> dst,
                                 constant VNSimParams &sp, uint2 gid, int2 axis) {
    if (gid.x >= sp.field_w || gid.y >= sp.field_h) return;
    const int2 hi = int2(int(sp.field_w) - 1, int(sp.field_h) - 1);

    float4 sum = src.read(gid) * kVNBlur[0];
    for (int i = 1; i < 5; ++i) {
        sum += src.read(uint2(clamp(int2(gid) + axis * i * VN_BLUR_STRIDE, int2(0), hi))) * kVNBlur[i];
        sum += src.read(uint2(clamp(int2(gid) - axis * i * VN_BLUR_STRIDE, int2(0), hi))) * kVNBlur[i];
    }
    dst.write(sum, gid);
}

kernel void vn_field_blur_x(texture2d<float, access::read> src  [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            constant VNSimParams &sp [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
    vn_field_blur(src, dst, sp, gid, int2(1, 0));
}

kernel void vn_field_blur_y(texture2d<float, access::read> src  [[texture(0)]],
                            texture2d<float, access::write> dst [[texture(1)]],
                            constant VNSimParams &sp [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
    vn_field_blur(src, dst, sp, gid, int2(0, 1));
}

#pragma mark - Fragment


/// Shades the isosurface of the smoothed field.
///
/// The normal is the field's own gradient -- the surface is defined by the
/// field, so its gradient IS the surface normal, which is the whole point of
/// working in screen space. Out of that come the three things that make a
/// surface read as water rather than as coloured fog: it refracts what is
/// behind it, it has a specular highlight, and it turns reflective at grazing
/// angles.
///
/// What it refracts is the window's own pixels. The compositor gives this
/// fragment one texture, the clone's content, and no way to sample the desktop
/// behind it -- so the liquid is made of the window that is closing, carried
/// along by the particles as `uv0` and distorted by the surface it now has.
fragment float4 vn_uber_water(VNUberStage in [[stage_in]],
                              texture2d<float> tex2D        [[texture(0)]],
                              texture2d<float> fieldTex     [[texture(1)]],
                              constant VNUberArgs &args     [[buffer(0)]],
                              constant VNShaderExtra &extra [[buffer(kVNShaderExtraIndex)]],
                              constant VNSimParams &sp      [[buffer(11)]],
                              sampler samp [[sampler(0)]]) {
    const float2 uv = vn_window_uv(in.tex.xy / max(in.tex.w, 1e-6), extra);
    const float  t  = vn_phase(args, extra);
    const bool   inside_window = all(uv >= 0.0) && all(uv <= 1.0);

    // The registry's identity rule: the clone composites once with the tag on
    // before the animation starts, and that frame has to be the untouched
    // window. The same branch covers a frame with no simulation behind it.
    // Its simulation was handed to a newer close: nothing of it is left to draw,
    // and the untouched window would be a window coming back.
    if (sp.valid == 2u) return float4(0.0);

    if (t <= 0.0 || sp.valid == 0) {
        return inside_window ? tex2D.sample(samp, uv) : float4(0.0);
    }

    // Where the window itself sits inside the clone's texture. The clone was
    // made from the frame, which carries the drop shadow, so the window's own
    // pixels are a sub-rect of [0,1] and not the whole of it. params[2..4] have
    // carried those insets since the first shader animation; this one was the
    // only one not using them.
    float2 w0, wsz;
    vn_window_rect(extra, w0, wsz);

    const float2 px = in.pos.xy;                       // [[position]]: render-target pixels
    const float2 fuv = px / float2(sp.domain_x, sp.domain_y);
    const float2 dtx = 1.0 / float2(float(sp.field_w), float(sp.field_h));

    const float4 f = fieldTex.sample(vn_field_sampler, fuv);
    const float  d = f.x;

    // Nothing is drawn under the liquid. The fluid is seeded across the
    // window's own rect, so on the first simulated frame the liquid already IS
    // the window, in place, carrying its pixels -- there is nothing to fade
    // between. Any copy drawn underneath is a second window on screen doing a
    // slow dissolve that the app never asked for.
    const float4 out = float4(0.0);

    const float cover = smoothstep(sp.iso * 0.6, sp.iso, d);
    if (cover <= 0.001) return out;

    // The surface normal, from the field's gradient. z is a constant rather
    // than anything measured: this is a plane of liquid seen face on, so the
    // surface tilts only where the field falls away, at the edges and the
    // ripples -- which is exactly where the gradient is large.
    const float dx = fieldTex.sample(vn_field_sampler, fuv + float2(dtx.x, 0)).x
                   - fieldTex.sample(vn_field_sampler, fuv - float2(dtx.x, 0)).x;
    const float dy = fieldTex.sample(vn_field_sampler, fuv + float2(0, dtx.y)).x
                   - fieldTex.sample(vn_field_sampler, fuv - float2(0, dtx.y)).x;
    const float3 n = normalize(float3(-dx, -dy, 0.55 * sp.iso));

    // The window coordinate this parcel of liquid is carrying, refracted
    // through that normal.
    //
    // `uv0` runs 0..1 across the WINDOW, because that is the rect the fluid was
    // seeded across -- but the texture's 0..1 is the FRAME. Sampling one with
    // the other is what put a thick dark border of drop shadow around a
    // shrunken copy of the window inside the liquid. Mapping it through the
    // window's sub-rect is the whole correction.
    const float2 carried = f.yz / max(d, 1e-4);
    const float2 refracted = w0 + clamp(carried + n.xy * 0.12, 0.0, 1.0) * wsz;
    float4 liquid = tex2D.sample(samp, refracted);

    // Thin liquid is see-through and the body of it is not, which is what
    // makes a splash read as a splash and a puddle as a puddle.
    const float thickness = saturate(d / max(sp.iso * 2.5, 1e-3));
    liquid.rgb = mix(liquid.rgb * 1.08, liquid.rgb * float3(0.62, 0.78, 1.0), thickness * 0.55);

    // The window's pixels are what the fluid is made of, but they do not stay
    // legible: once the liquid has folded over itself a few times the carried
    // coordinates are stretched to noise, and a marbled window reads as static
    // rather than as water. So the colour settles toward water blue as the
    // close goes on -- the window is recognisable while it still has a shape,
    // and water by the time it does not.
    //
    // How far and how soon is sp.tint, from the preferences. One knob moves
    // both, because they are the same question asked twice: at 0 the liquid
    // keeps the window's own colours the whole way, and at 1 it is water almost
    // immediately.
    // Measured in seconds from when the fluid was born, not as a share of the
    // close: a close can run for half a minute, and the water has to look like
    // water long before that.
    const float  wt = saturate(sp.age / kVNTintSeconds);

    const float3 kWaterBlue = float3(0.30, 0.52, 0.78);
    const float  amount = saturate(sp.tint);
    const float  begin  = mix(0.85, 0.02, amount);
    const float  finish = mix(1.00, 0.30, amount);
    const float  settled = smoothstep(begin, finish, wt) * amount;
    liquid.rgb = mix(liquid.rgb, kWaterBlue, settled * 0.8);

    const float3 L = normalize(float3(-0.35, -0.6, 0.72));
    const float3 V = float3(0.0, 0.0, 1.0);
    const float  spec = pow(saturate(dot(normalize(L + V), n)), 48.0);
    const float  fresnel = pow(1.0 - saturate(dot(n, V)), 3.0);

    liquid.rgb += spec * 0.9 + fresnel * 0.35;
    liquid.a = saturate(mix(0.72, 1.0, thickness));

    return mix(out, liquid, cover * liquid.a * saturate(sp.fade));
}

#endif // VN_WATER_METAL
