// Gaussian participating-medium example.
//
// Builds a small surface scene (a diffuse back wall plus a mirror) and a set
// of interpenetrating, differently colored anisotropic Gaussian kernels, then
// renders it once through Camera::render_gaussian. The single traversal
// produces three linear maps that are written as PPM files:
//
//   .task-build/gaussian.ppm      linear radiance value
//   .task-build/error.ppm         conservative per-channel absolute error
//   .task-build/transmittance.ppm first camera-segment transmittance
//
// Visualization scaling (the stored data are linear, so raw values can exceed
// the displayable [0, 1] range and need a fixed, documented scale):
//   - value:        display = clamp(VALUE_SCALE * value), VALUE_SCALE = 0.5
//   - error:        display = clamp(error / ERROR_SCALE), ERROR_SCALE is the
//                   requested abs_error (white == full tolerance)
//   - transmittance:display = transmittance directly (it already lies in
//                   [0, 1]); it is replicated to all three channels
//
// No tone mapping is applied; the scales are pure visualization gains.

#include "Camera.h"
#include "GaussianMedium.h"
#include "object/ObjectList.h"
#include "object/Triangle.h"
#include "light/PointLight.h"
#include "light/LightList.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

GaussianKernel make_kernel(const Point3& mean, double vxx, double vxy,
                           double vxz, double vyy, double vyz, double vzz,
                           double ext, const Radiance& src) {
    GaussianKernel k;
    k.mean = mean;
    k.covariance = {vxx, vxy, vxz, vyy, vyz, vzz};
    k.extinction = ext;
    k.source = src;
    return k;
}

int to_byte(double linear) {
    double clamped = std::fmax(0.0, std::fmin(1.0, linear));
    return static_cast<int>(255.999 * clamped);
}

bool write_ppm(const std::string& filename, int width, int height,
               const std::vector<Radiance>& pixels) {
    std::ofstream out(filename);
    if (!out) return false;
    out << "P3\n" << width << ' ' << height << "\n255\n";
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            const Radiance& p =
                pixels[static_cast<std::size_t>(j) * width + i];
            out << to_byte(p.x()) << ' ' << to_byte(p.y()) << ' '
                << to_byte(p.z()) << '\n';
        }
    }
    return static_cast<bool>(out);
}

} // namespace

int main() {
    // ---- Surfaces -------------------------------------------------------
    Material wall;
    wall.kd = 1.0;
    wall.ks = 0.2;
    wall.ka = 0.1;
    wall.diffusecolor = Radiance(0.35, 0.45, 0.6);

    Material mirror;
    mirror.kd = 0.2;
    mirror.ks = 1.0;
    mirror.ka = 0.0;
    mirror.diffusecolor = Radiance(0.2, 0.2, 0.2);
    mirror.is_reflective = true;
    mirror.reflectivity = 0.85;

    auto world = std::make_shared<ObjectList>();
    world->add(std::make_shared<Triangle>(Point3(-20, -20, -3),
                                          Point3(20, -20, -3),
                                          Point3(0, 20, -3), &wall));
    // A small mirror angled across part of the view.
    world->add(std::make_shared<Triangle>(Point3(-3, -1.2, -2.4),
                                          Point3(-1.2, -1.2, -2.4),
                                          Point3(-2.1, 1.2, -2.4), &mirror));
    world->build();

    // ---- Lights ---------------------------------------------------------
    auto light = std::make_shared<PointLight>(Point3(2.0, 2.0, -0.5),
                                              Radiance(12, 12, 12));
    LightList lights;
    lights.add(light);

    // ---- Interpenetrating, differently colored anisotropic media --------
    std::vector<GaussianKernel> kernels;
    kernels.push_back(make_kernel(Point3(0.0, 0.0, -1.2),
                                  0.5, 0.10, 0.05, 0.35, -0.04, 0.30, 0.9,
                                  Radiance(1.6, 0.25, 0.2)));   // red
    kernels.push_back(make_kernel(Point3(0.55, 0.25, -1.4),
                                  0.35, -0.08, 0.04, 0.45, 0.03, 0.25, 0.8,
                                  Radiance(0.2, 1.6, 0.3)));    // green
    kernels.push_back(make_kernel(Point3(-0.5, -0.2, -1.0),
                                  0.30, 0.06, -0.05, 0.40, 0.05, 0.55, 1.0,
                                  Radiance(0.25, 0.35, 1.7)));  // blue
    kernels.push_back(make_kernel(Point3(0.1, -0.4, -1.8),
                                  0.20, 0.0, 0.0, 0.20, 0.0, 0.20, 1.2,
                                  Radiance(1.2, 1.0, 0.2)));    // narrow yellow
    GaussianMedium medium(kernels);

    // ---- Camera ---------------------------------------------------------
    Camera cam;
    cam.image_width = 64;
    cam.image_height = 64;
    cam.lookfrom = Point3(0, 0, 0);
    cam.lookat = Point3(0, 0, -1);
    cam.vup = Vector3(0, 1, 0);
    cam.vfov = 60;
    cam.ambient_light = Radiance(0.15, 0.15, 0.15);
    cam.background = Radiance(0.05, 0.08, 0.12);
    cam.nbounces = 4;

    const double abs_error = 1e-4;
    const std::uint64_t max_work = 1000000;

    GaussianFrame frame;
    if (!cam.render_gaussian(*world, lights, medium, abs_error, max_work,
                             frame)) {
        std::cerr << "render_gaussian failed (budget or unsupported scene)\n";
        return 1;
    }

    // ---- Visualization scaling ------------------------------------------
    const double VALUE_SCALE = 0.5;
    std::vector<Radiance> value_view(frame.value.size());
    std::vector<Radiance> error_view(frame.error.size());
    std::vector<Radiance> transmittance_view(frame.transmittance.size());
    for (std::size_t i = 0; i < frame.value.size(); ++i) {
        value_view[i] = frame.value[i] * VALUE_SCALE;
        error_view[i] = frame.error[i] / abs_error;
        double t = frame.transmittance[i];
        transmittance_view[i] = Radiance(t, t, t);
    }

    const std::string dir = ".task-build/";
    bool ok = true;
    ok &= write_ppm(dir + "gaussian.ppm", frame.width, frame.height,
                    value_view);
    ok &= write_ppm(dir + "error.ppm", frame.width, frame.height, error_view);
    ok &= write_ppm(dir + "transmittance.ppm", frame.width, frame.height,
                    transmittance_view);
    if (!ok) {
        std::cerr << "failed to write output PPM files\n";
        return 1;
    }

    std::cout << "wrote .task-build/gaussian.ppm (linear value * "
              << VALUE_SCALE << ")\n"
              << "wrote .task-build/error.ppm (error / abs_error "
              << abs_error << ")\n"
              << "wrote .task-build/transmittance.ppm (first-segment "
                 "transmittance, grayscale)\n"
              << "frame " << frame.width << 'x' << frame.height
              << " rendered in one traversal with no tone mapping\n";
    return 0;
}
