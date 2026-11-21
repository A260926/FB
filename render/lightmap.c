#include "lightmap.h"

#include "backend.h"
#include "depth.h"
#include "material.h"
#include "math/fun.h"
#include "math/mat4.h"
#include "mesh.h"
#include "object.h"
#include "renderable.h"
#include "texture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The renderable vtable entries are the only way to tell the node types
 * apart; both are defined in their respective translation units. */
extern int entity_render(void *this, Mat4 transform, Renderer *renderer);
extern int object_render(void *this, Mat4 transform, Renderer *renderer);

#define LM_PI 3.14159265358979323846

/* ------------------------------------------------------------------------
 * Small helpers
 * --------------------------------------------------------------------- */

static double lm_orient2(double ax, double ay, double bx, double by, double cx,
                         double cy) {
  return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* Half-open edge rule: of the two directions along a shared edge exactly
 * one owns the points on it, so adjacent faces never double-cover. */
static int lm_edge_owned(double ax, double ay, double bx, double by) {
  return (by > ay) || (by == ay && bx < ax);
}

static int lm_path_cmp(const size_t *a, size_t an, const size_t *b, size_t bn) {
  size_t n = an < bn ? an : bn;
  size_t i;
  for (i = 0; i < n; i++) {
    if (a[i] != b[i])
      return a[i] < b[i] ? -1 : 1;
  }
  if (an != bn)
    return an < bn ? -1 : 1;
  return 0;
}

static int lm_valid_channel(double c) { return isfinite(c) && c >= 0.0; }

/* ------------------------------------------------------------------------
 * Scene teardown
 * --------------------------------------------------------------------- */

static void lm_instance_free(BakeInstance *inst) {
  free(inst->path);
  free(inst->pos_indices);
  free(inst->positions);
  free(inst->uvs);
  free(inst->uv_indices);
  free(inst->tris);
  free(inst->texel_face);
  free(inst->texel_point);
  free(inst->radiance);
  free(inst->radiance_next);
  memset(inst, 0, sizeof(*inst));
}

void bake_scene_init(BakeScene *scene) {
  if (scene)
    memset(scene, 0, sizeof(*scene));
}

void bake_scene_free(BakeScene *scene) {
  size_t i;
  if (!scene)
    return;
  for (i = 0; i < scene->instance_count; i++)
    lm_instance_free(&scene->instances[i]);
  free(scene->instances);
  bake_scene_init(scene);
}

/* ------------------------------------------------------------------------
 * Entity tree walking
 * --------------------------------------------------------------------- */

typedef struct {
  size_t *path;    // owned
  size_t length;
  Mat4 world;
  Object *object;  // live, only used while building/validating
} LmCollected;

typedef struct {
  LmCollected *items;
  size_t count;
  size_t capacity;
  size_t *path;    // scratch, grows with the tree depth
  size_t path_capacity;
  int error;
} LmWalk;

static void lm_walk(LmWalk *walk, Entity *entity, Mat4 parent, size_t depth) {
  Mat4 world;
  size_t i;

  if (walk->error)
    return;
  if (!entity || !entity->visible)
    return; // invisible subtrees are ignored entirely

  world = mat4MultiplyM(&entity->transform, &parent);

  if (depth + 1 > walk->path_capacity) {
    size_t capacity = walk->path_capacity ? walk->path_capacity * 2 : 16;
    size_t *grown = realloc(walk->path, capacity * sizeof(size_t));
    if (!grown) {
      walk->error = 1;
      return;
    }
    walk->path = grown;
    walk->path_capacity = capacity;
  }

  /* Children first, then the entity's own renderable: the same order
   * entity_render() draws in. */
  for (i = 0; i < entity->children_entities.size; i++) {
    Entity *child = &((Entity *)entity->children_entities.data)[i];
    walk->path[depth] = i;
    lm_walk(walk, child, world, depth + 1);
    if (walk->error)
      return;
  }

  {
    Renderable *renderable = entity->entity_renderable;
    if (!renderable || renderable->render != &object_render) {
      walk->error = 1; // only Entity nodes and Object leaves are supported
      return;
    }
  }

  if (walk->count == walk->capacity) {
    size_t capacity = walk->capacity ? walk->capacity * 2 : 8;
    LmCollected *grown = realloc(walk->items, capacity * sizeof(LmCollected));
    if (!grown) {
      walk->error = 1;
      return;
    }
    walk->items = grown;
    walk->capacity = capacity;
  }

  {
    LmCollected *item = &walk->items[walk->count];
    item->path = malloc((depth + 1) * sizeof(size_t));
    if (!item->path) {
      walk->error = 1;
      return;
    }
    memcpy(item->path, walk->path, depth * sizeof(size_t));
    item->length = depth;
    item->world = world;
    item->object = (Object *)entity->entity_renderable;
    walk->count++;
  }
}

static void lm_walk_free(LmWalk *walk) {
  size_t i;
  for (i = 0; i < walk->count; i++)
    free(walk->items[i].path);
  free(walk->items);
  free(walk->path);
  memset(walk, 0, sizeof(*walk));
}

static int lm_collect(const Entity *root, LmWalk *walk) {
  memset(walk, 0, sizeof(*walk));
  lm_walk(walk, (Entity *)root, mat4Identity(), 0);
  if (walk->error) {
    lm_walk_free(walk);
    return 1;
  }
  return 0;
}

static int lm_collected_cmp(const void *pa, const void *pb) {
  const LmCollected *a = pa;
  const LmCollected *b = pb;
  return lm_path_cmp(a->path, a->length, b->path, b->length);
}

/* ------------------------------------------------------------------------
 * Build
 * --------------------------------------------------------------------- */

static int lm_binding_valid(const BakeBinding *binding, int indexes_count) {
  int i;
  if (binding->width <= 0 || binding->height <= 0)
    return 0;
  if (binding->width > 16384 || binding->height > 16384)
    return 0; // keeps texel counts and indices far from overflow
  if (!lm_valid_channel(binding->emission.x) ||
      !lm_valid_channel(binding->emission.y) ||
      !lm_valid_channel(binding->emission.z))
    return 0;
  if (!lm_valid_channel(binding->reflectance.x) ||
      !lm_valid_channel(binding->reflectance.y) ||
      !lm_valid_channel(binding->reflectance.z))
    return 0;
  if (binding->reflectance.x >= 1.0f || binding->reflectance.y >= 1.0f ||
      binding->reflectance.z >= 1.0f)
    return 0;
  if (indexes_count == 0)
    return 1; // nothing to shade: UV arrays may be empty
  if (!binding->uvs || !binding->uv_indices || binding->uv_count == 0)
    return 0;
  for (i = 0; i < (int)binding->uv_count; i++) {
    float u = binding->uvs[i].x;
    float v = binding->uvs[i].y;
    if (!(u >= 0.0f && u <= 1.0f) || !(v >= 0.0f && v <= 1.0f))
      return 0;
  }
  for (i = 0; i < indexes_count; i++) {
    if (binding->uv_indices[i] >= binding->uv_count)
      return 0;
  }
  return 1;
}

static int lm_instance_build(BakeInstance *inst, const LmCollected *item,
                             const BakeBinding *binding) {
  const Object *object = item->object;
  const Mesh *mesh = object->mesh;
  size_t texels = (size_t)binding->width * (size_t)binding->height;
  int ok = 0;
  int i;

  memset(inst, 0, sizeof(*inst));
  inst->path_length = item->length;
  inst->world = item->world;
  inst->width = binding->width;
  inst->height = binding->height;
  inst->reflectance = binding->reflectance;
  inst->emission = binding->emission;

  if (!mesh || mesh->indexes_count < 0 || mesh->indexes_count % 3 != 0)
    return 1;
  inst->indexes_count = mesh->indexes_count;
  inst->tri_count = (size_t)mesh->indexes_count / 3;

  inst->path = malloc((item->length + 1) * sizeof(size_t));
  inst->texel_face = malloc(texels * sizeof(int));
  inst->texel_point = malloc(texels * sizeof(Vec3f));
  inst->radiance = calloc(texels, sizeof(Vec3f));
  inst->radiance_next = calloc(texels, sizeof(Vec3f));
  if (!inst->path || !inst->texel_face || !inst->texel_point ||
      !inst->radiance || !inst->radiance_next)
    goto fail;
  memcpy(inst->path, item->path, item->length * sizeof(size_t));

  if (inst->indexes_count > 0) {
    if (!mesh->pos_indices || !mesh->positions)
      goto fail;

    inst->pos_indices = malloc((size_t)inst->indexes_count * sizeof(uint16_t));
    inst->uv_indices = malloc((size_t)inst->indexes_count * sizeof(uint16_t));
    inst->tris = malloc(inst->tri_count * sizeof(BakeTri));
    if (!inst->pos_indices || !inst->uv_indices || !inst->tris)
      goto fail;
    memcpy(inst->pos_indices, mesh->pos_indices,
           (size_t)inst->indexes_count * sizeof(uint16_t));
    memcpy(inst->uv_indices, binding->uv_indices,
           (size_t)inst->indexes_count * sizeof(uint16_t));

    /* The mesh carries no position count, so freeze the referenced range. */
    inst->position_count = 0;
    for (i = 0; i < inst->indexes_count; i++) {
      if ((size_t)inst->pos_indices[i] + 1 > inst->position_count)
        inst->position_count = (size_t)inst->pos_indices[i] + 1;
    }
    inst->positions = malloc(inst->position_count * sizeof(Vec3f));
    if (!inst->positions)
      goto fail;
    memcpy(inst->positions, mesh->positions,
           inst->position_count * sizeof(Vec3f));

    inst->uv_count = binding->uv_count;
    inst->uvs = malloc(inst->uv_count * sizeof(Vec2f));
    if (!inst->uvs)
      goto fail;
    memcpy(inst->uvs, binding->uvs, inst->uv_count * sizeof(Vec2f));

    /* Freeze world triangles; the face normal comes from the transformed
     * winding, which handles non-uniform scale and reflections uniformly. */
    for (i = 0; i < (int)inst->tri_count; i++) {
      BakeTri *tri = &inst->tris[i];
      Vec3f e1, e2, n;
      tri->p0 = mat4MultiplyVec3(&inst->positions[inst->pos_indices[i * 3 + 0]],
                                 &inst->world);
      tri->p1 = mat4MultiplyVec3(&inst->positions[inst->pos_indices[i * 3 + 1]],
                                 &inst->world);
      tri->p2 = mat4MultiplyVec3(&inst->positions[inst->pos_indices[i * 3 + 2]],
                                 &inst->world);
      e1 = vec3fsubV(tri->p1, tri->p0);
      e2 = vec3fsubV(tri->p2, tri->p0);
      n = vec3Cross(e1, e2);
      tri->normal = vec3Normalize(n);
      tri->uv0 = inst->uvs[inst->uv_indices[i * 3 + 0]];
      tri->uv1 = inst->uvs[inst->uv_indices[i * 3 + 1]];
      tri->uv2 = inst->uvs[inst->uv_indices[i * 3 + 2]];
      tri->instance = -1; // filled in by the caller
      tri->face = i;
    }
  }

  {
    size_t t;
    for (t = 0; t < texels; t++)
      inst->texel_face[t] = -1;
  }

  /* Rasterize the secondary-UV triangles into coverage. */
  for (i = 0; i < (int)inst->tri_count; i++) {
    const BakeTri *tri = &inst->tris[i];
    double ax = tri->uv0.x, ay = tri->uv0.y;
    double bx = tri->uv1.x, by = tri->uv1.y;
    double cx = tri->uv2.x, cy = tri->uv2.y;
    double area = lm_orient2(ax, ay, bx, by, cx, cy);
    double minu, maxu, minv, maxv;
    int x0, x1, y0, y1, x, y;

    if (area == 0.0)
      continue; // zero-area UV faces are ignored
    if (area < 0.0) {
      /* Mirrored UVs are fine: unify to a positive orientation for the
       * half-open coverage test. The bound UVs stay untouched. */
      double tx = bx, ty = by;
      bx = cx; by = cy;
      cx = tx; cy = ty;
      area = -area;
    }

    minu = ax < bx ? ax : bx;
    minu = minu < cx ? minu : cx;
    maxu = ax > bx ? ax : bx;
    maxu = maxu > cx ? maxu : cx;
    minv = ay < by ? ay : by;
    minv = minv < cy ? minv : cy;
    maxv = ay > by ? ay : by;
    maxv = maxv > cy ? maxv : cy;

    x0 = (int)floor(minu * inst->width) - 1;
    x1 = (int)ceil(maxu * inst->width) + 1;
    y0 = (int)floor(minv * inst->height) - 1;
    y1 = (int)ceil(maxv * inst->height) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > inst->width - 1) x1 = inst->width - 1;
    if (y1 > inst->height - 1) y1 = inst->height - 1;

    for (y = y0; y <= y1; y++) {
      for (x = x0; x <= x1; x++) {
        double u = (x + 0.5) / inst->width;
        double v = (y + 0.5) / inst->height;
        double e0 = lm_orient2(ax, ay, bx, by, u, v);
        double e1 = lm_orient2(bx, by, cx, cy, u, v);
        double e2 = lm_orient2(cx, cy, ax, ay, u, v);
        int inside =
            (e0 > 0.0 || (e0 == 0.0 && lm_edge_owned(ax, ay, bx, by))) &&
            (e1 > 0.0 || (e1 == 0.0 && lm_edge_owned(bx, by, cx, cy))) &&
            (e2 > 0.0 || (e2 == 0.0 && lm_edge_owned(cx, cy, ax, ay)));
        int index;
        if (!inside)
          continue;
        index = y * inst->width + x;
        if (inst->texel_face[index] != -1)
          goto fail; // positive-area coverage conflict: report, never preempt
        inst->texel_face[index] = i;
        {
          /* Barycentric in the original (possibly mirrored) UV triangle,
           * then through to the world positions. */
          double full = lm_orient2(tri->uv0.x, tri->uv0.y, tri->uv1.x,
                                   tri->uv1.y, tri->uv2.x, tri->uv2.y);
          double w0 = lm_orient2(tri->uv1.x, tri->uv1.y, tri->uv2.x,
                                 tri->uv2.y, u, v) / full;
          double w1 = lm_orient2(tri->uv2.x, tri->uv2.y, tri->uv0.x,
                                 tri->uv0.y, u, v) / full;
          double w2 = lm_orient2(tri->uv0.x, tri->uv0.y, tri->uv1.x,
                                 tri->uv1.y, u, v) / full;
          inst->texel_point[index] = (Vec3f){
              (float)(w0 * tri->p0.x + w1 * tri->p1.x + w2 * tri->p2.x),
              (float)(w0 * tri->p0.y + w1 * tri->p1.y + w2 * tri->p2.y),
              (float)(w0 * tri->p0.z + w1 * tri->p1.z + w2 * tri->p2.z)};
        }
        inst->radiance[index] = inst->emission;
      }
    }
  }

  ok = 1;
fail:
  if (!ok) {
    lm_instance_free(inst);
    return 1;
  }
  return 0;
}

