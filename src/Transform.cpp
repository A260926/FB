#include "Transform.h"

#include "Noise.h"
#include <array>

std::vector<ModelTriangle> transformModel(const std::vector<ModelTriangle> &model, const glm::mat4 &m) {
	glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(m)));
	std::vector<ModelTriangle> out;
	out.reserve(model.size());
	for (ModelTriangle t : model) {
		for (int i = 0; i < 3; i++) {
			glm::vec4 v = m * glm::vec4(t.vertices[i], 1.0f);
			t.vertices[i] = glm::vec3(v);
			t.vertexNormals[i] = glm::normalize(normalMatrix * t.vertexNormals[i]);
		}
		t.normal = glm::normalize(normalMatrix * t.normal);
		out.push_back(t);
	}
	return out;
}

void appendInstance(std::vector<ModelTriangle> &scene, const std::vector<ModelTriangle> &mesh, const glm::mat4 &m) {
	std::vector<ModelTriangle> transformed = transformModel(mesh, m);
	scene.insert(scene.end(), transformed.begin(), transformed.end());
}

const std::vector<ModelTriangle> &selectLOD(float distance, float switchDistance,
                                            const std::vector<ModelTriangle> &highMesh,
                                            const std::vector<ModelTriangle> &lowMesh) {
	return (distance <= switchDistance) ? highMesh : lowMesh;
}

// Recursively 4-split a triangle to `level` 0, appending leaves.
static void subdivide(const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, const Colour &col, int level,
                      std::vector<ModelTriangle> &out) {
	if (level <= 0) {
		ModelTriangle t;
		t.vertices = {a, b, c};
		t.colour = col;
		out.push_back(t);
		return;
	}
	glm::vec3 ab = (a + b) * 0.5f, bc = (b + c) * 0.5f, ca = (c + a) * 0.5f;
	subdivide(a, ab, ca, col, level - 1, out);
	subdivide(ab, b, bc, col, level - 1, out);
	subdivide(ca, bc, c, col, level - 1, out);
	subdivide(ab, bc, ca, col, level - 1, out);
}

std::vector<ModelTriangle> displaceMesh(const std::vector<ModelTriangle> &mesh, int levels, float amplitude) {
	std::vector<ModelTriangle> patches;
	for (const ModelTriangle &t : mesh)
		subdivide(t.vertices[0], t.vertices[1], t.vertices[2], t.colour, levels, patches);
	// Push every vertex along the face normal by a fractal-noise height.
	for (ModelTriangle &t : patches) {
		glm::vec3 fn = glm::normalize(glm::cross(t.vertices[1] - t.vertices[0], t.vertices[2] - t.vertices[0]));
		for (int i = 0; i < 3; i++) {
			float h = fractalNoise(t.vertices[i] * 2.0f, 4);
			t.vertices[i] += fn * (h * amplitude);
		}
		t.normal = glm::normalize(glm::cross(t.vertices[1] - t.vertices[0], t.vertices[2] - t.vertices[0]));
		t.vertexNormals = {t.normal, t.normal, t.normal};
	}
	return patches;
}

std::vector<ModelTriangle> facetMirror(const std::vector<ModelTriangle> &mesh) {
	std::vector<ModelTriangle> out = mesh;
	for (ModelTriangle &t : out) {
		t.material = Material::Mirror;
		glm::vec3 n = glm::normalize(glm::cross(t.vertices[1] - t.vertices[0], t.vertices[2] - t.vertices[0]));
		t.normal = n;
		t.vertexNormals = {n, n, n}; // flat facets, not smooth
	}
	return out;
}
