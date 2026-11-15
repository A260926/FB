#include <Tools.hpp>
#include <loader/TextureLoader.hpp>
#include <memory>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

SoftRasterizer::TextureLoader::TextureLoader(const std::string &path) {
  int channels = 0;
  std::unique_ptr<unsigned char, decltype(&stbi_image_free)> bytes(
      stbi_load(path.c_str(), &m_width, &m_height, &channels, 3),
      &stbi_image_free);
  if (!bytes) {
    throw std::runtime_error("Cannot decode texture " + path + ": " +
                             stbi_failure_reason());
  }
  m_pixels.resize(std::size_t(m_width) * m_height);
  for (std::size_t i = 0; i < m_pixels.size(); ++i) {
    for (int c = 0; c < 3; ++c) {
      m_pixels[i][c] = srgbToLinear(bytes.get()[3 * i + c] / 255.f);
    }
  }
}

SoftRasterizer::TextureLoader::TextureLoader(
    int width, int height, std::vector<glm::vec3> linearPixels)
    : m_width(width), m_height(height), m_pixels(std::move(linearPixels)) {
  if (width <= 0 || height <= 0 ||
      m_pixels.size() != std::size_t(width) * height) {
    throw std::invalid_argument("invalid texture size");
  }
  for (const auto &pixelColor : m_pixels) {
    if (!finite(pixelColor)) {
      throw std::invalid_argument("invalid texture pixel");
    }
  }
}

glm::vec3
SoftRasterizer::TextureLoader::getTextureColor(const glm::vec2 &uv) const {
  if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
    return {};
  }
  const float x = std::clamp(uv.x, 0.f, 1.f) * (m_width - 1),
              y = (1 - std::clamp(uv.y, 0.f, 1.f)) * (m_height - 1);
  int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, m_width - 1),
      y1 = std::min(y0 + 1, m_height - 1);
  auto top = glm::mix(m_pixels[std::size_t(y0) * m_width + x0],
                      m_pixels[std::size_t(y0) * m_width + x1], x - x0);
  auto bottom = glm::mix(m_pixels[std::size_t(y1) * m_width + x0],
                         m_pixels[std::size_t(y1) * m_width + x1], x - x0);
  return glm::mix(top, bottom, y - y0);
}
