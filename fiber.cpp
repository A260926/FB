#include "fiber.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {
constexpr double PI      = 3.14159265358979323846;
constexpr double TWO_PI  = 6.28318530717958647693;
constexpr double EPS     = 1e-12;

double clamp_unit(double x) { return std::max(-1.0, std::min(1.0, x)); }

// Wrap an angle to [-pi, pi).
double wrap_pi(double a) {
    a = std::fmod(a + PI, TWO_PI);
    if (a < 0.0) a += TWO_PI;
    return a - PI;
}

double standard_normal_pdf(double x) {
    return std::exp(-0.5*x*x)*0.39894228040143267794;
}

double standard_normal_cdf(double x) {
    return 0.5*std::erfc(-x*0.70710678118654752440);
}

// Peter Acklam's inverse standard normal CDF, with one Newton polish.
double inverse_standard_normal(double p) {
    const double a[] = {-3.969683028665376e+01,  2.209460984245205e+02,
                        -2.759285104469687e+02,  1.383577518672690e+02,
                        -3.066479806614716e+01,  2.506628277459239e+00};
    const double b[] = {-5.447609879822406e+01,  1.615858368580409e+02,
                        -1.556989798598866e+02,  6.680131188771972e+01,
                        -1.328068155288572e+01};
    const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                        -2.400758277161838e+00, -2.549732539343734e+00,
                         4.374664141464968e+00,  2.938163982698783e+00};
    const double d[] = { 7.784695709041462e-03,  3.224671290700398e-01,
                         2.445134137142996e+00,  3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - plow;
    double q, r, x;
    if (p < plow) {
        q = std::sqrt(-2.0*std::log(p));
        x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
            ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    } else if (p <= phigh) {
        q = p - 0.5;
        r = q*q;
        x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q /
            (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1.0);
    } else {
        q = std::sqrt(-2.0*std::log(1.0-p));
        x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
             ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    }
    double e = standard_normal_cdf(x) - p;
    double u = e*std::sqrt(2.0*PI)*std::exp(0.5*x*x);
    return x - u/(1.0 + 0.5*x*u);
}

struct LobeShape {
    double mean;     // longitudinal truncated-normal mean (-so)
    double sigma;    // longitudinal standard deviation (p+1)*beta_m
    double center;   // azimuthal center, wrapped to [-pi, pi)
    double rho;      // azimuthal concentration, 0 for the uniform lobe 3
};

LobeShape lobe_shape(int p, const FiberMaterial &mat, const FiberFrame &frame,
                     double gamma_t, double gamma_o) {
    LobeShape s;
    s.mean  = -frame.so;
    s.sigma = (p + 1)*mat.beta_m;
    if (p < 3) {
        s.center = wrap_pi(2.0*p*gamma_t - 2.0*gamma_o + p*PI);
        s.rho    = std::exp(-(p + 1)*mat.beta_n);
    } else {
        s.center = 0.0;
        s.rho    = 0.0;   // N3 = 1/(2*pi)
    }
    return s;
}
} // namespace

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
bool FiberMaterial::valid() const {
    if (!std::isfinite(eta) || eta < 1.001 || eta > 3.0) return false;
    if (!all_finite(sigma_a)) return false;
    if (sigma_a.x < 0.0 || sigma_a.y < 0.0 || sigma_a.z < 0.0) return false;
    if (!std::isfinite(beta_m) || beta_m < 0.02 || beta_m > 1.0) return false;
    if (!std::isfinite(beta_n) || beta_n < 0.02 || beta_n > 1.0) return false;
    return true;
}

bool Fiber::valid() const {
    if (!all_finite(a) || !all_finite(b)) return false;
    if (!std::isfinite(radius) || radius <= 0.0) return false;
    if ((b - a).norm_sq() <= 0.0) return false;
    return material.valid();
}

