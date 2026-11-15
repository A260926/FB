#include "ObjLoader.h"

#include <Colour.h>
#include <TextureMap.h>
#include <cmath>
#include <fstream>
#include <glm/glm.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct MaterialDef {
	Colour colour;
	std::string mapKd; // texture filename, empty if none
};
} // namespace

// Parse a Wavefront .mtl file into a name -> material map.
static std::map<std::string, MaterialDef> loadMTL(const std::string &filename) {
	std::map<std::string, MaterialDef> materials;
	std::ifstream file(filename);
	if (!file.is_open()) {
		std::cout << "Failed to open material file: " << filename << std::endl;
		return materials;
	}
	std::string line;
	std::string currentName;
	while (std::getline(file, line)) {
		std::stringstream ss(line);
		std::string type;
		ss >> type;
		if (type == "newmtl") {
			ss >> currentName;
		} else if (type == "Kd" && !currentName.empty()) {
			float r, g, b;
			ss >> r >> g >> b;
			materials[currentName].colour =
			    Colour(currentName, static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255));
		} else if (type == "map_Kd" && !currentName.empty()) {
			ss >> materials[currentName].mapKd;
		}
	}
	return materials;
}

// Map a material name to a surface response.
static Material materialFor(const std::string &name) {
	if (name == "Mirror")
		return Material::Mirror;
	if (name == "Glass")
		return Material::Glass;
	if (name == "Marble")
		return Material::Procedural;
	if (name == "Bumpy")
		return Material::Bump;
	if (name == "Parallax")
		return Material::Parallax;
	if (name == "Metal")
		return Material::Metal;
	if (name == "Dispersive" || name == "Prism")
		return Material::Dispersive;
	if (name == "Subsurface" || name == "SSS" || name == "Wax" || name == "Jade")
		return Material::Subsurface;
	if (name == "NormalMap" || name == "Normal")
		return Material::NormalMap;
	if (name == "Emissive" || name == "Emit" || name == "Glow")
		return Material::Emissive;
	return Material::Diffuse;
}

// Parse one face token ("v", "v/vt", "v/vt/vn", "v//vn") into 0-based indices.
static void parseFaceToken(const std::string &token, int &v, int &vt) {
	v = -1;
	vt = -1;
	size_t firstSlash = token.find('/');
	if (firstSlash == std::string::npos) {
		v = std::stoi(token) - 1;
		return;
	}
	v = std::stoi(token.substr(0, firstSlash)) - 1;
	size_t secondSlash = token.find('/', firstSlash + 1);
	std::string vtStr = (secondSlash == std::string::npos) ? token.substr(firstSlash + 1)
	                                                       : token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
	if (!vtStr.empty())
		vt = std::stoi(vtStr) - 1;
}

namespace {
struct Face {
	int a, b, c;    // vertex indices
	int ta, tb, tc; // texture-coord indices (-1 if none)
	Colour colour;
	Material material;
	std::shared_ptr<TextureMap> texture;
};
} // namespace

