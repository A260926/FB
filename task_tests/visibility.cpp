// Visibility layer regression tests.
//
// Registered with the task_support driver in three groups (query, refresh,
// transport). This file defines no main(): task_support/driver.c provides it.
#include "task_support/check.h"

#include "util/Utilities.h"
#include "object/ObjectList.h"
#include "object/Sphere.h"
#include "object/Triangle.h"
#include "object/Cylinder.h"
#include "bvh/Node.h"
#include "Camera.h"
#include "light/PointLight.h"
#include "light/RectangularLight.h"
#include "light/LightList.h"
#include "Material.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t LAYER_A = 1u << 0;
constexpr std::uint32_t LAYER_B = 1u << 1;
constexpr std::uint32_t LAYER_C = 1u << 2;
constexpr std::uint32_t ALL = VISIBILITY_MASK_ALL;

// ---------------------------------------------------------------------------
// Independent filtered brute-force reference: scans the raw object list and
// applies the visibility rule itself, so it never reuses the production BVH
// or ObjectList filtering logic.
// ---------------------------------------------------------------------------
struct RefHit {
    bool hit = false;
    double t = 0.0;
    const Material* mat = nullptr;
    double u = 0.0, v = 0.0;
};

RefHit reference_intersect(const ObjectList& world, const Ray& ray, double tmin, double tmax) {
    RefHit best;
    best.t = tmax;
    Interval window(tmin, tmax);
    for (const auto& obj : world.objects) {
        if (!visibility_matches(obj->visibility_mask(), ray.visibility_mask())) {
            continue;
        }
        IntersectionRecord rec;
        Interval probe(window.min, best.t);
        if (obj->intersect(ray, probe, rec)) {
            best.hit = true;
            best.t = rec.t;
            best.mat = rec.mat_ptr;
            best.u = rec.u;
            best.v = rec.v;
        }
    }
    return best;
}

bool same_hit(const IntersectionRecord& rec, bool hit, const RefHit& ref) {
    if (hit != ref.hit) return false;
    if (!hit) return true;
    if (rec.t != ref.t) return false;
    if (rec.mat_ptr != ref.mat) return false;
    if (rec.u != ref.u || rec.v != ref.v) return false;
    return true;
}

// Compare both BVH paths against the independent reference for one ray.
bool check_against_reference(ObjectList& world, const Ray& ray) {
    const double tmin = 0.001, tmax = 100.0;
    RefHit ref = reference_intersect(world, ray, tmin, tmax);

    IntersectionRecord bvh_rec;
    world.use_bvh = UseBVH::ENABLE;
    bool bvh_hit = world.intersect(ray, Interval(tmin, tmax), bvh_rec);

    IntersectionRecord brute_rec;
    world.use_bvh = UseBVH::DISABLE;
    bool brute_hit = world.intersect(ray, Interval(tmin, tmax), brute_rec);

    return same_hit(bvh_rec, bvh_hit, ref) && same_hit(brute_rec, brute_hit, ref);
}

// ---------------------------------------------------------------------------
// Primitives whose intersection records the mask of the ray that reached them.
// ---------------------------------------------------------------------------
class MaskProbe : public Object {
  public:
    explicit MaskProbe(const Point3& center, double radius)
        : center(center), radius(radius) {}

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        if (!visibility_matches(visibility_mask(), r.visibility_mask())) {
            return false;
        }
        last_ray_mask = r.visibility_mask();
        ++hit_count;
        // Camera rays start at the origin (tests use a zero aperture); every
        // successor ray starts at a displaced surface point.
        if (r.origin().length_squared() > 0.25) ++successor_count;

        const Vector3 oc = center - r.origin();
        const double a = r.direction().length_squared();
        const double h = dot(r.direction(), oc);
        const double c = oc.length_squared() - radius * radius;
        const double disc = h * h - a * c;
        if (disc < 0.0) return false;
        const double root = (h - std::sqrt(disc)) / a;
        if (!ray_t.surrounds(root)) return false;

        rec.t = root;
        rec.p = r.at(root);
        rec.set_face_normal(r, (rec.p - center) / radius);
        rec.mat_ptr = &material;
        return true;
    }

    BoundingBox bounding_box() const override {
        return BoundingBox(center - Vector3(radius, radius, radius),
                           center + Vector3(radius, radius, radius));
    }

    mutable std::uint32_t last_ray_mask = 0;
    mutable int hit_count = 0;
    mutable int successor_count = 0;
    mutable Material material;

  private:
    Point3 center;
    double radius;
};

// Large recorder triangle: reports the mask of any ray reaching its surface.
class MaskTriangle : public Object {
  public:
    MaskTriangle(const Point3& v0, const Point3& v1, const Point3& v2)
        : v0(v0), v1(v1), v2(v2) {}

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        if (!visibility_matches(visibility_mask(), r.visibility_mask())) {
            return false;
        }
        last_ray_mask = r.visibility_mask();
        ++hit_count;
        if (r.origin().length_squared() > 0.25) ++successor_count;

        const Vector3 edge1 = v1 - v0;
        const Vector3 edge2 = v2 - v0;
        const Vector3 h = cross(r.direction(), edge2);
        const double a = dot(edge1, h);
        constexpr double epsilon = 1e-8;
        if (std::fabs(a) < epsilon) return false;
        const double f = 1.0 / a;
        const Vector3 s = r.origin() - v0;
        const double u = f * dot(s, h);
        if (u < 0.0 || u > 1.0) return false;
        const Vector3 q = cross(s, edge1);
        const double v = f * dot(r.direction(), q);
        if (v < 0.0 || u + v > 1.0) return false;
        const double t = f * dot(edge2, q);
        if (!ray_t.surrounds(t)) return false;