int bake_scene_build(BakeScene *scene, const Entity *root,
                     const BakeBinding *bindings, size_t binding_count) {
  BakeScene next;
  LmWalk walk;
  size_t i, j;
  int failed = 1;

  if (!scene || !root || (binding_count && !bindings))
    return INIT_ERROR;

  bake_scene_init(&next);
  if (lm_collect(root, &walk))
    return INIT_ERROR;

  if (walk.count != binding_count)
    goto out; // every visible Object bound exactly once, no extras

  qsort(walk.items, walk.count, sizeof(LmCollected), lm_collected_cmp);

  next.instances = calloc(walk.count ? walk.count : 1, sizeof(BakeInstance));
  if (!next.instances)
    goto out;
  next.instance_count = walk.count;

  for (i = 0; i < walk.count; i++) {
    const BakeBinding *binding = 0;
    for (j = 0; j < binding_count; j++) {
      if (lm_path_cmp(bindings[j].path, bindings[j].path_length,
                      walk.items[i].path, walk.items[i].length) == 0) {
        if (binding)
          goto out; // bound twice
        binding = &bindings[j];
      }
    }
    if (!binding)
      goto out; // visible Object without a binding
    if (!lm_binding_valid(binding, walk.items[i].object->mesh
                                       ? walk.items[i].object->mesh->indexes_count
                                       : -1))
      goto out;
    if (lm_instance_build(&next.instances[i], &walk.items[i], binding))
      goto out;
    {
      size_t t;
      for (t = 0; t < next.instances[i].tri_count; t++)
        next.instances[i].tris[t].instance = (int)i;
    }
  }

  /* Every binding must have matched: counts are equal and instances are
   * unique, so a duplicate binding path would already have failed above. */
  failed = 0;
out:
  lm_walk_free(&walk);
  if (failed) {
    bake_scene_free(&next);
    return INIT_ERROR; // the previous scene, if any, stays untouched
  }
  bake_scene_free(scene);
  *scene = next;
  return OK;
}

