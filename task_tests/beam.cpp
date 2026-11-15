// Beam traversal tests.
//
// Four groups are registered:
//   frusta    - conservative cone/box behaviour on boundary cases
//   traversal - dual traversal vs. an independent per-pixel brute force
//   updates   - refit / rebuild / alternating tracer state
//   render    - full Trace()+PPM comparisons, hidden geometry, stats

#include "check.h"

#include "ray_tracer.hpp"
#include "ppm_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

// ---------------------------------------------------------------- utils
bool Near(float a, float b, float tol = 2e-4f) {
  if (std::isinf(a) || std::isinf(b)) return a == b;
  return std::abs(a - b) <= tol * (1.0f + std::abs(a) + std::abs(b));
}

bool SameColor(Vec3u8 a, Vec3u8 b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

Material Plain(uint8_t r, uint8_t g, uint8_t b) {
  Material m;
  m.color = Vec3u8{r, g, b};
  m.specular = 0.0f;
  m.reflective = 0.0f;
  m.transparency = 0.0f;
  return m;
}

// A reference nearest hit: independent of the beam code, a plain scan
// over the objects in AddObject order using only the Object API.
struct RefHit {
  bool hit{false};
  float t{std::numeric_limits<float>::infinity()};
  const Object* obj{nullptr};
  std::size_t slot{kNoSlot};
  HitRecord record{};
};

RefHit BruteForce(const std::vector<Object*>& objects,
                  const Camera& camera, std::size_t row, std::size_t col) {
  const std::size_t w = camera.width();
  const std::size_t h = camera.height();
  auto corners = camera.CornersWorld();
  const Vec3f span_h = corners[1] - corners[0];
  const Vec3f span_v = corners[2] - corners[0];
  const float u = static_cast<float>(col) / static_cast<float>(w - 1);
  const float v = static_cast<float>(row) / static_cast<float>(h - 1);
  const Vec3f p = corners[0] + span_h * u + span_v * v;
  Ray ray(camera.center(), p);

  RefHit best;
  for (std::size_t slot = 0; slot < objects.size(); ++slot) {
    const HitRecord hrec = objects[slot]->Intersects(ray);
    if (!hrec.is_hit || !(hrec.t > 0.0f) || !std::isfinite(hrec.t)) continue;
    if (!best.hit || hrec.t < best.t ||
        (hrec.t == best.t && slot < best.slot)) {
      best.hit = true;
      best.t = hrec.t;
      best.obj = objects[slot];
      best.slot = slot;
      best.record = hrec;
    }
  }
  return best;
}

bool HitsAgree(const beam::Hit& got, const RefHit& ref) {
  if (got.IsHit() != ref.hit) return false;
  if (!ref.hit) return got.slot == kNoSlot;
  if (got.slot != ref.slot) return false;
  if (got.object != ref.obj) return false;
  if (!Near(got.record.t, ref.t)) return false;
  return Near(got.record.where.x, ref.record.where.x) &&
         Near(got.record.where.y, ref.record.where.y) &&
         Near(got.record.where.z, ref.record.where.z);
}

Camera MakeCamera(std::size_t w, std::size_t h, Vec3f center = {0, 0, 0},
                  Mat3x3 rot = Mat3x3{}, float depth = 5.0f) {
  // Camera derives the plane size from tan(fov/2); pick a fov whose
  // plane size rounds to exactly w x h.
  const float fovx = 2.0f * std::atan(static_cast<float>(w) /
                                     (2.0f * depth)) * 180.0f /
                     static_cast<float>(M_PI) + 1e-3f;
  const float fovy = 2.0f * std::atan(static_cast<float>(h) /
                                     (2.0f * depth)) * 180.0f /
                     static_cast<float>(M_PI) + 1e-3f;
  return Camera(depth, fovx, fovy, center, rot);
}

struct Scene {
  std::vector<std::unique_ptr<Object>> objects;
  template <typename T, typename... Args>
  T& Add(Args&&... args) {
    objects.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    return static_cast<T&>(*objects.back());
  }
};

// Verify the whole pixel grid of one tracer against BruteForce.
// `objects` must be in AddObject slot order.
bool VerifyGrid(const std::vector<Object*>& objects,
                const Camera& camera, const beam::BeamTraversal& beam) {
  for (std::size_t row = 0; row < camera.height(); ++row) {
    for (std::size_t col = 0; col < camera.width(); ++col) {
      const RefHit ref = BruteForce(objects, camera, row, col);
      if (!HitsAgree(beam.hit(row, col), ref)) {
        return false;
      }
    }
  }
  return true;
}

// Verify against the tracer's live object list.
bool VerifyTracer(const RayTracer& tracer, const Camera& camera) {
  std::vector<Object*> objects;
  const auto& view = tracer.Beam().objects_view();
  objects.assign(view.begin(), view.end());
  return VerifyGrid(objects, camera, tracer.Beam());
}

}  // namespace

