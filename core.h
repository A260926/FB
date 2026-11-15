#ifndef TINYRT_CORE_H
#define TINYRT_CORE_H
#include <cmath>

// Shared geometry core, extracted verbatim from the original single-file
// tracer.  vec3 and Sphere keep their original (single precision) layout so the
// legacy sphere scene, its Material and cast_ray() in tinyraytracer.cpp keep
// their exact behaviour; the fiber path performs all shading math in double.
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

vec3 cross(const vec3 v1, const vec3 v2);

// Original sphere material, kept byte-for-byte identical to the upstream
// tracer; it lives in the shared core because Sphere holds it by value.
struct Material {
    float refractive_index = 1;
    float albedo[4] = {2,0,0,0};
    vec3 diffuse_color = {0,0,0};
    float specular_exponent = 0;
};

struct Sphere {
    vec3 center;
    float radius;
    Material material;
};

// Original sphere intersection: returns [found, distance], with the 0.001
// world-unit self-intersection offset preserved.
struct SphereHit { bool hit; float t; };
SphereHit ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s);

// Double precision vectors used by every fiber computation.
struct dvec3 {
    double x=0.0, y=0.0, z=0.0;
    dvec3() = default;
    dvec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit dvec3(const vec3 &v) : x(v.x), y(v.y), z(v.z) {}
    double&       operator[](int i)       { return i==0 ? x : (i==1 ? y : z); }
    const double& operator[](int i) const { return i==0 ? x : (i==1 ? y : z); }
    dvec3 operator+(const dvec3 &v) const { return {x+v.x, y+v.y, z+v.z}; }
    dvec3 operator-(const dvec3 &v) const { return {x-v.x, y-v.y, z-v.z}; }
    dvec3 operator-()              const { return {-x, -y, -z}; }
    dvec3 operator*(double k)      const { return {x*k, y*k, z*k}; }
    double operator*(const dvec3 &v) const { return x*v.x + y*v.y + z*v.z; }
    double norm_sq() const { return x*x+y*y+z*z; }
    double norm() const { return std::sqrt(norm_sq()); }
    dvec3 normalized() const { return (*this)*(1.0/norm()); }
    vec3 to_vec3() const { return {float(x), float(y), float(z)}; }
};

dvec3 cross(const dvec3 &a, const dvec3 &b);

// Component-wise product (RGB throughput multiplication); the * operator on
// dvec3 remains the dot product, matching the convention used for vec3.
dvec3 cmul(const dvec3 &a, const dvec3 &b);

// Robust finite check for whole vectors / colors.
bool all_finite(const dvec3 &v);

#endif // TINYRT_CORE_H