/* ------------------------------------------------------------------------
 * Record access
 * --------------------------------------------------------------------- */

int bake_scene_find(const BakeScene *scene, const size_t *path,
                    size_t path_length) {
  size_t i;
  if (!scene)
    return -1;
  for (i = 0; i < scene->instance_count; i++) {
    if (lm_path_cmp(scene->instances[i].path, scene->instances[i].path_length,
                    path, path_length) == 0)
      return (int)i;
  }
  return -1;
}

int bake_scene_texel(const BakeScene *scene, int instance, int x, int y,
                     BakeTexel *out) {
  const BakeInstance *inst;
  int index, face;
  if (!scene || !out || instance < 0 || (size_t)instance >= scene->instance_count)
    return 1;
  inst = &scene->instances[instance];
  if (x < 0 || x >= inst->width || y < 0 || y >= inst->height)
    return 1;
  index = y * inst->width + x;
  face = inst->texel_face[index];
  memset(out, 0, sizeof(*out));
  out->face = face;
  out->covered = face >= 0;
  out->radiance = inst->radiance[index];
  if (out->covered) {
    out->point = inst->texel_point[index];
    out->normal = inst->tris[face].normal;
  }
  return 0;
}

/* ------------------------------------------------------------------------
 * Baking
 * --------------------------------------------------------------------- */

