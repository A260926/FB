// Incremental preview tests. Registered through task_cases[] and driven
// by task_support/driver.c (no main here).

#include "task_support/check.h"

#include "ray_tracer.hpp"
#include "ray_cache.hpp"
#include "ppm_writer.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

//---------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------

Material MatteMat(uint8_t r, uint8_t g, uint8_t b) {
  Material m;
  m.color = {r, g, b};
  m.specular = 10.0f;
  m.reflective = 0.0f;
  m.transparency = 0.0f;
  m.refractive_index = 1.0f;
  m.tint = 0.1f;
  m.emission = {0, 0, 0};
  m.emit_strength = 0.0f;
  return m;
}

Material MirrorMat(uint8_t r, uint8_t g, uint8_t b, float refl) {
  Material m = MatteMat(r, g, b);
  m.specular = 60.0f;
  m.reflective = refl;
  return m;
}

Material GlassMat(uint8_t r, uint8_t g, uint8_t b, float ior) {
  Material m = MatteMat(r, g, b);
  m.specular = 60.0f;
  m.reflective = 0.15f;
  m.transparency = 0.6f;
  m.refractive_index = ior;
  m.tint = 0.2f;
  return m;
}

AABB MakeDomain(Vec3f mn, Vec3f mx) {
  AABB d;
  d.min = mn;
  d.max = mx;
  return d;
}

bool ImagesEqual(const Image& a, const Image& b) {
  if (a.width != b.width || a.height != b.height) return false;
  if (a.data.size() != b.data.size()) return false;
  for (std::size_t i = 0; i < a.data.size(); ++i)
    if (a.data[i] != b.data[i]) return false;
  return true;
}

std::size_t ChangedPixels(const Image& a, const Image& b) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < a.data.size() && i < b.data.size(); ++i)
    if (a.data[i] != b.data[i]) ++n;
  return n;
}

// owns its camera/lights/tracer so references stay valid
struct World {
  Camera cam;
  Lights lights;
  RayTracer tracer;
  std::vector<Sphere*> spheres;
  World(float depth, float fov, Vec3f center)
      : cam(depth, fov, fov, center), lights(), tracer(cam, lights) {}
  World(const World&) = delete;
  World& operator=(const World&) = delete;
  Sphere* AddSphere(Vec3f c, float r, const Material& m) {
    Sphere* s = &tracer.AddObject<Sphere>(c, r, m);
    spheres.push_back(s);
    return s;
  }
};

using incremental::SphereEdit;

uint64_t TotalReplays(const incremental::FrameStats& st) {
  return st.ray_nodes_replayed + st.shadow_queries_replayed +
         st.probe_queries_replayed;
}

//---------------------------------------------------------------------
// scene: two matte spheres; b starts outside the view
//---------------------------------------------------------------------

struct TwoSphereScene {
  Vec3f a_pos{0, 0, 600};
  float a_r = 100;
  Vec3f b_pos{2000, 0, 600};
  float b_r = 60;
  Material a_mat = MatteMat(200, 80, 60);
  Material b_mat = MatteMat(60, 120, 200);
};

std::unique_ptr<World> BuildTwoSphere(const TwoSphereScene& s) {
  auto w = std::make_unique<World>(8, 90.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.5f);
  w->lights.AddPoint(0.5f, 200, 100, -100);
  w->tracer.SetBackground(10, 20, 30);
  w->AddSphere(s.a_pos, s.a_r, s.a_mat);
  w->AddSphere(s.b_pos, s.b_r, s.b_mat);
  return w;
}

AABB TwoSphereDomain() {
  return MakeDomain({-2500, -2500, -500}, {2500, 2500, 2500});
}

// dependencies: a sphere moves into previously empty (miss) rays
int CaseEntersOldMiss() {
  TwoSphereScene s0;
  auto w = BuildTwoSphere(s0);
  TASK_CHECK(w->tracer.EnableIncremental(TwoSphereDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();

  // a Trace without edits must do no intersection tests and no replays
  w->tracer.Trace(3);
  const auto st0 = w->tracer.IncrementalStats();
  TASK_CHECK(st0.object_tests == 0);
  TASK_CHECK(TotalReplays(st0) == 0);

  // move b into the camera view (previously all misses there)
  TwoSphereScene s1 = s0;
  s1.b_pos = {40, 20, 550};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.b_pos, s1.b_r, s1.b_mat}}));
  w->tracer.Trace(3);
  const auto st1 = w->tracer.IncrementalStats();
  TASK_CHECK(st1.ray_nodes_replayed > 0);

  auto cold = BuildTwoSphere(s1);
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));
  return 0;
}

