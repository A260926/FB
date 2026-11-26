/*
 * Local point-light regression tests.
 *
 * The expected pixels are computed by a self-contained reference in this
 * file: it walks no production lighting code. Given a triangle, its expected
 * accumulated node matrix and the lights that ought to be active, it repeats
 * the screen setup and barycentric rasterization and then applies the point
 * light formula from the task specification in double precision.
 *
 * Groups:
 *   hierarchy - light ownership through the Entity tree and traversal state
 *   fragment  - per-fragment world position/normal math and color clamping
 *   compat    - default-off image and ambient-only behaviour
 */

#include "task_support/check.h"

#include "memory_backend.h"

#include "render/entity.h"
#include "render/lighting.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/renderer.h"
#include "render/texture.h"

#include "math/fun.h"
#include "math/mat4.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TW 64
#define TH 48
#define FRAME_PIXELS (TW * TH)

/* ------------------------------------------------------------------ */
/* Small value types                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
  double x, y, z;
} D3;

typedef struct {
  D3 pos;
  D3 intensity;
  double range;
} DLight;

typedef struct {
  const Vec3f *verts; /* exactly three local-space vertices */
  Mat4 model;
  const DLight *lights;
  size_t nlights;
  D3 ambient;
  Pixel base;
} Ref;

typedef struct {
  MemoryBackend mb;
  Renderer renderer;
} Kit;

static int kit_start(Kit *kit) {
  if (memory_backend_init(&kit->mb, (Vec2i){TW, TH}) != 0)
    return 1;
  if (renderer_init(&kit->renderer, (Vec2i){TW, TH},
                    (Backend *)&kit->mb) != 0)
    return 1;
  kit->renderer.camera_projection =
      mat4Perspective(1.0f, 50.0f, (float)TW / (float)TH, 0.6f);
  kit->renderer.camera_view = mat4Translate((Vec3f){0.0f, 0.0f, 0.0f});
  return 0;
}

static void kit_finish(Kit *kit) { memory_backend_free(&kit->mb); }

static Mat4 ref_projection(void) {
  return mat4Perspective(1.0f, 50.0f, (float)TW / (float)TH, 0.6f);
}

/* Expected world-space light from a local PointLight and the full matrix of
 * the node it is bound to. Uses the public math library only. */
static DLight dlight_make(const PointLight *pl, const Mat4 *node_world) {
  Vec4f local = {pl->position.x, pl->position.y, pl->position.z, 1.0f};
  Mat4 wm = *node_world;
  Vec4f w = mat4MultiplyVec4(&local, &wm);
  DLight dl;
  dl.pos = (D3){w.x, w.y, w.z};
  dl.intensity =
      (D3){pl->intensity.x, pl->intensity.y, pl->intensity.z};
  dl.range = (double)pl->range;
  return dl;
}

/* ------------------------------------------------------------------ */
/* Independent reference                                               */
/* ------------------------------------------------------------------ */

static D3 ref_evaluate_lights(const Ref *rf, D3 n, D3 p) {
  D3 color = rf->ambient;
  for (size_t i = 0; i < rf->nlights; ++i) {
    const DLight *l = &rf->lights[i];
    double dx = l->pos.x - p.x;
    double dy = l->pos.y - p.y;
    double dz = l->pos.z - p.z;
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    if (!(d > 0.0) || !(d < l->range))
      continue;
    double ndl = (n.x * dx + n.y * dy + n.z * dz) / d;
    if (ndl < 0.0)
      ndl = 0.0;
    double t = 1.0 - d / l->range;
    double fall = ndl * t * t / (1.0 + d * d);
    color.x += l->intensity.x * fall;
    color.y += l->intensity.y * fall;
    color.z += l->intensity.z * fall;
  }
  return color;
}

static Pixel ref_shade_pixel(const Ref *rf, D3 light) {
  double r = light.x * (double)rf->base.r;
  double g = light.y * (double)rf->base.g;
  double b = light.z * (double)rf->base.b;
  if (r < 0.0)
    r = 0.0;
  else if (r > 255.0)
    r = 255.0;
  if (g < 0.0)
    g = 0.0;
  else if (g > 255.0)
    g = 255.0;
  if (b < 0.0)
    b = 0.0;
  else if (b > 255.0)
    b = 255.0;
  return pixelFromRGBA((uint8_t)r, (uint8_t)g, (uint8_t)b, rf->base.a);
}

typedef struct {
  int covered;
  int x, y;
  int32_t w0, w1, w2;
  double aw, bw, cw;
  D3 world[3];
  D3 p_persp;
  D3 p_affine;
  D3 normal;
  D3 light;
  double raw[3]; /* unclamped byte-space channels */
  int sensitive; /* within rounding margin of a truncation/clamp edge */
  Pixel expected;
} Info;

/* A byte value this close to an integer boundary can be pushed across it by
 * float-vs-double evaluation order; such pixels are checked with +-1. */
static int boundary_sensitive(double v) {
  if (v < 0.05 || v > 254.95)
    return 1;
  double f = v - floor(v);
  return f < 0.02 || f > 0.98;
}

