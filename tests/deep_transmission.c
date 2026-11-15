/**
 * Deep screen transmission regression tests.
 *
 * The production entry points are renderer_capture_deep,
 * deep_frame_sample, deep_frame_resolve and renderer_render_deep.  A small,
 * independently written per-pixel reference re-evaluates the same scene
 * description (mesh, transform, camera) and implements the layer selection and
 * transmission rule from the specification, so the checks compare two
 * implementations rather than one implementation against itself.
 *
 * Registered through task_cases/task_case_count; task_support/driver.c owns
 * main().
 */

#include "task_support/check.h"

#include "math/fun.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "render/backend.h"
#include "render/deep.h"
#include "render/depth.h"
#include "render/entity.h"
#include "render/fwd.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/pixel.h"
#include "render/renderer.h"
#include "render/texture.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 24
#define HEIGHT 18
#define PIXEL_COUNT (WIDTH * HEIGHT)
#define MAX_LAYERS 6
#define TEX_MAX 4096
#define MAX_FRAGS 64

// ---------------------------------------------------------------------------
// Pixel channel access (the Pixel format is a compile-time choice).
// ---------------------------------------------------------------------------

static void pixel_rgb(Pixel p, unsigned char *r, unsigned char *g,
                      unsigned char *b) {
#ifdef PINGO_PIXEL_UINT8
  *r = p.g;
  *g = p.g;
  *b = p.g;
#elif defined(PINGO_PIXEL_RGB565)
  *r = (unsigned char)(p.red << 3);
  *g = (unsigned char)(p.green << 2);
  *b = (unsigned char)(p.blue << 3);
#else
  *r = p.r;
  *g = p.g;
  *b = p.b;
#endif
}

static Vec3f linear_from_pixel(Pixel p) {
  unsigned char r, g, b;
  pixel_rgb(p, &r, &g, &b);
  return (Vec3f){r / 255.0f, g / 255.0f, b / 255.0f};
}

// ---------------------------------------------------------------------------
// Test backend: counts callbacks and can publish into a second buffer from
// inside beforeRender, the way a backend that resizes or swaps its surfaces
// would.
// ---------------------------------------------------------------------------

typedef struct TestBackend {
  Backend backend;
  Vec2i size;
  Pixel *frame;
  PingoDepth *depth;
  Pixel *alt_frame;
  PingoDepth *alt_depth;
  int use_alt;
  int swap_on_before;
  int fail_get;
  int before_calls;
  int after_calls;
} TestBackend;

static void tb_init(Renderer *r, Backend *b, Vec4i rect) {
  (void)r;
  (void)b;
  (void)rect;
}

static void tb_before(Renderer *r, Backend *b) {
  (void)r;
  TestBackend *t = (TestBackend *)b;
  t->before_calls++;
  if (t->swap_on_before)
    t->use_alt = 1;
}

static void tb_after(Renderer *r, Backend *b) {
  (void)r;
  ((TestBackend *)b)->after_calls++;
}

static Pixel *tb_frame(Renderer *r, Backend *b) {
  (void)r;
  TestBackend *t = (TestBackend *)b;
  if (t->fail_get)
    return 0;
  return t->use_alt ? t->alt_frame : t->frame;
}

static PingoDepth *tb_zeta(Renderer *r, Backend *b) {
  (void)r;
  TestBackend *t = (TestBackend *)b;
  if (t->fail_get)
    return 0;
  return t->use_alt ? t->alt_depth : t->depth;
}

static int test_backend_init(TestBackend *t, Vec2i size, int with_alt) {
  memset(t, 0, sizeof(*t));
  t->size = size;
  t->backend.init = tb_init;
  t->backend.beforeRender = tb_before;
  t->backend.afterRender = tb_after;
  t->backend.getFrameBuffer = tb_frame;
  t->backend.getZetaBuffer = tb_zeta;

  size_t count = (size_t)size.x * (size_t)size.y;
  t->frame = calloc(count, sizeof(Pixel));
  t->depth = calloc(count, sizeof(PingoDepth));
  if (with_alt) {
    t->alt_frame = calloc(count, sizeof(Pixel));
    t->alt_depth = calloc(count, sizeof(PingoDepth));
  }
  if (!t->frame || !t->depth || (with_alt && (!t->alt_frame || !t->alt_depth))) {
    free(t->frame);
    free(t->depth);
    free(t->alt_frame);
    free(t->alt_depth);
    return 1;
  }
  return 0;
}

static void test_backend_free(TestBackend *t) {
  free(t->frame);
  free(t->depth);
  free(t->alt_frame);
  free(t->alt_depth);
  memset(t, 0, sizeof(*t));
}

// ---------------------------------------------------------------------------
// Scene layer fixture: a textured quad whose two triangles share a diagonal.
// Varying znear/zfar tilts it so clip w changes across the screen.
// ---------------------------------------------------------------------------

typedef struct QuadLayer {
  Vec3f positions[4];
  Vec2f uvs[4];
  uint16_t pos_indices[6];
  uint16_t tex_indices[6];
  Mesh mesh;

  Pixel color_texels[TEX_MAX];
  int color_size;
  Texture color_texture;

  Pixel offset_texels[TEX_MAX];
  int offset_size;
  Texture offset_texture;

  Material material;
  Object object;
  ScreenTransmission transmission;
  int has_transmission;
} QuadLayer;

static void layer_reset(QuadLayer *l) {
  memset(l, 0, sizeof(*l));

  l->color_size = 1;
  l->color_texels[0] = pixelFromRGBA(255, 255, 255, 255);
  texture_init(&l->color_texture, (Vec2i){1, 1}, l->color_texels);

  l->offset_size = 1;
  l->offset_texels[0] = pixelFromRGBA(128, 128, 255, 255);
  texture_init(&l->offset_texture, (Vec2i){1, 1}, l->offset_texels);

  material_init(&l->material, &l->color_texture);
}

static void layer_quad(QuadLayer *l, float znear, float zfar, float sx,
                       float sy) {
  l->positions[0] = (Vec3f){-sx, -sy, -znear};
  l->positions[1] = (Vec3f){sx, -sy, -znear};
  l->positions[2] = (Vec3f){sx, sy, -zfar};
  l->positions[3] = (Vec3f){-sx, sy, -zfar};
  l->uvs[0] = (Vec2f){0, 0};
  l->uvs[1] = (Vec2f){1, 0};
  l->uvs[2] = (Vec2f){1, 1};
  l->uvs[3] = (Vec2f){0, 1};

  const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; i++) {
    l->pos_indices[i] = indices[i];
    l->tex_indices[i] = indices[i];
  }

  l->mesh.indexes_count = 6;
  l->mesh.pos_indices = l->pos_indices;
  l->mesh.tex_indices = l->tex_indices;
  l->mesh.positions = l->positions;
  l->mesh.textCoord = l->uvs;
}

static void layer_translate(QuadLayer *l, float dx, float dy, float dz) {
  for (int i = 0; i < 4; i++) {
    l->positions[i].x += dx;
    l->positions[i].y += dy;
    l->positions[i].z += dz;
  }
}

static void layer_color(QuadLayer *l, int size, const Pixel *texels) {
  l->color_size = size;
  memcpy(l->color_texels, texels, (size_t)size * (size_t)size * sizeof(Pixel));
  texture_init(&l->color_texture, (Vec2i){size, size}, l->color_texels);
}