// dependencies: a sphere leaves the old first hit of another sphere
int CaseLeavesOldFirstHit() {
  TwoSphereScene s0;
  s0.b_pos = {0, 0, 300};  // b occludes a for the central pixels
  auto w = BuildTwoSphere(s0);
  TASK_CHECK(w->tracer.EnableIncremental(TwoSphereDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();
  TASK_CHECK(w->tracer.IncrementalStats().created_nodes > 0);

  // move b out of the view: central pixels must now hit a
  TwoSphereScene s1 = s0;
  s1.b_pos = {1800, 0, 300};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.b_pos, s1.b_r, s1.b_mat}}));
  w->tracer.Trace(3);
  TASK_CHECK(w->tracer.IncrementalStats().ray_nodes_replayed > 0);

  auto cold = BuildTwoSphere(s1);
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));
  return 0;
}

//---------------------------------------------------------------------
// scene: occluder between a matte sphere and the point light
//---------------------------------------------------------------------

struct OccluderScene {
  Vec3f s_pos{0, 0, 600};
  float s_r = 100;
  Vec3f o_pos{350, 0, 180};
  float o_r = 50;
};

std::unique_ptr<World> BuildOccluder(const OccluderScene& s,
                                     const Material& surface_mat) {
  auto w = std::make_unique<World>(22, 40.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.3f);
  w->lights.AddPoint(0.7f, 500, 0, 0);
  w->tracer.SetBackground(8, 10, 14);
  w->AddSphere(s.s_pos, s.s_r, surface_mat);
  w->AddSphere(s.o_pos, s.o_r, MatteMat(40, 40, 90));
  return w;
}

AABB OccluderDomain() {
  return MakeDomain({-700, -700, -300}, {1000, 700, 800});
}

// dependencies: the occluder moves behind / in front of the point light
int CaseLightFrontBack() {
  OccluderScene s0;
  auto w = BuildOccluder(s0, MatteMat(200, 170, 60));
  TASK_CHECK(w->tracer.EnableIncremental(OccluderDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();

  // move the occluder behind the light: the shadow must disappear
  OccluderScene s1 = s0;
  s1.o_pos = {500, 0, -200};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.o_pos, s1.o_r, w->spheres[1]->material}}));
  w->tracer.Trace(3);
  const auto st1 = w->tracer.IncrementalStats();
  TASK_CHECK(st1.shadow_queries_replayed > 0);

  auto cold1 = BuildOccluder(s1, MatteMat(200, 170, 60));
  cold1->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold1->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));

  // move it back in front of the light: the shadow must come back
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s0.o_pos, s0.o_r, w->spheres[1]->material}}));
  w->tracer.Trace(3);
  TASK_CHECK(w->tracer.IncrementalStats().shadow_queries_replayed > 0);
  auto cold2 = BuildOccluder(s0, MatteMat(200, 170, 60));
  cold2->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold2->tracer.image()));
  TASK_CHECK(ImagesEqual(before, w->tracer.image()));
  return 0;
}

//---------------------------------------------------------------------
// replay: material-only edit of an object seen directly and in a mirror
//---------------------------------------------------------------------

struct MirrorScene {
  Vec3f m_pos{0, 0, 600};
  float m_r = 100;
  Vec3f d_pos{0, 150, 400};
  float d_r = 40;
  Material d_mat = MatteMat(200, 50, 50);
};

std::unique_ptr<World> BuildMirror(const MirrorScene& s) {
  auto w = std::make_unique<World>(22, 40.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.5f);
  w->lights.AddPoint(0.5f, 300, 200, -100);
  w->tracer.SetBackground(12, 14, 18);
  w->AddSphere(s.m_pos, s.m_r, MirrorMat(220, 220, 220, 0.7f));
  w->AddSphere(s.d_pos, s.d_r, s.d_mat);
  return w;
}