/* Full reference analysis of one screen pixel. */
static Info ref_info(const Ref *rf, int px, int py) {
  Info info;
  memset(&info, 0, sizeof info);
  info.x = px;
  info.y = py;

  Mat4 proj = ref_projection();
  Vec4f clip[3];
  int scr[3][2];
  int32_t minX = TW, minY = TH, maxX = 0, maxY = 0;

  for (int k = 0; k < 3; ++k) {
    Vec4f loc = {rf->verts[k].x, rf->verts[k].y, rf->verts[k].z, 1.0f};
    Mat4 mm = rf->model;
    Vec4f w4 = mat4MultiplyVec4(&loc, &mm);
    double iw = w4.w != 0.0f ? 1.0 / (double)w4.w : 1.0;
    info.world[k] =
        (D3){(double)w4.x * iw, (double)w4.y * iw, (double)w4.z * iw};

    Mat4 pm = proj;
    clip[k] = mat4MultiplyVec4(&w4, &pm);
    /* Direct division on purpose: a reciprocal multiply can flip the
     * truncation of a screen vertex that sits near a half pixel. */
    float nx = clip[k].x / clip[k].w;
    float ny = clip[k].y / clip[k].w;
    float halfX = TW * 0.5f;
    float halfY = TH * 0.5f;
    scr[k][0] = (int)(nx * halfX + halfX);
    scr[k][1] = (int)(ny * halfY + halfY);
    minX = MIN(minX, scr[k][0]);
    minY = MIN(minY, scr[k][1]);
    maxX = MAX(maxX, scr[k][0]);
    maxY = MAX(maxY, scr[k][1]);
  }

  minX = MIN(MAX(minX, 0), TW);
  minY = MIN(MAX(minY, 0), TH);
  maxX = MIN(MAX(maxX, 0), TW);
  maxY = MIN(MAX(maxY, 0), TH);

  /* Half-open bbox, matching the rasterizer loop. */
  if (px < minX || px >= maxX || py < minY || py >= maxY)
    return info;

  Vec2i a = {scr[0][0], scr[0][1]};
  Vec2i b = {scr[1][0], scr[1][1]};
  Vec2i c = {scr[2][0], scr[2][1]};
  info.w0 = orient2d(b, c, (Vec2i){px, py});
  info.w1 = orient2d(c, a, (Vec2i){px, py});
  info.w2 = orient2d(a, b, (Vec2i){px, py});
  if ((info.w0 | info.w1 | info.w2) < 0)
    return info;

  int32_t area = orient2d(a, b, c);
  if (area == 0)
    return info;

  /* Depth range gate, same expression as object.c. */
  float az = clip[0].z / clip[0].w;
  float bz = clip[1].z / clip[1].w;
  float cz = clip[2].z / clip[2].w;
  float depth =
      -(info.w0 * az + info.w1 * bz + info.w2 * cz) / (float)area;
  if (depth < -1.0f || depth > 1.0f)
    return info;

  info.covered = 1;
  info.aw = clip[0].w;
  info.bw = clip[1].w;
  info.cw = clip[2].w;

  double q0p = (double)info.w0 / info.aw;
  double q1p = (double)info.w1 / info.bw;
  double q2p = (double)info.w2 / info.cw;
  double qsp = q0p + q1p + q2p;

  double q0a = info.w0, q1a = info.w1, q2a = info.w2;
  double qsa = q0a + q1a + q2a;

  info.p_persp = (D3){
      (q0p * info.world[0].x + q1p * info.world[1].x +
       q2p * info.world[2].x) /
          qsp,
      (q0p * info.world[0].y + q1p * info.world[1].y +
       q2p * info.world[2].y) /
          qsp,
      (q0p * info.world[0].z + q1p * info.world[1].z +
       q2p * info.world[2].z) /
          qsp};
  info.p_affine = (D3){
      (q0a * info.world[0].x + q1a * info.world[1].x +
       q2a * info.world[2].x) /
          qsa,
      (q0a * info.world[0].y + q1a * info.world[1].y +
       q2a * info.world[2].y) /
          qsa,
      (q0a * info.world[0].z + q1a * info.world[1].z +
       q2a * info.world[2].z) /
          qsa};

  D3 e1 = {info.world[0].x - info.world[1].x,
           info.world[0].y - info.world[1].y,
           info.world[0].z - info.world[1].z};
  D3 e2 = {info.world[0].x - info.world[2].x,
           info.world[0].y - info.world[2].y,
           info.world[0].z - info.world[2].z};
  D3 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
          e1.x * e2.y - e1.y * e2.x};
  double nl = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  info.normal = (D3){n.x / nl, n.y / nl, n.z / nl};

  info.light = ref_evaluate_lights(rf, info.normal, info.p_persp);
  info.raw[0] = info.light.x * (double)rf->base.r;
  info.raw[1] = info.light.y * (double)rf->base.g;
  info.raw[2] = info.light.z * (double)rf->base.b;
  info.sensitive = boundary_sensitive(info.raw[0]) ||
                   boundary_sensitive(info.raw[1]) ||
                   boundary_sensitive(info.raw[2]);
  info.expected = ref_shade_pixel(rf, info.light);
  return info;
}

static int color_max_delta(Pixel got, Pixel exp) {
  int d = abs((int)got.r - (int)exp.r);
  d = MAX(d, abs((int)got.g - (int)exp.g));
  d = MAX(d, abs((int)got.b - (int)exp.b));
  d = MAX(d, abs((int)got.a - (int)exp.a));
  return d;
}

static int g_ref_checked;
static int g_ref_robust;

/* Scans every reference-covered pixel. Robust pixels must match byte-exactly;
 * pixels sitting on a float rounding boundary may differ by one. Alpha is
 * always exact. Returns the number of genuine mismatches. */
static int scan_ref(Kit *kit, const Ref *ref, int tol) {
  int bad = 0;
  g_ref_checked = 0;
  g_ref_robust = 0;
  for (int y = 0; y < TH; ++y) {
    for (int x = 0; x < TW; ++x) {
      Info info = ref_info(ref, x, y);
      if (!info.covered)
        continue;
      ++g_ref_checked;
      if (!info.sensitive)
        ++g_ref_robust;
      Pixel got = kit->mb.frame[x + y * TW];
      int dr = abs((int)got.r - (int)info.expected.r);
      int dg = abs((int)got.g - (int)info.expected.g);
      int db = abs((int)got.b - (int)info.expected.b);
      int da = abs((int)got.a - (int)info.expected.a);
      int md = MAX(MAX(dr, dg), db);
      int allowed = MAX(tol, info.sensitive ? 1 : 0);
      if (md > allowed || da != 0) {
        if (bad < 8)
          fprintf(stderr,
                  "  scan mismatch (%d,%d) actual bgra=(%u,%u,%u,%u) "
                  "expected=(%u,%u,%u,%u) d=(%d,%d,%d,%d)%s "
                  "P=(%.2f,%.2f,%.2f) N=(%.2f,%.2f,%.2f) light=(%.2f,%.2f,%.2f)\n",
                  x, y, got.b, got.g, got.r, got.a, info.expected.b,
                  info.expected.g, info.expected.r, info.expected.a, db,
                  dg, dr, da, info.sensitive ? " boundary" : "",
                  info.p_persp.x, info.p_persp.y, info.p_persp.z,
                  info.normal.x, info.normal.y, info.normal.z,
                  info.light.x, info.light.y, info.light.z);
        ++bad;
      }
    }
  }
  return bad;
}

