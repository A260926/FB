#include <scene/Camera.hpp>

void SoftRasterizer::Camera::lookAt(const glm::vec3 &eye,
                                    const glm::vec3 &center,
                                    const glm::vec3 &up) {
  if (!finite(eye) || !finite(center) || !finite(up) ||
      glm::length(center - eye) < 1e-6f ||
      glm::length(glm::cross(center - eye, up)) < 1e-6f) {
    throw std::invalid_argument("invalid camera basis");
  }
  m_eye = eye;
  m_center = center;
  m_up = unit(up);
}

void SoftRasterizer::Camera::projection(float degrees, float nearDistance,
                                        float farDistance) {
  if (!std::isfinite(degrees) || !std::isfinite(nearDistance) ||
      !std::isfinite(farDistance) || degrees <= 0 || degrees >= 179 ||
      nearDistance <= 0 || farDistance <= nearDistance) {
    throw std::invalid_argument("invalid camera projection");
  }
  m_fovy = degrees;
  m_near = nearDistance;
  m_far = farDistance;
}

SoftRasterizer::Ray SoftRasterizer::Camera::generateRay(
    float pixelX, float pixelY, std::size_t width, std::size_t height) const {
  if (!width || !height) {
    throw std::invalid_argument("zero camera resolution");
  }
  const auto forward = unit(m_center - m_eye),
             right = unit(glm::cross(forward, m_up)),
             vertical = glm::cross(right, forward);
  const float scale = std::tan(glm::radians(m_fovy) * 0.5f);
  const float x = (2 * pixelX / float(width) - 1) *
                  (float(width) / float(height)) * scale,
              y = (1 - 2 * pixelY / float(height)) * scale;
  const auto direction = unit(forward + x * right + y * vertical);
  const float cosine = glm::dot(direction, forward);
  return {m_eye, direction, m_near / cosine, m_far / cosine};
}
