// Shaders for Vanish's VN_ANIM_SHADER animation kind.
//
// Compiled to Vanish.metallib by package.sh and shipped inside the tweak
// bundle. WindowServer only ever loads the finished library -- it never sees
// this source, never runs the Metal compiler, and never talks to the compiler
// service. Every error here is a build error.
//
// The signatures mirror SkyLight's own UberCompositeVertex / UberCompositeFragment
// exactly, substituting the stock library in WindowServer's compositor.
//
// Modular structure:
//   - Common.metal    : Shared ABI structs, vertex shader (vn_uber_vertex),
//                       coordinate transforms, hashing & noise utilities.
//   - Dissolve.metal  : Thanos particle dissolve (vn_uber_dissolve).
//   - CRT.metal       : Retro CRT collapse (vn_uber_crt).
//   - Shatter.metal   : Invertible Voronoi glass shatter (vn_uber_shatter).
//   - Burn.metal      : EDR / HDR burning wavefront (vn_uber_burn).
//   - Water.metal     : Particle simulation, stepped by vn_particle_* in a
//                       compute pass and drawn by vn_uber_water.

#include "Common.metal"
#include "Dissolve.metal"
#include "CRT.metal"
#include "Shatter.metal"
#include "Burn.metal"
#include "Water.metal"