/* Explicit single-pixel assertion helper. */
static int probe_matches(Kit *kit, const Ref *ref, int x, int y, int tol,
                         const char *tag) {
  Info info = ref_info(ref, x, y);
  if (!info.covered) {
    fprintf(stderr, "  probe %s (%d,%d) is not reference-covered\n", tag, x,
            y);
    return 0;
  }
  Pixel got = kit->mb.frame[x + y * TW];
  int md = color_max_delta(got, info.expected);
  int allowed = MAX(tol, info.sensitive ? 1 : 0);
  if (md > allowed) {
    fprintf(stderr,
            "  probe %s (%d,%d) actual bgra=(%u,%u,%u,%u) expected="
            "(%u,%u,%u,%u)\n",
            tag, x, y, got.b, got.g, got.r, got.a, info.expected.b,
            info.expected.g, info.expected.r, info.expected.a);
    return 0;
  }
  return 1;
}

/* Finds a covered pixel well away from rounding boundaries, for an exact
 * TASK_CHECK probe. Returns 1 and fills x/y when one exists. */
static int find_robust_probe(Kit *kit, const Ref *ref, int *x, int *y) {
  for (int yy = 0; yy < TH; ++yy) {
    for (int xx = 0; xx < TW; ++xx) {
      Info info = ref_info(ref, xx, yy);
      if (!info.covered || info.sensitive)
        continue;
      Pixel got = kit->mb.frame[xx + yy * TW];
      if (color_max_delta(got, info.expected) == 0) {
        *x = xx;
        *y = yy;
        return 1;
      }
    }
  }
  return 0;
}

/* A pixel well inside the triangle, from the average screen position. */
static Vec2i ref_center(const Ref *ref) {
  Mat4 proj = ref_projection();
  double sx = 0.0, sy = 0.0;
  for (int k = 0; k < 3; ++k) {
    Vec4f loc = {ref->verts[k].x, ref->verts[k].y, ref->verts[k].z, 1.0f};
    Mat4 mm = ref->model;
    Vec4f w4 = mat4MultiplyVec4(&loc, &mm);
    Mat4 pm = proj;
    Vec4f c4 = mat4MultiplyVec4(&w4, &pm);
    float nx = c4.x / c4.w, ny = c4.y / c4.w;
    sx += nx * (TW * 0.5f) + TW * 0.5f;
    sy += ny * (TH * 0.5f) + TH * 0.5f;
  }
  return (Vec2i){(int)(sx / 3.0), (int)(sy / 3.0)};
}

/* ------------------------------------------------------------------ */
/* Scene fixtures                                                      */
/* ------------------------------------------------------------------ */

static Pixel white_base(void) { return pixelFromUInt8(255); }

static void empty_object(Object *o, Mesh *m) {
  memset(m, 0, sizeof *m);
  m->indexes_count = 0;
  object_init(o, m, 0);
}

static void triangle_mesh(Mesh *m, Vec3f positions[3], uint16_t indices[3]) {
  memset(m, 0, sizeof *m);
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  m->indexes_count = 3;
  m->positions = positions;
  m->pos_indices = indices;
}

/* Uniform-scale placement in front of the identity camera. */
static Mat4 placement(float x, float y, float z, float s) {
  Mat4 sc = mat4Scale((Vec3f){s, s, s});
  Mat4 tr = mat4Translate((Vec3f){x, y, z});
  return mat4MultiplyM(&sc, &tr); /* tr * sc */
}

static void render_once(Kit *kit) { renderer_render(&kit->renderer); }

/* ------------------------------------------------------------------ */
/* hierarchy group                                                     */
/* ------------------------------------------------------------------ */

/* A root light and a light on the drawing node itself must both reach the
 * triangle, each transformed by its own binding matrix. */
static int case_h_parent(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.1f, -0.9f, 0.4f},
                  {1.1f, -0.9f, -0.4f},
                  {0.0f, 1.0f, 0.0f}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);

  Mat4 model = placement(0.0f, 0.0f, -3.6f, 1.15f);
  Entity node;
  entity_init(&node, (Renderable *)&object, model);

  PointLight own = {{0.3f, 0.2f, 0.4f}, {6.0f, 5.0f, 3.0f}, 12.0f};
  entity_set_point_lights(&node, &own, 1);

  /* In front of the triangle (normal +z) so this light clearly contributes. */
  PointLight root_pl = {{-1.2f, 0.9f, -1.4f}, {3.5f, 3.0f, 2.5f}, 14.0f};
  Entity root;
  Mesh root_mesh;
  Object root_obj;
  empty_object(&root_obj, &root_mesh);
  entity_init(&root, (Renderable *)&root_obj, mat4Identity());
  Entity child_storage[1] = {node};
  entity_init_children(&root, (Renderable *)&root_obj, mat4Identity(),
                       child_storage, 1);
  /* Bind every light after the init entry point, which defaults to none. */
  entity_set_point_lights(&root, &root_pl, 1);
  entity_set_point_lights(&child_storage[0], &own, 1);

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&root);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.05f, 0.05f, 0.05f});
  render_once(&kit);

  DLight dl_own = dlight_make(&own, &model);
  Mat4 identity = mat4Identity();
  DLight dl_root = dlight_make(&root_pl, &identity);
  DLight all[2] = {dl_own, dl_root};
  Ref ref = {pos, model, all, 2, {0.05, 0.05, 0.05}, white_base()};

  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);
  TASK_CHECK(g_ref_checked > 200);

  Vec2i c = ref_center(&ref);
  TASK_CHECK(probe_matches(&kit, &ref, c.x, c.y, 0, "center"));
  TASK_CHECK(probe_matches(&kit, &ref, c.x - 6, c.y + 4, 0, "offset"));

  kit_finish(&kit);
  return 0;
}

