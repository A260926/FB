// CPU section preview: renders an obliquely cut hollow box with the real
// production rasterizer interfaces and writes a binary PPM (P6) to the path
// given as the only argument. No mesh is modified and no cap mesh is exported.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#include "World.h"
#include "ForwardRasterizer.h"
#include "PERSPECTIVECAMERA.H"
#include "Material.h"
#include "DIRECTIONALLIGHT.H"
#include "SectionPass.h"

namespace {

struct BoxMesh {
  std::vector<Point3D> vertices;
  std::vector<uint32_t> indices;
};

// Axis aligned box centered at c with half extent h. outward == false reverses
// every triangle, producing the inward shell of a cavity.
BoxMesh makeBox(const Point3D& c, double h, bool outward) {
  const Point3D p0(c.x - h, c.y - h, c.z - h);
  const Point3D p1(c.x + h, c.y - h, c.z - h);
  const Point3D p2(c.x + h, c.y + h, c.z - h);
  const Point3D p3(c.x - h, c.y + h, c.z - h);
  const Point3D p4(c.x - h, c.y - h, c.z + h);
  const Point3D p5(c.x + h, c.y - h, c.z + h);
  const Point3D p6(c.x + h, c.y + h, c.z + h);
  const Point3D p7(c.x - h, c.y + h, c.z + h);

  BoxMesh mesh;
  mesh.vertices = {p0, p1, p2, p3, p4, p5, p6, p7};
  const std::vector<uint32_t> outward_indices = {
    0, 3, 2, 0, 2, 1,
    4, 5, 6, 4, 6, 7,
    0, 1, 5, 0, 5, 4,
    3, 7, 6, 3, 6, 2,
    0, 4, 7, 0, 7, 3,
    1, 2, 6, 1, 6, 5
  };
  mesh.indices.reserve(outward_indices.size());
  for (size_t i = 0; i < outward_indices.size(); i += 3) {
    if (outward) {
      mesh.indices.push_back(outward_indices[i]);
      mesh.indices.push_back(outward_indices[i + 1]);
      mesh.indices.push_back(outward_indices[i + 2]);
    } else {
      mesh.indices.push_back(outward_indices[i + 2]);
      mesh.indices.push_back(outward_indices[i + 1]);
      mesh.indices.push_back(outward_indices[i]);
    }
  }
  return mesh;
}

GeometryObject* makeObject(Material* material, const BoxMesh& mesh) {
  return new GeometryObject(material, mesh.vertices, std::vector<RGBColor>(),
                            std::vector<Vector2D>(), mesh.indices);
}

// One shell collection: outward outer shell plus reversed inner cavity shell.
BoxMesh makeHollowBox(const Point3D& c, double outer_h, double inner_h) {
  BoxMesh outer = makeBox(c, outer_h, true);
  BoxMesh inner = makeBox(c, inner_h, false);
  BoxMesh merged = outer;
  const uint32_t offset = static_cast<uint32_t>(merged.vertices.size());
  for (const Point3D& vertex : inner.vertices) merged.vertices.push_back(vertex);
  for (uint32_t index : inner.indices) merged.indices.push_back(index + offset);
  return merged;
}

void writePPM(const std::string& path, const std::vector<RGBColor>& pixels,
              uint16_t width, uint16_t height) {
  std::ofstream file(path, std::ios::binary);
  file << "P6\n" << width << " " << height << "\n255\n";
  for (const RGBColor& pixel : pixels) {
    const unsigned char rgb[3] = {
      static_cast<unsigned char>(std::min(std::max(pixel.r, 0.0f), 1.0f) * 255.0f),
      static_cast<unsigned char>(std::min(std::max(pixel.g, 0.0f), 1.0f) * 255.0f),
      static_cast<unsigned char>(std::min(std::max(pixel.b, 0.0f), 1.0f) * 255.0f)
    };
    file.write(reinterpret_cast<const char*>(rgb), 3);
  }
}

struct PreviewProbe : ForwardRasterizer {
  using ForwardRasterizer::ForwardRasterizer;
  const std::vector<RGBColor>& pixels() const { return m_pixels; }
};

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    return 1;
  }

  const uint16_t width = 64;
  const uint16_t height = 64;

  std::vector<Light*> lights = {
    new DirectionalLight(RGBColor(1.0f, 1.0f, 1.0f), Vector3D(0.3, 0.5, -1.0))
  };
  FlatMaterial material(RGBColor(0.4f, 0.4f, 0.5f), RGBColor(0.2f), 4.0f);

  // One hollow object: an outward outer shell plus a reversed inner shell.
  GeometryObject* hollow = makeObject(&material,
                                      makeHollowBox(Point3D(0, 0, 15), 4.0, 2.0));
  const std::vector<GeometryObject*> objects = {hollow};

  World world(objects, lights, nullptr);
  PreviewProbe renderer(&world);
  PerspectiveCamera camera(Point3D(0, 0, 0), Point3D(0, 0, 1),
                           height, width, &renderer, 1, 100, 90.0f);
  world.m_camera = &camera;

  // Oblique cut: 0.3 x + z = 14, keeping 0.3 x + z - 14 >= 0.
  renderer.enableSection(Vector3D(0.3, 0.0, 1.0), -14.0,
                         RGBColor(0.2f, 0.7f, 0.3f),  // cap color
                         RGBColor(0.3f, 0.6f, 0.2f),  // cap diffuse
                         RGBColor(0.8f, 0.8f, 0.1f)); // cap specular
  renderer.render(true, false);

  writePPM(argv[1], renderer.pixels(), width, height);

  // GeometryObject has a concrete destructor; Light's destructor is not
  // virtual, so lights are intentionally not deleted through the base
  // pointer (consistent with the rest of the project).
  delete hollow;
  return 0;
}
