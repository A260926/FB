/**
 * Regression tests for the lightmap baker and the lightmapped render path.
 *
 * Every scene is built literally in C so the expected values can be derived
 * by hand: the transport tests recompute the sample rays, the nearest
 * intersections and the per-round radiance independently of the
 * implementation, and the integration tests compare the rendered frame
 * against an analytic replica of the rasterizer.
 */

#include "task_support/check.h"

#include "render/lightmap.h"
#include "render/depth.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/tests/memory_backend.h"
#include "render/texture.h"
#include "math/fun.h"
#include "math/mat4.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

static int close3(Vec3f v, float x, float y, float z) {
  const float t = 1e-3f;
  return fabsf(v.x - x) <= t && fabsf(v.y - y) <= t && fabsf(v.z - z) <= t;
}

static int texel_close(const BakeScene *scene, int instance, int x, int y,
                       float r, float g, float b) {
  BakeTexel texel;
  if (bake_scene_texel(scene, instance, x, y, &texel) != 0)
    return 0;
  return close3(texel.radiance, r, g, b);
}

/* The fixed hemisphere direction of the spec, recomputed here so the
 * expected transport values do not depend on the implementation. */
static void spec_direction(unsigned k, unsigned s, Vec3f n, double out[3]) {
  double u = (k + 0.5) / s;
  double v;
  double r, phi, lx, ly, lz;
  double ax = 0, ay = 0, az = 0;
  double tx, ty, tz, tl, bx, by, bz;
  {
    double x = 0.0, f = 0.5;
    unsigned kk = k;
    while (kk) {
      if (kk & 1u)
        x += f;
      kk >>= 1;
      f *= 0.5;
    }
    v = x;
  }
  r = sqrt(u);
  phi = 2.0 * 3.14159265358979323846 * v;
  lx = r * cos(phi);
  ly = r * sin(phi);
  lz = sqrt(1.0 - u);
  if (fabs((double)n.y) < 0.9)
    ay = 1.0;
  else
    ax = 1.0;
  tx = ay * n.z - az * n.y;
  ty = az * n.x - ax * n.z;
  tz = ax * n.y - ay * n.x;
  tl = sqrt(tx * tx + ty * ty + tz * tz);
  tx /= tl;
  ty /= tl;
  tz /= tl;
  bx = n.y * tz - n.z * ty;
  by = n.z * tx - n.x * tz;
  bz = n.x * ty - n.y * tx;
  out[0] = tx * lx + bx * ly + n.x * lz;
  out[1] = ty * lx + by * ly + n.y * lz;
  out[2] = tz * lx + bz * ly + n.z * lz;
}

/* Independent ray/triangle intersection (Moller-Trumbore, double sided). */
static int spec_ray_tri(Vec3f p0, Vec3f p1, Vec3f p2, const double o[3],
                        const double d[3], double min_t, double *t_out,
                        double *u_out, double *v_out) {
  double e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
  double e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
  double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                  d[0] * e2[1] - d[1] * e2[0]};
  double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
  double inv, tv[3], u, qv[3], v, t;
  if (det == 0.0)
    return 0;
  inv = 1.0 / det;
  tv[0] = o[0] - p0.x;
  tv[1] = o[1] - p0.y;
  tv[2] = o[2] - p0.z;
  u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
  if (u < 0.0 || u > 1.0)
    return 0;
  qv[0] = tv[1] * e1[2] - tv[2] * e1[1];
  qv[1] = tv[2] * e1[0] - tv[0] * e1[2];
  qv[2] = tv[0] * e1[1] - tv[1] * e1[0];
  v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
  if (v < 0.0 || u + v > 1.0)
    return 0;
  t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
  if (t <= min_t)
    return 0;
  *t_out = t;
  *u_out = u;
  *v_out = v;
  return 1;
}

/* An empty Object for inner entities; it has no triangles and simply
 * needs its own (empty) binding like every visible Object. */
static Mesh empty_mesh = {0, 0, 0, 0, 0};
static Object empty_object;
static int empty_object_ready = 0;

static void prepare_empty_object(void) {
  if (!empty_object_ready) {
    object_init(&empty_object, &empty_mesh, 0);
    empty_object_ready = 1;
  }
}

static void attach_children(Entity *parent, Entity *children, size_t count) {
  parent->children_entities.size = count;
  parent->children_entities.data = count ? children : 0;
}

static BakeBinding binding_for(const size_t *path, size_t path_length, int w,
                               int h, const Vec2f *uvs, size_t uv_count,
                               const uint16_t *uv_indices, Vec3f reflectance,
                               Vec3f emission) {
  BakeBinding b;
  b.path = path;
  b.path_length = path_length;
  b.width = w;
  b.height = h;
  b.uvs = uvs;
  b.uv_count = uv_count;
  b.uv_indices = uv_indices;
  b.reflectance = reflectance;
  b.emission = emission;
  return b;
}

static BakeBinding empty_binding(const size_t *path, size_t path_length) {
  return binding_for(path, path_length, 1, 1, 0, 0, 0, (Vec3f){0, 0, 0},
                     (Vec3f){0, 0, 0});
}

/* ------------------------------------------------------------------------
 * surface: one triangle Object shared by two entities with non-uniform
 * (and one reflecting) transforms.
 * --------------------------------------------------------------------- */

