#include <bvh/v2/spatial_sah_builder.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/ray.h>

#include <iostream>
#include <span>
#include <vector>

// Small, dependency-free example of the triangle-only spatial SAH builder. It builds a BVH for a
// handful of long diagonal triangles, traces a nearest and a shadow ray, and shows the difference
// between reference slots and original triangle indices.
int main() {
    using Scalar = double;
    using Vec3   = bvh::v2::Vec<Scalar, 3>;
    using Tri    = bvh::v2::Tri<Scalar, 3>;
    using Node   = bvh::v2::Node<Scalar, 3>;
    using Bvh    = bvh::v2::Bvh<Node>;
    using Ray    = bvh::v2::Ray<Scalar, 3>;
    using Builder = bvh::v2::SpatialSahBuilder<Node, 16>;

    std::vector<Tri> tris;
    for (size_t i = 0; i < 8; ++i) {
        Scalar z = 1 + Scalar(i) / Scalar(16);
        tris.emplace_back(Vec3(-10, -10, z), Vec3(10, 10, z), Vec3(10, 9.5, z));
    }

    Builder::Config config;
    // The tree may contain at most this many references *in addition* to the input faces.
    config.max_extra_references = tris.size() * 4;
    Builder::Result result = Builder::build(std::span<const Tri>(tris), config);
    const Bvh& bvh = result.bvh;

    std::cout << "input faces:      " << tris.size() << "\n";
    std::cout << "reference slots:  " << bvh.prim_ids.size() << "\n";
    std::cout << "spatial splits:   " << result.spatial_split_count << "\n";

    // Each slot in bvh.prim_ids is a *reference slot*. Its value is the index of the original
    // triangle it covers, and the same original index may appear in several slots.
    Ray ray(Vec3(0, -0.125, 0), Vec3(0, 0, 1), 0, 100);
    bvh::v2::GrowingStack<Node::Index> stack;
    long hit_face = -1;
    bvh.intersect<false, true>(ray, bvh.get_root().index, stack,
        [&](size_t begin, size_t end) {
            bool hit = false;
            for (size_t slot = begin; slot < end; ++slot) {
                size_t face = bvh.prim_ids[slot]; // original triangle id, not the slot id
                if (auto q = bvh::v2::PrecomputedTri<Scalar>(tris[face]).intersect(ray)) {
                    ray.tmax = std::get<0>(*q);
                    hit_face = static_cast<long>(face);
                    hit = true;
                }
            }
            return hit;
        });
    std::cout << "nearest original face: " << hit_face << "\n";

    // Any-hit (shadow) query against the same tree, using a finite ray window.
    Ray shadow(Vec3(0, -0.125, 0), Vec3(0, 0, 1), 0, 100);
    bvh::v2::GrowingStack<Node::Index> shadow_stack;
    bool occluded = false;
    bvh.intersect<true, true>(shadow, bvh.get_root().index, shadow_stack,
        [&](size_t begin, size_t end) {
            for (size_t slot = begin; slot < end; ++slot) {
                size_t face = bvh.prim_ids[slot];
                if (bvh::v2::PrecomputedTri<Scalar>(tris[face]).intersect(shadow)) {
                    occluded = true;
                    return true;
                }
            }
            return false;
        });
    std::cout << "shadow ray occluded: " << (occluded ? "yes" : "no") << "\n";
    return 0;
}
