//
// Checks for the scene-node hierarchy, visibility propagation and the
// frame-to-frame scene update entry points. Hand-computed rigid transforms
// and explicitly tiled scenes are used as independent references.
//

#include "task_support/check.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

#include "yocto_bvh.h"
#include "yocto_scene.h"
#include "yocto_trace.h"

using namespace yocto;
using std::vector;

// -----------------------------------------------------------------------------
// HELPERS
// -----------------------------------------------------------------------------

// Hand-computed rigid frames with exact integer components, so that frame
// compositions are exact in floating point and can be compared tightly.
static frame3f translation3d(float x, float y, float z) {
  return {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {x, y, z}};
}
static frame3f rotation_z90() { return {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, 0}}; }
static frame3f rotation_x90() { return {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 0, 0}}; }

static bool frame_near(const frame3f& a, const frame3f& b, float eps = 1e-5f) {
  for (auto i = 0; i < 4; i++) {
    for (auto j = 0; j < 3; j++) {
      if (std::abs(a[i][j] - b[i][j]) > eps) return false;
    }
  }
  return true;
}

static bool vec3_near(const vec3f& a, const vec3f& b, float eps = 1e-5f) {
  return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps &&
         std::abs(a.z - b.z) <= eps;
}

template <typename Func>
static bool throws_invalid_argument(Func&& func) {
  try {
    func();
  } catch (const std::invalid_argument&) { return true; }
  return false;
}

// Single-triangle shape in the z=0 plane, spanning [-1,1] in x and y.
static shape_data make_triangle_shape() {
  auto shape       = shape_data{};
  shape.positions  = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
  shape.triangles  = {{0, 1, 2}};
  return shape;
}

static material_data make_matte(const vec3f& color) {
  auto material     = material_data{};
  material.type     = material_type::matte;
  material.color    = color;
  material.roughness = 0.5f;
  return material;
}

static material_data make_emissive(const vec3f& emission) {
  auto material      = make_matte({0, 0, 0});
  material.emission  = emission;
  return material;
}

static instance_data make_instance(int shape, int material) {
  auto instance     = instance_data{};
  instance.shape    = shape;
  instance.material = material;
  return instance;
}

static scene_node make_node(int parent, int instance, const frame3f& local,
    bool visible = true) {
  auto node     = scene_node{};
  node.parent   = parent;
  node.instance = instance;
  node.local    = local;
  node.visible  = visible;
  return node;
}

static trace_params test_params(trace_sampler_type sampler = trace_sampler_type::path) {
  auto params       = trace_params{};
  params.resolution = 16;
  params.samples    = 2;
  params.bounces    = 2;
  params.seed       = 42;
  params.noparallel = true;
  params.sampler    = sampler;
  return params;
}

static camera_data test_camera() {
  auto camera   = camera_data{};
  camera.aspect = 1;
  camera.frame.o = {0, 0, 5};
  return camera;
}

static void trace_to_completion(trace_state& state, const scene_data& scene,
    const trace_bvh& bvh, const trace_lights& lights,
    const trace_params& params) {
  while (state.samples < params.samples) {
    trace_samples(state, scene, bvh, lights, params);
  }
}

static bool images_equal(const trace_state& a, const trace_state& b) {
  if (a.width != b.width || a.height != b.height) return false;
  if (a.image.size() != b.image.size()) return false;
  for (auto idx = (size_t)0; idx < a.image.size(); idx++) {
    if (!(a.image[idx] == b.image[idx])) return false;
  }
  return true;
}

static bool rngs_equal(const trace_state& a, const trace_state& b) {
  if (a.rngs.size() != b.rngs.size()) return false;
  for (auto idx = (size_t)0; idx < a.rngs.size(); idx++) {
    if (a.rngs[idx].state != b.rngs[idx].state) return false;
    if (a.rngs[idx].inc != b.rngs[idx].inc) return false;
  }
  return true;
}

static bool image_is_zero(const trace_state& state) {
  for (auto& pixel : state.image) {
    if (!(pixel == vec4f{0, 0, 0, 0})) return false;
  }
  return true;
}

// Snapshot of the bvh topology: node connectivity, leaf ranges and the
// primitive ordering, but not the bounding boxes.
struct bvh_topology {
  vector<bvh_node> nodes      = {};
  vector<int>      primitives = {};
};

static bvh_topology snapshot_topology(const bvh_tree& bvh) {
  return {bvh.nodes, bvh.primitives};
}

