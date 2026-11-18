/**
 * Point-light regression tests.
 *
 * Every expected value is recomputed here from the scene constants - world
 * matrices, local light positions, the ambient term and the surface colour -
 * rather than being read back from the renderer. The rasterization rules the
 * reference reproduces (screen triangle, barycentric weights, perspective
 * division) are the ones object.c documents; everything else, including the
 * effective light chain of a node, is derived from the scene as the test
 * built it.
 *
 * Three groups:
 *   hierarchy  effective lights down the entity tree, shared Objects,
 *              sibling order, several renderers, scaling
 *   fragment   world-position interpolation, per-fragment shading, the
 *              distance window and the final clamp
 *   compat     the old face-shaded path, which stays the default
 */

#include "task_support/check.h"

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/backend.h"
#include "render/entity.h"
#include "render/lighting.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/pixel.h"
#include "render/renderer.h"
#include "render/state.h"
#include "render/tests/memory_backend.h"

#include <math.h>
#include <stdlib.h>

#define WIDTH 64
#define HEIGHT 48

// ---------------------------------------------------------------------------
// Reference renderer
//
// A second, deliberately small implementation of exactly what the feature
// promises. It is fed the scene the test built, never the engine's result.
// ---------------------------------------------------------------------------

typedef struct {
  Vec3f local[3];
  Mat4 world;
} RefTriangle;

// mat4MultiplyM(&a, &b) composes "a then b" for row vectors, which is the
// convention the entity tree and object_render use. Spelled out once here so
// the reference matrices are built the same way the scene describes them.
static Mat4 then(Mat4 a, Mat4 b) { return mat4MultiplyM(&a, &b); }

static Vec3f ref_world_vertex(const RefTriangle *tri, int i) {
  Vec3f p = tri->local[i];
  return mat4MultiplyVec3(&p, (Mat4 *)&tri->world);
}

static Vec3f ref_normal(const RefTriangle *tri) {
  Vec3f a = ref_world_vertex(tri, 0);
  Vec3f b = ref_world_vertex(tri, 1);
  Vec3f c = ref_world_vertex(tri, 2);
  return vec3Normalize(
      vec3Cross(vec3fsubV(a, b), vec3fsubV(a, c)));
}

typedef struct {
  Vec2i scr[3];
  float inv_w[3];
  float ndc_z[3];
  int area;
} RefProjection;

static int ref_project(const RefTriangle *tri, Mat4 camera_view,
                       Mat4 projection, RefProjection *out) {
  Mat4 v = mat4Inverse(&camera_view);
  Mat4 vm = mat4MultiplyM(&v, (Mat4 *)&tri->world);

  for (int i = 0; i < 3; i++) {
    Vec4f p = {tri->local[i].x, tri->local[i].y, tri->local[i].z, 1};
    Vec4f cam = mat4MultiplyVec4(&p, &vm);
    Vec4f clip = mat4MultiplyVec4(&cam, &projection);
    if (clip.w == 0)
      return 0;
    out->inv_w[i] = 1.0f / clip.w;
    out->ndc_z[i] = clip.z / clip.w;
    out->scr[i].x = (int)(clip.x / clip.w * WIDTH * 0.5f + WIDTH * 0.5f);
    out->scr[i].y = (int)(clip.y / clip.w * HEIGHT * 0.5f + HEIGHT * 0.5f);
  }

  const Vec2i *a = &out->scr[0];
  const Vec2i *b = &out->scr[1];
  const Vec2i *c = &out->scr[2];
  out->area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
  return out->area != 0;
}

// Exactly the oriented areas object_render accumulates, evaluated at a pixel.
static int ref_weights(const RefProjection *pr, int x, int y, int *w0, int *w1,
                       int *w2) {
  const Vec2i *a = &pr->scr[0];
  const Vec2i *b = &pr->scr[1];
  const Vec2i *c = &pr->scr[2];
  *w0 = (c->x - b->x) * (y - b->y) - (c->y - b->y) * (x - b->x);
  *w1 = (a->x - c->x) * (y - c->y) - (a->y - c->y) * (x - c->x);
  *w2 = (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x);
  return (*w0 | *w1 | *w2) >= 0;
}

// World position of the fragment: screen barycentrics scaled by each vertex's
// inverse clip w, then normalised.
static int ref_world_position(const RefTriangle *tri, const RefProjection *pr,
                              int x, int y, Vec3f *out) {
  int w0, w1, w2;
  if (!ref_weights(pr, x, y, &w0, &w1, &w2))
    return 0;

  const float den = w0 * pr->inv_w[0] + w1 * pr->inv_w[1] + w2 * pr->inv_w[2];
  if (den == 0)
    return 0;

  Vec3f sum = {0, 0, 0};
  sum = vec3fsumV(sum, vec3fmul(ref_world_vertex(tri, 0), w0 * pr->inv_w[0]));
  sum = vec3fsumV(sum, vec3fmul(ref_world_vertex(tri, 1), w1 * pr->inv_w[1]));
  sum = vec3fsumV(sum, vec3fmul(ref_world_vertex(tri, 2), w2 * pr->inv_w[2]));
  *out = vec3fmul(sum, 1.0f / den);
  return 1;
}

// use_range == 0 skips the visibility test but keeps the same window - the
// shape a "forgot to check the distance" bug has.
static Vec3f ref_light_contribution_ex(Vec3f normal, Vec3f point,
                                       const PointLight *lights, size_t count,
                                       int use_range) {
  Vec3f sum = {0, 0, 0};
  for (size_t i = 0; i < count; i++) {
    const PointLight *l = &lights[i];
    Vec3f to_light = vec3fsubV(l->position, point);
    float d2 = vec3Dot(to_light, to_light);
    if (d2 <= 0.0f)
      continue;
    float d = sqrtf(d2);
    if (use_range && !(d > 0.0f && d < l->range))
      continue;

    Vec3f direction = vec3fmul(to_light, 1.0f / d);
    float lambert = vec3Dot(normal, direction);
    if (lambert <= 0.0f)
      continue;

    float window = 1.0f - d / l->range;
    float weight = (window * window) / (1.0f + d * d);
    sum = vec3fsumV(sum, vec3fmul(l->intensity, lambert * weight));
  }
  return sum;
}

static Vec3f ref_light_contribution(Vec3f normal, Vec3f point,
                                    const PointLight *lights, size_t count) {
  return ref_light_contribution_ex(normal, point, lights, count, 1);
}

static uint8_t ref_byte(float v) {
  if (!(v > 0.0f))
    return 0;
  if (v >= 255.0f)
    return 255;
  return (uint8_t)v;
}

