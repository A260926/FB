#pragma once
#include <scene/Scene.hpp>
#include <string>

namespace SoftRasterizer {
enum class Buffers { Color = 1, Depth = 2 };

inline Buffers operator|(Buffers a, Buffers b) {
  return static_cast<Buffers>(static_cast<int>(a) | static_cast<int>(b));
}
enum class Primitive { LINES, TRIANGLES };

class RenderingPipeline {
public:
  RenderingPipeline(std::size_t width = 800, std::size_t height = 600);
  virtual ~RenderingPipeline() = default;
  virtual void draw(Primitive type = Primitive::TRIANGLES) = 0;
  bool addScene(std::shared_ptr<Scene> scene);
  void clear(Buffers flags = Buffers::Color | Buffers::Depth);
  void display(Primitive type = Primitive::TRIANGLES);
  void save(const std::string &path, float exposure = 1) const;

  const std::vector<glm::vec3> &pixels() const {
    return m_color;
  }

  const std::vector<float> &depth() const {
    return m_depth;
  }

  std::size_t width() const {
    return m_width;
  }

  std::size_t height() const {
    return m_height;
  }

protected:
  std::size_t m_width, m_height;
  std::vector<glm::vec3> m_color;
  std::vector<float> m_depth;
  std::vector<std::shared_ptr<Scene>> m_scenes;
};
} // namespace SoftRasterizer
