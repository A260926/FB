// Surface guide example for the Fujiyama CPU renderer.
//
// Builds a small triangle mesh scene in memory (a floor quad made of two
// triangles plus a floating triangle in front of it), renders it with the
// surface guide output enabled, and writes a false-color preview of the
// per-pixel guide records to .task-build/guides.ppm.
//
// Each pixel of a SurfaceGuideBuffer contains:
//   uint32_t id      - the ObjectInstance guide id that collected the
//                      largest Gaussian filter weight (0 = background)
//   Real     depth   - filter-weighted primary ray distance of the
//                      winning id's samples
//   Vector   normal  - filter-weighted, normalized world space shading
//                      normal of the winning id's samples
//   Real     coverage- filter weight of non-background samples / all weight
//
// Build (from the repository root, after task_support/build.sh):
//   c++ -std=c++11 -O1 -pthread -I. -Isrc examples/surface_guides.cpp \
//       .task-build/fj_surface_guides.o .task-build/libfujiyama.a \
//       -o .task-build/example
// Run (from the repository root):
//   .task-build/example
//   -> writes .task-build/guides.ppm

#include "fj_surface_guides.h"
#include "fj_renderer.h"
#include "fj_mesh.h"
#include "fj_bvh_accelerator.h"
#include "fj_object_instance.h"
#include "fj_object_group.h"
#include "fj_accelerator.h"
#include "fj_shader.h"
#include "fj_framebuffer.h"
#include "fj_camera.h"

#include <cmath>
#include <cstdio>

using namespace fj;

namespace {

class ConstantShader : public Shader {
public:
  explicit ConstantShader(const Color &color) : color_(color) {}

private:
  void evaluate(const TraceContext &, const SurfaceInput &,
      SurfaceOutput *out) const override
  {
    out->Cs = color_;
    out->Os = 1;
  }

  Color color_;
};

// deterministic pastel color for a guide id (id 0 is black)
void id_color(uint32_t id, double *r, double *g, double *b)
{
  if (id == 0) {
    *r = *g = *b = 0;
    return;
  }
  const double hue = (id * 2654435761u % 1024) / 1024.;
  *r = .35 + .65 * std::fabs(std::sin(6.28318 * hue));
  *g = .35 + .65 * std::fabs(std::sin(6.28318 * hue + 2.0944));
  *b = .35 + .65 * std::fabs(std::sin(6.28318 * hue + 4.18879));
}

} // namespace