// ================================================================ frusta
namespace {

// A single-pixel / one-row / one-column region must never lose a box
// that a real ray of that region hits.  We build the same scene twice
// and compare against brute force over odd image sizes.
int FrustaDegenerateRegions() {
  Camera camera = MakeCamera(7, 5);
  Scene scene;
  Material m = Plain(120, 180, 60);
  // small targets, including ones exactly on the horizontal and
  // vertical centre rows/columns
  scene.Add<Sphere>(Vec3f{0, 0, 12}, 0.35f, m);
  scene.Add<Sphere>(Vec3f{2.2f, 0.0f, 14}, 0.3f, m);
  scene.Add<Sphere>(Vec3f{-2.2f, 0.0f, 14}, 0.3f, m);
  scene.Add<Sphere>(Vec3f{0, 1.8f, 16}, 0.3f, m);
  scene.Add<Quad>(Vec3f{-30, -30, 30}, Vec3f{30, -30, 30},
                  Vec3f{30, 30, 30}, Vec3f{-30, 30, 30}, m);

  Lights lights;
  lights.AddAmbient(1.0f);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  for (auto& o : scene.objects) {
    // rebuild through AddObject so slots match
    if (auto* s = dynamic_cast<Sphere*>(o.get())) {
      tracer.AddObject<Sphere>(s->center, s->radius, s->material);
    } else if (auto* q = dynamic_cast<Quad*>(o.get())) {
      tracer.AddObject<Quad>(q->v0, q->v1, q->v2, q->v3, q->material);
    }
  }
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  TASK_CHECK(VerifyTracer(tracer, camera));
  return 0;
}

// Boxes that cross the eye plane, contain the camera, or are nearly
// axis-parallel must be handled conservatively.
int FrustaCrossingAndInside() {
  Camera camera = MakeCamera(9, 7);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(200, 100, 40);

  RayTracer plain(camera, lights);
  RayTracer beam(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(0, 0, 0);
    // A block enclosing the camera: every ray starts inside it.
    rt.AddObject<Block>(Vec3f{0, 0, 0}, 3, 3, 3, Mat3x3{}, m);
    // A smaller sphere so something is actually visible.
    rt.AddObject<Sphere>(Vec3f{0.4f, 0.2f, 2.0f}, 0.6f, m);
    // A long, thin axis-aligned block far away.
    rt.AddObject<Block>(Vec3f{0, 0, 18}, 6, 0.2f, 0.2f, Mat3x3{}, m);
  };
  build(plain);
  build(beam);
  plain.SetBeamTraversal(false);
  beam.SetBeamTraversal(true);
  plain.Trace(0);
  beam.Trace(0);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(SameColor(plain.image().at(y, x), beam.image().at(y, x)));
    }
  }
  return 0;
}

// Unknown (default) bounds must never be culled: a pixel whose only
// object has an empty box still has to be tested and hit.
int FrustaUnknownBounds() {
  Camera camera = MakeCamera(6, 5);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(10, 220, 30);

  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  // A sphere then cleared to unknown bounds.  It is centred on the
  // view axis and large enough that every pixel must hit it.
  auto& s = tracer.AddObject<Sphere>(Vec3f{0, 0, 30}, 29.0f, m);
  s.bounds = AABB{};  // default empty / non-finite
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  TASK_CHECK(tracer.BeamStats().object_tests >=
             camera.width() * camera.height());
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(tracer.image().at(y, x).y == 220);
    }
  }
  return 0;
}

// Non-finite boxes (NaN/inf) behave like unknown bounds.
int FrustaNonFiniteBounds() {
  Camera camera = MakeCamera(5, 5);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(220, 220, 0);

  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  auto& s = tracer.AddObject<Sphere>(Vec3f{0, 0, 30}, 29.0f, m);
  s.bounds.min = Vec3f{std::numeric_limits<float>::quiet_NaN(), -1, -1};
  s.bounds.max = Vec3f{std::numeric_limits<float>::infinity(), 1, 1};
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(tracer.image().at(y, x).x == 220);
    }
  }
  return 0;
}

// Exact direction boundary: a tiny object placed exactly on the
// direction of one pixel's ray must be found even when its box only
// touches the cone boundary.
int FrustaBoundaryTouch() {
  Camera camera = MakeCamera(5, 5);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(0, 100, 250);

  RayTracer plain(camera, lights);
  RayTracer beam(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(0, 0, 0);
    // The centre pixel direction is straight ahead; put a tiny sphere
    // exactly there and a large wall behind it.
    rt.AddObject<Sphere>(Vec3f{0, 0, 10}, 0.02f, m);
    rt.AddObject<Quad>(Vec3f{-40, -40, 40}, Vec3f{40, -40, 40},
                       Vec3f{40, 40, 40}, Vec3f{-40, 40, 40}, m);
  };
  build(plain);
  build(beam);
  plain.SetBeamTraversal(false);
  beam.SetBeamTraversal(true);
  plain.Trace(0);
  beam.Trace(0);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(SameColor(plain.image().at(y, x), beam.image().at(y, x)));
    }
  }
  return 0;
}

}  // namespace

