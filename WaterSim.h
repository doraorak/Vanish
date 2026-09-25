#ifndef VN_WATER_SIM_H
#define VN_WATER_SIM_H

// The CPU half of the water simulation: the structures the kernels read, and
// the derivation of the constants that cannot be hardcoded because they depend
// on how much liquid a particular window turns into.
//
// Shared by Vanish.c and tools/watertest, so the parameters the fluid is tested
// with are the parameters it ships with. Anything here that disagrees with
// shaders/Water.metal is a bug the static asserts below are meant to catch.

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/// How far apart the particles sit, in RENDER-TARGET PIXELS -- fixed, and the
/// count derived from it, rather than the other way round.
///
/// This was a fixed count of 4096 and it made the physics depend on the size of
/// the window you closed. The spacing came out of the area, the smoothing
/// radius is three times the spacing, and the speed a particle may travel in
/// one substep is a fraction of the smoothing radius -- so a small window got a
/// small h and a speed cap three times lower than a large one. The same fluid
/// fell at a third the speed and collisions went soft, purely because the
/// window was small. Fixing the spacing instead fixes h, fixes the speed cap,
/// and has the side benefit that a small window is cheaper rather than merely
/// slower.
#ifndef kVNParticleSpacingPx
#define kVNParticleSpacingPx 7.0
#endif

/// Bounds on the derived count: enough to read as a body of liquid, capped so a
/// full-screen window cannot cost the frame everything. A window large enough
/// to hit the cap gets wider spacing, and a proportionally higher speed cap --
/// which is the harmless direction.
#define kVNParticleCountMin 512u
#ifndef kVNParticleCountMax
#define kVNParticleCountMax 8192u
#endif

/// Jacobi iterations of the density solve. The paper uses 4; 3 is the usual
/// real-time trade and is what the test harness was tuned against.
#ifndef kVNSolverIterations
#define kVNSolverIterations 3u
#endif

/// Solver steps per displayed frame. One step per frame would need the fluid to
/// move less than the smoothing radius in 1/60 s, which free fall across a
/// display breaks; each substep halves the displacement it has to survive.
#ifndef kVNSubSteps
#define kVNSubSteps 6u
#endif

/// Neighbours kept per particle. Must match kVNMaxNeighbours in Water.metal.
#define kVNMaxNeighbours 48u

/// The bin grid. The cell is never smaller than the smoothing radius, so the
/// 3x3 block around a particle holds every neighbour within it -- but it must
/// not be much larger either, or a cell holds far more particles than it has
/// slots and the overflow silently loses its neighbours. 64 cells across a
/// 3024px display forces 47px cells for a small window whose smoothing radius
/// is 29px, which put 24 particles in cells with 32 slots; 128 lets the cell
/// follow h instead, for two megabytes.
#define kVNBinDimMax 128u
#define kVNBinSlots  32u
#define kVNBinStride (1u + kVNBinSlots)

/// The surface field the fragment shades. Fixed, so it is allocated once and no
/// display size can force a resize -- a resize would mean freeing a texture
/// that may still be in flight in a command buffer we no longer own.
#ifndef kVNFieldDim
#define kVNFieldDim 512u
#endif

/// Smoothing radius as a multiple of the particle spacing. Three gives roughly
/// 28 neighbours in the plane, comfortably inside kVNMaxNeighbours.
#define kVNSmoothingRatio 3.0f

/// Gravity in pixels per second squared. Not earth's: a display is about a
/// third of a metre tall, which in these pixels makes real gravity fast enough
/// that the fall is over before it reads as one.
#define kVNGravityPx 5000.0f

/// How far a particle may travel in one substep, as a fraction of the smoothing
/// radius. Past this the 3x3 bin search stops finding the neighbours a particle
/// is about to collide with and the fluid tunnels through itself.
///
/// The neighbour list is rebuilt from predicted positions every substep and the
/// solve's own correction is capped separately, so this only has to keep a pair
/// that is about to touch inside each other's search before the step -- which
/// 0.35 was far more conservative than. At 0.6, with three substeps, the cap
/// sits above anything gravity reaches in a close: it is a backstop again
/// rather than the thing setting how fast water falls.
#define kVNMaxTravel 0.6f

