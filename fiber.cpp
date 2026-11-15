#include "fiber.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TWO_PI = 6.28318530717958647692;

bool finite3(const dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double clamp_unit(double x) { return std::max(-1.0, std::min(1.0, x)); }

double wrap_pi(double a) {
    a = std::fmod(a + PI, TWO_PI);
    if (a < 0.0) a += TWO_PI;
    return a - PI;
}

// ---- Gaussian helpers -----------------------------------------------------

double ncdf(double x) {
    return 0.5 * std::erfc(-x * 0.7071067811865475244);
}

// Inverse standard normal CDF (Acklam rational approximation plus one
// Newton refinement, accurate to roughly machine precision in double).
double norm_inv(double p) {
    p = std::max(1e-300, std::min(1.0 - 1e-300, p));
    static const double a[] = {
        -3.969683028665376e+01,  2.209460984245205e+02,
        -2.759285104469687e+02,  1.383577518672690e+02,
        -3.066479806614716e+01,  2.506628277459239e+00};
    static const double b[] = {
        -5.447609879822406e+01,  1.615858368580409e+02,
        -1.556989798598866e+02,  6.680131188771972e+01,
        -1.328068155288572e+01};
    static const double c[] = {
        -7.784894002430293e-03, -3.223964580411365e-01,
        -2.400758277161838e+00, -2.549732539343734e+00,
         4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {
         7.784695709041462e-03,  3.224671290700398e-01,
         2.445134137142996e+00,  3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - plow;
    double q, x;
    if (p < plow) {
        q = std::sqrt(-2.0 * std::log(p));
        x = (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
             ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    } else if (p <= phigh) {
        q = p - 0.5;
        double r = q*q;
        x = (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5])*q /
             (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1.0);
    } else {
        q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) /
              ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1.0);
    }
    double e = ncdf(x) - p;
    double u = std::exp(-0.5*x*x) * 0.39894228040143267794;
    if (u > 0.0) x -= e / u;
    return x;
}

// Normal distribution truncated to [-1, 1], mean mu, standard deviation sig.
struct TruncNormal {
    double mu, sig, Fa, Z;
    TruncNormal(double mu_, double sig_)
        : mu(mu_), sig(sig_),
          Fa(ncdf((-1.0 - mu_) / sig_)),
          Z(ncdf((1.0 - mu_) / sig_) - Fa) {}
    double pdf(double x) const {
        if (x < -1.0 || x > 1.0 || Z <= 0.0) return 0.0;
        double z = (x - mu) / sig;
        return std::exp(-0.5*z*z) * 0.39894228040143267794 / (sig * Z);
    }
    // Inverse CDF. No retries: u == 0 deterministically maps to s = -1,
    // u == 1 (outside the [0,1) contract but handled) to s = 1.
    double sample(double u) const {
        if (u <= 0.0) return -1.0;
        if (u >= 1.0) return 1.0;
        double q = Fa + u * Z;
        if (!(q > 0.0)) q = 1e-300;
        if (q >= 1.0) q = 1.0 - 1e-16;
        return clamp_unit(mu + sig * norm_inv(q));
    }
};

// Azimuthal wrapped-Cauchy (Poisson kernel) density with concentration
// rho in [0,1) and angular center c.
double azimuth_pdf(double phi, double center, double rho) {
    if (rho <= 0.0) return 1.0 / TWO_PI;
    double d = wrap_pi(phi - center);
    return (1.0 - rho*rho) /
           (TWO_PI * (1.0 + rho*rho - 2.0*rho*std::cos(d)));
}

// Closed-form inverse CDF of the wrapped Cauchy, so sampling never retries.
// u == 0 deterministically maps to the -pi cut.
double azimuth_sample(double u, double center, double rho) {
    if (u <= 0.0) return wrap_pi(center - PI);
    if (u >= 1.0) u = 1.0 - 1e-16;
    double offset;
    if (rho <= 0.0) {
        offset = TWO_PI * u - PI;
    } else {
        double k = (1.0 - rho) / (1.0 + rho);
        offset = 2.0 * std::atan(k * std::tan(PI * (u - 0.5)));
        offset = clamp_unit(offset / PI) * PI; // guard against round-off
    }
    return wrap_pi(center + offset);
}

TruncNormal make_longitudinal(const FiberMaterial& m, int p, double so) {
    return TruncNormal(-so, (p + 1) * m.beta_m);
}

double lobe_center(int p, double gamma_t, double gamma_o) {
    return 2.0*p*gamma_t - 2.0*gamma_o + p*PI;
}

} // namespace