AABB MirrorDomain() { return MakeDomain({-800, -800, -300}, {800, 800, 900}); }

int CaseMirrorMaterialOnly() {
  MirrorScene s0;
  auto w = BuildMirror(s0);
  TASK_CHECK(w->tracer.EnableIncremental(MirrorDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();
  const uint64_t root_center = w->tracer.IncrementalRootId(8, 8);
  TASK_CHECK(root_center != 0);

  // recolor the small sphere (geometry untouched)
  MirrorScene s1 = s0;
  s1.d_mat = MatteMat(50, 200, 80);
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.d_pos, s1.d_r, s1.d_mat}}));
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  // material-only edit: no ray re-traces and no new nodes at all
  TASK_CHECK(st.ray_nodes_replayed == 0);
  TASK_CHECK(st.created_nodes == 0);
  TASK_CHECK(w->tracer.IncrementalRootId(8, 8) == root_center);

  auto cold = BuildMirror(s1);
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));
  return 0;
}

// replay: only the shadow line changes; mirror children must be kept
int CaseShadowLineOnly() {
  OccluderScene s0;
  auto w = BuildOccluder(s0, MirrorMat(220, 220, 220, 0.7f));
  TASK_CHECK(w->tracer.EnableIncremental(OccluderDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();

  // snapshot root ids and reflection child ids of all pixels
  const unsigned H = w->cam.height(), W = w->cam.width();
  std::vector<uint64_t> roots0(H * W), refl0(H * W);
  for (unsigned r = 0; r < H; ++r)
    for (unsigned c = 0; c < W; ++c) {
      const uint64_t id = w->tracer.IncrementalRootId(r, c);
      roots0[r * W + c] = id;
      incremental::NodeInfo info;
      TASK_CHECK(w->tracer.IncrementalNodeInfo(id, info));
      refl0[r * W + c] = info.reflection_child;
    }

  // move the occluder out of the light path (geometry of the mirror
  // surface itself is untouched)
  OccluderScene s1 = s0;
  s1.o_pos = {900, 0, 180};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.o_pos, s1.o_r, w->spheres[1]->material}}));
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  TASK_CHECK(st.shadow_queries_replayed > 0);

  auto cold = BuildOccluder(s1, MirrorMat(220, 220, 220, 0.7f));
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(ChangedPixels(before, w->tracer.image()) > 0);

  // for every pixel whose color changed, the root and its reflection
  // child must have kept their identity (no subtree re-trace)
  int checked = 0;
  for (unsigned r = 0; r < H; ++r)
    for (unsigned c = 0; c < W; ++c) {
      if (before.at(r, c) == w->tracer.image().at(r, c)) continue;
      ++checked;
      TASK_CHECK(w->tracer.IncrementalRootId(r, c) == roots0[r * W + c]);
      incremental::NodeInfo info;
      TASK_CHECK(w->tracer.IncrementalNodeInfo(roots0[r * W + c], info));
      TASK_CHECK(info.alive);
      TASK_CHECK(info.reflection_child == refl0[r * W + c]);
    }
  TASK_CHECK(checked > 0);
  return 0;
}

//---------------------------------------------------------------------
// replay: probe ownership change inside nested transparent spheres
//---------------------------------------------------------------------

struct NestedScene {
  Vec3f outer_pos{0, 0, 600};
  float outer_r = 200;
  Vec3f inner_pos{0, 0, 600};
  float inner_r = 80;
};

std::unique_ptr<World> BuildNested(const NestedScene& s) {
  auto w = std::make_unique<World>(22, 40.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.5f);
  w->lights.AddPoint(0.5f, 300, 200, -100);
  w->tracer.SetBackground(30, 34, 40);
  w->AddSphere(s.outer_pos, s.outer_r, GlassMat(255, 255, 255, 1.5f));
  w->AddSphere(s.inner_pos, s.inner_r, GlassMat(255, 255, 255, 1.5f));
  return w;
}

AABB NestedDomain() {
  return MakeDomain({-700, -700, -300}, {700, 700, 900});
}