/// Equation 13. |dq| = 0.1h..0.3h, k = 0.1 and n = 4 are the paper's values;
/// n is fixed at 4 in the kernel.
#define kVNScorrK  0.1f
#define kVNScorrDq 0.2f

/// Equation 17, the paper's typical value.
#define kVNXSPHc 0.01f

/// Equation 16. Position based solvers damp; this puts some of it back.
#define kVNVorticityEps 0.00008f

/// The openings in the floor, one bit each, switched from the preferences.
#define kVNDrainLeft   1u
#define kVNDrainMiddle 2u
#define kVNDrainRight  4u
#define kVNDrainAll    (kVNDrainLeft | kVNDrainMiddle | kVNDrainRight)

/// How much of the rest density the isosurface sits at, after blurring.
#define kVNIsoLevel 0.55f

/// Default for the tint knob, chosen to match what the shader did before it
/// was adjustable.
#define kVNWaterTintDefault 0.70f

/// Equation 11's relaxation, as a fraction of the gradient sum a particle has
/// in a full neighbourhood. Expressed relatively because the absolute value
/// depends on h, which depends on the window.
#ifndef kVNCFMRelax
#define kVNCFMRelax 0.02f
#endif

/// Must match VNParticle in Water.metal.
typedef struct {
    float pos[2];
    float vel[2];
    float uv0[2];
    float ppos[2];
    float dp[2];
    float lambda;
    /// One bit per obstacle the particle started inside of, and must therefore
    /// pass through rather than be ejected from. See vn_collide in Water.metal.
    uint32_t ignore;
} VNParticle;

/// Must match VNSimParams in Water.metal. Scalars only: no vector member can
/// disagree about alignment between C and MSL if there is no vector member.
typedef struct {
    float domain_x, domain_y;
    float spawn_min_x, spawn_min_y;
    float spawn_max_x, spawn_max_y;
    float dt;
    float gravity;
    float h;
    float rho0;
    float cfm_eps;
    float scorr_k;
    float scorr_w_dq;
    float xsph_c;
    float vort_eps;
    float radius;
    float iso;
    float seed;
    uint32_t count;
    uint32_t spawn;
    uint32_t bin_w, bin_h;
    float    cell;
    uint32_t bin_slots;
    uint32_t field_w, field_h;
    uint32_t obstacles;
    uint32_t valid;
    /// How far the liquid settles from the window's own pixels toward water
    /// blue, 0..1, from the preferences. 0 keeps the window's colours for the
    /// whole close; 1 turns it almost at once.
    float    tint;
    /// Seconds since the fluid was seeded. The tint runs on this rather than on
    /// the close's phase, so it takes the same time whatever the duration.
    float    age;
    /// Which floor openings are open: kVNDrainLeft | kVNDrainMiddle | kVNDrainRight.
    uint32_t drains;
    /// 1 while the water is shown, falling to 0 as it fades out -- at the end
    /// of its duration, or when a newer water close takes over.
    float    fade;
    /// Obstacles that appeared this step, one bit per index. A particle inside
    /// one is let out of it rather than thrown to its edge.
    uint32_t fresh;
    /// How many other closes' density fields are bound for this one to keep
    /// out of, 0..2. See vn_couple in Water.metal.
    uint32_t others;
} VNSimParams;

/// Must match VNObstacle in Water.metal.
typedef struct {
    float min_x, min_y, max_x, max_y;
    float corner;
    float _pad;
} VNObstacle;

#define kVNMaxObstacles 24

_Static_assert(sizeof(VNParticle) == 48, "VNParticle must match Water.metal");
_Static_assert(sizeof(VNSimParams) == 136, "VNSimParams must match Water.metal");
_Static_assert(sizeof(VNObstacle) == 24, "VNObstacle must match Water.metal");

/// The 2D kernels, in the same form the shader uses them. 2D, not 3D: this
/// fluid lives in the plane of the screen, and the 3D normalisations would put
/// the rest density and the pressure gradient on different scales.
static inline double vn_sph_poly6(double r2, double h) {
    const double d = h * h - r2;
    if (d <= 0.0) return 0.0;
    const double h4 = h * h * h * h;
    return (4.0 / M_PI) * (d * d * d) / (h4 * h4);
}

