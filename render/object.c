#include "object.h"
#include "backend.h"
#include "deep_internal.h"
#include "depth.h"
#include "math/fun.h"
#include "math/mat4.h"
#include "mesh.h"
#include "render/material.h"
#include "renderer.h"
#include "state.h"

// Frustum culling helper function
static int frustum_cull_triangle(Vec4f a, Vec4f b, Vec4f c) {
  // Simple frustum culling - check if all vertices are outside any plane

  // Near plane (z > w)
  if (a.z > a.w && b.z > b.w && c.z > c.w)
    return 1;

  // Far plane (z < -w)
  if (a.z < -a.w && b.z < -b.w && c.z < -c.w)
    return 1;

  // Left plane (x < -w)
  if (a.x < -a.w && b.x < -b.w && c.x < -c.w)
    return 1;

  // Right plane (x > w)
  if (a.x > a.w && b.x > b.w && c.x > c.w)
    return 1;

  // Top plane (y > w)
  if (a.y > a.w && b.y > b.w && c.y > c.w)
    return 1;

  // Bottom plane (y < -w)
  if (a.y < -a.w && b.y < -b.w && c.y < -c.w)
    return 1;

  return 0; // Not culled
}

// Output of one triangle's journey from mesh space to screen space, shared by
// the ordinary raster path and the deep capture so both agree on the
// transform, coverage and perspective UV conventions.
typedef struct {
  Vec4f a, b, c; // clip space
  float aw, bw, cw;
  Vec2f tca, tcb, tcc; // perspective-divided if uv is set
  float invAw, invBw, invCw;
  int has_uv;
  Vec2i a_s, b_s, c_s;
  int32_t minX, minY, maxX, maxY;
  int32_t area;
  int32_t A01, B01, A12, B12, A20, B20;
  int32_t w0_row, w1_row, w2_row;
  float diffuse;
} ObjectTriangle;