static bool topology_equal(const bvh_topology& a, const bvh_topology& b) {
  if (a.nodes.size() != b.nodes.size()) return false;
  if (a.primitives != b.primitives) return false;
  for (auto idx = (size_t)0; idx < a.nodes.size(); idx++) {
    if (a.nodes[idx].internal != b.nodes[idx].internal) return false;
    if (a.nodes[idx].start != b.nodes[idx].start) return false;
    if (a.nodes[idx].num != b.nodes[idx].num) return false;
    if (a.nodes[idx].axis != b.nodes[idx].axis) return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// HIERARCHY: out-of-order three-level forest with multiple roots
// -----------------------------------------------------------------------------

static int case_forest_out_of_order() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  for (auto idx = 0; idx < 4; idx++) scene.instances.push_back(make_instance(0, 0));
  scene.instances[3].frame = translation3d(7, 7, 7);  // unbound instance

  // Logical forest (three levels under A, multiple roots), stored out of
  // order so that children come before their parents in the array:
  //   A(root, group) -> C(group, hidden) -> D(instance 0)
  //   B(root, group) -> E(instance 1)
  //   F(root, instance 2)
  auto rz90 = rotation_z90();
  rz90.o    = {1, 0, 0};
  scene.nodes = {
      make_node(2, 0, translation3d(1, 1, 0)),           // 0: D
      make_node(5, 1, rotation_x90()),                   // 1: E
      make_node(3, -1, translation3d(0, 0, 1), false),   // 2: C (hidden)
      make_node(-1, -1, rz90),                           // 3: A
      make_node(-1, 2, translation3d(-3, 0, 0)),         // 4: F
      make_node(-1, -1, translation3d(0, 2, 0)),         // 5: B
  };

  update_scene_nodes(scene);

  // Hand-computed worlds. Rz90 maps (x,y,z) to (-y,x,z); translations of
  // children are rotated by their ancestors, so composition does not commute.
  // world(D) = A * C * D: rotation Rz90, origin Rz90*(1,1,0)+(1,0,1) = (0,1,1)
  auto expected_d = frame3f{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 1, 1}};
  // world(E) = B * E: rotation Rx90, origin (0,2,0)
  auto expected_e = frame3f{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 2, 0}};
  TASK_CHECK(frame_near(scene.instances[0].frame, expected_d));
  TASK_CHECK(frame_near(scene.instances[1].frame, expected_e));
  TASK_CHECK(frame_near(scene.instances[2].frame, translation3d(-3, 0, 0)));

  // visibility is the logical and along the ancestor chain
  TASK_CHECK(scene.instances[0].visible == false);  // C is hidden
  TASK_CHECK(scene.instances[1].visible == true);
  TASK_CHECK(scene.instances[2].visible == true);

  // unbound instance keeps its own frame and visibility
  TASK_CHECK(frame_near(scene.instances[3].frame, translation3d(7, 7, 7)));
  TASK_CHECK(scene.instances[3].visible == true);

  // non-commutativity sanity check: T(1,0,0)*Rz90 != Rz90*T(1,0,0)
  auto rt = translation3d(1, 0, 0) * rotation_z90();
  auto tr = rotation_z90() * translation3d(1, 0, 0);
  TASK_CHECK(vec3_near(rt.o, {1, 0, 0}));
  TASK_CHECK(vec3_near(tr.o, {0, 1, 0}));
  return 0;
}

// -----------------------------------------------------------------------------
// HIERARCHY: two instances sharing one shape, against a tiled reference
// -----------------------------------------------------------------------------