// ============================================================= traversal
namespace {

int TraversalMatchesBruteForceRandom() {
  std::mt19937 rng(12345);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (int trial = 0; trial < 60; ++trial) {
    const std::size_t w = 2 + static_cast<std::size_t>(rng() % 17);
    const std::size_t h = 2 + static_cast<std::size_t>(rng() % 17);
    Camera camera = MakeCamera(w, h,
                               Vec3f{u(rng) * 4, u(rng) * 4, u(rng) * 4},
                               Mat3x3{u(rng), u(rng), u(rng)});
    Lights lights;
    lights.AddAmbient(1.0f);
    Material m = Plain(200, 100, 40);

    RayTracer tracer(camera, lights);
    tracer.SetBackground(0, 0, 0);
    const int n = 1 + static_cast<int>(rng() % 30);
    for (int i = 0; i < n; ++i) {
      const float x = u(rng) * 9, y = u(rng) * 9, z = 5 + u(rng) * 25;
      const int kind = static_cast<int>(rng() % 4);
      if (kind == 0) {
        const float r = 0.2f + std::abs(u(rng)) * 1.4f;
        tracer.AddObject<Sphere>(Vec3f{x, y, z}, r, m);
      } else if (kind == 1) {
        const float a = 0.5f + std::abs(u(rng)) * 2.0f;
        const float b = 0.5f + std::abs(u(rng)) * 2.0f;
        const float c = 0.5f + std::abs(u(rng)) * 2.0f;
        tracer.AddObject<Block>(Vec3f{x, y, z}, a, b, c,
                                Mat3x3{u(rng), u(rng), u(rng)}, m);
      } else if (kind == 2) {
        tracer.AddObject<Quad>(Vec3f{x - 1.5f, y - 1.5f, z},
                               Vec3f{x + 1.5f, y - 1.5f, z},
                               Vec3f{x + 1.5f, y + 1.5f, z},
                               Vec3f{x - 1.5f, y + 1.5f, z}, m);
      } else {
        tracer.AddObject<Triangle>(Vec3f{x - 1.5f, y - 1.5f, z},
                                   Vec3f{x + 1.5f, y - 1.5f, z},
                                   Vec3f{x, y + 1.5f, z}, m);
      }
    }
    tracer.SetBeamTraversal(true);
    tracer.Trace(0);
    TASK_CHECK(VerifyTracer(tracer, camera));
  }
  return 0;
}

int TraversalMixedPrimitives() {
  Camera camera = MakeCamera(11, 9);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(180, 120, 60);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  // overlapping different primitive types
  tracer.AddObject<Quad>(Vec3f{-4, -4, 8}, Vec3f{4, -4, 8},
                         Vec3f{4, 4, 8}, Vec3f{-4, 4, 8}, m);
  tracer.AddObject<Sphere>(Vec3f{0.5f, 0.3f, 7.0f}, 1.0f, m);
  tracer.AddObject<Triangle>(Vec3f{-3, -3, 6}, Vec3f{3, -3, 6},
                             Vec3f{0, 3, 6}, m);
  tracer.AddObject<Block>(Vec3f{-1.0f, 0.5f, 5.0f}, 0.8f, 0.5f, 0.4f,
                          Mat3x3{0.4f, 0.2f, -0.3f}, m);
  tracer.AddObject<Sphere>(Vec3f{2.0f, -1.0f, 9.0f}, 1.2f, m);
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  TASK_CHECK(VerifyTracer(tracer, camera));

  // Every pixel/object pair is tested at most once in this frame.
  const std::uint64_t pairs =
      static_cast<std::uint64_t>(camera.width()) * camera.height() *
      tracer.Beam().objects_view().size();
  TASK_CHECK(tracer.BeamStats().object_tests <= pairs);
  return 0;
}

// Emissive glow and background sampling use the same paths on both
// sides, including the miss branch.
int RenderEmissiveGlowParity() {
  Camera camera = MakeCamera(15, 13);
  Lights lights;
  lights.AddAmbient(0.3f);
  Material m;
  m.color = Vec3u8{120, 130, 140};
  m.specular = 10.0f;
  m.reflective = 0.3f;
  m.transparency = 0.2f;
  m.refractive_index = 1.4f;
  Material emitter;
  emitter.color = Vec3u8{0, 0, 0};
  emitter.specular = 0.0f;
  emitter.emission = Vec3u8{255, 180, 60};
  emitter.emit_strength = 1.2f;

  RayTracer plain(camera, lights), beam(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(5, 7, 9);
    rt.AddObject<Quad>(Vec3f{-20, -20, 20}, Vec3f{20, -20, 20},
                       Vec3f{20, 20, 20}, Vec3f{-20, 20, 20}, m);
    rt.AddObject<Sphere>(Vec3f{-1.5f, 0, 10}, 1.5f, m);
    rt.AddObject<Sphere>(Vec3f{1.8f, 0.5f, 13}, 1.0f, emitter);
  };
  build(plain);
  build(beam);
  plain.SetBeamTraversal(false);
  beam.SetBeamTraversal(true);
  plain.Trace(3);
  beam.Trace(3);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(SameColor(plain.image().at(y, x), beam.image().at(y, x)));
    }
  }
  return 0;
}

