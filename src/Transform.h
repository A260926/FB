#pragma once

#include <ModelTriangle.h>
#include <glm/glm.hpp>
#include <vector>

// A copy of `model` with every vertex and normal transformed by the 4x4 matrix
// `m` (build it with glm::translate / glm::scale / glm::rotate).
std::vector<ModelTriangle> transformModel(const std::vector<ModelTriangle> &model, const glm::mat4 &m);

// Append a transformed instance of `mesh` into `scene`, baking the transform
// into triangles. Call repeatedly to place many instances of one loaded mesh.
void appendInstance(std::vector<ModelTriangle> &scene, const std::vector<ModelTriangle> &mesh, const glm::mat4 &m);

// Level of detail: return highMesh when `distance` is within `switchDistance`,
// otherwise the cheaper lowMesh.
const std::vector<ModelTriangle> &selectLOD(float distance, float switchDistance,
                                            const std::vector<ModelTriangle> &highMesh,
                                            const std::vector<ModelTriangle> &lowMesh);

// True displacement mapping: subdivide each triangle `levels` times, then push
// every vertex along its interpolated normal by fractal-noise height * amplitude
// and recompute normals. Unlike bump/parallax this changes the silhouette.
std::vector<ModelTriangle> displaceMesh(const std::vector<ModelTriangle> &mesh, int levels, float amplitude);

// Turn a mesh into a faceted mirror (a disco ball): every triangle becomes a
// Mirror with flat per-facet normals, so each facet reflects like a flat tile
// and throws separate reflection spots around the room.
std::vector<ModelTriangle> facetMirror(const std::vector<ModelTriangle> &mesh);
