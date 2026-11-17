#ifndef UTILITIES_H
#define UTILITIES_H

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <random>


// C++ Std Usings

using std::make_shared;
using std::shared_ptr;

// Constants
inline thread_local std::mt19937 generator(std::random_device{}());
const double infinity = std::numeric_limits<double>::infinity();
const double pi = 3.1415926535897932385;
const double tau = 6.283185307179586;


// Utility Functions
inline double degrees_to_radians(double degrees) {
    return degrees * pi / 180.0;
}

inline double random_double() {
    // Returns a random real in [0, 1)
    std::uniform_real_distribution<double> distribution(0.0, 1.0);
    return distribution(generator);
}

inline int random_int(int min, int max) {
    // Returns a random integer in [min, max]
    std::uniform_int_distribution<int> distribution(min, max);
    return distribution(generator);
}

inline double random_double(double min, double max) {
    // Returns a random real in [min, max)
    std::uniform_real_distribution<double> distribution(min, max);
    return distribution(generator);
}




inline double saturate(double v) {
    // Clamp a value to the [0, 1] range
    return std::fmax(0.0, std::fmin(v, 1.0));
}

template<typename T> T Lerp(T a, T b, double t) {
    // Linear interpolation between two values
    return a * (1 - t) + b * t;
}


// Enums
enum class RenderMode {
    BINARY,
    PHONG,
    PATH
};

enum class UseBVH {
    ENABLE,
    DISABLE
};

enum class ToneMapper {
    LINEAR,
    REINHARD,
    FILMIC,
    LUMINANCE
};

enum class AntiAliasing {
    RANDOM,
    JITTERED,
    POISSON
};

// Common Headers

#include "util/Radiance.h"
#include "util/Ray.h"
#include "util/Vector3.h"
#include "util/Interval.h"


inline Vector3 reflect_pm(const Vector3& v, const Vector3& n) {
    // Reflects a vector v around a normal n
    return unit_vector(v - 2 * dot(v, n) * n);
}

inline Vector3 refract(const Vector3& uv, const Vector3& n, double eta) {
    // Using snell's law to refract a ray
    double cos_theta = std::fmin(dot(-uv, n), 1.0);
    Vector3 r_out_perp = eta * (uv + cos_theta * n);
    Vector3 r_out_parallel = -std::sqrt(std::abs(1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_parallel;
}

// INDIRECT RAYS
inline Vector3 random_in_unit_sphere() {
    // Generates a random point inside a unit sphere
    while (true) {
        auto p = Vector3(random_double(-1, 1), random_double(-1, 1), random_double(-1, 1));
        if (p.length_squared() < 1) return p;
    }
}

inline Vector3 random_in_hemisphere(const Vector3& normal) {
    // Generates a random direction within a hemisphere around a given normal
    Vector3 in_unit_sphere = random_in_unit_sphere();
    // If the point is in the same hemisphere as the normal, keep it
    if (dot(in_unit_sphere, normal) > 0.0) {
        return in_unit_sphere;
    } else {
        return -in_unit_sphere;
    }
} // END INDIRECT RAYS

// PIXEL SAMPLING
inline Vector3 sample_square() {
    // Returns a random point in the [-0.5, -0.5] to [+0.5, +0.5] range
    return Vector3(random_double() - 0.5, random_double() - 0.5, 0);
} 

// LENS SAMPLING
inline Vector3 random_in_unit_disk() {
    // Generates a random point inside a unit disk
    while (true) {
        auto p = Vector3(random_double(-1, 1), random_double(-1, 1), 0);
        if (p.length_squared() < 1) return p;
    }
}



#endif