static int case_shared_instances_tiled() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));
  scene.instances.push_back(make_instance(0, 0));  // shares shape 0

  auto group = rotation_z90();
  group.o    = {1, 2, 3};
  scene.nodes = {
      make_node(-1, -1, group),                 // 0: group
      make_node(0, 0, translation3d(2, 0, 0)),  // 1: first shared instance
      make_node(0, 1, translation3d(-2, 0, 0))  // 2: second shared instance
  };
  update_scene_nodes(scene);

  // hand-computed worlds: Rz90*(+-2,0,0) + (1,2,3)
  auto expected0 = frame3f{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {1, 4, 3}};
  auto expected1 = frame3f{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {1, 0, 3}};
  TASK_CHECK(frame_near(scene.instances[0].frame, expected0));
  TASK_CHECK(frame_near(scene.instances[1].frame, expected1));

  // explicitly tiled reference scene: same content, frames set by hand
  auto reference = scene_data{};
  reference.shapes.push_back(make_triangle_shape());
  reference.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  reference.instances.push_back(make_instance(0, 0));
  reference.instances.push_back(make_instance(0, 0));
  reference.instances[0].frame = expected0;
  reference.instances[1].frame = expected1;

  auto bvh     = make_scene_bvh(scene, false, true);
  auto ref_bvh = make_scene_bvh(reference, false, true);

  // identical intersection results for both scenes
  for (auto x : {1.0f, 1.25f, 0.75f}) {
    for (auto y : {4.0f, 0.0f, 3.9f}) {
      auto ray  = ray3f{{x, y, 0}, {0, 0, 1}};
      auto hit  = intersect_scene_bvh(bvh, scene, ray);
      auto rhit = intersect_scene_bvh(ref_bvh, reference, ray);
      TASK_CHECK(hit.hit == rhit.hit);
      TASK_CHECK(hit.instance == rhit.instance);
      if (hit.hit) TASK_CHECK(std::abs(hit.distance - rhit.distance) < 1e-5f);
    }
  }
  // the shared triangle sits in the z=3 plane
  auto hit = intersect_scene_bvh(bvh, scene, {{1, 4, 0}, {0, 0, 1}});
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 3) < 1e-5f);

  // moving the group moves both shared instances
  scene.nodes[0].local.o.x = 2;
  update_scene_nodes(scene);
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {2, 4, 3}));
  TASK_CHECK(vec3_near(scene.instances[1].frame.o, {2, 0, 3}));
  return 0;
}

// -----------------------------------------------------------------------------
// HIERARCHY: reparenting with and without keeping the world pose
// -----------------------------------------------------------------------------

static int case_reparent_keep_world() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));
  scene.nodes = {
      make_node(-1, -1, translation3d(1, 0, 0)),            // 0: G1
      make_node(-1, -1, translation3d(10, 0, 0), false),    // 1: G2 (hidden)
      make_node(0, 0, translation3d(0, 1, 0)),              // 2: N -> instance 0
  };
  update_scene_nodes(scene);
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 1, 0}));
  TASK_CHECK(scene.instances[0].visible == true);

  // keep_world: the instance world pose is preserved, the local frame is
  // recomputed, and visibility is re-inherited from the new ancestors
  reparent_scene_node(scene, 2, 1, true);
  TASK_CHECK(scene.nodes[2].parent == 1);
  TASK_CHECK(vec3_near(scene.nodes[2].local.o, {-9, 1, 0}));
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 1, 0}));
  TASK_CHECK(scene.instances[0].visible == false);  // G2 is hidden

  // keep_local: the local frame is preserved and the world pose follows
  reparent_scene_node(scene, 2, -1, false);
  TASK_CHECK(scene.nodes[2].parent == -1);
  TASK_CHECK(vec3_near(scene.nodes[2].local.o, {-9, 1, 0}));
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {-9, 1, 0}));
  TASK_CHECK(scene.instances[0].visible == true);

  // keep_world again under G1: world stays, local recomputed
  reparent_scene_node(scene, 2, 0, true);
  TASK_CHECK(vec3_near(scene.nodes[2].local.o, {-10, 1, 0}));
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {-9, 1, 0}));
  return 0;
}

// -----------------------------------------------------------------------------
// HIERARCHY: invalid hierarchies throw and roll back without partial publish
// -----------------------------------------------------------------------------

