// =====================================================================================
// Snapshot forking example.
//
// A preview renderer can keep an old scene_snapshot alive while a newer edit is
// built: every snapshot owns the descriptions and derived BVH/light state it
// needs, while prototype geometry, materials and BLAS stay shared and immutable.
//
// Update ordering matters and is safe:
//   1. Build the parent snapshot once.
//   2. Fork a LEFT branch (move an instance) from the parent.
//   3. Fork a RIGHT branch (replace a shared prototype / retarget an instance)
//      also from the parent. The two forks never touch each other or the parent.
//   4. Render parent, left, right in any order; each render pins one immutable
//      world/light version for its whole duration.
//
// Usage: snapshots <output-directory>
// Writes parent.ppm, left.ppm and right.ppm into that directory.
// =====================================================================================

#include "rtweekend.h"
#include "scene_snapshot.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "material.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>


namespace {


void write_ppm(const std::string& path, const std::string& ppm) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "cannot open " << path << '\n';
        std::exit(1);
    }
    out << ppm;
}


std::string render(camera& cam, const scene_snapshot& snapshot) {
    std::ostringstream out;
    auto* saved = std::cout.rdbuf(out.rdbuf());
    cam.render(snapshot);
    std::cout.rdbuf(saved);
    return out.str();
}


camera make_camera() {
    camera cam;
    cam.aspect_ratio = 1.0;
    cam.image_width = 64;
    cam.samples_per_pixel = 16;
    cam.max_depth = 8;
    cam.background = color(0.02, 0.03, 0.06);
    cam.vfov = 55;
    cam.lookfrom = point3(0, 2.0, 4.5);
    cam.lookat = point3(0, 0.4, -3.0);
    return cam;
}


} // namespace


int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 1;
    }
    const std::string dir = argv[1];

    auto ground_mat = make_shared<lambertian>(color(0.45, 0.5, 0.45));
    auto ball_mat = make_shared<lambertian>(color(0.85, 0.35, 0.25));
    auto pillar_mat = make_shared<lambertian>(color(0.3, 0.5, 0.85));
    auto glow_mat = make_shared<diffuse_light>(color(7, 6, 5));

    // Production geometry is built once and then treated as immutable.
    auto ball = make_shared<sphere>(point3(0, 0, 0), 0.7, ball_mat);
    auto pillar = make_shared<sphere>(point3(0, 0, 0), 0.5, pillar_mat);
    auto ground = make_shared<quad>(point3(-9, -0.8, -9), vec3(18, 0, 0),
                                    vec3(0, 0, 18), ground_mat);
    auto lamp = make_shared<quad>(point3(-2, 3.2, -4.5), vec3(4, 0, 0),
                                  vec3(0, 0, 2.5), glow_mat);

    std::vector<scene_prototype> prototypes = {
        scene_prototype{ball,   false},  // slot 0
        scene_prototype{pillar, false}, // slot 1
        scene_prototype{ground, false}, // slot 2
        scene_prototype{lamp,   true},  // slot 3 (area light)
    };

    // Instance slots are their input indices and stay stable across every fork.
    std::vector<scene_instance_desc> instances = {
        scene_instance_desc{0, vec3(0, 0, -3.5),   0.0,  true}, // 0: hero ball
        scene_instance_desc{1, vec3(1.6, 0, -3.0), 0.0,  true}, // 1: pillar
        scene_instance_desc{1, vec3(-1.6, 0, -3.0), 0.0,  true}, // 2: shares pillar
        scene_instance_desc{2, vec3(0, 0, 0),            0.0,  true}, // 3: ground
        scene_instance_desc{3, vec3(0, 0, 0),            0.0,  true}, // 4: light
    };

    // 1) The version an in-flight render may still be querying.
    scene_snapshot parent(prototypes, instances);

    // 2) LEFT branch: move the hero ball and hide one pillar.
    scene_snapshot left = parent.updated(
        {},
        {
            {0, scene_instance_desc{0, vec3(-2.2, 0, -3.2), 35.0, true}},
            {2, scene_instance_desc{1, vec3(-1.6, 0, -3.0), 0.0, false}},
        });

    // 3) RIGHT branch: replace the shared pillar prototype (instances 1 and 2
    //    both see it) and retarget the hero ball onto the new geometry, then
    //    reposition the light. The parent and LEFT branch are unaffected.
    auto wide_pillar = make_shared<sphere>(point3(0, 0, 0), 0.85, pillar_mat);
    scene_snapshot right = parent.updated(
        {{1, scene_prototype{wide_pillar, false}}},
        {
            {0, scene_instance_desc{1, vec3(0.9, 0, -3.4), -20.0, true}},
            {4, scene_instance_desc{3, vec3(0.6, 0, 0.0), 0.0, true}},
        });

    // 4) Render each immutable version; order does not change any pixel.
    std::srand(20260101);
    camera parent_cam = make_camera();
    write_ppm(dir + "/parent.ppm", render(parent_cam, parent));

    std::srand(20260101);
    camera left_cam = make_camera();
    write_ppm(dir + "/left.ppm", render(left_cam, left));

    std::srand(20260101);
    camera right_cam = make_camera();
    write_ppm(dir + "/right.ppm", render(right_cam, right));

    std::cout << "SNAPSHOT_EXAMPLE parent lights=" << parent.light_count()
              << " left lights=" << left.light_count()
              << " right lights=" << right.light_count() << '\n';
    std::cout << "Prototypes stay shared and immutable; updated() copies only"
                 " descriptions plus the affected BVH path, never the geometry.\n";
    return 0;
}
