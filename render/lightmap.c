#include "lightmap.h"

#include "backend.h"
#include "depth.h"
#include "entity.h"
#include "math/fun.h"
#include "material.h"
#include "mesh.h"
#include "object.h"
#include "renderer.h"
#include "state.h"
#include "texture.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The existing renderables expose their concrete type through the render
// function pointer; both symbols are non-static in entity.c/object.c.
extern int entity_render(void *this, Mat4 transform, Renderer *renderer);
extern int object_render(void *this, Mat4 transform, Renderer *renderer);

#define LIGHTMAP_PI 3.14159265358979323846f
// Texel-center edge tests below this distance treat the center as lying on
// the edge itself, where the half-open tie break decides coverage.
#define COVER_EPS 1e-6f

// ---------------------------------------------------------------------------
// Owned snapshot types
// ---------------------------------------------------------------------------

typedef struct {
  Vec3f a, b, c;
  Vec3f normal; // unit world normal, oriented from the transformed winding
  Vec2f ua, ub, uc;
  size_t instance;
  int face; // original face number inside the source mesh
} BakeTri;

typedef struct {
  size_t *path;
  size_t path_len;

  int width, height;
  size_t lm_offset; // offset in the scene-wide radiance arrays

  Vec3f reflectance;
  Vec3f emission;

  bool *covered;
  int *face_owner;

  // Frozen mesh identity, used to re-validate the live tree at render time.
  Mat4 world;
  int indexes_count;
  uint16_t *pos_indices;
  size_t positions_count;
  Vec3f *positions;

  // Own copy of the secondary UV set and its corner indices.
  Vec2f *uv;
  uint16_t *uv_indices;

  // Original face number -> triangle index in the scene, -1 when the face
  // produced no world triangle (degenerate geometry).
  int *face_to_tri;

  BakeInstanceView view;
} BakeInstance;

struct BakeScene {
  BakeInstance *instances;
  size_t instance_count;

  BakeTri *tris;
  size_t tri_count;

  // Two scene-wide radiance buffers so a whole round reads one immutable
  // copy and writes the other; views always point at radiance_cur.
  Vec3f *radiance_cur;
  Vec3f *radiance_next;
  size_t texel_count;

  int rounds;
};

// ---------------------------------------------------------------------------
// Small dynamic arrays (the scene is owned and may allocate freely)
// ---------------------------------------------------------------------------

static int reserve(void **base, size_t *cap, size_t need, size_t elem_size) {
  if (need <= *cap)
    return OK;
  size_t new_cap = *cap ? *cap : 8;
  while (new_cap < need) {
    if (new_cap > SIZE_MAX / 2)
      return INIT_ERROR;
    new_cap *= 2;
  }
  void *p = realloc(*base, new_cap * elem_size);
  if (p == 0)
    return INIT_ERROR;
  *base = p;
  *cap = new_cap;
  return OK;
}

typedef struct {
  size_t *data;
  size_t size, cap;
} PathList;

typedef struct {
  BakeInstance *data;
  size_t size, cap;
} InstanceList;

typedef struct {
  BakeTri *data;
  size_t size, cap;
} TriList;

static int path_push(PathList *p, size_t v) {
  if (reserve((void **)&p->data, &p->cap, p->size + 1, sizeof(size_t)))
    return INIT_ERROR;
  p->data[p->size++] = v;
  return OK;
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

static int finite_vec2(Vec2f v) { return isfinite(v.x) && isfinite(v.y); }

static int valid_uv(Vec2f v) {
  return finite_vec2(v) && v.x >= 0.0f && v.x <= 1.0f && v.y >= 0.0f &&
         v.y <= 1.0f;
}

static int valid_reflectance(Vec3f r) {
  return isfinite(r.x) && isfinite(r.y) && isfinite(r.z) && r.x >= 0.0f &&
         r.y >= 0.0f && r.z >= 0.0f && r.x < 1.0f && r.y < 1.0f &&
         r.z < 1.0f;
}

static int valid_emission(Vec3f e) {
  return isfinite(e.x) && isfinite(e.y) && isfinite(e.z) && e.x >= 0.0f &&
         e.y >= 0.0f && e.z >= 0.0f;
}

static int path_equal(const size_t *a, size_t na, const size_t *b,
                      size_t nb) {
  if (na != nb)
    return 0;
  for (size_t i = 0; i < na; i++)
    if (a[i] != b[i])
      return 0;
  return 1;
}

static int path_less(const size_t *a, size_t na, const size_t *b,
                     size_t nb) {
  size_t n = na < nb ? na : nb;
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return a[i] < b[i];
  }
  return na < nb;
}