// ---- validation -----------------------------------------------------------

bool fiber_material_valid(const FiberMaterial& m) {
    if (!std::isfinite(m.eta) || m.eta < 1.001 || m.eta > 3.0) return false;
    if (!finite3(m.sigma_a)) return false;
    if (m.sigma_a.x < 0.0 || m.sigma_a.y < 0.0 || m.sigma_a.z < 0.0)
        return false;
    if (!std::isfinite(m.beta_m) || m.beta_m < 0.02 || m.beta_m > 1.0)
        return false;
    if (!std::isfinite(m.beta_n) || m.beta_n < 0.02 || m.beta_n > 1.0)
        return false;
    return true;
}

bool fiber_valid(const Fiber& f) {
    if (!finite3(f.A) || !finite3(f.B)) return false;
    if (!std::isfinite(f.radius) || f.radius <= 0.0) return false;
    if ((f.B - f.A).norm_sq() <= 0.0) return false;
    return fiber_material_valid(f.material);
}

// ---- geometry -------------------------------------------------------------

FiberFrame make_fiber_frame(const dvec3& T, const dvec3& wo) {
    FiberFrame fr;
    fr.T = T;
    fr.so = clamp_unit(wo * T);
    fr.co = std::sqrt(std::max(0.0, 1.0 - fr.so*fr.so));
    if (fr.co > 1e-15) {
        fr.X = (wo - T * fr.so) * (1.0 / fr.co);
    } else {
        // Deterministic fallback for an axis-parallel wo. A side hit can
        // never produce this, but the frame stays well defined.
        dvec3 ref = std::fabs(T.x) < 0.9 ? dvec3{1,0,0} : dvec3{0,1,0};
        fr.X = (ref - T * (ref * T)).normalized();
    }
    fr.Y = cross(T, fr.X);
    return fr;
}

bool fiber_intersect(const Fiber& fiber, int fiber_id,
                     const dvec3& orig, const dvec3& dir,
                     double tmin, double tmax, FiberHit& hit) {
    if (!fiber_valid(fiber)) return false;
    if (!finite3(orig) || !finite3(dir) || !std::isfinite(tmin) ||
        !std::isfinite(tmax) || tmax <= tmin)
        return false;
    double dnorm = dir.norm();
    if (!std::isfinite(dnorm) || std::fabs(dnorm - 1.0) > 1e-6)
        return false; // contract: unit rays only

    dvec3 axis = fiber.B - fiber.A;
    double length = axis.norm();
    dvec3 T = axis * (1.0 / length);
    dvec3 w = orig - fiber.A;
    double a = dir * T;
    double b = w * T;
    dvec3 D = dir - T * a;
    dvec3 E = w   - T * b;
    double Aq = D * D;
    if (Aq <= 1e-18) return false; // parallel to the axis: no lateral surface
    double Bq = 2.0 * (D * E);
    double Cq = E * E - fiber.radius * fiber.radius;
    double disc = Bq*Bq - 4.0*Aq*Cq;
    // An exact tangency has disc == 0 and still counts as a hit; forgive
    // only round-off sized negative values.
    double scale = std::max(1.0, std::fabs(Bq*Bq));
    if (disc < 0.0) {
        if (disc >= -1e-12 * scale) disc = 0.0;
        else return false;
    }
    double sq = std::sqrt(disc);
    double roots[2] = {(-Bq - sq) / (2.0*Aq), (-Bq + sq) / (2.0*Aq)};

    for (double t : roots) {
        if (!std::isfinite(t) || t < tmin || t >= tmax) continue;
        double u = (b + a*t) / length;
        if (u < 0.0 || u > 1.0) continue; // open cylinder: no end caps
        dvec3 point = orig + dir * t;
        dvec3 axis_point = fiber.A + T * (u * length);
        dvec3 N = (point - axis_point) * (1.0 / fiber.radius);
        // Re-orthogonalize against T before normalizing.
        N = N - T * (N * T);
        double nn = N.norm();
        if (!(nn > 1e-12)) return false;
        N = N * (1.0 / nn);

        FiberFrame fr = make_fiber_frame(T, dir * (-1.0));
        double h = clamp_unit(N * fr.Y);

        hit.fiber_id = fiber_id;
        hit.t = t;
        hit.point = point;
        hit.normal = N;
        hit.h = h;
        hit.u = u;
        return true;
    }
    return false;
}

