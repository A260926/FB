/**
 * Regression tests for the offline lightmap baker and the lightmapped render
 * path. Registered through task_support/check.h in three groups:
 *
 *   surface     - snapshot, coverage and binding rules
 *   transport   - fixed-sample rays, nearest hits, two-round numbers
 *   integration - stepping/reset/release semantics and camera pixels
 *
 * The transport cases re-derive every ray, intersection and round value with
 * their own first-principles math rather than calling into lightmap.c, so a
 * shared implementation bug cannot make the expectation match itself.
 */

#include "task_support/check.h"

#include "render/backend.h"
#include "render/entity.h"
#include "render/lightmap.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/pixel.h"
#include "render/renderer.h"
#include "render/state.h"
#include "render/texture.h"
#include "tests/memory_backend.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846f

// ---------------------------------------------------------------------------
// Numeric helpers (independent of the production code)
// ---------------------------------------------------------------------------

static int fnear(float a, float b) {
  float d = fabsf(a - b);
  return d <= 1e-5f + 1e-5f * fabsf(b);
}

static int vnear(Vec3f v, float x, float y, float z) {
  return fnear(v.x, x) && fnear(v.y, y) && fnear(v.z, z);
}

static float t_radical_inverse(uint32_t n) {
  float r = 0.0f, half = 0.5f;
  while (n) {
    r += (float)(n & 1u) * half;
    n >>= 1;
    half *= 0.5f;
  }
  return r;
}

// Independent copy of the specified cosine hemisphere sampling.
static Vec3f t_sample(Vec3f n, int k, int S) {
  float u = (k + 0.5f) / (float)S;
  float v = t_radical_inverse((uint32_t)k);
  float r = sqrtf(u);
  float phi = 2.0f * TEST_PI * v;
  Vec3f local = {r * cosf(phi), r * sinf(phi), sqrtf(1.0f - u)};
  Vec3f axis = (fabsf(n.y) < 0.9f) ? (Vec3f){0, 1, 0} : (Vec3f){1, 0, 0};
  Vec3f t = {
      axis.y * n.z - axis.z * n.y,
      axis.z * n.x - axis.x * n.z,
      axis.x * n.y - axis.y * n.x,
  };
  float il = 1.0f / sqrtf(t.x * t.x + t.y * t.y + t.z * t.z);
  t.x *= il;
  t.y *= il;
  t.z *= il;
  Vec3f b = {n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z,
             n.x * t.y - n.y * t.x};
  return (Vec3f){t.x * local.x + b.x * local.y + n.x * local.z,
                 t.y * local.x + b.y * local.y + n.y * local.z,
                 t.z * local.x + b.z * local.y + n.z * local.z};
}

static Vec3f t_sub(Vec3f a, Vec3f b) {
  return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}
static Vec3f t_add(Vec3f a, Vec3f b) {
  return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z};
}
static Vec3f t_scale(Vec3f a, float s) {
  return (Vec3f){a.x * s, a.y * s, a.z * s};
}
static float t_dot(Vec3f a, Vec3f b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec3f t_cross(Vec3f a, Vec3f b) {
  return (Vec3f){a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x,
                 a.x * b.y - b.x * a.y};
}

// Independent Moller-Trumbore over an explicit world triangle list.
typedef struct {
  Vec3f a, b, c, n;
  Vec2f ua, ub, uc;
  int id;
} TTri;

typedef struct {
  int hit;
  int front;
  int id;
  Vec2f uv;
} THit;

static void t_trace(const TTri *tris, int count, Vec3f origin, Vec3f dir,
                    float eps, THit *out) {
  out->hit = 0;
  float best = 0.0f;
  for (int i = 0; i < count; i++) {
    Vec3f e1 = t_sub(tris[i].b, tris[i].a);
    Vec3f e2 = t_sub(tris[i].c, tris[i].a);
    Vec3f h = t_cross(dir, e2);
    float det = t_dot(e1, h);
    if (fabsf(det) < 1e-12f)
      continue;
    Vec3f s = t_sub(origin, tris[i].a);
    float u = t_dot(s, h) / det;
    if (u < 0 || u > 1)
      continue;
    Vec3f q = t_cross(s, e1);
    float v = t_dot(dir, q) / det;
    if (v < 0 || u + v > 1)
      continue;
    float t = t_dot(e2, q) / det;
    if (!(t > eps))
      continue;
    if (out->hit && !(t < best))
      continue;
    best = t;
    out->hit = 1;
    out->id = tris[i].id;
    out->front = t_dot(dir, tris[i].n) < 0.0f;
    float w = 1.0f - u - v;
    out->uv = (Vec2f){w * tris[i].ua.x + u * tris[i].ub.x +
                          v * tris[i].uc.x,
                      w * tris[i].ua.y + u * tris[i].ub.y +
                          v * tris[i].uc.y};
  }
}

// ---------------------------------------------------------------------------
// Scene construction helpers
// ---------------------------------------------------------------------------

// A one-triangle mesh with separate position and UV corner tables.
typedef struct {
  Vec3f pos[3];
  Vec2f uv[3];
  uint16_t idx[3];
  Mesh mesh;
  Material material;
  Object object;
} TriFixture;

static void tri_fixture_init(TriFixture *f, Vec3f p0, Vec3f p1, Vec3f p2,
                             Vec2f u0, Vec2f u1, Vec2f u2) {
  f->pos[0] = p0;
  f->pos[1] = p1;
  f->pos[2] = p2;
  f->uv[0] = u0;
  f->uv[1] = u1;
  f->uv[2] = u2;
  f->idx[0] = 0;
  f->idx[1] = 1;
  f->idx[2] = 2;
  f->mesh.indexes_count = 3;
  f->mesh.pos_indices = f->idx;
  f->mesh.tex_indices = f->idx;
  f->mesh.positions = f->pos;
  f->mesh.textCoord = f->uv;
  f->material.texture = 0;
  object_init(&f->object, &f->mesh, &f->material);
}

static BakeBinding make_binding(const size_t *path, size_t path_len, int w,
                                int h, const Vec2f *uv, size_t uv_count,
                                uint16_t *uv_idx, Vec3f refl, Vec3f emiss) {
  BakeBinding b;
  b.path = path;
  b.path_len = path_len;
  b.width = w;
  b.height = h;
  b.uv = uv;
  b.uv_count = uv_count;
  b.uv_indices = uv_idx;
  b.reflectance = refl;
  b.emission = emiss;
  return b;
}

// ===========================================================================
// SURFACE GROUP
// ===========================================================================

// Two UV triangles tile a unit square across the shared diagonal; the
// half-open rule must cover the diagonal texel centers exactly once and the
// whole interior without a coverage-conflict failure.
static int surface_square_coverage(void) {
  // Square split into two world triangles, both mapped CCW into UV unit
  // square with a mirrored winding between the two faces.
  Vec3f pos[4] = {{-2, -2, 0}, {2, -2, 0}, {2, 2, 0}, {-2, 2, 0}};
  Vec2f uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  uint16_t pi[6] = {0, 1, 2, 0, 2, 3};
  uint16_t ui[6] = {0, 1, 2, 0, 2, 3};
  Mesh mesh = {6, pi, ui, pos, uv};
  Material material = {0};
  Object object;
  object_init(&object, &mesh, &material);
  Entity root;
  entity_init(&root, (Renderable *)&object, mat4Identity());

  BakeBinding b = make_binding(0, 0, 4, 4, uv, 4, ui,
                               (Vec3f){0.3f, 0.3f, 0.3f},
                               (Vec3f){0, 0, 0});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) == OK);
  TASK_CHECK(scene != 0);
  TASK_CHECK(bake_instance_count(scene) == 1);

  const BakeInstanceView *v = bake_instance_view(scene, 0);
  int covered = 0;
  for (int t = 0; t < 16; t++) {
    TASK_CHECK(v->face_owner[t] == (v->covered[t] ? 0 : -1) ||
               v->face_owner[t] == 1);
    if (v->covered[t])
      covered++;
  }
  // Every texel center inside the unit square is covered; corner (0,0)
  // region and all interior rows are reached, only nothing lies outside.
  TASK_CHECK(covered >= 10);
  // Face ownership is one of the two original faces, never duplicated on a
  // texel, and both faces own at least one texel.
  int owners[2] = {0, 0};
  for (int t = 0; t < 16; t++)
    if (v->covered[t])
      owners[v->face_owner[t]]++;
  TASK_CHECK(owners[0] > 0 && owners[1] > 0);
  // Round zero: no emission means covered texels still read zero.
  for (int t = 0; t < 16; t++)
    TASK_CHECK(vnear(v->radiance[t], 0, 0, 0));

  bake_scene_free(scene);
  return 0;
}