static void layer_offset_map(QuadLayer *l, int size, const Pixel *texels) {
  l->offset_size = size;
  memcpy(l->offset_texels, texels, (size_t)size * (size_t)size * sizeof(Pixel));
  texture_init(&l->offset_texture, (Vec2i){size, size}, l->offset_texels);
}

static void layer_transmission(QuadLayer *l, float opacity, Vec3f tint,
                               Vec2f offset, int with_map) {
  l->transmission.opacity = opacity;
  l->transmission.tint = tint;
  l->transmission.offset = offset;
  l->transmission.offset_map = with_map ? &l->offset_texture : 0;
  l->has_transmission = 1;
  material_set_transmission(&l->material, &l->transmission);
}

// ---------------------------------------------------------------------------
// Scene assembly.  Children are traversed in order, so layer submission order
// is the array order.
// ---------------------------------------------------------------------------

typedef struct TestScene {
  TestBackend backend;
  Renderer renderer;
  QuadLayer *layers;
  int count;
  Entity entities[MAX_LAYERS];
  Entity root;
  Object empty_object;
  Mesh empty_mesh;
} TestScene;

static void scene_begin(TestScene *s, QuadLayer *layers, int count) {
  s->layers = layers;
  s->count = count;

  test_backend_init(&s->backend, (Vec2i){WIDTH, HEIGHT}, 0);
  renderer_init(&s->renderer, (Vec2i){WIDTH, HEIGHT}, (Backend *)&s->backend);

  memset(&s->empty_mesh, 0, sizeof(s->empty_mesh));
  object_init(&s->empty_object, &s->empty_mesh, 0);

  for (int i = 0; i < count; i++) {
    object_init(&layers[i].object, &layers[i].mesh, &layers[i].material);
    entity_init(&s->entities[i], (Renderable *)&layers[i].object,
                mat4Identity());
  }
  entity_init_children(&s->root, (Renderable *)&s->empty_object,
                       mat4Identity(), s->entities, (size_t)count);
  renderer_set_root_renderable(&s->renderer, (Renderable *)&s->root);

  s->renderer.camera_projection =
      mat4Perspective(1, 50, (float)WIDTH / (float)HEIGHT, 0.6f);
  s->renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
}

static void scene_end(TestScene *s) { test_backend_free(&s->backend); }

// ---------------------------------------------------------------------------
// Independent per-pixel reference.
//
// Only the requested pixel is evaluated: the triangle is projected, coverage
// is decided with the same integer screen vertices, and the spec's depth and
// perspective UV interpolation are written out again in this file.
// ---------------------------------------------------------------------------

typedef struct RefFrag {
  int layer_index;
  float depth;
  Vec3f color;
  int opaque;
  float opacity;
  Vec3f tint;
  Vec2f offset;
  Vec2f uv;
  Vec2f uv_affine;
} RefFrag;

typedef struct RefCtx {
  QuadLayer *layers;
  int count;
  Mat4 projection;
  Mat4 view;
  Mat4 view_inverse;
} RefCtx;

static float ref_diffuse(Vec4f a, Vec4f b, Vec4f c) {
  Vec3f a3 = {a.x, a.y, a.z};
  Vec3f b3 = {b.x, b.y, b.z};
  Vec3f c3 = {c.x, c.y, c.z};
  Vec3f n = vec3Normalize(vec3Cross(vec3fsubV(a3, b3), vec3fsubV(a3, c3)));
  Vec3f light = vec3Normalize((Vec3f){-8, 5, 5});
  float d = (1.0 + vec3Dot(n, light)) * 0.5;
  if (d < 0.0f)
    d = 0.0f;
  if (d > 1.0f)
    d = 1.0f;
  return d;
}

static int ref_layers_at(const RefCtx *ctx, int px, int py, RefFrag *out,
                         int max) {
  if (px < 0 || py < 0 || px >= WIDTH || py >= HEIGHT)
    return 0;

  Mat4 view_inverse = ctx->view_inverse;
  Mat4 p = ctx->projection;
  const float half_x = WIDTH * 0.5f;
  const float half_y = HEIGHT * 0.5f;
  int count = 0;

  for (int li = 0; li < ctx->count; li++) {
    QuadLayer *l = &ctx->layers[li];
    Material *material = &l->material;
    Mat4 vm = view_inverse; // every fixture layer uses an identity transform

    for (int i = 0; i + 2 < l->mesh.indexes_count; i += 3) {
      const Vec3f *p0 = &l->mesh.positions[l->mesh.pos_indices[i + 0]];
      const Vec3f *p1 = &l->mesh.positions[l->mesh.pos_indices[i + 1]];
      const Vec3f *p2 = &l->mesh.positions[l->mesh.pos_indices[i + 2]];

      Vec2f t0 = {0, 0}, t1 = {0, 0}, t2 = {0, 0};
      if (material != 0 && l->mesh.textCoord != 0 && l->mesh.tex_indices != 0) {
        t0 = l->mesh.textCoord[l->mesh.tex_indices[i + 0]];
        t1 = l->mesh.textCoord[l->mesh.tex_indices[i + 1]];
        t2 = l->mesh.textCoord[l->mesh.tex_indices[i + 2]];
      }

      Vec4f a = {p0->x, p0->y, p0->z, 1};
      Vec4f b = {p1->x, p1->y, p1->z, 1};
      Vec4f c = {p2->x, p2->y, p2->z, 1};
      a = mat4MultiplyVec4(&a, &vm);
      b = mat4MultiplyVec4(&b, &vm);
      c = mat4MultiplyVec4(&c, &vm);

      float diffuse = ref_diffuse(a, b, c);

      a = mat4MultiplyVec4(&a, &p);
      b = mat4MultiplyVec4(&b, &p);
      c = mat4MultiplyVec4(&c, &p);

      if (a.z > 0 && b.z > 0 && c.z > 0)
        continue;

      const float aw = a.w, bw = b.w, cw = c.w;
      a.x /= a.w; a.y /= a.w; a.z /= a.w; a.w = 1;
      b.x /= b.w; b.y /= b.w; b.z /= b.w; b.w = 1;
      c.x /= c.w; c.y /= c.w; c.z /= c.w; c.w = 1;

      if (isClockWise(a.x, a.y, b.x, b.y, c.x, c.y) >= 0)
        continue;

      Vec2i as = {(int)(a.x * half_x + half_x), (int)(a.y * half_y + half_y)};
      Vec2i bs = {(int)(b.x * half_x + half_x), (int)(b.y * half_y + half_y)};
      Vec2i cs = {(int)(c.x * half_x + half_x), (int)(c.y * half_y + half_y)};

      int32_t area = orient2d(as, bs, cs);
      if (area == 0)
        continue;

      Vec2i q = {px, py};
      int32_t w0 = orient2d(bs, cs, q);
      int32_t w1 = orient2d(cs, as, q);
      int32_t w2 = orient2d(as, bs, q);
      if ((w0 | w1 | w2) < 0)
        continue;

      float depth = -(w0 * a.z + w1 * b.z + w2 * c.z) / (float)area;
      if (!(depth > 0.0f && depth <= 1.0f))
        continue;

      int has_uv = 0;
      float inv_aw = 0, inv_bw = 0, inv_cw = 0;
      Vec2f ta = t0, tb = t1, tc = t2;
      if (material != 0 && aw != 0 && bw != 0 && cw != 0) {
        inv_aw = 1.0f / aw;
        inv_bw = 1.0f / bw;
        inv_cw = 1.0f / cw;
        ta.x *= inv_aw; ta.y *= inv_aw;
        tb.x *= inv_bw; tb.y *= inv_bw;
        tc.x *= inv_cw; tc.y *= inv_cw;
        has_uv = 1;
      }

      Vec2f uv = {0, 0};
      float interp_inv_w = 0;
      if (has_uv) {
        interp_inv_w = w0 * inv_aw + w1 * inv_bw + w2 * inv_cw;
        if (interp_inv_w == 0)
          continue;
        uv.x = (w0 * ta.x + w1 * tb.x + w2 * tc.x) / interp_inv_w;
        uv.y = (w0 * ta.y + w1 * tb.y + w2 * tc.y) / interp_inv_w;
      }

      // Barycentric weights normalised by their sum, used only to show that
      // the perspective UV genuinely differs when w varies.
      float sw = (float)w0 + (float)w1 + (float)w2;
      Vec2f uv_affine = {0, 0};
      if (sw != 0.0f) {
        uv_affine.x = (w0 * t0.x + w1 * t1.x + w2 * t2.x) / sw;
        uv_affine.y = (w0 * t0.y + w1 * t1.y + w2 * t2.y) / sw;
      }

      Vec3f color = {0, 0, 0};
      if (material != 0) {
        Pixel tex = texture_readF(material->texture, uv);
        color = linear_from_pixel(pixelMul(tex, diffuse));
      } else {
        color = linear_from_pixel(pixelMul(pixelFromUInt8(255), diffuse));
      }

      if (count == max)
        return count;

      RefFrag *f = &out[count++];
      f->layer_index = li;
      f->depth = depth;
      f->color = color;
      f->opaque = 1;
      f->opacity = 1.0f;
      f->tint = (Vec3f){0, 0, 0};
      f->offset = (Vec2f){0, 0};
      f->uv = uv;
      f->uv_affine = uv_affine;

      if (material != 0 && material->transmission != 0) {
        const ScreenTransmission *t = material->transmission;
        f->opaque = 0;
        f->opacity = t->opacity;
        f->tint = t->tint;
        Vec2f offset = t->offset;
        if (t->offset_map != 0 && has_uv) {
          Pixel map = texture_readF(t->offset_map, uv);
          unsigned char r, g, b;
          pixel_rgb(map, &r, &g, &b);
          (void)b;
          offset.x *= 2.0f * (float)r / 255.0f - 1.0f;
          offset.y *= 2.0f * (float)g / 255.0f - 1.0f;
        }
        f->offset = offset;
      }
    }
  }

  return count;
}