// ---- material -------------------------------------------------------------

namespace {

// Exact unpolarized Fresnel reflection for air -> dielectric of index eta
// at incident cosine cos_i.
double fresnel_unpolarized(double eta, double cos_i) {
    cos_i = clamp_unit(cos_i);
    double sin2_i = std::max(0.0, 1.0 - cos_i*cos_i);
    double cos_t = std::sqrt(std::max(0.0, 1.0 - sin2_i / (eta*eta)));
    double rs = (cos_i - eta*cos_t) / (cos_i + eta*cos_t);
    double rp = (eta*cos_i - cos_t) / (eta*cos_i + cos_t);
    return 0.5 * (rs*rs + rp*rp);
}

} // namespace

FiberLobes fiber_compute_lobes(const FiberMaterial& m, const FiberFrame& fr,
                               double h, double radius) {
    FiberLobes L;
    h = clamp_unit(h);
    double so = fr.so, co = std::max(fr.co, 1e-15);
    L.gamma_o = std::asin(h);
    L.eta_p = std::sqrt(std::max(0.0, m.eta*m.eta - so*so)) / co;
    double ratio = h / L.eta_p;
    L.gamma_t = std::asin(clamp_unit(ratio));
    L.ct = std::sqrt(std::max(0.0, 1.0 - so*so / (m.eta*m.eta)));
    L.chord = 2.0 * radius * std::cos(L.gamma_t) / std::max(L.ct, 1e-15);

    double cos_i = co * std::cos(L.gamma_o);
    L.F = fresnel_unpolarized(m.eta, cos_i);

    for (int c = 0; c < 3; ++c) {
        double sig = m.sigma_a[c];
        double Cv = std::exp(-sig * L.chord);
        L.C[c] = Cv;
        double F = L.F;
        double a0 = F;
        double a1 = (1.0-F)*(1.0-F) * Cv;
        double a2 = a1 * Cv * F;
        double num = a2 * Cv * F;
        double den = 1.0 - Cv * F;
        double a3 = 0.0;
        // Grazing limit: numerator and denominator vanish together and the
        // merged tail is defined to be zero. Never renormalize the rest.
        if (den > 0.0 && num > 0.0) {
            a3 = num / den;
            if (!std::isfinite(a3)) a3 = 0.0;
        }
        L.A[0][c] = a0;
        L.A[1][c] = a1;
        L.A[2][c] = a2;
        L.A[3][c] = a3;
    }
    return L;
}

void fiber_lobe_probabilities(const FiberLobes& lobes, double probs[4]) {
    double means[4], sum = 0.0;
    for (int p = 0; p < 4; ++p) {
        means[p] = (lobes.A[p].x + lobes.A[p].y + lobes.A[p].z) / 3.0;
        sum += means[p];
    }
    if (!(sum > 0.0)) {
        for (int p = 0; p < 4; ++p) probs[p] = 0.25;
        return;
    }
    for (int p = 0; p < 4; ++p) probs[p] = means[p] / sum;
}

