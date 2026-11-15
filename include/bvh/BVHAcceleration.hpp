#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <object/Object.hpp>

namespace SoftRasterizer {
class BVHAcceleration {
public:
  void rebuild(std::vector<std::shared_ptr<Object>> primitives);
  Intersection getIntersection(const Ray &ray) const;
  bool occluded(const Ray &ray) const;

  bool empty() const {
    return !m_nodes;
  }

private:
  struct Node {
    Bounds3 box;
    Node *left = nullptr, *right = nullptr;
    std::uint32_t begin = 0, count = 0;
  };

  /*Build in place over [begin, end); no per-child object arrays.*/
  Node *recursive(std::uint32_t begin, std::uint32_t end);
  void intersection(const Node *node, Ray &ray, Intersection &nearest) const;
  bool occludedNode(const Node *node, const Ray &ray) const;

  /*One owning pool; children only point into it. The first node is the root.*/
  std::unique_ptr<Node[]> m_nodes;
  std::size_t m_nodeCount = 0;
  std::vector<std::shared_ptr<Object>> m_objects;
};
} // namespace SoftRasterizer