static Vec3f s1_positions[3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
static uint16_t s1_indices[3] = {0, 1, 2};
static Vec2f s1_uvs[3] = {{0, 0}, {1, 0}, {0, 1}};
static uint16_t s1_uv_indices[3] = {0, 1, 2};

static int surface_shared_object_instances(void) {
  Mesh mesh = {3, s1_indices, s1_indices, s1_positions, s1_uvs};
  Object object;
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];
  BakeTexel texel;
  int i0, i1;

  prepare_empty_object();
  object_init(&object, &mesh, 0);
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&object, mat4Scale((Vec3f){2, 3, 1}));
  entity_init(&children[1], (Renderable *)&object,
              mat4Scale((Vec3f){-1, 1, 2}));
  attach_children(&root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == 0);

  /* The shared Object is two instances, never merged by pointer. */
  TASK_CHECK(scene.instance_count == 3);
  i0 = bake_scene_find(&scene, p0, 1);
  i1 = bake_scene_find(&scene, p1, 1);
  TASK_CHECK(i0 >= 0 && i1 >= 0 && i0 != i1);
  TASK_CHECK(bake_scene_find(&scene, 0, 0) >= 0);

  /* Texel (0,0) of the 1x1 map sits at UV (0.5,0.5), the midpoint of the
   * hypotenuse: barycentric (0, 0.5, 0.5) through the transformed
   * vertices. */
  TASK_CHECK(bake_scene_texel(&scene, i0, 0, 0, &texel) == 0);
  TASK_CHECK(texel.covered && texel.face == 0);
  TASK_CHECK(close3(texel.point, 1.0f, 1.5f, 0.0f));
  TASK_CHECK(close3(texel.normal, 0.0f, 0.0f, 1.0f));

  /* The reflection flips the winding, so the world normal flips too. */
  TASK_CHECK(bake_scene_texel(&scene, i1, 0, 0, &texel) == 0);
  TASK_CHECK(texel.covered && texel.face == 0);
  TASK_CHECK(close3(texel.point, -0.5f, 0.5f, 0.0f));
  TASK_CHECK(close3(texel.normal, 0.0f, 0.0f, -1.0f));

  /* Frozen world geometry survives independently of the source mesh. */
  TASK_CHECK(close3(scene.instances[i0].tris[0].p1, 2.0f, 0.0f, 0.0f));
  TASK_CHECK(close3(scene.instances[i0].tris[0].p2, 0.0f, 3.0f, 0.0f));
  TASK_CHECK(close3(scene.instances[i1].tris[0].p1, -1.0f, 0.0f, 0.0f));
  TASK_CHECK(close3(scene.instances[i1].tris[0].p2, 0.0f, 1.0f, 0.0f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * surface: mirrored UVs, shared-edge ownership, zero-area UV faces.
 * --------------------------------------------------------------------- */

static Vec3f s2_positions[4] = {
    {-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1},
};
static uint16_t s2_indices[6] = {0, 1, 3, 1, 2, 3};
/* Both UV triangles are mirrored (negative signed area). */
static Vec2f s2_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
static uint16_t s2_uv_indices[6] = {0, 1, 3, 1, 2, 3};

static int surface_mirrored_uv_shared_edge(void) {
  Mesh mesh = {6, s2_indices, s2_indices, s2_positions, s2_uvs};
  Object object;
  Entity root;
  BakeScene scene;
  BakeBinding bindings[1];
  BakeTexel texel;

  object_init(&object, &mesh, 0);
  entity_init(&root, (Renderable *)&object, mat4Identity());
  attach_children(&root, 0, 0);

  bindings[0] = binding_for(0, 0, 1, 1, s2_uvs, 4, s2_uv_indices,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 1) == 0);

  /* The texel centre (0.5,0.5) lies exactly on the shared edge of the two
   * mirrored UV triangles; the half-open rule hands it to exactly one
   * face (face 0 here), with no conflict reported. */
  TASK_CHECK(bake_scene_texel(&scene, 0, 0, 0, &texel) == 0);
  TASK_CHECK(texel.covered);
  TASK_CHECK(texel.face == 0);
  TASK_CHECK(close3(texel.point, 0.0f, 0.0f, 0.0f));
  TASK_CHECK(close3(texel.normal, 0.0f, 1.0f, 0.0f));

  bake_scene_free(&scene);
  return 0;
}

static uint16_t s3_uv_indices[6] = {0, 1, 2, 3, 3, 3};

static int surface_zero_area_uv_ignored(void) {
  /* Face 0 maps to a proper UV triangle, face 1 collapses to a point and
   * must be ignored by the coverage pass (not an error, no texels). */
  Mesh mesh = {6, s2_indices, s2_indices, s2_positions, s2_uvs};
  Object object;
  Entity root;
  BakeScene scene;
  BakeBinding bindings[1];
  BakeTexel texel;
  Vec2f uvs[4] = {{0, 0}, {1, 0}, {0, 1}, {0.5f, 0.5f}};

  object_init(&object, &mesh, 0);
  entity_init(&root, (Renderable *)&object, mat4Identity());
  attach_children(&root, 0, 0);

  bindings[0] = binding_for(0, 0, 1, 1, uvs, 4, s3_uv_indices,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 1) == 0);
  TASK_CHECK(bake_scene_texel(&scene, 0, 0, 0, &texel) == 0);
  TASK_CHECK(texel.covered && texel.face == 0);
  TASK_CHECK(close3(texel.point, 0.0f, 0.0f, 0.0f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * surface: positive-area coverage conflicts are reported, and a failed
 * build keeps the previous scene alive.
 * --------------------------------------------------------------------- */

static int surface_coverage_conflict_reported(void) {
  Mesh good_mesh = {3, s1_indices, s1_indices, s1_positions, s1_uvs};
  Object good_object;
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding good_bindings[3];
  BakeBinding bad_bindings[3];
  /* Face 1 nests inside face 0 in UV space: positive-area overlap. */
  Vec2f conflict_uvs[6] = {
      {0, 0}, {1, 0}, {0, 1}, {0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f},
  };
  /* The same mesh with disjoint UV triangles builds fine. */
  Vec2f disjoint_uvs[6] = {
      {0, 0}, {0.4f, 0}, {0, 1}, {0.6f, 0}, {1, 0}, {0.6f, 1},
  };
  uint16_t conflict_indices[6] = {0, 1, 2, 3, 4, 5};
  Vec3f conflict_positions[6] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                                 {0, 0, 1}, {1, 0, 1}, {0, 1, 1}};
  Mesh conflict_mesh = {6, conflict_indices, conflict_indices,
                        conflict_positions, conflict_uvs};
  Object conflict_object;

  prepare_empty_object();
  object_init(&good_object, &good_mesh, 0);
  object_init(&conflict_object, &conflict_mesh, 0);
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&good_object, mat4Identity());
  entity_init(&children[1], (Renderable *)&conflict_object, mat4Identity());
  attach_children(&root, children, 2);

  good_bindings[0] = empty_binding(0, 0);
  good_bindings[1] = binding_for(p0, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                                 (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){1, 1, 1});
  good_bindings[2] = binding_for(p1, 1, 4, 4, disjoint_uvs, 6, conflict_indices,
                                 (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, good_bindings, 3) == 0);
  TASK_CHECK(scene.instance_count == 3);
  TASK_CHECK(texel_close(&scene, bake_scene_find(&scene, p0, 1), 0, 0, 1, 1, 1));

  /* Same tree, but the second instance now has overlapping UVs: the
   * build must fail and the previous scene must be kept. */
  bad_bindings[0] = empty_binding(0, 0);
  bad_bindings[1] = binding_for(p0, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                                (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){2, 2, 2});
  bad_bindings[2] = binding_for(p1, 1, 4, 4, conflict_uvs, 6, conflict_indices,
                                (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  TASK_CHECK(bake_scene_build(&scene, &root, bad_bindings, 3) != 0);
  TASK_CHECK(scene.instance_count == 3);
  TASK_CHECK(texel_close(&scene, bake_scene_find(&scene, p0, 1), 0, 0, 1, 1, 1));
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, bake_scene_find(&scene, p0, 1), 0, 0, 1, 1, 1));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * surface: binding validation and the invisible-subtree rule.
 * --------------------------------------------------------------------- */

static int surface_binding_validation(void) {
  Mesh mesh = {3, s1_indices, s1_indices, s1_positions, s1_uvs};
  Object object;
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1}, p9[1] = {9};
  BakeBinding bindings[3];
  Vec2f bad_uvs[3] = {{0, 0}, {1.5f, 0}, {0, 1}};
  uint16_t bad_indices[3] = {0, 1, 9};

  prepare_empty_object();
  object_init(&object, &mesh, 0);
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&object, mat4Identity());
  entity_init(&children[1], (Renderable *)&object, mat4Identity());
  attach_children(&root, children, 2);

  bake_scene_init(&scene);

  /* Reflectance components must be in [0,1). */
  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){1.0f, 0, 0}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0});
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[1].reflectance = (Vec3f){-0.1f, 0, 0};
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[1].reflectance = (Vec3f){0.5f, 0.5f, 0.5f};

  /* Emission must be non-negative. */
  bindings[1].emission = (Vec3f){0, -1, 0};
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[1].emission = (Vec3f){0, 0, 0};

  /* UVs must stay inside [0,1]. */
  bindings[1].uvs = bad_uvs;
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[1].uvs = s1_uvs;

  /* UV indices must reference the bound UV array. */
  bindings[1].uv_indices = bad_indices;
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[1].uv_indices = s1_uv_indices;

  /* A visible Object without a binding, and a binding without a visible
   * Object, are both rejected. */
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 2) != 0);
  bindings[2] = binding_for(p9, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0});
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);

  /* Duplicate bindings for one path are rejected as well. */
  bindings[2] = binding_for(p0, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0});
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) != 0);
  bindings[2] = binding_for(p1, 1, 1, 1, s1_uvs, 3, s1_uv_indices,
                            (Vec3f){0, 0, 0}, (Vec3f){0, 0, 0});
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == 0);
  TASK_CHECK(scene.instance_count == 3);
  bake_scene_free(&scene);

  /* An invisible subtree is ignored entirely: its Object needs no
   * binding and produces no instance. */
  children[1].visible = false;
  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 2) == 0);
  TASK_CHECK(scene.instance_count == 2);
  TASK_CHECK(bake_scene_find(&scene, p1, 1) < 0);
  bake_scene_free(&scene);

  return 0;
}

/* ------------------------------------------------------------------------
 * surface: the scene owns its data; freeing every source changes nothing.
 * --------------------------------------------------------------------- */

static Vec3f s5_floor_positions[4] = {
    {-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {1, 0, -1},
};
static uint16_t s5_quad_indices[6] = {0, 1, 3, 1, 2, 3};
static Vec2f s5_quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};

static Vec3f s5_wall_positions[4] = {
    {-2, 0, 1}, {2, 0, 1}, {2, 2, 1}, {-2, 2, 1},
};
static uint16_t s5_wall_indices[6] = {0, 3, 1, 3, 2, 1};