static double lm_radical_inverse_base2(unsigned k) {
  double x = 0.0, f = 0.5;
  while (k) {
    if (k & 1u)
      x += f;
    k >>= 1;
    f *= 0.5;
  }
  return x;
}

/* Fixed cosine-hemisphere direction k of S around unit normal n. */
static void lm_sample_direction(unsigned k, unsigned s, const Vec3f *n,
                                double out[3]) {
  double u = (k + 0.5) / s;
  double v = lm_radical_inverse_base2(k);
  double r = sqrt(u);
  double phi = 2.0 * LM_PI * v;
  double lx = r * cos(phi);
  double ly = r * sin(phi);
  double lz = sqrt(1.0 - u);
  double ax = 0.0, ay = 0.0, az = 0.0;
  double tx, ty, tz, tl, bx, by, bz;

  if (fabs((double)n->y) < 0.9) {
    ay = 1.0;
  } else {
    ax = 1.0;
  }
  /* T = normalize(cross(axis, N)), B = cross(N, T) */
  tx = ay * (double)n->z - az * (double)n->y;
  ty = az * (double)n->x - ax * (double)n->z;
  tz = ax * (double)n->y - ay * (double)n->x;
  tl = sqrt(tx * tx + ty * ty + tz * tz);
  if (tl == 0.0) {
    out[0] = lx;
    out[1] = ly;
    out[2] = lz;
    return;
  }
  tx /= tl;
  ty /= tl;
  tz /= tl;
  bx = (double)n->y * tz - (double)n->z * ty;
  by = (double)n->z * tx - (double)n->x * tz;
  bz = (double)n->x * ty - (double)n->y * tx;

  out[0] = tx * lx + bx * ly + (double)n->x * lz;
  out[1] = ty * lx + by * ly + (double)n->y * lz;
  out[2] = tz * lx + bz * ly + (double)n->z * lz;
}