static int ref_lookup(const RefCtx *ctx, int px, int py, float upper,
                      Vec3f *out);

static int ref_lookup(const RefCtx *ctx, int px, int py, float upper,
                      Vec3f *out) {
  RefFrag frags[MAX_FRAGS];
  int n = ref_layers_at(ctx, px, py, frags, MAX_FRAGS);

  int best = -1;
  for (int i = 0; i < n; i++) {
    if (!(frags[i].depth < upper))
      continue;
    if (best < 0 || frags[i].depth >= frags[best].depth)
      best = i;
  }

  if (best < 0) {
    *out = (Vec3f){0, 0, 0};
    return 0;
  }

  const RefFrag *layer = &frags[best];
  if (layer->opaque) {
    *out = layer->color;
    return 0;
  }

  float fx = (float)px + layer->offset.x;
  float fy = (float)py + layer->offset.y;
  float bx = floorf(fx);
  float by = floorf(fy);
  float wx = fx - bx;
  float wy = fy - by;
  int ix = (int)bx;
  int iy = (int)by;

  float weights[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy,
                      wx * wy};
  int tx[4] = {ix, ix + 1, ix, ix + 1};
  int ty[4] = {iy, iy, iy + 1, iy + 1};

  Vec3f behind = {0, 0, 0};
  for (int i = 0; i < 4; i++) {
    if (weights[i] == 0.0f)
      continue;
    Vec3f tap = {0, 0, 0};
    ref_lookup(ctx, tx[i], ty[i], layer->depth, &tap);
    behind.x += weights[i] * tap.x;
    behind.y += weights[i] * tap.y;
    behind.z += weights[i] * tap.z;
  }

  out->x = layer->opacity * layer->color.x +
           (1 - layer->opacity) * layer->tint.x * behind.x;
  out->y = layer->opacity * layer->color.y +
           (1 - layer->opacity) * layer->tint.y * behind.y;
  out->z = layer->opacity * layer->color.z +
           (1 - layer->opacity) * layer->tint.z * behind.z;
  return 0;
}

static unsigned char ref_quantize(float v) {
  if (!(v > 0.0f))
    return 0;
  if (v >= 1.0f)
    return 255;
  double s = floor(255.0 * (double)v + 0.5);
  if (s < 0)
    s = 0;
  if (s > 255)
    s = 255;
  return (unsigned char)s;
}

static uint32_t ref_encode_depth(float d) {
  double s = floor((double)d * (double)UINT32_MAX);
  if (s < 0)
    s = 0;
  if (s > (double)UINT32_MAX)
    s = (double)UINT32_MAX;
  return (uint32_t)s;
}

static void ref_resolve(const RefCtx *ctx, Pixel *color, PingoDepth *depth) {
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      Vec3f c = {0, 0, 0};
      ref_lookup(ctx, x, y, INFINITY, &c);
      color[x + y * WIDTH] =
          pixelFromRGBA(ref_quantize(c.x), ref_quantize(c.y),
                        ref_quantize(c.z), 255);

      RefFrag frags[MAX_FRAGS];
      int n = ref_layers_at(ctx, x, y, frags, MAX_FRAGS);
      int best = -1;
      for (int i = 0; i < n; i++) {
        if (best < 0 || frags[i].depth > frags[best].depth)
          best = i;
      }
      depth[x + y * WIDTH].d = best < 0 ? 0 : ref_encode_depth(frags[best].depth);
    }
  }
}

// ---------------------------------------------------------------------------
// Small assertion helpers.
// ---------------------------------------------------------------------------

static int near3(Vec3f a, Vec3f b, float eps) {
  return fabsf(a.x - b.x) <= eps && fabsf(a.y - b.y) <= eps &&
         fabsf(a.z - b.z) <= eps;
}

static int channel_close(unsigned char a, unsigned char b) {
  int d = (int)a - (int)b;
  return d >= -1 && d <= 1;
}

static int pixel_close(Pixel a, Pixel b) {
  unsigned char ar, ag, ab, br, bg, bb;
  pixel_rgb(a, &ar, &ag, &ab);
  pixel_rgb(b, &br, &bg, &bb);
  return channel_close(ar, br) && channel_close(ag, bg) &&
         channel_close(ab, bb);
}

static int check_frame_matches_reference(const DeepFrame *frame,
                                         const RefCtx *ctx, float eps) {
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      Vec3f want, got;
      ref_lookup(ctx, x, y, INFINITY, &want);
      if (deep_frame_sample(frame, x, y, INFINITY, &got) != 0)
        return 0;
      if (!near3(want, got, eps))
        return 0;
    }
  }
  return 1;
}