/* Two sibling subtrees reference the very same Object but must each be lit by
 * their own light only. Swapping sibling order must not change either region.
 */
static int sibling_scene(Kit *kit, Entity *root, Entity kids[2],
                         Object *shared_object, Mat4 *left_m, Mat4 *right_m,
                         int swap) {
  static Vec3f pos[3] = {{-0.7f, -0.6f, 0.0f},
                         {0.7f, -0.6f, 0.0f},
                         {0.0f, 0.8f, 0.0f}};
  static uint16_t idx[3];
  static Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  object_init(shared_object, &mesh, 0);

  *left_m = placement(-1.65f, 0.25f, -3.4f, 0.85f);
  *right_m = placement(1.65f, 0.25f, -3.4f, 0.85f);

  PointLight red = {{0.0f, 0.0f, 0.0f}, {9.0f, 1.2f, 1.2f}, 20.0f};
  PointLight blue = {{0.0f, 0.0f, 0.0f}, {1.2f, 2.2f, 9.0f}, 20.0f};

  entity_init(&kids[0], (Renderable *)shared_object, *left_m);
  entity_set_point_lights(&kids[0], &red, 1);
  entity_init(&kids[1], (Renderable *)shared_object, *right_m);
  entity_set_point_lights(&kids[1], &blue, 1);

  if (swap) {
    Entity tmp = kids[0];
    kids[0] = kids[1];
    kids[1] = tmp;
  }

  static Mesh root_mesh;
  static Object root_obj;
  empty_object(&root_obj, &root_mesh);
  entity_init_children(root, (Renderable *)&root_obj, mat4Identity(), kids, 2);

  if (renderer_set_root_renderable(&kit->renderer, (Renderable *)root) != 0)
    return 1;
  renderer_set_point_lighting(&kit->renderer, true,
                              (Vec3f){0.03f, 0.03f, 0.03f});
  render_once(kit);
  return 0;
}

static int case_h_siblings(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Entity root;
  Entity kids[2];
  Object shared;
  Mat4 left_m, right_m;
  TASK_CHECK(sibling_scene(&kit, &root, kids, &shared, &left_m, &right_m, 0) ==
             0);

  static Vec3f pos[3] = {{-0.7f, -0.6f, 0.0f},
                         {0.7f, -0.6f, 0.0f},
                         {0.0f, 0.8f, 0.0f}};
  PointLight red = {{0, 0, 0}, {9, 1.2f, 1.2f}, 20};
  PointLight blue = {{0, 0, 0}, {1.2f, 2.2f, 9}, 20};
  DLight dlr = dlight_make(&red, &left_m);
  DLight dlb = dlight_make(&blue, &right_m);

  Ref left_ref = {pos, left_m, &dlr, 1, {0.03, 0.03, 0.03}, white_base()};
  Ref right_ref = {pos, right_m, &dlb, 1, {0.03, 0.03, 0.03}, white_base()};

  TASK_CHECK(scan_ref(&kit, &left_ref, 0) == 0);
  int left_count = g_ref_checked;
  TASK_CHECK(left_count > 60);
  TASK_CHECK(scan_ref(&kit, &right_ref, 0) == 0);
  int right_count = g_ref_checked;
  TASK_CHECK(right_count > 60);

  Vec2i lc = ref_center(&left_ref);
  Vec2i rc = ref_center(&right_ref);
  TASK_CHECK(probe_matches(&kit, &left_ref, lc.x, lc.y, 0, "left"));
  TASK_CHECK(probe_matches(&kit, &right_ref, rc.x, rc.y, 0, "right"));

  /* Re-render with swapped sibling order and re-check both regions. */
  TASK_CHECK(sibling_scene(&kit, &root, kids, &shared, &left_m, &right_m, 1) ==
             0);
  dlr = dlight_make(&red, &left_m);
  dlb = dlight_make(&blue, &right_m);
  left_ref.model = left_m;
  right_ref.model = right_m;
  TASK_CHECK(scan_ref(&kit, &left_ref, 0) == 0);
  TASK_CHECK(scan_ref(&kit, &right_ref, 0) == 0);

  kit_finish(&kit);
  return 0;
}