static int case_invalid_hierarchies_rollback() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));
  scene.instances.push_back(make_instance(0, 0));
  scene.nodes = {
      make_node(-1, -1, translation3d(1, 0, 0)),  // 0: G
      make_node(0, 0, translation3d(0, 1, 0)),    // 1: N0 -> instance 0
      make_node(-1, 1, translation3d(5, 0, 0)),   // 2: N1 -> instance 1
  };
  update_scene_nodes(scene);
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 1, 0}));
  TASK_CHECK(vec3_near(scene.instances[1].frame.o, {5, 0, 0}));

  auto check_unchanged = [&]() {
    return scene.nodes[0].parent == -1 && scene.nodes[1].parent == 0 &&
           scene.nodes[2].parent == -1 && scene.nodes[1].instance == 0 &&
           scene.nodes[2].instance == 1 &&
           vec3_near(scene.nodes[1].local.o, {0, 1, 0}) &&
           vec3_near(scene.nodes[2].local.o, {5, 0, 0}) &&
           vec3_near(scene.instances[0].frame.o, {1, 1, 0}) &&
           vec3_near(scene.instances[1].frame.o, {5, 0, 0}) &&
           scene.instances[0].visible && scene.instances[1].visible;
  };

  // reparenting failures: cycle, self-parenting, out-of-range indices
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 0, 1, true); }));   // cycle
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 0, 1, false); }));  // cycle
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 1, 1, true); }));   // self
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 1, 7, true); }));   // bad parent
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 7, 0, true); }));   // bad node
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, -1, 0, true); }));  // bad node
  TASK_CHECK(check_unchanged());

  // duplicate instance binding
  scene.nodes[2].instance = 0;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[2].instance = 1;
  // out-of-range parent and instance indices
  scene.nodes[1].parent = 99;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].parent = -2;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].parent = 0;
  scene.nodes[1].instance = 5;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].instance = -2;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].instance = 0;
  // self-parenting and cycles through update_scene_nodes
  scene.nodes[1].parent = 1;
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].parent = 0;
  scene.nodes[0].parent = 1;  // G -> N0 -> G
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[0].parent = -1;
  // non-finite local frame
  scene.nodes[1].local.o.x = std::numeric_limits<float>::quiet_NaN();
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].local.o.x = 0;
  // non-rigid local frames: scaling and reflection
  scene.nodes[1].local.x = {2, 0, 0};
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].local.x = {-1, 0, 0};
  TASK_CHECK(throws_invalid_argument([&]() { update_scene_nodes(scene); }));
  scene.nodes[1].local.x = {1, 0, 0};
  // no partial state was published by any of the failures
  TASK_CHECK(check_unchanged());

  // a failed reparent with keep_world on a broken hierarchy also rolls back
  scene.nodes[1].local.x = {2, 0, 0};
  TASK_CHECK(throws_invalid_argument(
      [&]() { reparent_scene_node(scene, 2, 0, true); }));
  TASK_CHECK(scene.nodes[2].parent == -1 &&
             vec3_near(scene.nodes[2].local.o, {5, 0, 0}));
  scene.nodes[1].local.x = {1, 0, 0};

  // the scene is still fully functional afterwards
  update_scene_nodes(scene);
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 1, 0}));
  TASK_CHECK(vec3_near(scene.instances[1].frame.o, {5, 0, 0}));
  return 0;
}

// -----------------------------------------------------------------------------
// VISIBILITY: hidden ancestors, near-far occlusion, find_any and instance queries
// -----------------------------------------------------------------------------

static int case_hidden_ancestor_occlusion() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));  // 0: near, bound
  scene.instances.push_back(make_instance(0, 0));  // 1: far, bound
  scene.instances.push_back(make_instance(0, 0));  // 2: farther, unbound
  scene.instances[2].frame = translation3d(0, 0, -8);
  scene.nodes = {
      make_node(-1, 0, translation3d(0, 0, -2)),  // 0: near node
      make_node(-1, 1, translation3d(0, 0, -5)),  // 1: far node
  };
  update_scene_nodes(scene);
  auto bvh = make_scene_bvh(scene, false, true);
  auto ray = ray3f{{0, 0, 0}, {0, 0, -1}};

  // all visible: the near instance is hit first
  auto hit = intersect_scene_bvh(bvh, scene, ray);
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 2) < 1e-5f);
  TASK_CHECK(intersect_scene_bvh(bvh, scene, ray, true).hit);

  // hide the near instance through its node: the far one is now hit, both
  // with the closest and the find_any queries, so the hidden near triangle
  // neither is hit nor occludes the visible one behind it
  scene.nodes[0].visible = false;
  update_scene_nodes(scene);
  TASK_CHECK(scene.instances[0].visible == false);
  hit = intersect_scene_bvh(bvh, scene, ray);
  TASK_CHECK(hit.hit && hit.instance == 1 && std::abs(hit.distance - 5) < 1e-5f);
  auto any = intersect_scene_bvh(bvh, scene, ray, true);
  TASK_CHECK(any.hit && any.instance == 1);
  TASK_CHECK(!intersect_instance_bvh(bvh, scene, 0, ray).hit);
  TASK_CHECK(intersect_instance_bvh(bvh, scene, 1, ray).hit);

  // hiding the unbound instance directly keeps the far instance visible
  scene.instances[2].visible = false;
  hit = intersect_scene_bvh(bvh, scene, ray);
  TASK_CHECK(hit.hit && hit.instance == 1);
  // hiding the far node too leaves only the unbound one, still hidden
  scene.nodes[1].visible = false;
  update_scene_nodes(scene);
  TASK_CHECK(!intersect_scene_bvh(bvh, scene, ray).hit);
  TASK_CHECK(!intersect_scene_bvh(bvh, scene, ray, true).hit);

  // showing everything again restores the original closest hit
  scene.nodes[0].visible = true;
  scene.nodes[1].visible = true;
  scene.instances[2].visible = true;
  update_scene_nodes(scene);
  hit = intersect_scene_bvh(bvh, scene, ray);
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 2) < 1e-5f);
  return 0;
}