static int layer_color_at(const RefCtx *ctx, int li, int x, int y, Vec3f *out) {
  RefFrag frags[MAX_FRAGS];
  int n = ref_layers_at(ctx, x, y, frags, MAX_FRAGS);
  for (int i = 0; i < n; i++) {
    if (frags[i].layer_index == li) {
      *out = frags[i].color;
      return 1;
    }
  }
  return 0;
}

static int layer_frag_at(const RefCtx *ctx, int li, int x, int y, RefFrag *out) {
  RefFrag frags[MAX_FRAGS];
  int n = ref_layers_at(ctx, x, y, frags, MAX_FRAGS);
  for (int i = 0; i < n; i++) {
    if (frags[i].layer_index == li) {
      *out = frags[i];
      return 1;
    }
  }
  return 0;
}

static void ref_ctx_from_scene(RefCtx *ctx, TestScene *s) {
  ctx->layers = s->layers;
  ctx->count = s->count;
  ctx->projection = s->renderer.camera_projection;
  ctx->view = s->renderer.camera_view;
  ctx->view_inverse = mat4Inverse(&ctx->view);
}

// ---------------------------------------------------------------------------
// layers group
// ---------------------------------------------------------------------------

// A nearer opaque surface hides a transmissive layer and an opaque background.
// Deep capture must still hold both hidden layers, and an upper bound between
// them must select them.
static int case_layers_occluded(void) {
  static QuadLayer layers[3];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(40, 90, 200, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 3.5f, 3.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(220, 60, 30, 255)});
  layer_transmission(&layers[1], 0.5f, (Vec3f){1, 1, 1}, (Vec2f){0, 0}, 0);

  layer_reset(&layers[2]);
  layer_quad(&layers[2], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[2], 1, (Pixel[]){pixelFromRGBA(20, 220, 80, 255)});

  scene_begin(&scene, layers, 3);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  const int x = 8, y = 6;
  RefFrag back, middle, front;
  TASK_CHECK(layer_frag_at(&ctx, 0, x, y, &back));
  TASK_CHECK(layer_frag_at(&ctx, 1, x, y, &middle));
  TASK_CHECK(layer_frag_at(&ctx, 2, x, y, &front));

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);
  TASK_CHECK(frame != 0);
  TASK_CHECK(scene.backend.before_calls == 0);
  TASK_CHECK(scene.backend.after_calls == 0);

  Vec3f got;
  TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
  TASK_CHECK(near3(got, front.color, 1e-4f));

  // Strictly between the middle and front layers: the front is excluded.
  const float between = (middle.depth + front.depth) * 0.5f;
  TASK_CHECK(deep_frame_sample(frame, x, y, between, &got) == 0);
  Vec3f want_middle = {0.5f * middle.color.x + 0.5f * back.color.x,
                       0.5f * middle.color.y + 0.5f * back.color.y,
                       0.5f * middle.color.z + 0.5f * back.color.z};
  TASK_CHECK(near3(got, want_middle, 1e-4f));

  // Exactly at the middle depth excludes it as well (d < upper is strict).
  TASK_CHECK(deep_frame_sample(frame, x, y, middle.depth, &got) == 0);
  TASK_CHECK(near3(got, back.color, 1e-4f));

  // The nearest image is the front opaque layer, and it is not what the
  // transmission sample asks for.
  Texture target;
  Pixel out[PIXEL_COUNT];
  PingoDepth depth[PIXEL_COUNT];
  memset(out, 0, sizeof(out));
  memset(depth, 0xff, sizeof(depth));
  texture_init(&target, (Vec2i){WIDTH, HEIGHT}, out);
  TASK_CHECK(deep_frame_resolve(frame, &target, depth) == 0);
  Vec3f nearest = linear_from_pixel(out[x + y * WIDTH]);
  TASK_CHECK(near3(nearest, front.color, 2.0f / 255.0f));
  TASK_CHECK(!near3(nearest, want_middle, 0.05f));

  // The resolved depth is the front-most captured layer.
  TASK_CHECK(depth[x + y * WIDTH].d == ref_encode_depth(front.depth));

  // A second resolve does not accumulate.
  Pixel out2[PIXEL_COUNT];
  PingoDepth depth2[PIXEL_COUNT];
  memset(out2, 0x7f, sizeof(out2));
  memset(depth2, 0, sizeof(depth2));
  Texture target2;
  texture_init(&target2, (Vec2i){WIDTH, HEIGHT}, out2);
  TASK_CHECK(deep_frame_resolve(frame, &target2, depth2) == 0);
  TASK_CHECK(memcmp(out, out2, sizeof(out)) == 0);
  TASK_CHECK(memcmp(depth, depth2, sizeof(depth)) == 0);

  // The whole frame agrees with the independent reference.
  TASK_CHECK(check_frame_matches_reference(frame, &ctx, 1e-4f));

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// Two surfaces at exactly the same depth: the last one submitted is selected.
static int case_layers_equal_depth(void) {
  static QuadLayer layers_a[2];
  static QuadLayer layers_b[2];
  TestScene scene;

  for (int pass = 0; pass < 2; pass++) {
    QuadLayer *layers = pass == 0 ? layers_a : layers_b;
    Pixel color_first = pixelFromRGBA(200, 30, 30, 255);
    Pixel color_second = pixelFromRGBA(30, 30, 200, 255);
    if (pass == 1) {
      Pixel tmp = color_first;
      color_first = color_second;
      color_second = tmp;
    }

    layer_reset(&layers[0]);
    layer_quad(&layers[0], 3.5f, 3.5f, 4.5f, 3.5f);
    layer_color(&layers[0], 1, &color_first);

    layer_reset(&layers[1]);
    layer_quad(&layers[1], 3.5f, 3.5f, 4.5f, 3.5f);
    layer_color(&layers[1], 1, &color_second);

    scene_begin(&scene, layers, 2);
    RefCtx ctx;
    ref_ctx_from_scene(&ctx, &scene);

    DeepFrame *frame = 0;
    TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);

    Vec3f want;
    TASK_CHECK(ref_lookup(&ctx, 9, 5, INFINITY, &want) == 0);
    Vec3f got;
    TASK_CHECK(deep_frame_sample(frame, 9, 5, INFINITY, &got) == 0);
    TASK_CHECK(near3(got, want, 1e-5f));

    // Both layers have identical geometry, so the last one submitted must win.
    Vec3f second, first;
    TASK_CHECK(layer_color_at(&ctx, 1, 9, 5, &second));
    TASK_CHECK(layer_color_at(&ctx, 0, 9, 5, &first));
    TASK_CHECK(near3(got, second, 1e-4f));
    TASK_CHECK(!near3(got, first, 0.05f));

    TASK_CHECK(check_frame_matches_reference(frame, &ctx, 1e-4f));

    deep_frame_destroy(frame);
    scene_end(&scene);
  }
  return 0;
}