static Pixel ref_shade_at(Vec3f normal, Vec3f point, const PointLight *lights,
                          size_t count, Vec3f ambient, Pixel base,
                          float *products) {
  Vec3f lit =
      vec3fsumV(ambient, ref_light_contribution(normal, point, lights, count));

  // Final stage: multiply per channel, clamp to the byte range, truncate.
  // Alpha is the base colour's, untouched.
  const float r = lit.x * (float)base.r;
  const float g = lit.y * (float)base.g;
  const float b = lit.z * (float)base.b;
  if (products) {
    products[0] = r;
    products[1] = g;
    products[2] = b;
  }

  Pixel out;
  out.r = ref_byte(r);
  out.g = ref_byte(g);
  out.b = ref_byte(b);
  out.a = base.a;
  return out;
}

static Pixel ref_shade(const RefTriangle *tri, const RefProjection *pr,
                       const PointLight *lights, size_t count, Vec3f ambient,
                       Pixel base, float *products, int x, int y, int *ok) {
  Vec3f point;
  if (!ref_world_position(tri, pr, x, y, &point)) {
    *ok = 0;
    return PIXELBLACK;
  }
  *ok = 1;
  return ref_shade_at(ref_normal(tri), point, lights, count, ambient, base,
                      products);
}

// The pre-light formula, kept verbatim as the reference for "lighting off":
// one camera-space normal per triangle, a fixed direction, one brightness for
// the whole face.
static float ref_legacy_brightness(const RefTriangle *tri, Mat4 camera_view) {
  Mat4 v = mat4Inverse(&camera_view);
  Mat4 vm = mat4MultiplyM(&v, (Mat4 *)&tri->world);
  Vec3f cam[3];
  for (int i = 0; i < 3; i++) {
    Vec4f p = {tri->local[i].x, tri->local[i].y, tri->local[i].z, 1};
    Vec4f c = mat4MultiplyVec4(&p, &vm);
    cam[i] = (Vec3f){c.x, c.y, c.z};
  }
  Vec3f normal = vec3Normalize(
      vec3Cross(vec3fsubV(cam[0], cam[1]), vec3fsubV(cam[0], cam[2])));
  Vec3f light = vec3Normalize((Vec3f){-8, 5, 5});
  float diffuse = (1.0 + vec3Dot(normal, light)) * 0.5;
  if (diffuse < 0.0f)
    diffuse = 0.0f;
  if (diffuse > 1.0f)
    diffuse = 1.0f;
  return diffuse;
}