// -----------------------------------------------------------------------------
// VISIBILITY: hidden instances do not contribute area lights
// -----------------------------------------------------------------------------

static int case_lights_exclude_hidden() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_emissive({2, 1, 0.5f}));
  scene.materials.push_back(make_matte({0.5f, 0.5f, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));  // 0: emissive, bound
  scene.instances.push_back(make_instance(0, 0));  // 1: emissive, bound
  scene.instances.push_back(make_instance(0, 1));  // 2: matte, unbound
  scene.nodes = {
      make_node(-1, 0, translation3d(0, 0, -2)),
      make_node(-1, 1, translation3d(3, 0, -2)),
  };
  auto params = test_params();
  update_scene_nodes(scene);

  // both emissive instances contribute one light each
  auto lights = make_trace_lights(scene, params);
  TASK_CHECK(lights.lights.size() == 2);
  TASK_CHECK(lights.lights[0].instance == 0 && lights.lights[1].instance == 1);

  // hiding one instance removes its light but keeps the other
  scene.nodes[0].visible = false;
  update_scene_nodes(scene);
  lights = make_trace_lights(scene, params);
  TASK_CHECK(lights.lights.size() == 1 && lights.lights[0].instance == 1);

  // and symmetrically for the other instance
  scene.nodes[0].visible = true;
  scene.nodes[1].visible = false;
  update_scene_nodes(scene);
  lights = make_trace_lights(scene, params);
  TASK_CHECK(lights.lights.size() == 1 && lights.lights[0].instance == 0);

  // environments are unaffected by instance visibility
  scene.environments.push_back({});
  scene.environments[0].emission = {1, 1, 1};
  lights = make_trace_lights(scene, params);
  TASK_CHECK(lights.lights.size() == 2);
  TASK_CHECK(lights.lights[1].environment == 0);
  return 0;
}

// -----------------------------------------------------------------------------
// REFRESH: refit preserves bvh topology and resets progressive state
// -----------------------------------------------------------------------------

