#include <filesystem>
#include <iostream>
#include <limits>
#include <loader/ObjLoader.hpp>
#include <map>
#include <tiny_obj_loader.h>
#include <tuple>

namespace {
using SoftRasterizer::Material;
using SoftRasterizer::MaterialType;
using SoftRasterizer::TextureLoader;

/*Convert MTL materials and share each texture within this model.*/
std::vector<std::shared_ptr<Material>>
loadMaterials(const std::vector<tinyobj::material_t> &objMaterials,
              const std::filesystem::path &modelDirectory) {
  std::vector<std::shared_ptr<Material>> materials;
  std::map<std::string, std::shared_ptr<TextureLoader>> textures;
  for (const auto &objMaterial : objMaterials) {
    auto material = std::make_shared<Material>();
    material->Kd = {objMaterial.diffuse[0], objMaterial.diffuse[1],
                    objMaterial.diffuse[2]};
    material->Ks = {objMaterial.specular[0], objMaterial.specular[1],
                    objMaterial.specular[2]};
    material->emission = {objMaterial.emission[0], objMaterial.emission[1],
                          objMaterial.emission[2]};
    material->ior = objMaterial.ior;
    material->specularExponent = objMaterial.shininess;
    if (objMaterial.illum == 5) {
      material->type = MaterialType::REFLECTION;
    }
    if (objMaterial.illum == 4 || objMaterial.illum == 6 ||
        objMaterial.illum == 7 || objMaterial.illum == 9) {
      material->type = MaterialType::REFLECTION_AND_REFRACTION;
    }
    if (!objMaterial.diffuse_texname.empty()) {
      auto file = (modelDirectory / objMaterial.diffuse_texname)
                      .lexically_normal()
                      .string();
      auto &texture = textures[file];
      if (!texture) {
        texture = std::make_shared<TextureLoader>(file);
      }
      material->texture = texture;
    }
    materials.push_back(std::move(material));
  }
  return materials;
}
} // namespace

SoftRasterizer::ObjLoader::ObjLoader(const std::string &path,
                                     const glm::mat4 &modelMatrix)
    : m_path(path), m_model(modelMatrix) {}

/*Load every OBJ shape, retaining per-face materials and per-corner
 * attributes.*/
