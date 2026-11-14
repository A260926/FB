//
// Renders the analytic visibility of a rectangular area light across a
// receiver plane. Each pixel is one receiver point; the gray value is
// round(25 + 230 * visible), so a finite occluder produces a fully lit
// region, a central umbra and a continuous analytic penumbra without any
// ray sampling.
//
// Output: a 96x64 ASCII P3 grayscale PPM given as argv[1].
//
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

#include "analytic_visibility.h"
#include "nanort.h"

namespace {

int RoundGray(double visible) {
  int c = static_cast<int>(std::floor(25.0 + 230.0 * visible + 0.5));
  if (c < 0) c = 0;
  if (c > 255) c = 255;
  return c;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " output.ppm\n";
    return 2;
  }

  // One double-sided opaque occluder quad at z = 5, split into two
  // triangles. It is smaller than the light, so receivers see an umbra
  // plus a soft penumbra around the quad's silhouette.
  std::vector<double> vertices;
  // quad corners: (-1.5,-1.5,5) (1.5,-1.5,5) (1.5,1.5,5) (-1.5,1.5,5)
  const double q[4][3] = {{-1.5, -1.5, 5.0},
                          {1.5, -1.5, 5.0},
                          {1.5, 1.5, 5.0},
                          {-1.5, 1.5, 5.0}};
  for (int i = 0; i < 4; ++i) {
    vertices.push_back(q[i][0]);
    vertices.push_back(q[i][1]);
    vertices.push_back(q[i][2]);
  }
  const std::vector<unsigned int> faces = {0, 1, 2, 0, 2, 3};

  nanort::TriangleMesh<double> mesh(&vertices[0], &faces[0],
                                    3 * sizeof(double));
  nanort::TriangleSAHPred<double> pred(&vertices[0], &faces[0],
                                       3 * sizeof(double));
  nanort::BVHAccel<double> bvh;
  if (!bvh.Build(static_cast<unsigned int>(faces.size() / 3), mesh, pred)) {
    std::cerr << "BVH build failed\n";
    return 3;
  }

  // Rectangle light centered at the origin direction on z = 10.
  const area_visibility::Rectangle light(
      area_visibility::Vec3(-2.0, -2.0, 10.0),
      area_visibility::Vec3(4.0, 0.0, 0.0),
      area_visibility::Vec3(0.0, 4.0, 0.0));

  const int width = 96;
  const int height = 64;

  std::ofstream out(argv[1]);
  if (!out) {
    std::cerr << "cannot open " << argv[1] << "\n";
    return 4;
  }
  out << "P3\n" << width << " " << height << "\n255\n";

  for (int py = 0; py < height; ++py) {
    for (int px = 0; px < width; ++px) {
      // Map pixels onto the receiver plane z = 0.
      const double rx = -4.0 + 8.0 * px / (width - 1);
      const double ry = -3.0 + 6.0 * (height - 1 - py) / (height - 1);
      const area_visibility::Vec3 receiver(rx, ry, 0.0);

      const area_visibility::VisibilityResult r =
          area_visibility::query(bvh, vertices, faces, receiver, light);
      const int gray = RoundGray(r.visible);
      out << gray << " " << gray << " " << gray << "\n";
    }
  }

  std::cout << "PPM written " << width << "x" << height << "\n";
  return 0;
}