// ---------------------------------------------------------------------------
// Local frame and geometry
// ---------------------------------------------------------------------------
FiberFrame fiber_frame(const dvec3 &wo, const dvec3 &T) {
    FiberFrame f;
    f.T  = T;
    f.so = clamp_unit(wo*T);
    f.co = std::sqrt(std::max(0.0, 1.0 - f.so*f.so));
    if (f.co > 1e-12) {
        f.X = (wo - T*f.so)*(1.0/f.co);
    } else {
        // Parallel ray: no side hit can exist, but keep a deterministic frame.
        dvec3 ref = std::fabs(T.x) < 0.9 ? dvec3(1, 0, 0) : dvec3(0, 1, 0);
        f.X = cross(ref, T).normalized();
    }
    f.Y = cross(T, f.X).normalized();
    return f;
}

bool intersect_fiber(const Fiber &fiber, const dvec3 &orig, const dvec3 &dir,
                     double tmin, double tmax, FiberHit *hit) {
    if (!fiber.valid() || !all_finite(orig) || !all_finite(dir)) return false;
    const double len = (fiber.b - fiber.a).norm();
    const dvec3 T = (fiber.b - fiber.a)*(1.0/len);
    const dvec3 ox = orig - fiber.a;

    // Remove the axial component: the side surface is a circle of radius r in
    // the plane perpendicular to T.
    const dvec3 q = ox - T*(ox*T);
    const dvec3 v = dir - T*(dir*T);
    const double aq = v*v;
    if (aq <= 1e-18) return false;                 // parallel to the axis: no side hit
    const double bq = 2.0*(q*v);
    const double cq = q*q - fiber.radius*fiber.radius;
    double disc = bq*bq - 4.0*aq*cq;
    const double disc_tol = 1e-12*std::max({1.0, bq*bq, std::fabs(4.0*aq*cq)});
    if (disc < 0.0) {
        if (disc > -disc_tol) disc = 0.0;          // tangent counts as a hit
        else return false;
    }
    const double sq = std::sqrt(disc);
    const double t0 = (-bq - sq)/(2.0*aq);
    const double t1 = (-bq + sq)/(2.0*aq);
    double t = t0;
    if (!(t >= tmin && t < tmax)) {
        t = t1;
        if (!(t >= tmin && t < tmax)) return false;
    }

    const dvec3 p = orig + dir*t;
    const double u = ((p - fiber.a)*T)/len;        // axial parameter, 0 at A, 1 at B
    const double u_tol = 1e-9;
    if (u < -u_tol || u > 1.0 + u_tol) return false;
    const double uc = std::max(0.0, std::min(1.0, u));

    dvec3 radial = p - (fiber.a + T*(uc*len));
    dvec3 N;
    if (radial.norm_sq() > 1e-24) {
        N = radial*(1.0/fiber.radius);
        if (N.norm_sq() < 1e-24) N = v.normalized();
        else N = N.normalized();
    } else {
        N = v.normalized();                        // exact tangent fallback
    }

    const dvec3 wo = dir*(-1.0);
    FiberFrame frame = fiber_frame(wo, T);
    double h = clamp_unit(N*frame.Y);

    if (hit) {
        hit->id     = fiber.id;
        hit->t      = t;
        hit->point  = p;
        hit->normal = N;
        hit->h      = h;
        hit->frame  = frame;
    }
    return true;
}

FiberHit intersect_fibers(const Fiber *fibers, std::size_t count,
                          const dvec3 &orig, const dvec3 &dir,
                          double tmin, double tmax, int skip_id) {
    FiberHit best;
    for (std::size_t i = 0; i < count; ++i) {
        if (fibers[i].id == skip_id) continue;
        FiberHit cur;
        if (!intersect_fiber(fibers[i], orig, dir, tmin, tmax, &cur)) continue;
        if (best.id < 0 || cur.t < best.t || (cur.t == best.t && cur.id < best.id))
            best = cur;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Sample stream
// ---------------------------------------------------------------------------
void SampleStream::reset(std::uint64_t seed) {
    state_ = seed + 0x9e3779b97f4a7c15ULL;
    used_  = 0;
}

double SampleStream::draw() {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30))*0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27))*0x94d049bb133111ebULL;
    z =  z ^ (z >> 31);
    ++used_;
    return double(z >> 11)*0x1.0p-53;              // [0, 1)
}

