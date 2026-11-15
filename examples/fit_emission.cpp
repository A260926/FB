// Inverse-lighting example for cpp-path-tracer.
//
// Builds a small room out of three PatchMesh objects (ceiling, floor, back
// wall) plus a sphere occluder, fits non-negative RGB emission for the two
// ceiling panels so that the floor and wall faces reach target brightness
// after all diffuse bounces, then renders the fitted exitance (B/pi) with
// the regular camera pipeline.
//
// Usage: fit_emission <output.ppm>

#include "object/ObjectList.h"
#include "object/PatchMesh.h"
#include "object/Sphere.h"
#include "PatchTransport.h"
#include "Camera.h"

#include <array>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fit_emission <output.ppm>\n";
        return 1;
    }
    const std::string output_path = argv[1];

    // Flat diffuse materials; rho = kd * diffusecolor must stay in [0, 0.95].
    Material ceil_mat;
    ceil_mat.kd = 0.9;
    ceil_mat.diffusecolor = Radiance(0.85, 0.85, 0.85);
    Material floor_mat;
    floor_mat.kd = 0.9;
    floor_mat.diffusecolor = Radiance(0.70, 0.65, 0.60);
    Material wall_mat;
    wall_mat.kd = 0.9;
    wall_mat.diffusecolor = Radiance(0.75, 0.72, 0.70);
    Material sphere_mat;
    sphere_mat.kd = 0.9;
    sphere_mat.diffusecolor = Radiance(0.60, 0.60, 0.65);

    // Room spans x in [-1, 1], y in [-1, 1], z in [-2, 0].
    // Ceiling: y = 1, outward normal -y (two triangles).
    std::vector<Point3> ceil_v = {
        Point3(-1, 1, 0), Point3(1, 1, 0), Point3(1, 1, -2), Point3(-1, 1, -2)};
    std::vector<std::array<int, 3>> ceil_t = {{{0, 2, 1}}, {{0, 3, 2}}};
    std::vector<Material*> ceil_m = {&ceil_mat, &ceil_mat};
    auto ceiling = std::make_shared<PatchMesh>(ceil_v, ceil_t, ceil_m);

    // Floor: y = -1, outward normal +y.
    std::vector<Point3> floor_v = {
        Point3(-1, -1, 0), Point3(1, -1, 0), Point3(1, -1, -2), Point3(-1, -1, -2)};
    std::vector<std::array<int, 3>> floor_t = {{{0, 1, 2}}, {{0, 2, 3}}};
    std::vector<Material*> floor_m = {&floor_mat, &floor_mat};
    auto floor = std::make_shared<PatchMesh>(floor_v, floor_t, floor_m);

    // Back wall: z = -2, outward normal +z.
    std::vector<Point3> wall_v = {
        Point3(-1, -1, -2), Point3(1, -1, -2), Point3(1, 1, -2), Point3(-1, 1, -2)};
    std::vector<std::array<int, 3>> wall_t = {{{0, 1, 2}}, {{0, 2, 3}}};
    std::vector<Material*> wall_m = {&wall_mat, &wall_mat};
    auto wall = std::make_shared<PatchMesh>(wall_v, wall_t, wall_m);

    // The real world: the three meshes plus an unrelated occluder.
    ObjectList world;
    world.add(ceiling);
    world.add(floor);
    world.add(wall);
    world.add(std::make_shared<Sphere>(Point3(0.35, -0.55, -0.9), 0.4, &sphere_mat));
    world.build();

    // Discrete transport over the six mesh faces.
    PatchTransport transport({ceiling, floor, wall}, world, 1e-6);

    // Global face ids: ceiling 0..1, floor 2..3, wall 4..5.
    std::vector<int> candidates = {0, 1};
    std::vector<FitObservation> observations = {
        {2, Radiance(0.55, 0.48, 0.40), 1.0},
        {3, Radiance(0.55, 0.48, 0.40), 1.0},
        {4, Radiance(0.40, 0.40, 0.45), 1.0},
        {5, Radiance(0.40, 0.40, 0.45), 1.0},
    };

    auto fit = transport.fit(candidates, observations, 1e-4, 2000);
    if (!fit) {
        std::cerr << "emission fit failed\n";
        return 1;
    }
    std::cout << "fit objective " << fit->objective()
              << " projected-gradient-norm " << fit->projected_gradient_norm() << "\n";
    for (int f = 0; f < fit->face_count(); ++f) {
        std::cout << "face " << f << " E [" << fit->emission(f) << "] B ["
                  << fit->exitance(f) << "]\n";
    }

    // Render the fitted exitance with the original camera pipeline.
    Camera cam;
    cam.image_width = 96;
    cam.image_height = 96;
    cam.lookfrom = Point3(0, 0, 4);
    cam.lookat = Point3(0, 0, -2);
    cam.vfov = 50;
    cam.aperture = 0.0;
    cam.samples_per_pixel = 1;
    cam.sampling_method = AntiAliasing::RANDOM;
    cam.tone_mapper = ToneMapper::LINEAR;
    cam.exposure = 0.0;
    cam.background = Radiance(0.02, 0.02, 0.03);
    cam.filename = output_path;
    cam.render_fitted(world, *fit);

    std::cout << "wrote " << output_path << "\n";
    return 0;
}
