#include <bvh/BVHAcceleration.hpp>

void SoftRasterizer::BVHAcceleration::rebuild(
    std::vector<std::shared_ptr<Object>> primitives) {
  if (primitives.size() > std::numeric_limits<std::uint32_t>::max() / 2) {
    throw std::length_error("too many BVH primitives");
  }
  m_objects = std::move(primitives);
  m_nodes.reset();
  m_nodeCount = 0;
  m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(),
                                 [](const auto &object) {
                                   return !object ||
                                          object->getBounds().empty();
                                 }),
                  m_objects.end());
  if (!m_objects.empty()) {
    // N primitives give at most N leaves, hence at most 2*N-1 nodes.
    m_nodes = std::make_unique<Node[]>(m_objects.size() * 2 - 1);
    recursive(0, static_cast<std::uint32_t>(m_objects.size()));
  }
}

SoftRasterizer::BVHAcceleration::Node *
SoftRasterizer::BVHAcceleration::recursive(std::uint32_t begin,
                                           std::uint32_t end) {
  // Take the next node from the fixed pool; child pointers remain stable.
  Node *node = &m_nodes[m_nodeCount++];
  /*Collect primitive bounds and choose the longest centroid axis.*/
  Bounds3 bounds, centroids;
  for (auto i = begin; i < end; ++i) {
    auto objectBounds = m_objects[i]->getBounds();
    bounds.expand(objectBounds);
    centroids.expand(objectBounds.centroid());
  }
  node->box = bounds;
  if (end - begin <= 4) {
    node->begin = begin;
    node->count = end - begin;
    return node;
  }
  auto diagonal = centroids.diagonal();
  int axis = diagonal.x > diagonal.y ? (diagonal.x > diagonal.z ? 0 : 2)
                                     : (diagonal.y > diagonal.z ? 1 : 2);
  auto mid = begin + (end - begin) / 2;
  /*Median splits keep the tree balanced and recursion depth bounded.*/
  std::nth_element(m_objects.begin() + begin, m_objects.begin() + mid,
                   m_objects.begin() + end,
                   [axis](const auto &leftEntry, const auto &rightEntry) {
                     return leftEntry->getBounds().centroid()[axis] <
                            rightEntry->getBounds().centroid()[axis];
                   });
  node->left = recursive(begin, mid);
  node->right = recursive(mid, end);
  return node;
}

SoftRasterizer::Intersection
SoftRasterizer::BVHAcceleration::getIntersection(const Ray &ray) const {
  Intersection nearest;
  if (empty()) {
    return nearest;
  }
  // One local ray shares the nearest-hit limit across all recursive calls.
  Ray queryRay = ray;
  intersection(m_nodes.get(), queryRay, nearest);
  return nearest;
}

void SoftRasterizer::BVHAcceleration::intersection(
    const Node *node, Ray &ray, Intersection &nearest) const {
  if (!node || !node->box.intersect(ray, ray.tMax)) {
    return;
  }
  if (node->count) {
    for (auto i = node->begin; i < node->begin + node->count; ++i) {
      auto hit = m_objects[i]->getIntersect(ray);
      if (hit.intersected && hit.intersect_time > ray.tMin &&
          hit.intersect_time < ray.tMax) {
        nearest = hit;
        ray.tMax = hit.intersect_time;
      }
    }
    return;
  }
  float leftEntry, rightEntry;
  bool leftHit = node->left->box.intersect(ray, ray.tMax, &leftEntry),
       rightHit = node->right->box.intersect(ray, ray.tMax, &rightEntry);
  if (leftHit && rightHit) {
    // Visit the nearer child first; the other call sees the updated tMax.
    const Node *nearChild = node->left, *farChild = node->right;
    if (rightEntry <= leftEntry) {
      std::swap(nearChild, farChild);
    }
    intersection(nearChild, ray, nearest);
    intersection(farChild, ray, nearest);
  } else if (leftHit) {
    intersection(node->left, ray, nearest);
  } else if (rightHit) {
    intersection(node->right, ray, nearest);
  }
}

bool SoftRasterizer::BVHAcceleration::occluded(const Ray &ray) const {
  return occludedNode(m_nodes.get(), ray);
}

bool SoftRasterizer::BVHAcceleration::occludedNode(const Node *node,
                                                   const Ray &ray) const {
  if (!node || !node->box.intersect(ray, ray.tMax)) {
    return false;
  }
  if (node->count) {
    for (auto i = node->begin; i < node->begin + node->count; ++i) {
      if (m_objects[i]->getIntersect(ray).intersected) {
        return true;
      }
    }
    return false;
  }
  // Match the previous right-first order; || stops at the first occluder.
  return occludedNode(node->right, ray) || occludedNode(node->left, ray);
}