static int lm_ray_tri(const BakeTri *tri, const double o[3],
                      const double d[3], double min_t, double *t_out,
                      double *u_out, double *v_out) {
  double e1[3] = {(double)tri->p1.x - tri->p0.x, (double)tri->p1.y - tri->p0.y,
                  (double)tri->p1.z - tri->p0.z};
  double e2[3] = {(double)tri->p2.x - tri->p0.x, (double)tri->p2.y - tri->p0.y,
                  (double)tri->p2.z - tri->p0.z};
  double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                  d[0] * e2[1] - d[1] * e2[0]};
  double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
  double inv, tv[3], u, qv[3], v, t;

  if (det == 0.0)
    return 0; // ray parallel to the triangle plane
  inv = 1.0 / det;
  tv[0] = o[0] - tri->p0.x;
  tv[1] = o[1] - tri->p0.y;
  tv[2] = o[2] - tri->p0.z;
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

static int lm_nearest_texel(double u, int n) {
  int i = (int)floor(u * n);
  if (i < 0)
    i = 0;
  if (i > n - 1)
    i = n - 1;
  return i;
}

/* Nearest hit of the ray against every world triangle. Instances are
 * stored sorted by entity path and faces in submission order, so keeping
 * the first strictly-smallest t implements the path/face tie-break. */
static int lm_trace(const BakeScene *scene, const double o[3],
                    const double d[3], double min_t, const BakeTri **hit,
                    double *hit_u, double *hit_v) {
  double best_t = 0.0, best_u = 0.0, best_v = 0.0;
  const BakeTri *best = 0;
  size_t i, f;
  for (i = 0; i < scene->instance_count; i++) {
    const BakeInstance *inst = &scene->instances[i];
    for (f = 0; f < inst->tri_count; f++) {
      double t, u, v;
      if (!lm_ray_tri(&inst->tris[f], o, d, min_t, &t, &u, &v))
        continue;
      if (!best || t < best_t) {
        best = &inst->tris[f];
        best_t = t;
        best_u = u;
        best_v = v;
      }
    }
  }
  *hit = best;
  *hit_u = best_u;
  *hit_v = best_v;
  return best != 0;
}

static void lm_bake_round(BakeScene *scene, unsigned samples, double epsilon,
                          const double environment[3]) {
  size_t i;
  int x, y;
  unsigned k;

  for (i = 0; i < scene->instance_count; i++) {
    BakeInstance *inst = &scene->instances[i];
    for (y = 0; y < inst->height; y++) {
      for (x = 0; x < inst->width; x++) {
        int index = y * inst->width + x;
        int face = inst->texel_face[index];
        const BakeTri *surface;
        Vec3f n, p;
        double o[3], sum[3] = {0.0, 0.0, 0.0};
        if (face < 0)
          continue;
        surface = &inst->tris[face];
        n = surface->normal;
        p = inst->texel_point[index];
        o[0] = (double)p.x + epsilon * (double)n.x;
        o[1] = (double)p.y + epsilon * (double)n.y;
        o[2] = (double)p.z + epsilon * (double)n.z;

        for (k = 0; k < samples; k++) {
          double d[3];
          const BakeTri *hit;
          double hu, hv;
          lm_sample_direction(k, samples, &n, d);
          if (!lm_trace(scene, o, d, epsilon, &hit, &hu, &hv)) {
            sum[0] += environment[0];
            sum[1] += environment[1];
            sum[2] += environment[2];
            continue;
          }
          if (d[0] * (double)hit->normal.x + d[1] * (double)hit->normal.y +
                  d[2] * (double)hit->normal.z >
              0.0)
            continue; // back face: absorbs, contributes nothing
          {
            const BakeInstance *target = &scene->instances[hit->instance];
            double w0 = 1.0 - hu - hv;
            double u = w0 * hit->uv0.x + hu * hit->uv1.x + hv * hit->uv2.x;
            double v = w0 * hit->uv0.y + hu * hit->uv1.y + hv * hit->uv2.y;
            int tx = lm_nearest_texel(u, target->width);
            int ty = lm_nearest_texel(v, target->height);
            int tindex = ty * target->width + tx;
            if (target->texel_face[tindex] >= 0) {
              Vec3f l = target->radiance[tindex]; // strictly the old round
              sum[0] += (double)l.x;
              sum[1] += (double)l.y;
              sum[2] += (double)l.z;
            }
          }
        }

        inst->radiance_next[index] = (Vec3f){
            (float)((double)inst->emission.x +
                    (double)inst->reflectance.x * sum[0] / samples),
            (float)((double)inst->emission.y +
                    (double)inst->reflectance.y * sum[1] / samples),
            (float)((double)inst->emission.z +
                    (double)inst->reflectance.z * sum[2] / samples)};
      }
    }
  }

  /* No in-place updates: the whole round read the old radiance, only now
   * do the new values become visible. */
  for (i = 0; i < scene->instance_count; i++) {
    BakeInstance *inst = &scene->instances[i];
    Vec3f *swap = inst->radiance;
    inst->radiance = inst->radiance_next;
    inst->radiance_next = swap;
  }
  scene->rounds++;
}

