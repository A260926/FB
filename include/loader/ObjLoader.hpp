#pragma once
#include <object/Mesh.hpp>
#include <string>

namespace SoftRasterizer {
class ObjLoader {
public:
  explicit ObjLoader(const std::string &path,
                     const glm::mat4 &modelMatrix = glm::mat4(1.f));
  std::shared_ptr<Mesh> load() const;

private:
  std::string m_path;
  glm::mat4 m_model{1};
};
} // namespace SoftRasterizer