/* Lights append down a subtree and stop applying outside it. */
static int case_h_subtree(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  static Vec3f pos[3] = {{-0.7f, -0.6f, 0},
                         {0.7f, -0.6f, 0},
                         {0, 0.8f, 0}};
  static uint16_t idx[3];
  static Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object shared;
  object_init(&shared, &mesh, 0);

  Mat4 m_a = placement(-1.7f, 0.35f, -3.4f, 0.8f);
  Mat4 m_b = placement(1.7f, 0.35f, -3.4f, 0.8f);
  /* G is a child of A, so this is its LOCAL transform; its accumulated
   * world matrix is m_a * g_local under the normal composition rule. */
  Mat4 g_local = mat4Translate((Vec3f){0.25f, -2.05f, -0.1f});
  Mat4 m_g = mat4MultiplyM(&g_local, &m_a);

  PointLight root_l = {{0, 0.5f, -1.1f}, {2.4f, 2.4f, 2.4f}, 20};
  PointLight a_l = {{0, 0, 0.6f}, {8, 1.4f, 1.4f}, 20};
  PointLight g_l = {{0, 0, 0.6f}, {1.4f, 8, 2.0f}, 20};

  Entity root;
  Entity root_kids[2];
  Entity a_kids[1];
  Mesh rm;
  Object ro;
  empty_object(&ro, &rm);

  entity_init(&root, (Renderable *)&ro, mat4Identity());
  entity_init(&root_kids[0], (Renderable *)&shared, m_a);
  entity_init(&root_kids[1], (Renderable *)&shared, m_b);
  entity_init(&a_kids[0], (Renderable *)&shared, g_local);

  entity_init_children(&root_kids[0], (Renderable *)&shared, m_a, a_kids, 1);
  entity_init_children(&root, (Renderable *)&ro, mat4Identity(), root_kids, 2);

  /* Bind after every init entry: init deliberately leaves lights empty. */
  entity_set_point_lights(&root, &root_l, 1);
  entity_set_point_lights(&root_kids[0], &a_l, 1);
  entity_set_point_lights(&a_kids[0], &g_l, 1);

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&root);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.04f, 0.04f, 0.04f});
  render_once(&kit);

  Mat4 idm = mat4Identity();
  DLight dlr = dlight_make(&root_l, &idm);
  DLight dla = dlight_make(&a_l, &m_a);
  DLight dlg = dlight_make(&g_l, &m_g);

  DLight ab[2] = {dlr, dla};
  Ref ref_a = {pos, m_a, ab, 2, {0.04, 0.04, 0.04}, white_base()};
  DLight rb[1] = {dlr};
  Ref ref_b = {pos, m_b, rb, 1, {0.04, 0.04, 0.04}, white_base()};
  DLight gab[3] = {dlr, dla, dlg};
  Ref ref_g = {pos, m_g, gab, 3, {0.04, 0.04, 0.04}, white_base()};

  TASK_CHECK(scan_ref(&kit, &ref_a, 0) == 0);
  TASK_CHECK(g_ref_checked > 50);
  TASK_CHECK(scan_ref(&kit, &ref_b, 0) == 0);
  TASK_CHECK(g_ref_checked > 50);
  TASK_CHECK(scan_ref(&kit, &ref_g, 0) == 0);
  TASK_CHECK(g_ref_checked > 40);

  Vec2i ca = ref_center(&ref_a);
  Vec2i cb = ref_center(&ref_b);
  Vec2i cg = ref_center(&ref_g);
  TASK_CHECK(probe_matches(&kit, &ref_a, ca.x, ca.y, 0, "A"));
  TASK_CHECK(probe_matches(&kit, &ref_b, cb.x, cb.y, 0, "B"));
  TASK_CHECK(probe_matches(&kit, &ref_g, cg.x, cg.y, 0, "G"));

  kit_finish(&kit);
  return 0;
}

/* Same Renderer changing roots, and two Renderers interleaved. */
static int case_h_switch_interleave(void) {
  Kit k1, k2;
  TASK_CHECK(kit_start(&k1) == 0);
  TASK_CHECK(kit_start(&k2) == 0);

  Vec3f pa[3] = {{-0.8f, -0.7f, 0.3f},
                 {0.8f, -0.7f, -0.3f},
                 {0, 0.9f, 0}};
  Vec3f pb[3] = {{-0.8f, -0.7f, -0.2f},
                 {0.8f, -0.7f, 0.4f},
                 {0, 0.9f, 0.1f}};
  uint16_t ia[3], ib[3];
  Mesh ma, mb;
  triangle_mesh(&ma, pa, ia);
  triangle_mesh(&mb, pb, ib);
  Object oa, ob;
  object_init(&oa, &ma, 0);
  object_init(&ob, &mb, 0);

  Mat4 xa = placement(-0.9f, 0.1f, -3.5f, 1.0f);
  Mat4 xb = placement(0.9f, -0.2f, -3.8f, 1.0f);
  PointLight la = {{0, 0, 0}, {8, 5, 2}, 20};
  PointLight lb = {{0, 0, 0}, {2, 6, 9}, 20};

  Entity ea, eb;
  entity_init(&ea, (Renderable *)&oa, xa);
  entity_set_point_lights(&ea, &la, 1);
  entity_init(&eb, (Renderable *)&ob, xb);
  entity_set_point_lights(&eb, &lb, 1);

  DLight dla = dlight_make(&la, &xa);
  DLight dlb = dlight_make(&lb, &xb);
  Ref ra = {pa, xa, &dla, 1, {0.05, 0.05, 0.05}, white_base()};
  Ref rb = {pb, xb, &dlb, 1, {0.05, 0.05, 0.05}, white_base()};

  /* k1 renders A, then B, then A again. */
  renderer_set_root_renderable(&k1.renderer, (Renderable *)&ea);
  renderer_set_point_lighting(&k1.renderer, true,
                              (Vec3f){0.05f, 0.05f, 0.05f});
  render_once(&k1);
  static Pixel save_a[FRAME_PIXELS];
  memcpy(save_a, k1.mb.frame, sizeof save_a);
  TASK_CHECK(scan_ref(&k1, &ra, 0) == 0);

  renderer_set_root_renderable(&k1.renderer, (Renderable *)&eb);
  render_once(&k1);
  TASK_CHECK(scan_ref(&k1, &rb, 0) == 0);

  renderer_set_root_renderable(&k1.renderer, (Renderable *)&ea);
  render_once(&k1);
  TASK_CHECK(memcmp(save_a, k1.mb.frame, sizeof save_a) == 0);
  TASK_CHECK(scan_ref(&k1, &ra, 0) == 0);

  /* Interleave k1/B and k2/A draws. */
  renderer_set_point_lighting(&k2.renderer, true,
                              (Vec3f){0.05f, 0.05f, 0.05f});
  renderer_set_root_renderable(&k2.renderer, (Renderable *)&ea);
  render_once(&k2);
  TASK_CHECK(scan_ref(&k2, &ra, 0) == 0);

  renderer_set_root_renderable(&k1.renderer, (Renderable *)&eb);
  render_once(&k1);
  renderer_set_root_renderable(&k2.renderer, (Renderable *)&ea);
  render_once(&k2);
  TASK_CHECK(scan_ref(&k1, &rb, 0) == 0);
  TASK_CHECK(scan_ref(&k2, &ra, 0) == 0);

  kit_finish(&k1);
  kit_finish(&k2);
  return 0;
}

/* An invisible node renders neither itself nor its subtree, so its lights
 * cannot leak into the frame. */