int main()
{
  // triangle mesh scene generated in memory
  Mesh mesh;
  mesh.SetPointCount(4);
  mesh.SetFaceCount(2);
  mesh.AddPointPosition();
  mesh.AddFaceIndices();

  // floor quad (two triangles) at z = -6
  mesh.SetPointPosition(0, Vector(-4, -1.5, -6));
  mesh.SetPointPosition(1, Vector( 4, -1.5, -6));
  mesh.SetPointPosition(2, Vector( 4,  1.5, -6));
  mesh.SetPointPosition(3, Vector(-4,  1.5, -6));
  mesh.SetFaceIndices(0, Index3(0, 1, 2));
  mesh.SetFaceIndices(1, Index3(0, 2, 3));

  mesh.ComputeNormals();
  mesh.ComputeBounds();

  BVHAccelerator mesh_acc;
  mesh_acc.SetPrimitiveSet(&mesh);
  if (mesh_acc.Build() != 0) {
    std::fprintf(stderr, "failed to build mesh accelerator\n");
    return 1;
  }

  ConstantShader floor_shader(Color(.7f, .7f, .7f));
  ConstantShader tri_shader(Color(.7f, .7f, .7f)); // same color, different labels

  ObjectInstance floor_obj;
  floor_obj.SetSurface(&mesh_acc);
  floor_obj.SetShader(&floor_shader, 0);
  floor_obj.SetGuideID(2);
  floor_obj.ComputeBounds();

  // the floating triangle gets its own mesh so it can carry its own label
  Mesh tri_mesh;
  tri_mesh.SetPointCount(3);
  tri_mesh.SetFaceCount(1);
  tri_mesh.AddPointPosition();
  tri_mesh.AddFaceIndices();
  tri_mesh.SetPointPosition(0, Vector(-1.2, -1.0, -3));
  tri_mesh.SetPointPosition(1, Vector( 1.2, -1.0, -3));
  tri_mesh.SetPointPosition(2, Vector( 0.0,  1.2, -3));
  tri_mesh.SetFaceIndices(0, Index3(0, 1, 2));
  tri_mesh.ComputeNormals();
  tri_mesh.ComputeBounds();

  BVHAccelerator tri_acc;
  tri_acc.SetPrimitiveSet(&tri_mesh);
  if (tri_acc.Build() != 0) {
    std::fprintf(stderr, "failed to build triangle accelerator\n");
    return 1;
  }

  ObjectInstance tri_obj;
  tri_obj.SetSurface(&tri_acc);
  tri_obj.SetShader(&tri_shader, 0);
  tri_obj.SetGuideID(3);
  tri_obj.ComputeBounds();

  ObjectGroup group;
  group.AddObject(&floor_obj);
  group.AddObject(&tri_obj);
  group.ComputeBounds();
  if (const_cast<Accelerator *>(group.GetSurfaceAccelerator())->Build() != 0) {
    std::fprintf(stderr, "failed to build group accelerator\n");
    return 1;
  }

  const int XRES = 128;
  const int YRES = 128;

  Camera camera;
  FrameBuffer framebuffer;
  Renderer renderer;
  SurfaceGuideBuffer guides;

  renderer.SetCamera(&camera);
  renderer.SetFrameBuffers(&framebuffer);
  renderer.SetTargetObjects(&group);
  renderer.SetResolution(XRES, YRES);
  renderer.SetTileSize(32, 32);
  renderer.SetSamplerType(RENDERER_ADAPTIVE_GRID_SAMPLER);
  renderer.SetPixelSamples(2, 2);
  renderer.SetMaxSubdivision(4);
  renderer.SetFilterWidth(1, 1);
  renderer.SetSampleJitter(0);
  renderer.SetSampleTimeRange(0, 0);
  renderer.SetFrameReportCallback(NULL, NULL, NULL, NULL);
  renderer.SetTileReportCallback(NULL, NULL, NULL, NULL);
  renderer.SetSurfaceGuides(&guides);

  if (renderer.RenderScene() != 0) {
    std::fprintf(stderr, "RenderScene failed\n");
    return 1;
  }

  // false-color the guide ids, dimmed by coverage
  FILE *file = std::fopen(".task-build/guides.ppm", "wb");
  if (file == NULL) {
    std::fprintf(stderr, "cannot open .task-build/guides.ppm\n");
    return 1;
  }
  std::fprintf(file, "P6\n%d %d\n255\n", XRES, YRES);
  for (int y = 0; y < YRES; y++) {
    for (int x = 0; x < XRES; x++) {
      const SurfaceGuide g = guides.GetPixel(x, y);
      double r, gg, b;
      id_color(g.id, &r, &gg, &b);
      const unsigned char rgb[3] = {
          (unsigned char)(255 * r * g.coverage + .5),
          (unsigned char)(255 * gg * g.coverage + .5),
          (unsigned char)(255 * b * g.coverage + .5)};
      std::fwrite(rgb, 1, 3, file);
    }
  }
  std::fclose(file);

  // print a few sample records
  const SurfaceGuide center = guides.GetPixel(XRES / 2, YRES / 2);
  std::printf("center pixel: id=%u depth=%.4f normal=(%.3f, %.3f, %.3f) coverage=%.3f\n",
      center.id, center.depth, center.normal.x, center.normal.y, center.normal.z,
      center.coverage);
  std::printf("wrote .task-build/guides.ppm (%d x %d)\n", XRES, YRES);

  return 0;
}