// Two transmissive layers over an opaque background: the chain has to be
// followed through, not truncated after one step.
static int case_layers_chain(void) {
  static QuadLayer layers[3];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(30, 160, 210, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 3.5f, 3.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(210, 120, 40, 255)});
  layer_transmission(&layers[1], 0.4f, (Vec3f){1, 0.5f, 0.5f}, (Vec2f){0, 0},
                     0);

  layer_reset(&layers[2]);
  layer_quad(&layers[2], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[2], 1, (Pixel[]){pixelFromRGBA(90, 40, 200, 255)});
  layer_transmission(&layers[2], 0.6f, (Vec3f){0.5f, 0.5f, 1}, (Vec2f){0, 0},
                     0);

  scene_begin(&scene, layers, 3);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);
  TASK_CHECK(check_frame_matches_reference(frame, &ctx, 1e-4f));

  const int x = 11, y = 7;
  Vec3f got, want;
  TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
  TASK_CHECK(ref_lookup(&ctx, x, y, INFINITY, &want) == 0);
  TASK_CHECK(near3(got, want, 1e-4f));

  // It is neither the front local colour nor a one-step blend with the
  // middle's local colour alone.
  Vec3f front, middle;
  TASK_CHECK(layer_color_at(&ctx, 2, x, y, &front));
  TASK_CHECK(layer_color_at(&ctx, 1, x, y, &middle));
  TASK_CHECK(!near3(got, front, 0.01f));
  Vec3f one_step = {0.6f * front.x + 0.4f * 0.5f * middle.x,
                    0.6f * front.y + 0.4f * 0.5f * middle.y,
                    0.6f * front.z + 0.4f * 0.5f * middle.z};
  TASK_CHECK(!near3(got, one_step, 0.01f));

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

// A high-frequency random texture, so the bilinear blend of four taps
// cannot coincide with any single texel.
#define NOISE_SIZE 32
static Pixel transport_noise[NOISE_SIZE * NOISE_SIZE];

static void build_transport_noise(void) {
  unsigned seed = 12345u;
  for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++) {
    // Small deterministic LCG: keeps the test independent of rand()'s
    // implementation-defined sequence.
    seed = seed * 1103515245u + 12345u;
    unsigned char r = (unsigned char)((seed >> 16) & 0xff);
    seed = seed * 1103515245u + 12345u;
    unsigned char g = (unsigned char)((seed >> 16) & 0xff);
    seed = seed * 1103515245u + 12345u;
    unsigned char b = (unsigned char)((seed >> 16) & 0xff);
    transport_noise[i] = pixelFromRGBA(r, g, b, 255);
  }
}

// Two transmissive layers with integer offsets so the bilinear weights are
// 0.5 in each direction: each of the four taps contributes equally.  The
// result must be the average of four distinct local colours.
static int case_transport_offset_bilinear(void) {
  static QuadLayer layers[2];
  TestScene scene;

  build_transport_noise();

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], NOISE_SIZE, transport_noise);

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(255, 255, 255, 255)});
  layer_transmission(&layers[1], 0.0f, (Vec3f){1, 1, 1}, (Vec2f){0.5f, 0.5f},
                     0);

  scene_begin(&scene, layers, 2);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);

  const int x = 9, y = 7;
  Vec3f got, want;
  TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
  TASK_CHECK(ref_lookup(&ctx, x, y, INFINITY, &want) == 0);
  TASK_CHECK(near3(got, want, 1e-4f));

  // Floor is x, ceil is x+1, and both axes have weight 0.5, so the bilinear
  // answer is the average of the four tap colours.
  Vec3f c00, c10, c01, c11;
  TASK_CHECK(layer_color_at(&ctx, 0, x, y, &c00));
  TASK_CHECK(layer_color_at(&ctx, 0, x + 1, y, &c10));
  TASK_CHECK(layer_color_at(&ctx, 0, x, y + 1, &c01));
  TASK_CHECK(layer_color_at(&ctx, 0, x + 1, y + 1, &c11));
  Vec3f average = {0.25f * (c00.x + c10.x + c01.x + c11.x),
                   0.25f * (c00.y + c10.y + c01.y + c11.y),
                   0.25f * (c00.z + c10.z + c01.z + c11.z)};
  TASK_CHECK(near3(got, average, 1e-4f));

  // Four taps, so the average is a genuine blend and not any single colour.
  // Adjacent screen pixels sample nearby texture cells, and the noise texture
  // makes their colours distinct.
  Vec3f p00 = c00, p10 = c10, p01 = c01, p11 = c11;
  TASK_CHECK(!near3(p00, p10, 0.01f) || !near3(p00, p01, 0.01f) ||
             !near3(p10, p11, 0.01f) || !near3(p01, p11, 0.01f));
  TASK_CHECK(!near3(got, c00, 0.01f));
  TASK_CHECK(!near3(got, c11, 0.01f));

  // Also check a handful of other pixels against the reference.
  for (int i = 0; i < 6; i++) {
    int px = 2 + i * 3;
    int py = 3 + (i * 5) % (HEIGHT - 6);
    TASK_CHECK(deep_frame_sample(frame, px, py, INFINITY, &got) == 0);
    TASK_CHECK(ref_lookup(&ctx, px, py, INFINITY, &want) == 0);
    TASK_CHECK(near3(got, want, 1e-4f));
  }

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// At the offset pixel there is a surface nearer than the current layer; it
// must be skipped in favour of the one genuinely behind the current layer.
// The occluder only covers the right-hand side of the screen, so the sampling
// pixel is in front of the transmissive layer while its offset tap lands under
// the occluder.
static int case_transport_depth_filtered_taps(void) {
  static QuadLayer layers[3];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(30, 30, 220, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 3.5f, 3.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(255, 255, 255, 255)});
  layer_transmission(&layers[1], 0.0f, (Vec3f){1, 1, 1}, (Vec2f){4.0f, 0.0f},
                     0);

  // Opaque occluder at z = -2.5, covering roughly screen x >= 16 only.
  layer_reset(&layers[2]);
  layer_quad(&layers[2], 2.5f, 2.5f, 1.5f, 1.8f);
  layer_translate(&layers[2], 2.0f, 0.0f, 0.0f);
  layer_color(&layers[2], 1, (Pixel[]){pixelFromRGBA(240, 40, 40, 255)});

  scene_begin(&scene, layers, 3);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);
  TASK_CHECK(check_frame_matches_reference(frame, &ctx, 1e-4f));

  // (12, 8) is outside the occluder, so the transmissive layer is front-most.
  const int x = 12, y = 8;
  const int tap_x = x + 4;
  Vec3f got, want, background, occluder;
  TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
  TASK_CHECK(ref_lookup(&ctx, x, y, INFINITY, &want) == 0);
  TASK_CHECK(near3(got, want, 1e-4f));

  // The tap is covered by the occluder and by the background.
  TASK_CHECK(layer_color_at(&ctx, 2, tap_x, y, &occluder));
  TASK_CHECK(layer_color_at(&ctx, 0, tap_x, y, &background));
  // Opacity 0, so the sample is exactly what the tap returned.  The occluder
  // is nearer than the sampling layer and must be filtered out.
  TASK_CHECK(near3(got, background, 1e-4f));
  TASK_CHECK(!near3(got, occluder, 0.05f));

  // From the sampling layer's own pixel the occluder is absent...
  RefFrag middle;
  TASK_CHECK(layer_frag_at(&ctx, 1, x, y, &middle));
  TASK_CHECK(layer_frag_at(&ctx, 2, x, y, &middle) == 0);

  // ... but it was still captured behind the transmissive layer: asking past
  // the transmissive depth at the tap pixel selects it.
  Vec3f at_tap;
  TASK_CHECK(deep_frame_sample(frame, tap_x, y, INFINITY, &at_tap) == 0);
  TASK_CHECK(near3(at_tap, occluder, 1e-4f));

  RefFrag sample_layer;
  TASK_CHECK(layer_frag_at(&ctx, 1, tap_x, y, &sample_layer));
  Vec3f below;
  TASK_CHECK(deep_frame_sample(frame, tap_x, y, sample_layer.depth, &below) ==
             0);
  TASK_CHECK(near3(below, background, 1e-4f));

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// A varying-w surface with an offset map: the offset multiplier is read with
// the perspective-correct UV, and differs across the screen.
static int case_transport_offset_map_uv(void) {
  static QuadLayer layers[2];
  TestScene scene;

  Pixel map[16];
  for (int i = 0; i < 16; i++) {
    const int x = i % 4, y = i / 4;
    map[i] = pixelFromRGBA((unsigned char)(20 * x + 10),
                           (unsigned char)(30 * y + 15), 0, 255);
  }

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  // A busy background so that different offsets land on visibly different
  // colours.
  build_transport_noise();
  layer_color(&layers[0], NOISE_SIZE, transport_noise);

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 2.5f, 4.5f, 4.5f, 3.5f); // tilted: w varies
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(255, 255, 255, 255)});
  layer_offset_map(&layers[1], 4, map);
  layer_transmission(&layers[1], 0.0f, (Vec3f){1, 1, 1}, (Vec2f){2.0f, 2.0f},
                     1);

  scene_begin(&scene, layers, 2);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);

  // Perspective UV must differ from naive affine UV at some pixel, otherwise
  // varying w is not being exercised at all.
  int perspective_differs = 0;
  for (int y = 1; y < HEIGHT - 1 && !perspective_differs; y++) {
    for (int x = 1; x < WIDTH - 1; x++) {
      RefFrag f;
      if (!layer_frag_at(&ctx, 1, x, y, &f))
        continue;
      if (fabsf(f.uv.x - f.uv_affine.x) > 1e-3f ||
          fabsf(f.uv.y - f.uv_affine.y) > 1e-3f) {
        perspective_differs = 1;
        break;
      }
    }
  }
  TASK_CHECK(perspective_differs);

  Vec3f first = {0, 0, 0};
  int have_first = 0, different_offsets = 0;
  for (int i = 0; i < 10; i++) {
    int x = 1 + (i * 5) % (WIDTH - 2);
    int y = 1 + (i * 7) % (HEIGHT - 2);
    RefFrag f;
    if (!layer_frag_at(&ctx, 1, x, y, &f))
      continue;

    Vec3f got, want;
    TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
    TASK_CHECK(ref_lookup(&ctx, x, y, INFINITY, &want) == 0);
    TASK_CHECK(near3(got, want, 1e-4f));

    if (!have_first) {
      first = want;
      have_first = 1;
    } else if (!near3(first, want, 1e-4f)) {
      different_offsets = 1;
    }
  }
  TASK_CHECK(have_first);
  TASK_CHECK(different_offsets);

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// The transmission answer must not accidentally equal the nearest visible
// colour; this is the "do not just read the finished image" distinction.
static int case_transport_not_nearest_image(void) {
  static QuadLayer layers[2];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(10, 200, 240, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(240, 200, 10, 255)});
  layer_transmission(&layers[1], 0.25f, (Vec3f){0.5f, 0.5f, 1}, (Vec2f){1, 1},
                     0);

  scene_begin(&scene, layers, 2);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);

  const int x = 10, y = 7;
  Vec3f got, want, front, behind;
  TASK_CHECK(deep_frame_sample(frame, x, y, INFINITY, &got) == 0);
  TASK_CHECK(ref_lookup(&ctx, x, y, INFINITY, &want) == 0);
  TASK_CHECK(near3(got, want, 1e-4f));

  TASK_CHECK(layer_color_at(&ctx, 1, x, y, &front));
  TASK_CHECK(layer_color_at(&ctx, 0, x, y, &behind));
  TASK_CHECK(!near3(got, front, 0.02f));
  TASK_CHECK(!near3(got, behind, 0.02f));

  // Explicitly reading the nearest image (the resolved frame) is not allowed
  // to reproduce the transmission sample here.
  Texture target;
  Pixel resolved[PIXEL_COUNT];
  PingoDepth depth[PIXEL_COUNT];
  memset(resolved, 0, sizeof(resolved));
  texture_init(&target, (Vec2i){WIDTH, HEIGHT}, resolved);
  TASK_CHECK(deep_frame_resolve(frame, &target, depth) == 0);
  TASK_CHECK(near3(linear_from_pixel(resolved[x + y * WIDTH]), front, 0.01f) ==
             0);

  deep_frame_destroy(frame);
  scene_end(&scene);
  return 0;
}