static int case_h_invisible(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.1f, -0.9f, 0},
                  {1.1f, -0.9f, 0},
                  {0, 1.0f, 0}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);
  Mat4 model = placement(0, 0, -2.8f, 1.0f);
  Entity node;
  entity_init(&node, (Renderable *)&object, model);
  PointLight pl = {{0, 0, 1.0f}, {9, 9, 9}, 20};
  entity_set_point_lights(&node, &pl, 1);

  DLight dl = dlight_make(&pl, &model);
  Ref ref = {pos, model, &dl, 1, {0.05, 0.05, 0.05}, white_base()};

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&node);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.05f, 0.05f, 0.05f});
  render_once(&kit);
  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);

  Vec2i c = ref_center(&ref);
  TASK_CHECK(kit.mb.frame[c.x + c.y * TW].a == 255);

  node.visible = false;
  render_once(&kit);
  Pixel hidden = kit.mb.frame[c.x + c.y * TW];
  TASK_CHECK(hidden.a == 0);

  node.visible = true;
  render_once(&kit);
  TASK_CHECK(probe_matches(&kit, &ref, c.x, c.y, 0, "visible again"));

  kit_finish(&kit);
  return 0;
}

/* ------------------------------------------------------------------ */
/* fragment group                                                      */
/* ------------------------------------------------------------------ */

static int case_f_formula(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.35f, -1.0f, 1.7f},
                  {1.35f, -1.0f, -1.7f},
                  {0.15f, 1.1f, 0.25f}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);

  /* Nested placement: root translation, child non-uniform scale * rotation. */
  Mat4 root_m = mat4Translate((Vec3f){0.0f, 0.0f, -4.7f});
  Mat4 sc = mat4Scale((Vec3f){1.25f, 0.8f, 1.15f});
  Mat4 rot = mat4RotateZ(0.55f);
  Mat4 child_l = mat4MultiplyM(&sc, &rot); /* rot * sc */
  Mat4 model = mat4MultiplyM(&child_l, &root_m);

  Entity root;
  Mesh rm;
  Object ro;
  empty_object(&ro, &rm);

  PointLight local_l = {{0.25f, -0.25f, 0.6f}, {7.0f, 3.2f, 2.2f}, 13.0f};
  PointLight root_l = {{-1.6f, 1.0f, -1.2f}, {2.0f, 5.0f, 8.0f}, 16.0f};

  Entity kids[1];
  entity_init(&root, (Renderable *)&ro, root_m);
  entity_init(&kids[0], (Renderable *)&object, child_l);
  entity_init_children(&root, (Renderable *)&ro, root_m, kids, 1);
  /* Bind after the init entry points. */
  entity_set_point_lights(&kids[0], &local_l, 1);
  entity_set_point_lights(&root, &root_l, 1);

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&root);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.06f, 0.06f, 0.08f});
  render_once(&kit);

  DLight dl1 = dlight_make(&local_l, &model);
  DLight dl2 = dlight_make(&root_l, &root_m);
  DLight dls[2] = {dl1, dl2};
  Ref ref = {pos, model, dls, 2, {0.06, 0.06, 0.08}, white_base()};

  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);
  TASK_CHECK(g_ref_checked > 180);
  TASK_CHECK(g_ref_robust > 60);

  Vec2i ctr = ref_center(&ref);
  TASK_CHECK(probe_matches(&kit, &ref, ctr.x, ctr.y, 0, "center"));
  int rx = 0, ry = 0;
  TASK_CHECK(find_robust_probe(&kit, &ref, &rx, &ry));
  TASK_CHECK(probe_matches(&kit, &ref, rx, ry, 0, "exact robust"));

  /* The scene must actually discriminate the wrong interpolation models. */
  int found_affine = 0, found_face = 0, found_variation = 0;
  int best_var = 0;
  int have_first = 0;
  Pixel first_color = {0};
  for (int y = 1; y < TH - 1; ++y) {
    for (int x = 1; x < TW - 1; ++x) {
      Info a = ref_info(&ref, x, y);
      if (!a.covered)
        continue;
      Pixel got = kit.mb.frame[x + y * TW];
      if (color_max_delta(got, a.expected) != 0)
        continue;

      /* Affine (screen-barycentric only) world position. */
      D3 la = ref_evaluate_lights(&ref, a.normal, a.p_affine);
      Pixel pa = ref_shade_pixel(&ref, la);
      int da = color_max_delta(a.expected, pa);
      if (da >= 6 && !found_affine) {
        found_affine = 1;
        TASK_CHECK(color_max_delta(got, pa) >= 6);
      }

      /* Face-level lighting: evaluate at the face centroid once. */
      D3 centroid = {
          (a.world[0].x + a.world[1].x + a.world[2].x) / 3.0,
          (a.world[0].y + a.world[1].y + a.world[2].y) / 3.0,
          (a.world[0].z + a.world[1].z + a.world[2].z) / 3.0};
      D3 lf = ref_evaluate_lights(&ref, a.normal, centroid);
      Pixel pf = ref_shade_pixel(&ref, lf);
      int df = color_max_delta(a.expected, pf);
      if (df >= 5 && !found_face) {
        found_face = 1;
        TASK_CHECK(color_max_delta(got, pf) >= 5);
      }

      /* Two covered pixels whose correct colours differ strongly. */
      if (!have_first) {
        first_color = a.expected;
        have_first = 1;
      } else {
        int v = color_max_delta(a.expected, first_color);
        if (v > best_var)
          best_var = v;
      }
    }
  }
  found_variation = best_var >= 40;
  TASK_CHECK(found_affine);
  TASK_CHECK(found_face);
  TASK_CHECK(found_variation);

  kit_finish(&kit);
  return 0;
}

/* Dark textured base plus over-bright coloured lights: clamping the light sum
 * before the multiply would give a different, darker pixel. */