// ---------------------------------------------------------------------------
// Scalar distributions
// ---------------------------------------------------------------------------
double fresnel_unpolarized(double cosi, double eta) {
    cosi = clamp_unit(cosi);
    if (cosi <= 0.0) return 1.0;                   // grazing limit
    const double sint2 = (1.0 - cosi*cosi)/(eta*eta);
    const double cost = std::sqrt(std::max(0.0, 1.0 - sint2));
    const double rs = (cosi - eta*cost)/(cosi + eta*cost);
    const double rp = (eta*cosi - cost)/(eta*cosi + cost);
    return 0.5*(rs*rs + rp*rp);
}

double truncated_normal_pdf(double x, double mean, double sigma) {
    if (x < -1.0 || x > 1.0 || !(sigma > 0.0)) return 0.0;
    const double z     = (x - mean)/sigma;
    const double alpha = (-1.0 - mean)/sigma;
    const double beta  = ( 1.0 - mean)/sigma;
    const double Z     = standard_normal_cdf(beta) - standard_normal_cdf(alpha);
    return standard_normal_pdf(z)/(sigma*Z);
}

double inverse_truncated_normal(double u, double mean, double sigma) {
    u = std::max(0.0, std::min(1.0, u));
    const double alpha = (-1.0 - mean)/sigma;
    const double beta  = ( 1.0 - mean)/sigma;
    const double fa    = standard_normal_cdf(alpha);
    const double p     = fa + u*(standard_normal_cdf(beta) - fa);
    const double z     = inverse_standard_normal(std::min(1.0 - 1e-15, std::max(1e-15, p)));
    return std::max(-1.0, std::min(1.0, mean + sigma*z));
}

double azimuthal_density(double phi, double center, double rho) {
    const double d = wrap_pi(phi - center);
    return (1.0 - rho*rho) /
           (TWO_PI*(1.0 + rho*rho - 2.0*rho*std::cos(d)));
}

// Inverse CDF of the Poisson-kernel azimuthal density.  Closed endpoints:
// w == 0 -> center - pi, w == 1 -> center + pi.
static double inverse_azimuth(double w, double center, double rho) {
    if (w <= 0.0) return wrap_pi(center - PI);
    if (w >= 1.0) return wrap_pi(center + PI);
    const double x = 2.0*std::atan((1.0 - rho)/(1.0 + rho)*std::tan(PI*(w - 0.5)));
    return wrap_pi(center + x);
}