static Pixel ref_legacy_pixel(Pixel base, float brightness) {
  Pixel out;
  out.r = (uint8_t)((float)base.r * brightness);
  out.g = (uint8_t)((float)base.g * brightness);
  out.b = (uint8_t)((float)base.b * brightness);
  out.a = base.a;
  return out;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

static MemoryBackend backend;
static Renderer renderer;
static Texture texture_storage;
static Material material_storage;
static Pixel texture_texel;

static int frame_init(void) {
  if (memory_backend_init(&backend, (Vec2i){WIDTH, HEIGHT}) != 0)
    return 1;
  if (renderer_init(&renderer, (Vec2i){WIDTH, HEIGHT}, (Backend *)&backend) !=
      OK)
    return 1;
  renderer.camera_projection =
      mat4Perspective(1, 50.0, (float)WIDTH / (float)HEIGHT, 0.6);
  renderer.camera_view = mat4Identity();
  return 0;
}

static void frame_free(void) { memory_backend_free(&backend); }

static Pixel frame_at(int x, int y) { return backend.frame[x + y * WIDTH]; }

static int pixel_is(Pixel p, int r, int g, int b, int a) {
  return p.r == (uint8_t)r && p.g == (uint8_t)g && p.b == (uint8_t)b &&
         p.a == (uint8_t)a;
}

static int pixel_eq(Pixel p, Pixel q) {
  return pixel_is(p, q.r, q.g, q.b, q.a);
}

// One material per scene: the pointer stays valid for the whole case, which
// is all the renderer promises.
static Material *make_material(Pixel colour) {
  texture_texel = colour;
  texture_init(&texture_storage, (Vec2i){1, 1}, &texture_texel);
  material_init(&material_storage, &texture_storage);
  return &material_storage;
}

static void set_camera(Renderer *r, Vec2i size) {
  r->camera_projection =
      mat4Perspective(1, 50.0, (float)size.x / (float)size.y, 0.6);
  r->camera_view = mat4Identity();
}

// ---------------------------------------------------------------------------
// Hierarchy scene
//
// root (light L0)
//   +- left  (light LA) -> grand (light LG)
//   +- right (light LB)
//
// Every node renders the same Object and therefore the same mesh. If any of
// them shared one effective light list, the colours below would collapse.
//
// The children arrays hold the nodes themselves - entity_render indexes into
// the array - so the node a test edits with entity_set_point_lights() is the
// one that is drawn.
// ---------------------------------------------------------------------------

typedef struct {
  Entity root;
  Entity branches[2]; // [0] left, [1] right
  Entity grand;       // lives inside left's children array below
  Entity left_children[1];
  Object object;
  Object grand_object;
  Object empty_object;
  Mesh mesh;
  Mesh grand_mesh;
  Mesh empty_mesh;
  Vec3f quad[4];
  Vec3f grand_quad[4];
  PointLight l0, la, lb, lg;
  Mat4 local_root, local_left, local_right, local_grand;
  Mat4 world_root, world_left, world_right, world_grand;
  Pixel base;
} Hierarchy;

static void hierarchy_build(Hierarchy *h) {
  h->base = pixelFromRGBA(204, 140, 102, 200);

  h->quad[0] = (Vec3f){-0.95f, -0.90f, 0.00f};
  h->quad[1] = (Vec3f){0.95f, -0.90f, 0.45f};
  h->quad[2] = (Vec3f){0.95f, 0.90f, 0.27f};
  h->quad[3] = (Vec3f){-0.95f, 0.90f, 0.60f};

  h->grand_quad[0] = (Vec3f){-1.60f, -1.60f, 0.00f};
  h->grand_quad[1] = (Vec3f){1.60f, -1.60f, 0.65f};
  h->grand_quad[2] = (Vec3f){1.60f, 1.60f, 0.40f};
  h->grand_quad[3] = (Vec3f){-1.60f, 1.60f, 0.85f};

  static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
  h->mesh.indexes_count = 6;
  h->mesh.pos_indices = indices;
  h->mesh.tex_indices = 0;
  h->mesh.positions = h->quad;
  h->mesh.textCoord = 0;

  h->grand_mesh = h->mesh;
  h->grand_mesh.positions = h->grand_quad;

  h->empty_mesh.indexes_count = 0;
  h->empty_mesh.pos_indices = 0;
  h->empty_mesh.tex_indices = 0;
  h->empty_mesh.positions = 0;
  h->empty_mesh.textCoord = 0;

  object_init(&h->object, &h->mesh, make_material(h->base));
  object_init(&h->grand_object, &h->grand_mesh, make_material(h->base));
  object_init(&h->empty_object, &h->empty_mesh, make_material(h->base));

  Mat4 ry = mat4RotateY(0.12f);
  Mat4 rt = mat4Translate((Vec3f){0, 0, -5.4f});
  Mat4 root_matrix = then(ry, rt);

  Mat4 rz = mat4RotateZ(0.20f);
  Mat4 as = mat4Scale((Vec3f){0.80f, 0.75f, 1.0f});
  Mat4 at = mat4Translate((Vec3f){-0.85f, 0.55f, 0.0f});
  Mat4 left_matrix = then(then(rz, as), at);

  Mat4 by = mat4RotateY(-0.30f);
  Mat4 bs = mat4Scale((Vec3f){0.78f, 0.88f, 1.0f});
  Mat4 bt = mat4Translate((Vec3f){0.85f, -0.05f, 0.25f});
  Mat4 right_matrix = then(then(by, bs), bt);

  Mat4 gy = mat4RotateY(0.25f);
  Mat4 gs = mat4Scale((Vec3f){0.75f, 0.75f, 0.75f});
  Mat4 gt = mat4Translate((Vec3f){-1.75f, -0.75f, 1.1f});
  Mat4 grand_matrix = then(then(gy, gs), gt);

  h->local_root = root_matrix;
  h->local_left = left_matrix;
  h->local_right = right_matrix;
  h->local_grand = grand_matrix;

  // entity_render does new = mat4MultiplyM(&local, &parent), i.e. parent*local.
  h->world_root = root_matrix;
  h->world_left = then(left_matrix, h->world_root);
  h->world_right = then(right_matrix, h->world_root);
  h->world_grand = then(grand_matrix, h->world_left);

  // Light positions are local to their node. The range stays a world value.
  h->l0.position = (Vec3f){-0.30f, 0.30f, 1.00f};
  h->l0.intensity = (Vec3f){1.10f, 0.80f, 0.50f};
  h->l0.range = 3.5f;

  h->la.position = (Vec3f){0.40f, 0.10f, 0.80f};
  h->la.intensity = (Vec3f){0.20f, 1.70f, 0.40f};
  h->la.range = 2.8f;

  h->lb.position = (Vec3f){-0.50f, -0.20f, 0.70f};
  h->lb.intensity = (Vec3f){0.40f, 0.30f, 2.00f};
  h->lb.range = 2.5f;

  h->lg.position = (Vec3f){0.00f, 0.00f, 1.00f};
  h->lg.intensity = (Vec3f){1.20f, 0.40f, 0.10f};
  h->lg.range = 1.1f;

  // grand sits inside left's children array, so initialise it there.
  h->grand = (Entity){0};
  entity_init_children(&h->left_children[0], (Renderable *)&h->grand_object,
                       grand_matrix, 0, 0);
  entity_set_point_lights(&h->left_children[0], &h->lg, 1);

  entity_init_children(&h->branches[0], (Renderable *)&h->object, left_matrix,
                       h->left_children, 1);
  entity_set_point_lights(&h->branches[0], &h->la, 1);

  entity_init_children(&h->branches[1], (Renderable *)&h->object, right_matrix,
                       0, 0);
  entity_set_point_lights(&h->branches[1], &h->lb, 1);

  entity_init_children(&h->root, (Renderable *)&h->empty_object, root_matrix,
                       h->branches, 2);
  entity_set_point_lights(&h->root, &h->l0, 1);
}

static PointLight world_light(PointLight local, Mat4 world) {
  PointLight out = local;
  Vec3f p = local.position;
  out.position = mat4MultiplyVec3(&p, (Mat4 *)&world);
  return out;
}

static void hierarchy_lights(const Hierarchy *h, int node, PointLight out[3],
                             size_t *count) {
  size_t n = 0;
  out[n++] = world_light(h->l0, h->world_root);
  if (node == 1 || node == 3)
    out[n++] = world_light(h->la, h->world_left);
  if (node == 2)
    out[n++] = world_light(h->lb, h->world_right);
  if (node == 3)
    out[n++] = world_light(h->lg, h->world_grand);
  *count = n;
}

// tri_index selects the half of the quad: 0 is (0,1,2), 1 is (0,2,3).
static int hierarchy_project(const Hierarchy *h, int node, int tri_index,
                             RefTriangle *tri, RefProjection *pr) {
  const Vec3f *quad = node == 3 ? h->grand_quad : h->quad;
  tri->local[0] = quad[0];
  tri->local[1] = quad[tri_index == 0 ? 1 : 2];
  tri->local[2] = quad[tri_index == 0 ? 2 : 3];
  switch (node) {
  case 0:
    tri->world = h->world_root;
    break;
  case 1:
    tri->world = h->world_left;
    break;
  case 2:
    tri->world = h->world_right;
    break;
  default:
    tri->world = h->world_grand;
    break;
  }
  return ref_project(tri, renderer.camera_view, renderer.camera_projection, pr);
}

static Pixel hierarchy_reference(const Hierarchy *h, int node, int tri_index,
                                 const PointLight *lights, size_t light_count,
                                 Vec3f ambient, int x, int y, int *covered) {
  RefTriangle tri;
  RefProjection pr;
  if (!hierarchy_project(h, node, tri_index, &tri, &pr))
    return PIXELBLACK;
  return ref_shade(&tri, &pr, lights, light_count, ambient, h->base, 0, x, y,
                   covered);
}

// ---------------------------------------------------------------------------
// hierarchy cases
// ---------------------------------------------------------------------------

static int case_hierarchy_effective_lists(void) {
  if (frame_init())
    return 1;

  Hierarchy h;
  hierarchy_build(&h);
  Vec3f ambient = {0.05f, 0.06f, 0.07f};
  renderer_set_root_renderable(&renderer, (Renderable *)&h.root);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  struct {
    int node, tri, x, y, r, g, b;
  } samples[] = {
      {1, 0, 25, 25, 55, 71, 23},  // left: root + left
      {1, 1, 21, 30, 43, 65, 20},  // left, second triangle
      {2, 0, 42, 19, 32, 19, 38},  // right: root + right
      {2, 1, 35, 24, 88, 48, 114}, // right, second triangle
      {3, 0, 18, 19, 19, 10, 7},   // grand: root + left + grand
      {3, 1, 4, 25, 30, 13, 8},    // grand, second triangle
  };

  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    PointLight lights[3];
    size_t count;
    hierarchy_lights(&h, samples[i].node, lights, &count);
    int covered = 0;
    Pixel expected = hierarchy_reference(&h, samples[i].node, samples[i].tri,
                                         lights, count, ambient, samples[i].x,
                                         samples[i].y, &covered);
    TASK_CHECK(covered);
    TASK_CHECK(pixel_is(expected, samples[i].r, samples[i].g, samples[i].b,
                        200));
    TASK_CHECK(pixel_eq(frame_at(samples[i].x, samples[i].y), expected));
  }

  frame_free();
  return 0;
}

