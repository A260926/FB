/**
 * Lightmap baking preview.
 *
 * Builds a small static corner room entirely in C: a warm emitting ceiling,
 * a gray floor, a blue back wall, a red left wall and a thin occluder that
 * casts a partial shadow. The diffuse indirect light is baked once and then
 * reused from two different cameras.
 *
 * Usage: lightmap_preview <output directory>
 * Writes round0.ppm (emission only), round2.ppm (two bounce rounds) and
 * camera.ppm (the round-2 bake seen from a moved camera), all binary P6.
 */

#include "render/array.h"
#include "render/backend.h"
#include "render/entity.h"
#include "render/lightmap.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/pixel.h"
#include "render/renderer.h"
#include "render/state.h"
#include "tests/memory_backend.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMG_W 96
#define IMG_H 72
#define LIGHTMAP 8

// A two-triangle quad with its own position/UV tables and one lightmap
// binding. Corners are passed in counter-clockwise order seen from the
// side the baked normal must point toward.
typedef struct {
  Vec3f pos[4];
  Vec2f uv[4];
  uint16_t index[6];
  Mesh mesh;
  Material material;
  Object object;
  Entity entity;
  BakeBinding binding;
} Quad;

static void quad_init(Quad *q, const Vec3f corner[4], Vec3f reflectance,
                      Vec3f emission, const size_t *path, size_t path_len) {
  for (int i = 0; i < 4; i++) {
    q->pos[i] = corner[i];
    q->uv[i] = (Vec2f){(float)(i == 1 || i == 2),
                      (float)(i >= 2)};
  }
  q->uv[0] = (Vec2f){0, 0};
  q->uv[1] = (Vec2f){1, 0};
  q->uv[2] = (Vec2f){1, 1};
  q->uv[3] = (Vec2f){0, 1};
  q->index[0] = 0;
  q->index[1] = 1;
  q->index[2] = 2;
  q->index[3] = 0;
  q->index[4] = 2;
  q->index[5] = 3;

  q->mesh.indexes_count = 6;
  q->mesh.pos_indices = q->index;
  q->mesh.tex_indices = q->index;
  q->mesh.positions = q->pos;
  q->mesh.textCoord = q->uv;

  // No base texture: the render path treats a missing material as white.
  q->material.texture = 0;
  object_init(&q->object, &q->mesh, &q->material);
  entity_init(&q->entity, (Renderable *)&q->object, mat4Identity());

  q->binding.path = path;
  q->binding.path_len = path_len;
  q->binding.width = LIGHTMAP;
  q->binding.height = LIGHTMAP;
  q->binding.uv = q->uv;
  q->binding.uv_count = 4;
  q->binding.uv_indices = q->index;
  q->binding.reflectance = reflectance;
  q->binding.emission = emission;
}

// Row 0 of the framebuffer is the image bottom, so rows are flipped on write.
static int write_ppm(const char *path, const Pixel *frame, int width,
                     int height) {
  FILE *f = fopen(path, "wb");
  if (f == 0)
    return 1;
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; y++) {
    const Pixel *row = &frame[(height - 1 - y) * width];
    for (int x = 0; x < width; x++) {
      unsigned char rgb[3] = {row[x].r, row[x].g, row[x].b};
      if (fwrite(rgb, 1, 3, f) != 3) {
        fclose(f);
        return 1;
      }
    }
  }
  return fclose(f) != 0;
}

