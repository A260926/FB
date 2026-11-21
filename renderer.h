#ifndef RENDERER_H
#define RENDERER_H

#include "volume_sampling.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace volr {

constexpr double kPi = 3.14159265358979323846;

using volsamp::Box;
using volsamp::RGB;
using volsamp::RNG;
using volsamp::SampleMode;
using volsamp::SamplerContext;
using volsamp::SampleResult;
using volsamp::SegmentTable;
using volsamp::Vec3;
using volsamp::VolumeParams;

struct Material {
    double refractive_index = 1.0;
    double albedo[4] = {2.0, 0.0, 0.0, 0.0};
    RGB    diffuse_color{0.0, 0.0, 0.0};
    double specular_exponent = 0.0;
};

struct Sphere {
    Vec3 center;
    double radius = 1.0;
    Material material;
};

struct PointLight {
    Vec3 pos;
    RGB  intensity{1.0, 1.0, 1.0};
};

struct VolumeBox {
    Vec3 mn;
    Vec3 mx;
    RGB  sigma_a{0.0, 0.0, 0.0};
    RGB  sigma_s{0.0, 0.0, 0.0};
};

struct Scene {
    std::vector<Sphere>     spheres;
    bool                    checkerboard = true;
    RGB                     background{0.2, 0.7, 0.8};
    std::vector<PointLight> lights;
    std::vector<VolumeBox>  media;
};

struct RenderOptions {
    SampleMode mode = SampleMode::Mixed;
    int        samples_per_segment = 1; // positive integer
    uint64_t   seed = 0x9e3779b97f4a7c15ull;
    int        max_depth = 4;
};

// Observable random-sample budget of one render.
struct VolBudget {
    long long volume_samples = 0;
    long long volume_shadow_queries = 0;
    void reset() { volume_samples = 0; volume_shadow_queries = 0; }
};

// ---------- validation (rejected at construction time) ----------

inline bool finite_rgb(const RGB& c) {
    return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z);
}

inline void validate_scene(const Scene& s) {
    for (const VolumeBox& b : s.media) {
        if (!(b.mx.x > b.mn.x && b.mx.y > b.mn.y && b.mx.z > b.mn.z))
            throw std::invalid_argument("volume box must have strictly positive extent");
        for (const RGB* c : {&b.sigma_a, &b.sigma_s}) {
            if (!finite_rgb(*c) || c->x < 0.0 || c->y < 0.0 || c->z < 0.0)
                throw std::invalid_argument("volume coefficients must be finite and non-negative");
        }
        if (!std::isfinite(b.mn.x) || !std::isfinite(b.mn.y) || !std::isfinite(b.mn.z) ||
            !std::isfinite(b.mx.x) || !std::isfinite(b.mx.y) || !std::isfinite(b.mx.z))
            throw std::invalid_argument("volume box bounds must be finite");
    }
    for (const PointLight& l : s.lights) {
        if (!finite_rgb(l.intensity) ||
            l.intensity.x < 0.0 || l.intensity.y < 0.0 || l.intensity.z < 0.0)
            throw std::invalid_argument("light intensity must be finite and non-negative");
        if (!std::isfinite(l.pos.x) || !std::isfinite(l.pos.y) || !std::isfinite(l.pos.z))
            throw std::invalid_argument("light position must be finite");
        for (const VolumeBox& b : s.media) {
            if (l.pos.x >= b.mn.x && l.pos.x <= b.mx.x &&
                l.pos.y >= b.mn.y && l.pos.y <= b.mx.y &&
                l.pos.z >= b.mn.z && l.pos.z <= b.mx.z)
                throw std::invalid_argument("point light must lie outside every medium box");
        }
    }
    for (const Sphere& sp : s.spheres) {
        if (!(sp.radius > 0.0) || !std::isfinite(sp.radius))
            throw std::invalid_argument("sphere radius must be positive and finite");
    }
}