static int surface_source_freed(void) {
  /* Everything the scene is built from is heap-allocated and released
   * right after the build; baking and reading must be unaffected. */
  Entity *root = calloc(1, sizeof(Entity));
  Entity *children = calloc(2, sizeof(Entity));
  Object *objects = calloc(2, sizeof(Object));
  Mesh *meshes = calloc(2, sizeof(Mesh));
  Vec3f *positions = malloc(sizeof(Vec3f) * 8);
  Vec2f *uvs = malloc(sizeof(Vec2f) * 8);
  uint16_t *indices = malloc(sizeof(uint16_t) * 12);
  uint16_t *uv_indices = malloc(sizeof(uint16_t) * 12);
  BakeBinding *bindings = calloc(3, sizeof(BakeBinding));
  size_t *paths = malloc(sizeof(size_t) * 2);
  BakeScene scene;
  BakeTexel texel;

  memcpy(positions, s5_floor_positions, sizeof(Vec3f) * 4);
  memcpy(positions + 4, s5_wall_positions, sizeof(Vec3f) * 4);
  memcpy(uvs, s5_quad_uvs, sizeof(Vec2f) * 4);
  memcpy(uvs + 4, s5_quad_uvs, sizeof(Vec2f) * 4);
  memcpy(indices, s5_quad_indices, sizeof(uint16_t) * 6);
  memcpy(indices + 6, s5_wall_indices, sizeof(uint16_t) * 6);
  memcpy(uv_indices, s5_quad_indices, sizeof(uint16_t) * 6);
  memcpy(uv_indices + 6, s5_wall_indices, sizeof(uint16_t) * 6);

  meshes[0] = (Mesh){6, indices, indices, positions, uvs};
  meshes[1] = (Mesh){6, indices + 6, indices + 6, positions + 4, uvs + 4};
  object_init(&objects[0], &meshes[0], 0);
  object_init(&objects[1], &meshes[1], 0);

  prepare_empty_object();
  entity_init(root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(root, children, 2);

  paths[0] = 0;
  paths[1] = 1;
  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(&paths[0], 1, 1, 1, uvs, 4, uv_indices,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(&paths[1], 1, 1, 1, uvs + 4, 4, uv_indices + 6,
                            (Vec3f){0.5f, 0.25f, 0.1f},
                            (Vec3f){1.0f, 0.5f, 0.25f});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, root, bindings, 3) == 0);

  free(root);
  free(children);
  free(objects);
  free(meshes);
  free(positions);
  free(uvs);
  free(indices);
  free(uv_indices);
  free(bindings);
  free(paths);

  /* The floor sees the wall exactly as in the hand-computed scene. */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.0f, 0.5f, 0.25f));
  TASK_CHECK(bake_scene_texel(&scene, 1, 0, 0, &texel) == 0);
  TASK_CHECK(close3(texel.point, 0, 0, 0));
  TASK_CHECK(close3(texel.normal, 0, 1, 0));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: the floor/wall scene with hand-computed rounds.
 *
 * Floor at y=0 (normal +Y, reflectance 0.5), wall at z=1 (normal -Z,
 * emission (1,0.5,0.25), reflectance (0.5,0.25,0.1)). With S=1 the single
 * sample direction from the floor's centre is (0,1,1)/sqrt(2): it hits
 * the wall's front face at (0,1.001,1), UV (0.5,0.5005) -> texel (0,0).
 * The wall's own ray leaves towards (-1,0,-1)/sqrt(2) and misses.
 * --------------------------------------------------------------------- */

static void build_floor_wall(BakeScene *scene, Entity *root, Entity *children,
                             Object *objects, Mesh *meshes, Vec3f wall_emission,
                             Vec3f wall_reflectance, Vec3f floor_reflectance) {
  static Vec3f wall_pos[4] = {{-2, 0, 1}, {2, 0, 1}, {2, 2, 1}, {-2, 2, 1}};
  static uint16_t wall_idx[6] = {0, 3, 1, 3, 2, 1};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  static uint16_t floor_idx[6] = {0, 1, 3, 1, 2, 3};
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];

  meshes[0] = (Mesh){6, floor_idx, floor_idx, s5_floor_positions, quad_uvs};
  meshes[1] = (Mesh){6, wall_idx, wall_idx, wall_pos, quad_uvs};
  object_init(&objects[0], &meshes[0], 0);
  object_init(&objects[1], &meshes[1], 0);

  prepare_empty_object();
  entity_init(root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, quad_uvs, 4, floor_idx,
                            floor_reflectance, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, wall_idx,
                            wall_reflectance, wall_emission);
  bake_scene_build(scene, root, bindings, 3);
}

static int transport_two_rounds(void) {
  BakeScene scene;
  Entity root, children[2];
  Object objects[2];
  Mesh meshes[2];
  double dir[3], origin[3], t, u, v;
  Vec3f n = {0, 1, 0};

  bake_scene_init(&scene);
  build_floor_wall(&scene, &root, children, objects, meshes,
                   (Vec3f){1.0f, 0.5f, 0.25f}, (Vec3f){0.5f, 0.25f, 0.1f},
                   (Vec3f){0.5f, 0.5f, 0.5f});
  TASK_CHECK(scene.instance_count == 3);

  /* Independently recompute the floor's sample ray and its wall hit. */
  spec_direction(0, 1, n, dir);
  TASK_CHECK(fabs(dir[0] - 0.0) < 1e-9 && fabs(dir[1] - sqrt(0.5)) < 1e-9 &&
             fabs(dir[2] - sqrt(0.5)) < 1e-9);
  origin[0] = 0.0;
  origin[1] = 0.001;
  origin[2] = 0.0;
  TASK_CHECK(spec_ray_tri((Vec3f){-2, 2, 1}, (Vec3f){2, 2, 1}, (Vec3f){2, 0, 1},
                          origin, dir, 0.001, &t, &u, &v));
  TASK_CHECK(fabs(t - sqrt(2.0)) < 1e-6);
  /* Hit (0, 1.001, 1) on wall face 1: UV (0.5, 0.5005) -> texel (0,0). */
  {
    double w0 = 1.0 - u - v;
    double hu = w0 * 0.0 + u * 1.0 + v * 1.0;
    double hv = w0 * 1.0 + u * 1.0 + v * 0.0;
    TASK_CHECK(fabs(hu - 0.5) < 1e-6 && fabs(hv - 0.5005) < 1e-6);
  }

  /* Round 1, environment 0: floor gets half the wall emission. */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.0f, 0.5f, 0.25f));
  TASK_CHECK(scene.rounds == 1);

  /* Round 2 with a new environment: only later rounds see it. The floor
   * still reads the wall's round-1 value (its emission). */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0.2f, 0.4f, 0.6f}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.1f, 0.6f, 0.31f));

  /* Round 3: the floor now reads the wall's round-2 value. */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0.2f, 0.4f, 0.6f}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.55f, 0.3f, 0.155f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.1f, 0.6f, 0.31f));
  TASK_CHECK(scene.rounds == 3);

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: a foreground quad seen from its back face absorbs the ray;
 * the contribution is zero, not the environment and not its emission.
 * --------------------------------------------------------------------- */

static int transport_backface_absorbs(void) {
  static Vec3f occ_pos[4] = {{-1, 1, 0}, {1, 1, 0}, {1, 1, 2}, {-1, 1, 2}};
  static uint16_t occ_idx[6] = {0, 3, 1, 3, 2, 1};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  static uint16_t floor_idx[6] = {0, 1, 3, 1, 2, 3};
  Mesh meshes[2];
  Object objects[2];
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];

  meshes[0] = (Mesh){6, floor_idx, floor_idx, s5_floor_positions, quad_uvs};
  meshes[1] = (Mesh){6, occ_idx, occ_idx, occ_pos, quad_uvs};
  object_init(&objects[0], &meshes[0], 0);
  object_init(&objects[1], &meshes[1], 0);

  prepare_empty_object();
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(&root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, quad_uvs, 4, floor_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, occ_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){9, 9, 9});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == 0);

  /* The occluder faces the same way as the floor (+Y), so the floor's
   * ray leaves through its back face: absorbed. Environment (5,5,5) and
   * emission 9 must not leak into the floor. */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){5, 5, 5}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.0f, 0.0f, 0.0f));
  /* The occluder's own ray misses and collects the environment. */
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 11.5f, 11.5f, 11.5f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: one round must not see its own new values.
 *
 * Ceiling B (child 0, processed first) reflects; floor A (child 1) emits
 * (2,2,2). After round 1, A must still be exactly its emission: an
 * in-place update would let A see B's brand-new round-1 value.
 * --------------------------------------------------------------------- */