        rec.t = t;
        rec.p = r.at(t);
        rec.set_face_normal(r, unit_vector(cross(edge1, edge2)));
        rec.mat_ptr = &material;
        return true;
    }

    BoundingBox bounding_box() const override {
        Point3 lo(std::min({v0.x(), v1.x(), v2.x()}),
                  std::min({v0.y(), v1.y(), v2.y()}),
                  std::min({v0.z(), v1.z(), v2.z()}));
        Point3 hi(std::max({v0.x(), v1.x(), v2.x()}),
                  std::max({v0.y(), v1.y(), v2.y()}),
                  std::max({v0.z(), v1.z(), v2.z()}));
        return BoundingBox(lo, hi);
    }

    mutable std::uint32_t last_ray_mask = 0;
    mutable int hit_count = 0;
    mutable int successor_count = 0;
    mutable Material material;

  private:
    Point3 v0, v1, v2;
};

// Scene of well-separated spheres along -z: sphere i is centered at z = -(4i+3).
std::vector<std::shared_ptr<Sphere>> make_separated_spheres(int count,
                                                            std::vector<Material>& mats,
                                                            bool excluded = false) {
    mats.resize(count);
    std::vector<std::shared_ptr<Sphere>> spheres;
    spheres.reserve(count);
    for (int i = 0; i < count; ++i) {
        spheres.push_back(std::make_shared<Sphere>(Point3(0, 0, -(4.0 * i + 3.0)), 1.0,
                                                   &mats[i], excluded));
    }
    return spheres;
}

// ===========================================================================
// query group
// ===========================================================================

int query_defaults_and_zero_layer() {
    Material mat;
    Sphere sphere(Point3(0, 0, -3), 1.0, &mat);
    TASK_CHECK(sphere.visibility_mask() == ALL);

    Ray default_ray(Point3(0, 0, 0), Vector3(0, 0, -1));
    TASK_CHECK(default_ray.visibility_mask() == ALL);

    Ray masked_ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_A);
    TASK_CHECK(masked_ray.visibility_mask() == LAYER_A);

    // Zero query layer sees nothing, even with an all-layers object.
    ObjectList world;
    world.add(std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat));
    world.build();
    Ray zero_ray(Point3(0, 0, 0), Vector3(0, 0, -1), 0u);
    IntersectionRecord rec;
    TASK_CHECK(!world.intersect(zero_ray, Interval(0.001, 100.0), rec));

    // Zero object mask is invisible to every query layer, including all-bits.
    sphere.set_visibility_mask(0u);
    TASK_CHECK(sphere.visibility_mask() == 0u);
    ObjectList hidden_world;
    hidden_world.add(std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat));
    hidden_world.objects.back()->set_visibility_mask(0u);
    hidden_world.build();
    TASK_CHECK(!hidden_world.intersect(default_ray, Interval(0.001, 100.0), rec));
    return 0;
}

int query_three_primitives_consistency() {
    Material sphere_mat, tri_mat, cyl_mat;
    auto sphere = std::make_shared<Sphere>(Point3(-3, 0, -4), 1.0, &sphere_mat);
    auto triangle = std::make_shared<Triangle>(Point3(-1, -1, -4), Point3(-1, 1, -4),
                                               Point3(1, 0, -4), &tri_mat);
    auto cylinder = std::make_shared<Cylinder>(Point3(3, 0, -4), Vector3(0, 1, 0),
                                               0.8, 1.2, &cyl_mat);
    ObjectList world;
    world.add(sphere);
    world.add(triangle);
    world.add(cylinder);
    world.build();

    const std::uint32_t ray_masks[] = {ALL, LAYER_A, LAYER_B, LAYER_A | LAYER_B, 0u};
    const std::uint32_t obj_masks[] = {ALL, LAYER_A, LAYER_B, LAYER_C, 0u};
    const Point3 origins[] = {Point3(-3, 0, 0), Point3(0, 0, 0), Point3(3, 0, 0)};
    for (std::uint32_t om : obj_masks) {
        sphere->set_visibility_mask(om);
        triangle->set_visibility_mask(om);
        cylinder->set_visibility_mask(om);
        world.refresh_visibility({sphere.get(), triangle.get(), cylinder.get()});
        for (std::uint32_t rm : ray_masks) {
            for (const auto& origin : origins) {
                Ray ray(origin, Vector3(0, 0, -1), rm);
                TASK_CHECK(check_against_reference(world, ray));
            }
        }
    }
    return 0;
}

int query_hidden_near_face_keeps_window() {
    Material near_mat, far_mat;
    auto near = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &near_mat);
    auto far = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &far_mat);

    ObjectList world;
    world.add(far);
    world.add(near);
    world.build();

    Ray ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_B);

    // Visible baseline: the near sphere wins.
    IntersectionRecord rec;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100.0), rec));
    TASK_CHECK(rec.t == 2.0);
    TASK_CHECK(rec.mat_ptr == &near_mat);

    // Hide the near sphere on a disjoint layer: the far sphere must be reached
    // and report its own material, UVs and t, exactly as the reference does.
    near->set_visibility_mask(LAYER_A);
    world.refresh_visibility({near.get()});

    RefHit ref = reference_intersect(world, ray, 0.001, 100.0);
    TASK_CHECK(ref.hit && ref.t == 6.0 && ref.mat == &far_mat);

    IntersectionRecord bvh_rec;
    world.use_bvh = UseBVH::ENABLE;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100.0), bvh_rec));
    TASK_CHECK(same_hit(bvh_rec, true, ref));

    IntersectionRecord brute_rec;
    world.use_bvh = UseBVH::DISABLE;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100.0), brute_rec));
    TASK_CHECK(same_hit(brute_rec, true, ref));
    return 0;
}

