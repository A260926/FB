/**
 * Lightmap preview: a small emissive-panel room baked offline and viewed
 * from two cameras.
 *
 * Usage: lightmap_preview <output-dir>
 *
 * Writes round0.ppm (no bounce rounds: only direct emission), round2.ppm
 * (two gather rounds: walls and floor pick up the light and the coloured
 * bounce) and camera.ppm (the same bake from a moved camera - moving the
 * camera never needs a rebake).
 */

#include "render/lightmap.h"
#include "render/mesh.h"
#include "render/object.h"
#include "render/tests/memory_backend.h"
#include "math/mat4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 96
#define HEIGHT 72
#define SAMPLES 64
#define EXPOSURE 0.5f

typedef struct {
  Vec3f positions[4];
  Vec2f uvs[4];
} QuadDef;

/* Corners are wound so both triangles (0,1,2) and (0,2,3) face the
 * requested side; uvs span the whole quad. */
static const QuadDef floor_quad = {
    {{-2, 0, -1}, {-2, 0, 3}, {2, 0, 3}, {2, 0, -1}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};
static const QuadDef back_wall_quad = {
    {{-2, 0, 3}, {-2, 3, 3}, {2, 3, 3}, {2, 0, 3}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};
static const QuadDef left_wall_quad = {
    {{-2, 0, -1}, {-2, 3, -1}, {-2, 3, 3}, {-2, 0, 3}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};
static const QuadDef right_wall_quad = {
    {{2, 0, -1}, {2, 0, 3}, {2, 3, 3}, {2, 3, -1}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};
static const QuadDef light_quad = {
    {{-1.0f, 3, 0.2f}, {1.0f, 3, 0.2f}, {1.0f, 3, 1.8f}, {-1.0f, 3, 1.8f}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};
static const QuadDef occluder_quad = {
    {{-0.8f, 1.5f, 0.6f}, {-0.8f, 1.5f, 1.4f}, {0.2f, 1.5f, 1.4f},
     {0.2f, 1.5f, 0.6f}},
    {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
};

static const uint16_t quad_indices[6] = {0, 1, 2, 0, 2, 3};

static Mesh empty_mesh = {0, 0, 0, 0, 0};
static Object empty_object;
static Mesh meshes[6];
static Object objects[6];
static Entity root, children[6];

static int write_ppm(const char *path, const Pixel *frame, int width,
                     int height) {
  FILE *f = fopen(path, "wb");
  int x, y;
  if (!f)
    return 1;
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  /* Row 0 of the framebuffer is the bottom of the image. */
  for (y = height - 1; y >= 0; y--) {
    for (x = 0; x < width; x++) {
      Pixel p = frame[y * width + x];
      fputc(p.r, f);
      fputc(p.g, f);
      fputc(p.b, f);
    }
  }
  if (fclose(f) != 0)
    return 1;
  return 0;
}

static int lit_pixels(const Pixel *frame, int count) {
  int i, lit = 0;
  for (i = 0; i < count; i++) {
    if (frame[i].r || frame[i].g || frame[i].b)
      lit++;
  }
  return lit;
}

int main(int argc, char **argv) {
  static const QuadDef *quads[6];
  static const Vec3f reflectance[6] = {
      {0.7f, 0.7f, 0.7f},   /* floor */
      {0.7f, 0.7f, 0.7f},   /* back wall */
      {0.8f, 0.08f, 0.08f}, /* left wall: red */
      {0.08f, 0.8f, 0.08f}, /* right wall: green */
      {0.0f, 0.0f, 0.0f},   /* light panel */
      {0.5f, 0.5f, 0.5f},   /* occluder */
  };
  static const Vec3f emission[6] = {
      {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {8, 8, 8}, {0, 0, 0},
  };
  static const int map_size[6] = {16, 8, 8, 8, 8, 4};
  BakeBinding bindings[7];
  BakeScene scene;
  MemoryBackend backend;
  Renderer renderer;
  size_t paths[6][1];
  Mat4 rot, tilt, orientation, camera1, camera2, translation;
  char path[1024];
  int i, status = 0;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
    return 2;
  }

  quads[0] = &floor_quad;
  quads[1] = &back_wall_quad;
  quads[2] = &left_wall_quad;
  quads[3] = &right_wall_quad;
  quads[4] = &light_quad;
  quads[5] = &occluder_quad;

  object_init(&empty_object, &empty_mesh, 0);
  entity_init(&root, (Renderable *)&empty_object, mat4Identity());
  for (i = 0; i < 6; i++) {
    meshes[i].indexes_count = 6;
    meshes[i].pos_indices = quad_indices;
    meshes[i].tex_indices = quad_indices;
    meshes[i].positions = quads[i]->positions;
    meshes[i].textCoord = quads[i]->uvs;
    object_init(&objects[i], &meshes[i], 0);
    entity_init(&children[i], (Renderable *)&objects[i], mat4Identity());
    paths[i][0] = (size_t)i;
    bindings[i + 1] = (BakeBinding){
        &paths[i][0], 1, map_size[i], map_size[i], quads[i]->uvs, 4,
        quad_indices, reflectance[i], emission[i]};
  }
  root.children_entities.size = 6;
  root.children_entities.data = children;
  bindings[0] = (BakeBinding){0, 0, 1, 1, 0, 0, 0, {0, 0, 0}, {0, 0, 0}};

  bake_scene_init(&scene);
  if (bake_scene_build(&scene, &root, bindings, 7) != 0) {
    fprintf(stderr, "bake_scene_build failed\n");
    return 1;
  }

  if (memory_backend_init(&backend, (Vec2i){WIDTH, HEIGHT}) != 0) {
    fprintf(stderr, "memory_backend_init failed\n");
    return 1;
  }
  if (renderer_init(&renderer, (Vec2i){WIDTH, HEIGHT}, (Backend *)&backend) !=
      0) {
    fprintf(stderr, "renderer_init failed\n");
    return 1;
  }
  renderer_set_root_renderable(&renderer, (Renderable *)&root);
  renderer.camera_projection =
      mat4Perspective(1.0f, 50.0f, (float)WIDTH / (float)HEIGHT, 1.0f);

  /* Camera 1: inside the room's front, looking in and up at the light. */
  rot = mat4RotateY(3.14159265f);
  tilt = mat4RotateX(-0.5f);
  orientation = mat4MultiplyM(&rot, &tilt);
  translation = mat4Translate((Vec3f){0, 1.4f, -2.0f});
  camera1 = mat4MultiplyM(&orientation, &translation);

  /* Camera 2: off to the right, angled back across the room. */
  rot = mat4RotateY(3.14159265f - 0.55f);
  tilt = mat4RotateX(-0.15f);
  orientation = mat4MultiplyM(&rot, &tilt);
  translation = mat4Translate((Vec3f){1.3f, 1.2f, -1.5f});
  camera2 = mat4MultiplyM(&orientation, &translation);

  /* Round 0: emission only, the bake untouched. */
  renderer.camera_view = camera1;
  if (renderer_render_lightmapped(&renderer, &scene, EXPOSURE) != 0) {
    fprintf(stderr, "render round0 failed\n");
    return 1;
  }
  snprintf(path, sizeof(path), "%s/round0.ppm", argv[1]);
  status |= write_ppm(path, backend.frame, WIDTH, HEIGHT);

  /* Two gather rounds with fixed hemisphere samples. */
  if (bake_step(&scene, 2, SAMPLES, 0.01f, (Vec3f){0, 0, 0}) != 0) {
    fprintf(stderr, "bake_step failed\n");
    return 1;
  }
  if (renderer_render_lightmapped(&renderer, &scene, EXPOSURE) != 0) {
    fprintf(stderr, "render round2 failed\n");
    return 1;
  }
  snprintf(path, sizeof(path), "%s/round2.ppm", argv[1]);
  status |= write_ppm(path, backend.frame, WIDTH, HEIGHT);
  if (lit_pixels(backend.frame, WIDTH * HEIGHT) < 100) {
    fprintf(stderr, "round2.ppm is suspiciously dark\n");
    status = 1;
  }

  /* Same bake, another camera: no rebake needed. */
  renderer.camera_view = camera2;
  if (renderer_render_lightmapped(&renderer, &scene, EXPOSURE) != 0) {
    fprintf(stderr, "render camera failed\n");
    return 1;
  }
  snprintf(path, sizeof(path), "%s/camera.ppm", argv[1]);
  status |= write_ppm(path, backend.frame, WIDTH, HEIGHT);
  if (lit_pixels(backend.frame, WIDTH * HEIGHT) < 100) {
    fprintf(stderr, "camera.ppm is suspiciously dark\n");
    status = 1;
  }

  memory_backend_free(&backend);
  bake_scene_free(&scene);
  return status;
}
