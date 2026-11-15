#ifndef PATCH_MESH_H
#define PATCH_MESH_H

#include "util/Utilities.h"
#include "object/Object.h"

#include <array>
#include <vector>

class Node;

// A triangle mesh built for the inverse-lighting (emission fitting) entry
// point. The mesh owns copies of its vertex positions and triangle indices;
// per-face Material pointers follow the codebase's usual external-lifetime
// convention (borrowed, never deleted by the mesh).
//
// Each triangle keeps its source face number (its position in the input
// index array). The internal BVH may reorder its proxy primitives during
// construction, but proxies carry their source face number, so intersection
// queries always report the original face id.
//
// This entry supports only static, finite, non-degenerate triangles with
// flat diffuse materials. Construction validates the geometry and throws
// std::invalid_argument on any violation.
class PatchMesh : public Object {
  public:
    // vertices:      vertex positions (copied, owned by the mesh)
    // triangles:     triples of vertex indices, CCW winding gives the
    //                outward (front-face) normal (copied, owned by the mesh)
    // face_materials: one Material* per triangle, same external-lifetime
    //                convention as Triangle (not owned)
    PatchMesh(const std::vector<Point3>& vertices,
              const std::vector<std::array<int, 3>>& triangles,
              const std::vector<Material*>& face_materials);

    ~PatchMesh() override = default;

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override;

    BoundingBox bounding_box() const override { return bbox_; }

    // Read-only geometry queries (source face numbering).
    int face_count() const { return static_cast<int>(triangles_.size()); }
    Point3 vertex(int face, int corner) const;
    Vector3 face_normal(int face) const;   // oriented unit normal (winding order)
    double face_area(int face) const;
    Material* face_material(int face) const;

  private:
    // Proxy primitive handed to the internal BVH. Remembers its source face
    // number, so BVH reordering cannot change reported face identities.
    class Face;

    std::vector<Point3> vertices_;               // owned vertex snapshot
    std::vector<std::array<int, 3>> triangles_;  // owned index snapshot
    std::vector<Material*> materials_;           // borrowed, per source face
    std::vector<Vector3> normals_;               // oriented unit normals
    std::vector<double> areas_;                  // triangle areas
    std::vector<shared_ptr<Object>> face_objects_; // proxies (reordered by BVH)
    shared_ptr<Node> bvh_;                       // internal acceleration
    BoundingBox bbox_;
};

#endif