static int transport_no_inplace_update(void) {
  static Vec3f ceil_pos[4] = {{-1, 1, 0}, {1, 1, 0}, {1, 1, 2}, {-1, 1, 2}};
  static uint16_t ceil_idx[6] = {0, 1, 2, 0, 2, 3};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  static uint16_t floor_idx[6] = {0, 1, 3, 1, 2, 3};
  Mesh meshes[2];
  Object objects[2];
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];

  meshes[0] = (Mesh){6, ceil_idx, ceil_idx, ceil_pos, quad_uvs};
  meshes[1] = (Mesh){6, floor_idx, floor_idx, s5_floor_positions, quad_uvs};
  object_init(&objects[0], &meshes[0], 0);
  object_init(&objects[1], &meshes[1], 0);

  prepare_empty_object();
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(&root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, quad_uvs, 4, ceil_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, floor_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){2, 2, 2});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == 0);

  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  /* B (instance 1) gathered A's emission; A (instance 2) must be
   * untouched by B's new value within the same round. */
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 1.0f, 1.0f, 1.0f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 2.0f, 2.0f, 2.0f));

  /* Round 2: A finally sees B's round-1 value. */
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 1.0f, 1.0f, 1.0f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 2.5f, 2.5f, 2.5f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: two emitters with different areas but equal radiance give
 * the same gather; two scenes interleaved; split steps equal one shot.
 * --------------------------------------------------------------------- */

static void build_emitter_scene(BakeScene *scene, Entity *root,
                                Entity *children, Object *objects, Mesh *meshes,
                                const Vec3f *emitter_positions, int big) {
  static Vec3f recv_pos[4] = {
      {-0.5f, 0, -0.5f}, {-0.5f, 0, 0.5f}, {0.5f, 0, 0.5f}, {0.5f, 0, -0.5f},
  };
  static uint16_t quad_idx[6] = {0, 1, 3, 1, 2, 3};
  static uint16_t emit_idx[6] = {0, 1, 2, 0, 2, 3};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];

  meshes[0] = (Mesh){6, quad_idx, quad_idx, recv_pos, quad_uvs};
  meshes[1] = (Mesh){6, emit_idx, emit_idx, emitter_positions, quad_uvs};
  object_init(&objects[0], &meshes[0], 0);
  object_init(&objects[1], &meshes[1], 0);

  prepare_empty_object();
  entity_init(root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, quad_uvs, 4, quad_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, emit_idx,
                            (Vec3f){0, 0, 0}, (Vec3f){0.8f, 0.4f, 0.2f});
  (void)big;
  bake_scene_build(scene, root, bindings, 3);
}

static int transport_equal_radiance_interleaved(void) {
  static Vec3f small_emitter[4] = {
      {-0.5f, 1, 0.5f}, {0.5f, 1, 0.5f}, {0.5f, 1, 1.5f}, {-0.5f, 1, 1.5f},
  };
  static Vec3f big_emitter[4] = {
      {-2, 1, -1}, {2, 1, -1}, {2, 1, 3}, {-2, 1, 3},
  };
  BakeScene small_a, small_b, big;
  Entity root_s, root_b, children_s[2], children_b[2];
  Object objects_s[2], objects_b[2];
  Mesh meshes_s[2], meshes_b[2];
  size_t i;

  bake_scene_init(&small_a);
  bake_scene_init(&small_b);
  bake_scene_init(&big);
  build_emitter_scene(&small_a, &root_s, children_s, objects_s, meshes_s,
                      small_emitter, 0);
  build_emitter_scene(&big, &root_b, children_b, objects_b, meshes_b,
                      big_emitter, 1);
  /* A second, identical small scene stepped independently to compare
   * split stepping against one-shot stepping. */
  {
    Entity root_t, children_t[2];
    Object objects_t[2];
    Mesh meshes_t[2];
    build_emitter_scene(&small_b, &root_t, children_t, objects_t, meshes_t,
                        small_emitter, 0);
  }

  /* Interleave the two scenes round by round: radiance is radiance,
   * independent of the emitter's area, and the scenes share nothing. */
  TASK_CHECK(bake_step(&small_a, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(bake_step(&big, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&small_a, 1, 0, 0, 0.4f, 0.2f, 0.1f));
  TASK_CHECK(texel_close(&big, 1, 0, 0, 0.4f, 0.2f, 0.1f));
  TASK_CHECK(bake_step(&small_a, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(bake_step(&big, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&small_a, 1, 0, 0, 0.4f, 0.2f, 0.1f));
  TASK_CHECK(texel_close(&big, 1, 0, 0, 0.4f, 0.2f, 0.1f));
  /* Both emitters keep their own radiance despite the area difference. */
  TASK_CHECK(texel_close(&small_a, 2, 0, 0, 0.8f, 0.4f, 0.2f));
  TASK_CHECK(texel_close(&big, 2, 0, 0, 0.8f, 0.4f, 0.2f));

  /* Split stepping (1+1) must equal one shot (2), bit for bit. */
  TASK_CHECK(bake_step(&small_b, 2, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(small_a.instance_count == small_b.instance_count);
  for (i = 0; i < small_a.instance_count; i++) {
    const BakeInstance *a = &small_a.instances[i];
    const BakeInstance *b = &small_b.instances[i];
    TASK_CHECK(a->width == b->width && a->height == b->height);
    TASK_CHECK(memcmp(a->radiance, b->radiance,
                      (size_t)(a->width * a->height) * sizeof(Vec3f)) == 0);
  }
  TASK_CHECK(small_a.rounds == small_b.rounds);

  bake_scene_free(&small_a);
  bake_scene_free(&small_b);
  bake_scene_free(&big);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: reset returns to emission; invalid arguments never advance.
 * --------------------------------------------------------------------- */

static int transport_reset_and_invalid(void) {
  BakeScene scene;
  Entity root, children[2];
  Object objects[2];
  Mesh meshes[2];

  bake_scene_init(&scene);
  build_floor_wall(&scene, &root, children, objects, meshes,
                   (Vec3f){1.0f, 0.5f, 0.25f}, (Vec3f){0.5f, 0.25f, 0.1f},
                   (Vec3f){0.5f, 0.5f, 0.5f});

  TASK_CHECK(bake_step(&scene, 2, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));
  TASK_CHECK(scene.rounds == 2);

  /* Invalid parameters: no round is advanced, values stay put. */
  TASK_CHECK(bake_step(&scene, -1, 1, 0.001f, (Vec3f){0, 0, 0}) != 0);
  TASK_CHECK(bake_step(&scene, 1, 0, 0.001f, (Vec3f){0, 0, 0}) != 0);
  TASK_CHECK(bake_step(&scene, 1, -4, 0.001f, (Vec3f){0, 0, 0}) != 0);
  TASK_CHECK(bake_step(&scene, 1, 1, -0.5f, (Vec3f){0, 0, 0}) != 0);
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){-1, 0, 0}) != 0);
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f,
                       (Vec3f){NAN, 0, 0}) != 0);
  TASK_CHECK(bake_step(0, 1, 1, 0.001f, (Vec3f){0, 0, 0}) != 0);
  TASK_CHECK(scene.rounds == 2);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));

  /* Zero rounds is a valid no-op. */
  TASK_CHECK(bake_step(&scene, 0, 1, 0.001f, (Vec3f){1, 1, 1}) == 0);
  TASK_CHECK(scene.rounds == 2);

  /* Reset returns every covered texel to emission and restarts the
   * round count; the next round matches a fresh scene's first round. */
  bake_reset(&scene);
  TASK_CHECK(scene.rounds == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.0f, 0.0f, 0.0f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.0f, 0.5f, 0.25f));
  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  TASK_CHECK(texel_close(&scene, 1, 0, 0, 0.5f, 0.25f, 0.125f));
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 1.0f, 0.5f, 0.25f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * transport: equal-distance hits break ties by entity path, then face.
 * --------------------------------------------------------------------- */