// Two faces deliberately overlapping on an interior texel must fail the
// build, and the caller's output pointer must be left untouched.
static int surface_coverage_conflict(void) {
  Vec3f pos[6] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0},
                  {0, 0, 0}, {2, 2, 0}, {0, 2, 0}};
  // Both UV faces map to the same triangle, so interior texels are covered
  // twice and the build must report a positive-area conflict.
  Vec2f uv[6] = {{0, 0}, {1, 0}, {1, 1},
                 {0, 0}, {1, 0}, {1, 1}};
  uint16_t idx[6] = {0, 1, 2, 3, 4, 5};
  Mesh mesh = {6, idx, idx, pos, uv};
  Material material = {0};
  Object object;
  object_init(&object, &mesh, &material);
  Entity root;
  entity_init(&root, (Renderable *)&object, mat4Identity());

  BakeBinding b = make_binding(0, 0, 4, 4, uv, 6, idx, (Vec3f){0.2f, 0, 0},
                               (Vec3f){0, 0, 0});
  BakeScene *sentinel = (BakeScene *)0x1;
  BakeScene *scene = sentinel;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) != OK);
  TASK_CHECK(scene == sentinel); // old output preserved
  return 0;
}

// A zero-area UV face covers nothing but does not fail the build; an
// out-of-range UV index and an out-of-domain UV do fail.
static int surface_zero_area_and_bad_uv(void) {
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){0, 0},
                   (Vec2f){1, 1}); // first two corners coincide: zero area
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());

  BakeBinding b = make_binding(0, 0, 2, 2, f.uv, 3, f.idx,
                               (Vec3f){0.4f, 0.4f, 0.4f},
                               (Vec3f){1, 1, 1});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) == OK);
  const BakeInstanceView *v = bake_instance_view(scene, 0);
  for (int t = 0; t < 4; t++)
    TASK_CHECK(!v->covered[t]); // degenerate UV face never covers
  bake_scene_free(scene);

  // Out-of-range secondary index.
  uint16_t bad_idx[3] = {0, 7, 2};
  BakeBinding bad = b;
  bad.uv_indices = bad_idx;
  scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);
  TASK_CHECK(scene == 0);

  // UV outside [0,1].
  Vec2f bad_uv[3] = {{0, 0}, {1.2f, 0}, {1, 1}};
  bad = b;
  bad.uv = bad_uv;
  bad.uv_indices = f.idx;
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);
  return 0;
}

// The same Object attached in two places produces two instances, frozen with
// different non-uniform scale transforms (one of them a determinant-negative
// reflection) and never merged by pointer.
static int surface_shared_object_two_instances(void) {
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});

  Entity children[2];
  entity_init(&children[0], (Renderable *)&f.object,
              mat4Scale((Vec3f){2.0f, 1.0f, 1.0f}));
  // Negative x scale: a valid but orientation-reversing (reflection)
  // transform, combined with non-uniform y scaling.
  entity_init(&children[1], (Renderable *)&f.object,
              mat4Scale((Vec3f){-1.0f, 3.0f, 1.0f}));
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());
  Array kids;
  array_init(&kids, 2, children);
  root.children_entities = kids;
  // The root itself draws a separate empty-mesh object bound on the empty
  // path, so every visible Object is bound exactly once.
  TriFixture empty;
  tri_fixture_init(&empty, (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0},
                   (Vec3f){0, 0, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  root.entity_renderable = (Renderable *)&empty.object;
  empty.mesh.indexes_count = 0;

  size_t p0[] = {0};
  size_t p1[] = {1};
  BakeBinding bindings[3] = {
      make_binding(p0, 1, 2, 2, f.uv, 3, f.idx, (Vec3f){0.2f, 0, 0},
                   (Vec3f){0, 0, 0}),
      make_binding(p1, 1, 2, 2, f.uv, 3, f.idx, (Vec3f){0, 0.2f, 0},
                   (Vec3f){0, 0, 0}),
      make_binding(0, 0, 2, 2, f.uv, 3, f.idx, (Vec3f){0.1f, 0.1f, 0.1f},
                   (Vec3f){0, 0, 0}),
  };
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == OK);
  TASK_CHECK(bake_instance_count(scene) == 3);

  // Traversal visits children before the root's own object, so instances 0
  // and 1 are the two shared-object placements.
  const BakeInstanceView *v0 = bake_instance_view(scene, 0);
  const BakeInstanceView *v1 = bake_instance_view(scene, 1);
  TASK_CHECK(v0->path_len == 1 && v0->path[0] == 0);
  TASK_CHECK(v1->path_len == 1 && v1->path[0] == 1);
  TASK_CHECK(v0 != v1);

  // Both placements, including the reflected one, bake to finite radiance;
  // the reflection flips the winding-derived normal rather than breaking it.
  TASK_CHECK(bake_step(scene, 1, 4, 1e-4f, (Vec3f){0.1f, 0.1f, 0.1f}) == OK);
  for (size_t mi = 0; mi < 3; mi++) {
    const BakeInstanceView *v = bake_instance_view(scene, mi);
    for (int t = 0; t < v->width * v->height; t++)
      TASK_CHECK(isfinite(v->radiance[t].x) &&
                 isfinite(v->radiance[t].y) &&
                 isfinite(v->radiance[t].z));
  }

  // One binding for two visible instances must fail: each visible Object is
  // bound exactly once, never merged by pointer.
  BakeScene *one = 0;
  TASK_CHECK(bake_scene_build(&one, &root, &bindings[0], 1) != OK);
  bake_scene_free(scene);
  return 0;
}

// Material constants, duplicate paths, unused bindings and invisible
// subtrees.
static int surface_binding_validation(void) {
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());

  BakeBinding good = make_binding(0, 0, 2, 2, f.uv, 3, f.idx,
                                  (Vec3f){0.5f, 0.5f, 0.5f},
                                  (Vec3f){1, 2, 3});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &good, 1) == OK);
  bake_scene_free(scene);

  BakeBinding bad = good;
  bad.reflectance = (Vec3f){1.0f, 0.5f, 0.5f}; // component must be < 1
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);
  bad.reflectance = (Vec3f){-0.1f, 0.5f, 0.5f};
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);
  bad = good;
  bad.emission = (Vec3f){0, -1, 0}; // emission non-negative
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);
  bad = good;
  bad.width = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &bad, 1) != OK);

  // Duplicate binding paths.
  BakeBinding two[2] = {good, good};
  TASK_CHECK(bake_scene_build(&scene, &root, two, 2) != OK);

  // An unused binding (path not present) fails.
  size_t missing[] = {9};
  BakeBinding extra =
      make_binding(missing, 1, 2, 2, f.uv, 3, f.idx, good.reflectance,
                   good.emission);
  BakeBinding with_extra[2] = {good, extra};
  TASK_CHECK(bake_scene_build(&scene, &root, with_extra, 2) != OK);

  // An invisible subtree is ignored and needs no binding.
  TriFixture hidden;
  tri_fixture_init(&hidden, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity hidden_entity;
  entity_init(&hidden_entity, (Renderable *)&hidden.object,
              mat4Identity());
  hidden_entity.visible = false;
  Entity kids[1] = {hidden_entity};
  Entity root2;
  entity_init(&root2, (Renderable *)&f.object, mat4Identity());
  Array arr;
  array_init(&arr, 1, kids);
  root2.children_entities = arr;
  TASK_CHECK(bake_scene_build(&scene, &root2, &good, 1) == OK);
  TASK_CHECK(bake_instance_count(scene) == 1);
  bake_scene_free(scene);
  return 0;
}