int query_duplicate_shared_object() {
    Material mat;
    auto shared = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat);
    auto other = std::make_shared<Sphere>(Point3(3, 0, -5), 1.0, &mat);

    ObjectList world;
    world.add(shared);
    world.add(other);
    world.add(shared);  // Same shared_ptr appears twice.
    world.build();

    const std::uint32_t masks[] = {ALL, LAYER_A, LAYER_B, 0u};
    for (std::uint32_t om : masks) {
        shared->set_visibility_mask(om);
        other->set_visibility_mask(LAYER_B);
        world.refresh_visibility({shared.get(), other.get()});
        for (std::uint32_t rm : masks) {
            Ray ray(Point3(0, 0, 0), Vector3(0, 0, -1), rm);
            TASK_CHECK(check_against_reference(world, ray));
            Ray side_ray(Point3(3, 0, 0), Vector3(0, 0, -1), rm);
            TASK_CHECK(check_against_reference(world, side_ray));
        }
    }
    return 0;
}

int query_excluded_path() {
    Material in_mat, ex_mat;
    auto inside = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &in_mat);
    auto excluded = std::make_shared<Sphere>(Point3(4, 0, -3), 1.0, &ex_mat, true);

    ObjectList world;
    world.add(inside);
    world.add(excluded);
    world.build();
    TASK_CHECK(world.excluded_objects.size() == 1);

    // Hide the excluded object: it must disappear even though it is not in the
    // BVH, and the hidden object must not shorten the distance window.
    excluded->set_visibility_mask(LAYER_A);
    world.refresh_visibility({excluded.get()});  // No tree work, but must stay valid.

    Ray ray(Point3(4, 0, 0), Vector3(0, 0, -1), LAYER_B);
    RefHit ref = reference_intersect(world, ray, 0.001, 100.0);
    TASK_CHECK(!ref.hit);

    IntersectionRecord rec;
    world.use_bvh = UseBVH::ENABLE;
    TASK_CHECK(!world.intersect(ray, Interval(0.001, 100.0), rec));

    // Restore it and confirm the excluded path still finds it.
    excluded->set_visibility_mask(ALL);
    world.refresh_visibility({excluded.get()});
    ref = reference_intersect(world, ray, 0.001, 100.0);
    TASK_CHECK(ref.hit && ref.mat == &ex_mat && ref.t == 2.0);
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100.0), rec));
    TASK_CHECK(rec.mat_ptr == &ex_mat && rec.t == 2.0);

    // Hide the in-BVH object while the excluded one stays visible.
    inside->set_visibility_mask(LAYER_A);
    world.refresh_visibility({inside.get()});
    Ray center_ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_B);
    TASK_CHECK(check_against_reference(world, center_ray));
    return 0;
}

int query_cached_node_rejection() {
    // With every primitive on LAYER_A, a LAYER_B query must be rejected at the
    // root cache before any bounding box test.
    Material mat;
    auto a = std::make_shared<Sphere>(Point3(-2, 0, -3), 1.0, &mat);
    auto b = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat);
    a->set_visibility_mask(LAYER_A);
    b->set_visibility_mask(LAYER_A);

    ObjectList world;
    world.add(a);
    world.add(b);
    world.build();
    TASK_CHECK(world.root->cached_visibility_mask() == LAYER_A);

    Sphere::hit_call_count = 0;
    Ray ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_B);
    IntersectionRecord rec;
    TASK_CHECK(!world.intersect(ray, Interval(0.001, 100.0), rec));
    TASK_CHECK(Sphere::hit_call_count == 0);

    Ray visible_ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_A);
    TASK_CHECK(world.intersect(visible_ray, Interval(0.001, 100.0), rec));
    return 0;
}

// ===========================================================================
// refresh group
// ===========================================================================

int refresh_empty_and_unknown() {
    Material mat;
    auto sphere = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat);
    ObjectList world;
    world.add(sphere);
    world.build();

    VisibilityRefreshStats empty = world.refresh_visibility({});
    TASK_CHECK(empty.object_reads == 0 && empty.node_reductions == 0);

    auto foreign = std::make_shared<Sphere>(Point3(9, 9, 9), 1.0, &mat);
    VisibilityRefreshStats unknown = world.refresh_visibility({foreign.get(), nullptr, foreign.get()});
    TASK_CHECK(unknown.object_reads == 0 && unknown.node_reductions == 0);
    TASK_CHECK(world.root->cached_visibility_mask() == ALL);

    // Scene with no BVH (all objects excluded) must accept refresh harmlessly.
    ObjectList excluded_world;
    auto ex = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat, true);
    excluded_world.add(ex);
    excluded_world.build();
    TASK_CHECK(!excluded_world.root);
    VisibilityRefreshStats no_tree = excluded_world.refresh_visibility({ex.get()});
    TASK_CHECK(no_tree.object_reads == 0 && no_tree.node_reductions == 0);
    return 0;
}