static int transport_tie_break_path(void) {
  /* Two instances of the SAME Object at the SAME transform: identical
   * world triangles. The receiver's ray hits both at bitwise-equal t;
   * the lexicographically smaller path ([0], red) must win. */
  static Vec3f emit_pos[4] = {{-1, 1, 0}, {1, 1, 0}, {1, 1, 2}, {-1, 1, 2}};
  static uint16_t emit_idx[6] = {0, 1, 2, 0, 2, 3};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  static uint16_t floor_idx[6] = {0, 1, 3, 1, 2, 3};
  Mesh emit_mesh = {6, emit_idx, emit_idx, emit_pos, quad_uvs};
  Mesh floor_mesh = {6, floor_idx, floor_idx, s5_floor_positions, quad_uvs};
  Object objects[3];
  Entity root, children[3];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1}, p2[1] = {2};
  BakeBinding bindings[4];

  object_init(&objects[0], &emit_mesh, 0);
  object_init(&objects[1], &emit_mesh, 0); /* shared Object, two mounts */
  object_init(&objects[2], &floor_mesh, 0);

  prepare_empty_object();
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  entity_init(&children[2], (Renderable *)&objects[2], mat4Identity());
  attach_children(&root, children, 3);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 1, 1, quad_uvs, 4, emit_idx,
                            (Vec3f){0, 0, 0}, (Vec3f){1, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, emit_idx,
                            (Vec3f){0, 0, 0}, (Vec3f){0, 1, 0});
  bindings[3] = binding_for(p2, 1, 1, 1, quad_uvs, 4, floor_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 4) == 0);
  TASK_CHECK(scene.instance_count == 4);

  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  /* Receiver is the last instance (path [2]). */
  TASK_CHECK(texel_close(&scene, 3, 0, 0, 0.5f, 0.0f, 0.0f));

  bake_scene_free(&scene);
  return 0;
}

static int transport_tie_break_face(void) {
  /* One instance, one triangle duplicated as faces 0 and 1. Face 0 maps
   * to a covered texel, face 1 to a degenerate (ignored) UV. Equal t:
   * the lower face number wins, so the receiver sees the emission. */
  static Vec3f dup_pos[3] = {{-1, 1, 0}, {1, 1, 0}, {0, 1, 2}};
  static uint16_t dup_idx[6] = {0, 1, 2, 0, 1, 2};
  static Vec2f dup_uvs[4] = {{0, 0}, {1, 0}, {0.4f, 1}, {0.9f, 0.9f}};
  static uint16_t dup_uv_idx[6] = {0, 1, 2, 3, 3, 3};
  static Vec3f recv_pos[4] = {
      {-1.5f, 0, -1}, {-1.5f, 0, 1}, {0.5f, 0, 1}, {0.5f, 0, -1},
  };
  static uint16_t quad_idx[6] = {0, 1, 3, 1, 2, 3};
  static Vec2f quad_uvs[4] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
  Mesh emit_mesh = {6, dup_idx, dup_idx, dup_pos, quad_uvs};
  Mesh recv_mesh = {6, quad_idx, quad_idx, recv_pos, quad_uvs};
  Object objects[2];
  Entity root, children[2];
  BakeScene scene;
  size_t p0[1] = {0}, p1[1] = {1};
  BakeBinding bindings[3];

  object_init(&objects[0], &emit_mesh, 0);
  object_init(&objects[1], &recv_mesh, 0);

  prepare_empty_object();
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&children[0], (Renderable *)&objects[0], mat4Identity());
  entity_init(&children[1], (Renderable *)&objects[1], mat4Identity());
  attach_children(&root, children, 2);

  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(p0, 1, 2, 1, dup_uvs, 4, dup_uv_idx,
                            (Vec3f){0, 0, 0}, (Vec3f){1, 0, 0});
  bindings[2] = binding_for(p1, 1, 1, 1, quad_uvs, 4, quad_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f}, (Vec3f){0, 0, 0});

  bake_scene_init(&scene);
  TASK_CHECK(bake_scene_build(&scene, &root, bindings, 3) == 0);

  /* Face 0 covers texel (0,0) of the 2x1 map; texel (1,0) stays
   * uncovered because face 1's UVs collapse to a point. */
  {
    BakeTexel texel;
    TASK_CHECK(bake_scene_texel(&scene, 1, 0, 0, &texel) == 0);
    TASK_CHECK(texel.covered && texel.face == 0);
    TASK_CHECK(bake_scene_texel(&scene, 1, 1, 0, &texel) == 0);
    TASK_CHECK(!texel.covered);
  }

  TASK_CHECK(bake_step(&scene, 1, 1, 0.001f, (Vec3f){0, 0, 0}) == 0);
  /* The receiver's ray hits the duplicated triangle; face 0's covered
   * texel (red) wins over face 1's uncovered one (zero). */
  TASK_CHECK(texel_close(&scene, 2, 0, 0, 0.5f, 0.0f, 0.0f));

  bake_scene_free(&scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * integration: analytic replica of the lightmapped rasterizer.
 *
 * This draws the integration scene's objects exactly like the production
 * path should: same transforms, coverage, depth and shading math, but
 * written independently against the spec.
 * --------------------------------------------------------------------- */

typedef struct {
  const Vec3f *positions;
  const Vec2f *primary_uvs;
  const uint16_t *pos_indices;
  const uint16_t *primary_indices;
  const Vec2f *lm_uvs;
  const uint16_t *lm_indices;
  int indexes_count;
  int instance;          // bake instance for radiance lookups
  const Texture *texture; // NULL: white base colour
} ReplicaObject;

static uint8_t replica_channel(float base, float radiance, float exposure) {
  float c = base / 255.0f * radiance * exposure;
  if (c < 0.0f)
    c = 0.0f;
  if (c > 1.0f)
    c = 1.0f;
  return (uint8_t)(c * 255.0f);
}

static void replica_draw(const ReplicaObject *obj, const BakeScene *scene,
                         Mat4 m, Mat4 camera_view, Mat4 projection,
                         Pixel *frame, PingoDepth *zbuffer, Vec2i size,
                         float exposure) {
  Mat4 v = mat4Inverse(&camera_view);
  int i;

  for (i = 0; i < obj->indexes_count; i += 3) {
    Vec3f ver1 = obj->positions[obj->pos_indices[i + 0]];
    Vec3f ver2 = obj->positions[obj->pos_indices[i + 1]];
    Vec3f ver3 = obj->positions[obj->pos_indices[i + 2]];
    Vec2f tca = {0, 0}, tcb = {0, 0}, tcc = {0, 0};
    Vec2f la = obj->lm_uvs[obj->lm_indices[i + 0]];
    Vec2f lb = obj->lm_uvs[obj->lm_indices[i + 1]];
    Vec2f lc = obj->lm_uvs[obj->lm_indices[i + 2]];
    Vec4f a = {ver1.x, ver1.y, ver1.z, 1};
    Vec4f b = {ver2.x, ver2.y, ver2.z, 1};
    Vec4f c = {ver3.x, ver3.y, ver3.z, 1};
    Mat4 vm = mat4MultiplyM(&v, &m);
    float aw, bw, cw, halfX, halfY;
    Vec2i a_s, b_s, c_s, minTriangle;
    int32_t minX, minY, maxX, maxY, area;
    float areaInverse;
    int32_t A01, B01, A12, B12, A20, B20;
    int32_t w0_row, w1_row, w2_row;
    float invAw = 0, invBw = 0, invCw = 0;
    int y, x;

    if (obj->primary_uvs && obj->primary_indices) {
      tca = obj->primary_uvs[obj->primary_indices[i + 0]];
      tcb = obj->primary_uvs[obj->primary_indices[i + 1]];
      tcc = obj->primary_uvs[obj->primary_indices[i + 2]];
    }

    a = mat4MultiplyVec4(&a, &vm);
    b = mat4MultiplyVec4(&b, &vm);
    c = mat4MultiplyVec4(&c, &vm);
    a = mat4MultiplyVec4(&a, &projection);
    b = mat4MultiplyVec4(&b, &projection);
    c = mat4MultiplyVec4(&c, &projection);

    if (a.z > 0 && b.z > 0 && c.z > 0)
      continue;

    aw = a.w;
    bw = b.w;
    cw = c.w;
    a.x /= a.w; a.y /= a.w; a.z /= a.w;
    b.x /= b.w; b.y /= b.w; b.z /= b.w;
    c.x /= c.w; c.y /= c.w; c.z /= c.w;

    /* Backface culling is enabled by default. */
    if (isClockWise(a.x, a.y, b.x, b.y, c.x, c.y) >= 0)
      continue;

    halfX = size.x * 0.5f;
    halfY = size.y * 0.5f;
    a_s = (Vec2i){(int)(a.x * halfX + halfX), (int)(a.y * halfY + halfY)};
    b_s = (Vec2i){(int)(b.x * halfX + halfX), (int)(b.y * halfY + halfY)};
    c_s = (Vec2i){(int)(c.x * halfX + halfX), (int)(c.y * halfY + halfY)};

    minX = MIN(MIN(a_s.x, b_s.x), c_s.x);
    minY = MIN(MIN(a_s.y, b_s.y), c_s.y);
    maxX = MAX(MAX(a_s.x, b_s.x), c_s.x);
    maxY = MAX(MAX(a_s.y, b_s.y), c_s.y);
    minX = MIN(MAX(minX, 0), size.x);
    minY = MIN(MAX(minY, 0), size.y);
    maxX = MIN(MAX(maxX, 0), size.x);
    maxY = MIN(MAX(maxY, 0), size.y);

    minTriangle = (Vec2i){minX, minY};
    area = orient2d(a_s, b_s, c_s);
    if (area == 0)
      continue;
    areaInverse = 1.0f / area;

    A01 = a_s.y - b_s.y;
    B01 = b_s.x - a_s.x;
    A12 = b_s.y - c_s.y;
    B12 = c_s.x - b_s.x;
    A20 = c_s.y - a_s.y;
    B20 = a_s.x - c_s.x;
    w0_row = orient2d(b_s, c_s, minTriangle);
    w1_row = orient2d(c_s, a_s, minTriangle);
    w2_row = orient2d(a_s, b_s, minTriangle);

    if (aw != 0 && bw != 0 && cw != 0) {
      invAw = 1.0f / aw;
      invBw = 1.0f / bw;
      invCw = 1.0f / cw;
      tca.x *= invAw; tca.y *= invAw;
      tcb.x *= invBw; tcb.y *= invBw;
      tcc.x *= invCw; tcc.y *= invCw;
      la.x *= invAw; la.y *= invAw;
      lb.x *= invBw; lb.y *= invBw;
      lc.x *= invCw; lc.y *= invCw;
    }

    for (y = minY; y < maxY; y++, w0_row += B12, w1_row += B20, w2_row += B01) {
      int32_t w0 = w0_row, w1 = w1_row, w2 = w2_row;
      for (x = minX; x < maxX; x++, w0 += A12, w1 += A20, w2 += A01) {
        float depth, interpInvW, su, sv;
        int pixel_index, tx, ty;
        Pixel base = {255, 255, 255, 255};
        BakeTexel texel;

        if ((w0 | w1 | w2) < 0)
          continue;
        depth = -(w0 * a.z + w1 * b.z + w2 * c.z) * areaInverse;
        if (depth < -1.0f || depth > 1.0f)
          continue;
        pixel_index = x + y * size.x;
        if (depth_check(zbuffer, pixel_index, depth))
          continue;
        depth_write(zbuffer, pixel_index, depth);

        interpInvW = w0 * invAw + w1 * invBw + w2 * invCw;
        if (interpInvW == 0)
          continue;

        if (obj->texture) {
          float tu = (w0 * tca.x + w1 * tcb.x + w2 * tcc.x) / interpInvW;
          float tv = (w0 * tca.y + w1 * tcb.y + w2 * tcc.y) / interpInvW;
          base = texture_readF((Texture *)obj->texture, (Vec2f){tu, tv});
        }

        su = (w0 * la.x + w1 * lb.x + w2 * lc.x) / interpInvW;
        sv = (w0 * la.y + w1 * lb.y + w2 * lc.y) / interpInvW;
        tx = (int)floorf(su * scene->instances[obj->instance].width);
        ty = (int)floorf(sv * scene->instances[obj->instance].height);
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        if (tx > scene->instances[obj->instance].width - 1)
          tx = scene->instances[obj->instance].width - 1;
        if (ty > scene->instances[obj->instance].height - 1)
          ty = scene->instances[obj->instance].height - 1;

        memset(&texel, 0, sizeof(texel));
        bake_scene_texel(scene, obj->instance, tx, ty, &texel);

        frame[pixel_index] = pixelFromRGBA(
            replica_channel((float)base.r, texel.radiance.x, exposure),
            replica_channel((float)base.g, texel.radiance.y, exposure),
            replica_channel((float)base.b, texel.radiance.z, exposure),
            base.a);
      }
    }
  }
}

/* ------------------------------------------------------------------------
 * integration scene: receiver quad (2x2 lightmap) facing the camera and
 * a red emitter panel that only texel (1,0) can see.
 * --------------------------------------------------------------------- */

static Vec3f it_recv_pos[4] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}};
static uint16_t it_recv_idx[6] = {0, 1, 3, 1, 2, 3};
static Vec2f it_quad_uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