// Round-zero radiance is exactly the emission on covered texels.
static int surface_initial_emission(void) {
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());
  BakeBinding b = make_binding(0, 0, 2, 2, f.uv, 3, f.idx,
                               (Vec3f){0.1f, 0.2f, 0.3f},
                               (Vec3f){0.7f, 0.8f, 0.9f});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) == OK);
  TASK_CHECK(bake_rounds(scene) == 0);
  const BakeInstanceView *v = bake_instance_view(scene, 0);
  for (int t = 0; t < 4; t++)
    if (v->covered[t])
      TASK_CHECK(vnear(v->radiance[t], 0.7f, 0.8f, 0.9f));
    else
      TASK_CHECK(vnear(v->radiance[t], 0, 0, 0));
  bake_scene_free(scene);
  return 0;
}

// ===========================================================================
// TRANSPORT GROUP
// ===========================================================================

// Independent half-open coverage used by the reference simulator.
static float t_edge(Vec2f a, Vec2f b, Vec2f p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
static int t_top_left(Vec2f a, Vec2f b) {
  return (a.y == b.y && a.x < b.x) || (a.y > b.y);
}
static int t_inside(float w, Vec2f a, Vec2f b) {
  if (w > 1e-6f)
    return 1;
  if (w < -1e-6f)
    return 0;
  return t_top_left(a, b);
}

typedef struct {
  int w, h;
  bool *covered;
  Vec3f *l_cur;
  Vec3f *l_next;
  Vec3f refl;
  Vec3f emiss;
} Minst;

typedef struct {
  TTri *tris;
  int tri_count;
  Minst *insts;
  int inst_count;
} MScene;

static int m_nearest(float u, int n) {
  int i = (int)floorf(u * (float)n);
  if (i < 0)
    i = 0;
  if (i > n - 1)
    i = n - 1;
  return i;
}

static void m_cover(Minst *m, Vec2f ua, Vec2f ub, Vec2f uc) {
  float area = (ub.x - ua.x) * (uc.y - ua.y) -
               (ub.y - ua.y) * (uc.x - ua.x);
  if (fabsf(area) <= 1e-6f)
    return;
  Vec2f p0 = ua, p1 = ub, p2 = uc;
  if (area < 0) {
    p1 = uc;
    p2 = ub;
  }
  for (int y = 0; y < m->h; y++)
    for (int x = 0; x < m->w; x++) {
      Vec2f c = {(x + 0.5f) / (float)m->w, (y + 0.5f) / (float)m->h};
      if (t_inside(t_edge(p1, p2, c), p1, p2) &&
          t_inside(t_edge(p2, p0, c), p2, p0) &&
          t_inside(t_edge(p0, p1, c), p0, p1))
        m->covered[y * m->w + x] = true;
    }
}

static void m_step(MScene *ms, int S, float eps, Vec3f env) {
  for (int mi = 0; mi < ms->inst_count; mi++) {
    Minst *m = &ms->insts[mi];
    size_t grid = (size_t)m->w * m->h;
    for (size_t t = 0; t < grid; t++)
      m->l_next[t] = (Vec3f){0, 0, 0};
  }
  for (int mi = 0; mi < ms->inst_count; mi++) {
    Minst *m = &ms->insts[mi];
    for (int y = 0; y < m->h; y++)
      for (int x = 0; x < m->w; x++) {
        size_t t = (size_t)y * m->w + (size_t)x;
        if (!m->covered[t])
          continue;
        // The fixtures give every instance exactly one whole-square face.
        TTri *tri = 0;
        for (int i = 0; i < ms->tri_count; i++)
          if (ms->tris[i].id == mi) {
            tri = &ms->tris[i];
            break;
          }
        Vec2f center = {(x + 0.5f) / (float)m->w,
                        (y + 0.5f) / (float)m->h};
        float denom = (tri->ub.x - tri->ua.x) *
                          (tri->uc.y - tri->ua.y) -
                      (tri->ub.y - tri->ua.y) *
                          (tri->uc.x - tri->ua.x);
        float wa = ((tri->ub.x - center.x) * (tri->uc.y - center.y) -
                    (tri->ub.y - center.y) * (tri->uc.x - center.x)) /
                   denom;
        float wb = ((tri->uc.x - center.x) * (tri->ua.y - center.y) -
                    (tri->uc.y - center.y) * (tri->ua.x - center.x)) /
                   denom;
        float wc = 1.0f - wa - wb;
        Vec3f p = {wa * tri->a.x + wb * tri->b.x + wc * tri->c.x,
                   wa * tri->a.y + wb * tri->b.y + wc * tri->c.y,
                   wa * tri->a.z + wb * tri->b.z + wc * tri->c.z};
        Vec3f origin = t_add(p, t_scale(tri->n, eps));
        Vec3f sum = {0, 0, 0};
        for (int k = 0; k < S; k++) {
          Vec3f dir = t_sample(tri->n, k, S);
          THit hit;
          t_trace(ms->tris, ms->tri_count, origin, dir, eps, &hit);
          Vec3f lh;
          if (!hit.hit) {
            lh = env;
          } else if (!hit.front) {
            lh = (Vec3f){0, 0, 0};
          } else {
            Minst *target = &ms->insts[hit.id];
            int tx = m_nearest(hit.uv.x, target->w);
            int ty = m_nearest(hit.uv.y, target->h);
            size_t idx = (size_t)ty * (size_t)target->w + (size_t)tx;
            lh = target->covered[idx]
                     ? target->l_cur[idx]
                     : (Vec3f){0, 0, 0};
          }
          sum = t_add(sum, lh);
        }
        float inv = 1.0f / (float)S;
        m->l_next[t] = (Vec3f){
            m->emiss.x + m->refl.x * sum.x * inv,
            m->emiss.y + m->refl.y * sum.y * inv,
            m->emiss.z + m->refl.z * sum.z * inv};
      }
  }
  for (int mi = 0; mi < ms->inst_count; mi++) {
    Minst *m = &ms->insts[mi];
    Vec3f *tmp = m->l_cur;
    m->l_cur = m->l_next;
    m->l_next = tmp;
  }
}

// Builds the canonical two-sheet scene both as pingo objects and as the
// parallel simulator scene. Emitter id 0 at z=0 normal +z, receiver id 1 at
// z=1 normal -z. Lightmaps are 2x2 over the unit UV square.
typedef struct {
  TriFixture emitter;
  TriFixture receiver;
  Entity emitter_entity;
  Entity root;
  size_t child_path[1];
  BakeBinding bindings[2];
  TTri tris[2];
  Minst minsts[2];
  bool cover0[4];
  bool cover1[4];
  Vec3f l0[4], l0n[4], l1[4], l1n[4];
} TwoSheet;

static void two_sheet_init(TwoSheet *ts, Vec3f emitter_emission,
                           Vec3f emitter_refl, Vec3f receiver_refl) {
  // Emitter z=0, CCW seen from +z.
  tri_fixture_init(&ts->emitter, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  // Receiver z=1 wound so its normal points -z, toward the emitter.
  tri_fixture_init(&ts->receiver, (Vec3f){-1, -1, 0}, (Vec3f){1, 1, 0},
                   (Vec3f){1, -1, 0}, (Vec2f){0, 0}, (Vec2f){1, 1},
                   (Vec2f){1, 0});
  // Child world transform cancels the root's +1 translation -> emitter z=0.
  entity_init(&ts->emitter_entity, (Renderable *)&ts->emitter.object,
              mat4Translate((Vec3f){0, 0, -1}));
  entity_init(&ts->root, (Renderable *)&ts->receiver.object,
              mat4Translate((Vec3f){0, 0, 1}));
  Array kids;
  array_init(&kids, 1, &ts->emitter_entity);
  ts->root.children_entities = kids;

  ts->child_path[0] = 0;
  ts->bindings[0] = make_binding(
      ts->child_path, 1, 2, 2, ts->emitter.uv, 3, ts->emitter.idx,
      emitter_refl, emitter_emission);
  ts->bindings[1] = make_binding(0, 0, 2, 2, ts->receiver.uv, 3,
                                 ts->receiver.idx, receiver_refl,
                                 (Vec3f){0, 0, 0});

  // Parallel world triangles.
  ts->tris[0] = (TTri){{-1, -1, 0}, {1, -1, 0}, {1, 1, 0},
                       {0, 0, 1},
                       {0, 0}, {1, 0}, {1, 1}, 0};
  Vec3f rn = {0, 0, -1};
  ts->tris[1] = (TTri){{-1, -1, 1}, {1, 1, 1}, {1, -1, 1}, rn,
                       {0, 0}, {1, 1}, {1, 0}, 1};

  memset(ts->cover0, 0, sizeof(ts->cover0));
  memset(ts->cover1, 0, sizeof(ts->cover1));
  ts->minsts[0] = (Minst){2, 2, ts->cover0, ts->l0, ts->l0n,
                          emitter_refl, emitter_emission};
  ts->minsts[1] = (Minst){2, 2, ts->cover1, ts->l1, ts->l1n,
                          receiver_refl, {0, 0, 0}};
  m_cover(&ts->minsts[0], (Vec2f){0, 0}, (Vec2f){1, 0}, (Vec2f){1, 1});
  m_cover(&ts->minsts[1], (Vec2f){0, 0}, (Vec2f){1, 1}, (Vec2f){1, 0});
  for (int t = 0; t < 4; t++) {
    ts->l0[t] = ts->cover0[t] ? emitter_emission : (Vec3f){0, 0, 0};
    ts->l1[t] = (Vec3f){0, 0, 0};
  }
}

// The four fixed sample directions at a +z normal texel match the specified
// sequence exactly, including the Y auxiliary axis choice.
static int transport_sample_layout(void) {
  Vec3f n = {0, 0, 1};
  static const float expect[4][3] = {
      {0.35355339f, 0.0f, 0.93541435f},
      {-0.61237244f, 0.0f, 0.79056942f},
      {0.0f, 0.79056942f, 0.61237244f},
      {0.0f, -0.93541435f, 0.35355339f},
  };
  for (int k = 0; k < 4; k++) {
    Vec3f d = t_sample(n, k, 4);
    TASK_CHECK(fnear(d.x, expect[k][0]));
    TASK_CHECK(fnear(d.y, expect[k][1]));
    TASK_CHECK(fnear(d.z, expect[k][2]));
    TASK_CHECK(fnear(t_dot(d, d), 1.0f)); // unit length
  }
  // With |N.y| >= 0.9 the auxiliary axis switches to X.
  Vec3f up = {0, 1, 0};
  for (int k = 0; k < 4; k++) {
    Vec3f d = t_sample(up, k, 4);
    TASK_CHECK(fnear(t_dot(d, up), sqrtf(1.0f - (k + 0.5f) / 4.0f)));
  }
  return 0;
}

// No geometry in front: every ray misses and round one is emission +
// reflectance * environment, with no extra pi factor.
static int transport_all_miss_environment(void) {
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());
  Vec3f refl = {0.2f, 0.4f, 0.6f};
  Vec3f emiss = {1, 2, 3};
  BakeBinding b = make_binding(0, 0, 2, 2, f.uv, 3, f.idx, refl, emiss);
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) == OK);
  Vec3f env = {0.5f, 0.25f, 0.125f};
  TASK_CHECK(bake_step(scene, 1, 8, 1e-4f, env) == OK);
  TASK_CHECK(bake_rounds(scene) == 1);
  const BakeInstanceView *v = bake_instance_view(scene, 0);
  for (int t = 0; t < 4; t++)
    if (v->covered[t])
      TASK_CHECK(vnear(v->radiance[t], 1 + 0.2f * 0.5f,
                       2 + 0.4f * 0.25f, 3 + 0.6f * 0.125f));
  bake_scene_free(scene);
  return 0;
}

