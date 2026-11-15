#include <object/Object.hpp>

void SoftRasterizer::Object::setModelMatrix(const glm::mat4 &modelMatrix) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      if (!std::isfinite(modelMatrix[c][r])) {
        throw std::invalid_argument("non-finite model matrix");
      }
    }
  }

  //ensure the model matrix is affine and invertible
  const float determinant = glm::determinant(glm::mat3(modelMatrix));
  if (std::abs(modelMatrix[0][3]) + std::abs(modelMatrix[1][3]) +
              std::abs(modelMatrix[2][3]) + std::abs(modelMatrix[3][3] - 1) >
          1e-6f ||
      !std::isfinite(determinant) || std::abs(determinant) < 1e-12f) {
    throw std::invalid_argument("model matrix must be affine and invertible");
  }
  /*Restore the previous model if the derived geometry rejects this transform.*/
  auto previousModel = m_model;
  m_model = modelMatrix;
  try {
    transform();
  } catch (...) {
    m_model = previousModel;
    transform();
    throw;
  }
  ++m_revision;
}

void SoftRasterizer::Object::updateModelMatrix(const glm::vec3 &axis,
                                               float angle,
                                               const glm::vec3 &translation,
                                               const glm::vec3 &scale) {
  setModelMatrix(composeTransform(axis, angle, translation, scale));
}

SoftRasterizer::Vertex::Vertex(const glm::vec3 &position,
                               const glm::vec3 &normal,
                               const glm::vec2 &texCoord)
    : position(position), normal(normal), texCoord(texCoord) {}

void SoftRasterizer::Object::setMaterial(std::shared_ptr<Material> material) {
  if (!material) {
    throw std::invalid_argument("null material");
  }
  m_material = std::move(material);
  ++m_revision;
}

void SoftRasterizer::Object::bindShader2Mesh(std::shared_ptr<Shader> shader) {
  m_shader = std::move(shader);
  if (m_shader && m_shader->texture) {
    m_material->texture = m_shader->texture;
  }
  ++m_revision;
}