int bake_step(BakeScene *scene, int rounds, int samples, float epsilon,
              Vec3f environment) {
  double env[3];
  int r;

  if (!scene || rounds < 0 || samples < 1)
    return 1;
  if (!isfinite((double)epsilon) || epsilon < 0.0f)
    return 1;
  if (!lm_valid_channel(environment.x) || !lm_valid_channel(environment.y) ||
      !lm_valid_channel(environment.z))
    return 1;

  env[0] = environment.x;
  env[1] = environment.y;
  env[2] = environment.z;
  for (r = 0; r < rounds; r++)
    lm_bake_round(scene, (unsigned)samples, (double)epsilon, env);
  return 0;
}

void bake_reset(BakeScene *scene) {
  size_t i;
  int t;
  if (!scene)
    return;
  for (i = 0; i < scene->instance_count; i++) {
    BakeInstance *inst = &scene->instances[i];
    int texels = inst->width * inst->height;
    for (t = 0; t < texels; t++) {
      inst->radiance[t] =
          inst->texel_face[t] >= 0 ? inst->emission : (Vec3f){0, 0, 0};
      inst->radiance_next[t] = (Vec3f){0, 0, 0};
    }
  }
  scene->rounds = 0;
}

/* ------------------------------------------------------------------------
 * Lightmapped rendering
 * --------------------------------------------------------------------- */

static int lm_validate_one(const LmCollected *item, const BakeScene *scene,
                           int *instance_out) {
  const Object *object = item->object;
  const Mesh *mesh = object->mesh;
  size_t i;
  const BakeInstance *inst = 0;

  for (i = 0; i < scene->instance_count; i++) {
    if (lm_path_cmp(scene->instances[i].path, scene->instances[i].path_length,
                    item->path, item->length) == 0) {
      inst = &scene->instances[i];
      break;
    }
  }
  if (!inst)
    return 1; // live instance missing from the snapshot
  if (!mesh || mesh->indexes_count != inst->indexes_count)
    return 1;
  if (memcmp(&item->world, &inst->world, sizeof(Mat4)) != 0)
    return 1; // transform drifted since the bake
  if (inst->indexes_count > 0) {
    if (!mesh->pos_indices || !mesh->positions)
      return 1;
    if (memcmp(mesh->pos_indices, inst->pos_indices,
               (size_t)inst->indexes_count * sizeof(uint16_t)) != 0)
      return 1;
    if (memcmp(mesh->positions, inst->positions,
               inst->position_count * sizeof(Vec3f)) != 0)
      return 1;
  }
  *instance_out = (int)(inst - scene->instances);
  return 0;
}

/* Frustum test identical to object.c's: kept private so the lightmapped
 * path culls exactly like the textured one. */
static int lm_frustum_cull_triangle(Vec4f a, Vec4f b, Vec4f c) {
  if (a.z > a.w && b.z > b.w && c.z > c.w)
    return 1;
  if (a.z < -a.w && b.z < -b.w && c.z < -c.w)
    return 1;
  if (a.x < -a.w && b.x < -b.w && c.x < -c.w)
    return 1;
  if (a.x > a.w && b.x > b.w && c.x > c.w)
    return 1;
  if (a.y > a.w && b.y > b.w && c.y > c.w)
    return 1;
  if (a.y < -a.w && b.y < -b.w && c.y < -c.w)
    return 1;
  return 0;
}

static uint8_t lm_channel_to_byte(float base, float radiance, float exposure) {
  float c = base / 255.0f * radiance * exposure;
  if (c < 0.0f)
    c = 0.0f;
  if (c > 1.0f)
    c = 1.0f;
  return (uint8_t)(c * 255.0f); // truncate, as specified
}

/* Rasterizes one instance into private buffers, mirroring object_render's
 * transform, coverage and depth decisions exactly; only the shading
 * differs (baked radiance instead of the fixed directional light). */
