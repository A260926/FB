//
// Example: editing a scene between frames with scene nodes.
//
// This program builds a small scene where three cards share the same mesh
// and are grouped under one editable scene node, then renders two frames:
//
//   1. before.ppm -- the scene as built;
//   2. after.ppm  -- the same scene after moving and rotating the group,
//                    hiding one card and deforming the shared mesh.
//
// The frame-to-frame call order is:
//
//   // once, at setup
//   auto bvh    = make_trace_bvh(scene, params);
//   auto lights = make_trace_lights(scene, params);
//   auto state  = make_trace_state(scene, params);
//
//   // every frame
//   ... edit scene.nodes, shape vertices, visibility ...
//   update_trace_scene(scene, bvh, lights, state, params, updated_shapes);
//   while (state.samples < params.samples)
//     trace_samples(state, scene, bvh, lights, params);
//   ... read back get_rendered_image(state) ...
//
// update_trace_scene() validates the node hierarchy, publishes node frames
// and visibility to the bound instances, refits the existing bvh (without
// rebuilding it), rebuilds the light set and resets the progressive state,
// so the next frame starts from fresh samples. Hidden instances are skipped
// by intersection queries and do not contribute area lights.
//
// Usage: scene_nodes_example [output-directory]
//

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "yocto_bvh.h"
#include "yocto_scene.h"
#include "yocto_trace.h"

using namespace yocto;

// Save a linear image as a binary PPM with a simple gamma tonemap.
static void save_ppm(const std::string& filename, const image_data& image) {
  auto file = std::ofstream(filename, std::ios::binary);
  file << "P6\n" << image.width << " " << image.height << "\n255\n";
  for (auto& pixel : image.pixels) {
    for (auto channel = 0; channel < 3; channel++) {
      auto value = std::pow(std::fmax(0.0f, std::fmin(1.0f, pixel[channel])),
          1.0f / 2.2f);
      file.put((char)std::lround(value * 255));
    }
  }
}

// Shared card mesh: two triangles forming a 1x1 card in the z=0 plane.
static shape_data make_card_shape() {
  auto shape      = shape_data{};
  shape.positions = {{-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0.5f, 0.5f, 0},
      {-0.5f, 0.5f, 0}};
  shape.triangles = {{0, 1, 2}, {0, 2, 3}};
  return shape;
}

// Ground mesh: two triangles forming a large quad.
static shape_data make_ground_shape() {
  auto shape      = shape_data{};
  shape.positions = {{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}};
  shape.triangles = {{0, 1, 2}, {0, 2, 3}};
  return shape;
}

static material_data make_material(const vec3f& color, const vec3f& emission) {
  auto material      = material_data{};
  material.type      = material_type::matte;
  material.color     = color;
  material.roughness = 0.5f;
  material.emission  = emission;
  return material;
}

int main(int argc, char** argv) {
  auto output = std::string(argc > 1 ? argv[1] : ".");
  std::filesystem::create_directories(output);
  std::printf(
      "frame loop: edit nodes/shapes -> update_trace_scene -> trace_samples\n");

  // scene with three shared-mesh cards grouped under one node, an area
  // light above and a ground plane
  auto scene         = scene_data{};
  scene.shapes       = {make_card_shape(), make_ground_shape(), make_card_shape()};
  scene.materials    = {
      make_material({0.8f, 0.2f, 0.2f}, {0, 0, 0}),   // red card
      make_material({0.2f, 0.8f, 0.2f}, {0, 0, 0}),   // green card
      make_material({0.2f, 0.2f, 0.8f}, {0, 0, 0}),   // blue card
      make_material({0, 0, 0}, {4, 4, 4}),            // light
      make_material({0.6f, 0.6f, 0.6f}, {0, 0, 0}),   // ground
  };
  scene.instances    = {
      {identity3x4f, 0, 0},                          // 0: red card
      {identity3x4f, 0, 1},                          // 1: green card
      {identity3x4f, 0, 2},                          // 2: blue card
      {translation_frame({0, 2.5f, -3}) *
           rotation_frame({1, 0, 0}, pif / 2), 2, 3},  // 3: light (unbound)
      {identity3x4f, 1, 4},                          // 4: ground (unbound)
  };
  auto camera   = camera_data{};
  camera.aspect = 1;
  camera.frame  = lookat_frame({0, 1.5f, 3.5f}, {0, 0.8f, -3}, {0, 1, 0}, false);
  scene.cameras = {camera};
  // the three cards are bound to nodes grouped under one editable parent
  scene.nodes = {
      {-1, -1, translation_frame({0, 1, -3}), true},       // 0: card group
      {0, 0, translation_frame({-1.2f, 0, 0}), true},      // 1: red card
      {0, 1, translation_frame({0, 0, 0}), true},          // 2: green card
      {0, 2, translation_frame({1.2f, 0, 0}), true},       // 3: blue card
  };
  update_scene_nodes(scene);

  // render setup, done once
  auto params       = trace_params{};
  params.resolution = 96;
  params.samples    = 16;
  params.bounces    = 4;
  params.noparallel = true;
  auto bvh          = make_trace_bvh(scene, params);
  auto lights       = make_trace_lights(scene, params);
  auto state        = make_trace_state(scene, params);

  // frame 1: render the scene as built
  while (state.samples < params.samples) {
    trace_samples(state, scene, bvh, lights, params);
  }
  save_ppm(output + "/before.ppm", get_rendered_image(state));
  std::printf("wrote %s\n", (output + "/before.ppm").c_str());

  // edit between frames: move and rotate the whole card group, hide the
  // blue card and deform the shared card mesh; the light and the ground
  // are not bound to nodes and keep their own transforms
  scene.nodes[0].local = translation_frame({0.8f, 1.2f, -3}) *
                         rotation_frame({0, 1, 0}, pif / 8);
  scene.nodes[3].visible = false;
  scene.shapes[0].positions[2].z = 0.3f;  // bend the shared card
  scene.shapes[0].positions[3].z = 0.3f;

  // frame 2: publish the edits, refit the bvh, rebuild lights, reset the
  // progressive state and render again with the same params
  update_trace_scene(scene, bvh, lights, state, params, {0});
  while (state.samples < params.samples) {
    trace_samples(state, scene, bvh, lights, params);
  }
  save_ppm(output + "/after.ppm", get_rendered_image(state));
  std::printf("wrote %s\n", (output + "/after.ppm").c_str());
  return 0;
}
