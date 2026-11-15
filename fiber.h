#ifndef FIBER_H
#define FIBER_H
#include "core.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Finite open-cylinder fiber geometry and an explicit four-lobe approximate
// fiber scattering model.
//
// This is NOT a full Marschner model: there is no cuticle tilt (the fiber is
// treated as azimuthally symmetric) and no volumetric scattering.  It keeps
// the R/TT/TRT/... structure of a smooth dielectric cylinder, merges every
// bounce order p >= 3 into lobe 3, and drives all shading math in double.
//
// Units: positions, fiber radii and absorption coefficients sigma_a are in
// world units; sigma_a is an absorption coefficient per world length, so the
// transmittance of a chord of length l is exp(-sigma_a * l), evaluated
// independently per RGB channel.
// ---------------------------------------------------------------------------

// Material parameters of one fiber.
struct FiberMaterial {
    double eta    = 1.5;            // relative IOR, air -> fiber; valid range [1.001, 3]
    dvec3  sigma_a{0.0, 0.0, 0.0};  // RGB absorption per world length; every component >= 0
    double beta_m = 0.25;           // longitudinal lobe width;  valid range [0.02, 1]
    double beta_n = 0.5;            // azimuthal lobe sharpness; valid range [0.02, 1]

    // Returns false for any out-of-range or non-finite parameter instead of
    // letting an invalid material reach the tracer.
    bool valid() const;
};

// A finite open cylinder: the lateral surface swept by radius r around the
// segment A..B.  There are no end caps and no triangle mesh; rays parallel to
// the axis never hit the side surface.
struct Fiber {
    dvec3 a;                        // endpoint A
    dvec3 b;                        // endpoint B
    double radius = 0.0;            // cylinder radius, world units; must be > 0
    FiberMaterial material;
    int id = 0;                     // stable caller-assigned fiber number

    bool valid() const;             // finite endpoints, non-degenerate, r > 0, material valid
};

// Local shading frame at a fiber hit.
//   T  = normalize(B - A)                      fiber tangent, A -> B
//   wo = -ray direction (points back toward the viewer)
//   X  = normalized projection of wo onto the plane perpendicular to T
//   Y  = cross(T, X)
// so = dot(wo, T); co = sqrt(1 - so^2) is the cosine of the incidence angle
// against the cylinder normal plane.
struct FiberFrame {
    dvec3 T;
    dvec3 X;
    dvec3 Y;
    double so = 0.0;
    double co = 0.0;
};

// Result of a lateral-surface intersection.
struct FiberHit {
    int    id = -1;                 // stable fiber id
    double t  = 0.0;                // ray parameter of the hit
    dvec3  point;                   // hit position in world space
    dvec3  normal;                  // outward unit surface normal
    double h = 0.0;                 // section coordinate dot(N, Y) in [-1, 1]
    FiberFrame frame;               // T/X/Y/so/co usable directly by the material
};

// Build the T/X/Y frame for an outgoing direction wo (must be unit length)
// around tangent T.  Degenerate convention: when wo is parallel to T (co == 0)
// the side surface cannot be hit, so X and Y fall back to an orthonormal basis
// of the plane perpendicular to T; the caller must never evaluate a grazing
// co == 0 hit.
FiberFrame fiber_frame(const dvec3 &wo, const dvec3 &T);

// Intersect a unit-length ray (orig, dir) with the lateral surface of one
// fiber, searching t in [tmin, tmax) and accepting only hits whose axial
// parameter u in [0, 1] (u = 0 at A, u = 1 at B; both endpoints included).
// A ray tangent to the cylinder counts as a (grazing) hit.  A ray parallel to
// the axis never hits.  Returns true and fills *hit on success.
bool intersect_fiber(const Fiber &fiber, const dvec3 &orig, const dvec3 &dir,
                     double tmin, double tmax, FiberHit *hit);

// Nearest lateral hit across all fibers.  Exact-equal distances resolve to the
// smallest fiber id.  Returns -1 id when nothing is hit.
FiberHit intersect_fibers(const Fiber *fibers, std::size_t count,
                          const dvec3 &orig, const dvec3 &dir,
                          double tmin, double tmax, int skip_id);

