#include "object/PatchMesh.h"

#include "bvh/Node.h"

#include <stdexcept>

// Proxy primitive for one source face of a PatchMesh. Lives inside the
// mesh's internal BVH; reports the owning mesh and its source face number
// through IntersectionRecord, independent of BVH reordering.
class PatchMesh::Face : public Object {
  public:
    Face(const PatchMesh* mesh, int face) : mesh_(mesh), face_(face) {}

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        const Point3& v0 = mesh_->vertices_[mesh_->triangles_[face_][0]];
        const Point3& v1 = mesh_->vertices_[mesh_->triangles_[face_][1]];
        const Point3& v2 = mesh_->vertices_[mesh_->triangles_[face_][2]];

        // Möller–Trumbore, matching Triangle's double-sided test.
        const Vector3 edge1 = v1 - v0;
        const Vector3 edge2 = v2 - v0;
        const Vector3 h = cross(r.direction(), edge2);
        const double a = dot(edge1, h);

        constexpr double epsilon = 1e-8;
        if (std::fabs(a) < epsilon)
            return false;

        const double f = 1.0 / a;
        const Vector3 s = r.origin() - v0;
        const double u = f * dot(s, h);
        if (u < 0.0 || u > 1.0)
            return false;

        const Vector3 q = cross(s, edge1);
        const double v = f * dot(r.direction(), q);
        if (v < 0.0 || u + v > 1.0)
            return false;

        const double t = f * dot(edge2, q);
        if (!ray_t.surrounds(t))
            return false;

        rec.t = t;
        rec.p = r.at(t);
        rec.set_face_normal(r, mesh_->normals_[face_]);
        rec.mat_ptr = mesh_->materials_[face_];

        // Patch identity: the source mesh and the source face number.
        rec.patch_mesh = mesh_;
        rec.patch_face = face_;

        return true;
    }

    BoundingBox bounding_box() const override {
        const Point3& v0 = mesh_->vertices_[mesh_->triangles_[face_][0]];
        const Point3& v1 = mesh_->vertices_[mesh_->triangles_[face_][1]];
        const Point3& v2 = mesh_->vertices_[mesh_->triangles_[face_][2]];
        Point3 min(
            std::min({v0.x(), v1.x(), v2.x()}),
            std::min({v0.y(), v1.y(), v2.y()}),
            std::min({v0.z(), v1.z(), v2.z()}));
        Point3 max(
            std::max({v0.x(), v1.x(), v2.x()}),
            std::max({v0.y(), v1.y(), v2.y()}),
            std::max({v0.z(), v1.z(), v2.z()}));
        return BoundingBox(min, max);
    }

  private:
    const PatchMesh* mesh_;
    int face_;
};

PatchMesh::PatchMesh(const std::vector<Point3>& vertices,
                     const std::vector<std::array<int, 3>>& triangles,
                     const std::vector<Material*>& face_materials)
    : vertices_(vertices), triangles_(triangles), materials_(face_materials) {
    if (triangles_.empty())
        throw std::invalid_argument("PatchMesh: no triangles");
    if (vertices_.empty())
        throw std::invalid_argument("PatchMesh: no vertices");
    if (materials_.size() != triangles_.size())
        throw std::invalid_argument("PatchMesh: material count != triangle count");

    for (const Point3& p : vertices_) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()) || !std::isfinite(p.z()))
            throw std::invalid_argument("PatchMesh: non-finite vertex");
    }

    const int nv = static_cast<int>(vertices_.size());
    normals_.reserve(triangles_.size());
    areas_.reserve(triangles_.size());
    face_objects_.reserve(triangles_.size());

    for (size_t f = 0; f < triangles_.size(); ++f) {
        const auto& tri = triangles_[f];
        for (int k = 0; k < 3; ++k) {
            if (tri[k] < 0 || tri[k] >= nv)
                throw std::invalid_argument("PatchMesh: vertex index out of range");
        }
        if (materials_[f] == nullptr)
            throw std::invalid_argument("PatchMesh: null face material");

        const Point3& v0 = vertices_[tri[0]];
        const Point3& v1 = vertices_[tri[1]];
        const Point3& v2 = vertices_[tri[2]];
        const Vector3 n = cross(v1 - v0, v2 - v0);
        const double len2 = n.length_squared();
        if (len2 == 0.0)
            throw std::invalid_argument("PatchMesh: degenerate triangle");

        normals_.push_back(n / std::sqrt(len2));
        areas_.push_back(0.5 * std::sqrt(len2));
        face_objects_.push_back(std::make_shared<Face>(this, static_cast<int>(f)));
    }

    // Internal acceleration structure. Node sorts face_objects_ in place;
    // each Face proxy keeps its source face number, so reported identities
    // are unaffected by the reordering.
    bvh_ = std::make_shared<Node>(face_objects_, 0, face_objects_.size());

    bbox_ = face_objects_[0]->bounding_box();
    for (size_t f = 1; f < face_objects_.size(); ++f)
        bbox_ = BoundingBox(bbox_, face_objects_[f]->bounding_box());
}

bool PatchMesh::intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
    return bvh_->intersect(r, ray_t, rec);
}

Point3 PatchMesh::vertex(int face, int corner) const {
    if (face < 0 || face >= face_count() || corner < 0 || corner > 2)
        throw std::out_of_range("PatchMesh::vertex");
    return vertices_[triangles_[face][corner]];
}

Vector3 PatchMesh::face_normal(int face) const {
    if (face < 0 || face >= face_count())
        throw std::out_of_range("PatchMesh::face_normal");
    return normals_[face];
}

double PatchMesh::face_area(int face) const {
    if (face < 0 || face >= face_count())
        throw std::out_of_range("PatchMesh::face_area");
    return areas_[face];
}

Material* PatchMesh::face_material(int face) const {
    if (face < 0 || face >= face_count())
        throw std::out_of_range("PatchMesh::face_material");
    return materials_[face];
}
