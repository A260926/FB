#include "core.h"

vec3 cross(const vec3 v1, const vec3 v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

SphereHit ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s) {
    vec3 L = s.center - orig;
    float tca = L*dir;
    float d2 = L*L - tca*tca;
    if (d2 > s.radius*s.radius) return {false, 0};
    float thc = std::sqrt(s.radius*s.radius - d2);
    float t0 = tca-thc, t1 = tca+thc;
    if (t0>.001) return {true, t0};  // offset the original point by .001 to avoid occlusion by the object itself
    if (t1>.001) return {true, t1};
    return {false, 0};
}

dvec3 cross(const dvec3 &a, const dvec3 &b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}

dvec3 cmul(const dvec3 &a, const dvec3 &b) {
    return {a.x*b.x, a.y*b.y, a.z*b.z};
}

bool all_finite(const dvec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