int CaseNestedProbeOwnership() {
  NestedScene s0;
  auto w = BuildNested(s0);
  TASK_CHECK(w->tracer.EnableIncremental(NestedDomain()));
  w->tracer.Trace(4);
  const Image before = w->tracer.image();

  // move the inner sphere so that some exit probes leave the outer
  // sphere's volume (the surrounding medium changes for those nodes)
  NestedScene s1 = s0;
  s1.inner_pos = {150, 0, 600};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], s1.inner_pos, s1.inner_r,
        w->spheres[1]->material}}));
  w->tracer.Trace(4);
  const auto st = w->tracer.IncrementalStats();
  TASK_CHECK(st.probe_queries_replayed > 0);

  auto cold = BuildNested(s1);
  cold->tracer.Trace(4);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));
  return 0;
}

//---------------------------------------------------------------------
// lifecycle: merged batches, atomic rejection, no stale dirty state
//---------------------------------------------------------------------

struct TrioScene {
  Vec3f a_pos{0, 0, 600};
  Vec3f b_pos{250, 0, 600};
  Vec3f c_pos{-250, 0, 600};
};

std::unique_ptr<World> BuildTrio(const TrioScene& s, const Material& a_mat,
                                 const Material& b_mat,
                                 const Material& c_mat) {
  auto w = std::make_unique<World>(22, 40.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.5f);
  w->lights.AddPoint(0.5f, 200, 100, -100);
  w->tracer.SetBackground(10, 12, 16);
  w->AddSphere(s.a_pos, 80, a_mat);
  w->AddSphere(s.b_pos, 60, b_mat);
  w->AddSphere(s.c_pos, 60, c_mat);
  return w;
}

AABB TrioDomain() {
  return MakeDomain({-800, -800, -300}, {800, 800, 900});
}

int CaseMergedBatchesAndRejection() {
  const Material mat_a = MatteMat(200, 80, 60);
  const Material mat_b = MatteMat(60, 120, 200);
  const Material mat_c = MirrorMat(220, 220, 220, 0.5f);
  TrioScene s0;
  auto w = BuildTrio(s0, mat_a, mat_b, mat_c);
  TASK_CHECK(w->tracer.EnableIncremental(TrioDomain()));
  w->tracer.Trace(3);

  // two edit batches merge before the next Trace
  TASK_CHECK(w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 600},
                                          80, mat_a}}));
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], {250, -80, 600}, 60, mat_b},
       {w->spheres[2], {-250, 0, 600}, 60, MatteMat(220, 160, 40)}}));

  // rejected batches must not change geometry, image or cache
  Material bad = mat_a;
  bad.reflective = 1.5f;  // out of [0,1]
  TASK_CHECK(!w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 600},
                                           80, bad}}));
  TASK_CHECK(!w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 600},
                                           -5, mat_a}}));  // bad radius
  TASK_CHECK(!w->tracer.ApplySphereEdits(
      {{w->spheres[0], {0, 80, 600}, 80, mat_a},
       {w->spheres[0], {0, 80, 600}, 80, mat_a}}));  // duplicate
  Sphere foreign({0, 0, 600}, 80, mat_a);  // not registered
  TASK_CHECK(!w->tracer.ApplySphereEdits({{&foreign, {0, 80, 600}, 80,
                                           mat_a}}));
  TASK_CHECK(!w->tracer.ApplySphereEdits(
      {{w->spheres[0], {5000, 0, 600}, 80, mat_a}}));  // outside domain
  bad = mat_a;
  bad.emit_strength = 1.0f;  // halo not allowed
  TASK_CHECK(!w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 600},
                                           80, bad}}));
  // the rejected batches changed nothing
  TASK_CHECK(w->spheres[0]->center.x == 0 &&
             w->spheres[0]->center.y == 80 &&
             w->spheres[0]->center.z == 600);
  TASK_CHECK(w->spheres[0]->radius == 80);

  // a legal edit afterwards applies cleanly
  TASK_CHECK(w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 550},
                                          80, mat_a}}));
  w->tracer.Trace(3);

  TrioScene s1;
  s1.a_pos = {0, 80, 550};
  s1.b_pos = {250, -80, 600};
  auto cold = BuildTrio(s1, mat_a, mat_b, MatteMat(220, 160, 40));
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));

  // no stale dirty state: an edit-free Trace does nothing
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  TASK_CHECK(st.object_tests == 0);
  TASK_CHECK(TotalReplays(st) == 0);

  // moving a sphere away and back to its exact state merges to a no-op
  const Image keep = w->tracer.image();
  TASK_CHECK(w->tracer.ApplySphereEdits({{w->spheres[0], {0, -200, 600},
                                          80, mat_a}}));
  TASK_CHECK(w->tracer.ApplySphereEdits({{w->spheres[0], {0, 80, 550},
                                          80, mat_a}}));
  w->tracer.Trace(3);
  const auto st2 = w->tracer.IncrementalStats();
  TASK_CHECK(TotalReplays(st2) == 0);
  TASK_CHECK(ImagesEqual(keep, w->tracer.image()));
  return 0;
}