static void lm_render_object(const Object *o, const BakeInstance *inst, Mat4 m,
                             Renderer *r, Texture *frame, PingoDepth *zbuffer,
                             float exposure) {
  const Vec2i scrSize = r->framebuffer.size;
  Mat4 v = mat4Inverse(&r->camera_view);
  Mat4 p = r->camera_projection;
  const Mesh *mesh = o->mesh;
  int i;

  for (i = 0; i < mesh->indexes_count; i += 3) {
    const Vec3f *ver1 = &mesh->positions[mesh->pos_indices[i + 0]];
    const Vec3f *ver2 = &mesh->positions[mesh->pos_indices[i + 1]];
    const Vec3f *ver3 = &mesh->positions[mesh->pos_indices[i + 2]];

    Vec2f tca = {0, 0};
    Vec2f tcb = {0, 0};
    Vec2f tcc = {0, 0};
    Vec2f lma, lmb, lmc;

    if (mesh->textCoord != 0 && mesh->tex_indices != 0) {
      tca = mesh->textCoord[mesh->tex_indices[i + 0]];
      tcb = mesh->textCoord[mesh->tex_indices[i + 1]];
      tcc = mesh->textCoord[mesh->tex_indices[i + 2]];
    }

    lma = inst->uvs[inst->uv_indices[i + 0]];
    lmb = inst->uvs[inst->uv_indices[i + 1]];
    lmc = inst->uvs[inst->uv_indices[i + 2]];

    {
      Vec4f a = {ver1->x, ver1->y, ver1->z, 1};
      Vec4f b = {ver2->x, ver2->y, ver2->z, 1};
      Vec4f c = {ver3->x, ver3->y, ver3->z, 1};
      Mat4 vm = mat4MultiplyM(&v, &m);
      float aw, bw, cw;
      float halfX, halfY;
      Vec2i a_s, b_s, c_s;
      int32_t minX, minY, maxX, maxY, area;
      float areaInverse;
      int32_t A01, B01, A12, B12, A20, B20;
      int32_t w0_row, w1_row, w2_row;
      Vec2i minTriangle;
      float invAw = 0, invBw = 0, invCw = 0;
      int16_t y;
      int32_t x;

      a = mat4MultiplyVec4(&a, &vm);
      b = mat4MultiplyVec4(&b, &vm);
      c = mat4MultiplyVec4(&c, &vm);

      a = mat4MultiplyVec4(&a, &p);
      b = mat4MultiplyVec4(&b, &p);
      c = mat4MultiplyVec4(&c, &p);

      if (r->enable_frustum_culling && lm_frustum_cull_triangle(a, b, c))
        continue;

      if (a.z > 0 && b.z > 0 && c.z > 0)
        continue;

      aw = a.w;
      bw = b.w;
      cw = c.w;

      a.x /= a.w; a.y /= a.w; a.z /= a.w; a.w = 1;
      b.x /= b.w; b.y /= b.w; b.z /= b.w; b.w = 1;
      c.x /= c.w; c.y /= c.w; c.z /= c.w; c.w = 1;

      if (r->enable_backface_culling) {
        float clocking = isClockWise(a.x, a.y, b.x, b.y, c.x, c.y);
        if (clocking >= 0)
          continue;
      }

      halfX = scrSize.x * 0.5f;
      halfY = scrSize.y * 0.5f;
      a_s = (Vec2i){(int)(a.x * halfX + halfX), (int)(a.y * halfY + halfY)};
      b_s = (Vec2i){(int)(b.x * halfX + halfX), (int)(b.y * halfY + halfY)};
      c_s = (Vec2i){(int)(c.x * halfX + halfX), (int)(c.y * halfY + halfY)};

      minX = MIN(MIN(a_s.x, b_s.x), c_s.x);
      minY = MIN(MIN(a_s.y, b_s.y), c_s.y);
      maxX = MAX(MAX(a_s.x, b_s.x), c_s.x);
      maxY = MAX(MAX(a_s.y, b_s.y), c_s.y);

      minX = MIN(MAX(minX, 0), r->framebuffer.size.x);
      minY = MIN(MAX(minY, 0), r->framebuffer.size.y);
      maxX = MIN(MAX(maxX, 0), r->framebuffer.size.x);
      maxY = MIN(MAX(maxY, 0), r->framebuffer.size.y);

      minTriangle = (Vec2i){minX, minY};
      area = orient2d(a_s, b_s, c_s);
      if (area == 0)
        continue;
      areaInverse = 1.0f / area;

      A01 = (a_s.y - b_s.y);
      B01 = (b_s.x - a_s.x);
      A12 = (b_s.y - c_s.y);
      B12 = (c_s.x - b_s.x);
      A20 = (c_s.y - a_s.y);
      B20 = (a_s.x - c_s.x);

      w0_row = orient2d(b_s, c_s, minTriangle);
      w1_row = orient2d(c_s, a_s, minTriangle);
      w2_row = orient2d(a_s, b_s, minTriangle);

      /* The secondary UV always rides the perspective weights; the
       * primary set only when a texture is actually sampled. */
      if (aw != 0 && bw != 0 && cw != 0) {
        invAw = 1.0f / aw;
        invBw = 1.0f / bw;
        invCw = 1.0f / cw;
        tca.x *= invAw; tca.y *= invAw;
        tcb.x *= invBw; tcb.y *= invBw;
        tcc.x *= invCw; tcc.y *= invCw;
        lma.x *= invAw; lma.y *= invAw;
        lmb.x *= invBw; lmb.y *= invBw;
        lmc.x *= invCw; lmc.y *= invCw;
      }

      for (y = (int16_t)minY; y < maxY;
           y++, w0_row += B12, w1_row += B20, w2_row += B01) {
        int32_t w0 = w0_row;
        int32_t w1 = w1_row;
        int32_t w2 = w2_row;

        for (x = minX; x < maxX; x++, w0 += A12, w1 += A20, w2 += A01) {
          float depth;
          int pixel_index;
          float interpInvW;
          Pixel base;
          float su, sv;
          int tx, ty, tindex;
          Vec3f l = {0, 0, 0};

          if ((w0 | w1 | w2) < 0)
            continue;

          depth = -(w0 * a.z + w1 * b.z + w2 * c.z) * areaInverse;
          if (depth < -1.0f || depth > 1.0f)
            continue;

          pixel_index = x + y * scrSize.x;
          if (depth_check(zbuffer, pixel_index, depth))
            continue;
          depth_write(zbuffer, pixel_index, depth);

          interpInvW = w0 * invAw + w1 * invBw + w2 * invCw;
          if (interpInvW == 0)
            continue;

          if (o->material != 0 && o->material->texture != 0) {
            const float textCoordx =
                (w0 * tca.x + w1 * tcb.x + w2 * tcc.x) / interpInvW;
            const float textCoordy =
                (w0 * tca.y + w1 * tcb.y + w2 * tcc.y) / interpInvW;
            base = texture_readF(o->material->texture,
                                 (Vec2f){textCoordx, textCoordy});
          } else {
            base = pixelFromUInt8(255);
          }

          su = (w0 * lma.x + w1 * lmb.x + w2 * lmc.x) / interpInvW;
          sv = (w0 * lma.y + w1 * lmb.y + w2 * lmc.y) / interpInvW;
          tx = lm_nearest_texel(su, inst->width);
          ty = lm_nearest_texel(sv, inst->height);
          tindex = ty * inst->width + tx;
          if (inst->texel_face[tindex] >= 0)
            l = inst->radiance[tindex];

          texture_draw(frame, (Vec2i){x, y},
                       pixelFromRGBA(
                           lm_channel_to_byte((float)base.r, l.x, exposure),
                           lm_channel_to_byte((float)base.g, l.y, exposure),
                           lm_channel_to_byte((float)base.b, l.z, exposure),
                           base.a));
        }
      }
    }
  }
}