static Vec3f it_emit_pos[4] = {{2, 0, 1}, {3, 0, 1}, {3, 1, 1}, {2, 1, 1}};
static uint16_t it_emit_idx[6] = {0, 3, 1, 3, 2, 1};

typedef struct {
  Entity root, children[2];
  Object objects[2];
  Mesh meshes[2];
  BakeScene scene;
  size_t p0[1], p1[1];
} IntegrationFixture;

static void integration_fixture_build(IntegrationFixture *f) {
  BakeBinding bindings[3];

  f->meshes[0] = (Mesh){6, it_recv_idx, it_recv_idx, it_recv_pos, it_quad_uvs};
  f->meshes[1] = (Mesh){6, it_emit_idx, it_emit_idx, it_emit_pos, it_quad_uvs};
  object_init(&f->objects[0], &f->meshes[0], 0);
  object_init(&f->objects[1], &f->meshes[1], 0);

  prepare_empty_object();
  entity_init(&f->root, (Renderable *)&empty_object, mat4Identity());
  entity_init(&f->children[0], (Renderable *)&f->objects[0], mat4Identity());
  entity_init(&f->children[1], (Renderable *)&f->objects[1], mat4Identity());
  attach_children(&f->root, f->children, 2);

  f->p0[0] = 0;
  f->p1[0] = 1;
  bindings[0] = empty_binding(0, 0);
  bindings[1] = binding_for(f->p0, 1, 2, 2, it_quad_uvs, 4, it_recv_idx,
                            (Vec3f){0.5f, 0.5f, 0.5f},
                            (Vec3f){0.2f, 0.2f, 0.2f});
  bindings[2] = binding_for(f->p1, 1, 1, 1, it_quad_uvs, 4, it_emit_idx,
                            (Vec3f){0, 0, 0}, (Vec3f){2, 0, 0});

  bake_scene_init(&f->scene);
  bake_scene_build(&f->scene, &f->root, bindings, 3);
  bake_step(&f->scene, 1, 1, 0.001f, (Vec3f){0, 0, 0});
}

/* Projects a world point the way the rasterizer would. */
static Vec2i project_for_test(Vec3f world, Mat4 camera_view, Mat4 projection,
                              Vec2i size) {
  Mat4 v = mat4Inverse(&camera_view);
  Vec4f p = {world.x, world.y, world.z, 1};
  p = mat4MultiplyVec4(&p, &v);
  p = mat4MultiplyVec4(&p, &projection);
  p.x /= p.w;
  p.y /= p.w;
  return (Vec2i){(int)(p.x * size.x * 0.5f + size.x * 0.5f),
                 (int)(p.y * size.y * 0.5f + size.y * 0.5f)};
}

