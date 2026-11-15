#pragma once
#include <Tools.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <ray/Ray.hpp>

namespace SoftRasterizer {
class Camera {
public:
  void lookAt(const glm::vec3 &eye, const glm::vec3 &center,
              const glm::vec3 &up);
  void projection(float verticalDegrees, float nearDistance, float farDistance);

  glm::mat4 view() const {
    return glm::lookAtRH(m_eye, m_center, m_up);
  }

  glm::mat4 projectionMatrix(float aspect) const {
    if (!std::isfinite(aspect) || aspect <= 0) {
      throw std::invalid_argument("invalid aspect ratio");
    }
    return glm::perspectiveRH_NO(glm::radians(m_fovy), aspect, m_near, m_far);
  }

  Ray generateRay(float pixelX, float pixelY, std::size_t width,
                  std::size_t height) const;

  glm::vec3 position() const {
    return m_eye;
  }

private:
  glm::vec3 m_eye{0, 0, 3}, m_center{0}, m_up{0, 1, 0};
  float m_fovy = 45, m_near = 0.05f, m_far = 100;
};
} // namespace SoftRasterizer