static int object_build_triangle(Object *o, Mat4 m, Renderer *r, int i,
                                 ObjectTriangle *out) {
  const Vec2i scrSize = r->framebuffer.size;

  // VIEW MATRIX
  Mat4 v = mat4Inverse(&r->camera_view);
  Mat4 p = r->camera_projection;

  const Vec3f *ver1 = &o->mesh->positions[o->mesh->pos_indices[i + 0]];
  const Vec3f *ver2 = &o->mesh->positions[o->mesh->pos_indices[i + 1]];
  const Vec3f *ver3 = &o->mesh->positions[o->mesh->pos_indices[i + 2]];

  Vec2f tca = {0, 0};
  Vec2f tcb = {0, 0};
  Vec2f tcc = {0, 0};

  // A mesh need not carry texture coordinates - teapot and pingo do not.
  // Checking only the material dereferenced NULL for those, so every such
  // mesh crashed the renderer instead of drawing untextured.
  if (o->material != 0 && o->mesh->textCoord != 0 &&
      o->mesh->tex_indices != 0) {
    tca = o->mesh->textCoord[o->mesh->tex_indices[i + 0]];
    tcb = o->mesh->textCoord[o->mesh->tex_indices[i + 1]];
    tcc = o->mesh->textCoord[o->mesh->tex_indices[i + 2]];
  }

  Vec4f a = {ver1->x, ver1->y, ver1->z, 1};
  Vec4f b = {ver2->x, ver2->y, ver2->z, 1};
  Vec4f c = {ver3->x, ver3->y, ver3->z, 1};

  Mat4 vm = mat4MultiplyM(&v, &m);

  a = mat4MultiplyVec4(&a, &vm);
  b = mat4MultiplyVec4(&b, &vm);
  c = mat4MultiplyVec4(&c, &vm);

  // Calc Face Normal
  // Built member-wise on purpose: casting &a from Vec4f* to Vec3f* and
  // dereferencing it violates strict aliasing, which -O2 and above are
  // entitled to act on.
  Vec3f a3 = {a.x, a.y, a.z};
  Vec3f b3 = {b.x, b.y, b.z};
  Vec3f c3 = {c.x, c.y, c.z};
  Vec3f na = vec3fsubV(a3, b3);
  Vec3f nb = vec3fsubV(a3, c3);
  Vec3f normal = vec3Normalize(vec3Cross(na, nb));
  Vec3f light = vec3Normalize((Vec3f){-8, 5, 5});
  float diffuseLight = (1.0 + vec3Dot(normal, light)) * 0.5;
  diffuseLight = MIN(1.0, MAX(diffuseLight, 0));

  a = mat4MultiplyVec4(&a, &p);
  b = mat4MultiplyVec4(&b, &p);
  c = mat4MultiplyVec4(&c, &p);

  // Frustum culling (configurable)
  if (r->enable_frustum_culling && frustum_cull_triangle(a, b, c))
    return 0;

  // Triangle is completely behind camera
  if (a.z > 0 && b.z > 0 && c.z > 0)
    return 0;

  // Clip-space w, kept because the perspective divide just below replaces
  // it with 1 and the texture interpolation needs it.
  const float aw = a.w;
  const float bw = b.w;
  const float cw = c.w;

  // convert to device coordinates by perspective division
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

  // Backface culling (configurable)
  if (r->enable_backface_culling) {
    float clocking = isClockWise(a.x, a.y, b.x, b.y, c.x, c.y);
    if (clocking >= 0)
      return 0;
  }

  // Compute Screen coordinates (optimized)
  float halfX = scrSize.x * 0.5f;
  float halfY = scrSize.y * 0.5f;
  Vec2i a_s = {(int)(a.x * halfX + halfX), (int)(a.y * halfY + halfY)};
  Vec2i b_s = {(int)(b.x * halfX + halfX), (int)(b.y * halfY + halfY)};
  Vec2i c_s = {(int)(c.x * halfX + halfX), (int)(c.y * halfY + halfY)};

  int32_t minX = MIN(MIN(a_s.x, b_s.x), c_s.x);
  int32_t minY = MIN(MIN(a_s.y, b_s.y), c_s.y);
  int32_t maxX = MAX(MAX(a_s.x, b_s.x), c_s.x);
  int32_t maxY = MAX(MAX(a_s.y, b_s.y), c_s.y);

  minX = MIN(MAX(minX, 0), r->framebuffer.size.x);
  minY = MIN(MAX(minY, 0), r->framebuffer.size.y);
  maxX = MIN(MAX(maxX, 0), r->framebuffer.size.x);
  maxY = MIN(MAX(maxY, 0), r->framebuffer.size.y);

  // Barycentric coordinates at minX/minY corner
  Vec2i minTriangle = {minX, minY};

  int32_t area = orient2d(a_s, b_s, c_s);
  if (area == 0)
    return 0;

  int32_t A01 = (a_s.y - b_s.y); // Barycentric coordinates steps
  int32_t B01 = (b_s.x - a_s.x);
  int32_t A12 = (b_s.y - c_s.y);
  int32_t B12 = (c_s.x - b_s.x);
  int32_t A20 = (c_s.y - a_s.y);
  int32_t B20 = (a_s.x - c_s.x);

  int32_t w0_row = orient2d(b_s, c_s, minTriangle);
  int32_t w1_row = orient2d(c_s, a_s, minTriangle);
  int32_t w2_row = orient2d(a_s, b_s, minTriangle);

  float invAw = 0, invBw = 0, invCw = 0;
  int has_uv = 0;
  if (o->material != 0 && aw != 0 && bw != 0 && cw != 0) {
    invAw = 1.0f / aw;
    invBw = 1.0f / bw;
    invCw = 1.0f / cw;
    tca.x *= invAw;
    tca.y *= invAw;
    tcb.x *= invBw;
    tcb.y *= invBw;
    tcc.x *= invCw;
    tcc.y *= invCw;
    has_uv = 1;
  }

  out->a = a;
  out->b = b;
  out->c = c;
  out->aw = aw;
  out->bw = bw;
  out->cw = cw;
  out->tca = tca;
  out->tcb = tcb;
  out->tcc = tcc;
  out->invAw = invAw;
  out->invBw = invBw;
  out->invCw = invCw;
  out->has_uv = has_uv;
  out->a_s = a_s;
  out->b_s = b_s;
  out->c_s = c_s;
  out->minX = minX;
  out->minY = minY;
  out->maxX = maxX;
  out->maxY = maxY;
  out->area = area;
  out->A01 = A01;
  out->B01 = B01;
  out->A12 = A12;
  out->B12 = B12;
  out->A20 = A20;
  out->B20 = B20;
  out->w0_row = w0_row;
  out->w1_row = w1_row;
  out->w2_row = w2_row;
  out->diffuse = diffuseLight;

  return 1;
}

