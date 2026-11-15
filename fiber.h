#ifndef FIBER_H
#define FIBER_H
// Finite open-cylinder fiber geometry and the approximate four-lobe fiber
// scattering model. All material math is double precision.
//
// Coordinate conventions
// ----------------------
// A fiber is the lateral surface of a finite cylinder with endpoints A, B
// and radius r: no end caps, no triangle mesh. A point on the axis is
// Q(u) = A + u (B-A), u in [0,1]; T = normalize(B-A).
//
// At a side hit, wo points back along the incident ray (wo = -ray_dir, unit
// length). The material frame is:
//   so = dot(wo, T), co = sqrt(1 - so^2)
//   X  = normalize(wo - T*so)           (wo projected on the T-perp plane)
//   Y  = cross(T, X)                    (right handed with T, X)
//   h  = dot(N, Y)                      (cross-section coordinate, [-1,1])
// Directions wi use s = dot(wi,T) in [-1,1] and
// phi = atan2(dot(wi,Y), dot(wi,X)); solid angle is d(omega) = ds d(phi).
#include "core.h"
#include <array>

struct FiberMaterial {
    double eta = 1.5;                 // relative IOR, valid in [1.001, 3]
    dvec3  sigma_a{0.0, 0.0, 0.0};    // RGB absorption, per world length, >= 0
    double beta_m = 0.25;             // longitudinal width,  in [0.02, 1]
    double beta_n = 0.25;             // azimuthal width,      in [0.02, 1]
};

struct Fiber {
    dvec3 A;
    dvec3 B;
    double radius = 0.0;
    FiberMaterial material;
};

struct FiberHit {
    int    fiber_id = -1;  // stable index of the fiber in the scene vector
    double t = 0.0;        // ray parameter of the side hit, in [tmin,tmax)
    dvec3  point;          // hit position in world space
    dvec3  normal;         // outward unit normal, always perpendicular to T
    double h = 0.0;        // cross-section coordinate dot(normal, Y)
    double u = 0.0;        // axial parameter in [0,1]
};

// Parameter validation. Rejects non-finite values, zero-length fibers,
// non-positive radii and out-of-range material parameters.
bool fiber_material_valid(const FiberMaterial& m);
bool fiber_valid(const Fiber& f);

// Side-only intersection with a finite open cylinder. Rays parallel to the
// axis never hit (there is no side to cross); a tangent root (zero
// discriminant) counts as a hit. The search interval is half-open
// [tmin, tmax) and the axial parameter must lie in [0,1]. On success the
// hit structure, including h, is filled.
bool fiber_intersect(const Fiber& fiber, int fiber_id,
                     const dvec3& orig, const dvec3& dir,
                     double tmin, double tmax, FiberHit& hit);

// Orthonormal material frame built from the tangent and wo (both unit).
// Side intersections always have co > 0; the co == 0 fallback is a
// deterministic perpendicular vector used only if a caller asks for a frame
// around an axis-parallel direction.
struct FiberFrame {
    dvec3 T;
    dvec3 X;
    dvec3 Y;
    double so = 0.0;
    double co = 0.0;
};
FiberFrame make_fiber_frame(const dvec3& T, const dvec3& wo);

// The four attenuation lobes of the approximate model. This is not a full
// Marschner model: there is no tilt angle and no volume scattering.
//   A0 = F            (surface reflection)
//   A1 = (1-F)^2 C    (transmission through the chord)
//   A2 = A1 C F       (one internal round trip)
//   A3 = A2 C F/(1-CF) (all higher orders merged into one lobe)
// The grazing 0/0 limit of A3 is defined as 0. The attenuated lobes are
// never renormalized: absorbed energy simply disappears.
struct FiberLobes {
    double F = 0.0;                 // unpolarized Fresnel, scalar
    dvec3  C{0.0, 0.0, 0.0};        // per-channel chord transmission
    std::array<dvec3, 4> A;         // A[p] per-channel lobe attenuation
    double gamma_o = 0.0;
    double gamma_t = 0.0;
    double eta_p = 0.0;
    double ct = 0.0;
    double chord = 0.0;             // internal path length in world units
};
FiberLobes fiber_compute_lobes(const FiberMaterial& m, const FiberFrame& frame,
                               double h, double radius);

// Lobe selection probabilities: arithmetic mean of the lobe's RGB
// attenuation divided by the sum of the four means.
void fiber_lobe_probabilities(const FiberLobes& lobes, double probs[4]);

// Directional energy kernel K(wi) w.r.t. solid angle ds d(phi). It already
// includes the correct projected measure for cylindrical fibers, so no
// surface cosine must be multiplied on top of it.
dvec3 fiber_evaluate_K(const FiberMaterial& m, const FiberFrame& frame,
                       double h, double radius, const dvec3& wi);

// Full four-lobe mixture pdf (never the density of the picked lobe alone).
double fiber_pdf(const FiberMaterial& m, const FiberFrame& frame,
                 double h, double radius, const dvec3& wi);

struct FiberSample {
    bool  ok = false;   // false on non-finite or out-of-range inputs
    dvec3 wi;           // unit sampled direction
    dvec3 K;            // kernel evaluated at wi
    double pdf = 0.0;   // full mixture pdf at wi
};
// Importance sample the model from three independent samples in [0,1).
// The call never retries: u3 picks the lobe once, u1 drives a truncated
// normal inverse CDF for s, u2 an inverse-CDF azimuth draw. Endpoints are
// handled deterministically (u1 == 0 -> s == -1; at the s poles the
// azimuth collapses and wi == +/- T).
FiberSample fiber_sample(const FiberMaterial& m, const FiberFrame& frame,
                         double h, double radius,
                         double u1, double u2, double u3);

#endif // FIBER_H