// lifecycle: camera changes trigger a full rebuild
int CaseCameraChangeRebuilds() {
  TwoSphereScene s0;
  s0.b_pos = {200, 0, 600};
  auto w = BuildTwoSphere(s0);
  TASK_CHECK(w->tracer.EnableIncremental(TwoSphereDomain()));
  w->tracer.Trace(3);
  const std::size_t live0 = w->tracer.IncrementalNodes().size();
  const uint64_t root0 = w->tracer.IncrementalRootId(0, 0);
  TASK_CHECK(live0 > 0);
  TASK_CHECK(root0 != 0);

  w->cam.Translate({60, 0, 0});
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  // the whole cache was rebuilt: every old node retired, a full new set
  // of nodes created, and the pixel roots got fresh ids
  TASK_CHECK(st.retired_nodes == live0);
  TASK_CHECK(st.created_nodes == live0);
  TASK_CHECK(w->tracer.IncrementalRootId(0, 0) != root0);

  auto cold = BuildTwoSphere(s0);
  cold->cam.Translate({60, 0, 0});
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  return 0;
}

// lifecycle: adding an object triggers a full rebuild
int CaseAddObjectRebuilds() {
  TwoSphereScene s0;
  s0.b_pos = {200, 0, 600};
  auto w = BuildTwoSphere(s0);
  TASK_CHECK(w->tracer.EnableIncremental(TwoSphereDomain()));
  w->tracer.Trace(3);
  const std::size_t live0 = w->tracer.IncrementalNodes().size();
  const uint64_t root0 = w->tracer.IncrementalRootId(0, 0);

  w->AddSphere({0, 150, 500}, 70, MatteMat(90, 200, 120));
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  TASK_CHECK(st.retired_nodes == live0);
  // a full rebuild records exactly the nodes that are now live
  TASK_CHECK(st.created_nodes == w->tracer.IncrementalNodes().size());
  TASK_CHECK(w->tracer.IncrementalRootId(0, 0) != root0);

  auto cold = BuildTwoSphere(s0);
  cold->AddSphere({0, 150, 500}, 70, MatteMat(90, 200, 120));
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  return 0;
}

// lifecycle: two tracers alternate, each with its own cache
int CaseTwoTracersAlternate() {
  TwoSphereScene s0;
  s0.b_pos = {300, 0, 600};
  auto w1 = BuildTwoSphere(s0);
  auto w2 = BuildTwoSphere(s0);
  TASK_CHECK(w1->tracer.EnableIncremental(TwoSphereDomain()));
  TASK_CHECK(w2->tracer.EnableIncremental(TwoSphereDomain()));
  w1->tracer.Trace(3);
  w2->tracer.Trace(3);
  const Image w2_before = w2->tracer.image();

  // edit only the first tracer's scene
  TwoSphereScene s1 = s0;
  s1.b_pos = {300, 150, 550};
  TASK_CHECK(w1->tracer.ApplySphereEdits(
      {{w1->spheres[1], s1.b_pos, s1.b_r, s1.b_mat}}));
  w1->tracer.Trace(3);
  TASK_CHECK(w1->tracer.IncrementalStats().ray_nodes_replayed > 0);

  // the second tracer sees no edits and does no work
  w2->tracer.Trace(3);
  const auto st2 = w2->tracer.IncrementalStats();
  TASK_CHECK(st2.object_tests == 0);
  TASK_CHECK(TotalReplays(st2) == 0);
  TASK_CHECK(ImagesEqual(w2_before, w2->tracer.image()));

  // each matches the cold trace of its own scene
  auto cold1 = BuildTwoSphere(s1);
  cold1->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w1->tracer.image(), cold1->tracer.image()));
  auto cold0 = BuildTwoSphere(s0);
  cold0->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w2->tracer.image(), cold0->tracer.image()));
  return 0;
}

