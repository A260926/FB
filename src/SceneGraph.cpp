#include "SceneGraph.h"

#include "Transform.h"

namespace {

// Walk the hierarchy, accumulating the world transform and appending each
// node's mesh (baked into world space) to `out`.
void flattenInto(const SceneNode &node, const glm::mat4 &parentWorld, std::vector<ModelTriangle> &out) {
	const glm::mat4 world = parentWorld * node.localTransform;

	if (!node.mesh.empty()) {
		std::vector<ModelTriangle> transformed = transformModel(node.mesh, world);
		out.insert(out.end(), transformed.begin(), transformed.end());
	}

	for (const SceneNode &child : node.children) {
		flattenInto(child, world, out);
	}
}

} // namespace

SceneNode makeNode(const glm::mat4 &t, std::vector<ModelTriangle> mesh) {
	SceneNode node;
	node.localTransform = t;
	node.mesh = std::move(mesh);
	return node;
}

std::vector<ModelTriangle> flatten(const SceneNode &root) {
	std::vector<ModelTriangle> out;
	flattenInto(root, glm::mat4(1.0f), out);
	return out;
}
