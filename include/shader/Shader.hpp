#pragma once
#include <object/Material.hpp>

namespace SoftRasterizer {
enum class SHADERS_TYPE { NORMAL, TEXTURE, PHONG, DISPLACEMENT, BUMP };

struct Shader {
  SHADERS_TYPE type = SHADERS_TYPE::PHONG;
  std::shared_ptr<TextureLoader> texture;

  explicit Shader(const std::string &path)
      : texture(std::make_shared<TextureLoader>(path)) {}

  explicit Shader(std::shared_ptr<TextureLoader> t = nullptr)
      : texture(std::move(t)) {}

  void setFragmentShader(SHADERS_TYPE t) {
    if (t == SHADERS_TYPE::BUMP || t == SHADERS_TYPE::DISPLACEMENT) {
      throw std::invalid_argument(
          "Bump/displacement shaders are not implemented");
    }
    type = t;
  }
};
} // namespace SoftRasterizer