// A back-facing foreground sheet absorbs the ray even though a lit sheet
// stands behind it; the parallel simulator and production agree for two
// full rounds on every covered texel.
static int transport_backface_and_two_rounds(void) {
  TwoSheet ts;
  two_sheet_init(&ts, (Vec3f){2, 3, 4}, (Vec3f){0.1f, 0.2f, 0.3f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &ts.root, ts.bindings, 2) == OK);

  MScene ms = {ts.tris, 2, ts.minsts, 2};
  Vec3f env = {0.1f, 0.2f, 0.3f};
  const int S = 4;
  const float eps = 1e-4f;
  Vec3f round1[4];

  for (int round = 1; round <= 2; round++) {
    TASK_CHECK(bake_step(scene, 1, S, eps, env) == OK);
    m_step(&ms, S, eps, env);
    for (int mi = 0; mi < 2; mi++) {
      const BakeInstanceView *v = bake_instance_view(scene, (size_t)mi);
      for (int t = 0; t < 4; t++) {
        TASK_CHECK(v->covered[t] == ts.minsts[mi].covered[t]);
        TASK_CHECK(fnear(v->radiance[t].x, ts.minsts[mi].l_cur[t].x));
        TASK_CHECK(fnear(v->radiance[t].y, ts.minsts[mi].l_cur[t].y));
        TASK_CHECK(fnear(v->radiance[t].z, ts.minsts[mi].l_cur[t].z));
      }
    }
    if (round == 1) {
      const BakeInstanceView *v1 = bake_instance_view(scene, 1);
      for (int t = 0; t < 4; t++)
        round1[t] = v1->radiance[t];
    }
  }

  // Hard-coded round-1 receiver texels, cross-checked with an independent
  // tracer: texels (0,0) and (1,0) each take one emitter hit, texel (1,1)
  // misses the emitter's triangular half four times and sees only
  // environment; texel (0,1) is uncovered.
  TASK_CHECK(vnear(round1[0], 0.2875f, 0.45f, 0.6125f));
  TASK_CHECK(vnear(round1[1], 0.2875f, 0.45f, 0.6125f));
  TASK_CHECK(vnear(round1[3], 0.05f, 0.1f, 0.15f));
  bake_scene_free(scene);
  return 0;
}