inline Scene make_scene(std::vector<Sphere> spheres, bool checkerboard, RGB background,
                        std::vector<PointLight> lights, std::vector<VolumeBox> media) {
    Scene s;
    s.spheres = std::move(spheres);
    s.checkerboard = checkerboard;
    s.background = background;
    s.lights = std::move(lights);
    s.media = std::move(media);
    validate_scene(s);
    return s;
}

// The original four-sphere scene, with no media.
inline Scene default_scene() {
    const Material ivory     {1.0, {0.9,  0.5, 0.1, 0.0}, {0.4, 0.4, 0.3},   50.0};
    const Material glass     {1.5, {0.0,  0.9, 0.1, 0.8}, {0.6, 0.7, 0.8},  125.0};
    const Material red_rubber{1.0, {1.4,  0.3, 0.0, 0.0}, {0.3, 0.1, 0.1},   10.0};
    const Material mirror    {1.0, {0.0, 16.0, 0.8, 0.0}, {1.0, 1.0, 1.0}, 1425.0};
    Scene s;
    s.spheres = {
        {{-3.0,    0.0, -16.0}, 2.0, ivory},
        {{-1.0, -1.5, -12.0}, 2.0, glass},
        {{ 1.5, -0.5, -18.0}, 3.0, red_rubber},
        {{ 7.0,  5.0, -18.0}, 4.0, mirror}
    };
    s.checkerboard = true;
    s.background = {0.2, 0.7, 0.8};
    s.lights = {
        {{-20.0, 20.0,  20.0}, {1.0, 1.0, 1.0}},
        {{ 30.0, 50.0, -25.0}, {1.0, 1.0, 1.0}},
        {{ 30.0, 20.0,  30.0}, {1.0, 1.0, 1.0}}
    };
    return s;
}

inline VolumeParams to_volume_params(const Scene& s) {
    VolumeParams vp;
    vp.boxes.reserve(s.media.size());
    for (const VolumeBox& b : s.media)
        vp.boxes.push_back(Box{b.mn, b.mx, b.sigma_a, b.sigma_s});
    return vp;
}

// ---------- geometry (mirrors the original surface scene) ----------

struct Hit {
    bool hit = false;
    double t = 0.0;
    Vec3 point;
    Vec3 normal;
    Material material;
};

inline Vec3 reflect_dir(const Vec3& I, const Vec3& N) {
    return I - N * (2.0 * (I * N));
}

inline Vec3 refract_dir(const Vec3& I, const Vec3& N, double eta_t, double eta_i = 1.0) {
    double cosi = -std::max(-1.0, std::min(1.0, I * N));
    if (cosi < 0.0) return refract_dir(I, N * -1.0, eta_i, eta_t);
    double eta = eta_i / eta_t;
    double k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    if (k < 0.0) return {1.0, 0.0, 0.0}; // total reflection, quirk kept from the original
    return I * eta + N * (eta * cosi - std::sqrt(k));
}

inline bool ray_sphere_hit(const Vec3& orig, const Vec3& dir, const Sphere& sp, double& t_out) {
    Vec3 L = sp.center - orig;
    double tca = L * dir;
    double d2 = L * L - tca * tca;
    if (d2 > sp.radius * sp.radius) return false;
    double thc = std::sqrt(sp.radius * sp.radius - d2);
    double t0 = tca - thc, t1 = tca + thc;
    if (t0 > 0.001) { t_out = t0; return true; }
    if (t1 > 0.001) { t_out = t1; return true; }
    return false;
}