std::vector<ModelTriangle> loadOBJ(const std::string &filename, float scale) {
	std::vector<ModelTriangle> triangles;
	std::ifstream file(filename);
	if (!file.is_open()) {
		std::cout << "Failed to open file: " << filename << std::endl;
		return triangles;
	}
	std::string directory;
	size_t slash = filename.find_last_of("/\\");
	if (slash != std::string::npos)
		directory = filename.substr(0, slash + 1);

	std::map<std::string, MaterialDef> materials;
	std::map<std::string, std::shared_ptr<TextureMap>> textureCache;
	std::vector<glm::vec3> vertices;
	std::vector<glm::vec2> texCoords;
	std::vector<Face> faces;
	Colour currentColour;
	Material currentMaterial = Material::Diffuse;
	std::shared_ptr<TextureMap> currentTexture;
	std::string line;
	while (std::getline(file, line)) {
		std::stringstream ss(line);
		std::string type;
		ss >> type;
		if (type == "mtllib") {
			std::string mtlName;
			ss >> mtlName;
			materials = loadMTL(directory + mtlName);
		} else if (type == "usemtl") {
			std::string mtlName;
			ss >> mtlName;
			currentMaterial = materialFor(mtlName);
			currentTexture = nullptr;
			auto it = materials.find(mtlName);
			if (it != materials.end()) {
				currentColour = it->second.colour;
				if (!it->second.mapKd.empty()) {
					std::string path = directory + it->second.mapKd;
					auto cached = textureCache.find(path);
					if (cached == textureCache.end()) {
						currentTexture = std::make_shared<TextureMap>(path);
						textureCache[path] = currentTexture;
					} else {
						currentTexture = cached->second;
					}
				}
			}
		} else if (type == "v") {
			float x, y, z;
			ss >> x >> y >> z;
			vertices.push_back(glm::vec3(x * scale, y * scale, z * scale));
		} else if (type == "vt") {
			float u, v;
			ss >> u >> v;
			texCoords.push_back(glm::vec2(u, v));
		} else if (type == "f") {
			std::string t1, t2, t3;
			ss >> t1 >> t2 >> t3;
			int a, b, c, ta, tb, tc;
			parseFaceToken(t1, a, ta);
			parseFaceToken(t2, b, tb);
			parseFaceToken(t3, c, tc);
			faces.push_back({a, b, c, ta, tb, tc, currentColour, currentMaterial, currentTexture});
		}
	}

	// Face normals (guard degenerate triangles so we never divide by zero).
	std::vector<glm::vec3> faceNormals(faces.size());
	for (size_t i = 0; i < faces.size(); i++) {
		const Face &f = faces[i];
		glm::vec3 cross = glm::cross(vertices[f.b] - vertices[f.a], vertices[f.c] - vertices[f.a]);
		float len = glm::length(cross);
		faceNormals[i] = (len > 1e-8f) ? cross / len : glm::vec3(0.0f, 1.0f, 0.0f);
	}

	// Which faces touch each vertex, so we can average their normals.
	std::vector<std::vector<int>> vertexFaces(vertices.size());
	for (size_t i = 0; i < faces.size(); i++) {
		vertexFaces[faces[i].a].push_back(static_cast<int>(i));
		vertexFaces[faces[i].b].push_back(static_cast<int>(i));
		vertexFaces[faces[i].c].push_back(static_cast<int>(i));
	}

	// Smooth a vertex normal by averaging only the adjacent face normals within
	// ~60 degrees, so hard edges stay crisp while curved surfaces are smoothed.
	const float threshold = 0.5f;
	auto smoothNormal = [&](int vertexIndex, size_t faceIndex) {
		glm::vec3 sum(0.0f);
		for (int g : vertexFaces[vertexIndex]) {
			if (glm::dot(faceNormals[g], faceNormals[faceIndex]) > threshold)
				sum += faceNormals[g];
		}
		return (glm::length(sum) > 0.0f) ? glm::normalize(sum) : faceNormals[faceIndex];
	};

	triangles.reserve(faces.size());
	for (size_t i = 0; i < faces.size(); i++) {
		const Face &f = faces[i];
		ModelTriangle triangle(vertices[f.a], vertices[f.b], vertices[f.c], f.colour);
		triangle.material = f.material;
		triangle.normal = faceNormals[i];
		triangle.vertexNormals = {smoothNormal(f.a, i), smoothNormal(f.b, i), smoothNormal(f.c, i)};
		if (f.ta >= 0 && f.tb >= 0 && f.tc >= 0) {
			triangle.texturePoints = {TexturePoint(texCoords[f.ta].x, texCoords[f.ta].y),
			                          TexturePoint(texCoords[f.tb].x, texCoords[f.tb].y),
			                          TexturePoint(texCoords[f.tc].x, texCoords[f.tc].y)};
			triangle.texture = f.texture;
		}
		triangles.push_back(triangle);
	}
	return triangles;
}