namespace {

// Per-lobe longitudinal and azimuthal densities at (s, phi).
void lobe_densities(const FiberMaterial& m, const FiberFrame& fr,
                    const FiberLobes& L, double s, double phi,
                    double Mp[4], double Np[4]) {
    for (int p = 0; p < 4; ++p) {
        TruncNormal tn = make_longitudinal(m, p, fr.so);
        Mp[p] = tn.pdf(s);
        double center = lobe_center(p, L.gamma_t, L.gamma_o);
        if (p == 3) {
            Np[p] = 1.0 / TWO_PI;
        } else {
            double rho = std::exp(-(p + 1) * m.beta_n);
            Np[p] = azimuth_pdf(phi, center, rho);
        }
    }
}

} // namespace

dvec3 fiber_evaluate_K(const FiberMaterial& m, const FiberFrame& fr,
                       double h, double radius, const dvec3& wi) {
    if (!fiber_material_valid(m) || !finite3(wi)) return {0,0,0};
    FiberLobes L = fiber_compute_lobes(m, fr, h, radius);
    double s = clamp_unit(wi * fr.T);
    double phi = std::atan2(wi * fr.Y, wi * fr.X);
    double Mp[4], Np[4];
    lobe_densities(m, fr, L, s, phi, Mp, Np);
    dvec3 K{0,0,0};
    for (int p = 0; p < 4; ++p) {
        double w = Mp[p] * Np[p];
        K = K + L.A[p] * w;
    }
    return K;
}

double fiber_pdf(const FiberMaterial& m, const FiberFrame& fr,
                 double h, double radius, const dvec3& wi) {
    if (!fiber_material_valid(m) || !finite3(wi)) return 0.0;
    FiberLobes L = fiber_compute_lobes(m, fr, h, radius);
    double probs[4];
    fiber_lobe_probabilities(L, probs);
    double s = clamp_unit(wi * fr.T);
    double phi = std::atan2(wi * fr.Y, wi * fr.X);
    double Mp[4], Np[4];
    lobe_densities(m, fr, L, s, phi, Mp, Np);
    double pdf = 0.0;
    for (int p = 0; p < 4; ++p) pdf += probs[p] * Mp[p] * Np[p];
    return pdf;
}

FiberSample fiber_sample(const FiberMaterial& m, const FiberFrame& fr,
                         double h, double radius,
                         double u1, double u2, double u3) {
    FiberSample out;
    if (!fiber_material_valid(m)) return out;
    if (!std::isfinite(u1) || !std::isfinite(u2) || !std::isfinite(u3))
        return out;
    if (u1 < 0.0 || u1 >= 1.0 || u2 < 0.0 || u2 >= 1.0 ||
        u3 < 0.0 || u3 >= 1.0)
        return out;

    FiberLobes L = fiber_compute_lobes(m, fr, h, radius);
    double probs[4];
    fiber_lobe_probabilities(L, probs);

    int lobe = 3;
    double cum = 0.0;
    for (int p = 0; p < 4; ++p) {
        cum += probs[p];
        if (u3 < cum) { lobe = p; break; }
    }

    TruncNormal tn = make_longitudinal(m, lobe, fr.so);
    double s = tn.sample(u1);
    double center = lobe_center(lobe, L.gamma_t, L.gamma_o);
    double phi;
    if (lobe == 3) {
        phi = wrap_pi(center + TWO_PI * u2 - PI);
    } else {
        double rho = std::exp(-(lobe + 1) * m.beta_n);
        phi = azimuth_sample(u2, center, rho);
    }

    double radial = std::sqrt(std::max(0.0, 1.0 - s*s));
    // At the s poles the azimuth collapses deterministically to +/- T.
    dvec3 wi = fr.T * s +
               (fr.X * std::cos(phi) + fr.Y * std::sin(phi)) * radial;
    double wn = wi.norm();
    if (!(wn > 0.0) || !std::isfinite(wn)) return out;
    wi = wi * (1.0 / wn);

    out.ok = true;
    out.wi = wi;
    out.K = fiber_evaluate_K(m, fr, h, radius, wi);
    out.pdf = fiber_pdf(m, fr, h, radius, wi);
    return out;
}
