#ifndef TINYRT_CORE_H
#define TINYRT_CORE_H
// Shared geometric core for the legacy sphere tracer and the fiber preview.
// Everything in this header is inline so it can be included from several
// translation units without violating the one-definition rule.
#include <cmath>
#include <tuple>

// Original single-precision vector, kept byte-for-byte compatible with the
// upstream tinyraytracer code.
struct vec3 {
    float x=0, y=0, z=0;
          float& operator[](const int i)       { return i==0 ? x : (1==i ? y : z); }
    const float& operator[](const int i) const { return i==0 ? x : (1==i ? y : z); }
    vec3  operator*(const float v) const { return {x*v, y*v, z*v};       }
    float operator*(const vec3& v) const { return x*v.x + y*v.y + z*v.z; }
    vec3  operator+(const vec3& v) const { return {x+v.x, y+v.y, z+v.z}; }
    vec3  operator-(const vec3& v) const { return {x-v.x, y-v.y, z-v.z}; }
    vec3  operator-()              const { return {-x, -y, -z};          }
    float norm() const { return std::sqrt(x*x+y*y+z*z); }
    vec3 normalized() const { return (*this)*(1.f/norm()); }
};

inline vec3 cross(const vec3 v1, const vec3 v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

// Double-precision vector used by all fiber geometry and material math.
struct dvec3 {
    double x=0, y=0, z=0;
          double& operator[](const int i)       { return i==0 ? x : (1==i ? y : z); }
    const double& operator[](const int i) const { return i==0 ? x : (1==i ? y : z); }
    dvec3  operator*(const double v) const { return {x*v, y*v, z*v};       }
    double operator*(const dvec3& v) const { return x*v.x + y*v.y + z*v.z; }
    dvec3  operator+(const dvec3& v) const { return {x+v.x, y+v.y, z+v.z}; }
    dvec3  operator-(const dvec3& v) const { return {x-v.x, y-v.y, z-v.z}; }
    dvec3  operator-()               const { return {-x, -y, -z};          }
    double norm() const { return std::sqrt(x*x+y*y+z*z); }
    double norm_sq() const { return x*x+y*y+z*z; }
    dvec3 normalized() const { return (*this)*(1.0/norm()); }
};

// Componentwise (per-channel) product; operator* stays the dot product,
// matching the original vec3 convention.
inline dvec3 cmul(const dvec3& a, const dvec3& b) {
    return {a.x*b.x, a.y*b.y, a.z*b.z};
}

inline dvec3 cross(const dvec3& v1, const dvec3& v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

inline dvec3 to_dvec3(const vec3& v) { return {v.x, v.y, v.z}; }
inline vec3  to_vec3(const dvec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

// Original material description; retained unchanged for the sphere scene.
struct Material {
    float refractive_index = 1;
    float albedo[4] = {2,0,0,0};
    vec3 diffuse_color = {0,0,0};
    float specular_exponent = 0;
};

// Original sphere. The fiber renderer treats spheres as black occluders.
struct Sphere {
    vec3 center;
    float radius;
    Material material;
};

// Original sphere/ray intersection (single precision). Returns the nearest
// root strictly farther than 0.001 world units, matching the upstream code.
inline std::tuple<bool,float> ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s) {
    vec3 L = s.center - orig;
    float tca = L*dir;
    float d2 = L*L - tca*tca;
    if (d2 > s.radius*s.radius) return {false, 0};
    float thc = std::sqrt(s.radius*s.radius - d2);
    float t0 = tca-thc, t1 = tca+thc;
    if (t0>.001) return {true, t0};
    if (t1>.001) return {true, t1};
    return {false, 0};
}

// Double-precision sphere intersection for the fiber transport. The interval
// convention is (tmin, tmax): the same strict 0.001 minimum distance the
// original tracer uses.
inline bool ray_sphere_intersect_d(const dvec3& orig, const dvec3& dir,
                                   const dvec3& center, double radius,
                                   double tmin, double tmax, double& hit_t) {
    dvec3 L = center - orig;
    double tca = L*dir;
    double d2 = L*L - tca*tca;
    double r2 = radius*radius;
    if (d2 > r2) return false;
    double thc = std::sqrt(r2 - d2);
    double t0 = tca-thc, t1 = tca+thc;
    if (t0 > tmin && t0 < tmax) { hit_t = t0; return true; }
    if (t1 > tmin && t1 < tmax) { hit_t = t1; return true; }
    return false;
}

#endif // TINYRT_CORE_H