int object_render(void *this, Mat4 m, Renderer *r) {
  Object *o = this;

  IF_NULL_RETURN(o, RENDER_ERROR);
  IF_NULL_RETURN(r, RENDER_ERROR);

  const Vec2i scrSize = r->framebuffer.size;

  for (int i = 0; i < o->mesh->indexes_count; i += 3) {
    ObjectTriangle tri;
    if (!object_build_triangle(o, m, r, i, &tri))
      continue;

    const float areaInverse = 1.0 / tri.area;
    const Vec4f a = tri.a, b = tri.b, c = tri.c;
    const Vec2f tca = tri.tca, tcb = tri.tcb, tcc = tri.tcc;
    const float invAw = tri.invAw;
    const float invBw = tri.invBw;
    const float invCw = tri.invCw;

    int32_t w0_row = tri.w0_row;
    int32_t w1_row = tri.w1_row;
    int32_t w2_row = tri.w2_row;

    for (int16_t y = tri.minY; y < tri.maxY;
         y++, w0_row += tri.B12, w1_row += tri.B20, w2_row += tri.B01) {
      int32_t w0 = w0_row;
      int32_t w1 = w1_row;
      int32_t w2 = w2_row;

      for (int32_t x = tri.minX; x < tri.maxX;
           x++, w0 += tri.A12, w1 += tri.A20, w2 += tri.A01) {
        // Early rejection using bitwise OR (optimization)
        if ((w0 | w1 | w2) < 0)
          continue;

        float depth = -(w0 * a.z + w1 * b.z + w2 * c.z) * areaInverse;
        if (depth < -1.0f || depth > 1.0f)
          continue;

        // Early Z-test (configurable optimization)
        int pixel_index = x + y * scrSize.x;
        if (r->enable_early_z_test) {
          if (depth_check(r->backend->getZetaBuffer(r, r->backend), pixel_index,
                          depth))
            continue;
        } else {
          if (depth_check(r->backend->getZetaBuffer(r, r->backend), pixel_index,
                          depth))
            continue;
        }

        depth_write(r->backend->getZetaBuffer(r, r->backend), pixel_index,
                    depth);

        if (o->material != 0) {
          // Texture lookup

          const float interpInvW = w0 * invAw + w1 * invBw + w2 * invCw;
          if (interpInvW == 0) {
            continue;
          }
          const float textCoordx =
              (w0 * tca.x + w1 * tcb.x + w2 * tcc.x) / interpInvW;
          const float textCoordy =
              (w0 * tca.y + w1 * tcb.y + w2 * tcc.y) / interpInvW;

          Pixel text = texture_readF(o->material->texture,
                                     (Vec2f){textCoordx, textCoordy});
          texture_draw(&r->framebuffer, (Vec2i){x, y},
                       pixelMul(text, tri.diffuse));
        } else {
          texture_draw(&r->framebuffer, (Vec2i){x, y},
                       pixelMul(pixelFromUInt8(255), tri.diffuse));
        }
      }
    }
  }

  return OK;
};