// Two independent single-node scenes, each centred at a sensible distance.
// Rendering them in sequence through the same renderer, back and forth, pins
// the "changing root, same renderer" case.
static int case_hierarchy_root_switch(void) {
  if (frame_init())
    return 1;

  static const uint16_t indices[3] = {0, 1, 2};
  Vec3f quad_a[3] = {{-0.7f, -0.6f, 0.0f}, {0.7f, -0.6f, 0.2f},
                     {-0.1f, 0.65f, 0.35f}};
  Vec3f quad_b[3] = {{-0.5f, -0.7f, 0.1f}, {0.6f, -0.5f, 0.0f},
                     {0.0f, 0.7f, 0.25f}};

  Mesh mesh_a;
  mesh_a.indexes_count = 3;
  mesh_a.pos_indices = indices;
  mesh_a.tex_indices = 0;
  mesh_a.positions = quad_a;
  mesh_a.textCoord = 0;
  Mesh mesh_b = mesh_a;
  mesh_b.positions = quad_b;

  Pixel base = pixelFromRGBA(230, 180, 120, 255);
  Object object_a;
  Object object_b;
  object_init(&object_a, &mesh_a, make_material(base));
  object_init(&object_b, &mesh_b, make_material(base));

  Entity node_a;
  Entity node_b;
  Mat4 move_a = mat4Translate((Vec3f){-0.45f, 0.15f, -4.0f});
  Mat4 turn = mat4RotateY(0.22f);
  Mat4 move_b = mat4Translate((Vec3f){0.40f, -0.10f, -3.4f});
  Mat4 world_a = then(mat4Scale((Vec3f){0.9f, 1.1f, 1.0f}), then(turn, move_a));
  Mat4 world_b = then(mat4RotateX(-0.15f), move_b);
  entity_init(&node_a, (Renderable *)&object_a, world_a);
  entity_init(&node_b, (Renderable *)&object_b, world_b);

  PointLight light_a;
  light_a.position = (Vec3f){-0.4f, 0.3f, 0.9f};
  light_a.intensity = (Vec3f){2.2f, 1.4f, 0.3f};
  light_a.range = 2.6f;
  entity_set_point_lights(&node_a, &light_a, 1);

  PointLight light_b;
  light_b.position = (Vec3f){0.5f, -0.2f, 0.8f};
  light_b.intensity = (Vec3f){0.3f, 1.3f, 2.4f};
  light_b.range = 2.1f;
  entity_set_point_lights(&node_b, &light_b, 1);

  const Vec3f ambient = {0.04f, 0.04f, 0.04f};
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);

  RefTriangle tri;
  RefProjection pr_a, pr_b;
  tri.local[0] = quad_a[0];
  tri.local[1] = quad_a[1];
  tri.local[2] = quad_a[2];
  tri.world = world_a;
  TASK_CHECK(
      ref_project(&tri, renderer.camera_view, renderer.camera_projection,
                  &pr_a));
  PointLight wa;
  wa.position = mat4MultiplyVec3(&light_a.position, &world_a);
  wa.intensity = light_a.intensity;
  wa.range = light_a.range;

  RefTriangle tri_b;
  tri_b.local[0] = quad_b[0];
  tri_b.local[1] = quad_b[1];
  tri_b.local[2] = quad_b[2];
  tri_b.world = world_b;
  TASK_CHECK(
      ref_project(&tri_b, renderer.camera_view, renderer.camera_projection,
                  &pr_b));
  PointLight wb;
  wb.position = mat4MultiplyVec3(&light_b.position, &world_b);
  wb.intensity = light_b.intensity;
  wb.range = light_b.range;

  // The centroid pixels are comfortably interior in both scenes.
  int ax = 0, ay = 0, bx = 0, by = 0;
  for (int i = 0; i < 3; i++) {
    ax += pr_a.scr[i].x / 3;
    ay += pr_a.scr[i].y / 3;
    bx += pr_b.scr[i].x / 3;
    by += pr_b.scr[i].y / 3;
  }
  int w0, w1, w2;
  TASK_CHECK(ref_weights(&pr_a, ax, ay, &w0, &w1, &w2));
  TASK_CHECK(w0 > 0 && w1 > 0 && w2 > 0);
  TASK_CHECK(ref_weights(&pr_b, bx, by, &w0, &w1, &w2));
  TASK_CHECK(w0 > 0 && w1 > 0 && w2 > 0);

  int covered = 0;
  Pixel pixel_a =
      ref_shade(&tri, &pr_a, &wa, 1, ambient, base, 0, ax, ay, &covered);
  TASK_CHECK(covered);
  Pixel pixel_b =
      ref_shade(&tri_b, &pr_b, &wb, 1, ambient, base, 0, bx, by, &covered);
  TASK_CHECK(covered);
  TASK_CHECK(!pixel_eq(pixel_a, pixel_b));

  renderer_set_root_renderable(&renderer, (Renderable *)&node_a);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(ax, ay), pixel_a));
  TASK_CHECK(pixel_is(frame_at(bx, by), 0, 0, 0, 0));

  renderer_set_root_renderable(&renderer, (Renderable *)&node_b);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(bx, by), pixel_b));
  TASK_CHECK(pixel_is(frame_at(ax, ay), 0, 0, 0, 0));

  renderer_set_root_renderable(&renderer, (Renderable *)&node_a);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(ax, ay), pixel_a));

  frame_free();
  return 0;
}