// Equidistant hits on coincident sheets break by entity path, then face;
// the lexicographically first path supplies the sampled radiance.
static int transport_tie_break_path_order(void) {
  TwoSheet ts;
  two_sheet_init(&ts, (Vec3f){1, 0, 0}, (Vec3f){0, 0, 0},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  // Add a second emitter instance at the exact same world placement, path
  // {1}, emitting blue instead of red.
  TriFixture emitter2;
  tri_fixture_init(&emitter2, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity e2;
  entity_init(&e2, (Renderable *)&emitter2.object,
              mat4Translate((Vec3f){0, 0, -1}));
  Entity kids[2] = {ts.emitter_entity, e2};
  Array arr;
  array_init(&arr, 2, kids);
  ts.root.children_entities = arr;

  size_t p1[] = {1};
  BakeBinding extra = make_binding(p1, 1, 2, 2, emitter2.uv, 3, emitter2.idx,
                                   (Vec3f){0, 0, 0}, (Vec3f){0, 0, 1});
  BakeBinding bindings[3] = {ts.bindings[0], extra, ts.bindings[1]};
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &ts.root, bindings, 3) == OK);
  TASK_CHECK(bake_step(scene, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);

  // Every front hit is equidistant between the red path-{0} sheet and the
  // blue path-{1} sheet; path order makes red win, so no channel mixing and
  // the red-only value matches the single-emitter scene.
  const BakeInstanceView *rv = bake_instance_view(scene, 2);
  for (int t = 0; t < 4; t++)
    if (rv->covered[t]) {
      TASK_CHECK(rv->radiance[t].z <= 1e-6f); // blue never sampled
      TASK_CHECK(rv->radiance[t].x >= 0.0f);
    }
  bake_scene_free(scene);
  return 0;
}

// Two emitters of different world area but identical emitted radiance give
// the receiver identical radiance: the radiance equation has no area term.
typedef struct {
  Vec3f em_pos[4];
  Vec2f em_uv[4];
  uint16_t em_pi[6];
  uint16_t em_ui[6];
  Mesh em_mesh;
  Material em_material;
  Object em_object;
  Entity em_entity;
  TriFixture receiver;
  Entity root;
  size_t child_path[1];
  BakeBinding bindings[2];
} AreaScene;

static void area_scene_init(AreaScene *as, float half) {
  as->em_pos[0] = (Vec3f){-half, -half, 0};
  as->em_pos[1] = (Vec3f){half, -half, 0};
  as->em_pos[2] = (Vec3f){half, half, 0};
  as->em_pos[3] = (Vec3f){-half, half, 0};
  as->em_uv[0] = (Vec2f){0, 0};
  as->em_uv[1] = (Vec2f){1, 0};
  as->em_uv[2] = (Vec2f){1, 1};
  as->em_uv[3] = (Vec2f){0, 1};
  uint16_t pi[6] = {0, 1, 2, 0, 2, 3};
  uint16_t ui[6] = {0, 1, 2, 0, 2, 3};
  memcpy(as->em_pi, pi, sizeof(pi));
  memcpy(as->em_ui, ui, sizeof(ui));
  as->em_mesh.indexes_count = 6;
  as->em_mesh.pos_indices = as->em_pi;
  as->em_mesh.tex_indices = as->em_ui;
  as->em_mesh.positions = as->em_pos;
  as->em_mesh.textCoord = as->em_uv;
  as->em_material.texture = 0;
  object_init(&as->em_object, &as->em_mesh, &as->em_material);

  // Small receiver at z=2 facing the emitter (normal -z).
  tri_fixture_init(&as->receiver, (Vec3f){-0.5f, -0.5f, 0},
                   (Vec3f){0.5f, 0.5f, 0}, (Vec3f){0.5f, -0.5f, 0},
                   (Vec2f){0, 0}, (Vec2f){1, 1}, (Vec2f){1, 0});

  // The child's translate cancels the root's +2 so the emitter lands at z=0.
  entity_init(&as->em_entity, (Renderable *)&as->em_object,
              mat4Translate((Vec3f){0, 0, -2}));
  entity_init(&as->root, (Renderable *)&as->receiver.object,
              mat4Translate((Vec3f){0, 0, 2}));
  Array kids;
  array_init(&kids, 1, &as->em_entity);
  as->root.children_entities = kids;

  as->child_path[0] = 0;
  as->bindings[0] = make_binding(
      as->child_path, 1, 4, 4, as->em_uv, 4, as->em_ui,
      (Vec3f){0, 0, 0}, (Vec3f){1, 1, 1});
  as->bindings[1] = make_binding(0, 0, 2, 2, as->receiver.uv, 3,
                                 as->receiver.idx, (Vec3f){0.5f, 0.5f, 0.5f},
                                 (Vec3f){0, 0, 0});
}

static int transport_area_invariance(void) {
  AreaScene small, large;
  area_scene_init(&small, 10.0f);
  area_scene_init(&large, 20.0f);
  BakeScene *a = 0, *b = 0;
  TASK_CHECK(bake_scene_build(&a, &small.root, small.bindings, 2) == OK);
  TASK_CHECK(bake_scene_build(&b, &large.root, large.bindings, 2) == OK);
  // Black environment: any miss would read zero, so equality plus positivity
  // proves every receiver ray reached an emitting texel in both scenes.
  TASK_CHECK(bake_step(a, 1, 8, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(b, 1, 8, 1e-4f, (Vec3f){0, 0, 0}) == OK);

  const BakeInstanceView *ra = bake_instance_view(a, 1);
  const BakeInstanceView *rb = bake_instance_view(b, 1);
  for (int t = 0; t < 4; t++)
    if (ra->covered[t]) {
      TASK_CHECK(ra->radiance[t].x > 0.0f);
      TASK_CHECK(fnear(ra->radiance[t].x, rb->radiance[t].x));
      TASK_CHECK(fnear(ra->radiance[t].y, rb->radiance[t].y));
      TASK_CHECK(fnear(ra->radiance[t].z, rb->radiance[t].z));
    }
  bake_scene_free(a);
  bake_scene_free(b);
  return 0;
}

// Two mutually illuminating sheets. The same geometry is built twice with
// the child/root roles swapped, so traversal (and thus instance processing
// order) is reversed while every world triangle and material is identical.
// Double buffering makes the rounds order independent; an in-place update
// would let the later-processed sheet read the earlier one's fresh values.
static int transport_no_in_place_update(void) {
  // Sheet X at z=0 normal +z; sheet Y at z=1 normal -z.
  TriFixture x, y;
  tri_fixture_init(&x, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  tri_fixture_init(&y, (Vec3f){-1, -1, 0}, (Vec3f){1, 1, 0},
                   (Vec3f){1, -1, 0}, (Vec2f){0, 0}, (Vec2f){1, 1},
                   (Vec2f){1, 0});
  Vec3f refl = {0.4f, 0.4f, 0.4f};

  // Tree A: Y is the root (world z=1), X its cancelling-translate child.
  Entity a_child, a_root;
  entity_init(&a_child, (Renderable *)&x.object,
              mat4Translate((Vec3f){0, 0, -1}));
  entity_init(&a_root, (Renderable *)&y.object,
              mat4Translate((Vec3f){0, 0, 1}));
  Array a_kids;
  array_init(&a_kids, 1, &a_child);
  a_root.children_entities = a_kids;

  // Tree B: roles swapped - X is the root, Y the +1-translated child.
  Entity b_child, b_root;
  entity_init(&b_child, (Renderable *)&y.object,
              mat4Translate((Vec3f){0, 0, 1}));
  entity_init(&b_root, (Renderable *)&x.object, mat4Identity());
  Array b_kids;
  array_init(&b_kids, 1, &b_child);
  b_root.children_entities = b_kids;

  size_t child_path[1] = {0};
  // In A: child X binds red emission, root Y binds green.
  BakeBinding a_binds[2] = {
      make_binding(child_path, 1, 2, 2, x.uv, 3, x.idx, refl,
                   (Vec3f){1, 0, 0}),
      make_binding(0, 0, 2, 2, y.uv, 3, y.idx, refl,
                   (Vec3f){0, 1, 0}),
  };
  // In B: child Y binds green, root X binds red - same world materials.
  BakeBinding b_binds[2] = {
      make_binding(child_path, 1, 2, 2, y.uv, 3, y.idx, refl,
                   (Vec3f){0, 1, 0}),
      make_binding(0, 0, 2, 2, x.uv, 3, x.idx, refl,
                   (Vec3f){1, 0, 0}),
  };

  BakeScene *sa = 0, *sb = 0;
  TASK_CHECK(bake_scene_build(&sa, &a_root, a_binds, 2) == OK);
  TASK_CHECK(bake_scene_build(&sb, &b_root, b_binds, 2) == OK);
  // Black environment forces every transported value through another sheet.
  TASK_CHECK(bake_step(sa, 2, 8, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(sb, 2, 8, 1e-4f, (Vec3f){0, 0, 0}) == OK);

  // Gather per-sheet results by emission color, independent of instance slot.
  const BakeInstanceView *ax = bake_instance_view(sa, 0);
  const BakeInstanceView *ay = bake_instance_view(sa, 1);
  const BakeInstanceView *by = bake_instance_view(sb, 0);
  const BakeInstanceView *bx = bake_instance_view(sb, 1);
  for (int t = 0; t < 4; t++) {
    TASK_CHECK(fnear(ax->radiance[t].x, bx->radiance[t].x));
    TASK_CHECK(fnear(ax->radiance[t].y, bx->radiance[t].y));
    TASK_CHECK(fnear(ay->radiance[t].x, by->radiance[t].x));
    TASK_CHECK(fnear(ay->radiance[t].y, by->radiance[t].y));
  }
  bake_scene_free(sa);
  bake_scene_free(sb);
  return 0;
}

// ===========================================================================
// INTEGRATION GROUP
// ===========================================================================

// Stepping two rounds at once equals stepping them one at a time; reset
// restores emission and the round counter; invalid calls advance nothing.
static int integration_step_equivalence_and_reset(void) {
  TwoSheet one_shot;
  two_sheet_init(&one_shot, (Vec3f){2, 3, 4}, (Vec3f){0.1f, 0.2f, 0.3f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  TwoSheet stepped;
  two_sheet_init(&stepped, (Vec3f){2, 3, 4}, (Vec3f){0.1f, 0.2f, 0.3f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  BakeScene *a = 0, *b = 0;
  TASK_CHECK(bake_scene_build(&a, &one_shot.root, one_shot.bindings, 2) ==
             OK);
  TASK_CHECK(bake_scene_build(&b, &stepped.root, stepped.bindings, 2) ==
             OK);
  Vec3f env = {0.1f, 0.2f, 0.3f};

  TASK_CHECK(bake_step(a, 2, 4, 1e-4f, env) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, env) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, env) == OK);
  TASK_CHECK(bake_rounds(a) == 2 && bake_rounds(b) == 2);
  for (size_t mi = 0; mi < 2; mi++) {
    const BakeInstanceView *va = bake_instance_view(a, mi);
    const BakeInstanceView *vb = bake_instance_view(b, mi);
    for (int t = 0; t < 4; t++) {
      TASK_CHECK(fnear(va->radiance[t].x, vb->radiance[t].x));
      TASK_CHECK(fnear(va->radiance[t].y, vb->radiance[t].y));
      TASK_CHECK(fnear(va->radiance[t].z, vb->radiance[t].z));
    }
  }

  // Reset returns every covered texel to emission.
  TASK_CHECK(bake_reset(b) == OK);
  TASK_CHECK(bake_rounds(b) == 0);
  const BakeInstanceView *em = bake_instance_view(b, 0);
  for (int t = 0; t < 4; t++)
    if (em->covered[t])
      TASK_CHECK(vnear(em->radiance[t], 2, 3, 4));
    else
      TASK_CHECK(vnear(em->radiance[t], 0, 0, 0));
  TASK_CHECK(bake_reset(0) != OK);

  // Invalid arguments do not advance a round.
  TASK_CHECK(bake_step(a, 0, 4, 1e-4f, env) != OK);
  TASK_CHECK(bake_step(a, 1, 0, 1e-4f, env) != OK);
  TASK_CHECK(bake_step(a, 1, 4, 0.0f, env) != OK);
  TASK_CHECK(bake_step(a, 1, 4, -1.0f, env) != OK);
  TASK_CHECK(bake_step(a, 1, 4, 1e-4f, (Vec3f){-1, 0, 0}) != OK);
  TASK_CHECK(bake_step(0, 1, 4, 1e-4f, env) != OK);
  TASK_CHECK(bake_rounds(a) == 2);

  // Float radiance is never pre-clamped: values above 1 survive in the map.
  TriFixture f;
  tri_fixture_init(&f, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  Entity root;
  entity_init(&root, (Renderable *)&f.object, mat4Identity());
  BakeBinding hb = make_binding(0, 0, 1, 1, f.uv, 3, f.idx,
                                (Vec3f){0.5f, 0.5f, 0.5f},
                                (Vec3f){5, 6, 7});
  BakeScene *hot = 0;
  TASK_CHECK(bake_scene_build(&hot, &root, &hb, 1) == OK);
  TASK_CHECK(bake_step(hot, 1, 1, 1e-4f, (Vec3f){2, 2, 2}) == OK);
  const BakeInstanceView *hv = bake_instance_view(hot, 0);
  TASK_CHECK(hv->radiance[0].x > 1.0f && hv->radiance[0].y > 1.0f &&
             hv->radiance[0].z > 1.0f);
  bake_scene_free(hot);
  bake_scene_free(a);
  bake_scene_free(b);
  return 0;
}

// The environment only affects later rounds and never resets the scene.
static int integration_environment_latched(void) {
  TwoSheet s1, s2;
  two_sheet_init(&s1, (Vec3f){1, 1, 1}, (Vec3f){0.2f, 0.2f, 0.2f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  two_sheet_init(&s2, (Vec3f){1, 1, 1}, (Vec3f){0.2f, 0.2f, 0.2f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  BakeScene *a = 0, *b = 0;
  TASK_CHECK(bake_scene_build(&a, &s1.root, s1.bindings, 2) == OK);
  TASK_CHECK(bake_scene_build(&b, &s2.root, s2.bindings, 2) == OK);

  TASK_CHECK(bake_step(a, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  // Round two differs only because the environment changed; the counter
  // keeps climbing and the first round is shared history.
  TASK_CHECK(bake_step(a, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, (Vec3f){0.9f, 0.7f, 0.5f}) == OK);
  TASK_CHECK(bake_rounds(a) == 2 && bake_rounds(b) == 2);
  int differs = 0;
  for (size_t mi = 0; mi < 2; mi++) {
    const BakeInstanceView *va = bake_instance_view(a, mi);
    const BakeInstanceView *vb = bake_instance_view(b, mi);
    for (int t = 0; t < 4; t++)
      if (!fnear(va->radiance[t].x, vb->radiance[t].x) ||
          !fnear(va->radiance[t].y, vb->radiance[t].y))
        differs = 1;
  }
  TASK_CHECK(differs);
  bake_scene_free(a);
  bake_scene_free(b);
  return 0;
}

// After the source tree, meshes, bindings and materials are released, the
// owned scene still bakes and serves identical results.
static int integration_source_released(void) {
  Entity *root = calloc(1, sizeof(Entity));
  Entity *child = calloc(1, sizeof(Entity));
  TriFixture *em = calloc(1, sizeof(TriFixture));
  TriFixture *rc = calloc(1, sizeof(TriFixture));
  tri_fixture_init(em, (Vec3f){-1, -1, 0}, (Vec3f){1, -1, 0},
                   (Vec3f){1, 1, 0}, (Vec2f){0, 0}, (Vec2f){1, 0},
                   (Vec2f){1, 1});
  tri_fixture_init(rc, (Vec3f){-1, -1, 0}, (Vec3f){1, 1, 0},
                   (Vec3f){1, -1, 0}, (Vec2f){0, 0}, (Vec2f){1, 1},
                   (Vec2f){1, 0});
  entity_init(child, (Renderable *)&em->object,
              mat4Translate((Vec3f){0, 0, -1}));
  entity_init(root, (Renderable *)&rc->object,
              mat4Translate((Vec3f){0, 0, 1}));
  Array kids;
  array_init(&kids, 1, child);
  root->children_entities = kids;

  size_t *path = malloc(sizeof(size_t));
  path[0] = 0;
  Vec2f *uv_copy = malloc(3 * sizeof(Vec2f));
  uint16_t *idx_copy = malloc(3 * sizeof(uint16_t));
  memcpy(uv_copy, em->uv, 3 * sizeof(Vec2f));
  memcpy(idx_copy, em->idx, 3 * sizeof(uint16_t));
  BakeBinding *bindings = calloc(2, sizeof(BakeBinding));
  bindings[0] = make_binding(path, 1, 2, 2, uv_copy, 3, idx_copy,
                             (Vec3f){0.1f, 0.2f, 0.3f},
                             (Vec3f){2, 3, 4});
  bindings[1] = make_binding(0, 0, 2, 2, uv_copy, 3, idx_copy,
                             (Vec3f){0.5f, 0.5f, 0.5f},
                             (Vec3f){0, 0, 0});

  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, root, bindings, 2) == OK);

  // Release every source object.
  free(path);
  free(uv_copy);
  free(idx_copy);
  free(bindings);
  free(em);
  free(rc);
  free(child);
  free(root);

  TASK_CHECK(bake_step(scene, 2, 4, 1e-4f, (Vec3f){0.1f, 0.2f, 0.3f}) ==
             OK);
  TASK_CHECK(bake_rounds(scene) == 2);
  TASK_CHECK(bake_instance_count(scene) == 2);
  const BakeInstanceView *v = bake_instance_view(scene, 0);
  int lit = 0;
  for (int t = 0; t < 4; t++) {
    TASK_CHECK(isfinite(v->radiance[t].x));
    if (v->covered[t])
      lit++;
  }
  TASK_CHECK(lit == 3);
  bake_scene_free(scene);
  return 0;
}

// Two scenes interleaved never share state.
static int integration_two_scenes_interleaved(void) {
  TwoSheet s1, s2;
  two_sheet_init(&s1, (Vec3f){1, 0, 0}, (Vec3f){0.1f, 0.1f, 0.1f},
                 (Vec3f){0.5f, 0.5f, 0.5f});
  two_sheet_init(&s2, (Vec3f){0, 0, 1}, (Vec3f){0.2f, 0.2f, 0.2f},
                 (Vec3f){0.25f, 0.25f, 0.25f});
  BakeScene *a = 0, *b = 0;
  TASK_CHECK(bake_scene_build(&a, &s1.root, s1.bindings, 2) == OK);
  TASK_CHECK(bake_scene_build(&b, &s2.root, s2.bindings, 2) == OK);

  TASK_CHECK(bake_step(a, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(a, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(b, 1, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);

  // Reference: each scene run straight through with the same arguments.
  BakeScene *ra = 0, *rb = 0;
  TASK_CHECK(bake_scene_build(&ra, &s1.root, s1.bindings, 2) == OK);
  TASK_CHECK(bake_scene_build(&rb, &s2.root, s2.bindings, 2) == OK);
  TASK_CHECK(bake_step(ra, 2, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);
  TASK_CHECK(bake_step(rb, 2, 4, 1e-4f, (Vec3f){0, 0, 0}) == OK);

  for (int which = 0; which < 2; which++) {
    BakeScene *live = which ? b : a;
    BakeScene *ref = which ? rb : ra;
    for (size_t mi = 0; mi < 2; mi++) {
      const BakeInstanceView *vl = bake_instance_view(live, mi);
      const BakeInstanceView *vr = bake_instance_view(ref, mi);
      for (int t = 0; t < 4; t++) {
        TASK_CHECK(fnear(vl->radiance[t].x, vr->radiance[t].x));
        TASK_CHECK(fnear(vl->radiance[t].y, vr->radiance[t].y));
        TASK_CHECK(fnear(vl->radiance[t].z, vr->radiance[t].z));
      }
    }
  }
  bake_scene_free(a);
  bake_scene_free(b);
  bake_scene_free(ra);
  bake_scene_free(rb);
  return 0;
}

// --- Camera pixel fixtures ------------------------------------------------

#define CAM_W 64
#define CAM_H 48

// A triangle whose vertices sit at three different depths, so clip-space w
// varies across the face and the secondary UV must be perspective corrected.
static Vec3f cam_pos[3] = {{-1.0f, -0.8f, -2.0f},
                           {1.0f, -0.8f, -4.0f},
                           {0.0f, 1.0f, -3.0f}};
static Vec2f cam_uv[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
static uint16_t cam_idx[3] = {0, 1, 2};

typedef struct {
  Mesh mesh;
  Material material;
  Object object;
  Entity entity;
} CamPiece;

static void cam_piece_init(CamPiece *p, Vec3f shift, Texture *tex,
                           Vec3f emission, Vec3f refl) {
  p->mesh.indexes_count = 3;
  p->mesh.pos_indices = cam_idx;
  p->mesh.tex_indices = cam_idx;
  p->mesh.positions = cam_pos;
  p->mesh.textCoord = cam_uv;
  p->material.texture = tex;
  object_init(&p->object, &p->mesh, &p->material);
  entity_init(&p->entity, (Renderable *)&p->object, mat4Translate(shift));
  (void)emission;
  (void)refl;
}

static uint8_t truncate_channel(float v) {
  if (v < 0)
    v = 0;
  if (v > 1)
    v = 1;
  return (uint8_t)(v * 255.0f);
}

// Renders emission-only (round zero) triangles and checks real framebuffer
// pixels, a camera change, a material base-color swap and alpha.
static int integration_camera_pixels(void) {
  // Two pieces share the same w-varying mesh, shifted left and right.
  static Pixel white_tex_pixels[4];
  static Pixel gray_tex_pixels[4];
  for (int i = 0; i < 4; i++) {
    white_tex_pixels[i] = pixelFromRGBA(255, 255, 255, 255);
    gray_tex_pixels[i] = pixelFromRGBA(128, 128, 128, 128);
  }
  Texture white_tex, gray_tex;
  texture_init(&white_tex, (Vec2i){2, 2}, white_tex_pixels);
  texture_init(&gray_tex, (Vec2i){2, 2}, gray_tex_pixels);

  CamPiece red_piece, blue_piece;
  cam_piece_init(&red_piece, (Vec3f){-1.4f, 0, 0}, &white_tex,
                 (Vec3f){1, 0, 0}, (Vec3f){0, 0, 0});
  cam_piece_init(&blue_piece, (Vec3f){1.4f, 0, 0}, &white_tex,
                 (Vec3f){0, 0, 1}, (Vec3f){0, 0, 0});

  Entity pieces[2] = {red_piece.entity, blue_piece.entity};
  Entity root;
  entity_init(&root, (Renderable *)&red_piece.object, mat4Identity());
  Array kids;
  array_init(&kids, 2, pieces);
  root.children_entities = kids;
  // Root itself must not draw: give it an empty mesh object.
  Mesh empty_mesh = {0, 0, 0, 0, 0};
  Object root_object;
  object_init(&root_object, &empty_mesh, 0);
  root.entity_renderable = (Renderable *)&root_object;

  size_t paths[2][1] = {{0}, {1}};
  BakeBinding bindings[3] = {
      make_binding(paths[0], 1, 2, 2, cam_uv, 3, cam_idx,
                   (Vec3f){0, 0, 0}, (Vec3f){1, 0, 0}),
      make_binding(paths[1], 1, 2, 2, cam_uv, 3, cam_idx,
                   (Vec3f){0, 0, 0}, (Vec3f){0, 0, 1}),
      // The root's own empty object is bound on the empty path.
      make_binding(0, 0, 2, 2, cam_uv, 3, cam_idx,
                   (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0}),
  };
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == OK);
  // Round zero: L is pure emission. Traversal visits children first, so the
  // two pieces are instances 0 and 1.
  const BakeInstanceView *rv = bake_instance_view(scene, 0);
  const BakeInstanceView *bv = bake_instance_view(scene, 1);
  TASK_CHECK(rv->covered[0] && bv->covered[0]);

  MemoryBackend backend;
  TASK_CHECK(memory_backend_init(&backend, (Vec2i){CAM_W, CAM_H}) == 0);
  Renderer renderer;
  TASK_CHECK(renderer_init(&renderer, (Vec2i){CAM_W, CAM_H},
                           (Backend *)&backend) == OK);
  renderer.camera_projection =
      mat4Perspective(1, 50.0f, (float)CAM_W / (float)CAM_H, 0.6f);
  renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
  TASK_CHECK(renderer_set_root_renderable(&renderer,
                                          (Renderable *)&root) == OK);

  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) == OK);
  // Find the reddest and bluest lit pixels.
  int red_best = -1, blue_best = -1;
  for (int i = 0; i < CAM_W * CAM_H; i++) {
    Pixel p = backend.frame[i];
    if (p.r > 40 && p.b < p.r / 2 &&
        (red_best < 0 || p.r > backend.frame[red_best].r))
      red_best = i;
    if (p.b > 40 && p.r < p.b / 2 &&
        (blue_best < 0 || p.b > backend.frame[blue_best].b))
      blue_best = i;
  }
  TASK_CHECK(red_best >= 0 && blue_best >= 0);
  TASK_CHECK(backend.frame[red_best].r == 255);
  TASK_CHECK(backend.frame[red_best].g == 0);
  TASK_CHECK(backend.frame[blue_best].b == 255);
  TASK_CHECK(backend.frame[blue_best].r == 0);
  // White base texture keeps alpha 255.
  TASK_CHECK(backend.frame[red_best].a == 255);

  // Exposure scales before truncation.
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 0.25f) == OK);
  TASK_CHECK(backend.frame[red_best].r == truncate_channel(0.25f));

  // Swap the blue piece's base texture for the gray one: color halves and
  // alpha follows the base texture (128).
  blue_piece.material.texture = &gray_tex;
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) == OK);
  TASK_CHECK(backend.frame[blue_best].b == 128);
  TASK_CHECK(backend.frame[blue_best].a == 128);
  blue_piece.material.texture = &white_tex;

  // Moving the camera needs no re-baking; the frame still renders and still
  // contains both saturated pieces.
  renderer.camera_view = mat4Translate((Vec3f){0.3f, 0, 0});
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) == OK);
  int reds = 0, blues = 0;
  for (int i = 0; i < CAM_W * CAM_H; i++) {
    if (backend.frame[i].r > 40 && backend.frame[i].b < 20)
      reds++;
    if (backend.frame[i].b > 40 && backend.frame[i].r < 20)
      blues++;
  }
  TASK_CHECK(reds > 0 && blues > 0);

  // A mismatched live transform is rejected without touching the backend.
  renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
  root.transform = mat4Translate((Vec3f){0.25f, 0, 0});
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) != OK);
  root.transform = mat4Identity();

  // clear == false is rejected.
  renderer.clear = false;
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) != OK);
  renderer.clear = true;

  memory_backend_free(&backend);
  bake_scene_free(scene);
  return 0;
}

// Backend that hands out a fresh buffer from inside beforeRender; the
// lightmapped entry must fetch the current buffer and publish into the new
// one while leaving the old one untouched.
typedef struct {
  Backend backend;
  Vec2i size;
  Pixel *frame_a;
  Pixel *frame_b;
  PingoDepth *depth_a;
  PingoDepth *depth_b;
  int use_b;
  int before_count;
  int after_count;
} SwapBackend;

static void swap_before(Renderer *r, Backend *be) {
  SwapBackend *s = (SwapBackend *)be;
  s->use_b = 1;
  s->before_count++;
  (void)r;
}
static void swap_after(Renderer *r, Backend *be) {
  ((SwapBackend *)be)->after_count++;
  (void)r;
}
static Pixel *swap_frame(Renderer *r, Backend *be) {
  (void)r;
  SwapBackend *s = (SwapBackend *)be;
  return s->use_b ? s->frame_b : s->frame_a;
}
static PingoDepth *swap_depth(Renderer *r, Backend *be) {
  (void)r;
  SwapBackend *s = (SwapBackend *)be;
  return s->use_b ? s->depth_b : s->depth_a;
}
static void swap_init(Renderer *r, Backend *be, Vec4i rect) {
  (void)r;
  (void)be;
  (void)rect;
}

static int integration_backend_buffer_swap(void) {
  SwapBackend sb;
  memset(&sb, 0, sizeof(sb));
  sb.size = (Vec2i){CAM_W, CAM_H};
  size_t pixels = CAM_W * CAM_H;
  sb.frame_a = calloc(pixels, sizeof(Pixel));
  sb.frame_b = calloc(pixels, sizeof(Pixel));
  sb.depth_a = calloc(pixels, sizeof(PingoDepth));
  sb.depth_b = calloc(pixels, sizeof(PingoDepth));
  TASK_CHECK(sb.frame_a && sb.frame_b && sb.depth_a && sb.depth_b);
  sb.backend.init = swap_init;
  sb.backend.beforeRender = swap_before;
  sb.backend.afterRender = swap_after;
  sb.backend.getFrameBuffer = swap_frame;
  sb.backend.getZetaBuffer = swap_depth;

  CamPiece piece;
  cam_piece_init(&piece, (Vec3f){0, 0, 0}, 0, (Vec3f){1, 1, 1},
                 (Vec3f){0, 0, 0});
  Entity root;
  entity_init(&root, (Renderable *)&piece.object, mat4Identity());
  BakeBinding b = make_binding(0, 0, 2, 2, cam_uv, 3, cam_idx,
                               (Vec3f){0, 0, 0}, (Vec3f){1, 1, 1});
  BakeScene *scene = 0;
  TASK_CHECK(bake_scene_build(&scene, &root, &b, 1) == OK);

  Renderer renderer;
  TASK_CHECK(renderer_init(&renderer, (Vec2i){CAM_W, CAM_H},
                           (Backend *)&sb) == OK);
  renderer.camera_projection =
      mat4Perspective(1, 50.0f, (float)CAM_W / (float)CAM_H, 0.6f);
  renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
  TASK_CHECK(renderer_set_root_renderable(&renderer,
                                          (Renderable *)&root) == OK);

  // Poison the original buffer; a failed validation must never call back.
  root.transform = mat4Translate((Vec3f){9, 9, 9});
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) != OK);
  TASK_CHECK(sb.before_count == 0 && sb.after_count == 0);
  TASK_CHECK(sb.use_b == 0);

  root.transform = mat4Identity();
  TASK_CHECK(renderer_render_lightmapped(&renderer, scene, 1.0f) == OK);
  TASK_CHECK(sb.before_count == 1 && sb.after_count == 1);
  int lit_b = 0, lit_a = 0;
  for (size_t i = 0; i < pixels; i++) {
    if (sb.frame_b[i].r || sb.frame_b[i].g || sb.frame_b[i].b)
      lit_b++;
    if (sb.frame_a[i].r || sb.frame_a[i].g || sb.frame_a[i].b)
      lit_a++;
  }
  TASK_CHECK(lit_b > 0);  // published into the buffer swapped in beforeRender
  TASK_CHECK(lit_a == 0); // original buffer never touched

  free(sb.frame_a);
  free(sb.frame_b);
  free(sb.depth_a);
  free(sb.depth_b);
  bake_scene_free(scene);
  return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

const struct task_case task_cases[] = {
    // surface
    {"surface", "square_coverage", surface_square_coverage},
    {"surface", "coverage_conflict", surface_coverage_conflict},
    {"surface", "zero_area_and_bad_uv", surface_zero_area_and_bad_uv},
    {"surface", "shared_object_two_instances",
     surface_shared_object_two_instances},
    {"surface", "binding_validation", surface_binding_validation},
    {"surface", "initial_emission", surface_initial_emission},
    // transport
    {"transport", "sample_layout", transport_sample_layout},
    {"transport", "all_miss_environment", transport_all_miss_environment},
    {"transport", "backface_and_two_rounds",
     transport_backface_and_two_rounds},
    {"transport", "tie_break_path_order", transport_tie_break_path_order},
    {"transport", "area_invariance", transport_area_invariance},
    {"transport", "no_in_place_update", transport_no_in_place_update},
    // integration
    {"integration", "step_equivalence_and_reset",
     integration_step_equivalence_and_reset},
    {"integration", "environment_latched", integration_environment_latched},
    {"integration", "source_released", integration_source_released},
    {"integration", "two_scenes_interleaved",
     integration_two_scenes_interleaved},
    {"integration", "camera_pixels", integration_camera_pixels},
    {"integration", "backend_buffer_swap", integration_backend_buffer_swap},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