static int case_update_refit_and_reset() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_emissive({2, 1, 0.5f}));
  scene.instances.push_back(make_instance(0, 0));
  scene.instances.push_back(make_instance(0, 0));  // shares shape 0
  auto group = rotation_z90();
  group.o    = {1, 2, 3};
  scene.nodes = {
      make_node(-1, -1, group),
      make_node(0, 0, translation3d(2, 0, 0)),
      make_node(0, 1, translation3d(-2, 0, 0)),
  };
  scene.cameras.push_back(test_camera());
  update_scene_nodes(scene);
  auto params = test_params();
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);

  // snapshot the bvh topology and dirty the progressive state
  auto scene_topology = snapshot_topology(bvh.bvh.bvh);
  auto shape_topology = snapshot_topology(bvh.bvh.shapes[0].bvh);
  trace_to_completion(state, scene, bvh, lights, params);
  TASK_CHECK(state.samples == params.samples);

  // move the group between frames, without touching the shared shape
  scene.nodes[0].local.o.x += 1;
  auto old_root_bbox = bvh.bvh.bvh.nodes[0].bbox;
  update_trace_scene(scene, bvh, lights, state, params, {});

  // node connectivity, leaf ranges and primitive ordering are preserved
  TASK_CHECK(topology_equal(scene_topology, snapshot_topology(bvh.bvh.bvh)));
  TASK_CHECK(topology_equal(shape_topology, snapshot_topology(bvh.bvh.shapes[0].bvh)));
  // the top-level bounds followed the +1 x translation of the group
  TASK_CHECK(
      std::abs(bvh.bvh.bvh.nodes[0].bbox.min.x - old_root_bbox.min.x - 1) < 1e-5f);
  TASK_CHECK(
      std::abs(bvh.bvh.bvh.nodes[0].bbox.max.x - old_root_bbox.max.x - 1) < 1e-5f);
  TASK_CHECK(bvh.bvh.bvh.nodes[0].bbox.min.y == old_root_bbox.min.y &&
             bvh.bvh.bvh.nodes[0].bbox.min.z == old_root_bbox.min.z);

  // the progressive state matches a state newly created with the same params
  auto fresh = make_trace_state(scene, params);
  TASK_CHECK(state.samples == fresh.samples);
  TASK_CHECK(image_is_zero(state));
  TASK_CHECK(rngs_equal(state, fresh));
  TASK_CHECK(lights.lights.size() == 2);
  TASK_CHECK(std::abs(lights.lights[0].elements_cdf.back() - 2.0f) < 1e-5f);

  // deform the shared shape between frames and dirty the state again
  trace_to_completion(state, scene, bvh, lights, params);
  scene.shapes[0].positions[2].y = 1.5f;
  update_trace_scene(scene, bvh, lights, state, params, {0});

  // the topology is still the original one after the refit
  TASK_CHECK(topology_equal(scene_topology, snapshot_topology(bvh.bvh.bvh)));
  TASK_CHECK(topology_equal(shape_topology, snapshot_topology(bvh.bvh.shapes[0].bvh)));
  TASK_CHECK(state.samples == 0 && image_is_zero(state));

  // the light areas followed the deformed shared triangle (area 2.5)
  TASK_CHECK(lights.lights.size() == 2);
  TASK_CHECK(std::abs(lights.lights[0].elements_cdf.back() - 2.5f) < 1e-5f);
  TASK_CHECK(std::abs(lights.lights[1].elements_cdf.back() - 2.5f) < 1e-5f);

  // intersections see the moved and deformed geometry: the rotated triangle
  // now spans x in [0.5,3] at height y=4 in the z=3 plane
  auto hit = intersect_scene_bvh(bvh.bvh, scene, {{2.25f, 4, 0}, {0, 0, 1}});
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 3) < 1e-5f);
  TASK_CHECK(!intersect_scene_bvh(bvh.bvh, scene, {{0.25f, 4, 0}, {0, 0, 1}}).hit);

  // tracing after the update accumulates only new samples
  trace_to_completion(state, scene, bvh, lights, params);
  TASK_CHECK(state.samples == params.samples);
  return 0;
}

// -----------------------------------------------------------------------------
// REFRESH: traced images match a freshly built tiled reference scene
// -----------------------------------------------------------------------------

static scene_data make_node_render_scene() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_emissive({2, 1, 0.5f}));
  scene.materials.push_back(make_matte({0.2f, 0.2f, 0.2f}));
  scene.instances.push_back(make_instance(0, 0));  // emissive
  scene.instances.push_back(make_instance(0, 1));  // matte
  auto camera       = camera_data{};
  camera.aspect     = 1;
  camera.frame.o    = {0, 0, 5};
  scene.cameras.push_back(camera);
  auto group = rotation_z90();
  group.o    = {0, 0, -3};
  scene.nodes = {
      make_node(-1, -1, group),
      make_node(0, 0, translation3d(1, 0, 0)),
      make_node(0, 1, translation3d(-1, 0, 0)),
  };
  update_scene_nodes(scene);
  return scene;
}

static scene_data make_tiled_render_scene() {
  // explicitly tiled reference: same content with frames set by hand
  auto scene       = make_node_render_scene();
  scene.nodes.clear();
  scene.instances[0].frame = frame3f{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 1, -3}};
  scene.instances[1].frame = frame3f{{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, {0, -1, -3}};
  return scene;
}

