#pragma once

#include <ModelTriangle.h>
#include <glm/glm.hpp>
#include <vector>

// A node in a hierarchical scene graph. Each node carries a transform relative
// to its parent, an optional mesh, and any number of child nodes.
struct SceneNode {
	glm::mat4 localTransform{1.0f};
	std::vector<ModelTriangle> mesh;
	std::vector<SceneNode> children;
};

// Convenience factory for building a node from a transform and a mesh.
SceneNode makeNode(const glm::mat4 &t, std::vector<ModelTriangle> mesh);

// Recursively compose parent*child world transforms and emit every node's mesh
// transformed into world space. The root's localTransform is applied first,
// then each child's transform is composed under its parent.
std::vector<ModelTriangle> flatten(const SceneNode &root);