// Every pixel/object pair is tested at most once in this frame.
int TraversalExactlyOnceWhenUncullable() {
  Camera camera = MakeCamera(9, 7);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(200, 100, 40);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  // Five nested blocks all containing the eye: no ray test can ever
  // reject them, so the traversal must visit every pair exactly once.
  const std::size_t n = 5;
  for (std::size_t i = 0; i < n; ++i) {
    tracer.AddObject<Block>(Vec3f{0, 0, 0}, 3.0f + static_cast<float>(i),
                            3.0f, 3.0f, Mat3x3{}, m);
  }
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  TASK_CHECK(tracer.BeamStats().object_tests ==
             static_cast<std::uint64_t>(camera.width()) *
                 camera.height() * n);
  TASK_CHECK(VerifyTracer(tracer, camera));
  return 0;
}

// Two distinct slots with the same geometry type must stay distinct
// identities and tie-break by the smaller slot.
int TraversalSameDistanceTie() {
  Camera camera = MakeCamera(5, 5);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material first = Plain(10, 20, 30);
  Material second = Plain(200, 200, 200);

  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  // Two coincident spheres: identical geometry and identical t.
  tracer.AddObject<Sphere>(Vec3f{2, 0, 10}, 1.0f, second);
  tracer.AddObject<Sphere>(Vec3f{2, 0, 10}, 1.0f, first);
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  // The smaller slot (0) must win for the pixels that see the spheres.
  std::size_t seen = 0;
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      const beam::Hit& h = tracer.Beam().hit(y, x);
      if (h.IsHit()) {
        TASK_CHECK(h.slot == 0);
        TASK_CHECK(h.object == tracer.Beam().objects_view()[0]);
        ++seen;
      }
    }
  }
  TASK_CHECK(seen > 0);
  return 0;
}

// Object slots are a permutation and leaves reference the real slot
// indices even when the BVH reorders them.
int TraversalSlotsArePermutation() {
  Camera camera = MakeCamera(6, 6);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(150, 150, 150);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  for (int i = 0; i < 16; ++i) {
    const float x = -3.0f + static_cast<float>(i % 4) * 2.0f;
    const float y = -3.0f + static_cast<float>(i / 4) * 2.0f;
    tracer.AddObject<Sphere>(Vec3f{x, y, 12}, 0.4f, m);
  }
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  const auto& slots = tracer.Beam().object_slots();
  TASK_CHECK(slots.size() == 16);
  std::vector<int> sorted = slots;
  std::sort(sorted.begin(), sorted.end());
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    TASK_CHECK(sorted[i] == static_cast<int>(i));
  }
  // Leaf entries point at the matching original object.
  for (const auto& node : tracer.Beam().object_nodes()) {
    if (!node.IsLeaf()) continue;
    for (int i = node.begin; i < node.end; ++i) {
      const int slot = slots[static_cast<std::size_t>(i)];
      TASK_CHECK(slot >= 0 && static_cast<std::size_t>(slot) < 16);
      const Vec3f expected{
          -3.0f + static_cast<float>(slot % 4) * 2.0f,
          -3.0f + static_cast<float>(slot / 4) * 2.0f,
          12.0f};
      TASK_CHECK(tracer.Beam().objects_view()[static_cast<std::size_t>(slot)]
                     ->center == expected);
    }
  }
  return 0;
}

}  // namespace