static int case_hierarchy_tree_edits(void) {
  if (frame_init())
    return 1;

  Hierarchy h;
  hierarchy_build(&h);
  const Vec3f ambient = {0.05f, 0.06f, 0.07f};
  renderer_set_root_renderable(&renderer, (Renderable *)&h.root);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  PointLight lights[3];
  size_t count;
  int covered;

  hierarchy_lights(&h, 1, lights, &count);
  Pixel left_ref =
      hierarchy_reference(&h, 1, 0, lights, count, ambient, 25, 25, &covered);
  TASK_CHECK(covered);
  hierarchy_lights(&h, 2, lights, &count);
  Pixel right_ref =
      hierarchy_reference(&h, 2, 1, lights, count, ambient, 35, 24, &covered);
  TASK_CHECK(covered);
  hierarchy_lights(&h, 3, lights, &count);
  Pixel grand_ref =
      hierarchy_reference(&h, 3, 1, lights, count, ambient, 4, 25, &covered);
  TASK_CHECK(covered);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));
  TASK_CHECK(pixel_eq(frame_at(35, 24), right_ref));
  TASK_CHECK(pixel_eq(frame_at(4, 25), grand_ref));

  // Siblings swapped: every node keeps its own lights and the shared Object
  // sees whichever node is currently drawing it.
  Entity swap = h.branches[0];
  h.branches[0] = h.branches[1];
  h.branches[1] = swap;
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));
  TASK_CHECK(pixel_eq(frame_at(35, 24), right_ref));
  TASK_CHECK(pixel_eq(frame_at(4, 25), grand_ref));

  // Swapped back.
  swap = h.branches[0];
  h.branches[0] = h.branches[1];
  h.branches[1] = swap;
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));

  // An invisible node draws nothing and contributes no lights to anything.
  h.branches[1].visible = false;
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_is(frame_at(42, 19), 0, 0, 0, 0));
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));
  h.branches[1].visible = true;
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(35, 24), right_ref));

  // Leave the subtree: the right branch is no longer visited at all.
  h.root.children_entities.size = 1;
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));
  TASK_CHECK(pixel_eq(frame_at(4, 25), grand_ref));
  TASK_CHECK(pixel_is(frame_at(42, 19), 0, 0, 0, 0));

  // Dropping the grandchild's own light must not touch its parent's pixel,
  // and the grandchild's own pixel must change.
  entity_set_point_lights(&h.left_children[0], 0, 0);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_ref));

  PointLight parent_only[2];
  parent_only[0] = world_light(h.l0, h.world_root);
  parent_only[1] = world_light(h.la, h.world_left);
  Pixel grand_without = hierarchy_reference(&h, 3, 1, parent_only, 2, ambient,
                                            4, 25, &covered);
  TASK_CHECK(covered);
  TASK_CHECK(!pixel_eq(grand_without, grand_ref));
  TASK_CHECK(pixel_eq(frame_at(4, 25), grand_without));

  frame_free();
  return 0;
}

static int case_hierarchy_renderer_isolation(void) {
  static MemoryBackend backend_b;
  if (frame_init())
    return 1;
  if (memory_backend_init(&backend_b, (Vec2i){WIDTH, HEIGHT}) != 0)
    return 1;

  Hierarchy h;
  hierarchy_build(&h);
  const Vec3f ambient = {0.05f, 0.06f, 0.07f};
  const Vec3f ambient_b = {0.20f, 0.10f, 0.00f};

  Renderer second;
  TASK_CHECK(renderer_init(&second, (Vec2i){WIDTH, HEIGHT},
                           (Backend *)&backend_b) == OK);
  set_camera(&second, (Vec2i){WIDTH, HEIGHT});

  entity_set_point_lights(&h.root, 0, 0);
  entity_set_point_lights(&h.left_children[0], 0, 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&h.root);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);
  renderer_set_root_renderable(&second, (Renderable *)&h.root);
  TASK_CHECK(renderer_set_point_lighting(&second, true, ambient_b) == OK);

  // Root lights were cleared above, so each node contributes only its own.
  int covered = 0;
  PointLight only_la[1];
  only_la[0] = world_light(h.la, h.world_left);
  Pixel left_expected =
      hierarchy_reference(&h, 1, 0, only_la, 1, ambient, 25, 25, &covered);
  TASK_CHECK(covered);

  // The second renderer draws the same tree with a different ambient, so its
  // pixel must show that ambient and not the first renderer's.
  PointLight only_lb[1];
  only_lb[0] = world_light(h.lb, h.world_right);
  Pixel right_expected =
      hierarchy_reference(&h, 2, 0, only_lb, 1, ambient_b, 42, 19, &covered);
  TASK_CHECK(covered);

  // Interleaved: each renderer keeps its own root, ambient and list.
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(renderer_render(&second) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_expected));
  TASK_CHECK(pixel_eq(backend_b.frame[42 + 19 * WIDTH], right_expected));

  const Pixel second_before = backend_b.frame[42 + 19 * WIDTH];
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_expected));
  TASK_CHECK(pixel_eq(backend_b.frame[42 + 19 * WIDTH], second_before));

  // Turning one renderer off leaves the other alone.
  TASK_CHECK(renderer_set_point_lighting(&second, false, (Vec3f){0, 0, 0}) ==
             OK);
  TASK_CHECK(renderer_render(&second) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(25, 25), left_expected));
  TASK_CHECK(!pixel_eq(backend_b.frame[42 + 19 * WIDTH], right_expected));

  frame_free();
  memory_backend_free(&backend_b);
  return 0;
}

static int case_hierarchy_scale(void) {
  if (frame_init())
    return 1;

  // One node with a non-uniform scale. The light's local offset must be
  // scaled with the geometry; its range must not be.
  Vec3f quad[3] = {{-0.55f, -0.5f, 0.0f}, {0.55f, -0.5f, 0.2f},
                   {0.0f, 0.55f, 0.35f}};
  Mesh mesh;
  static const uint16_t indices[3] = {0, 1, 2};
  mesh.indexes_count = 3;
  mesh.pos_indices = indices;
  mesh.tex_indices = 0;
  mesh.positions = quad;
  mesh.textCoord = 0;

  Pixel base = pixelFromRGBA(180, 200, 220, 255);
  Object object;
  object_init(&object, &mesh, make_material(base));

  Mat4 scale = mat4Scale((Vec3f){1.6f, 0.7f, 1.0f});
  Mat4 move = mat4Translate((Vec3f){0.25f, 0.1f, -4.2f});
  Mat4 world = then(scale, move);

  Entity node;
  entity_init(&node, (Renderable *)&object, world);
  PointLight light;
  light.position = (Vec3f){-0.20f, 0.00f, 0.90f};
  light.intensity = (Vec3f){3.2f, 2.4f, 0.8f};
  light.range = 2.2f;
  entity_set_point_lights(&node, &light, 1);

  renderer_set_root_renderable(&renderer, (Renderable *)&node);
  const Vec3f ambient = {0.03f, 0.03f, 0.03f};
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  RefTriangle tri;
  tri.local[0] = quad[0];
  tri.local[1] = quad[1];
  tri.local[2] = quad[2];
  tri.world = world;
  RefProjection pr;
  TASK_CHECK(
      ref_project(&tri, renderer.camera_view, renderer.camera_projection, &pr));

  PointLight scaled;
  scaled.position = mat4MultiplyVec3(&light.position, &world);
  scaled.intensity = light.intensity;
  scaled.range = light.range;

  PointLight unscaled = light; // the wrong reading: position left local
  PointLight range_scaled = scaled;
  range_scaled.range = light.range * 1.6f; // likewise wrong

  int checked = 0;
  for (int y = 0; y < HEIGHT && checked < 3; y++) {
    for (int x = 0; x < WIDTH && checked < 3; x++) {
      int w0, w1, w2;
      if (!ref_weights(&pr, x, y, &w0, &w1, &w2))
        continue;
      const int margin = abs(pr.area) / 5;
      if (w0 < margin || w1 < margin || w2 < margin)
        continue;

      float products[3];
      int ok = 0;
      Pixel expected = ref_shade(&tri, &pr, &scaled, 1, ambient, base, products,
                                 x, y, &ok);
      if (!ok)
        continue;

      Pixel wrong_position =
          ref_shade(&tri, &pr, &unscaled, 1, ambient, base, 0, x, y, &ok);
      Pixel wrong_range =
          ref_shade(&tri, &pr, &range_scaled, 1, ambient, base, 0, x, y, &ok);

      TASK_CHECK(pixel_eq(frame_at(x, y), expected));
      TASK_CHECK(!pixel_eq(wrong_position, expected));
      TASK_CHECK(!pixel_eq(wrong_range, expected));
      checked++;
    }
  }
  TASK_CHECK(checked == 3);

  frame_free();
  return 0;
}