static int run_trace_matches_reference(trace_sampler_type sampler) {
  auto params = test_params(sampler);
  auto scene  = make_node_render_scene();
  auto tiled  = make_tiled_render_scene();

  // the node scene traces exactly like the tiled reference
  auto bvh     = make_trace_bvh(scene, params);
  auto lights  = make_trace_lights(scene, params);
  auto state   = make_trace_state(scene, params);
  auto ref_bvh = make_trace_bvh(tiled, params);
  auto ref_lights = make_trace_lights(tiled, params);
  auto ref_state  = make_trace_state(tiled, params);
  trace_to_completion(state, scene, bvh, lights, params);
  trace_to_completion(ref_state, tiled, ref_bvh, ref_lights, params);
  TASK_CHECK(images_equal(state, ref_state));

  // edit the node scene and the tiled reference in the same way
  scene.nodes[0].local.o.x += 1;
  scene.shapes[0].positions[2].y = 1.5f;
  tiled.instances[0].frame.o.x += 1;
  tiled.instances[1].frame.o.x += 1;
  tiled.shapes[0].positions[2].y = 1.5f;

  // update in place and rebuild the reference from scratch
  update_trace_scene(scene, bvh, lights, state, params, {0});
  TASK_CHECK(state.samples == 0 && image_is_zero(state));
  auto ref_bvh2   = make_trace_bvh(tiled, params);
  auto ref_lights2 = make_trace_lights(tiled, params);
  auto ref_state2  = make_trace_state(tiled, params);
  TASK_CHECK(rngs_equal(state, ref_state2));
  TASK_CHECK(lights.lights.size() == ref_lights2.lights.size());

  // the updated scene still traces exactly like the fresh reference, so no
  // stale samples from before the edit were mixed in
  trace_to_completion(state, scene, bvh, lights, params);
  trace_to_completion(ref_state2, tiled, ref_bvh2, ref_lights2, params);
  TASK_CHECK(state.samples == params.samples);
  TASK_CHECK(images_equal(state, ref_state2));
  return 0;
}

static int case_trace_matches_tiled_path() {
  return run_trace_matches_reference(trace_sampler_type::path);
}

static int case_trace_matches_tiled_naive() {
  return run_trace_matches_reference(trace_sampler_type::naive);
}

// -----------------------------------------------------------------------------
// REFRESH: consecutive frames with shared geometry edits and reparenting
// -----------------------------------------------------------------------------

static int case_animated_shared_geometry() {
  auto scene = scene_data{};
  scene.shapes.push_back(make_triangle_shape());
  scene.materials.push_back(make_emissive({2, 1, 0.5f}));
  scene.materials.push_back(make_matte({0.2f, 0.2f, 0.2f}));
  scene.instances.push_back(make_instance(0, 0));  // emissive
  scene.instances.push_back(make_instance(0, 1));  // matte
  scene.nodes = {
      make_node(-1, -1, translation3d(0, 0, -3)),   // 0: G
      make_node(0, 0, translation3d(1, 0, 0)),      // 1: N0 -> instance 0
      make_node(0, 1, translation3d(-1, 0, 0)),     // 2: N1 -> instance 1
      make_node(-1, -1, translation3d(0, 5, 0)),    // 3: GA
  };
  scene.cameras.push_back(test_camera());
  auto params = test_params();
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);

  // frame 0: deformed shared triangle, both instances visible
  scene.shapes[0].positions[2].y = 1.25f;
  update_trace_scene(scene, bvh, lights, state, params, {0});
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 0, -3}));
  TASK_CHECK(vec3_near(scene.instances[1].frame.o, {-1, 0, -3}));
  TASK_CHECK(lights.lights.size() == 1);
  TASK_CHECK(std::abs(lights.lights[0].elements_cdf.back() - 2.25f) < 1e-5f);
  auto hit = intersect_scene_bvh(bvh.bvh, scene, {{1, -0.333f, 0}, {0, 0, -1}});
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 3) < 1e-4f);
  TASK_CHECK(state.samples == 0);
  trace_to_completion(state, scene, bvh, lights, params);
  TASK_CHECK(state.samples == params.samples);

  // frame 1: keep-world reparent under GA, hide the matte instance
  reparent_scene_node(scene, 1, 3, true);
  scene.nodes[2].visible = false;
  scene.shapes[0].positions[2].y = 1.5f;
  update_trace_scene(scene, bvh, lights, state, params, {0});
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, 0, -3}));  // kept world
  TASK_CHECK(scene.instances[1].visible == false);
  TASK_CHECK(lights.lights.size() == 1);
  TASK_CHECK(std::abs(lights.lights[0].elements_cdf.back() - 2.5f) < 1e-5f);
  hit = intersect_scene_bvh(bvh.bvh, scene, {{1, -0.333f, 0}, {0, 0, -1}});
  TASK_CHECK(hit.hit && hit.instance == 0);
  TASK_CHECK(!intersect_scene_bvh(bvh.bvh, scene, {{-1, -0.333f, 0}, {0, 0, -1}}).hit);
  TASK_CHECK(state.samples == 0);
  trace_to_completion(state, scene, bvh, lights, params);

  // frame 2: keep-local reparent back under G, show the matte instance again
  reparent_scene_node(scene, 1, 0, false);
  scene.nodes[2].visible = true;
  scene.shapes[0].positions[2].y = 1.75f;
  update_trace_scene(scene, bvh, lights, state, params, {0});
  // local (1,-5,-3) under G at (0,0,-3) gives world (1,-5,-6)
  TASK_CHECK(vec3_near(scene.nodes[1].local.o, {1, -5, -3}));
  TASK_CHECK(vec3_near(scene.instances[0].frame.o, {1, -5, -6}));
  TASK_CHECK(scene.instances[1].visible == true);
  TASK_CHECK(lights.lights.size() == 1);
  TASK_CHECK(std::abs(lights.lights[0].elements_cdf.back() - 2.75f) < 1e-5f);
  hit = intersect_scene_bvh(bvh.bvh, scene, {{1, -5.333f, 0}, {0, 0, -1}});
  TASK_CHECK(hit.hit && hit.instance == 0 && std::abs(hit.distance - 6) < 1e-4f);
  hit = intersect_scene_bvh(bvh.bvh, scene, {{-1, -0.333f, 0}, {0, 0, -1}});
  TASK_CHECK(hit.hit && hit.instance == 1 && std::abs(hit.distance - 3) < 1e-4f);
  TASK_CHECK(state.samples == 0);
  trace_to_completion(state, scene, bvh, lights, params);
  TASK_CHECK(state.samples == params.samples);
  return 0;
}