// =============================================================== updates
namespace {

int UpdatesRefitAndRebuild() {
  Camera camera = MakeCamera(7, 7);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(200, 100, 40);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  auto& moving = tracer.AddObject<Sphere>(Vec3f{0, 0, 12}, 1.0f, m);
  tracer.AddObject<Sphere>(Vec3f{20, 20, 40}, 1.0f, m);
  tracer.AddObject<Sphere>(Vec3f{-20, -20, 40}, 1.0f, m);
  tracer.AddObject<Sphere>(Vec3f{20, -20, 40}, 1.0f, m);
  tracer.SetBeamTraversal(true);

  tracer.Trace(0);
  const std::uint64_t gen1 = tracer.Beam().topology_generation();
  const std::size_t nodes1 = tracer.Beam().object_nodes().size();
  const std::vector<int> slots1 = tracer.Beam().object_slots();

  // Move the object and update its bounds: topology unchanged.
  moving.center = Vec3f{1.0f, 0.0f, 18.0f};
  moving.bounds.min = Vec3f{0.0f, -1.0f, 17.0f};
  moving.bounds.max = Vec3f{2.0f, 1.0f, 19.0f};
  tracer.Trace(0);
  TASK_CHECK(!tracer.Beam().last_sync_rebuilt());
  TASK_CHECK(tracer.Beam().topology_generation() == gen1);
  TASK_CHECK(tracer.Beam().object_nodes().size() == nodes1);
  TASK_CHECK(tracer.Beam().object_slots() == slots1);
  TASK_CHECK(VerifyTracer(tracer, camera));

  // AddObject grows the list: a rebuild is allowed.
  tracer.AddObject<Sphere>(Vec3f{0, 0, 30}, 1.0f, m);
  tracer.Trace(0);
  TASK_CHECK(tracer.Beam().last_sync_rebuilt());
  TASK_CHECK(tracer.Beam().topology_generation() != gen1);
  TASK_CHECK(VerifyTracer(tracer, camera));
  return 0;
}

// Sweep an unknown bounds flag back to a finite box and verify the
// culling changes take effect (the object becomes visible again).
int UpdatesKnownUnknownToggle() {
  Camera camera = MakeCamera(9, 9);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material visible = Plain(0, 230, 0);
  Material occluder = Plain(230, 0, 0);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  tracer.AddObject<Quad>(Vec3f{-40, -40, 15}, Vec3f{40, -40, 15},
                         Vec3f{40, 40, 15}, Vec3f{-40, 40, 15}, visible);
  auto& small = tracer.AddObject<Sphere>(Vec3f{0, 0, 5}, 1.0f, occluder);
  tracer.SetBeamTraversal(true);

  // Unknown bounds: the sphere still blocks the centre.
  small.bounds = AABB{};
  tracer.Trace(0);
  TASK_CHECK(tracer.image().at(4, 4).x > 200);
  TASK_CHECK(tracer.BeamStats().object_tests >=
             camera.width() * camera.height());

  // Finite bounds smaller than the sphere: geometry is unchanged but
  // the box hides it from the traversal; correct behaviour requires
  // the caller to keep bounds in sync, so restore an honest box and
  // check the visible result again.
  small.bounds.min = Vec3f{-1.1f, -1.1f, 3.9f};
  small.bounds.max = Vec3f{1.1f, 1.1f, 6.1f};
  tracer.Trace(0);
  TASK_CHECK(tracer.image().at(4, 4).x > 200);
  return 0;
}

// Two tracers alternating frames must not share any state.
int UpdatesAlternatingTracers() {
  Lights lights;
  lights.AddAmbient(1.0f);
  Material a_mat = Plain(200, 0, 0);
  Material b_mat = Plain(0, 0, 200);

  Camera cam_a = MakeCamera(9, 7, Vec3f{-1, 0, 0});
  Camera cam_b = MakeCamera(5, 11, Vec3f{1, 0.5f, 0}, Mat3x3{0.2f, 0, 0.1f});

  RayTracer a(cam_a, lights);
  RayTracer b(cam_b, lights);
  a.SetBackground(0, 0, 0);
  b.SetBackground(0, 0, 0);
  a.AddObject<Sphere>(Vec3f{0, 0, 12}, 1.5f, a_mat);
  b.AddObject<Sphere>(Vec3f{1, 0.5f, 14}, 2.0f, b_mat);
  a.SetBeamTraversal(true);
  b.SetBeamTraversal(true);

  for (int frame = 0; frame < 3; ++frame) {
    a.Trace(0);
    TASK_CHECK(a.image().width == 9 && a.image().height == 7);
    bool a_has_red = false;
    std::size_t a_hits = 0;
    for (unsigned y = 0; y < a.image().height; ++y) {
      for (unsigned x = 0; x < a.image().width; ++x) {
        const Vec3u8 p = a.image().at(y, x);
        // a only ever draws its red sphere or the black background
        TASK_CHECK(p.z == 0 && p.y == 0);
        if (p.x > 0) a_has_red = true;
      }
    }
    TASK_CHECK(a_has_red);
    for (unsigned y = 0; y < a.Beam().height(); ++y) {
      for (unsigned x = 0; x < a.Beam().width(); ++x) {
        if (a.Beam().hit(y, x).IsHit()) {
          TASK_CHECK(a.Beam().hit(y, x).slot == 0);
          ++a_hits;
        }
      }
    }
    TASK_CHECK(a_hits > 0);

    b.Trace(0);
    TASK_CHECK(b.image().width == 5 && b.image().height == 11);
    bool b_has_blue = false;
    for (unsigned y = 0; y < b.image().height; ++y) {
      for (unsigned x = 0; x < b.image().width; ++x) {
        const Vec3u8 p = b.image().at(y, x);
        // b only ever draws its blue sphere or the black background
        TASK_CHECK(p.x == 0 && p.y == 0);
        if (p.z > 0) b_has_blue = true;
      }
    }
    TASK_CHECK(b_has_blue);
    // a's screen hierarchy still matches a's camera, not b's
    TASK_CHECK(a.Beam().width() == 9 && a.Beam().height() == 7);
  }
  return 0;
}

// Camera translation and rotation must be re-read every Trace.
int UpdatesCameraChanges() {
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(180, 90, 45);
  Camera camera = MakeCamera(11, 9);
  RayTracer plain(camera, lights), beam(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(0, 0, 0);
    rt.AddObject<Sphere>(Vec3f{-2, 0, 12}, 1.0f, m);
    rt.AddObject<Sphere>(Vec3f{2, 1, 16}, 1.4f, m);
    rt.AddObject<Quad>(Vec3f{-30, -30, 25}, Vec3f{30, -30, 25},
                       Vec3f{30, 30, 25}, Vec3f{-30, 30, 25}, m);
  };
  build(plain);
  build(beam);
  // Camera is a reference held by both tracers, so move it and Trace.
  for (int i = 0; i < 4; ++i) {
    if (i > 0) {
      camera.Translate(Vec3f{1.0f, -0.5f, 2.0f});
      camera.Rotate(0.05f * static_cast<float>(i),
                    0.07f * static_cast<float>(i),
                    -0.04f * static_cast<float>(i));
    }
    plain.SetBeamTraversal(false);
    beam.SetBeamTraversal(true);
    plain.Trace(0);
    beam.Trace(0);
    for (unsigned y = 0; y < camera.height(); ++y) {
      for (unsigned x = 0; x < camera.width(); ++x) {
        TASK_CHECK(SameColor(plain.image().at(y, x),
                             beam.image().at(y, x)));
      }
    }
  }
  return 0;
}

// Odd image sizes exercise the smaller final quadtree block.
int UpdatesOddSizes() {
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(90, 160, 210);
  const std::size_t sizes[][2] = {{3, 2}, {2, 3}, {7, 5}, {5, 7}, {13, 9}};
  for (auto& size : sizes) {
    Camera camera = MakeCamera(size[0], size[1]);
    RayTracer plain(camera, lights), beam(camera, lights);
    auto build = [&](RayTracer& rt) {
      rt.SetBackground(0, 0, 0);
      rt.AddObject<Sphere>(Vec3f{0.4f, -0.2f, 11}, 1.2f, m);
      rt.AddObject<Sphere>(Vec3f{-1.0f, 0.7f, 14}, 1.6f, m);
      rt.AddObject<Quad>(Vec3f{-20, -20, 30}, Vec3f{20, -20, 30},
                         Vec3f{20, 20, 30}, Vec3f{-20, 20, 30}, m);
    };
    build(plain);
    build(beam);
    plain.SetBeamTraversal(false);
    beam.SetBeamTraversal(true);
    plain.Trace(0);
    beam.Trace(0);
    TASK_CHECK(beam.image().width == size[0] &&
               beam.image().height == size[1]);
    for (unsigned y = 0; y < camera.height(); ++y) {
      for (unsigned x = 0; x < camera.width(); ++x) {
        TASK_CHECK(SameColor(plain.image().at(y, x),
                             beam.image().at(y, x)));
      }
    }
  }
  return 0;
}

}  // namespace