// ---------------------------------------------------------------------------
// Fragment scene: one triangle with vertices at noticeably different depths,
// a non-uniformly scaled and rotated node, two coloured lights and a solid
// base colour.
// ---------------------------------------------------------------------------

typedef struct {
  Entity node;
  Object object;
  Mesh mesh;
  Vec3f verts[3];
  PointLight lights[2];
  Vec3f ambient;
  Pixel base;
  Mat4 world;
} FragmentScene;

static void fragment_build(FragmentScene *s) {
  s->verts[0] = (Vec3f){-1.0f, -0.8f, -0.8f};
  s->verts[1] = (Vec3f){1.1f, -0.7f, 0.55f};
  s->verts[2] = (Vec3f){0.0f, 0.95f, 0.9f};

  static const uint16_t indices[3] = {0, 1, 2};
  s->mesh.indexes_count = 3;
  s->mesh.pos_indices = indices;
  s->mesh.tex_indices = 0;
  s->mesh.positions = s->verts;
  s->mesh.textCoord = 0;

  s->base = pixelFromRGBA(160, 190, 230, 255);
  object_init(&s->object, &s->mesh, make_material(s->base));

  Mat4 scale = mat4Scale((Vec3f){1.25f, 0.85f, 1.0f});
  Mat4 turn = mat4RotateY(0.30f);
  Mat4 move = mat4Translate((Vec3f){0.30f, -0.02f, -3.5f});
  s->world = then(then(scale, turn), move);

  entity_init(&s->node, (Renderable *)&s->object, s->world);

  s->lights[0].position = (Vec3f){-0.30f, 0.50f, 0.90f};
  s->lights[0].intensity = (Vec3f){6.0f, 0.9f, 0.45f};
  s->lights[0].range = 2.4f;

  s->lights[1].position = (Vec3f){1.10f, -0.60f, 0.70f};
  s->lights[1].intensity = (Vec3f){0.4f, 2.2f, 1.0f};
  s->lights[1].range = 2.0f;

  s->ambient = (Vec3f){0.02f, 0.02f, 0.02f};
  entity_set_point_lights(&s->node, s->lights, 2);
}

static void fragment_triangle(const FragmentScene *s, RefTriangle *tri) {
  tri->local[0] = s->verts[0];
  tri->local[1] = s->verts[1];
  tri->local[2] = s->verts[2];
  tri->world = s->world;
}

static int fragment_project(const FragmentScene *s, RefTriangle *tri,
                            RefProjection *pr) {
  fragment_triangle(s, tri);
  return ref_project(tri, renderer.camera_view, renderer.camera_projection, pr);
}

static void fragment_lights(const FragmentScene *s, PointLight out[2]) {
  for (int i = 0; i < 2; i++) {
    Vec3f p = s->lights[i].position;
    out[i] = s->lights[i];
    out[i].position = mat4MultiplyVec3(&p, (Mat4 *)&s->world);
  }
}

static Pixel fragment_reference(const FragmentScene *s,
                                const PointLight *lights, size_t count,
                                Vec3f ambient, Pixel base, int x, int y,
                                int *covered) {
  RefTriangle tri;
  RefProjection pr;
  if (!fragment_project(s, &tri, &pr))
    return PIXELBLACK;
  return ref_shade(&tri, &pr, lights, count, ambient, base, 0, x, y, covered);
}

// Brightness of a whole triangle from one point, for the "face-wide" variant.
static Vec3f face_lighting(const RefTriangle *tri, const PointLight *lights,
                           size_t count, Vec3f ambient) {
  Vec3f centre = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    Vec3f w = ref_world_vertex(tri, i);
    centre = vec3fsumV(centre, vec3fmul(w, 1.0f / 3.0f));
  }
  return vec3fsumV(ambient,
                   ref_light_contribution(ref_normal(tri), centre, lights,
                                          count));
}

static int fragment_case_common(int use_material) {
  if (frame_init())
    return 1;

  FragmentScene s;
  fragment_build(&s);
  if (!use_material) {
    s.object.material = 0;
    s.mesh.tex_indices = 0;
  }

  renderer.camera_view = mat4RotateX(0.10f);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, s.ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  PointLight wl[2];
  fragment_lights(&s, wl);
  const Pixel base = use_material ? s.base : pixelFromUInt8(255);

  struct {
    int x, y;
    int r, g, b; // only meaningful on the textured path
  } samples[] = {
      {36, 27, 242, 46, 30},
      {39, 27, 196, 38, 25},
      {36, 21, 55, 13, 10},
  };

  RefTriangle tri;
  RefProjection pr;
  TASK_CHECK(fragment_project(&s, &tri, &pr));

  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    int covered = 0;
    float products[3];
    Pixel expected = ref_shade(&tri, &pr, wl, 2, s.ambient, base, products,
                               samples[i].x, samples[i].y, &covered);
    TASK_CHECK(covered);
    TASK_CHECK(pixel_eq(frame_at(samples[i].x, samples[i].y), expected));
    if (use_material) {
      TASK_CHECK(pixel_is(expected, samples[i].r, samples[i].g, samples[i].b,
                          255));
    } else {
      TASK_CHECK(expected.a == 255);
    }

    // A single brightness for the whole face differs from the per-fragment
    // result on at least one channel.
    Vec3f face = face_lighting(&tri, wl, 2, s.ambient);
    float face_products[3] = {face.x * (float)base.r, face.y * (float)base.g,
                              face.z * (float)base.b};
    int differs = 0;
    for (int c = 0; c < 3; c++)
      if (fabsf(face_products[c] - products[c]) > 3.0f)
        differs = 1;
    TASK_CHECK(differs);

    // So does an affine position (screen-linear barycentrics, no 1/w).
    int w0, w1, w2;
    TASK_CHECK(ref_weights(&pr, samples[i].x, samples[i].y, &w0, &w1, &w2));
    const float ssum = (float)(w0 + w1 + w2);
    Vec3f affine = {0, 0, 0};
    affine = vec3fsumV(affine, vec3fmul(ref_world_vertex(&tri, 0),
                                        (float)w0 / ssum));
    affine = vec3fsumV(affine, vec3fmul(ref_world_vertex(&tri, 1),
                                        (float)w1 / ssum));
    affine = vec3fsumV(affine, vec3fmul(ref_world_vertex(&tri, 2),
                                        (float)w2 / ssum));
    Vec3f affine_lit = vec3fsumV(
        s.ambient,
        ref_light_contribution(ref_normal(&tri), affine, wl, 2));
    float affine_products[3] = {affine_lit.x * (float)base.r,
                                affine_lit.y * (float)base.g,
                                affine_lit.z * (float)base.b};
    differs = 0;
    for (int c = 0; c < 3; c++)
      if (fabsf(affine_products[c] - products[c]) > 3.0f)
        differs = 1;
    TASK_CHECK(differs);
  }

  frame_free();
  return 0;
}