int refresh_single_leaf_work_and_preservation() {
    const int count = 4096;
    std::vector<Material> mats;
    auto spheres = make_separated_spheres(count, mats);

    ObjectList world;
    for (auto& s : spheres) world.add(s);
    world.build();

    const Node* const old_root = world.root.get();
    const BoundingBox old_box = world.root->bounding_box();
    const size_t node_count = world.tree_nodes.size();
    TASK_CHECK(node_count >= static_cast<size_t>(count));

    // Depth of the leaf holding sphere 0, counted through parent links.
    size_t leaf_index = world.object_occurrences.at(spheres[0].get()).front();
    size_t depth = 0;
    for (size_t cur = leaf_index; world.tree_parents[cur] != static_cast<size_t>(-1);
         cur = world.tree_parents[cur]) {
        ++depth;
    }

    spheres[0]->set_visibility_mask(LAYER_A);
    VisibilityRefreshStats stats = world.refresh_visibility({spheres[0].get()});

    // One primitive read (enumeration) + one at the leaf reduction. Only the
    // leaf and ancestors up to the first unchanged OR are reduced, so the work
    // is bounded by the leaf-to-root path and never scales with the 4096 objects.
    TASK_CHECK(stats.object_reads == 2);
    TASK_CHECK(stats.node_reductions >= 1);
    TASK_CHECK(stats.node_reductions <= depth + 1);
    TASK_CHECK(stats.node_reductions < node_count);

    // Topology, root pointer, node links and geometry boxes are preserved.
    TASK_CHECK(world.root.get() == old_root);
    TASK_CHECK(world.tree_nodes.size() == node_count);
    TASK_CHECK(world.root->bounding_box().x.min == old_box.x.min);
    TASK_CHECK(world.root->bounding_box().x.max == old_box.x.max);
    TASK_CHECK(world.root->bounding_box().y.min == old_box.y.min);
    TASK_CHECK(world.root->bounding_box().y.max == old_box.y.max);
    TASK_CHECK(world.root->bounding_box().z.min == old_box.z.min);
    TASK_CHECK(world.root->bounding_box().z.max == old_box.z.max);
    TASK_CHECK(world.root->left && world.root->right);

    // The root OR still covers the new layer (it already held every other bit).
    TASK_CHECK((world.root->cached_visibility_mask() & LAYER_A) != 0);

    // Unrelated primitives were never read: a second leaf refresh has the same
    // primitive read count and stays within the same path bound.
    spheres[0]->set_visibility_mask(ALL);
    VisibilityRefreshStats again = world.refresh_visibility({spheres[0].get()});
    TASK_CHECK(again.object_reads == 2);
    TASK_CHECK(again.node_reductions >= 1);
    TASK_CHECK(again.node_reductions <= depth + 1);
    return 0;
}

int refresh_same_parent_multiple_leaves() {
    const int count = 4096;
    std::vector<Material> mats;
    auto spheres = make_separated_spheres(count, mats);

    ObjectList world;
    for (auto& s : spheres) world.add(s);
    world.build();

    // Find a node whose two children are both leaves, by grouping leaf indices
    // on their parent.
    std::unordered_map<size_t, std::vector<size_t>> leaves_by_parent;
    for (size_t i = 0; i < world.tree_nodes.size(); ++i) {
        if (world.object_occurrences.count(world.tree_nodes[i]->left.get())) {
            leaves_by_parent[world.tree_parents[i]].push_back(i);
        }
    }
    size_t parent = static_cast<size_t>(-1);
    std::vector<size_t> sibling_leaves;
    for (auto& entry : leaves_by_parent) {
        if (entry.second.size() >= 2) {
            parent = entry.first;
            sibling_leaves = entry.second;
            break;
        }
    }
    TASK_CHECK(parent != static_cast<size_t>(-1));
    TASK_CHECK(sibling_leaves.size() >= 2);

    const Object* obj0 = world.tree_nodes[sibling_leaves[0]]->left.get();
    const Object* obj1 = world.tree_nodes[sibling_leaves[1]]->left.get();

    size_t parent_depth = 0;
    for (size_t cur = parent; world.tree_parents[cur] != static_cast<size_t>(-1);
         cur = world.tree_parents[cur]) {
        ++parent_depth;
    }

    // Change both siblings and refresh in one call with a duplicated entry.
    const_cast<Object*>(obj0)->set_visibility_mask(LAYER_B);
    const_cast<Object*>(obj1)->set_visibility_mask(LAYER_B);
    VisibilityRefreshStats stats =
        world.refresh_visibility({obj0, obj1, obj0});  // Duplicate handled once.

    // Two unique enumeration reads + two leaf reductions; the shared parent is
    // reduced only once despite two dirty leaves, and propagation is bounded by
    // the union of the two ancestor paths.
    TASK_CHECK(stats.object_reads == 4);
    TASK_CHECK(stats.node_reductions >= 3);  // Two leaves + their shared parent.
    TASK_CHECK(stats.node_reductions <= 2 + parent_depth + 1);
    TASK_CHECK((world.root->cached_visibility_mask() & LAYER_B) != 0);

    // A single-leaf refresh on the same tree stays on its own leaf-to-root
    // path: primitive reads halve and no node beyond the path union is touched.
    const_cast<Object*>(obj0)->set_visibility_mask(LAYER_C);
    VisibilityRefreshStats single = world.refresh_visibility({obj0});
    TASK_CHECK(single.object_reads == 2);
    TASK_CHECK(single.object_reads < stats.object_reads);
    TASK_CHECK(single.node_reductions >= 1);
    TASK_CHECK(single.node_reductions <= parent_depth + 2);
    TASK_CHECK(stats.node_reductions < world.tree_nodes.size());
    TASK_CHECK(single.node_reductions < world.tree_nodes.size());
    return 0;
}