// ================================================================ render
namespace {

// Full render parity with beam traversal disabled, including
// reflective and transparent materials and a PPM round-trip.
int RenderReflectiveTransparentParity() {
  Camera camera = MakeCamera(21, 17);
  Lights lights;
  lights.AddAmbient(0.4f);
  lights.AddPoint(0.6f, 4.0f, 6.0f, -3.0f);

  Material matte = Plain(180, 120, 60);
  Material mirror;
  mirror.color = Vec3u8{220, 220, 220};
  mirror.specular = 30.0f;
  mirror.reflective = 0.6f;
  Material glass;
  glass.color = Vec3u8{200, 230, 240};
  glass.specular = 10.0f;
  glass.transparency = 0.7f;
  glass.refractive_index = 1.5f;
  glass.tint = 0.2f;

  RayTracer plain(camera, lights), beam(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(7, 9, 12);
    rt.AddObject<Quad>(Vec3f{-12, -12, 20}, Vec3f{12, -12, 20},
                       Vec3f{12, 12, 20}, Vec3f{-12, 12, 20}, matte);
    rt.AddObject<Sphere>(Vec3f{-1.4f, 0.3f, 10}, 1.6f, mirror);
    rt.AddObject<Sphere>(Vec3f{1.6f, -0.2f, 12}, 1.4f, glass);
    rt.AddObject<Block>(Vec3f{0.2f, 1.8f, 14}, 1.0f, 0.6f, 0.8f,
                        Mat3x3{0.3f, 0.1f, 0.2f}, matte);
  };
  build(plain);
  build(beam);
  plain.SetBeamTraversal(false);
  beam.SetBeamTraversal(true);
  plain.Trace(4);
  beam.Trace(4);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(SameColor(plain.image().at(y, x), beam.image().at(y, x)));
    }
  }
  // PPM round-trip of the beam image.
  const std::string path = ".task-build/beam_test_render.ppm";
  Ppm::Write(beam.image(), path);
  Image loaded;
  Ppm::Read(loaded, path);
  TASK_CHECK(loaded.width == beam.image().width);
  TASK_CHECK(loaded.height == beam.image().height);
  TASK_CHECK(loaded.data.size() == beam.image().data.size());
  return 0;
}

