#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace SoftRasterizer {
class TextureLoader {
public:
  explicit TextureLoader(const std::string &path);
  TextureLoader(int width, int height, std::vector<glm::vec3> linearPixels);
  // OBJ UVs: origin bottom-left. File storage: row zero at top.
  glm::vec3 getTextureColor(const glm::vec2 &uv) const;

private:
  int m_width = 0, m_height = 0;
  std::vector<glm::vec3> m_pixels;
};
} // namespace SoftRasterizer