int refresh_early_stop_when_or_unchanged() {
    // Two leaves under one parent; removing a bit that the sibling still holds
    // leaves the parent OR unchanged, so propagation stops at the parent.
    Material m0, m1;
    auto s0 = std::make_shared<Sphere>(Point3(-2, 0, -3), 1.0, &m0);
    auto s1 = std::make_shared<Sphere>(Point3(2, 0, -3), 1.0, &m1);
    ObjectList world;
    world.add(s0);
    world.add(s1);
    world.build();
    TASK_CHECK(world.tree_nodes.size() == 3);  // root + two leaves

    s0->set_visibility_mask(LAYER_A);
    s1->set_visibility_mask(LAYER_A);
    world.refresh_visibility({s0.get(), s1.get()});
    TASK_CHECK(world.root->cached_visibility_mask() == LAYER_A);

    // s0 goes to zero but s1 keeps LAYER_A: leaf reduction changes, parent OR
    // stays LAYER_A, so only leaf + parent reductions happen (root == parent).
    s0->set_visibility_mask(0u);
    VisibilityRefreshStats stats = world.refresh_visibility({s0.get()});
    TASK_CHECK(stats.object_reads == 2);
    TASK_CHECK(stats.node_reductions == 2);
    TASK_CHECK(world.root->cached_visibility_mask() == LAYER_A);
    return 0;
}

int refresh_shared_between_two_trees() {
    const int count = 64;
    std::vector<Material> mats;
    auto spheres = make_separated_spheres(count, mats);

    ObjectList tree_one;
    ObjectList tree_two;
    for (auto& s : spheres) {
        tree_one.add(s);
        tree_two.add(s);  // The same shared_ptrs back a second independent tree.
    }
    tree_one.build();
    tree_two.build();
    TASK_CHECK(tree_one.root.get() != tree_two.root.get());

    auto toggle_and_check = [&](std::uint32_t target_mask) -> int {
        spheres[10]->set_visibility_mask(target_mask);
        VisibilityRefreshStats one = tree_one.refresh_visibility({spheres[10].get()});
        VisibilityRefreshStats two = tree_two.refresh_visibility({spheres[10].get()});
        TASK_CHECK(one.object_reads > 0 && two.object_reads > 0);
        TASK_CHECK((tree_one.root->cached_visibility_mask() & target_mask) != 0 ||
                   target_mask == 0);
        TASK_CHECK((tree_two.root->cached_visibility_mask() & target_mask) != 0 ||
                   target_mask == 0);

        Ray ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_C);
        IntersectionRecord rec_one, rec_two;
        bool hit_one = tree_one.intersect(ray, Interval(0.001, 100.0), rec_one);
        bool hit_two = tree_two.intersect(ray, Interval(0.001, 100.0), rec_two);
        TASK_CHECK(hit_one == hit_two);
        if (hit_one) {
            TASK_CHECK(rec_one.t == rec_two.t && rec_one.mat_ptr == rec_two.mat_ptr);
        }
        return 0;
    };

    for (std::uint32_t target_mask : {LAYER_A, LAYER_B, ALL, 0u}) {
        if (toggle_and_check(target_mask)) return 1;
    }

    // Local-work check: one leaf refresh in each tree stays leaf-local.
    spheres[10]->set_visibility_mask(LAYER_C);
    VisibilityRefreshStats local_one = tree_one.refresh_visibility({spheres[10].get()});
    TASK_CHECK(local_one.object_reads == 2);
    TASK_CHECK(local_one.node_reductions < tree_one.tree_nodes.size());
    return 0;
}

int refresh_rebuild_and_clear_replace_index() {
    Material mat;
    auto a = std::make_shared<Sphere>(Point3(-2, 0, -3), 1.0, &mat);
    auto b = std::make_shared<Sphere>(Point3(2, 0, -3), 1.0, &mat);
    auto ex = std::make_shared<Sphere>(Point3(4, 0, -3), 1.0, &mat, true);

    ObjectList world;
    world.add(a);
    world.add(b);
    world.add(ex);
    world.build();
    TASK_CHECK(world.object_occurrences.size() == 2);
    TASK_CHECK(world.excluded_objects.size() == 1);

    // Rebuild after adding an object: the old index and excluded list are replaced.
    auto c = std::make_shared<Sphere>(Point3(0, 4, -3), 1.0, &mat);
    world.add(c);
    world.build();
    TASK_CHECK(world.object_occurrences.count(c.get()) == 1);
    TASK_CHECK(world.excluded_objects.size() == 1);
    TASK_CHECK(world.tree_parents.size() == world.tree_nodes.size());

    // clear() must leave no stale references; refresh on the empty list is a no-op.
    const Node* old_root = world.root.get();
    (void)old_root;
    world.clear();
    TASK_CHECK(world.objects.empty());
    TASK_CHECK(world.excluded_objects.empty());
    TASK_CHECK(!world.root);
    TASK_CHECK(world.tree_nodes.empty());
    TASK_CHECK(world.tree_parents.empty());
    TASK_CHECK(world.object_occurrences.empty());
    VisibilityRefreshStats after_clear = world.refresh_visibility({a.get()});
    TASK_CHECK(after_clear.object_reads == 0 && after_clear.node_reductions == 0);
    return 0;
}