// lifecycle: record identity - siblings and prefixes keep their ids
int CaseNodeIdentity() {
  auto make = [](const Vec3f& back_pos) {
    auto w = std::make_unique<World>(22, 40.0f, Vec3f{0, 0, -400});
    w->lights.AddAmbient(0.5f);
    w->lights.AddPoint(0.5f, 300, 200, -100);
    w->tracer.SetBackground(14, 16, 20);
    Material both = GlassMat(230, 230, 230, 1.3f);
    both.reflective = 0.4f;
    both.transparency = 0.5f;
    w->AddSphere({0, 0, 600}, 100, both);             // mirror + glass
    w->AddSphere(back_pos, 40, MatteMat(230, 120, 40));  // behind it
    return w;
  };
  auto w = make(Vec3f{0, 0, 900});
  TASK_CHECK(w->tracer.EnableIncremental(
      MakeDomain({-800, -800, -300}, {800, 800, 1100})));
  w->tracer.Trace(4);
  const Image before = w->tracer.image();

  // find a pixel where the root has both children and the refraction
  // grandchild (ray that left the main sphere) hits the backdrop
  // sphere (object slot 1)
  uint64_t root = 0;
  uint64_t refl_child = 0;
  uint64_t refr_child = 0;
  uint64_t refr_grandchild = 0;
  bool found = false;
  for (unsigned r = 0; r < w->cam.height() && !found; ++r)
    for (unsigned c = 0; c < w->cam.width() && !found; ++c) {
      const uint64_t id = w->tracer.IncrementalRootId(r, c);
      if (!id) continue;
      incremental::NodeInfo info;
      TASK_CHECK(w->tracer.IncrementalNodeInfo(id, info));
      if (!info.reflection_child || !info.refraction_child) continue;
      incremental::NodeInfo child;
      TASK_CHECK(w->tracer.IncrementalNodeInfo(info.refraction_child,
                                               child));
      if (!child.refraction_child) continue;
      incremental::NodeInfo grandchild;
      TASK_CHECK(w->tracer.IncrementalNodeInfo(child.refraction_child,
                                               grandchild));
      if (grandchild.object_slot != 1) continue;
      root = id;
      refl_child = info.reflection_child;
      refr_child = info.refraction_child;
      refr_grandchild = child.refraction_child;
      found = true;
    }
  TASK_CHECK(found);

  // move the backdrop sphere: only the refraction path crosses it
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], {60, 0, 900}, 40, w->spheres[1]->material}}));
  w->tracer.Trace(4);
  TASK_CHECK(w->tracer.IncrementalStats().ray_nodes_replayed > 0);

  // the prefix (root and inner refraction node) and the reflection
  // sibling kept their identity
  incremental::NodeInfo info;
  TASK_CHECK(w->tracer.IncrementalNodeInfo(root, info));
  TASK_CHECK(info.alive);
  TASK_CHECK(info.reflection_child == refl_child);
  TASK_CHECK(info.refraction_child == refr_child);
  // the refraction grandchild (which hit the backdrop) was replaced and
  // its old id retired for good
  incremental::NodeInfo child_info;
  TASK_CHECK(w->tracer.IncrementalNodeInfo(refr_child, child_info));
  TASK_CHECK(child_info.alive);
  TASK_CHECK(child_info.refraction_child != refr_grandchild);
  // retired ids are never reused: the old grandchild is dead and no
  // live node recycled its id
  std::size_t live_with_old_id = 0;
  for (const auto& n : w->tracer.IncrementalNodes())
    if (n.id == refr_grandchild) ++live_with_old_id;
  TASK_CHECK(live_with_old_id == 0);
  incremental::NodeInfo old_info;
  if (w->tracer.IncrementalNodeInfo(refr_grandchild, old_info))
    TASK_CHECK(!old_info.alive);

  auto cold = make(Vec3f{60, 0, 900});
  cold->tracer.Trace(4);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  TASK_CHECK(!ImagesEqual(before, w->tracer.image()));
  return 0;
}