static int case_f_texture_clamp(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Pixel texels[4];
  texels[0] = pixelFromRGBA(36, 28, 44, 205);
  texels[1] = texels[0];
  texels[2] = texels[0];
  texels[3] = texels[0];
  Texture texture;
  texture_init(&texture, (Vec2i){2, 2}, texels);
  Material material;
  material_init(&material, &texture);

  Vec3f pos[3] = {{-1.2f, -0.9f, 0.2f},
                  {1.2f, -0.9f, -0.2f},
                  {0.0f, 1.05f, 0.0f}};
  Vec2f uvs[3] = {{0, 0}, {1, 0}, {0.5f, 1}};
  uint16_t idx[3] = {0, 1, 2};
  Mesh mesh;
  memset(&mesh, 0, sizeof mesh);
  mesh.indexes_count = 3;
  mesh.positions = pos;
  mesh.pos_indices = idx;
  mesh.textCoord = uvs;
  mesh.tex_indices = idx;
  Object object;
  object_init(&object, &mesh, &material);

  Mat4 model = placement(0.0f, 0.0f, -3.5f, 1.1f);
  Entity node;
  entity_init(&node, (Renderable *)&object, model);

  PointLight hot = {{0.0f, -0.1f, 0.6f}, {12.0f, 2.2f, 1.4f}, 12.0f};
  PointLight hot2 = {{-0.8f, 0.4f, 0.2f}, {2.0f, 10.0f, 3.0f}, 12.0f};
  PointLight pls[2] = {hot, hot2};
  entity_set_point_lights(&node, pls, 2);

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&node);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.02f, 0.02f, 0.02f});
  render_once(&kit);

  DLight dl[2] = {dlight_make(&hot, &model), dlight_make(&hot2, &model)};
  Pixel base = texels[0];
  Ref ref = {pos, model, dl, 2, {0.02, 0.02, 0.02}, base};

  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);
  TASK_CHECK(g_ref_checked > 150);

  /* Find an over-bright red pixel where premature light clamping is wrong. */
  int proved_clamp = 0;
  for (int y = 0; y < TH && !proved_clamp; ++y) {
    for (int x = 0; x < TW; ++x) {
      Info info = ref_info(&ref, x, y);
      if (!info.covered)
        continue;
      Pixel got = kit.mb.frame[x + y * TW];
      if (color_max_delta(got, info.expected) != 0)
        continue;
      if (info.expected.r < 120)
        continue;
      /* Naive: clamp the light channels to [0,1] before multiplying. */
      D3 capped = {MIN(1.0, MAX(0.0, info.light.x)),
                   MIN(1.0, MAX(0.0, info.light.y)),
                   MIN(1.0, MAX(0.0, info.light.z))};
      Pixel premature = ref_shade_pixel(&ref, capped);
      if ((int)info.expected.r - (int)premature.r >= 50) {
        TASK_CHECK((int)got.r - (int)premature.r >= 50);
        TASK_CHECK(got.a == 205);
        proved_clamp = 1;
        break;
      }
    }
  }
  TASK_CHECK(proved_clamp);

  kit_finish(&kit);
  return 0;
}

/* Range cutoff and non-uniform scale moving the light position while range
 * stays an unscaled world distance. */
static int case_f_range_scale(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.4f, -1.0f, 0},
                  {1.4f, -1.0f, 0},
                  {0.0f, 1.1f, 0}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);

  /* Heavy non-uniform scale: a local x offset becomes a large world offset. */
  Mat4 sc = mat4Scale((Vec3f){3.0f, 0.55f, 1.0f});
  Mat4 tr = mat4Translate((Vec3f){0.0f, 0.0f, -3.6f});
  Mat4 model = mat4MultiplyM(&sc, &tr); /* tr * sc */
  Entity node;
  entity_init(&node, (Renderable *)&object, model);

  /* Local x 0.5 -> world x 1.5; local z 0.9 lifts it off the surface so the
   * +z face normal actually catches it. Tiny range reaches one region only. */
  PointLight spot = {{0.5f, 0.0f, 0.9f}, {10.0f, 8.0f, 6.0f}, 1.7f};
  /* Out-of-range-everywhere light (range positive but far smaller than d). */
  PointLight far_off = {{-4.0f, 0.0f, 0.0f}, {10.0f, 10.0f, 10.0f}, 0.4f};
  PointLight pls[2] = {spot, far_off};
  entity_set_point_lights(&node, pls, 2);

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&node);
  renderer_set_point_lighting(&kit.renderer, true,
                              (Vec3f){0.08f, 0.08f, 0.08f});
  render_once(&kit);

  DLight dl[2] = {dlight_make(&spot, &model),
                  dlight_make(&far_off, &model)};
  Ref ref = {pos, model, dl, 2, {0.08, 0.08, 0.08}, white_base()};

  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);
  TASK_CHECK(g_ref_checked > 150);

  /* The world light really was moved by the node scale. */
  TASK_CHECK(fabs(dl[0].pos.x - 1.5) < 1e-4);

  /* Find one pixel lit by the spot and one past its range. */
  int saw_lit = 0, saw_cutoff = 0;
  for (int y = 0; y < TH; ++y) {
    for (int x = 0; x < TW; ++x) {
      Info info = ref_info(&ref, x, y);
      if (!info.covered)
        continue;
      Pixel got = kit.mb.frame[x + y * TW];
      if (color_max_delta(got, info.expected) != 0)
        continue;
      D3 to = {dl[0].pos.x - info.p_persp.x, dl[0].pos.y - info.p_persp.y,
               dl[0].pos.z - info.p_persp.z};
      double d = sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
      double ambient_byte = 0.08 * 255.0;
      if (d < 1.2 && !saw_lit) {
        TASK_CHECK((int)got.r > (int)ambient_byte + 20);
        saw_lit = 1;
      }
      if (d > 1.7 && !saw_cutoff) {
        /* Spot contributes zero here; far_off never does: ambient only. */
        Pixel amb = ref_shade_pixel(
            &ref, (D3){0.08, 0.08, 0.08});
        TASK_CHECK(color_max_delta(got, amb) == 0);
        saw_cutoff = 1;
      }
    }
  }
  TASK_CHECK(saw_lit);
  TASK_CHECK(saw_cutoff);

  kit_finish(&kit);
  return 0;
}

/* ------------------------------------------------------------------ */
/* compat group                                                        */
/* ------------------------------------------------------------------ */

/* Independent legacy face-shading value (camera space normal equals world
 * normal with the identity camera used here). */