static const BakeBinding *find_binding(const BakeBinding *bindings,
                                       size_t binding_count, bool *used,
                                       const size_t *path, size_t path_len) {
  for (size_t i = 0; i < binding_count; i++) {
    if (path_equal(bindings[i].path, bindings[i].path_len, path, path_len)) {
      used[i] = true;
      return &bindings[i];
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

static Vec3f vec_sub(Vec3f a, Vec3f b) {
  return (Vec3f){a.x - b.x, a.y - b.y, a.z - b.z};
}
static Vec3f vec_add(Vec3f a, Vec3f b) {
  return (Vec3f){a.x + b.x, a.y + b.y, a.z + b.z};
}
static Vec3f vec_scale(Vec3f a, float s) {
  return (Vec3f){a.x * s, a.y * s, a.z * s};
}
static float vec_dot(Vec3f a, Vec3f b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec3f vec_cross(Vec3f a, Vec3f b) {
  return (Vec3f){a.y * b.z - b.y * a.z, a.z * b.x - b.z * a.x,
                 a.x * b.y - b.x * a.y};
}
static float vec_len(Vec3f v) { return sqrtf(vec_dot(v, v)); }

static Vec3f transform_point(Mat4 *m, Vec3f v) {
  Vec4f p = {v.x, v.y, v.z, 1.0f};
  p = mat4MultiplyVec4(&p, m);
  return (Vec3f){p.x, p.y, p.z};
}

// Standard orient2d(a,b,p): positive when p is left of the directed edge
// a->b, i.e. inside a counter-clockwise triangle under the top/left rule.
static float edge2(Vec2f a, Vec2f b, Vec2f p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Half-open top/left rule: for an edge shared with a neighbour, exactly one
// of the two opposite edge directions qualifies, so the shared texel centers
// are covered once.
static int is_top_left(Vec2f a, Vec2f b) {
  return (a.y == b.y && a.x < b.x) || (a.y > b.y);
}

static int edge_inside(float w, Vec2f a, Vec2f b) {
  if (w > COVER_EPS)
    return 1;
  if (w < -COVER_EPS)
    return 0;
  return is_top_left(a, b);
}

// Barycentric weights of p in the (non-degenerate) UV triangle (a,b,c).
static void bary2(Vec2f a, Vec2f b, Vec2f c, Vec2f p, float *wa, float *wb,
                  float *wc) {
  float denom = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  float la = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / denom;
  float lb = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / denom;
  *wa = la;
  *wb = lb;
  *wc = 1.0f - la - lb;
}

static int nearest_texel(float u, int n) {
  int i = (int)floorf(u * (float)n);
  if (i < 0)
    i = 0;
  if (i > n - 1)
    i = n - 1;
  return i;
}

// Rasterizes one face of an instance into its lightmap. Two positive-area
// faces of the same instance covering one texel is a hard failure.
static int cover_face(BakeInstance *inst, int face, Vec2f ua, Vec2f ub,
                      Vec2f uc) {
  float area2_uv = (ub.x - ua.x) * (uc.y - ua.y) -
                   (ub.y - ua.y) * (uc.x - ua.x);
  if (fabsf(area2_uv) <= COVER_EPS)
    return OK; // zero-area UV face: never covers a texel

  // Re-orient to counter-clockwise in UV space.
  Vec2f p0 = ua, p1 = ub, p2 = uc;
  if (area2_uv < 0.0f) {
    p1 = uc;
    p2 = ub;
  }

  float minu = fminf(p0.x, fminf(p1.x, p2.x));
  float maxu = fmaxf(p0.x, fmaxf(p1.x, p2.x));
  float minv = fminf(p0.y, fminf(p1.y, p2.y));
  float maxv = fmaxf(p0.y, fmaxf(p1.y, p2.y));

  int x0 = nearest_texel(minu, inst->width);
  int x1 = nearest_texel(maxu, inst->width);
  int y0 = nearest_texel(minv, inst->height);
  int y1 = nearest_texel(maxv, inst->height);

  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      Vec2f center = {(x + 0.5f) / (float)inst->width,
                      (y + 0.5f) / (float)inst->height};
      float w0 = edge2(p1, p2, center);
      float w1 = edge2(p2, p0, center);
      float w2 = edge2(p0, p1, center);
      if (!edge_inside(w0, p1, p2) || !edge_inside(w1, p2, p0) ||
          !edge_inside(w2, p0, p1))
        continue;

      size_t idx = (size_t)y * (size_t)inst->width + (size_t)x;
      if (inst->covered[idx] && inst->face_owner[idx] != face)
        return INIT_ERROR; // positive-area coverage conflict
      inst->covered[idx] = true;
      inst->face_owner[idx] = face;
    }
  }
  return OK;
}

// ---------------------------------------------------------------------------
// Tree walk: freeze every visible Object into an owned instance
// ---------------------------------------------------------------------------

typedef struct {
  const BakeBinding *bindings;
  size_t binding_count;
  bool *used;
  InstanceList *insts;
  TriList *tris;
} WalkCtx;

static void free_instance_contents(BakeInstance *inst) {
  free(inst->path);
  free(inst->covered);
  free(inst->face_owner);
  free(inst->pos_indices);
  free(inst->positions);
  free(inst->uv);
  free(inst->uv_indices);
  free(inst->face_to_tri);
  memset(inst, 0, sizeof(*inst));
}

static int walk(Renderable *r, Mat4 *parent, PathList *path, WalkCtx *cx) {
  if (r == 0)
    return INIT_ERROR;

  if (r->render == entity_render) {
    Entity *e = (Entity *)r;
    if (!e->visible)
      return OK; // invisible subtree is ignored wholesale

    Mat4 world = mat4MultiplyM(&e->transform, parent);

    for (size_t i = 0; i < e->children_entities.size; i++) {
      if (path_push(path, i))
        return INIT_ERROR;
      Entity *child = &((Entity *)e->children_entities.data)[i];
      if (walk(&child->renderable, &world, path, cx))
        return INIT_ERROR;
      path->size--;
    }
    return walk(e->entity_renderable, &world, path, cx);
  }

  if (r->render != object_render)
    return INIT_ERROR; // only Entity and Object are supported

  Object *o = (Object *)r;
  const BakeBinding *b =
      find_binding(cx->bindings, cx->binding_count, cx->used, path->data,
                   path->size);
  if (b == 0)
    return INIT_ERROR; // visible Object without a binding

  if (b->width < 1 || b->height < 1 || b->uv == 0 || b->uv_count == 0 ||
      b->uv_indices == 0 || (b->path_len != 0 && b->path == 0))
    return INIT_ERROR;
  if (!valid_reflectance(b->reflectance) || !valid_emission(b->emission))
    return INIT_ERROR;
  for (size_t i = 0; i < b->uv_count; i++)
    if (!valid_uv(b->uv[i]))
      return INIT_ERROR;

  Mesh *mesh = o->mesh;
  if (mesh == 0 || mesh->indexes_count < 0 ||
      (mesh->indexes_count % 3) != 0)
    return INIT_ERROR;
  // A face-bearing mesh must provide its position table and index list; an
  // empty mesh legitimately carries neither.
  if (mesh->indexes_count > 0 &&
      (mesh->pos_indices == 0 || mesh->positions == 0 ||
       b->uv_indices == 0))
    return INIT_ERROR;
  for (int i = 0; i < mesh->indexes_count; i++)
    if (b->uv_indices[i] >= b->uv_count)
      return INIT_ERROR;

  Mat4 world = *parent;

  if (reserve((void **)&cx->insts->data, &cx->insts->cap,
              cx->insts->size + 1, sizeof(BakeInstance)))
    return INIT_ERROR;
  BakeInstance *inst = &cx->insts->data[cx->insts->size++];
  memset(inst, 0, sizeof(*inst));

  inst->path = malloc(path->size ? path->size * sizeof(size_t) : 1);
  if (inst->path == 0)
    return INIT_ERROR;
  memcpy(inst->path, path->data, path->size * sizeof(size_t));
  inst->path_len = path->size;
  inst->width = b->width;
  inst->height = b->height;
  inst->reflectance = b->reflectance;
  inst->emission = b->emission;
  inst->world = world;
  inst->indexes_count = mesh->indexes_count;

  size_t faces = (size_t)mesh->indexes_count / 3;
  size_t grid = (size_t)inst->width * (size_t)inst->height;
  inst->covered = calloc(grid, sizeof(bool));
  inst->face_owner = malloc(grid * sizeof(int));
  inst->face_to_tri = malloc(faces ? faces * sizeof(int) : 1);
  if (mesh->indexes_count > 0) {
    inst->pos_indices =
        malloc((size_t)mesh->indexes_count * sizeof(uint16_t));
    inst->uv_indices =
        malloc((size_t)mesh->indexes_count * sizeof(uint16_t));
  }
  if (b->uv_count > 0)
    inst->uv = malloc(b->uv_count * sizeof(Vec2f));
  if (inst->covered == 0 || inst->face_owner == 0 ||
      inst->face_to_tri == 0 ||
      (mesh->indexes_count > 0 &&
       (inst->pos_indices == 0 || inst->uv_indices == 0)) ||
      (b->uv_count > 0 && inst->uv == 0))
    return INIT_ERROR;
  for (size_t i = 0; i < grid; i++)
    inst->face_owner[i] = -1;
  for (size_t i = 0; i < faces; i++)
    inst->face_to_tri[i] = -1;
  if (mesh->indexes_count > 0) {
    memcpy(inst->pos_indices, mesh->pos_indices,
           (size_t)mesh->indexes_count * sizeof(uint16_t));
    memcpy(inst->uv_indices, b->uv_indices,
           (size_t)mesh->indexes_count * sizeof(uint16_t));
  }
  if (b->uv_count > 0)
    memcpy(inst->uv, b->uv, b->uv_count * sizeof(Vec2f));

  // Compact own copy of the referenced position table. A mesh with no faces
  // carries no referenced positions.
  uint16_t max_pos = 0;
  for (int i = 0; i < mesh->indexes_count; i++)
    if (mesh->pos_indices[i] > max_pos)
      max_pos = mesh->pos_indices[i];
  inst->positions_count =
      mesh->indexes_count == 0 ? 0 : (size_t)max_pos + 1;
  if (inst->positions_count != 0) {
    inst->positions = malloc(inst->positions_count * sizeof(Vec3f));
    if (inst->positions == 0)
      return INIT_ERROR;
    memcpy(inst->positions, mesh->positions,
           inst->positions_count * sizeof(Vec3f));
  }

  for (int face = 0; face < (int)faces; face++) {
    int i = face * 3;
    uint16_t p0 = mesh->pos_indices[i + 0];
    uint16_t p1 = mesh->pos_indices[i + 1];
    uint16_t p2 = mesh->pos_indices[i + 2];

    BakeTri tri;
    tri.a = transform_point(&world, mesh->positions[p0]);
    tri.b = transform_point(&world, mesh->positions[p1]);
    tri.c = transform_point(&world, mesh->positions[p2]);

    Vec3f cross = vec_cross(vec_sub(tri.b, tri.a), vec_sub(tri.c, tri.a));
    float len = vec_len(cross);
    if (len == 0.0f)
      continue; // degenerate world face: no surface, no ray can hit it
    tri.normal = vec_scale(cross, 1.0f / len);

    tri.ua = b->uv[b->uv_indices[i + 0]];
    tri.ub = b->uv[b->uv_indices[i + 1]];
    tri.uc = b->uv[b->uv_indices[i + 2]];
    tri.instance = cx->insts->size - 1;
    tri.face = face;

    if (reserve((void **)&cx->tris->data, &cx->tris->cap,
                cx->tris->size + 1, sizeof(BakeTri)))
      return INIT_ERROR;
    inst->face_to_tri[face] = (int)cx->tris->size;
    cx->tris->data[cx->tris->size++] = tri;

    if (cover_face(inst, face, tri.ua, tri.ub, tri.uc))
      return INIT_ERROR;
  }
  inst->view.path = inst->path;
  inst->view.path_len = inst->path_len;
  inst->view.width = inst->width;
  inst->view.height = inst->height;
  inst->view.covered = inst->covered;
  inst->view.face_owner = inst->face_owner;
  return OK;
}

// ---------------------------------------------------------------------------
// Build / free
// ---------------------------------------------------------------------------

int bake_scene_build(BakeScene **out, Entity *root,
                     const BakeBinding *bindings, size_t binding_count) {
  if (out == 0 || root == 0 || (binding_count != 0 && bindings == 0))
    return INIT_ERROR;

  bool *used = calloc(binding_count ? binding_count : 1, sizeof(bool));
  InstanceList insts = {0, 0, 0};
  TriList tris = {0, 0, 0};
  PathList path = {0, 0, 0};
  if (used == 0)
    return INIT_ERROR;

  // Reject duplicate binding paths up front. The empty path addresses the
  // root Object itself and may carry a NULL pointer.
  for (size_t i = 0; i < binding_count; i++) {
    if ((bindings[i].path_len != 0 && bindings[i].path == 0) ||
        bindings[i].width < 1 || bindings[i].height < 1) {
      free(used);
      return INIT_ERROR;
    }
    for (size_t j = 0; j < i; j++)
      if (path_equal(bindings[i].path, bindings[i].path_len, bindings[j].path,
                     bindings[j].path_len)) {
        free(used);
        return INIT_ERROR;
      }
  }

  WalkCtx cx = {bindings, binding_count, used, &insts, &tris};
  Mat4 identity = mat4Identity();
  int error = walk(&root->renderable, &identity, &path, &cx);

  if (error == 0) {
    for (size_t i = 0; i < binding_count; i++)
      if (!used[i])
        error = INIT_ERROR; // binding for an Object the tree does not contain
  }

  free(used);
  free(path.data);

  BakeScene *scene = 0;
  if (error == 0) {
    scene = calloc(1, sizeof(BakeScene));
    if (scene == 0)
      error = INIT_ERROR;
  }
  if (error == 0) {
    scene->instances = insts.data;
    scene->instance_count = insts.size;
    scene->tris = tris.data;
    scene->tri_count = tris.size;
    scene->rounds = 0;

    for (size_t i = 0; i < scene->instance_count; i++)
      scene->texel_count +=
          (size_t)scene->instances[i].width *
          (size_t)scene->instances[i].height;

    scene->radiance_cur = calloc(scene->texel_count ? scene->texel_count : 1,
                                 sizeof(Vec3f));
    scene->radiance_next = malloc(
        (scene->texel_count ? scene->texel_count : 1) * sizeof(Vec3f));
    if (scene->radiance_cur == 0 || scene->radiance_next == 0)
      error = INIT_ERROR;
  }

  if (error) {
    for (size_t i = 0; i < insts.size; i++)
      free_instance_contents(&insts.data[i]);
    free(insts.data);
    free(tris.data);
    if (scene != 0) {
      free(scene->radiance_cur);
      free(scene->radiance_next);
      free(scene);
    }
    return INIT_ERROR; // *out is deliberately left untouched
  }

  // Assign scene-wide texel offsets in instance order.
  size_t offset = 0;
  for (size_t i = 0; i < scene->instance_count; i++) {
    BakeInstance *inst = &scene->instances[i];
    inst->lm_offset = offset;
    inst->view.radiance = scene->radiance_cur + offset;
    offset += (size_t)inst->width * (size_t)inst->height;
  }

  // Initial L: emission on covered texels, zero elsewhere.
  for (size_t i = 0; i < scene->instance_count; i++) {
    BakeInstance *inst = &scene->instances[i];
    size_t grid = (size_t)inst->width * (size_t)inst->height;
    for (size_t t = 0; t < grid; t++)
      if (inst->covered[t])
        scene->radiance_cur[inst->lm_offset + t] = inst->emission;
  }

  *out = scene;
  return OK;
}

void bake_scene_free(BakeScene *scene) {
  if (scene == 0)
    return;
  for (size_t i = 0; i < scene->instance_count; i++)
    free_instance_contents(&scene->instances[i]);
  free(scene->instances);
  free(scene->tris);
  free(scene->radiance_cur);
  free(scene->radiance_next);
  free(scene);
}

// ---------------------------------------------------------------------------
// Fixed-sample transport
// ---------------------------------------------------------------------------

static float radical_inverse_base2(uint32_t n) {
  float result = 0.0f;
  float half = 0.5f;
  while (n != 0) {
    result += (float)(n & 1u) * half;
    n >>= 1;
    half *= 0.5f;
  }
  return result;
}

typedef struct {
  int hit;
  int front;
  size_t tri_index;
  Vec2f uv;
} TraceHit;

// Nearest t > epsilon over every world triangle. Geometric occlusion is
// two-sided; equidistant hits break by entity path then original face.
static void trace_ray(const BakeScene *scene, Vec3f origin, Vec3f dir,
                      float epsilon, TraceHit *hit) {
  hit->hit = 0;
  float best_t = 0.0f;

  for (size_t i = 0; i < scene->tri_count; i++) {
    const BakeTri *tri = &scene->tris[i];
    Vec3f e1 = vec_sub(tri->b, tri->a);
    Vec3f e2 = vec_sub(tri->c, tri->a);
    Vec3f h = vec_cross(dir, e2);
    float det = vec_dot(e1, h);
    // Only the degenerate (parallel/coplanar) case is dropped; a vanishingly
    // small det yields NaN barycentrics whose t fails the t > epsilon gate.
    if (det == 0.0f)
      continue;

    Vec3f s = vec_sub(origin, tri->a);
    float u = vec_dot(s, h) / det;
    if (u < 0.0f || u > 1.0f)
      continue;
    Vec3f q = vec_cross(s, e1);
    float v = vec_dot(dir, q) / det;
    if (v < 0.0f || u + v > 1.0f)
      continue;
    float t = vec_dot(e2, q) / det;
    if (!(t > epsilon))
      continue;

    int closer = !hit->hit || t < best_t;
    int equidistant = hit->hit && t == best_t;
    if (equidistant) {
      const BakeTri *winner = &scene->tris[hit->tri_index];
      const BakeInstance *win_inst = &scene->instances[winner->instance];
      const BakeInstance *cand_inst = &scene->instances[tri->instance];
      if (path_less(cand_inst->path, cand_inst->path_len, win_inst->path,
                    win_inst->path_len) ||
          (path_equal(cand_inst->path, cand_inst->path_len, win_inst->path,
                      win_inst->path_len) &&
           tri->face < winner->face))
        closer = 1;
    }
    if (!closer)
      continue;

    best_t = t;
    hit->hit = 1;
    hit->tri_index = i;
    hit->front = vec_dot(dir, tri->normal) < 0.0f;
    float w = 1.0f - u - v;
    hit->uv = (Vec2f){w * tri->ua.x + u * tri->ub.x + v * tri->uc.x,
                      w * tri->ua.y + u * tri->ub.y + v * tri->uc.y};
  }
}

// Reconstructs the covered surface point and its unit world normal from the
// lightmap texel center in secondary UV space.
static void surface_point(const BakeInstance *inst, const BakeTri *tri,
                          int x, int y, Vec3f *p, Vec3f *n) {
  Vec2f center = {(x + 0.5f) / (float)inst->width,
                  (y + 0.5f) / (float)inst->height};
  float wa, wb, wc;
  bary2(tri->ua, tri->ub, tri->uc, center, &wa, &wb, &wc);
  Vec3f pos = vec_add(vec_add(vec_scale(tri->a, wa), vec_scale(tri->b, wb)),
                      vec_scale(tri->c, wc));
  *p = pos;
  *n = tri->normal;
}

static Vec3f sample_hemisphere(Vec3f n, int k, int S) {
  float u = (k + 0.5f) / (float)S;
  float v = radical_inverse_base2((uint32_t)k);
  float r = sqrtf(u);
  float phi = 2.0f * LIGHTMAP_PI * v;
  Vec3f local = {r * cosf(phi), r * sinf(phi), sqrtf(1.0f - u)};

  Vec3f axis = (fabsf(n.y) < 0.9f) ? (Vec3f){0.0f, 1.0f, 0.0f}
                                   : (Vec3f){1.0f, 0.0f, 0.0f};
  Vec3f tangent = vec_cross(axis, n);
  tangent = vec_scale(tangent, 1.0f / vec_len(tangent));
  Vec3f bitangent = vec_cross(n, tangent);

  return (Vec3f){
      tangent.x * local.x + bitangent.x * local.y + n.x * local.z,
      tangent.y * local.x + bitangent.y * local.y + n.y * local.z,
      tangent.z * local.x + bitangent.z * local.y + n.z * local.z};
}

static int valid_environment(Vec3f e) {
  return isfinite(e.x) && isfinite(e.y) && isfinite(e.z) && e.x >= 0.0f &&
         e.y >= 0.0f && e.z >= 0.0f;
}

int bake_step(BakeScene *scene, int rounds, int S, float epsilon,
              Vec3f environment) {
  if (scene == 0 || rounds < 1 || S < 1 || !(epsilon > 0.0f) ||
      !isfinite(epsilon) || !valid_environment(environment))
    return INIT_ERROR;

  for (int round = 0; round < rounds; round++) {
    memset(scene->radiance_next, 0,
           scene->texel_count * sizeof(Vec3f));

    for (size_t ii = 0; ii < scene->instance_count; ii++) {
      BakeInstance *inst = &scene->instances[ii];
      size_t grid = (size_t)inst->width * (size_t)inst->height;

      for (size_t tex = 0; tex < grid; tex++) {
        if (!inst->covered[tex])
          continue;
        int face = inst->face_owner[tex];
        int tri_idx = inst->face_to_tri[face];
        if (tri_idx < 0)
          continue;
        const BakeTri *tri = &scene->tris[tri_idx];
        int x = (int)(tex % (size_t)inst->width);
        int y = (int)(tex / (size_t)inst->width);

        Vec3f p, n;
        surface_point(inst, tri, x, y, &p, &n);
        Vec3f origin = vec_add(p, vec_scale(n, epsilon));

        Vec3f sum = {0.0f, 0.0f, 0.0f};
        for (int k = 0; k < S; k++) {
          Vec3f dir = sample_hemisphere(n, k, S);
          TraceHit hit;
          trace_ray(scene, origin, dir, epsilon, &hit);

          Vec3f l_hit;
          if (!hit.hit) {
            l_hit = environment;
          } else if (!hit.front) {
            l_hit = (Vec3f){0.0f, 0.0f, 0.0f}; // back face: absorbed
          } else {
            const BakeTri *hit_tri = &scene->tris[hit.tri_index];
            const BakeInstance *target =
                &scene->instances[hit_tri->instance];
            int tx = nearest_texel(hit.uv.x, target->width);
            int ty = nearest_texel(hit.uv.y, target->height);
            size_t idx = (size_t)ty * (size_t)target->width + (size_t)tx;
            if (target->covered[idx])
              l_hit = scene->radiance_cur[target->lm_offset + idx];
            else
              l_hit = (Vec3f){0.0f, 0.0f, 0.0f};
          }
          sum = vec_add(sum, l_hit);
        }

        float inv_s = 1.0f / (float)S;
        Vec3f bounced = vec_scale(sum, inv_s);
        Vec3f value = {
            inst->emission.x + inst->reflectance.x * bounced.x,
            inst->emission.y + inst->reflectance.y * bounced.y,
            inst->emission.z + inst->reflectance.z * bounced.z};
        scene->radiance_next[inst->lm_offset + tex] = value;
      }
    }

    Vec3f *tmp = scene->radiance_cur;
    scene->radiance_cur = scene->radiance_next;
    scene->radiance_next = tmp;
    for (size_t ii = 0; ii < scene->instance_count; ii++) {
      BakeInstance *inst = &scene->instances[ii];
      inst->view.radiance = scene->radiance_cur + inst->lm_offset;
    }
    scene->rounds++;
  }
  return OK;
}

int bake_reset(BakeScene *scene) {
  if (scene == 0)
    return INIT_ERROR;
  for (size_t ii = 0; ii < scene->instance_count; ii++) {
    BakeInstance *inst = &scene->instances[ii];
    size_t grid = (size_t)inst->width * (size_t)inst->height;
    for (size_t t = 0; t < grid; t++) {
      scene->radiance_cur[inst->lm_offset + t] =
          inst->covered[t] ? inst->emission : (Vec3f){0.0f, 0.0f, 0.0f};
    }
  }
  scene->rounds = 0;
  return OK;
}

int bake_rounds(const BakeScene *scene) {
  return scene == 0 ? -1 : scene->rounds;
}

size_t bake_instance_count(const BakeScene *scene) {
  return scene == 0 ? 0 : scene->instance_count;
}

const BakeInstanceView *bake_instance_view(const BakeScene *scene,
                                           size_t index) {
  if (scene == 0 || index >= scene->instance_count)
    return 0;
  return &scene->instances[index].view;
}

// ---------------------------------------------------------------------------
// Lightmapped rendering on private buffers
// ---------------------------------------------------------------------------

typedef struct {
  Renderer *renderer;
  const BakeScene *scene;
  float exposure;
  Pixel *color;
  PingoDepth *depth;
} LightRenderCtx;

static const BakeInstance *find_instance(const BakeScene *scene,
                                         const size_t *path,
                                         size_t path_len) {
  for (size_t i = 0; i < scene->instance_count; i++) {
    const BakeInstance *inst = &scene->instances[i];
    if (path_equal(inst->path, inst->path_len, path, path_len))
      return inst;
  }
  return 0;
}

static uint8_t clamp_byte(float v) {
  if (v < 0.0f)
    v = 0.0f;
  if (v > 1.0f)
    v = 1.0f;
  return (uint8_t)(v * 255.0f);
}

// Mirrors object_render's transform, coverage and depth path, but shades with
// baked radiance looked up through the perspective-interpolated secondary UV.
static int rasterize_lightmapped(Object *o, Mat4 *m,
                                 const BakeInstance *inst,
                                 LightRenderCtx *cx) {
  Renderer *r = cx->renderer;
  const Vec2i scr_size = r->framebuffer.size;

  Mat4 view = mat4Inverse(&r->camera_view);
  Mat4 proj = r->camera_projection;

  for (int i = 0; i < o->mesh->indexes_count; i += 3) {
    const Vec3f *ver1 = &o->mesh->positions[o->mesh->pos_indices[i + 0]];
    const Vec3f *ver2 = &o->mesh->positions[o->mesh->pos_indices[i + 1]];
    const Vec3f *ver3 = &o->mesh->positions[o->mesh->pos_indices[i + 2]];

    Vec2f lca = inst->uv[inst->uv_indices[i + 0]];
    Vec2f lcb = inst->uv[inst->uv_indices[i + 1]];
    Vec2f lcc = inst->uv[inst->uv_indices[i + 2]];

    Vec2f tca = {0, 0}, tcb = {0, 0}, tcc = {0, 0};
    if (o->material != 0 && o->material->texture != 0 &&
        o->mesh->textCoord != 0 && o->mesh->tex_indices != 0) {
      tca = o->mesh->textCoord[o->mesh->tex_indices[i + 0]];
      tcb = o->mesh->textCoord[o->mesh->tex_indices[i + 1]];
      tcc = o->mesh->textCoord[o->mesh->tex_indices[i + 2]];
    }

    Vec4f a = {ver1->x, ver1->y, ver1->z, 1};
    Vec4f b = {ver2->x, ver2->y, ver2->z, 1};
    Vec4f c = {ver3->x, ver3->y, ver3->z, 1};

    Mat4 vm = mat4MultiplyM(&view, m);
    a = mat4MultiplyVec4(&a, &vm);
    b = mat4MultiplyVec4(&b, &vm);
    c = mat4MultiplyVec4(&c, &vm);

    a = mat4MultiplyVec4(&a, &proj);
    b = mat4MultiplyVec4(&b, &proj);
    c = mat4MultiplyVec4(&c, &proj);

    if (r->enable_frustum_culling) {
      Vec4f *vs[3] = {&a, &b, &c};
      int outside = 0;
      for (int plane = 0; plane < 6; plane++) {
        int all = 1;
        for (int q = 0; q < 3; q++) {
          Vec4f vv = *vs[q];
          int out_q = 0;
          switch (plane) {
          case 0:
            out_q = vv.z > vv.w;
            break; // near
          case 1:
            out_q = vv.z < -vv.w;
            break; // far
          case 2:
            out_q = vv.x < -vv.w;
            break; // left
          case 3:
            out_q = vv.x > vv.w;
            break; // right
          case 4:
            out_q = vv.y > vv.w;
            break; // top
          default:
            out_q = vv.y < -vv.w;
            break; // bottom
          }
          if (!out_q)
            all = 0;
        }
        if (all)
          outside = 1;
      }
      if (outside)
        continue;
    }

    if (a.z > 0 && b.z > 0 && c.z > 0)
      continue;
    // A vanishing clip-space w cannot be perspective-interpolated; the
    // original draw path skips the same configuration.
    if (a.w == 0.0f || b.w == 0.0f || c.w == 0.0f)
      continue;

    const float aw = a.w, bw = b.w, cw = c.w;
    a.x /= a.w;
    a.y /= a.w;
    a.z /= a.w;
    a.w = 1;
    b.x /= b.w;
    b.y /= b.w;
    b.z /= b.w;
    b.w = 1;
    c.x /= c.w;
    c.y /= c.w;
    c.z /= c.w;
    c.w = 1;

    if (r->enable_backface_culling) {
      if (isClockWise(a.x, a.y, b.x, b.y, c.x, c.y) >= 0)
        continue;
    }

    float half_x = scr_size.x * 0.5f;
    float half_y = scr_size.y * 0.5f;
    Vec2i a_s = {(int)(a.x * half_x + half_x),
                 (int)(a.y * half_y + half_y)};
    Vec2i b_s = {(int)(b.x * half_x + half_x),
                 (int)(b.y * half_y + half_y)};
    Vec2i c_s = {(int)(c.x * half_x + half_x),
                 (int)(c.y * half_y + half_y)};

    int32_t min_x = MIN(MIN(a_s.x, b_s.x), c_s.x);
    int32_t min_y = MIN(MIN(a_s.y, b_s.y), c_s.y);
    int32_t max_x = MAX(MAX(a_s.x, b_s.x), c_s.x);
    int32_t max_y = MAX(MAX(a_s.y, b_s.y), c_s.y);
    min_x = MIN(MAX(min_x, 0), scr_size.x);
    min_y = MIN(MAX(min_y, 0), scr_size.y);
    max_x = MIN(MAX(max_x, 0), scr_size.x);
    max_y = MIN(MAX(max_y, 0), scr_size.y);

    Vec2i min_tri = {min_x, min_y};
    int32_t area = orient2d(a_s, b_s, c_s);
    if (area == 0)
      continue;
    float area_inverse = 1.0f / area;

    int32_t A01 = a_s.y - b_s.y;
    int32_t B01 = b_s.x - a_s.x;
    int32_t A12 = b_s.y - c_s.y;
    int32_t B12 = c_s.x - b_s.x;
    int32_t A20 = c_s.y - a_s.y;
    int32_t B20 = a_s.x - c_s.x;

    int32_t w0_row = orient2d(b_s, c_s, min_tri);
    int32_t w1_row = orient2d(c_s, a_s, min_tri);
    int32_t w2_row = orient2d(a_s, b_s, min_tri);

    // Perspective-correct interpolation of both UV sets, exactly as the
    // original path interpolates its texture coordinates.
    float inv_aw = 1.0f / aw, inv_bw = 1.0f / bw, inv_cw = 1.0f / cw;
    tca.x *= inv_aw;
    tca.y *= inv_aw;
    tcb.x *= inv_bw;
    tcb.y *= inv_bw;
    tcc.x *= inv_cw;
    tcc.y *= inv_cw;
    Vec2f la = {lca.x * inv_aw, lca.y * inv_aw};
    Vec2f lb = {lcb.x * inv_bw, lcb.y * inv_bw};
    Vec2f lc = {lcc.x * inv_cw, lcc.y * inv_cw};

    for (int16_t y = min_y; y < max_y;
         y++, w0_row += B12, w1_row += B20, w2_row += B01) {
      int32_t w0 = w0_row, w1 = w1_row, w2 = w2_row;
      for (int32_t x = min_x; x < max_x;
           x++, w0 += A12, w1 += A20, w2 += A01) {
        if ((w0 | w1 | w2) < 0)
          continue;

        float depth = -(w0 * a.z + w1 * b.z + w2 * c.z) * area_inverse;
        if (depth < -1.0f || depth > 1.0f)
          continue;

        int pixel_index = x + y * scr_size.x;
        if (depth_check(cx->depth, pixel_index, depth))
          continue;
        depth_write(cx->depth, pixel_index, depth);

        float interp_inv_w =
            w0 * inv_aw + w1 * inv_bw + w2 * inv_cw;
        if (interp_inv_w == 0.0f)
          continue;

        float lm_u = (w0 * la.x + w1 * lb.x + w2 * lc.x) / interp_inv_w;
        float lm_v = (w0 * la.y + w1 * lb.y + w2 * lc.y) / interp_inv_w;
        int tx = nearest_texel(lm_u, inst->width);
        int ty = nearest_texel(lm_v, inst->height);
        size_t lm_idx = (size_t)ty * (size_t)inst->width + (size_t)tx;

        Vec3f radiance = {0.0f, 0.0f, 0.0f};
        if (inst->covered[lm_idx])
          radiance =
              cx->scene->radiance_cur[inst->lm_offset + lm_idx];

        Pixel base = pixelFromRGBA(255, 255, 255, 255);
        if (o->material != 0 && o->material->texture != 0) {
          float tex_u =
              (w0 * tca.x + w1 * tcb.x + w2 * tcc.x) / interp_inv_w;
          float tex_v =
              (w0 * tca.y + w1 * tcb.y + w2 * tcc.y) / interp_inv_w;
          base = texture_readF(o->material->texture,
                               (Vec2f){tex_u, tex_v});
        }

        float rr = (base.r / 255.0f) * radiance.x * cx->exposure;
        float gg = (base.g / 255.0f) * radiance.y * cx->exposure;
        float bb = (base.b / 255.0f) * radiance.z * cx->exposure;
        cx->color[pixel_index] =
            pixelFromRGBA(clamp_byte(rr), clamp_byte(gg), clamp_byte(bb),
                          base.a);
      }
    }
  }
  return OK;
}

static int light_render_walk(Renderable *r, Mat4 *parent, PathList *path,
                             LightRenderCtx *cx) {
  if (r == 0)
    return RENDER_ERROR;

  if (r->render == entity_render) {
    Entity *e = (Entity *)r;
    if (!e->visible)
      return OK;
    Mat4 world = mat4MultiplyM(&e->transform, parent);
    for (size_t i = 0; i < e->children_entities.size; i++) {
      if (path_push(path, i))
        return RENDER_ERROR;
      Entity *child = &((Entity *)e->children_entities.data)[i];
      if (light_render_walk(&child->renderable, &world, path, cx))
        return RENDER_ERROR;
      path->size--;
    }
    return light_render_walk(e->entity_renderable, &world, path, cx);
  }

  if (r->render != object_render)
    return RENDER_ERROR;

  const BakeInstance *inst =
      find_instance(cx->scene, path->data, path->size);
  if (inst == 0)
    return RENDER_ERROR;
  return rasterize_lightmapped((Object *)r, parent, inst, cx);
}

// ---------------------------------------------------------------------------
// Live-tree validation against the snapshot
// ---------------------------------------------------------------------------

static int instance_matches(const BakeInstance *inst, const Object *o,
                            const Mat4 *world) {
  if (memcmp(&inst->world, world, sizeof(Mat4)) != 0)
    return 0;
  const Mesh *mesh = o->mesh;
  if (mesh == 0 || mesh->indexes_count != inst->indexes_count)
    return 0;
  if (mesh->indexes_count > 0 &&
      (mesh->pos_indices == 0 || mesh->positions == 0))
    return 0;
  for (int i = 0; i < mesh->indexes_count; i++)
    if (mesh->pos_indices[i] != inst->pos_indices[i])
      return 0;
  for (size_t i = 0; i < inst->positions_count; i++) {
    Vec3f a = mesh->positions[i];
    Vec3f b = inst->positions[i];
    if (memcmp(&a, &b, sizeof(Vec3f)) != 0)
      return 0;
  }
  return 1;
}

static int validate_walk(Renderable *r, Mat4 *parent, PathList *path,
                         const BakeScene *scene, size_t *matched) {
  if (r == 0)
    return RENDER_ERROR;

  if (r->render == entity_render) {
    Entity *e = (Entity *)r;
    if (!e->visible)
      return OK;
    Mat4 world = mat4MultiplyM(&e->transform, parent);
    for (size_t i = 0; i < e->children_entities.size; i++) {
      if (path_push(path, i))
        return RENDER_ERROR;
      Entity *child = &((Entity *)e->children_entities.data)[i];
      if (validate_walk(&child->renderable, &world, path, scene, matched))
        return RENDER_ERROR;
      path->size--;
    }
    return validate_walk(e->entity_renderable, &world, path, scene, matched);
  }

  if (r->render != object_render)
    return RENDER_ERROR;

  const BakeInstance *inst = find_instance(scene, path->data, path->size);
  if (inst == 0 || !instance_matches(inst, (const Object *)r, parent))
    return RENDER_ERROR;
  (*matched)++;
  return OK;
}

int renderer_render_lightmapped(Renderer *r, const BakeScene *scene,
                                float exposure) {
  if (r == 0 || scene == 0 || !r->clear || r->backend == 0 ||
      r->root_renderable == 0 || !isfinite(exposure) ||
      r->framebuffer.size.x <= 0 || r->framebuffer.size.y <= 0)
    return RENDER_ERROR;

  // All validation finishes before any backend callback.
  PathList path = {0, 0, 0};
  Mat4 identity = mat4Identity();
  size_t matched = 0;
  if (validate_walk(r->root_renderable, &identity, &path, scene, &matched) ||
      matched != scene->instance_count) {
    free(path.data);
    return RENDER_ERROR;
  }
  free(path.data);

  size_t pixels =
      (size_t)r->framebuffer.size.x * (size_t)r->framebuffer.size.y;
  Pixel *color = calloc(pixels, sizeof(Pixel));
  PingoDepth *depth = calloc(pixels, sizeof(PingoDepth));
  if (color == 0 || depth == 0) {
    free(color);
    free(depth);
    return RENDER_ERROR;
  }

  LightRenderCtx cx = {r, scene, exposure, color, depth};
  PathList render_path = {0, 0, 0};
  int error =
      light_render_walk(r->root_renderable, &identity, &render_path, &cx);
  free(render_path.data);
  if (error) {
    free(color);
    free(depth);
    return RENDER_ERROR; // no half frame is ever published
  }

  Backend *be = r->backend;
  be->beforeRender(r, be);
  Pixel *frame = be->getFrameBuffer(r, be);
  PingoDepth *zeta = be->getZetaBuffer(r, be);
  memcpy(frame, color, pixels * sizeof(Pixel));
  memcpy(zeta, depth, pixels * sizeof(PingoDepth));
  be->afterRender(r, be);

  free(color);
  free(depth);
  return OK;
}
