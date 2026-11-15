#include <object/Cube.hpp>

static std::vector<SoftRasterizer::Vertex> cubeVertices() {
  std::vector<SoftRasterizer::Vertex> vertices;
  for (auto position : std::array<glm::vec3, 8>{{{-1, -1, -1},
                                                 {1, -1, -1},
                                                 {1, 1, -1},
                                                 {-1, 1, -1},
                                                 {-1, -1, 1},
                                                 {1, -1, 1},
                                                 {1, 1, 1},
                                                 {-1, 1, 1}}}) {
    vertices.emplace_back(0.5f * position);
  }
  return vertices;
}

SoftRasterizer::Cube::Cube()
    : Mesh(cubeVertices(), {{0, 2, 1},
                            {0, 3, 2},
                            {4, 5, 6},
                            {4, 6, 7},
                            {0, 1, 5},
                            {0, 5, 4},
                            {3, 7, 6},
                            {3, 6, 2},
                            {0, 4, 7},
                            {0, 7, 3},
                            {1, 2, 6},
                            {1, 6, 5}}) {}