int refresh_excluded_object_new_mask_observed() {
    // Objects outside the BVH carry no cached state, but queries must still see
    // mask changes immediately after refresh (which is a no-op for them).
    Material mat;
    auto ex = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat, true);
    ObjectList world;
    world.add(ex);
    world.build();

    Ray ray(Point3(0, 0, 0), Vector3(0, 0, -1), LAYER_B);
    IntersectionRecord rec;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100.0), rec));

    ex->set_visibility_mask(LAYER_A);
    VisibilityRefreshStats stats = world.refresh_visibility({ex.get()});
    TASK_CHECK(stats.object_reads == 0 && stats.node_reductions == 0);
    TASK_CHECK(!world.intersect(ray, Interval(0.001, 100.0), rec));
    return 0;
}

// ===========================================================================
// transport group
// ===========================================================================

int transport_camera_mask_and_inheritance() {
    // A large planar mirror at z = -3 faces the camera. Reflecting a ray
    // (vx, vy, -1) across its +z normal sends it back along (vx, vy, +1), so
    // the inner pixels' successor rays converge near the axis behind the
    // camera, where a layer-B recording probe sits. Primary rays never travel
    // toward +z, so the probe can only be reached through a reflected ray.
    Material mirror_mat;
    mirror_mat.is_reflective = true;
    mirror_mat.reflectivity = 1.0;
    mirror_mat.kd = 0.0;

    auto mirror = std::make_shared<Triangle>(Point3(-10, -10, -3), Point3(10, -10, -3),
                                             Point3(0, 10, -3), &mirror_mat);

    // Recorder behind the camera (z = +1): primary rays head toward -z, so it
    // can only be struck by the reflected successor rays.
    auto back_probe = std::make_shared<MaskTriangle>(Point3(-6, -6, 1), Point3(6, -6, 1),
                                                     Point3(0, 6, 1));
    back_probe->set_visibility_mask(LAYER_B);

    // Occluder on a separate layer, off the central reflection axis so the
    // center pixels still reach the back probe when every layer is visible.
    auto occluder = std::make_shared<MaskProbe>(Point3(0.8, 0, -1.5), 0.4);
    occluder->set_visibility_mask(LAYER_A);

    ObjectList world;
    world.add(mirror);
    world.add(back_probe);
    world.add(occluder);
    world.build();

    LightList lights;
    lights.add(std::make_shared<PointLight>(Point3(0, 5, 0), Radiance(1, 1, 1)));

    auto make_camera = [&](std::uint32_t mask) {
        Camera camera;
        camera.image_width = 4;
        camera.image_height = 4;
        camera.lookfrom = Point3(0, 0, 0);
        camera.lookat = Point3(0, 0, -1);
        camera.render_mode = RenderMode::PHONG;
        camera.samples_per_pixel = 1;
        camera.ambient_light = Radiance(0, 0, 0);
        camera.visibility_mask = mask;
        camera.filename = "";
        return camera;
    };

    // LAYER_B camera: the back probe records the query mask on the reflected
    // successor ray; the LAYER_A occluder is never touched.
    {
        Camera camera = make_camera(LAYER_B);
        back_probe->hit_count = 0;
        back_probe->successor_count = 0;
        occluder->hit_count = 0;
        camera.render_phong(world, lights);
        TASK_CHECK(back_probe->successor_count > 0 && back_probe->last_ray_mask == LAYER_B);
        TASK_CHECK(occluder->hit_count == 0);
    }

    // All-layers control: every object is reached and records the full mask.
    {
        Camera camera = make_camera(ALL);
        back_probe->hit_count = 0;
        back_probe->successor_count = 0;
        occluder->hit_count = 0;
        camera.render_phong(world, lights);
        TASK_CHECK(back_probe->successor_count > 0 && back_probe->last_ray_mask == ALL);
        TASK_CHECK(occluder->hit_count > 0 && occluder->last_ray_mask == ALL);
    }

    // Disjoint query layer: the layer-B probe and layer-A occluder vanish.
    {
        Camera camera = make_camera(LAYER_C);
        back_probe->hit_count = 0;
        back_probe->successor_count = 0;
        occluder->hit_count = 0;
        camera.render_phong(world, lights);
        TASK_CHECK(back_probe->successor_count == 0);
        TASK_CHECK(occluder->hit_count == 0);
    }
    return 0;
}

int transport_shadow_inheritance_point_light() {
    Material shaded_mat;
    auto shaded = std::make_shared<MaskProbe>(Point3(0, 0, -6), 1.0);
    auto occluder = std::make_shared<Sphere>(Point3(0, 0, -3), 0.6, &shaded_mat);
    occluder->set_visibility_mask(LAYER_A);

    ObjectList world;
    world.add(shaded);
    world.add(occluder);
    world.build();

    PointLight light(Point3(0, 0, 0), Radiance(100, 100, 100));

    const Point3 surface(0, 0, -5);
    // LAYER_A query: the occluder blocks the shadow first segment.
    double blocked = light.shadow_factor(surface, world, LAYER_A);
    TASK_CHECK(blocked < 0.99);

    // LAYER_B query: the occluder is invisible, so the point is fully lit.
    double clear = light.shadow_factor(surface, world, LAYER_B);
    TASK_CHECK(clear == 1.0);

    // Default argument still means all layers (old callers keep working).
    double legacy = light.shadow_factor(surface, world);
    TASK_CHECK(legacy == blocked);
    return 0;
}