// ---------------------------------------------------------------------------
// lifetime group
// ---------------------------------------------------------------------------

// The frame owns everything it needs: after the source meshes, materials,
// description, textures, entity and renderer are gone, sampling and resolving
// still produce the same result.
static int case_lifetime_survives_destroy(void) {
  static QuadLayer layers[2];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(35, 180, 70, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(200, 60, 160, 255)});
  layer_transmission(&layers[1], 0.45f, (Vec3f){0.8f, 0.9f, 1},
                     (Vec2f){1.5f, -0.5f}, 0);

  // Put one layer's vertex array on the heap so it can genuinely be freed.
  Vec3f *heap_positions = malloc(sizeof(Vec3f) * 4);
  TASK_CHECK(heap_positions != 0);
  memcpy(heap_positions, layers[0].positions, sizeof(Vec3f) * 4);
  layers[0].mesh.positions = heap_positions;

  scene_begin(&scene, layers, 2);
  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);

  Vec3f want_samples[4];
  const int sx[4] = {3, 8, 13, 18};
  const int sy[4] = {4, 9, 6, 12};
  for (int i = 0; i < 4; i++)
    TASK_CHECK(ref_lookup(&ctx, sx[i], sy[i], INFINITY, &want_samples[i]) == 0);
  Pixel want_image[PIXEL_COUNT];
  PingoDepth want_depth[PIXEL_COUNT];
  ref_resolve(&ctx, want_image, want_depth);

  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);
  TASK_CHECK(frame != 0);

  // Release and clobber every source.
  free(heap_positions);
  layers[0].mesh.positions = 0;

  // These are the test's own allocations, not part of the frame; save them
  // before the structs are overwritten so they can still be released.
  Pixel *backend_frame = scene.backend.frame;
  PingoDepth *backend_depth = scene.backend.depth;

  memset(layers, 0x5a, sizeof(layers));
  memset(&scene.root, 0x5a, sizeof(scene.root));
  memset(&scene.renderer, 0x5a, sizeof(scene.renderer));
  memset(&scene.backend, 0x5a, sizeof(scene.backend));

  for (int i = 0; i < 4; i++) {
    Vec3f got;
    TASK_CHECK(deep_frame_sample(frame, sx[i], sy[i], INFINITY, &got) == 0);
    TASK_CHECK(near3(got, want_samples[i], 1e-5f));
  }

  Pixel got_image[PIXEL_COUNT];
  PingoDepth got_depth[PIXEL_COUNT];
  memset(got_image, 0, sizeof(got_image));
  memset(got_depth, 0, sizeof(got_depth));
  Texture target;
  texture_init(&target, (Vec2i){WIDTH, HEIGHT}, got_image);
  TASK_CHECK(deep_frame_resolve(frame, &target, got_depth) == 0);

  int image_ok = 1;
  for (int i = 0; i < PIXEL_COUNT; i++) {
    if (!pixel_close(got_image[i], want_image[i]))
      image_ok = 0;
    if (got_depth[i].d != want_depth[i].d)
      image_ok = 0;
  }
  TASK_CHECK(image_ok);

  // Resolving again is stable.
  Pixel again[PIXEL_COUNT];
  PingoDepth again_depth[PIXEL_COUNT];
  memset(again, 1, sizeof(again));
  Texture target2;
  texture_init(&target2, (Vec2i){WIDTH, HEIGHT}, again);
  TASK_CHECK(deep_frame_resolve(frame, &target2, again_depth) == 0);
  TASK_CHECK(memcmp(got_image, again, sizeof(got_image)) == 0);
  TASK_CHECK(memcmp(got_depth, again_depth, sizeof(got_depth)) == 0);

  deep_frame_destroy(frame);
  // Backend storage was clobbered above; only the plain buffers are real.
  free(backend_frame);
  free(backend_depth);
  return 0;
}