//---------------------------------------------------------------------
// render: 64x64 wall workload
//---------------------------------------------------------------------

struct WallScene {
  Vec3f s_pos{300, 200, 800};
  float s_r = 50;
};

std::unique_ptr<World> BuildWall(const WallScene& s) {
  auto w = std::make_unique<World>(32, 90.0f, Vec3f{0, 0, -400});
  w->lights.AddAmbient(0.3f);
  w->lights.AddPoint(0.7f, 0, 0, -300);  // light in front of the wall
  w->tracer.SetBackground(5, 6, 8);
  Material wall_mat = MatteMat(120, 130, 140);
  wall_mat.specular = 0;
  // opaque quad blocking every primary ray
  w->tracer.AddObject<Quad>(Vec3f{-1000, -1000, 500},
                            Vec3f{-1000, 1000, 500},
                            Vec3f{1000, 1000, 500},
                            Vec3f{1000, -1000, 500}, wall_mat);
  w->AddSphere(s.s_pos, s.s_r, MatteMat(200, 60, 60));
  return w;
}

AABB WallDomain() {
  return MakeDomain({-1500, -1500, -600}, {1500, 1500, 1500});
}

int CaseWallHiddenEdit() {
  WallScene s0;
  auto w = BuildWall(s0);
  TASK_CHECK(w->tracer.EnableIncremental(WallDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();
  const std::size_t live = w->tracer.IncrementalNodes().size();
  TASK_CHECK(live == 64 * 64);  // matte wall: one root node per pixel

  // move the hidden sphere behind the wall: nothing may be replayed
  WallScene s1 = s0;
  s1.s_pos = {500, 400, 900};
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[0], s1.s_pos, s1.s_r, w->spheres[0]->material}}));
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  TASK_CHECK(st.ray_nodes_replayed == 0);
  TASK_CHECK(st.shadow_queries_replayed == 0);
  TASK_CHECK(st.probe_queries_replayed == 0);
  TASK_CHECK(st.object_tests == 0);
  // the reverse index only visited a tiny fraction of the live records
  TASK_CHECK(st.index_nodes_visited * 8 < live);
  TASK_CHECK(st.pixel_reductions == 64 * 64);
  TASK_CHECK(ImagesEqual(before, w->tracer.image()));

  auto cold = BuildWall(s1);
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  return 0;
}

int CaseWallFrontEdit() {
  WallScene s0;
  auto w = BuildWall(s0);
  TASK_CHECK(w->tracer.EnableIncremental(WallDomain()));
  w->tracer.Trace(3);
  const Image before = w->tracer.image();
  const unsigned H = w->cam.height(), W = w->cam.width();
  std::vector<uint64_t> roots0(H * W);
  for (unsigned r = 0; r < H; ++r)
    for (unsigned c = 0; c < W; ++c) roots0[r * W + c] =
        w->tracer.IncrementalRootId(r, c);

  // move the sphere in front of the wall, covering a local area
  WallScene s1 = s0;
  s1.s_pos = {100, 50, 100};
  s1.s_r = 60;
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[0], s1.s_pos, s1.s_r, w->spheres[0]->material}}));
  w->tracer.Trace(3);
  const auto st = w->tracer.IncrementalStats();
  // the image really changed, with bounded replay work
  TASK_CHECK(ChangedPixels(before, w->tracer.image()) > 0);
  TASK_CHECK(st.ray_nodes_replayed + st.shadow_queries_replayed < 512);

  // unaffected pixels kept their root ids
  std::size_t preserved = 0;
  for (unsigned r = 0; r < H; ++r)
    for (unsigned c = 0; c < W; ++c)
      if (w->tracer.IncrementalRootId(r, c) == roots0[r * W + c])
        ++preserved;
  TASK_CHECK(preserved > 64 * 64 - 512);

  auto cold = BuildWall(s1);
  cold->tracer.Trace(3);
  TASK_CHECK(ImagesEqual(w->tracer.image(), cold->tracer.image()));
  return 0;
}

