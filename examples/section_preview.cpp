// Section preview example: renders a closed box with an internal cavity and
// cuts it with an oblique world-space section plane. Only an image is
// produced (PPM at the given output path); triangle meshes are never
// modified and no cap mesh is exported.

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "World.h"
#include "ForwardRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "Material.h"
#include "PointLight.h"
#include "SectionPass.h"

namespace {

// Axis-aligned box centered at the origin with half extents (hx, hy, hz).
// outward_winding == false reverses every triangle, producing an inward
// facing cavity shell.
void appendBox(std::vector<Point3D>& vertices, std::vector<uint32_t>& indices,
               const double hx, const double hy, const double hz,
               const bool outward_winding) {
  const uint32_t base = static_cast<uint32_t>(vertices.size());
  const std::vector<Point3D> box_vertices = {
    Point3D(-hx, -hy, -hz), Point3D(hx, -hy, -hz),
    Point3D(hx, hy, -hz), Point3D(-hx, hy, -hz),
    Point3D(-hx, -hy, hz), Point3D(hx, -hy, hz),
    Point3D(hx, hy, hz), Point3D(-hx, hy, hz)
  };
  const std::vector<uint32_t> outward = {
    0, 3, 2, 0, 2, 1,       // -z
    4, 5, 6, 4, 6, 7,       // +z
    0, 1, 5, 0, 5, 4,       // -y
    3, 7, 6, 3, 6, 2,       // +y
    0, 4, 7, 0, 7, 3,       // -x
    1, 2, 6, 1, 6, 5        // +x
  };
  for (const Point3D& vertex : box_vertices) {
    vertices.push_back(vertex);
  }
  for (size_t i = 0; i < outward.size(); i += 3) {
    if (outward_winding) {
      indices.push_back(base + outward[i]);
      indices.push_back(base + outward[i + 1]);
      indices.push_back(base + outward[i + 2]);
    } else {
      indices.push_back(base + outward[i]);
      indices.push_back(base + outward[i + 2]);
      indices.push_back(base + outward[i + 1]);
    }
  }
}

class PpmProbe : public ForwardRasterizer {
 public:
  using ForwardRasterizer::ForwardRasterizer;
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
};

void writePpm(const char* path, const std::vector<RGBColor>& pixels,
              const uint32_t width, const uint32_t height) {
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return;
  }
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  for (const RGBColor& pixel : pixels) {
    const uint8_t color[3] = {
      static_cast<uint8_t>(std::min(pixel.r, 1.0f) * 255.0f),
      static_cast<uint8_t>(std::min(pixel.g, 1.0f) * 255.0f),
      static_cast<uint8_t>(std::min(pixel.b, 1.0f) * 255.0f)
    };
    std::fwrite(color, 1, 3, file);
  }
  std::fclose(file);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
    return 1;
  }

  const uint32_t width = 64;
  const uint32_t height = 64;

  World world;
  PpmProbe renderer(&world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1),
                           height, width, &renderer, 1, 60, 90.0f);
  world.m_camera = &camera;

  FlatMaterial shell_material(RGBColor(0.35f, 0.35f, 0.45f),
                              RGBColor(0.25f, 0.25f, 0.3f), 8.0f);

  // One closed object: outward outer shell plus a reversed inward-facing
  // cavity shell. Non-zero winding subtracts the cavity inside the object.
  std::vector<Point3D> vertices;
  std::vector<uint32_t> indices;
  appendBox(vertices, indices, 3.0, 3.0, 3.0, true);
  appendBox(vertices, indices, 1.4, 1.4, 1.4, false);
  const std::vector<RGBColor> colors(vertices.size(),
                                     RGBColor(1.0f, 1.0f, 1.0f));

  GeometryObject box(&shell_material, vertices, colors,
                     std::vector<Vector2D>(), indices, Point3D(0, 0, 8));
  world.m_objects = {&box};

  PointLight light(RGBColor(1.0f, 1.0f, 1.0f), Point3D(-6, 8, -4));
  world.m_lights = {&light};

  // Oblique cut keeping n.p + d >= 0 (normal and d are normalized together).
  // The retained region y + z >= 9 removes the front of the shell over the
  // box center, exposing a cap annulus with the cavity opening in the middle.
  renderer.enableSection(Vector3D(0.0, 1.0, 1.0), -9.0,
                         RGBColor(0.9f, 0.45f, 0.2f),
                         RGBColor(0.8f, 0.4f, 0.2f),
                         RGBColor(0.15f, 0.15f, 0.15f));
  renderer.render(true, false);

  writePpm(argv[1], renderer.pixels(), width, height);
  return 0;
}