inline Hit intersect(const Scene& s, const Vec3& orig, const Vec3& dir) {
    Hit out;
    double nearest = 1e10;
    Vec3 pt, normal;
    Material mat;

    if (s.checkerboard && std::abs(dir.y) > 0.001) {
        double d = -(orig.y + 4.0) / dir.y;
        Vec3 p = orig + dir * d;
        if (d > 0.001 && d < nearest && std::abs(p.x) < 10.0 && p.z < -10.0 && p.z > -30.0) {
            nearest = d;
            pt = p;
            normal = {0.0, 1.0, 0.0};
            mat = Material{};
            mat.diffuse_color = (int(0.5 * pt.x + 1000.0) + int(0.5 * pt.z)) & 1
                                    ? RGB{0.3, 0.3, 0.3} : RGB{0.3, 0.2, 0.1};
        }
    }
    for (const Sphere& sp : s.spheres) {
        double d = 0.0;
        if (!ray_sphere_hit(orig, dir, sp, d) || d > nearest) continue;
        nearest = d;
        pt = orig + dir * nearest;
        normal = (pt - sp.center).normalized();
        mat = sp.material;
    }
    if (nearest < 1000.0) {
        out.hit = true;
        out.t = nearest;
        out.point = pt;
        out.normal = normal;
        out.material = mat;
    }
    return out;
}

// Transmittance-only finite shadow query: solid occlusion plus medium
// transmission. No source term is accumulated. Objects farther than the
// target point cannot occlude it.
inline RGB shadow_transmittance(const Scene& s, const Vec3& from,
                                const Vec3& to, bool* occluded = nullptr) {
    Vec3 delta = to - from;
    double r = delta.norm();
    if (!(r > 0.0)) { if (occluded) *occluded = false; return {1.0, 1.0, 1.0}; }
    Vec3 sdir = delta * (1.0 / r);

    Hit h = intersect(s, from, sdir);
    bool blocked = h.hit && h.t < r;
    if (occluded) *occluded = blocked;
    if (blocked) return {0.0, 0.0, 0.0};

    if (s.media.empty()) return {1.0, 1.0, 1.0};
    VolumeParams vp = to_volume_params(s);
    SegmentTable tab = volsamp::build_segments(vp, from, sdir, r);
    return tab.transmittance(r);
}

// ---------- recursive single-scattering renderer ----------

inline std::vector<volsamp::LightInfo> make_light_infos(const Scene& s) {
    std::vector<volsamp::LightInfo> infos;
    infos.reserve(s.lights.size());
    double total_lum = 0.0;
    for (const PointLight& l : s.lights) {
        volsamp::LightInfo info = volsamp::make_light_info(l.pos, l.intensity);
        infos.push_back(info);
        total_lum += info.luminance;
    }
    if (total_lum > 0.0)
        for (auto& info : infos) info.selection_pdf = info.luminance / total_lum;
    return infos;
}

inline RGB render_ray(const Scene& s, const Vec3& orig, const Vec3& dir,
                      const RenderOptions& opt, RNG& rng, VolBudget& budget, int depth);

inline RGB volume_source(const Scene& s, const VolumeParams& vp,
                         const Vec3& orig, const Vec3& dir, double t_end,
                         const std::vector<volsamp::LightInfo>& light_infos,
                         const RenderOptions& opt, RNG& rng, VolBudget& budget) {
    SegmentTable table = volsamp::build_segments(vp, orig, dir, t_end);
    SamplerContext ctx{orig, dir, table, light_infos, opt.mode};
    if (!volsamp::source_is_active(ctx)) return {0.0, 0.0, 0.0};

    RGB accum{0.0, 0.0, 0.0};
    const int n = opt.samples_per_segment;
    for (int sample = 0; sample < n; ++sample) {
        SampleResult sr = volsamp::sample_distance(orig, dir, ctx, rng);
        budget.volume_samples++;

        const volsamp::LightInfo& li = light_infos[sr.light];
        Vec3 p = orig + dir * sr.distance;
        RGB sig_s = table.sigma_s_at(sr.distance);
        RGB T_view = table.transmittance(sr.distance);

        bool blocked = false;
        RGB T_light = shadow_transmittance(s, p, li.pos, &blocked);
        budget.volume_shadow_queries++;
        if (blocked) continue;

        double r2 = (li.pos - p).norm2();
        RGB contrib{0.0, 0.0, 0.0};
        if (r2 > 0.0 && sr.joint_density > 0.0) {
            double inv = 1.0 / (4.0 * kPi * r2 * sr.joint_density);
            for (int c = 0; c < 3; ++c)
                contrib[c] = T_view[c] * sig_s[c] * li.intensity[c] * T_light[c] * inv;
        }
        accum += contrib;
    }
    return accum * (1.0 / n);
}

