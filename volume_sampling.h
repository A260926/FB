#ifndef VOLUME_SAMPLING_H
#define VOLUME_SAMPLING_H

#include <cstdint>
#include <vector>

namespace volsamp {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    double&       operator[](int i)       { return i == 0 ? x : (i == 1 ? y : z); }
    const double& operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    Vec3  operator*(double v)        const { return {x * v, y * v, z * v}; }
    double operator*(const Vec3& v)  const { return x * v.x + y * v.y + z * v.z; }
    Vec3  operator+(const Vec3& v)   const { return {x + v.x, y + v.y, z + v.z}; }
    Vec3  operator-(const Vec3& v)   const { return {x - v.x, y - v.y, z - v.z}; }
    Vec3  operator-()                const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& v)        { x += v.x; y += v.y; z += v.z; return *this; }
    double norm() const;
    double norm2() const { return x * x + y * y + z * z; }
    Vec3 normalized() const;
};
using RGB = Vec3;

Vec3 cross(const Vec3& a, const Vec3& b);
double dot(const Vec3& a, const Vec3& b);
RGB  mul(const RGB& a, const RGB& b); // component-wise

// Deterministic, seedable RNG (splitmix64). No global state.
class RNG {
public:
    explicit RNG(uint64_t seed) : state_(seed) {}
    uint64_t next_u64();
    double uniform();                       // [0, 1)
    uint32_t select_index(const double* weights, uint32_t n); // CDF draw
private:
    uint64_t state_;
};

// Axis-aligned participating medium box. Coefficients are per channel.
struct Box {
    Vec3 min_b;
    Vec3 max_b;
    RGB  sigma_a;
    RGB  sigma_s;
};

struct VolumeParams {
    std::vector<Box> boxes;
};

struct Interval {
    double t0 = 0.0;
    double t1 = 0.0;
    RGB sigma_a{0, 0, 0};
    RGB sigma_s{0, 0, 0};
    RGB tau0{0, 0, 0}; // cumulative optical depth (per channel) at t0
    RGB sigma_t() const { return sigma_a + sigma_s; }
};

// Ordered, non-overlapping occupied intervals of a ray inside [0, t_end].
// Gaps are simply absent; occupied length never counts vacuum.
struct SegmentTable {
    std::vector<Interval> segs;
    double t_end = 0.0;
    double occupied_length = 0.0;
    RGB total_tau{0, 0, 0};

    bool empty() const { return segs.empty(); }
    const Interval* interval_at(double t) const;
    RGB sigma_s_at(double t) const; // zero in gaps
    RGB tau_at(double t) const;
    RGB transmittance(double t) const; // exp(-tau(t)), per channel
};

// Merge all box crossings on r(u) = orig + u * unit_dir, restricted to [0, t_end].
// Overlapping boxes have their coefficients summed over world length.
SegmentTable build_segments(const VolumeParams& volume,
                            const Vec3& orig, const Vec3& unit_dir, double t_end);

// Far side of the last box exit (> 0), or 0 when the ray never enters a box.
double last_medium_exit(const VolumeParams& volume,
                        const Vec3& orig, const Vec3& unit_dir);

enum class SampleMode { Optical, Mixed };

struct LightInfo {
    Vec3  pos;
    RGB   intensity;
    double luminance = 0.0; // .2126 R + .7152 G + .0722 B
    double selection_pdf = 0.0;
};

LightInfo make_light_info(const Vec3& pos, const RGB& intensity);

struct SampleResult {
    double distance = 0.0;
    int    light = -1;
    double joint_density = 0.0; // p_l * q_d (optical) or p_l * (q_d+q_e,l)/2 (mixed)
};

// A queryable distribution bound to one ray, one segment table and one light set.
struct SamplerContext {
    Vec3 ray_orig;
    Vec3 ray_dir;
    const SegmentTable&      table;
    const std::vector<LightInfo>& lights;
    SampleMode mode;
};

bool source_is_active(const SamplerContext& ctx); // empty support / black lights / no scattering

// Proposal 1: full three-channel optical mixture (inverse query, all channels).
double optical_density(const SamplerContext& ctx, double t);
// Proposal 2: equiangular density around the given light, on the occupied support.
double equiangular_density(const SamplerContext& ctx, int light, double t);
// Mode dependent joint distance density for a preselected light.
double joint_density(const SamplerContext& ctx, int light, double t);

int select_light(const SamplerContext& ctx, RNG& rng);
double sample_optical(const SegmentTable& table, RNG& rng);
double sample_equiangular(const SegmentTable& table, const Vec3& ray_orig,
                          const Vec3& unit_dir, const Vec3& light_pos, RNG& rng);
// Draws light, strategy, distance and reports the FULL mixture joint density.
SampleResult sample_distance(const Vec3& ray_orig, const Vec3& unit_dir,
                             const SamplerContext& ctx, RNG& rng);

} // namespace volsamp

#endif
