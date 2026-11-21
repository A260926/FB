#ifndef RADIANCE_H
#define RADIANCE_H

#include "util/Vector3.h"

#include <sstream>

using Radiance = Vector3;

inline std::string get_color_string(const Radiance& pixel) {
    // Get the radiance of the pixel
    auto r = std::fmax(0.0, std::fmin(1.0, pixel.x()));
    auto g = std::fmax(0.0, std::fmin(1.0, pixel.y()));
    auto b = std::fmax(0.0, std::fmin(1.0, pixel.z()));
    // Translate the [0,1] component values to the byte range [0,255].
    int rbyte = int(255.999 * r);
    int gbyte = int(255.999 * g);
    int bbyte = int(255.999 * b);
    // Create a string stream to format the color components.
    std::ostringstream oss;
    oss << rbyte << ' ' << gbyte << ' ' << bbyte;
    return oss.str();
}



// TONE MAPPING FUNCTIONS

// Linear Tone Mapping
inline Radiance linear_tone_map(const Radiance& hdr_color, double exposure) {
    // Map the HDR color to the [0,1] range and apply exposure
    Radiance mapped_color = exposure * hdr_color;
    return Radiance(
        std::fmax(0.0, std::fmin(1.0, mapped_color.x())),
        std::fmax(0.0, std::fmin(1.0, mapped_color.y())),
        std::fmax(0.0, std::fmin(1.0, mapped_color.z()))
    );
}

// Reinhard Tone Mapping
inline Vector3 reinhard_tone_map(const Vector3& hdr_color, double exposure) {
    // Apply Reinhard tone mapping
    Vector3 mapped_color = exposure * hdr_color;
    return Vector3(
        std::fmax(0.0, mapped_color.x() / (1.0 + mapped_color.x())),
        std::fmax(0.0, mapped_color.y() / (1.0 + mapped_color.y())),
        std::fmax(0.0, mapped_color.z() / (1.0 + mapped_color.z()))
    );

}

// Filmic Tone Mapping (ACES Approximation)
inline Vector3 filmic_tone_map(const Vector3& hdr_color, double exposure) {
    // Apply filmic tone mapping
    Vector3 mapped_color = exposure * hdr_color;
    const double a = 2.51;
    const double b = 0.03;
    const double c = 2.43;
    const double d = 0.59;
    const double e = 0.14;
    auto filmic_curve = [&](double x) {
        return (x * (a * x + b)) / (x * (c * x + d) + e);
    };
    return Vector3(
        std::fmax(0.0, std::fmin(1.0, filmic_curve(mapped_color.x()))),
        std::fmax(0.0, std::fmin(1.0, filmic_curve(mapped_color.y()))),
        std::fmax(0.0, std::fmin(1.0, filmic_curve(mapped_color.z())))
    );
}

inline Radiance luminance_based_scaling(const Radiance& hdr_color, double exposure) {
    // Calculate luminance
    double luminance = 0.2126 * hdr_color.x() + 0.7152 * hdr_color.y() + 0.0722 * hdr_color.z();
    // Scale luminance by exposure
    double scaled_luminance = exposure * luminance;
    // Avoid division by zero for black pixels
    if (luminance == 0.0) {
        return Vector3(0.0, 0.0, 0.0); // No scaling needed for black
    }
    // Scale RGB values proportionally
    Radiance scaled_color = (scaled_luminance / luminance) * hdr_color;
    // Clamp to [0, 1] to ensure valid color range
    return Radiance(
        std::min(1.0, std::max(0.0, scaled_color.x())),
        std::min(1.0, std::max(0.0, scaled_color.y())),
        std::min(1.0, std::max(0.0, scaled_color.z()))
    );
}



#endif