int transport_shadow_inheritance_rect_light() {
    Material mat;
    auto shaded = std::make_shared<MaskProbe>(Point3(0, 0, -6), 1.0);
    auto occluder = std::make_shared<Sphere>(Point3(0, 0, -3), 0.6, &mat);
    occluder->set_visibility_mask(LAYER_A);

    ObjectList world;
    world.add(shaded);
    world.add(occluder);
    world.build();

    RectangularLight light(Point3(0, 0, -1), Vector3(1, 0, 0), Vector3(0, 1, 0),
                           0.5, 0.5, Radiance(100, 100, 100), /*num_samples=*/4);

    const Point3 surface(0, 0, -5);
    const Point3 sample(0, 0, -1);
    double blocked = light.shadow_factor_from_sample(surface, sample, world, LAYER_A);
    TASK_CHECK(blocked == 0.0);  // Rect light shadow_frac defaults to fully dark.
    double clear = light.shadow_factor_from_sample(surface, sample, world, LAYER_B);
    TASK_CHECK(clear == 1.0);
    return 0;
}

int transport_refraction_continuation() {
    // A refractive occluder on LAYER_A: the LAYER_A shadow ray passes through it
    // (attenuated), and its refraction continuation must carry the same mask.
    Material glass;
    glass.is_refractive = true;
    glass.refractiveindex = 1.5;
    auto pane = std::make_shared<MaskProbe>(Point3(0, 0, -3), 0.8);
    pane->set_visibility_mask(LAYER_A);
    pane->material = glass;

    Material back_mat;
    auto back = std::make_shared<MaskProbe>(Point3(0, 0, -7), 1.0);
    back->set_visibility_mask(LAYER_A);

    ObjectList world;
    world.add(pane);
    world.add(back);
    world.build();

    PointLight light(Point3(0, 0, 0), Radiance(100, 100, 100));
    double through = light.shadow_factor(Point3(0, 0, -5), world, LAYER_A);
    TASK_CHECK(pane->hit_count > 0);
    TASK_CHECK(pane->last_ray_mask == LAYER_A);
    TASK_CHECK(through > 0.0 && through < 1.0);  // Attenuated, not blocked.

    // LAYER_B skips the pane entirely: full illumination.
    double clear = light.shadow_factor(Point3(0, 0, -5), world, LAYER_B);
    TASK_CHECK(clear == 1.0);
    return 0;
}

int transport_path_bounce_inheritance() {
    // Part 1: deterministic reflection through the PATH renderer (the same
    // trace_path_phong reflection branch the production path tracer uses).
    {
        Material mirror_mat;
        mirror_mat.is_reflective = true;
        mirror_mat.reflectivity = 1.0;
        mirror_mat.kd = 0.0;

        auto mirror = std::make_shared<Triangle>(Point3(-10, -10, -3), Point3(10, -10, -3),
                                                 Point3(0, 10, -3), &mirror_mat);
        auto back_probe = std::make_shared<MaskTriangle>(Point3(-6, -6, 1), Point3(6, -6, 1),
                                                         Point3(0, 6, 1));
        back_probe->set_visibility_mask(LAYER_B);

        ObjectList world;
        world.add(mirror);
        world.add(back_probe);
        world.build();

        LightList lights;
        lights.add(std::make_shared<PointLight>(Point3(0, 5, 0), Radiance(1, 1, 1)));

        auto make_camera = [&](std::uint32_t mask) {
            Camera camera;
            camera.image_width = 4;
            camera.image_height = 4;
            camera.lookfrom = Point3(0, 0, 0);
            camera.lookat = Point3(0, 0, -1);
            camera.render_mode = RenderMode::PATH;
            camera.samples_per_pixel = 1;
            camera.nbounces = 4;
            camera.ambient_light = Radiance(0, 0, 0);
            camera.visibility_mask = mask;
            camera.filename = "";
            return camera;
        };

        Camera layer_camera = make_camera(LAYER_B);
        back_probe->hit_count = 0;
        back_probe->successor_count = 0;
        layer_camera.render_path(world, lights);
        TASK_CHECK(back_probe->successor_count > 0 && back_probe->last_ray_mask == LAYER_B);

        // Fixed-seed control: the all-layers camera follows the same random
        // stream; only the carried mask differs.
        generator.seed(1234);
        Camera control = make_camera(ALL);
        back_probe->hit_count = 0;
        back_probe->successor_count = 0;
        control.render_path(world, lights);
        TASK_CHECK(back_probe->successor_count > 0 && back_probe->last_ray_mask == ALL);
    }

    // Part 2: diffuse bounce. A large diffuse ground fills the lower half; the
    // layer-B recorder encloses the scene, so every upward hemisphere scatter
    // reaches it and reports the mask on the diffuse successor ray.
    {
        Material diffuse;
        diffuse.kd = 0.9;
        auto ground = std::make_shared<Sphere>(Point3(0, -10, -3), 10.0, &diffuse);
        auto enclosure = std::make_shared<MaskProbe>(Point3(0, 0, -3), 40.0);
        enclosure->set_visibility_mask(LAYER_B);

        ObjectList world;
        world.add(ground);
        world.add(enclosure);
        world.build();

        LightList lights;
        lights.add(std::make_shared<PointLight>(Point3(0, 5, -2), Radiance(50, 50, 50)));

        Camera camera;
        camera.image_width = 4;
        camera.image_height = 4;
        camera.lookfrom = Point3(0, 0, 0);
        camera.lookat = Point3(0, 0, -1);
        camera.render_mode = RenderMode::PATH;
        camera.samples_per_pixel = 1;
        camera.nbounces = 4;
        camera.ambient_light = Radiance(0, 0, 0);
        camera.visibility_mask = LAYER_B;
        camera.filename = "";

        generator.seed(1234);
        enclosure->hit_count = 0;
        camera.render_path(world, lights);
        TASK_CHECK(enclosure->hit_count > 0);
        TASK_CHECK(enclosure->last_ray_mask == LAYER_B);

        // Disjoint layer: the enclosure vanishes, no successor reaches it.
        camera.visibility_mask = LAYER_C;
        generator.seed(1234);
        enclosure->hit_count = 0;
        camera.render_path(world, lights);
        TASK_CHECK(enclosure->hit_count == 0);
    }
    return 0;
}