static int render_to_file(Renderer *renderer, BakeScene *scene,
                          float exposure, const char *path) {
  if (renderer_render_lightmapped(renderer, scene, exposure) != OK) {
    fprintf(stderr, "lightmapped render failed for %s\n", path);
    return 1;
  }
  MemoryBackend *backend = (MemoryBackend *)renderer->backend;
  if (write_ppm(path, backend->frame, IMG_W, IMG_H)) {
    fprintf(stderr, "could not write %s\n", path);
    return 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
    return 2;
  }

  enum { EMITTER, FLOOR, BACK, LEFT, OCCLUDER, PIECE_COUNT };

  // Winding is chosen so the baked normal faces into the room.
  Vec3f emitter_corners[4] = {
      {-1.5f, 1.2f, -3.0f}, {-1.5f, 1.2f, -6.0f},
      {1.5f, 1.2f, -6.0f}, {1.5f, 1.2f, -3.0f}}; // normal -y
  Vec3f floor_corners[4] = {
      {-1.5f, -1.2f, -3.0f}, {1.5f, -1.2f, -3.0f},
      {1.5f, -1.2f, -6.0f}, {-1.5f, -1.2f, -6.0f}}; // normal +y
  Vec3f back_corners[4] = {
      {-1.5f, -1.2f, -6.0f}, {1.5f, -1.2f, -6.0f},
      {1.5f, 1.2f, -6.0f}, {-1.5f, 1.2f, -6.0f}}; // normal +z
  Vec3f left_corners[4] = {
      {-1.5f, -1.2f, -3.0f}, {-1.5f, -1.2f, -6.0f},
      {-1.5f, 1.2f, -6.0f}, {-1.5f, 1.2f, -3.0f}}; // normal +x
  Vec3f occluder_corners[4] = {
      {-0.2f, -1.2f, -4.0f}, {0.8f, -1.2f, -4.0f},
      {0.8f, 0.3f, -4.0f}, {-0.2f, 0.3f, -4.0f}}; // normal +z

  Quad pieces[PIECE_COUNT];
  size_t paths[PIECE_COUNT][1];
  quad_init(&pieces[EMITTER], emitter_corners, (Vec3f){0, 0, 0},
            (Vec3f){2.0f, 1.8f, 1.4f}, paths[EMITTER], 1);
  quad_init(&pieces[FLOOR], floor_corners, (Vec3f){0.6f, 0.6f, 0.6f},
            (Vec3f){0, 0, 0}, paths[FLOOR], 1);
  quad_init(&pieces[BACK], back_corners, (Vec3f){0.3f, 0.5f, 0.7f},
            (Vec3f){0, 0, 0}, paths[BACK], 1);
  quad_init(&pieces[LEFT], left_corners, (Vec3f){0.7f, 0.3f, 0.3f},
            (Vec3f){0, 0, 0}, paths[LEFT], 1);
  quad_init(&pieces[OCCLUDER], occluder_corners,
            (Vec3f){0.4f, 0.4f, 0.4f}, (Vec3f){0, 0, 0},
            paths[OCCLUDER], 1);
  for (int i = 0; i < PIECE_COUNT; i++)
    paths[i][0] = (size_t)i;

  // Root entity carries an empty object on the empty path and the five
  // pieces as children.
  Mesh empty_mesh = {0, 0, 0, 0, 0};
  Object root_object;
  object_init(&root_object, &empty_mesh, 0);
  Entity root;
  entity_init(&root, (Renderable *)&root_object, mat4Identity());
  Entity child_entities[PIECE_COUNT];
  for (int i = 0; i < PIECE_COUNT; i++)
    child_entities[i] = pieces[i].entity;
  Array children;
  array_init(&children, PIECE_COUNT, child_entities);
  root.children_entities = children;

  BakeBinding bindings[PIECE_COUNT + 1];
  for (int i = 0; i < PIECE_COUNT; i++)
    bindings[i] = pieces[i].binding;
  // Empty-path binding for the root's face-less object.
  static const Vec2f empty_uv[1] = {{0, 0}};
  static uint16_t empty_idx[1] = {0};
  bindings[PIECE_COUNT] = (BakeBinding){
      0, 0, LIGHTMAP, LIGHTMAP, empty_uv, 1, empty_idx,
      {0, 0, 0}, {0, 0, 0}};

  BakeScene *scene = 0;
  if (bake_scene_build(&scene, &root, bindings, PIECE_COUNT + 1) != OK) {
    fprintf(stderr, "bake_scene_build failed\n");
    return 1;
  }

  MemoryBackend backend;
  if (memory_backend_init(&backend, (Vec2i){IMG_W, IMG_H}) != 0) {
    fprintf(stderr, "backend allocation failed\n");
    return 1;
  }
  Renderer renderer;
  if (renderer_init(&renderer, (Vec2i){IMG_W, IMG_H},
                    (Backend *)&backend) != OK) {
    fprintf(stderr, "renderer_init failed\n");
    return 1;
  }
  renderer.camera_projection =
      mat4Perspective(1, 50.0f, (float)IMG_W / (float)IMG_H, 0.6f);
  renderer.camera_view = mat4Translate((Vec3f){0, 0, 0});
  renderer_set_root_renderable(&renderer, (Renderable *)&root);
  // Winding here is authored for the bake normals; show every face in the
  // preview rather than culling the back of a wall.
  renderer_enable_backface_culling(&renderer, false);

  const float exposure = 0.6f;
  const Vec3f environment = {0.04f, 0.04f, 0.05f};

  char path[256];
  snprintf(path, sizeof(path), "%s/round0.ppm", argv[1]);
  if (render_to_file(&renderer, scene, exposure, path))
    return 1;

  // Two fixed-sample rounds, then the same camera shows the indirect light.
  if (bake_step(scene, 2, 16, 1e-4f, environment) != OK) {
    fprintf(stderr, "bake_step failed\n");
    return 1;
  }
  snprintf(path, sizeof(path), "%s/round2.ppm", argv[1]);
  if (render_to_file(&renderer, scene, exposure, path))
    return 1;

  // The bake is camera independent: move the camera without re-baking.
  renderer.camera_view = mat4Translate((Vec3f){0.8f, 0.25f, 0.0f});
  snprintf(path, sizeof(path), "%s/camera.ppm", argv[1]);
  if (render_to_file(&renderer, scene, exposure, path))
    return 1;

  memory_backend_free(&backend);
  bake_scene_free(scene);
  return 0;
}
