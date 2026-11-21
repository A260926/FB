#include "volume_sampling.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace volsamp {

// ---------- vec helpers ----------

double Vec3::norm() const { return std::sqrt(norm2()); }

Vec3 Vec3::normalized() const {
    double n = norm();
    if (n <= 0.0) return {0.0, 0.0, 0.0};
    return (*this) * (1.0 / n);
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

RGB mul(const RGB& a, const RGB& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// ---------- RNG ----------

uint64_t RNG::next_u64() {
    uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double RNG::uniform() {
    // 53-bit mantissa -> [0, 1)
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

uint32_t RNG::select_index(const double* weights, uint32_t n) {
    double total = 0.0;
    for (uint32_t i = 0; i < n; ++i) total += weights[i];
    double target = uniform() * total;
    double acc = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        acc += weights[i];
        if (target < acc) return i;
    }
    return n - 1;
}

// ---------- segment table ----------

namespace {

struct Event {
    double t;
    RGB da;
    RGB ds;
};

// Slab intersection of a ray with an axis aligned box. Returns the entering
// and exiting parameters restricted to [0, t_end].
bool box_hit(const Box& box, const Vec3& orig, const Vec3& dir,
             double t_end, double& t_in, double& t_out) {
    double lo = 0.0, hi = t_end;
    const Vec3 mn = box.min_b, mx = box.max_b;
    for (int axis = 0; axis < 3; ++axis) {
        double o = orig[axis], d = dir[axis];
        if (std::abs(d) < 1e-15) {
            if (o < mn[axis] || o > mx[axis]) return false;
            continue;
        }
        double t1 = (mn[axis] - o) / d;
        double t2 = (mx[axis] - o) / d;
        if (t1 > t2) std::swap(t1, t2);
        lo = std::max(lo, t1);
        hi = std::min(hi, t2);
        if (lo > hi) return false;
    }
    t_in = lo;
    t_out = hi;
    return t_out > t_in;
}

} // namespace

SegmentTable build_segments(const VolumeParams& volume,
                            const Vec3& orig, const Vec3& unit_dir, double t_end) {
    SegmentTable table;
    table.t_end = t_end;
    if (!(t_end > 0.0)) return table;

    std::vector<Event> events;
    events.reserve(volume.boxes.size() * 2);
    for (const Box& box : volume.boxes) {
        double t_in, t_out;
        if (!box_hit(box, orig, unit_dir, t_end, t_in, t_out)) continue;
        if (t_in < 0.0) t_in = 0.0;
        if (t_out > t_end) t_out = t_end;
        if (!(t_out - t_in > 1e-15 * std::max(1.0, t_end))) continue;
        events.push_back({t_in, box.sigma_a, box.sigma_s});
        events.push_back({t_out, box.sigma_a * -1.0, box.sigma_s * -1.0});
    }
    if (events.empty()) return table;

    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.t < b.t; });

    const double tol = 1e-12 * std::max(1.0, std::abs(events.back().t));
    RGB cur_a{0, 0, 0}, cur_s{0, 0, 0}, tau{0, 0, 0};
    size_t i = 0;
    double seg_start = events.front().t;
    while (i < events.size()) {
        double t = events[i].t;
        if (t - seg_start > tol &&
            (cur_a.x + cur_a.y + cur_a.z + cur_s.x + cur_s.y + cur_s.z) > 0.0) {
            Interval iv;
            iv.t0 = seg_start;
            iv.t1 = t;
            iv.sigma_a = cur_a;
            iv.sigma_s = cur_s;
            iv.tau0 = tau;
            table.segs.push_back(iv);
            RGB st = iv.sigma_t();
            double len = t - seg_start;
            tau += st * len;
            table.occupied_length += len;
        }
        // coalesce every event at (numerically) the same position
        while (i < events.size() && events[i].t <= t + tol) {
            cur_a += events[i].da;
            cur_s += events[i].ds;
            ++i;
        }
        // cancel subtraction round-off so a real segment is never dropped
        for (int c = 0; c < 3; ++c) {
            if (cur_a[c] < 0.0 && cur_a[c] > -1e-14) cur_a[c] = 0.0;
            if (cur_s[c] < 0.0 && cur_s[c] > -1e-14) cur_s[c] = 0.0;
        }
        seg_start = t;
    }
    table.total_tau = tau;
    return table;
}

double last_medium_exit(const VolumeParams& volume,
                        const Vec3& orig, const Vec3& unit_dir) {
    double far = 0.0;
    for (const Box& box : volume.boxes) {
        double t_in, t_out;
        if (!box_hit(box, orig, unit_dir, 1e300, t_in, t_out)) continue;
        if (t_out < 0.0) continue;
        far = std::max(far, t_out);
    }
    return far;
}

const Interval* SegmentTable::interval_at(double t) const {
    for (const Interval& s : segs) {
        if (t < s.t0) return nullptr;
        if (t <= s.t1) return &s;
    }
    return nullptr;
}

RGB SegmentTable::sigma_s_at(double t) const {
    const Interval* s = interval_at(t);
    return s ? s->sigma_s : RGB{0, 0, 0};
}

RGB SegmentTable::tau_at(double t) const {
    if (segs.empty() || t <= segs.front().t0) return {0, 0, 0};
    for (size_t i = 0; i < segs.size(); ++i) {
        const Interval& s = segs[i];
        if (t <= s.t1) {
            if (t < s.t0) return s.tau0; // gap: optical depth stays constant
            double dt = t - s.t0;
            return s.tau0 + s.sigma_t() * dt;
        }
    }
    return total_tau;
}

RGB SegmentTable::transmittance(double t) const {
    RGB tau = tau_at(t);
    return {std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z)};
}