static Pixel old_shade(const Ref *ref, int x, int y) {
  Info info = ref_info(ref, x, y);
  if (!info.covered)
    return (Pixel){0};
  Vec3f wa = {(float)info.world[0].x, (float)info.world[0].y,
              (float)info.world[0].z};
  Vec3f wb = {(float)info.world[1].x, (float)info.world[1].y,
              (float)info.world[1].z};
  Vec3f wc = {(float)info.world[2].x, (float)info.world[2].y,
              (float)info.world[2].z};
  Vec3f n = vec3Normalize(vec3Cross(vec3fsubV(wa, wb), vec3fsubV(wa, wc)));
  Vec3f ld = vec3Normalize((Vec3f){-8, 5, 5});
  float d = (1.0f + vec3Dot(n, ld)) * 0.5f;
  d = MIN(1.0f, MAX(d, 0.0f));
  Pixel b = ref->base;
  return pixelFromRGBA((uint8_t)(b.r * d), (uint8_t)(b.g * d),
                       (uint8_t)(b.b * d), b.a);
}

static int scan_old(Kit *kit, const Ref *ref) {
  int bad = 0;
  g_ref_checked = 0;
  for (int y = 0; y < TH; ++y) {
    for (int x = 0; x < TW; ++x) {
      Info info = ref_info(ref, x, y);
      if (!info.covered)
        continue;
      ++g_ref_checked;
      Pixel exp = old_shade(ref, x, y);
      Pixel got = kit->mb.frame[x + y * TW];
      if (color_max_delta(got, exp) > 0) {
        if (bad < 8)
          fprintf(stderr,
                  "  old mismatch (%d,%d) got=(%u,%u,%u) exp=(%u,%u,%u)\n",
                  x, y, got.r, got.g, got.b, exp.r, exp.g, exp.b);
        ++bad;
      }
    }
  }
  return bad;
}

static int case_compat_default_off(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.1f, -0.9f, 0},
                  {1.1f, -0.9f, 0},
                  {0, 1.0f, 0}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);
  Mat4 model = placement(0, 0, -2.6f, 1.0f);
  Entity node;
  entity_init(&node, (Renderable *)&object, model);
  PointLight pl = {{0, 0, 0}, {9, 9, 9}, 20};
  entity_set_point_lights(&node, &pl, 1);

  /* No renderer_set_point_lighting call: initialized mode is off. */
  renderer_set_root_renderable(&kit.renderer, (Renderable *)&node);
  render_once(&kit);

  Ref ref = {pos, model, 0, 0, {0, 0, 0}, white_base()};
  TASK_CHECK(scan_old(&kit, &ref) == 0);
  TASK_CHECK(g_ref_checked > 150);

  static Pixel saved[FRAME_PIXELS];
  memcpy(saved, kit.mb.frame, sizeof saved);

  /* Explicitly switching off (with any ambient) keeps the same image. */
  renderer_set_point_lighting(&kit.renderer, false,
                              (Vec3f){1.0f, 0.0f, 0.5f});
  render_once(&kit);
  TASK_CHECK(memcmp(saved, kit.mb.frame, sizeof saved) == 0);

  /* Removing the borrowed light lists while off changes nothing either. */
  entity_set_point_lights(&node, NULL, 0);
  render_once(&kit);
  TASK_CHECK(memcmp(saved, kit.mb.frame, sizeof saved) == 0);

  kit_finish(&kit);
  return 0;
}

static int case_compat_ambient_only(void) {
  Kit kit;
  TASK_CHECK(kit_start(&kit) == 0);

  Vec3f pos[3] = {{-1.1f, -0.9f, 0.2f},
                  {1.1f, -0.9f, -0.2f},
                  {0, 1.0f, 0}};
  uint16_t idx[3];
  Mesh mesh;
  triangle_mesh(&mesh, pos, idx);
  Object object;
  object_init(&object, &mesh, 0);
  Mat4 model = placement(0, 0, -3.0f, 1.1f);
  Entity node;
  entity_init(&node, (Renderable *)&object, model);
  /* No lights bound at all. */

  renderer_set_root_renderable(&kit.renderer, (Renderable *)&node);
  Vec3f amb = {0.25f, 0.5f, 0.125f};
  renderer_set_point_lighting(&kit.renderer, true, amb);
  render_once(&kit);

  Ref ref = {pos, model, 0, 0, {0.25, 0.5, 0.125}, white_base()};
  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);
  TASK_CHECK(g_ref_checked > 150);

  Vec2i c = ref_center(&ref);
  TASK_CHECK(probe_matches(&kit, &ref, c.x, c.y, 0, "ambient center"));
  Pixel p = kit.mb.frame[c.x + c.y * TW];
  TASK_CHECK(p.r == 63);  /* trunc(255 * 0.25) */
  TASK_CHECK(p.g == 127); /* trunc(255 * 0.5)  */
  TASK_CHECK(p.b == 31);  /* trunc(255 * .125) */

  /* Binding then clearing a list returns to ambient-only. */
  PointLight pl = {{0, 0, 0}, {9, 9, 9}, 20};
  entity_set_point_lights(&node, &pl, 1);
  renderer_render(&kit.renderer);
  entity_set_point_lights(&node, NULL, 0);
  renderer_render(&kit.renderer);
  TASK_CHECK(scan_ref(&kit, &ref, 0) == 0);

  kit_finish(&kit);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"hierarchy", "parent_and_own_lights", case_h_parent},
    {"hierarchy", "siblings_shared_object_reorder", case_h_siblings},
    {"hierarchy", "subtree_append_and_exit", case_h_subtree},
    {"hierarchy", "root_switch_interleave", case_h_switch_interleave},
    {"hierarchy", "invisible_subtree_excluded", case_h_invisible},
    {"fragment", "world_formula_discrimination", case_f_formula},
    {"fragment", "texture_premature_clamp", case_f_texture_clamp},
    {"fragment", "range_and_uniform_scale_position", case_f_range_scale},
    {"compat", "default_off_keeps_legacy_image", case_compat_default_off},
    {"compat", "enabled_without_lights_is_ambient_only",
     case_compat_ambient_only},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