// renderer_render_deep must follow beforeRender's buffer choice and publish a
// fully resolved frame there.
static int case_lifetime_backend_swap(void) {
  static QuadLayer layers[2];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(15, 70, 210, 255)});

  layer_reset(&layers[1]);
  layer_quad(&layers[1], 2.5f, 2.5f, 4.5f, 3.5f);
  layer_color(&layers[1], 1, (Pixel[]){pixelFromRGBA(220, 180, 20, 255)});
  layer_transmission(&layers[1], 0.5f, (Vec3f){1, 1, 1}, (Vec2f){1, 0}, 0);

  // Build with an alternate buffer and a swap in beforeRender.
  test_backend_init(&scene.backend, (Vec2i){WIDTH, HEIGHT}, 1);
  scene.backend.swap_on_before = 1;
  renderer_init(&scene.renderer, (Vec2i){WIDTH, HEIGHT},
                (Backend *)&scene.backend);

  memset(&scene.empty_mesh, 0, sizeof(scene.empty_mesh));
  object_init(&scene.empty_object, &scene.empty_mesh, 0);
  for (int i = 0; i < 2; i++) {
    object_init(&layers[i].object, &layers[i].mesh, &layers[i].material);
    entity_init(&scene.entities[i], (Renderable *)&layers[i].object,
                mat4Identity());
  }
  entity_init_children(&scene.root, (Renderable *)&scene.empty_object,
                       mat4Identity(), scene.entities, 2);
  renderer_set_root_renderable(&scene.renderer, (Renderable *)&scene.root);
  scene.renderer.camera_projection =
      mat4Perspective(1, 50, (float)WIDTH / (float)HEIGHT, 0.6f);
  scene.renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
  scene.layers = layers;
  scene.count = 2;

  RefCtx ctx;
  ref_ctx_from_scene(&ctx, &scene);
  Pixel want[PIXEL_COUNT];
  PingoDepth want_depth[PIXEL_COUNT];
  ref_resolve(&ctx, want, want_depth);

  for (int i = 0; i < PIXEL_COUNT; i++) {
    scene.backend.frame[i] = pixelFromRGBA(1, 2, 3, 255);
    scene.backend.alt_frame[i] = pixelFromRGBA(4, 5, 6, 255);
  }

  TASK_CHECK(renderer_render_deep(&scene.renderer) == 0);
  TASK_CHECK(scene.backend.before_calls == 1);
  TASK_CHECK(scene.backend.after_calls == 1);
  TASK_CHECK(scene.backend.use_alt == 1);

  int image_ok = 1;
  for (int i = 0; i < PIXEL_COUNT; i++) {
    if (!pixel_close(scene.backend.alt_frame[i], want[i]))
      image_ok = 0;
    if (scene.backend.alt_depth[i].d != want_depth[i].d)
      image_ok = 0;
  }
  TASK_CHECK(image_ok);
  // The original buffer was not published to.
  unsigned char r, g, b;
  pixel_rgb(scene.backend.frame[0], &r, &g, &b);
  TASK_CHECK(r == 1 && g == 2 && b == 3);

  // A repeat works and does not double-count.
  TASK_CHECK(renderer_render_deep(&scene.renderer) == 0);
  TASK_CHECK(scene.backend.before_calls == 2);
  TASK_CHECK(scene.backend.after_calls == 2);

  scene_end(&scene);
  return 0;
}

// Failures leave buffers, output pointers and callbacks untouched.
static int case_lifetime_failure_not_published(void) {
  static QuadLayer layers[1];
  TestScene scene;

  layer_reset(&layers[0]);
  layer_quad(&layers[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers[0], 1, (Pixel[]){pixelFromRGBA(60, 120, 180, 255)});
  scene_begin(&scene, layers, 1);

  // clear must be true.
  scene.renderer.clear = 0;
  for (int i = 0; i < PIXEL_COUNT; i++)
    scene.backend.frame[i] = pixelFromRGBA(9, 8, 7, 255);
  TASK_CHECK(renderer_render_deep(&scene.renderer) != 0);
  TASK_CHECK(scene.backend.before_calls == 0);
  TASK_CHECK(scene.backend.after_calls == 0);
  unsigned char r, g, b;
  pixel_rgb(scene.backend.frame[0], &r, &g, &b);
  TASK_CHECK(r == 9 && g == 8 && b == 7);

  scene.renderer.clear = 1;

  // A capture with no root leaves *out alone.
  Renderable *saved_root = scene.renderer.root_renderable;
  scene.renderer.root_renderable = 0;
  DeepFrame *sentinel = (DeepFrame *)&layers[0];
  DeepFrame *out = sentinel;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &out) != 0);
  TASK_CHECK(out == sentinel);
  TASK_CHECK(renderer_render_deep(&scene.renderer) != 0);
  TASK_CHECK(scene.backend.before_calls == 0);
  TASK_CHECK(scene.backend.after_calls == 0);
  scene.renderer.root_renderable = saved_root;

  // Failure from the backend after beforeRender: nothing is published and no
  // afterRender is called.
  scene.backend.fail_get = 1;
  TASK_CHECK(renderer_render_deep(&scene.renderer) != 0);
  TASK_CHECK(scene.backend.before_calls == 1);
  TASK_CHECK(scene.backend.after_calls == 0);
  pixel_rgb(scene.backend.frame[0], &r, &g, &b);
  TASK_CHECK(r == 9 && g == 8 && b == 7);
  scene.backend.fail_get = 0;

  // Resolve with a mismatched target changes neither output.
  DeepFrame *frame = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame) == 0);

  Pixel small[8 * 8];
  PingoDepth small_depth[8 * 8];
  memset(small, 0x33, sizeof(small));
  memset(small_depth, 0x44, sizeof(small_depth));
  Pixel small_before[8 * 8];
  PingoDepth small_depth_before[8 * 8];
  memcpy(small_before, small, sizeof(small));
  memcpy(small_depth_before, small_depth, sizeof(small_depth));

  Texture small_target;
  texture_init(&small_target, (Vec2i){8, 8}, small);
  TASK_CHECK(deep_frame_resolve(frame, &small_target, small_depth) != 0);
  TASK_CHECK(memcmp(small, small_before, sizeof(small)) == 0);
  TASK_CHECK(memcmp(small_depth, small_depth_before, sizeof(small_depth)) == 0);

  // Invalid bounds leave the output untouched.
  Vec3f untouched = {0.125f, 0.25f, 0.5f};
  Vec3f sample = untouched;
  TASK_CHECK(deep_frame_sample(frame, 4, 4, NAN, &sample) != 0);
  TASK_CHECK(near3(sample, untouched, 0.0f));
  TASK_CHECK(deep_frame_sample(frame, 4, 4, 0.0f, &sample) != 0);
  TASK_CHECK(near3(sample, untouched, 0.0f));
  TASK_CHECK(deep_frame_sample(frame, 4, 4, -INFINITY, &sample) != 0);
  TASK_CHECK(near3(sample, untouched, 0.0f));

  // Capture reads nothing from the backend.
  const int before = scene.backend.before_calls;
  DeepFrame *frame2 = 0;
  TASK_CHECK(renderer_capture_deep(&scene.renderer, &frame2) == 0);
  TASK_CHECK(scene.backend.before_calls == before);
  TASK_CHECK(scene.backend.after_calls == 0);

  deep_frame_destroy(frame);
  deep_frame_destroy(frame2);
  deep_frame_destroy(0);
  scene_end(&scene);
  return 0;
}