// ---------- lights ----------

LightInfo make_light_info(const Vec3& pos, const RGB& intensity) {
    LightInfo info;
    info.pos = pos;
    info.intensity = intensity;
    info.luminance = 0.2126 * intensity.x + 0.7152 * intensity.y + 0.0722 * intensity.z;
    return info;
}

bool source_is_active(const SamplerContext& ctx) {
    if (ctx.table.empty()) return false;
    double total_lum = 0.0;
    for (const LightInfo& l : ctx.lights) total_lum += l.luminance;
    return total_lum > 0.0 && std::isfinite(total_lum);
}

int select_light(const SamplerContext& ctx, RNG& rng) {
    std::vector<double> w;
    w.reserve(ctx.lights.size());
    for (const LightInfo& l : ctx.lights)
        w.push_back(l.luminance);
    uint32_t idx = rng.select_index(w.data(), static_cast<uint32_t>(w.size()));
    return static_cast<int>(idx);
}

// ---------- proposal 1: per-channel optical distance ----------

double sample_optical(const SegmentTable& table, RNG& rng) {
    const uint32_t c = rng.next_u64() % 3;
    const double tau_total = table.total_tau[c];
    const double u = rng.uniform();

    if (tau_total > 0.0) {
        // CDF: (1 - exp(-tau_c(t))) / (1 - exp(-tau_total)); expm1 keeps
        // this accurate for both tiny and huge total optical depth.
        double denom = -std::expm1(-tau_total);
        double target = -std::log1p(-u * denom);
        // equivalent to tau coordinate of the draw, stable for thick media
        for (const Interval& s : table.segs) {
            double st = s.sigma_t()[c];
            if (!(st > 0.0)) continue;
            double seg_tau = st * (s.t1 - s.t0);
            if (target > seg_tau) {
                target -= seg_tau;
                continue;
            }
            double t = s.t0 + target / st;
            return std::min(t, s.t1);
        }
        return table.segs.back().t1;
    }

    // Fully transparent channel: uniform over the occupied world length.
    double remain = u * table.occupied_length;
    for (const Interval& s : table.segs) {
        double len = s.t1 - s.t0;
        if (remain > len) {
            remain -= len;
            continue;
        }
        return s.t0 + remain;
    }
    return table.segs.back().t1;
}

double optical_density(const SamplerContext& ctx, double t) {
    const SegmentTable& table = ctx.table;
    const Interval* s = table.interval_at(t);
    double q = 0.0;
    for (uint32_t c = 0; c < 3; ++c) {
        const double tau_total = table.total_tau[c];
        if (tau_total > 0.0) {
            if (!s) continue;
            double st = s->sigma_t()[c];
            double tau = table.tau_at(t)[c];
            double denom = -std::expm1(-tau_total);
            q += st * std::exp(-tau) / denom;
        } else {
            if (s) q += 1.0 / table.occupied_length;
        }
    }
    return q / 3.0;
}