int object_init(Object *this, Mesh *mesh, Material *material) {
  IF_NULL_RETURN(this, INIT_ERROR);
  IF_NULL_RETURN(mesh, INIT_ERROR);
  // IF_NULL_RETURN(material, INIT_ERROR);

  this->material = material;
  this->mesh = mesh;
  this->renderable.render = &object_render;

  return OK;
}

// The deep capture reuses object_build_triangle, so transforms, backface and
// frustum rules, screen coverage, perspective UV and the depth interpolation
// all match the ordinary render exactly.  What changes is that no depth test
// rejects a fragment: every covered layer is recorded, including layers hidden
// behind closer opaque ones.
int object_capture_layers(Object *o, Mat4 m, Renderer *r,
                          DeepCapture *capture) {
  IF_NULL_RETURN(o, RENDER_ERROR);
  IF_NULL_RETURN(r, RENDER_ERROR);
  IF_NULL_RETURN(capture, RENDER_ERROR);
  IF_NULL_RETURN(o->mesh, RENDER_ERROR);

  const ScreenTransmission *transmission = 0;
  if (o->material != 0)
    transmission = o->material->transmission;

  for (int i = 0; i < o->mesh->indexes_count; i += 3) {
    ObjectTriangle tri;
    if (!object_build_triangle(o, m, r, i, &tri))
      continue;

    const float areaInverse = 1.0 / tri.area;
    const float a_z = tri.a.z, b_z = tri.b.z, c_z = tri.c.z;

    int32_t w0_row = tri.w0_row;
    int32_t w1_row = tri.w1_row;
    int32_t w2_row = tri.w2_row;

    for (int16_t y = tri.minY; y < tri.maxY;
         y++, w0_row += tri.B12, w1_row += tri.B20, w2_row += tri.B01) {
      int32_t w0 = w0_row;
      int32_t w1 = w1_row;
      int32_t w2 = w2_row;

      for (int32_t x = tri.minX; x < tri.maxX;
           x++, w0 += tri.A12, w1 += tri.A20, w2 += tri.A01) {
        if ((w0 | w1 | w2) < 0)
          continue;

        const float depth = -(w0 * a_z + w1 * b_z + w2 * c_z) * areaInverse;

        Vec3f color = {0, 0, 0};
        if (o->material != 0) {
          float uv_x = 0, uv_y = 0;
          int has_uv = tri.has_uv;
          if (has_uv) {
            const float interpInvW =
                w0 * tri.invAw + w1 * tri.invBw + w2 * tri.invCw;
            if (interpInvW == 0)
              continue;
            uv_x = (w0 * tri.tca.x + w1 * tri.tcb.x + w2 * tri.tcc.x) /
                   interpInvW;
            uv_y = (w0 * tri.tca.y + w1 * tri.tcb.y + w2 * tri.tcc.y) /
                   interpInvW;
          }

          // The local colour L, evaluated exactly as the ordinary path does:
          // texture_readF and pixelMul, then the RGB bytes treated as linear.
          const Pixel text =
              texture_readF(o->material->texture, (Vec2f){uv_x, uv_y});
          color = deep_pixel_to_linear(pixelMul(text, tri.diffuse));

          if (deep_capture_emit(capture, x, y, depth, color, transmission,
                                (Vec2f){uv_x, uv_y}, has_uv) != OK)
            return RENDER_ERROR;
        } else {
          const Pixel text = pixelMul(pixelFromUInt8(255), tri.diffuse);
          color = deep_pixel_to_linear(text);
          if (deep_capture_emit(capture, x, y, depth, color, 0,
                                (Vec2f){0, 0}, 0) != OK)
            return RENDER_ERROR;
        }
      }
    }
  }

  return OK;
}