// Two renderers used alternately keep their own frames and buffers.
static int case_lifetime_interleaved_renderers(void) {
  static QuadLayer layers_a[1];
  static QuadLayer layers_b[1];
  TestScene a, b;

  layer_reset(&layers_a[0]);
  layer_quad(&layers_a[0], 5.0f, 5.0f, 4.5f, 3.5f);
  layer_color(&layers_a[0], 1, (Pixel[]){pixelFromRGBA(30, 200, 40, 255)});

  layer_reset(&layers_b[0]);
  layer_quad(&layers_b[0], 3.0f, 3.0f, 4.5f, 3.5f);
  layer_color(&layers_b[0], 1, (Pixel[]){pixelFromRGBA(200, 30, 40, 255)});

  scene_begin(&a, layers_a, 1);
  scene_begin(&b, layers_b, 1);

  DeepFrame *fa = 0, *fb = 0;
  TASK_CHECK(renderer_capture_deep(&a.renderer, &fa) == 0);
  TASK_CHECK(renderer_capture_deep(&b.renderer, &fb) == 0);

  Vec3f va, vb, want_a, want_b;
  TASK_CHECK(deep_frame_sample(fa, 6, 5, INFINITY, &va) == 0);
  TASK_CHECK(deep_frame_sample(fb, 6, 5, INFINITY, &vb) == 0);

  RefCtx ca, cb;
  ref_ctx_from_scene(&ca, &a);
  ref_ctx_from_scene(&cb, &b);
  TASK_CHECK(ref_lookup(&ca, 6, 5, INFINITY, &want_a) == 0);
  TASK_CHECK(ref_lookup(&cb, 6, 5, INFINITY, &want_b) == 0);
  TASK_CHECK(near3(va, want_a, 1e-4f));
  TASK_CHECK(near3(vb, want_b, 1e-4f));
  TASK_CHECK(!near3(va, vb, 0.1f));

  TASK_CHECK(renderer_render_deep(&a.renderer) == 0);
  TASK_CHECK(renderer_render_deep(&b.renderer) == 0);
  TASK_CHECK(renderer_render_deep(&a.renderer) == 0);

  // Frames remain valid after the renders.
  Vec3f again;
  TASK_CHECK(deep_frame_sample(fa, 6, 5, INFINITY, &again) == 0);
  TASK_CHECK(near3(again, want_a, 1e-5f));

  deep_frame_destroy(fa);
  deep_frame_destroy(fb);
  scene_end(&a);
  scene_end(&b);
  return 0;
}

// material_set_transmission validation and default binding.
static int case_lifetime_material_binding(void) {
  Texture color, offset_map;
  Pixel color_texel = pixelFromRGBA(255, 255, 255, 255);
  Pixel map_texel = pixelFromRGBA(128, 128, 128, 255);
  texture_init(&color, (Vec2i){1, 1}, &color_texel);
  texture_init(&offset_map, (Vec2i){1, 1}, &map_texel);

  Material material;
  TASK_CHECK(material_init(&material, &color) == 0);
  TASK_CHECK(material.transmission == 0);

  ScreenTransmission good;
  good.opacity = 0.5f;
  good.tint = (Vec3f){1, 1, 1};
  good.offset = (Vec2f){1.5f, -2.0f};
  good.offset_map = &offset_map;
  TASK_CHECK(material_set_transmission(&material, &good) == 0);
  TASK_CHECK(material.transmission == &good);

  ScreenTransmission bad = good;
  bad.opacity = 1.5f;
  TASK_CHECK(material_set_transmission(&material, &bad) != 0);
  TASK_CHECK(material.transmission == &good);

  bad = good;
  bad.tint = (Vec3f){0.5f, -0.1f, 0.5f};
  TASK_CHECK(material_set_transmission(&material, &bad) != 0);
  TASK_CHECK(material.transmission == &good);

  bad = good;
  bad.offset = (Vec2f){NAN, 0.0f};
  TASK_CHECK(material_set_transmission(&material, &bad) != 0);
  TASK_CHECK(material.transmission == &good);

  bad = good;
  bad.offset = (Vec2f){INFINITY, 0.0f};
  TASK_CHECK(material_set_transmission(&material, &bad) != 0);
  TASK_CHECK(material.transmission == &good);

  bad = good;
  Texture broken;
  memset(&broken, 0, sizeof(broken));
  bad.offset_map = &broken;
  TASK_CHECK(material_set_transmission(&material, &bad) != 0);
  TASK_CHECK(material.transmission == &good);

  bad = good;
  bad.offset_map = 0; // no map is valid
  TASK_CHECK(material_set_transmission(&material, &bad) == 0);
  TASK_CHECK(material.transmission == &bad);

  TASK_CHECK(material_set_transmission(&material, 0) == 0);
  TASK_CHECK(material.transmission == 0);

  TASK_CHECK(material_set_transmission(0, &good) != 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
    {"layers", "occluded_layers", case_layers_occluded},
    {"layers", "equal_depth_tie", case_layers_equal_depth},
    {"layers", "transmission_chain", case_layers_chain},
    {"transport", "offset_bilinear", case_transport_offset_bilinear},
    {"transport", "depth_filtered_taps", case_transport_depth_filtered_taps},
    {"transport", "offset_map_uv", case_transport_offset_map_uv},
    {"transport", "not_nearest_image", case_transport_not_nearest_image},
    {"lifetime", "survives_destroy", case_lifetime_survives_destroy},
    {"lifetime", "backend_buffer_swap", case_lifetime_backend_swap},
    {"lifetime", "failure_not_published", case_lifetime_failure_not_published},
    {"lifetime", "interleaved_renderers", case_lifetime_interleaved_renderers},
    {"lifetime", "material_binding", case_lifetime_material_binding},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