// ---------- proposal 2: equiangular around one light ----------

namespace {

struct EquiGeom {
    double t_light;
    double D;
    bool collinear;
};

EquiGeom equi_geometry(const Vec3& ray_orig, const Vec3& unit_dir,
                       const Vec3& light_pos, double scale) {
    Vec3 L = light_pos - ray_orig;
    EquiGeom g;
    g.t_light = dot(L, unit_dir);
    Vec3 proj = unit_dir * g.t_light;
    g.D = (L - proj).norm();
    g.collinear = g.D <= 1e-12 * std::max(1.0, scale);
    return g;
}

} // namespace

double sample_equiangular(const SegmentTable& table, const Vec3& ray_orig,
                          const Vec3& unit_dir, const Vec3& light_pos, RNG& rng) {
    double scale = std::max(1.0, table.t_end);
    EquiGeom g = equi_geometry(ray_orig, unit_dir, light_pos, scale);
    const double u = rng.uniform();

    if (g.collinear) {
        double remain = u * table.occupied_length;
        for (const Interval& s : table.segs) {
            double len = s.t1 - s.t0;
            if (remain > len) { remain -= len; continue; }
            return s.t0 + remain;
        }
        return table.segs.back().t1;
    }

    std::vector<double> angle_len;
    angle_len.reserve(table.segs.size());
    for (const Interval& s : table.segs) {
        double a0 = std::atan((s.t0 - g.t_light) / g.D);
        double a1 = std::atan((s.t1 - g.t_light) / g.D);
        double dl = a1 - a0;
        angle_len.push_back(dl);
    }
    double total_angle = 0.0;
    for (double dl : angle_len) total_angle += dl;
    double target = u * total_angle;
    for (size_t i = 0; i < table.segs.size(); ++i) {
        if (target > angle_len[i]) { target -= angle_len[i]; continue; }
        const Interval& s = table.segs[i];
        double a0 = std::atan((s.t0 - g.t_light) / g.D);
        double theta = a0 + target;
        double t = g.t_light + g.D * std::tan(theta);
        if (t < s.t0) t = s.t0;
        if (t > s.t1) t = s.t1;
        return t;
    }
    return table.segs.back().t1;
}

double equiangular_density(const SamplerContext& ctx, int light, double t) {
    const SegmentTable& table = ctx.table;
    if (!table.interval_at(t)) return 0.0;
    const LightInfo& L = ctx.lights[light];
    // The ray origin/direction are not stored in the context; recover the
    // geometry from the call site through the table-free overload below.
    // This overload is only used through joint_density(), which always passes
    // a context carrying the ray frame.
    const Vec3& orig = ctx.ray_orig;
    const Vec3& dir = ctx.ray_dir;
    double scale = std::max(1.0, table.t_end);
    EquiGeom g = equi_geometry(orig, dir, L.pos, scale);
    if (g.collinear) return 1.0 / table.occupied_length;

    double total_angle = 0.0;
    for (const Interval& s : table.segs) {
        double a0 = std::atan((s.t0 - g.t_light) / g.D);
        double a1 = std::atan((s.t1 - g.t_light) / g.D);
        total_angle += a1 - a0;
    }
    double dt = t - g.t_light;
    return g.D / (total_angle * (g.D * g.D + dt * dt));
}

double joint_density(const SamplerContext& ctx, int light, double t) {
    const LightInfo& L = ctx.lights[light];
    double qd = optical_density(ctx, t);
    if (ctx.mode == SampleMode::Optical) {
        return L.selection_pdf * qd;
    }
    double qe = equiangular_density(ctx, light, t);
    return L.selection_pdf * 0.5 * (qd + qe);
}

SampleResult sample_distance(const Vec3& ray_orig, const Vec3& unit_dir,
                             const SamplerContext& ctx, RNG& rng) {
    SampleResult out;
    out.light = select_light(ctx, rng);
    double t;
    if (ctx.mode == SampleMode::Optical) {
        t = sample_optical(ctx.table, rng);
    } else if (rng.uniform() < 0.5) {
        t = sample_optical(ctx.table, rng);
    } else {
        t = sample_equiangular(ctx.table, ray_orig, unit_dir,
                               ctx.lights[out.light].pos, rng);
    }
    out.distance = t;
    out.joint_density = joint_density(ctx, out.light, t);
    return out;
}

} // namespace volsamp