// render: the example flow - real images on disk plus real stats
int CaseExampleImages() {
  auto make = [] {
    auto w = std::make_unique<World>(64, 60.0f, Vec3f{0, 0, -400});
    w->lights.AddAmbient(0.4f);
    w->lights.AddPoint(0.6f, 300, 200, -100);
    w->tracer.SetBackground(18, 22, 30);
    Material matte = MatteMat(200, 90, 60);
    matte.specular = 20;
    Material mirror = MirrorMat(230, 230, 230, 0.6f);
    mirror.specular = 80;
    Material glass = GlassMat(240, 250, 255, 1.4f);
    w->AddSphere({-160, 0, 700}, 120, matte);
    w->AddSphere({180, -40, 500}, 90, mirror);
    w->AddSphere({0, 120, 900}, 140, glass);
    return w;
  };
  auto w = make();
  TASK_CHECK(w->tracer.EnableIncremental(
      MakeDomain({-1200, -1200, -300}, {1200, 1200, 1400})));
  w->tracer.Trace(4);
  const auto cold_stats = w->tracer.IncrementalStats();
  TASK_CHECK(cold_stats.created_nodes > 0);
  TASK_CHECK(cold_stats.object_tests > 0);

  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "incremental_example";
  std::error_code ec;
  fs::create_directories(dir, ec);
  TASK_CHECK(!ec);
  const std::string before_path = (dir / "before.ppm").string();
  const std::string after_path = (dir / "after.ppm").string();
  Ppm::Write(w->tracer.image(), before_path);

  Material glass2 = GlassMat(255, 200, 220, 1.4f);
  TASK_CHECK(w->tracer.ApplySphereEdits(
      {{w->spheres[1], {60, -20, 500}, 90, w->spheres[1]->material},
       {w->spheres[2], {0, 120, 900}, 140, glass2}}));
  w->tracer.Trace(4);
  Ppm::Write(w->tracer.image(), after_path);
  const auto st = w->tracer.IncrementalStats();
  // real incremental work happened
  TASK_CHECK(st.ray_nodes_replayed > 0);
  TASK_CHECK(st.pixel_reductions > 0);

  // read this run's images back and verify them
  Image before_img, after_img;
  Ppm::Read(before_img, before_path);
  Ppm::Read(after_img, after_path);
  TASK_CHECK(before_img.width == w->cam.width() &&
             before_img.height == w->cam.height());
  TASK_CHECK(after_img.width == w->cam.width() &&
             after_img.height == w->cam.height());
  TASK_CHECK(ChangedPixels(before_img, after_img) > 0);
  TASK_CHECK(ImagesEqual(after_img, w->tracer.image()));
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"dependencies", "enters_old_miss", CaseEntersOldMiss},
    {"dependencies", "leaves_old_first_hit", CaseLeavesOldFirstHit},
    {"dependencies", "light_front_back_move", CaseLightFrontBack},
    {"replay", "mirror_material_only", CaseMirrorMaterialOnly},
    {"replay", "shadow_line_only", CaseShadowLineOnly},
    {"replay", "nested_probe_ownership", CaseNestedProbeOwnership},
    {"lifecycle", "merged_batches_and_rejection",
     CaseMergedBatchesAndRejection},
    {"lifecycle", "camera_change_rebuilds", CaseCameraChangeRebuilds},
    {"lifecycle", "add_object_rebuilds", CaseAddObjectRebuilds},
    {"lifecycle", "two_tracers_alternate", CaseTwoTracersAlternate},
    {"lifecycle", "node_identity", CaseNodeIdentity},
    {"render", "wall_hidden_edit", CaseWallHiddenEdit},
    {"render", "wall_front_edit", CaseWallFrontEdit},
    {"render", "example_images", CaseExampleImages},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
