// Thin-lens depth-of-field example.
//
// Builds a small flat-shaded scene (a near occluder, a focal-plane marker and
// far colored quads), then renders it twice through the ThinLens entry:
//   1. a center pinhole image (aperture radius R = 0),
//   2. a real aperture image averaged over a caller-supplied set of pupils.
// It writes both frames and a side-by-side comparison BMP using the renderer's
// original exporter. Everything is deterministic: the pupil samples are given
// explicitly, so no random numbers or external assets are used.
//
// Build (from the repository root):
//   c++ -std=c++20 -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -I. -Iincludes \
//     -Ilib/glm-0.9.8.2 $(find src -name '*.cpp' ! -name main.cpp) \
//     lib/EasyBMP_1.06/EasyBMP.cpp examples/thinlens_example.cpp -o thinlens_example
//   ./thinlens_example

#include <cmath>
#include <vector>
#include <cstdio>

#include "World.h"
#include "PERSPECTIVECAMERA.H"
#include "ForwardRasterizer.h"
#include "ThinLens.h"
#include "Material.h"
#include "DIRECTIONALLIGHT.H"
#include "GeometryObject.h"

namespace {

GeometryObject* make_quad(Material* material, const Point3D& a, const Point3D& b,
                          const Point3D& c, const Point3D& d, const RGBColor& color) {
  std::vector<Point3D> verts{ a, b, c, d };
  std::vector<RGBColor> colors{ color, color, color, color };
  std::vector<uint32_t> indices{ 0, 1, 2, 0, 2, 3 };
  return new GeometryObject(material, verts, colors, std::vector<Vector2D>(), indices);
}

} // namespace

int main() {
  const uint32_t width = 200, height = 150;
  const double focal_F = 10.0;
  const double aperture_R = 2.5;

  FlatMaterial material(RGBColor(0.4f, 0.4f, 0.4f), RGBColor(0.0f, 0.0f, 0.0f), 1.0f);
  // Light travels toward +Z so the camera-facing quads receive diffuse light.
  DirectionalLight light(RGBColor(1, 1, 1), Vector3D(0, 0, 1));
  std::vector<Light*> lights{ &light };

  std::vector<GeometryObject*> objects;
  objects.push_back(make_quad(&material, {-4.0, -1.5, 6}, {-1.5, -1.5, 6},
                              {-1.5, 1.5, 6}, {-4.0, 1.5, 6}, RGBColor(1, 1, 0))); // near occluder
  objects.push_back(make_quad(&material, {-0.8, -0.8, focal_F}, {0.8, -0.8, focal_F},
                              {0.8, 0.8, focal_F}, {-0.8, 0.8, focal_F}, RGBColor(0, 1, 1))); // focal marker
  objects.push_back(make_quad(&material, {-6, -3, 25}, {-2, -3, 25}, {-2, 3, 25}, {-6, 3, 25}, RGBColor(1, 0, 0)));
  objects.push_back(make_quad(&material, {-2, -3, 25}, {2, -3, 25}, {2, 3, 25}, {-2, 3, 25}, RGBColor(0, 1, 0)));
  objects.push_back(make_quad(&material, {2, -3, 25}, {6, -3, 25}, {6, 3, 25}, {2, 3, 25}, RGBColor(0, 0, 1)));

  ForwardRasterizer renderer;
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1), height, width, &renderer, 1, 200, 90.0f);
  World world(objects, lights, &camera);
  renderer.set_world(&world);
  renderer.set_camera(&camera);

  // Caller-supplied pupils: the center plus an eight-sample ring on the unit
  // disk, all with equal non-negative weights.
  std::vector<PupilSample> samples;
  samples.push_back(PupilSample(0.0, 0.0, 1.0));
  const int ring = 8;
  for (int k = 0; k < ring; ++k) {
    const double angle = 2.0 * 3.14159265358979 * k / ring;
    samples.push_back(PupilSample(std::cos(angle), std::sin(angle), 1.0));
  }

  // 1. Pinhole reference (R = 0 ignores the pupil distribution).
  ThinLensImage pinhole = ThinLens::render(camera, renderer, world, focal_F, 0.0, samples);
  ThinLens::exportImage(pinhole, renderer, "thinlens_pinhole.bmp");

  // 2. Real aperture depth-of-field preview.
  ThinLensImage dof = ThinLens::render(camera, renderer, world, focal_F, aperture_R, samples);
  ThinLens::exportImage(dof, renderer, "thinlens_dof.bmp");

  // 3. Side-by-side comparison through the same original BMP exporter.
  std::vector<RGBColor> comparison(height * width * 2, BACKGROUND_COLOR);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t src = y * width + x;
      comparison[y * (width * 2) + x] = pinhole.color[src];
      comparison[y * (width * 2) + width + x] = dof.color[src];
    }
  }
  renderer.exportImageSized(comparison, "thinlens_comparison.bmp",
                            static_cast<uint16_t>(width * 2), static_cast<uint16_t>(height));

  std::printf("ThinLens example: F=%.1f R=%.1f pupils=%zu image=%ux%u\n",
              focal_F, aperture_R, samples.size(), width, height);
  std::printf("Wrote thinlens_pinhole.bmp, thinlens_dof.bmp, thinlens_comparison.bmp\n");
  return 0;
}