// The required hidden-geometry/statistics scenario.
int RenderHiddenGridStats() {
  Camera camera = MakeCamera(65, 49);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material near_mat = Plain(200, 90, 40);
  Material far_mat = Plain(40, 200, 110);

  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  const std::size_t object_count = 1 + 64 * 64;
  tracer.AddObject<Quad>(Vec3f{-80, -80, 10}, Vec3f{80, -80, 10},
                         Vec3f{80, 80, 10}, Vec3f{-80, 80, 10}, near_mat);
  for (int iy = 0; iy < 64; ++iy) {
    for (int ix = 0; ix < 64; ++ix) {
      const float x = -40.0f + static_cast<float>(ix) * (80.0f / 63.0f);
      const float y = -40.0f + static_cast<float>(iy) * (80.0f / 63.0f);
      tracer.AddObject<Sphere>(Vec3f{x, y, 80.0f}, 0.3f, far_mat);
    }
  }
  tracer.SetBeamTraversal(true);
  tracer.Trace(1);

  const beam::WorkStats& stats = tracer.BeamStats();
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(camera.width()) * camera.height();
  TASK_CHECK(stats.beam_box_tests < pixels * object_count / 16);
  TASK_CHECK(stats.object_tests <= 2 * pixels);
  TASK_CHECK(stats.depth_rejects > 0);
  // The cover wins everywhere; no hidden sphere is visible.
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      const beam::Hit& h = tracer.Beam().hit(y, x);
      TASK_CHECK(h.IsHit());
      TASK_CHECK(h.slot == 0);
      TASK_CHECK(h.object == tracer.Beam().objects_view()[0]);
    }
  }
  return 0;
}

// Four quads with a one-pixel-visible seam: the target behind the
// seam must survive.
int RenderSeamHole() {
  Camera camera = MakeCamera(65, 49);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material cover = Plain(200, 90, 40);
  Material target = Plain(0, 220, 0);

  RayTracer beam(camera, lights), plain(camera, lights);
  auto build = [&](RayTracer& rt) {
    rt.SetBackground(0, 0, 0);
    const float gx = 0.06f, gy = 0.06f;
    rt.AddObject<Quad>(Vec3f{-100, -100, 10}, Vec3f{-gx, -100, 10},
                       Vec3f{-gx, 100, 10}, Vec3f{-100, 100, 10}, cover);
    rt.AddObject<Quad>(Vec3f{gx, -100, 10}, Vec3f{100, -100, 10},
                       Vec3f{100, 100, 10}, Vec3f{gx, 100, 10}, cover);
    rt.AddObject<Quad>(Vec3f{-gx, -100, 10}, Vec3f{gx, -100, 10},
                       Vec3f{gx, -gy, 10}, Vec3f{-gx, -gy, 10}, cover);
    rt.AddObject<Quad>(Vec3f{-gx, gy, 10}, Vec3f{gx, gy, 10},
                       Vec3f{gx, 100, 10}, Vec3f{-gx, 100, 10}, cover);
    rt.AddObject<Sphere>(Vec3f{0, 0, 40}, 3.0f, target);
  };
  build(beam);
  build(plain);
  beam.SetBeamTraversal(true);
  plain.SetBeamTraversal(false);
  beam.Trace(0);
  plain.Trace(0);

  std::size_t green = 0;
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      const Vec3u8 p = beam.image().at(y, x);
      if (p.y > 100 && p.x < 100) ++green;
      TASK_CHECK(SameColor(plain.image().at(y, x), p));
    }
  }
  // The seam exposes exactly the one pixel on the view axis.
  TASK_CHECK(green == 1);
  // The target really is behind the cover: slot 4 is the sphere.
  const beam::Hit& centre = beam.Beam().hit(camera.height() / 2,
                                            camera.width() / 2);
  TASK_CHECK(centre.IsHit());
  TASK_CHECK(centre.slot == 4);
  return 0;
}

// Every pixel is tested with at most one exact intersection per
// object slot, and stats only count the primary beam pass.
int RenderWorkStatsSanity() {
  Camera camera = MakeCamera(9, 7);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(120, 200, 90);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(0, 0, 0);
  const std::size_t n = 25;
  for (std::size_t i = 0; i < n; ++i) {
    tracer.AddObject<Sphere>(
        Vec3f{-3.0f + static_cast<float>(i % 5) * 1.5f,
              -2.0f + static_cast<float>(i / 5) * 1.2f, 9.0f + 0.4f * static_cast<float>(i)},
        0.35f, m);
  }
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  const beam::WorkStats& s = tracer.BeamStats();
  TASK_CHECK(s.object_tests <= camera.width() * camera.height() * n);
  TASK_CHECK(s.beam_box_tests > 0);
  // Screen depth reduction: the root maximum equals the largest real
  // pixel forward depth (all misses are infinite here only if no hit;
  // this scene hits everywhere).
  const auto& nodes = tracer.Beam().screen_nodes();
  TASK_CHECK(!nodes.empty());
  float maxpix = -std::numeric_limits<float>::infinity();
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      maxpix = std::max(maxpix, tracer.Beam().hit(y, x).forward_depth);
    }
  }
  TASK_CHECK(Near(nodes[static_cast<std::size_t>(tracer.Beam().screen_root())]
                      .max_forward_depth,
                  maxpix, 1e-3f));
  return 0;
}