// ---------------------------------------------------------------------------
// Four-lobe attenuation
// ---------------------------------------------------------------------------
FiberLobes fiber_attenuation(const FiberMaterial &mat, double h, double r,
                             const FiberFrame &frame) {
    FiberLobes out{};
    if (!(frame.co > 0.0) || !(r > 0.0)) return out;   // parallel rays never reach here
    const double so      = frame.so;
    const double co      = frame.co;
    const double gamma_o = std::asin(clamp_unit(h));
    const double eta_p   = std::sqrt(std::max(0.0, mat.eta*mat.eta - so*so))/co;
    const double gamma_t = std::asin(clamp_unit(h/eta_p));
    const double ct      = std::sqrt(std::max(0.0, 1.0 - so*so/(mat.eta*mat.eta)));
    const double chord   = 2.0*r*std::cos(gamma_t)/ct;

    const double F = fresnel_unpolarized(co*std::cos(gamma_o), mat.eta);
    out.h = h;
    out.F = F;
    out.gamma_o = gamma_o;
    out.gamma_t = gamma_t;
    out.chord   = chord;
    out.eta_p   = eta_p;
    for (int k = 0; k < 3; ++k) {
        const double sa = mat.sigma_a[k];
        const double C  = std::exp(-sa*chord);
        out.A[0][k] = F;
        out.A[1][k] = (1.0-F)*(1.0-F)*C;
        out.A[2][k] = out.A[1][k]*C*F;
        const double q = C*F;
        // Grazing limit: numerator and denominator vanish together -> A3 = 0.
        out.A[3][k] = (q < 1.0 - EPS) ? out.A[2][k]*q/(1.0 - q) : 0.0;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Kernel, mixture pdf and sampling
// ---------------------------------------------------------------------------
// Lobe selection probabilities: RGB arithmetic mean of each lobe's
// attenuation, normalized by the sum over all four lobes.  Also returns the
// unnormalized four-lobe energy sum_p A_p.
static dvec3 lobe_probabilities(const FiberLobes &lobes, double probs[4]) {
    double total = 0.0;
    for (int p = 0; p < 4; ++p) {
        probs[p] = (lobes.A[p].x + lobes.A[p].y + lobes.A[p].z)/3.0;
        total += probs[p];
    }
    dvec3 energy{0, 0, 0};
    for (int p = 0; p < 4; ++p) {
        if (total > 0.0) probs[p] /= total;
        else probs[p] = (p == 0) ? 1.0 : 0.0;
        energy = energy + lobes.A[p];
    }
    return energy;
}

// Reconstruct wi from its (s, phi) coordinates in the T/X/Y frame.
static dvec3 direction_from_s_phi(const FiberFrame &frame, double s, double phi) {
    const double rxy = std::sqrt(std::max(0.0, 1.0 - s*s));
    dvec3 wi = frame.T*s + frame.X*(rxy*std::cos(phi)) + frame.Y*(rxy*std::sin(phi));
    const double nrm = wi.norm();
    return nrm > 0.0 ? wi*(1.0/nrm) : frame.T*(s >= 0.0 ? 1.0 : -1.0);
}

dvec3 fiber_kernel(const FiberMaterial &mat, const FiberLobes &lobes,
                   const FiberFrame &frame, const dvec3 &wi) {
    dvec3 K{0, 0, 0};
    if (!std::isfinite(wi.x) || !std::isfinite(wi.y) || !std::isfinite(wi.z)) return K;
    const double s   = clamp_unit(wi*frame.T);
    const double phi = std::atan2(wi*frame.Y, wi*frame.X);
    for (int p = 0; p < 4; ++p) {
        const LobeShape sh = lobe_shape(p, mat, frame, lobes.gamma_t, lobes.gamma_o);
        const double Mp = truncated_normal_pdf(s, sh.mean, sh.sigma);
        const double Np = azimuthal_density(phi, sh.center, sh.rho);
        K = K + lobes.A[p]*(Mp*Np);
    }
    return K;
}

double fiber_pdf(const FiberMaterial &mat, const FiberLobes &lobes,
                 const FiberFrame &frame, const dvec3 &wi) {
    double probs[4];
    lobe_probabilities(lobes, probs);
    const double s   = clamp_unit(wi*frame.T);
    const double phi = std::atan2(wi*frame.Y, wi*frame.X);
    double pdf = 0.0;
    for (int p = 0; p < 4; ++p) {
        const LobeShape sh = lobe_shape(p, mat, frame, lobes.gamma_t, lobes.gamma_o);
        pdf += probs[p]*truncated_normal_pdf(s, sh.mean, sh.sigma)
                      *azimuthal_density(phi, sh.center, sh.rho);
    }
    return pdf;
}

FiberSample fiber_sample(const FiberMaterial &mat, const FiberLobes &lobes,
                         const FiberFrame &frame, double u, double v, double w) {
    FiberSample out{};
    u = clamp_unit(u);
    v = clamp_unit(v);
    w = clamp_unit(w);

    double probs[4];
    lobe_probabilities(lobes, probs);
    int p = 3;
    double cum = 0.0;
    for (int k = 0; k < 4; ++k) {
        cum += probs[k];
        if (u < cum || k == 3) { p = k; break; }
    }
    const LobeShape sh = lobe_shape(p, mat, frame, lobes.gamma_t, lobes.gamma_o);

    const double s   = inverse_truncated_normal(v, sh.mean, sh.sigma);
    const double phi = (p == 3)
                       ? wrap_pi(TWO_PI*w - PI)
                       : inverse_azimuth(w, sh.center, sh.rho);
    out.wi     = direction_from_s_phi(frame, s, phi);
    out.lobe   = p;
    out.kernel = fiber_kernel(mat, lobes, frame, out.wi);
    out.pdf    = fiber_pdf(mat, lobes, frame, out.wi);
    return out;
}