static int case_fragment_position_and_normal(void) {
  return fragment_case_common(1);
}

static int case_fragment_no_material(void) { return fragment_case_common(0); }

static int case_fragment_final_clamp(void) {
  if (frame_init())
    return 1;

  FragmentScene s;
  fragment_build(&s);
  renderer.camera_view = mat4RotateX(0.10f);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, s.ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  PointLight wl[2];
  fragment_lights(&s, wl);

  RefTriangle tri;
  RefProjection pr;
  TASK_CHECK(fragment_project(&s, &tri, &pr));

  int covered = 0;
  float products[3];
  Pixel expected = ref_shade(&tri, &pr, wl, 2, s.ambient, s.base, products, 38,
                             31, &covered);
  TASK_CHECK(covered);
  // The linear product is well past 255 on red: a clamp of the light sum to
  // one before the multiply would have produced 160 instead.
  TASK_CHECK(products[0] > 300.0f);
  TASK_CHECK(pixel_is(expected, 255, 77, 49, 255));
  TASK_CHECK(pixel_eq(frame_at(38, 31), expected));

  frame_free();
  return 0;
}

static int case_fragment_distance_window(void) {
  if (frame_init())
    return 1;

  FragmentScene s;
  fragment_build(&s);
  // The second light is out of range for the pixels checked below, so it must
  // contribute exactly zero there.
  s.lights[1].range = 0.7f;
  entity_set_point_lights(&s.node, s.lights, 2);

  renderer.camera_view = mat4RotateX(0.10f);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, s.ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  PointLight wl[2];
  fragment_lights(&s, wl);

  int covered = 0;
  Pixel expected = fragment_reference(&s, wl, 1, s.ambient, s.base, 39, 27,
                                      &covered);
  TASK_CHECK(covered);
  TASK_CHECK(pixel_eq(frame_at(39, 27), expected));

  // Further from the second light, well beyond its range and still on the
  // lit side of the surface. Only the first light may contribute: the pixel
  // equals the first light alone, and the blue channel would more than
  // double if the range were ignored.
  Pixel far = fragment_reference(&s, wl, 2, s.ambient, s.base, 37, 25,
                                 &covered);
  TASK_CHECK(covered);
  TASK_CHECK(pixel_is(far, 148, 29, 20, 255));
  TASK_CHECK(pixel_eq(frame_at(37, 25), far));

  Pixel first_only =
      fragment_reference(&s, wl, 1, s.ambient, s.base, 37, 25, &covered);
  TASK_CHECK(covered);
  TASK_CHECK(pixel_eq(far, first_only));

  RefTriangle tri;
  RefProjection pr;
  TASK_CHECK(fragment_project(&s, &tri, &pr));
  Vec3f point;
  TASK_CHECK(ref_world_position(&tri, &pr, 37, 25, &point));

  // Dropping the range test while keeping the same window leaks the second
  // light in here: green moves by several counts, blue by more than a third.
  Vec3f leaked =
      vec3fsumV(s.ambient, ref_light_contribution_ex(ref_normal(&tri), point,
                                                     wl, 2, 0));
  Pixel leaked_pixel;
  leaked_pixel.r = ref_byte(leaked.x * (float)s.base.r);
  leaked_pixel.g = ref_byte(leaked.y * (float)s.base.g);
  leaked_pixel.b = ref_byte(leaked.z * (float)s.base.b);
  leaked_pixel.a = s.base.a;
  TASK_CHECK(!pixel_eq(leaked_pixel, far));
  TASK_CHECK(abs((int)leaked_pixel.g - (int)far.g) > 6);
  TASK_CHECK(abs((int)leaked_pixel.b - (int)far.b) > 3);

  frame_free();
  return 0;
}

static int case_fragment_ambient_only(void) {
  if (frame_init())
    return 1;

  FragmentScene s;
  fragment_build(&s);
  entity_set_point_lights(&s.node, 0, 0);

  renderer.camera_view = mat4RotateX(0.10f);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  const Vec3f ambient = {0.30f, 0.20f, 0.10f};
  TASK_CHECK(renderer_set_point_lighting(&renderer, true, ambient) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);

  // 0.30*160 = 48, 0.20*190 = 38, 0.10*230 = 23
  int covered = 0;
  Pixel expected = fragment_reference(&s, 0, 0, ambient, s.base, 39, 27,
                                      &covered);
  TASK_CHECK(covered);
  TASK_CHECK(pixel_is(expected, 48, 38, 23, 255));
  TASK_CHECK(pixel_eq(frame_at(39, 27), expected));
  TASK_CHECK(pixel_is(frame_at(38, 23), 48, 38, 23, 255));
  TASK_CHECK(pixel_is(frame_at(33, 22), 48, 38, 23, 255));

  // A light on the back of the surface has a negative dot product: the image
  // stays ambient-only.
  PointLight behind;
  behind.position = (Vec3f){0.0f, 0.0f, -0.9f}; // behind the triangle plane
  behind.intensity = (Vec3f){5.0f, 5.0f, 5.0f};
  behind.range = 10.0f;
  entity_set_point_lights(&s.node, &behind, 1);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_is(frame_at(39, 27), 48, 38, 23, 255));
  TASK_CHECK(pixel_is(frame_at(33, 22), 48, 38, 23, 255));

  frame_free();
  return 0;
}