static inline double vn_sph_spiky_grad_mag(double r, double h) {
    if (r <= 1e-5 || r >= h) return 0.0;
    const double h2 = h * h;
    return (30.0 / M_PI) * (h - r) * (h - r) / (h2 * h2 * h);
}

/// Fills in everything that follows from how much liquid there is.
///
/// The rest density is measured, not assumed: it is the density a particle has
/// in the regular lattice the fluid is seeded on, summed here with the same
/// kernel the shader uses. Getting it from a formula for some other spacing
/// would make the fluid expand or collapse on its first frame.
///
/// The relaxation of equation 11 is measured the same way, as a fraction of the
/// gradient sum in a full neighbourhood, because its absolute size depends on h
/// and h depends on the window.
static inline void vn_water_derive(VNSimParams *sp, float span_x, float span_y) {
    const double area = (double)span_x * (double)span_y;

    // Count from spacing, not spacing from count.
    double count = floor(area / (kVNParticleSpacingPx * kVNParticleSpacingPx));
    if (count < (double)kVNParticleCountMin) count = (double)kVNParticleCountMin;
    if (count > (double)kVNParticleCountMax) count = (double)kVNParticleCountMax;

    // Then the true spacing, which equals the target except where the cap bit.
    const double s = sqrt(fmax(area, 1.0) / count);
    const double h = kVNSmoothingRatio * s;

    double rho0 = 0.0, grad2 = 0.0;
    double gx = 0.0, gy = 0.0;
    const int reach = (int)ceil(h / s) + 1;
    for (int j = -reach; j <= reach; j++) {
        for (int i = -reach; i <= reach; i++) {
            const double dx = i * s, dy = j * s;
            const double r2 = dx * dx + dy * dy;
            if (r2 >= h * h) continue;
            rho0 += vn_sph_poly6(r2, h);
            if (i == 0 && j == 0) continue;
            const double r = sqrt(r2);
            const double g = vn_sph_spiky_grad_mag(r, h);
            const double ux = -dx / r, uy = -dy / r;   // the shader's -rhat
            grad2 += (g * ux) * (g * ux) + (g * uy) * (g * uy);
            gx += g * ux;
            gy += g * uy;
        }
    }
    grad2 += gx * gx + gy * gy;          // the k == i term of equation 11
    grad2 /= (rho0 * rho0);

    sp->h          = (float)h;
    sp->rho0       = (float)rho0;
    sp->cfm_eps    = (float)(kVNCFMRelax * grad2);
    sp->scorr_k    = kVNScorrK;
    sp->scorr_w_dq = (float)vn_sph_poly6((kVNScorrDq * h) * (kVNScorrDq * h), h);
    sp->xsph_c     = kVNXSPHc;
    sp->vort_eps   = kVNVorticityEps;
    sp->radius     = (float)(0.5 * s);
    sp->iso        = kVNIsoLevel;
    sp->tint       = kVNWaterTintDefault;   // the caller overrides from prefs
    sp->count      = (uint32_t)count;
    sp->bin_slots  = kVNBinSlots;
    sp->field_w    = kVNFieldDim;
    sp->field_h    = kVNFieldDim;

    // The cell must be at least h for the 3x3 search to be complete, and the
    // grid must fit the fixed allocation, so on a very wide display the cells
    // grow instead of the table.
    const double cell = fmax(h, fmax((double)sp->domain_x, (double)sp->domain_y) / (double)kVNBinDimMax);
    sp->cell   = (float)cell;
    sp->bin_w  = (uint32_t)fmin(ceil(sp->domain_x / cell), (double)kVNBinDimMax);
    sp->bin_h  = (uint32_t)fmin(ceil(sp->domain_y / cell), (double)kVNBinDimMax);
    if (sp->bin_w < 1) sp->bin_w = 1;
    if (sp->bin_h < 1) sp->bin_h = 1;
}

/// The velocity clamp that keeps a particle inside the bin search in one
/// substep. Returns the maximum speed in pixels per second.
static inline float vn_water_max_speed(const VNSimParams *sp) {
    return sp->dt > 1e-6f ? (kVNMaxTravel * sp->h / sp->dt) : 1e9f;
}

#endif /* VN_WATER_SIM_H */