// -----------------------------------------------------------------------------
// REFRESH: invalid updates leave every derived structure untouched
// -----------------------------------------------------------------------------

static int case_update_failures_are_atomic() {
  auto scene  = make_node_render_scene();
  auto params = test_params();
  auto bvh    = make_trace_bvh(scene, params);
  auto lights = make_trace_lights(scene, params);
  auto state  = make_trace_state(scene, params);
  trace_to_completion(state, scene, bvh, lights, params);

  auto old_frame   = scene.instances[0].frame;
  auto old_visible = scene.instances[0].visible;
  auto old_bbox    = bvh.bvh.bvh.nodes[0].bbox;
  auto old_lights  = lights.lights.size();
  auto old_samples = state.samples;

  // out-of-range updated shape indices
  TASK_CHECK(throws_invalid_argument(
      [&]() { update_trace_scene(scene, bvh, lights, state, params, {7}); }));
  TASK_CHECK(throws_invalid_argument(
      [&]() { update_trace_scene(scene, bvh, lights, state, params, {-1}); }));
  // invalid hierarchy
  scene.nodes[1].local.x = {2, 0, 0};
  TASK_CHECK(throws_invalid_argument(
      [&]() { update_trace_scene(scene, bvh, lights, state, params, {0}); }));
  scene.nodes[1].local.x = {1, 0, 0};

  // instance state, bvh, lights and trace state are all unchanged
  TASK_CHECK(frame_near(scene.instances[0].frame, old_frame));
  TASK_CHECK(scene.instances[0].visible == old_visible);
  TASK_CHECK(bvh.bvh.bvh.nodes[0].bbox.min == old_bbox.min &&
             bvh.bvh.bvh.nodes[0].bbox.max == old_bbox.max);
  TASK_CHECK(lights.lights.size() == old_lights);
  TASK_CHECK(state.samples == old_samples);
  return 0;
}

// -----------------------------------------------------------------------------
// CASE REGISTRATION
// -----------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"hierarchy", "forest_out_of_order", case_forest_out_of_order},
    {"hierarchy", "shared_instances_tiled", case_shared_instances_tiled},
    {"hierarchy", "reparent_keep_world", case_reparent_keep_world},
    {"hierarchy", "invalid_hierarchies_rollback", case_invalid_hierarchies_rollback},
    {"visibility", "hidden_ancestor_occlusion", case_hidden_ancestor_occlusion},
    {"visibility", "lights_exclude_hidden", case_lights_exclude_hidden},
    {"refresh", "update_refit_and_reset", case_update_refit_and_reset},
    {"refresh", "trace_matches_tiled_path", case_trace_matches_tiled_path},
    {"refresh", "trace_matches_tiled_naive", case_trace_matches_tiled_naive},
    {"refresh", "animated_shared_geometry", case_animated_shared_geometry},
    {"refresh", "update_failures_are_atomic", case_update_failures_are_atomic},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