/* Validates and renders in one collected walk: the list is in draw order
 * (children first, then the entity's own object, like entity_render), so
 * the render pass below consumes it directly. */
int renderer_render_lightmapped(Renderer *r, const BakeScene *scene,
                                float exposure) {
  LmWalk walk;
  size_t i;
  int failed = 1;
  Pixel *color = 0;
  PingoDepth *depth = 0;
  size_t pixels;
  Backend *be;

  if (!r || !scene || !r->backend || !r->root_renderable)
    return RENDER_ERROR;
  if (!r->clear)
    return RENDER_ERROR; // this entry requires clear=true
  if (exposure != exposure)
    return RENDER_ERROR; // NaN would turn into undefined bytes
  if (r->root_renderable->render != &entity_render)
    return RENDER_ERROR; // the snapshot was built from an Entity tree

  if (lm_collect((const Entity *)r->root_renderable, &walk))
    return RENDER_ERROR;
  if (walk.count != scene->instance_count)
    goto out;
  for (i = 0; i < walk.count; i++) {
    int instance = -1;
    if (lm_validate_one(&walk.items[i], scene, &instance))
      goto out;
  }

  pixels = (size_t)r->framebuffer.size.x * (size_t)r->framebuffer.size.y;
  color = malloc(pixels * sizeof(Pixel));
  depth = malloc(pixels * sizeof(PingoDepth));
  if (!color || !depth)
    goto out;
  memset(color, 0, pixels * sizeof(Pixel));
  memset(depth, 0, pixels * sizeof(PingoDepth));

  {
    Texture frame;
    frame.size = r->framebuffer.size;
    frame.frameBuffer = color;

    /* Draw in the same order entity_render would: children first, then
     * the entity's own object. The collected list is in that order. */
    for (i = 0; i < walk.count; i++) {
      int instance = bake_scene_find(scene, walk.items[i].path,
                                     walk.items[i].length);
      if (instance < 0)
        goto out;
      lm_render_object(walk.items[i].object, &scene->instances[instance],
                       walk.items[i].world, r, &frame, depth, exposure);
    }
  }

  /* Everything drew: only now touch the backend and publish the frame. */
  be = r->backend;
  be->beforeRender(r, be);
  {
    Pixel *target = be->getFrameBuffer(r, be);
    PingoDepth *ztarget = be->getZetaBuffer(r, be);
    if (target && ztarget) {
      memcpy(target, color, pixels * sizeof(Pixel));
      memcpy(ztarget, depth, pixels * sizeof(PingoDepth));
      r->framebuffer.frameBuffer = target;
      failed = 0;
    }
  }
  be->afterRender(r, be);

out:
  free(color);
  free(depth);
  lm_walk_free(&walk);
  return failed ? RENDER_ERROR : OK;
}