int transport_default_layer_pixel_invariance() {
    // With default all-layers masks, a render must be pixel-identical whether or
    // not the visibility machinery is exercised: filtering only adds bit tests,
    // no extra random draws. PHONG with one sample is deterministic.
    Material plain;
    plain.kd = 0.7;
    auto a = std::make_shared<Sphere>(Point3(-1, 0, -3), 1.0, &plain);
    auto b = std::make_shared<Sphere>(Point3(1, 0, -3), 1.0, &plain);

    ObjectList world;
    world.add(a);
    world.add(b);
    world.build();

    LightList lights;
    lights.add(std::make_shared<PointLight>(Point3(0, 4, -1), Radiance(20, 20, 20)));

    auto make_camera = [&](std::uint32_t mask) {
        Camera camera;
        camera.image_width = 6;
        camera.image_height = 6;
        camera.lookfrom = Point3(0, 0, 0);
        camera.lookat = Point3(0, 0, -1);
        camera.render_mode = RenderMode::PHONG;
        camera.samples_per_pixel = 1;
        camera.visibility_mask = mask;
        camera.filename = "";
        return camera;
    };

    Camera default_camera = make_camera(ALL);  // Explicit all-bits.
    std::vector<Radiance> explicit_pixels = default_camera.render_phong(world, lights);

    // A second camera left at its constructed default mask must match.
    Camera untouched_camera;
    untouched_camera.image_width = 6;
    untouched_camera.image_height = 6;
    untouched_camera.lookfrom = Point3(0, 0, 0);
    untouched_camera.lookat = Point3(0, 0, -1);
    untouched_camera.render_mode = RenderMode::PHONG;
    untouched_camera.samples_per_pixel = 1;
    untouched_camera.filename = "";
    TASK_CHECK(untouched_camera.visibility_mask == ALL);
    std::vector<Radiance> default_pixels = untouched_camera.render_phong(world, lights);

    TASK_CHECK(explicit_pixels.size() == default_pixels.size());
    for (size_t i = 0; i < default_pixels.size(); ++i) {
        TASK_CHECK(default_pixels[i].x() == explicit_pixels[i].x());
        TASK_CHECK(default_pixels[i].y() == explicit_pixels[i].y());
        TASK_CHECK(default_pixels[i].z() == explicit_pixels[i].z());
    }
    return 0;
}

}  // namespace

// ===========================================================================
// Registration (no main in this file; driver.c drives the cases).
// ===========================================================================
const struct task_case task_cases[] = {
    // query
    {"query", "defaults_and_zero_layer", query_defaults_and_zero_layer},
    {"query", "three_primitives_consistency", query_three_primitives_consistency},
    {"query", "hidden_near_face_keeps_window", query_hidden_near_face_keeps_window},
    {"query", "duplicate_shared_object", query_duplicate_shared_object},
    {"query", "excluded_path", query_excluded_path},
    {"query", "cached_node_rejection", query_cached_node_rejection},

    // refresh
    {"refresh", "empty_and_unknown", refresh_empty_and_unknown},
    {"refresh", "single_leaf_work_and_preservation", refresh_single_leaf_work_and_preservation},
    {"refresh", "same_parent_multiple_leaves", refresh_same_parent_multiple_leaves},
    {"refresh", "early_stop_when_or_unchanged", refresh_early_stop_when_or_unchanged},
    {"refresh", "shared_between_two_trees", refresh_shared_between_two_trees},
    {"refresh", "rebuild_and_clear_replace_index", refresh_rebuild_and_clear_replace_index},
    {"refresh", "excluded_object_new_mask_observed", refresh_excluded_object_new_mask_observed},

    // transport
    {"transport", "camera_mask_and_inheritance", transport_camera_mask_and_inheritance},
    {"transport", "shadow_inheritance_point_light", transport_shadow_inheritance_point_light},
    {"transport", "shadow_inheritance_rect_light", transport_shadow_inheritance_rect_light},
    {"transport", "refraction_continuation", transport_refraction_continuation},
    {"transport", "path_bounce_inheritance", transport_path_bounce_inheritance},
    {"transport", "default_layer_pixel_invariance", transport_default_layer_pixel_invariance},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
