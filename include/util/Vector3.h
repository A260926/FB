#ifndef VECTOR3_H
#define VECTOR3_H

#include <cmath>
#include <iostream>

class Vector3 {
  public:
    double element[3]; // x, y, z

    Vector3() : element{0,0,0} {}
    Vector3(double e0, double e1, double e2) : element{e0, e1, e2} {}

    double x() const { return element[0]; }
    double y() const { return element[1]; }
    double z() const { return element[2]; }

    Vector3 operator-() const { return Vector3(-element[0], -element[1], -element[2]); }
    double operator[](int i) const { return element[i]; }
    double& operator[](int i) { return element[i]; }

    Vector3& operator+=(const Vector3& v) {
        for (int i = 0; i < 3; ++i) {
            element[i] += v.element[i];
        }
        return *this;
    }

    Vector3& operator*=(double t) {
        for (int i = 0; i < 3; ++i) {
            element[i] *= t;
        }
        return *this;
    }

    Vector3& operator/=(double t) {
        if (fabs(t) < 1e-12) {
            element[0] = element[1] = element[2] = 0;
            return *this;
        }
        return *this *= 1/t;
    }

    double length() const {
        return std::sqrt(length_squared());
    }

    double length_squared() const {
        double x2 = element[0] * element[0];
        double y2 = element[1] * element[1];
        double z2 = element[2] * element[2];
        return x2 + y2 + z2;
    }

    bool near_zero() const {
        // Return true if the vector is close to zero in all dimensions.
        const double s = 1e-8;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(element[i]) >= s) return false;
        }
        return true;
    }

    Vector3& operator*=(const Vector3& v) {
        // Element-wise multiplication
        element[0] *= v.element[0];
        element[1] *= v.element[1];
        element[2] *= v.element[2];
        return *this;
    }
};

// Type aliases for Vector3
using Point3 = Vector3;


// Vector Utility Functions

inline std::ostream& operator<<(std::ostream& out, const Vector3& v) {
    return out << v.element[0] << ' ' << v.element[1] << ' ' << v.element[2];
}

inline Vector3 operator+(const Vector3& u, const Vector3& v) {
    return Vector3(u.element[0] + v.element[0], u.element[1] + v.element[1], u.element[2] + v.element[2]);
}

inline Vector3 operator-(const Vector3& u, const Vector3& v) {
    return Vector3(u.element[0] - v.element[0], u.element[1] - v.element[1], u.element[2] - v.element[2]);
}

inline Vector3 operator*(const Vector3& u, const Vector3& v) {
    return Vector3(u.element[0] * v.element[0], u.element[1] * v.element[1], u.element[2] * v.element[2]);
}

inline Vector3 operator*(double t, const Vector3& v) {
    return Vector3(t*v.element[0], t*v.element[1], t*v.element[2]);
}

inline Vector3 operator*(const Vector3& v, double t) {
    return t * v;
}

inline Vector3 operator/(const Vector3& v, double t) {
    if (fabs(t) < 1e-12) return Vector3(0,0,0);
    return (1/t) * v;
}

inline bool operator!=(const Vector3& u, const Vector3& v) {
    return (u.element[0] != v.element[0]) || (u.element[1] != v.element[1]) || (u.element[2] != v.element[2]);
}

inline double dot(const Vector3& u, const Vector3& v) {
    double result = 0;
    for (int i = 0; i < 3; ++i) {
        result += u.element[i] * v.element[i];
    }
    return result;
}

inline Vector3 cross(const Vector3& u, const Vector3& v) {
    double cx = u.element[1] * v.element[2] - u.element[2] * v.element[1];
    double cy = u.element[2] * v.element[0] - u.element[0] * v.element[2];
    double cz = u.element[0] * v.element[1] - u.element[1] * v.element[0];
    return Vector3(cx, cy, cz);
}

inline Vector3 unit_vector(const Vector3& v) {
    if (v.length_squared() < 1e-12) return Vector3(0,0,0);
    return v / v.length();
}

inline Vector3 perpendicular(const Vector3& v) {
    // Return a vector perpendicular to the input vector
    if (fabs(v.x()) > fabs(v.z())) {
        return Vector3(-v.y(), v.x(), 0.0);
    } else {
        return Vector3(0.0, -v.z(), v.y());
    }
}

inline Vector3 saturate(const Vector3& v) {
    // Clamp the input vector to the range [0, 1]
    return Vector3(std::fmax(0.0, std::fmin(v.x(), 1.0)),
                std::fmax(0.0, std::fmin(v.y(), 1.0)),
                std::fmax(0.0, std::fmin(v.z(), 1.0)));
}

inline Vector3 normalize(const Vector3& v, const Vector3 fallback) {
    // Normalize the input vector, or return the fallback vector if the input is the zero vector
    const double l = v.length();

    return l == 0 ? fallback : v / l; 
}

inline Vector3 normalize(const Vector3& v) {
    // Normalize the input vector
    const double l = v.length();
    if (l < 1e-12) return Vector3(0,0,0);
    return  v / l; 
}

#endif