static int integration_camera_pixels(void) {
  IntegrationFixture f;
  MemoryBackend backend;
  Renderer renderer;
  Mat4 camera1, camera2, projection, rot, trans;
  Pixel expected[64 * 48];
  PingoDepth expected_depth[64 * 48];
  ReplicaObject replica[2];
  Vec2i size = {64, 48};
  int i, covered;
  Vec2i s00, s10, s01, s11;

  integration_fixture_build(&f);
  TASK_CHECK(f.scene.instance_count == 3);
  /* The bake itself: only texel (1,0) caught the red emitter. */
  TASK_CHECK(texel_close(&f.scene, 1, 0, 0, 0.2f, 0.2f, 0.2f));
  TASK_CHECK(texel_close(&f.scene, 1, 1, 0, 1.2f, 0.2f, 0.2f));
  TASK_CHECK(texel_close(&f.scene, 1, 0, 1, 0.2f, 0.2f, 0.2f));
  TASK_CHECK(texel_close(&f.scene, 1, 1, 1, 0.2f, 0.2f, 0.2f));

  TASK_CHECK(memory_backend_init(&backend, size) == 0);
  TASK_CHECK(renderer_init(&renderer, size, (Backend *)&backend) == 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&f.root);

  rot = mat4RotateY(0.15f);
  trans = mat4Translate((Vec3f){1, 1, 3});
  camera1 = mat4MultiplyM(&rot, &trans);
  projection = mat4Perspective(1, 50, 64.0f / 48.0f, 0.8f);
  renderer.camera_view = camera1;
  renderer.camera_projection = projection;

  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);

  /* Analytic replica of the same frame. */
  memset(expected, 0, sizeof(expected));
  memset(expected_depth, 0, sizeof(expected_depth));
  replica[0] = (ReplicaObject){it_recv_pos, it_quad_uvs, it_recv_idx,
                               it_recv_idx, it_quad_uvs, it_recv_idx, 6,
                               bake_scene_find(&f.scene, f.p0, 1), 0};
  replica[1] = (ReplicaObject){it_emit_pos, 0, it_emit_idx, 0,
                               it_quad_uvs, it_emit_idx, 6,
                               bake_scene_find(&f.scene, f.p1, 1), 0};
  replica_draw(&replica[0], &f.scene, mat4Identity(), camera1, projection,
               expected, expected_depth, size, 1.0f);
  replica_draw(&replica[1], &f.scene, mat4Identity(), camera1, projection,
               expected, expected_depth, size, 1.0f);
  TASK_CHECK(memcmp(backend.frame, expected, sizeof(expected)) == 0);

  /* Hand-computed anchors at the projected texel centres. */
  s00 = project_for_test((Vec3f){0.5f, 0.5f, 0}, camera1, projection, size);
  s10 = project_for_test((Vec3f){1.5f, 0.5f, 0}, camera1, projection, size);
  s01 = project_for_test((Vec3f){0.5f, 1.5f, 0}, camera1, projection, size);
  s11 = project_for_test((Vec3f){1.5f, 1.5f, 0}, camera1, projection, size);
  {
    Pixel p00 = backend.frame[s00.y * 64 + s00.x];
    Pixel p10 = backend.frame[s10.y * 64 + s10.x];
    Pixel p01 = backend.frame[s01.y * 64 + s01.x];
    Pixel p11 = backend.frame[s11.y * 64 + s11.x];
    TASK_CHECK(p00.r == 51 && p00.g == 51 && p00.b == 51 && p00.a == 255);
    TASK_CHECK(p10.r == 255 && p10.g == 51 && p10.b == 51 && p10.a == 255);
    TASK_CHECK(p01.r == 51 && p01.g == 51 && p01.b == 51);
    TASK_CHECK(p11.r == 51 && p11.g == 51 && p11.b == 51);
  }

  /* The triangles really do have varying clip w (perspective matters). */
  {
    Mat4 v = mat4Inverse(&camera1);
    Mat4 ident = mat4Identity();
    Vec4f a = {0, 0, 0, 1}, b = {2, 2, 0, 1};
    Mat4 vm = mat4MultiplyM(&v, &ident);
    a = mat4MultiplyVec4(&a, &vm);
    b = mat4MultiplyVec4(&b, &vm);
    a = mat4MultiplyVec4(&a, &projection);
    b = mat4MultiplyVec4(&b, &projection);
    TASK_CHECK(fabsf(a.w - b.w) > 0.05f);
  }

  /* Something was actually drawn. */
  covered = 0;
  for (i = 0; i < 64 * 48; i++) {
    if (backend.frame[i].r || backend.frame[i].g || backend.frame[i].b)
      covered++;
  }
  TASK_CHECK(covered > 200);

  /* Camera 2: same bake, different view, no rebaking. */
  camera2 = mat4Translate((Vec3f){1, 1, 3.5f});
  renderer.camera_view = camera2;
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);
  memset(expected, 0, sizeof(expected));
  memset(expected_depth, 0, sizeof(expected_depth));
  replica_draw(&replica[0], &f.scene, mat4Identity(), camera2, projection,
               expected, expected_depth, size, 1.0f);
  replica_draw(&replica[1], &f.scene, mat4Identity(), camera2, projection,
               expected, expected_depth, size, 1.0f);
  TASK_CHECK(memcmp(backend.frame, expected, sizeof(expected)) == 0);
  s10 = project_for_test((Vec3f){1.5f, 0.5f, 0}, camera2, projection, size);
  {
    Pixel p10 = backend.frame[s10.y * 64 + s10.x];
    TASK_CHECK(p10.r == 255 && p10.g == 51 && p10.b == 51);
  }

  memory_backend_free(&backend);
  bake_scene_free(&f.scene);
  return 0;
}