// ---------------------------------------------------------------------------
// Compat: the renderer without point lighting must reproduce the old image.
// ---------------------------------------------------------------------------

typedef struct {
  Entity node;
  Object object;
  Mesh mesh;
  Vec3f verts[3];
  Pixel base;
  int textured;
} CompatScene;

static void compat_build(CompatScene *s, int textured) {
  s->textured = textured;
  s->verts[0] = (Vec3f){-1.0f, -0.9f, 0.0f};
  s->verts[1] = (Vec3f){1.0f, -0.9f, 0.0f};
  s->verts[2] = (Vec3f){0.0f, 0.9f, 0.0f};

  static const uint16_t indices[3] = {0, 1, 2};
  s->mesh.indexes_count = 3;
  s->mesh.pos_indices = indices;
  s->mesh.tex_indices = 0;
  s->mesh.positions = s->verts;
  s->mesh.textCoord = 0;

  if (textured) {
    s->base = pixelFromRGBA(200, 100, 50, 180);
    object_init(&s->object, &s->mesh, make_material(s->base));
  } else {
    s->base = pixelFromUInt8(255);
    object_init(&s->object, &s->mesh, 0);
  }
  entity_init(&s->node, (Renderable *)&s->object,
              mat4Translate((Vec3f){0, 0, -3.0f}));
}

static Pixel compat_legacy_pixel(const CompatScene *s) {
  RefTriangle tri;
  tri.local[0] = s->verts[0];
  tri.local[1] = s->verts[1];
  tri.local[2] = s->verts[2];
  tri.world = s->node.transform;
  float brightness = ref_legacy_brightness(&tri, renderer.camera_view);
  Pixel base = s->textured ? s->base : pixelFromUInt8(255);
  return ref_legacy_pixel(base, brightness);
}

static int compat_check_legacy(const CompatScene *s) {
  Pixel expected = compat_legacy_pixel(s);
  int pts[][2] = {{32, 24}, {20, 15}, {32, 32}};
  for (int i = 0; i < 3; i++) {
    TASK_CHECK(pixel_eq(frame_at(pts[i][0], pts[i][1]), expected));
    TASK_CHECK(frame_at(pts[i][0], pts[i][1]).r > 0);
  }
  return 0;
}

static int case_compat_default_off(void) {
  if (frame_init())
    return 1;

  CompatScene s;
  compat_build(&s, 1);
  // A light is attached and the renderer is never told about lighting: the
  // image must be the old one.
  PointLight light;
  light.position = (Vec3f){0.0f, 0.0f, 1.0f};
  light.intensity = (Vec3f){3.0f, 2.0f, 1.0f};
  light.range = 2.0f;
  entity_set_point_lights(&s.node, &light, 1);

  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  TASK_CHECK(renderer_render(&renderer) == OK);

  TASK_CHECK(compat_check_legacy(&s) == 0);
  // base (200,100,50) times the legacy face brightness, alpha kept
  TASK_CHECK(pixel_is(frame_at(32, 24), 146, 73, 36, 180));
  TASK_CHECK(renderer.point_lighting_enabled == false);

  // Both entity initialisers leave the light list empty, and with lighting
  // enabled an empty list means ambient only - also from the plain
  // entity_init path, which this scene used.
  Entity fresh_a;
  Entity fresh_b;
  Entity no_children[1];
  TASK_CHECK(entity_init(&fresh_a, (Renderable *)&s.object,
                         mat4Identity()) == OK);
  TASK_CHECK(entity_init_children(&fresh_b, (Renderable *)&s.object,
                                  mat4Identity(), no_children, 1) == OK);
  TASK_CHECK(fresh_a.point_lights == 0 && fresh_a.point_lights_count == 0);
  TASK_CHECK(fresh_b.point_lights == 0 && fresh_b.point_lights_count == 0);

  // The same scene, enabled with no lights at all, is ambient only.
  entity_set_point_lights(&s.node, 0, 0);
  TASK_CHECK(renderer_set_point_lighting(&renderer, true,
                                         (Vec3f){0.25f, 0.5f, 0.75f}) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_is(frame_at(32, 24), 50, 50, 37, 180));

  // A NULL list with a non-zero count still means "no lights".
  entity_set_point_lights(&s.node, 0, 4);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_is(frame_at(32, 24), 50, 50, 37, 180));

  frame_free();
  return 0;
}

static int case_compat_toggle(void) {
  if (frame_init())
    return 1;

  CompatScene s;
  compat_build(&s, 1);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);

  TASK_CHECK(renderer_render(&renderer) == OK);
  const Pixel legacy = frame_at(32, 24);

  TASK_CHECK(renderer_set_point_lighting(&renderer, true,
                                         (Vec3f){0.25f, 0.5f, 0.75f}) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(!pixel_eq(frame_at(32, 24), legacy));

  // Off again, including an ambient passed on the way out, must bring back
  // the exact old bytes.
  TASK_CHECK(renderer_set_point_lighting(&renderer, false,
                                         (Vec3f){0.9f, 0.9f, 0.9f}) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_eq(frame_at(32, 24), legacy));
  TASK_CHECK(pixel_eq(frame_at(20, 15), legacy));

  frame_free();
  return 0;
}

static int case_compat_untextured(void) {
  if (frame_init())
    return 1;

  CompatScene s;
  compat_build(&s, 0);
  renderer_set_root_renderable(&renderer, (Renderable *)&s.node);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(compat_check_legacy(&s) == 0);

  // Enabled and ambient only: white surface times the ambient.
  TASK_CHECK(renderer_set_point_lighting(&renderer, true,
                                         (Vec3f){0.2f, 0.4f, 0.6f}) == OK);
  TASK_CHECK(renderer_render(&renderer) == OK);
  TASK_CHECK(pixel_is(frame_at(32, 24), 51, 102, 153, 255));

  frame_free();
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"hierarchy", "effective_lists", case_hierarchy_effective_lists},
    {"hierarchy", "root_switch", case_hierarchy_root_switch},
    {"hierarchy", "tree_edits", case_hierarchy_tree_edits},
    {"hierarchy", "renderer_isolation", case_hierarchy_renderer_isolation},
    {"hierarchy", "scale", case_hierarchy_scale},
    {"fragment", "position_and_normal", case_fragment_position_and_normal},
    {"fragment", "no_material", case_fragment_no_material},
    {"fragment", "final_clamp", case_fragment_final_clamp},
    {"fragment", "distance_window", case_fragment_distance_window},
    {"fragment", "ambient_only", case_fragment_ambient_only},
    {"compat", "default_off", case_compat_default_off},
    {"compat", "toggle", case_compat_toggle},
    {"compat", "untextured", case_compat_untextured},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