inline RGB shade_surface(const Scene& s, const Hit& h, const Vec3& dir,
                         const RenderOptions& opt, RNG& rng, VolBudget& budget, int depth) {
    Vec3 refl = reflect_dir(dir, h.normal).normalized();
    Vec3 refr = refract_dir(dir, h.normal, h.material.refractive_index).normalized();
    RGB reflect_color = render_ray(s, h.point, refl, opt, rng, budget, depth + 1);
    RGB refract_color = render_ray(s, h.point, refr, opt, rng, budget, depth + 1);

    double diffuse_intensity = 0.0, specular_intensity = 0.0;
    for (const PointLight& light : s.lights) {
        Vec3 light_delta = light.pos - h.point;
        Vec3 light_dir = light_delta.normalized();
        bool blocked = false;
        RGB T = shadow_transmittance(s, h.point, light.pos, &blocked);
        if (blocked) continue; // original artistic rule: fully shadowed lights drop out
        double ndotl = std::max(0.0, light_dir * h.normal);
        double spec = std::pow(std::max(0.0, -reflect_dir(light_dir * -1.0, h.normal) * dir),
                               h.material.specular_exponent);
        diffuse_intensity += (T.x + T.y + T.z) * (1.0 / 3.0) * ndotl;
        specular_intensity += (T.x + T.y + T.z) * (1.0 / 3.0) * spec;
    }
    const Material& m = h.material;
    return m.diffuse_color * diffuse_intensity * m.albedo[0]
         + RGB{1.0, 1.0, 1.0} * specular_intensity * m.albedo[1]
         + reflect_color * m.albedo[2]
         + refract_color * m.albedo[3];
}

inline RGB render_ray(const Scene& s, const Vec3& orig, const Vec3& dir,
                      const RenderOptions& opt, RNG& rng, VolBudget& budget, int depth) {
    if (opt.samples_per_segment <= 0)
        throw std::invalid_argument("samples_per_segment must be a positive integer");

    Hit h = intersect(s, orig, dir);
    double t_end = 0.0;
    if (h.hit) {
        t_end = h.t;
    } else if (!s.media.empty()) {
        VolumeParams vp0 = to_volume_params(s);
        t_end = volsamp::last_medium_exit(vp0, orig, dir);
    }

    RGB source{0.0, 0.0, 0.0};
    RGB T_end{1.0, 1.0, 1.0};
    if (!s.media.empty() && t_end > 0.0) {
        VolumeParams vp = to_volume_params(s);
        auto light_infos = make_light_infos(s);
        source = volume_source(s, vp, orig, dir, t_end, light_infos, opt, rng, budget);
        SegmentTable to_surface = volsamp::build_segments(vp, orig, dir, t_end);
        T_end = to_surface.transmittance(t_end);
    }

    RGB terminal;
    if (depth > opt.max_depth || !h.hit) {
        terminal = s.background;
    } else {
        terminal = shade_surface(s, h, dir, opt, rng, budget, depth);
    }
    return source + volsamp::mul(T_end, terminal);
}

// Public selectable single-scatter entry point. The direction must be unit.
inline RGB volume_cast_ray(const Scene& s, const Vec3& orig, const Vec3& unit_dir,
                           const RenderOptions& opt, VolBudget* budget = nullptr) {
    validate_scene(s);
    double nrm = unit_dir.norm();
    if (!std::isfinite(nrm) || std::abs(nrm - 1.0) > 1e-9)
        throw std::invalid_argument("volume_cast_ray expects a unit direction");
    VolBudget local;
    VolBudget& b = budget ? *budget : local;
    b.reset();
    RNG rng(opt.seed);
    return render_ray(s, orig, unit_dir, opt, rng, b, 0);
}

} // namespace volr

#endif