static int integration_material_and_exposure(void) {
  IntegrationFixture f;
  MemoryBackend backend;
  Renderer renderer;
  Mat4 camera, projection;
  Vec2i size = {64, 48};
  Vec2i s10, s00;
  Pixel base_pixels[1];
  Texture base_texture;
  Material material;
  Pixel p10, p00;

  integration_fixture_build(&f);
  TASK_CHECK(memory_backend_init(&backend, size) == 0);
  TASK_CHECK(renderer_init(&renderer, size, (Backend *)&backend) == 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&f.root);
  camera = mat4Translate((Vec3f){1, 1, 3});
  projection = mat4Perspective(1, 50, 64.0f / 48.0f, 0.8f);
  renderer.camera_view = camera;
  renderer.camera_projection = projection;

  /* Base colour texture (200,100,50,201) multiplied with the bake. */
  base_pixels[0] = pixelFromRGBA(200, 100, 50, 201);
  texture_init(&base_texture, (Vec2i){1, 1}, base_pixels);
  material_init(&material, &base_texture);
  f.objects[0].material = &material;

  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);
  s10 = project_for_test((Vec3f){1.5f, 0.5f, 0}, camera, projection, size);
  s00 = project_for_test((Vec3f){0.5f, 0.5f, 0}, camera, projection, size);
  p10 = backend.frame[s10.y * 64 + s10.x];
  p00 = backend.frame[s00.y * 64 + s00.x];
  /* 200/255*1.2 -> 240, 100/255*0.2 -> 20, 50/255*0.2 -> 10. */
  TASK_CHECK(p10.r == 240 && p10.g == 20 && p10.b == 10 && p10.a == 201);
  TASK_CHECK(p00.r == 40 && p00.g == 20 && p00.b == 10 && p00.a == 201);

  /* Swapping the base texture needs no rebake. */
  base_pixels[0] = pixelFromRGBA(40, 80, 160, 255);
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);
  p10 = backend.frame[s10.y * 64 + s10.x];
  /* 40/255*1.2 -> 48, 80/255*0.2 -> 16, 160/255*0.2 -> 32. */
  TASK_CHECK(p10.r == 48 && p10.g == 16 && p10.b == 32 && p10.a == 255);

  /* Exposure scales linearly before the clamp. */
  f.objects[0].material = 0;
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 0.5f) == 0);
  p10 = backend.frame[s10.y * 64 + s10.x];
  p00 = backend.frame[s00.y * 64 + s00.x];
  TASK_CHECK(p10.r == 153 && p10.g == 25 && p10.b == 25);
  TASK_CHECK(p00.r == 25 && p00.g == 25 && p00.b == 25);

  memory_backend_free(&backend);
  bake_scene_free(&f.scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * integration: a scene that fails validation must not call the backend.
 * --------------------------------------------------------------------- */

typedef struct {
  Backend backend;
  Vec2i size;
  Pixel *frame;
  PingoDepth *depth;
  int init_calls, before_calls, after_calls;
} CountingBackend;

static void counting_init(Renderer *r, Backend *b, Vec4i rect) {
  (void)r;
  (void)rect;
  ((CountingBackend *)b)->init_calls++;
}
static void counting_before(Renderer *r, Backend *b) {
  (void)r;
  ((CountingBackend *)b)->before_calls++;
}
static void counting_after(Renderer *r, Backend *b) {
  (void)r;
  ((CountingBackend *)b)->after_calls++;
}
static Pixel *counting_frame(Renderer *r, Backend *b) {
  (void)r;
  return ((CountingBackend *)b)->frame;
}
static PingoDepth *counting_depth(Renderer *r, Backend *b) {
  (void)r;
  return ((CountingBackend *)b)->depth;
}

static int counting_backend_init(CountingBackend *this, Vec2i size) {
  this->size = size;
  this->backend.init = counting_init;
  this->backend.beforeRender = counting_before;
  this->backend.afterRender = counting_after;
  this->backend.getFrameBuffer = counting_frame;
  this->backend.getZetaBuffer = counting_depth;
  this->frame = calloc((size_t)size.x * size.y, sizeof(Pixel));
  this->depth = calloc((size_t)size.x * size.y, sizeof(PingoDepth));
  this->init_calls = this->before_calls = this->after_calls = 0;
  return this->frame && this->depth ? 0 : 1;
}

static int integration_error_scene_no_callbacks(void) {
  IntegrationFixture f;
  CountingBackend backend;
  Renderer renderer;
  Vec2i size = {64, 48};

  integration_fixture_build(&f);
  TASK_CHECK(counting_backend_init(&backend, size) == 0);
  TASK_CHECK(renderer_init(&renderer, size, (Backend *)&backend) == 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&f.root);
  renderer.camera_view = mat4Translate((Vec3f){1, 1, 3});
  renderer.camera_projection = mat4Perspective(1, 50, 64.0f / 48.0f, 0.8f);
  TASK_CHECK(backend.init_calls == 1);

  /* clear=false is required to fail before any backend call. */
  renderer.clear = false;
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) != 0);
  TASK_CHECK(backend.before_calls == 0 && backend.after_calls == 0);
  renderer.clear = true;

  /* A moved entity breaks the transform match. */
  f.children[0].transform = mat4Translate((Vec3f){0.1f, 0, 0});
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) != 0);
  TASK_CHECK(backend.before_calls == 0 && backend.after_calls == 0);
  f.children[0].transform = mat4Identity();

  /* A missing instance breaks the count match. */
  f.root.children_entities.size = 1;
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) != 0);
  TASK_CHECK(backend.before_calls == 0 && backend.after_calls == 0);
  f.root.children_entities.size = 2;

  /* Geometry drift is caught too. */
  it_recv_pos[0].x += 1.0f;
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) != 0);
  TASK_CHECK(backend.before_calls == 0 && backend.after_calls == 0);
  it_recv_pos[0].x -= 1.0f;

  /* Bad arguments. */
  TASK_CHECK(renderer_render_lightmapped(&renderer, 0, 1.0f) != 0);
  TASK_CHECK(renderer_render_lightmapped(0, &f.scene, 1.0f) != 0);
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, NAN) != 0);
  TASK_CHECK(backend.before_calls == 0 && backend.after_calls == 0);

  /* And the healthy path still works afterwards. */
  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);
  TASK_CHECK(backend.before_calls == 1 && backend.after_calls == 1);

  free(backend.frame);
  free(backend.depth);
  bake_scene_free(&f.scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * integration: the backend may swap buffers in beforeRender; the frame
 * must land in the buffer that is current afterwards.
 * --------------------------------------------------------------------- */

typedef struct {
  Backend backend;
  Vec2i size;
  Pixel *frame_a, *frame_b;
  PingoDepth *depth_a, *depth_b;
  int use_b, before_calls;
} SwapBackend;

static void swap_init(Renderer *r, Backend *b, Vec4i rect) {
  (void)r;
  (void)b;
  (void)rect;
}
static void swap_before(Renderer *r, Backend *b) {
  (void)r;
  ((SwapBackend *)b)->use_b = 1;
  ((SwapBackend *)b)->before_calls++;
}
static void swap_after(Renderer *r, Backend *b) {
  (void)r;
  (void)b;
}
static Pixel *swap_frame(Renderer *r, Backend *b) {
  (void)r;
  return ((SwapBackend *)b)->use_b ? ((SwapBackend *)b)->frame_b
                                   : ((SwapBackend *)b)->frame_a;
}
static PingoDepth *swap_depth(Renderer *r, Backend *b) {
  (void)r;
  return ((SwapBackend *)b)->use_b ? ((SwapBackend *)b)->depth_b
                                   : ((SwapBackend *)b)->depth_a;
}

static int integration_backend_buffer_swap(void) {
  IntegrationFixture f;
  SwapBackend backend;
  Renderer renderer;
  Vec2i size = {64, 48};
  int i, lit_a = 0, lit_b = 0, depth_a = 0, depth_b = 0;

  integration_fixture_build(&f);

  memset(&backend, 0, sizeof(backend));
  backend.size = size;
  backend.backend.init = swap_init;
  backend.backend.beforeRender = swap_before;
  backend.backend.afterRender = swap_after;
  backend.backend.getFrameBuffer = swap_frame;
  backend.backend.getZetaBuffer = swap_depth;
  backend.frame_a = calloc(64 * 48, sizeof(Pixel));
  backend.frame_b = calloc(64 * 48, sizeof(Pixel));
  backend.depth_a = calloc(64 * 48, sizeof(PingoDepth));
  backend.depth_b = calloc(64 * 48, sizeof(PingoDepth));
  TASK_CHECK(backend.frame_a && backend.frame_b && backend.depth_a &&
             backend.depth_b);

  TASK_CHECK(renderer_init(&renderer, size, (Backend *)&backend) == 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&f.root);
  renderer.camera_view = mat4Translate((Vec3f){1, 1, 3});
  renderer.camera_projection = mat4Perspective(1, 50, 64.0f / 48.0f, 0.8f);

  TASK_CHECK(renderer_render_lightmapped(&renderer, &f.scene, 1.0f) == 0);
  TASK_CHECK(backend.before_calls == 1);

  for (i = 0; i < 64 * 48; i++) {
    if (backend.frame_a[i].r || backend.frame_a[i].g || backend.frame_a[i].b)
      lit_a++;
    if (backend.frame_b[i].r || backend.frame_b[i].g || backend.frame_b[i].b)
      lit_b++;
    if (backend.depth_a[i].d)
      depth_a++;
    if (backend.depth_b[i].d)
      depth_b++;
  }
  /* The whole frame went to the buffers that were current after
   * beforeRender; the initial buffers stayed pristine. */
  TASK_CHECK(lit_b > 200);
  TASK_CHECK(lit_a == 0);
  TASK_CHECK(depth_b > 200);
  TASK_CHECK(depth_a == 0);
  TASK_CHECK(renderer.framebuffer.frameBuffer == backend.frame_b);

  free(backend.frame_a);
  free(backend.frame_b);
  free(backend.depth_a);
  free(backend.depth_b);
  bake_scene_free(&f.scene);
  return 0;
}

/* ------------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    {"surface", "shared_object_instances", surface_shared_object_instances},
    {"surface", "mirrored_uv_shared_edge", surface_mirrored_uv_shared_edge},
    {"surface", "zero_area_uv_ignored", surface_zero_area_uv_ignored},
    {"surface", "coverage_conflict_reported", surface_coverage_conflict_reported},
    {"surface", "binding_validation", surface_binding_validation},
    {"surface", "source_freed", surface_source_freed},
    {"transport", "two_rounds", transport_two_rounds},
    {"transport", "backface_absorbs", transport_backface_absorbs},
    {"transport", "no_inplace_update", transport_no_inplace_update},
    {"transport", "equal_radiance_interleaved", transport_equal_radiance_interleaved},
    {"transport", "reset_and_invalid", transport_reset_and_invalid},
    {"transport", "tie_break_path", transport_tie_break_path},
    {"transport", "tie_break_face", transport_tie_break_face},
    {"integration", "camera_pixels", integration_camera_pixels},
    {"integration", "material_and_exposure", integration_material_and_exposure},
    {"integration", "error_scene_no_callbacks", integration_error_scene_no_callbacks},
    {"integration", "backend_buffer_swap", integration_backend_buffer_swap},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