std::shared_ptr<SoftRasterizer::Mesh> SoftRasterizer::ObjLoader::load() const {
  const auto modelDirectory = std::filesystem::path(m_path).parent_path();
  tinyobj::ObjReaderConfig config;
  config.triangulate = true;
  config.mtl_search_path = modelDirectory.string();
  tinyobj::ObjReader reader;
  if (!reader.ParseFromFile(m_path, config)) {
    throw std::runtime_error("Cannot load OBJ " + m_path + ": " +
                             reader.Error());
  }
  if (!reader.Warning().empty()) {
    std::cerr << "OBJ warning: " << reader.Warning() << '\n';
  }
  const auto &attrib = reader.GetAttrib();
  /*Load materials before assembling indexed geometry.*/
  auto materials = loadMaterials(reader.GetMaterials(), modelDirectory);
  auto fallback = std::make_shared<Material>();
  std::vector<Vertex> vertices;
  std::vector<glm::uvec3> faces;
  std::vector<std::shared_ptr<Material>> faceMaterials;
  // Missing normals use flat faces (smoothing group 0) or area-weighted
  // smoothing groups.
  using VertexKey = std::tuple<int, int, int, int, std::size_t>;
  std::map<VertexKey, std::uint32_t> uniqueVertices;
  std::map<std::pair<int, int>, glm::vec3> smoothNormals;
  std::vector<std::pair<int, int>> smoothKeys;
  std::vector<bool> suppliedNormals;
  std::size_t faceIndex = 0;
  /*Assemble every shape, reusing vertices with matching corner attributes.*/
  for (const auto &shape : reader.GetShapes()) {
    std::size_t offset = 0;
    for (std::size_t shapeFaceIndex = 0;
         shapeFaceIndex < shape.mesh.num_face_vertices.size();
         ++shapeFaceIndex, ++faceIndex) {
      const auto cornerCount = shape.mesh.num_face_vertices[shapeFaceIndex];
      if (cornerCount != 3 || offset + 3 > shape.mesh.indices.size()) {
        throw std::runtime_error("OBJ triangulation failed");
      }
      const int smoothingGroup =
          shapeFaceIndex < shape.mesh.smoothing_group_ids.size()
              ? int(shape.mesh.smoothing_group_ids[shapeFaceIndex])
              : 0;
      glm::uvec3 face;
      for (int corner = 0; corner < 3; ++corner) {
        auto objIndex = shape.mesh.indices[offset + corner];
        if (objIndex.vertex_index < 0 ||
            std::size_t(objIndex.vertex_index) * 3 + 2 >=
                attrib.vertices.size()) {
          throw std::runtime_error("invalid OBJ vertex index");
        }
        VertexKey key{
            objIndex.vertex_index, objIndex.normal_index,
            objIndex.texcoord_index, smoothingGroup,
            (objIndex.normal_index < 0 && smoothingGroup == 0) ? faceIndex : 0};
        auto vertexIter = uniqueVertices.find(key);
        if (vertexIter == uniqueVertices.end()) {
          if (vertices.size() >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("OBJ too large");
          }
          Vertex vertex;
          auto positionOffset = std::size_t(objIndex.vertex_index) * 3;
          vertex.position = {attrib.vertices[positionOffset],
                             attrib.vertices[positionOffset + 1],
                             attrib.vertices[positionOffset + 2]};
          if (positionOffset + 2 < attrib.colors.size()) {
            vertex.color = {attrib.colors[positionOffset],
                            attrib.colors[positionOffset + 1],
                            attrib.colors[positionOffset + 2]};
          }
          if (objIndex.normal_index >= 0) {
            auto normalOffset = std::size_t(objIndex.normal_index) * 3;
            if (normalOffset + 2 >= attrib.normals.size()) {
              throw std::runtime_error("invalid OBJ normal index");
            }
            vertex.normal = unit({attrib.normals[normalOffset],
                                  attrib.normals[normalOffset + 1],
                                  attrib.normals[normalOffset + 2]});
          }
          if (objIndex.texcoord_index >= 0) {
            auto texCoordOffset = std::size_t(objIndex.texcoord_index) * 2;
            if (texCoordOffset + 1 >= attrib.texcoords.size()) {
              throw std::runtime_error("invalid OBJ UV index");
            }
            vertex.texCoord = {attrib.texcoords[texCoordOffset],
                               attrib.texcoords[texCoordOffset + 1]};
          }
          auto index = static_cast<std::uint32_t>(vertices.size());
          vertexIter = uniqueVertices.emplace(key, index).first;
          vertices.push_back(vertex);
          smoothKeys.emplace_back(objIndex.vertex_index, smoothingGroup);
          suppliedNormals.push_back(objIndex.normal_index >= 0);
        }
        face[corner] = vertexIter->second;
      }
      offset += 3;
      auto normal =
          glm::cross(vertices[face.y].position - vertices[face.x].position,
                     vertices[face.z].position - vertices[face.x].position);
      for (int corner = 0; corner < 3; ++corner) {
        if (smoothingGroup > 0) {
          smoothNormals.try_emplace(smoothKeys[face[corner]], glm::vec3(0))
              .first->second += normal;
        } else if (!suppliedNormals[face[corner]]) {
          vertices[face[corner]].normal = unit(normal);
        }
      }
      faces.push_back(face);
      int materialIndex = shapeFaceIndex < shape.mesh.material_ids.size()
                              ? shape.mesh.material_ids[shapeFaceIndex]
                              : -1;
      faceMaterials.push_back(materialIndex >= 0 && std::size_t(materialIndex) <
                                                        materials.size()
                                  ? materials[materialIndex]
                                  : fallback);
    }
  }
  /*Fill missing smooth normals after all contributing faces are known.*/
  for (std::size_t vertexIndex = 0; vertexIndex < vertices.size();
       ++vertexIndex) {
    if (!suppliedNormals[vertexIndex] && smoothKeys[vertexIndex].second != 0) {
      vertices[vertexIndex].normal =
          unit(smoothNormals[smoothKeys[vertexIndex]]);
    }
  }
  auto mesh = std::make_shared<Mesh>(std::move(vertices), std::move(faces),
                                     std::move(faceMaterials));
  mesh->setModelMatrix(m_model);
  return mesh;
}