// Misses are reported as slot max() with an empty HitRecord.
int RenderMissSlot() {
  Camera camera = MakeCamera(5, 4);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(100, 100, 100);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(1, 2, 3);
  // A tiny sphere far off-axis so most pixels miss.
  tracer.AddObject<Sphere>(Vec3f{50, 50, 60}, 0.5f, m);
  tracer.SetBeamTraversal(true);
  tracer.Trace(0);
  std::size_t misses = 0;
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      const beam::Hit& h = tracer.Beam().hit(y, x);
      if (!h.IsHit()) {
        ++misses;
        TASK_CHECK(h.slot == kNoSlot);
        TASK_CHECK(h.object == nullptr);
        TASK_CHECK(!h.record.is_hit);
        TASK_CHECK(SameColor(tracer.image().at(y, x), Vec3u8{1, 2, 3}));
      }
    }
  }
  TASK_CHECK(misses > 0);
  return 0;
}

// The traversal is off by default and toggling does not change the
// resulting image.
int RenderDefaultOffAndToggle() {
  Camera camera = MakeCamera(8, 8);
  Lights lights;
  lights.AddAmbient(1.0f);
  Material m = Plain(200, 100, 40);
  RayTracer tracer(camera, lights);
  tracer.SetBackground(3, 5, 7);
  tracer.AddObject<Sphere>(Vec3f{0, 0, 12}, 2.0f, m);
  TASK_CHECK(!tracer.BeamTraversalEnabled());
  tracer.Trace(1);
  Image before = tracer.image();
  // Statistics are zero when the beam pass is not used.
  TASK_CHECK(tracer.BeamStats().beam_box_tests == 0);
  TASK_CHECK(tracer.BeamStats().depth_rejects == 0);
  TASK_CHECK(tracer.BeamStats().object_tests == 0);
  tracer.SetBeamTraversal(true);
  TASK_CHECK(tracer.BeamTraversalEnabled());
  tracer.Trace(1);
  TASK_CHECK(tracer.BeamStats().object_tests > 0);
  for (unsigned y = 0; y < camera.height(); ++y) {
    for (unsigned x = 0; x < camera.width(); ++x) {
      TASK_CHECK(SameColor(before.at(y, x), tracer.image().at(y, x)));
    }
  }
  // Turning it off again clears the counters rather than leaving stale
  // values from the previous beam frame.
  tracer.SetBeamTraversal(false);
  tracer.Trace(1);
  TASK_CHECK(tracer.BeamStats().object_tests == 0);
  return 0;
}

}  // namespace

// ============================================================ registration
// The driver declares `const struct task_case task_cases[]` with C++
// linkage, so no extern "C" here.
const task_case task_cases[] = {
    {"frusta", "degenerate_regions", FrustaDegenerateRegions},
    {"frusta", "crossing_and_inside", FrustaCrossingAndInside},
    {"frusta", "unknown_bounds", FrustaUnknownBounds},
    {"frusta", "non_finite_bounds", FrustaNonFiniteBounds},
    {"frusta", "boundary_touch", FrustaBoundaryTouch},

    {"traversal", "matches_brute_force_random", TraversalMatchesBruteForceRandom},
    {"traversal", "mixed_primitives", TraversalMixedPrimitives},
    {"traversal", "exactly_once_uncullable", TraversalExactlyOnceWhenUncullable},
    {"traversal", "same_distance_tie", TraversalSameDistanceTie},
    {"traversal", "slots_are_permutation", TraversalSlotsArePermutation},

    {"updates", "refit_and_rebuild", UpdatesRefitAndRebuild},
    {"updates", "known_unknown_toggle", UpdatesKnownUnknownToggle},
    {"updates", "alternating_tracers", UpdatesAlternatingTracers},
    {"updates", "camera_changes", UpdatesCameraChanges},
    {"updates", "odd_sizes", UpdatesOddSizes},

    {"render", "reflective_transparent_parity",
     RenderReflectiveTransparentParity},
    {"render", "emissive_glow_parity", RenderEmissiveGlowParity},
    {"render", "hidden_grid_stats", RenderHiddenGridStats},
    {"render", "seam_hole", RenderSeamHole},
    {"render", "work_stats_sanity", RenderWorkStatsSanity},
    {"render", "miss_slot", RenderMissSlot},
    {"render", "default_off_and_toggle", RenderDefaultOffAndToggle},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