// Deterministic sample stream.  Three independent [0, 1) channels are drawn in
// a fixed order (u, then v, then w); consumed() reports how many scalars were
// taken so transport tests can verify the exact sample budget of each event.
// Samples at the channel boundary map to the closed conventions below rather
// than being retried.
class SampleStream {
public:
    SampleStream() = default;
    explicit SampleStream(std::uint64_t seed) { reset(seed); }

    void reset(std::uint64_t seed);             // deterministic splitmix64 stream
    double draw();                              // one [0, 1) scalar; advances the stream
    std::uint64_t consumed() const { return used_; }

private:
    std::uint64_t state_ = 0x9e3779b97f4a7c15ULL;
    std::uint64_t used_  = 0;
};

// Four per-channel attenuation values A0..A3 (lobes R, TT, TRT, merged tail).
struct FiberLobes {
    dvec3  A[4];
    double h = 0.0;              // section coordinate dot(N, Y) the lobes were built from
    double F = 0.0;              // unpolarized Fresnel at entry
    double gamma_o = 0.0;        // incidence section angle, asin(h)
    double gamma_t = 0.0;        // refracted section angle
    double chord = 0.0;          // internal chord length l (world units)
    double eta_p = 0.0;          // projected IOR
};

// Exact unpolarized Fresnel reflectance for light travelling from air (IOR 1)
// into a medium of IOR eta at interface cosine cosi in [0, 1].  Total internal
// reflection cannot occur entering from air; the grazing limit returns 1.
double fresnel_unpolarized(double cosi, double eta);

// Truncated standard-normal density on [-1, 1], re-normalized to integrate to
// one over that interval.  mean is the (un-truncated) mean -so; sigma > 0.
double truncated_normal_pdf(double x, double mean, double sigma);

// Inverse CDF of the same truncated normal.  u in [0, 1]: u == 0 -> -1,
// u == 1 -> 1 (closed endpoints, no rejection).
double inverse_truncated_normal(double u, double mean, double sigma);

// Periodic azimuthal density N_p(phi), a Poisson-kernel density normalized over
// [0, 2*pi).  |rho| < 1; rho == 0 degenerates to the uniform 1/(2*pi).
double azimuthal_density(double phi, double center, double rho);

// Compute the four lobe attenuations from the actual geometry of a hit.
// h is dot(N, Y) in [-1, 1], r the fiber radius, frame the local T/X/Y frame.
FiberLobes fiber_attenuation(const FiberMaterial &mat, double h, double r,
                             const FiberFrame &frame);

// Directional energy kernel K(wi) with respect to solid angle d omega = ds dphi
// (s = dot(wi, T), phi = atan2(dot(wi, Y), dot(wi, X))).  wi must be unit
// length.  K already includes every surface cosine factor internally; callers
// must not multiply by another cosine.
dvec3 fiber_kernel(const FiberMaterial &mat, const FiberLobes &lobes,
                   const FiberFrame &frame, const dvec3 &wi);

// Mixture pdf (per solid angle) used by fiber_sample: the sum of all four
// lobe pdfs weighted by the lobe selection probabilities.  It is the pdf of
// the full mixture, never the density of the single lobe that was drawn.
double fiber_pdf(const FiberMaterial &mat, const FiberLobes &lobes,
                 const FiberFrame &frame, const dvec3 &wi);

struct FiberSample {
    dvec3  wi;                   // unit-length sampled scattered direction
    dvec3  kernel;               // K(wi), per RGB channel
    double pdf = 0.0;            // full four-lobe mixture pdf at wi
    int    lobe = -1;            // lobe that generated wi (0..3)
};

// Draw one scattered direction from three [0, 1) samples:
//   u selects the lobe, v is inverted through the longitudinal CDF,
//   w is inverted through the azimuthal CDF.
// Lobe probabilities are the per-lobe means over RGB of A_p, divided by the
// sum over the four lobes; no retry loop is used.  Pole convention: a sample
// landing on |s| == 1 yields wi = +/- T with phi defined by the same (X, Y)
// frame, and both kernel and pdf are still reported.
FiberSample fiber_sample(const FiberMaterial &mat, const FiberLobes &lobes,
                         const FiberFrame &frame, double u, double v, double w);

#endif // FIBER_H
