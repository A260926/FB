#ifndef SUBSURFACE_EXAMPLE_CPP
#define SUBSURFACE_EXAMPLE_CPP
//==============================================================================================
// Example render: a translucent wax block with real thickness, lit from behind so light enters
// the closed boundary, scatters multiple times inside and leaves through a different surface
// point. Usage: example <output-directory> ; writes <output-directory>/wax.ppm
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//==============================================================================================

#include "rtweekend.h"

#include "camera.h"
#include "bvh.h"
#include "quad.h"
#include "sphere.h"
#include "hittable_list.h"
#include "material.h"
#include "subsurface.h"

#include <fstream>
#include <iostream>
#include <string>


int main(int argc, char** argv) {
    const std::string out_dir = (argc > 1) ? argv[1] : ".";

    hittable_list world;

    // A ground plane of ordinary diffuse material, kept to exercise the old material path.
    auto ground_mat = make_shared<lambertian>(color(0.35, 0.35, 0.4));
    world.add(make_shared<quad>(point3(-6, -1.0, -6), vec3(12, 0, 0), vec3(0, 0, 12),
                                ground_mat));

    // The wax block: a convex six-quad box, already placed (rotated and translated). Its boundary
    // material is irrelevant to internal transport and is replaced by the subsurface marker.
    auto boundary_mat = make_shared<lambertian>(color(0.8, 0.8, 0.8));
    auto wax_boundary = make_shared<translate>(
        make_shared<rotate_y>(
            box(point3(-0.9, -0.9, -0.9), point3(0.9, 0.9, 0.9), boundary_mat),
            25.0),
        vec3(-1.1, 0.0, 0.0));
    // Warm, slightly translucent wax: strong scattering, weak absorption, index above air.
    subsurface_params wax_params(color(2.2, 1.7, 1.2), color(0.05, 0.12, 0.25), 1.45);
    world.add(make_shared<subsurface_object>(wax_boundary, wax_params));

    // A wax sphere next to the block to cover the spherical boundary path.
    auto sphere_boundary =
        make_shared<sphere>(point3(1.3, 0.0, 0.2), 0.9, boundary_mat);
    subsurface_params sphere_params(color(1.4, 2.0, 1.6), color(0.2, 0.06, 0.12), 1.4);
    world.add(make_shared<subsurface_object>(sphere_boundary, sphere_params));

    // A bright quad light behind and above the objects, plus a fill light to the side.
    auto key_light_mat = make_shared<diffuse_light>(color(9, 8, 7));
    auto key_light = make_shared<quad>(point3(-2.5, 3.0, -3.5), vec3(5, 0, 0),
                                       vec3(0, 0, 3), key_light_mat);
    world.add(key_light);

    auto fill_light_mat = make_shared<diffuse_light>(color(2, 2.5, 3));
    auto fill_light = make_shared<quad>(point3(3.5, 1.5, -1.0), vec3(0, 0, 3),
                                        vec3(0, 2, 0), fill_light_mat);
    world.add(fill_light);

    hittable_list lights;
    lights.add(key_light);
    lights.add(fill_light);

    bvh_node accel(world);

    camera cam;
    cam.aspect_ratio = 1.0;
    cam.image_width = 100;
    cam.samples_per_pixel = 36;
    cam.max_depth = 30;
    cam.background = color(0.02, 0.03, 0.05);
    cam.vfov = 50;
    cam.lookfrom = point3(0, 1.4, 5.0);
    cam.lookat = point3(0, 0, 0);
    cam.vup = vec3(0, 1, 0);
    cam.defocus_angle = 0;

    std::ofstream file(out_dir + "/wax.ppm");
    if (!file) {
        std::cerr << "Could not open " << out_dir << "/wax.ppm for writing\n";
        return 1;
    }
    auto* old_buf = std::cout.rdbuf(file.rdbuf());
    cam.render(accel, lights);
    std::cout.rdbuf(old_buf);

    std::clog << "Wrote " << out_dir << "/wax.ppm\n";
    return 0;
}


#endif
