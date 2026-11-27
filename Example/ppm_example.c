/*
** PPM rendering example for libtess2.
**
** The scene deliberately contains the three features the geometric tests
** care about:
**   - a square with a hole,
**   - two crossed rectangles (winding 2 in the middle),
**   - a self-intersecting bowtie plus a disjoint island.
**
** Each output triangle is filled with its own colour so overlapping or
** missing triangles are visible at a glance, and the TESS_BOUNDARY_CONTOURS
** output is stroked on top in black.
**
** Build (from the repository root):
**   cc -std=c99 -O2 -IInclude Example/ppm_example.c <every .c file in Source/> -lm
**       -o ppm_example && ./ppm_example out.ppm
**
** Only the C standard library is used.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tesselator.h"

#define IMAGE_W 320
#define IMAGE_H 320

typedef struct {
  float x;
  float y;
} Vec2;

typedef struct {
  Vec2* pts;
  int npts;
} Contour;

typedef struct {
  Contour contours[8];
  int ncontours;
} Scene;

static void AddContour(Scene* scene, int npts, const float* xy) {
  Contour* c = &scene->contours[scene->ncontours++];
  c->pts = (Vec2*)malloc(sizeof(Vec2) * npts);
  c->npts = npts;
  for (int i = 0; i < npts; ++i) {
    c->pts[i].x = xy[i * 2];
    c->pts[i].y = xy[i * 2 + 1];
  }
}

static void FreeScene(Scene* scene) {
  for (int i = 0; i < scene->ncontours; ++i) free(scene->contours[i].pts);
}

static void BuildScene(Scene* scene) {
  static const float holeOuter[] = {
      -30.f, -10.f, -6.f, -10.f, -6.f, 30.f, -30.f, 30.f};
  // Inner loop wound opposite to the outer loop, so with nonzero winding
  // the enclosed square stays empty and the hole is visible.
  static const float holeInner[] = {
      -22.f, -2.f, -22.f, 22.f, -14.f, 22.f, -14.f, -2.f};

  static const float crossA[] = {
      2.f, -4.f, 30.f, -4.f, 30.f, 4.f, 2.f, 4.f};
  static const float crossB[] = {
      12.f, -18.f, 20.f, -18.f, 20.f, 18.f, 12.f, 18.f};

  // Self-intersecting bowtie, plus a separate island.  The bowtie's two
  // lobes have opposite winding and both are filled under ODD; the island
  // is disjoint from every other contour.
  static const float bowtie[] = {
      4.f, 24.f, 12.f, 32.f, 12.f, 24.f, 4.f, 32.f};
  static const float island[] = {
      16.f, 24.f, 30.f, 24.f, 30.f, 32.f, 16.f, 32.f};

  AddContour(scene, 4, holeOuter);
  AddContour(scene, 4, holeInner);
  AddContour(scene, 4, crossA);
  AddContour(scene, 4, crossB);
  AddContour(scene, 4, bowtie);
  AddContour(scene, 4, island);
}

static void SceneBounds(const Scene* scene, float* minx, float* miny,
                        float* maxx, float* maxy) {
  *minx = *miny = 1e30f;
  *maxx = *maxy = -1e30f;
  for (int i = 0; i < scene->ncontours; ++i) {
    const Contour* c = &scene->contours[i];
    for (int j = 0; j < c->npts; ++j) {
      if (c->pts[j].x < *minx) *minx = c->pts[j].x;
      if (c->pts[j].y < *miny) *miny = c->pts[j].y;
      if (c->pts[j].x > *maxx) *maxx = c->pts[j].x;
      if (c->pts[j].y > *maxy) *maxy = c->pts[j].y;
    }
  }
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "tess2_scene.ppm";
  Scene scene;
  scene.ncontours = 0;
  BuildScene(&scene);

  // Two independent tesselator objects: one for filled triangles, one for
  // the boundary stroke.
  TESStesselator* fill = tessNewTess(NULL);
  TESStesselator* stroke = tessNewTess(NULL);
  if (!fill || !stroke) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  const float normal[3] = {0.f, 0.f, 1.f};
  for (int i = 0; i < scene.ncontours; ++i) {
    const Contour* c = &scene.contours[i];
    // Contours keep their listed orientation; the hole loop is wound the
    // opposite way, and nonzero winding leaves it empty.
    tessAddContour(fill, 2, c->pts, sizeof(Vec2), c->npts);
    tessAddContour(stroke, 2, c->pts, sizeof(Vec2), c->npts);
  }

  if (!tessTesselate(fill, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2,
                     normal)) {
    fprintf(stderr, "fill tessellation failed\n");
    return 1;
  }
  if (!tessTesselate(stroke, TESS_WINDING_NONZERO, TESS_BOUNDARY_CONTOURS,
                     0, 2, normal)) {
    fprintf(stderr, "boundary tessellation failed\n");
    return 1;
  }

  const float* verts = tessGetVertices(fill);
  const int* elems = tessGetElements(fill);
  const int ntris = tessGetElementCount(fill);

  const float* bverts = tessGetVertices(stroke);
  const int* belems = tessGetElements(stroke);
  const int nbcont = tessGetElementCount(stroke);

  float minx, miny, maxx, maxy;
  SceneBounds(&scene, &minx, &miny, &maxx, &maxy);
  float spanX = maxx - minx, spanY = maxy - miny;
  if (spanX <= 0.f) spanX = 1.f;
  if (spanY <= 0.f) spanY = 1.f;
  float scale = 0.92f * ((float)IMAGE_W / spanX < (float)IMAGE_H / spanY
                            ? (float)IMAGE_W / spanX
                            : (float)IMAGE_H / spanY);
  float offx = (IMAGE_W - spanX * scale) * 0.5f;
  float offy = (IMAGE_H - spanY * scale) * 0.5f;

  unsigned char* pixels =
      (unsigned char*)malloc((size_t)IMAGE_W * IMAGE_H * 3);
  if (!pixels) return 1;
  memset(pixels, 255, (size_t)IMAGE_W * IMAGE_H * 3);

  // Fill one triangle per output polygon with a per-triangle colour, then
  // stroke the boundary contours in black.  Pixels are sampled at their
  // centres; each triangle is tested with exact orientation signs so the
  // hole and the crossing are rendered correctly.
  for (int i = 0; i < ntris; ++i) {
    int a = elems[i * 3 + 0];
    int b = elems[i * 3 + 1];
    int c = elems[i * 3 + 2];
    if (a == TESS_UNDEF || b == TESS_UNDEF || c == TESS_UNDEF) continue;
    float ax = offx + (verts[a * 2 + 0] - minx) * scale;
    float ay = offy + (verts[a * 2 + 1] - miny) * scale;
    float bx = offx + (verts[b * 2 + 0] - minx) * scale;
    float by = offy + (verts[b * 2 + 1] - miny) * scale;
    float cx = offx + (verts[c * 2 + 0] - minx) * scale;
    float cy = offy + (verts[c * 2 + 1] - miny) * scale;

    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0.f) continue;
    float s = area > 0.f ? 1.f : -1.f;

    unsigned char r = (unsigned char)(70 + (i * 53) % 150);
    unsigned char g = (unsigned char)(90 + (i * 97) % 140);
    unsigned char bl = (unsigned char)(110 + (i * 31) % 130);

    for (int py = 0; py < IMAGE_H; ++py) {
      float y = (float)py + 0.5f;
      for (int px = 0; px < IMAGE_W; ++px) {
        float x = (float)px + 0.5f;
        float e0 = ((bx - ax) * (y - ay) - (by - ay) * (x - ax)) * s;
        float e1 = ((cx - bx) * (y - by) - (cy - by) * (x - bx)) * s;
        float e2 = ((ax - cx) * (y - cy) - (ay - cy) * (x - cx)) * s;
        if (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) {
          unsigned char* p = pixels + ((size_t)py * IMAGE_W + px) * 3;
          p[0] = r;
          p[1] = g;
          p[2] = bl;
        }
      }
    }
  }

  // Stroke every directed boundary edge.  The order and the direction of
  // these edges are the library's own; only their geometry is used here.
  for (int ci = 0; ci < nbcont; ++ci) {
    int base = belems[ci * 2];
    int count = belems[ci * 2 + 1];
    for (int j = 0; j < count; ++j) {
      int i0 = base + j;
      int i1 = base + (j + 1) % count;
      float x0 = offx + (bverts[i0 * 2 + 0] - minx) * scale;
      float y0 = offy + (bverts[i0 * 2 + 1] - miny) * scale;
      float x1 = offx + (bverts[i1 * 2 + 0] - minx) * scale;
      float y1 = offy + (bverts[i1 * 2 + 1] - miny) * scale;
      int steps = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
      for (int k = 0; k <= steps; ++k) {
        float t = (float)k / (float)steps;
        int px = (int)(x0 + (x1 - x0) * t);
        int py = (int)(y0 + (y1 - y0) * t);
        if (px >= 0 && px < IMAGE_W && py >= 0 && py < IMAGE_H) {
          unsigned char* p = pixels + ((size_t)py * IMAGE_W + px) * 3;
          p[0] = p[1] = p[2] = 0;
        }
      }
    }
  }

  FILE* out = fopen(path, "wb");
  if (!out) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  fprintf(out, "P6\n%d %d\n255\n", IMAGE_W, IMAGE_H);
  fwrite(pixels, 1, (size_t)IMAGE_W * IMAGE_H * 3, out);
  fclose(out);

  printf("WROTE %s triangles=%d boundary_contours=%d\n", path, ntris, nbcont);

  free(pixels);
  tessDeleteTess(fill);
  tessDeleteTess(stroke);
  FreeScene(&scene);
  return 0;
}
