// Regression tests for visibility-layer support: per-object and per-ray
// visibility masks, incremental BVH cache refresh, and mask inheritance
// through the camera / light transport paths.
//
// Cases are registered with the task driver (task_support/driver.c); no main
// function is defined here.

#include "task_support/check.h"

#include "object/ObjectList.h"
#include "object/Sphere.h"
#include "object/Triangle.h"
#include "object/Cylinder.h"
#include "bvh/Node.h"
#include "Camera.h"
#include "light/PointLight.h"
#include "light/RectangularLight.h"
#include "light/LightList.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kAll = 0xffffffffu;
const Point3 kOrigin(0, 0, 0);
const Vector3 kForward(0, 0, -1);

// Independent reference: brute force over a flat object list with the
// visibility filter applied by hand. Used to cross-check every ObjectList
// query path.
bool reference_intersect(const std::vector<std::shared_ptr<Object>>& objects,
                         const Ray& r, Interval ray_t, IntersectionRecord& rec) {
    bool hit = false;
    double nearest = ray_t.max;
    for (const auto& obj : objects) {
        if ((r.visibility_mask() & obj->visibility_mask()) == 0) continue;
        IntersectionRecord tmp;
        if (obj->intersect(r, Interval(ray_t.min, nearest), tmp)) {
            hit = true;
            nearest = tmp.t;
            rec = tmp;
        }
    }
    return hit;
}

bool same_record(const IntersectionRecord& a, const IntersectionRecord& b) {
    if (a.t != b.t || a.mat_ptr != b.mat_ptr) return false;
    // UV coordinates are only meaningful when the hit material is textured;
    // untextured hits leave whatever was in the record (pre-existing engine
    // behavior, independent of visibility layers).
    if (a.mat_ptr && a.mat_ptr->has_texture) {
        return a.u == b.u && a.v == b.v;
    }
    return true;
}

// Compare the BVH path and the brute-force path of `world` against the
// independently filtered reference for one ray.
int check_all_paths(ObjectList& world, const std::vector<std::shared_ptr<Object>>& objects,
                    const Point3& ro, const Vector3& rd, std::uint32_t mask,
                    double tmin, double tmax) {
    Ray ray(ro, rd, mask);
    IntersectionRecord ref;
    bool ref_hit = reference_intersect(objects, ray, Interval(tmin, tmax), ref);

    IntersectionRecord bvh_rec;
    world.use_bvh = UseBVH::ENABLE;
    bool bvh_hit = world.intersect(ray, Interval(tmin, tmax), bvh_rec);

    IntersectionRecord brute_rec;
    world.use_bvh = UseBVH::DISABLE;
    bool brute_hit = world.intersect(ray, Interval(tmin, tmax), brute_rec);
    world.use_bvh = UseBVH::ENABLE;

    TASK_CHECK(bvh_hit == ref_hit);
    TASK_CHECK(brute_hit == ref_hit);
    if (ref_hit) {
        TASK_CHECK(same_record(ref, bvh_rec));
        TASK_CHECK(same_record(ref, brute_rec));
    }
    return 0;
}

// Sphere that records the visibility mask of every ray that reaches its
// intersection test.
class MaskRecorder : public Sphere {
  public:
    mutable std::vector<std::uint32_t> seen;

    MaskRecorder(const Point3& center, double radius, Material* mat)
        : Sphere(center, radius, mat) {}

    bool intersect(const Ray& r, Interval ray_t, IntersectionRecord& rec) const override {
        seen.push_back(r.visibility_mask());
        return Sphere::intersect(r, ray_t, rec);
    }
};

bool all_seen_equal(const MaskRecorder& rec, std::uint32_t mask) {
    return !rec.seen.empty() &&
           std::all_of(rec.seen.begin(), rec.seen.end(),
                       [mask](std::uint32_t m) { return m == mask; });
}

// 16^3 well-separated spheres on a grid, all on the same visibility layer.
struct GridScene {
    Material mat;
    std::vector<std::shared_ptr<Object>> objects;
    ObjectList world;

    explicit GridScene(std::uint32_t mask) {
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j)
                for (int k = 0; k < 16; ++k) {
                    auto s = std::make_shared<Sphere>(
                        Point3((i - 7.5) * 3.0, (j - 7.5) * 3.0, (k - 7.5) * 3.0), 1.0, &mat);
                    s->set_visibility_mask(mask);
                    objects.push_back(s);
                    world.add(s);
                }
        world.build();
    }
};

// Find an internal node whose two children are both leaf nodes wrapping
// primitives, i.e. two sibling leaves.
const Node* find_parent_of_leaves(const Node* node) {
    if (!node) return nullptr;
    const Node* left_node = node->left ? dynamic_cast<const Node*>(node->left.get()) : nullptr;
    const Node* right_node = node->right ? dynamic_cast<const Node*>(node->right.get()) : nullptr;
    if (left_node && right_node) {
        auto is_leaf = [](const Node* n) {
            return n->left && !n->right && !dynamic_cast<const Node*>(n->left.get());
        };
        if (is_leaf(left_node) && is_leaf(right_node)) return node;
    }
    if (const Node* found = find_parent_of_leaves(left_node)) return found;
    return find_parent_of_leaves(right_node);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Camera make_test_camera() {
    Camera cam;
    cam.image_width = 3;
    cam.image_height = 3;
    cam.samples_per_pixel = 1;
    cam.sampling_method = AntiAliasing::RANDOM;
    cam.aperture = 0.0;
    cam.nbounces = 3;
    cam.num_threads = 1;
    cam.lookfrom = Point3(0, 0, 0);
    cam.lookat = Point3(0, 0, -1);
    return cam;
}

// ---------------------------------------------------------------------------
// query group
// ---------------------------------------------------------------------------

int case_query_defaults_and_primitives() {
    // Defaults: rays and objects both start on all layers.
    Ray default_ray(kOrigin, kForward);
    TASK_CHECK(default_ray.visibility_mask() == kAll);
    Ray layered_ray(kOrigin, kForward, 0x2a);
    TASK_CHECK(layered_ray.visibility_mask() == 0x2a);
    TASK_CHECK(layered_ray.direction().z() == kForward.z() && layered_ray.direction().x() == 0);

    Material mat;
    Sphere sphere(Point3(0, 0, -3), 1.0, &mat);
    TASK_CHECK(sphere.visibility_mask() == kAll);
    sphere.set_visibility_mask(0x5);
    TASK_CHECK(sphere.visibility_mask() == 0x5);
    sphere.set_visibility_mask(kAll);

    // Unmasked behavior is unchanged: same t and material as before.
    IntersectionRecord rec;
    TASK_CHECK(sphere.intersect(default_ray, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 2);
    TASK_CHECK(rec.mat_ptr == &mat);

    // Sphere / Triangle / Cylinder direct tests with masks.
    Material tri_mat, cyl_mat;
    Triangle tri(Point3(-1, -1, -5), Point3(1, -1, -5), Point3(0, 1, -5), &tri_mat);
    Cylinder cyl(Point3(0, 0, -9), Vector3(0, 1, 0), 1.0, 1.0, &cyl_mat);

    Ray visible_ray(kOrigin, kForward, 0x1);
    Ray hidden_ray(kOrigin, kForward, 0x2);
    Ray zero_ray(kOrigin, kForward, 0);  // zero query layer: sees nothing

    Object* primitives[3] = {&sphere, &tri, &cyl};
    const double expected_t[3] = {2, 5, 8};
    for (int i = 0; i < 3; ++i) {
        primitives[i]->set_visibility_mask(0x1);
        IntersectionRecord hit;
        TASK_CHECK(primitives[i]->intersect(visible_ray, Interval(0.001, 100), hit));
        TASK_CHECK(hit.t == expected_t[i]);
        // Disjoint layers: treated as non-existent.
        TASK_CHECK(!primitives[i]->intersect(hidden_ray, Interval(0.001, 100), hit));
        // Zero query layer counts as hidden too.
        TASK_CHECK(!primitives[i]->intersect(zero_ray, Interval(0.001, 100), hit));
        // Object on no layer is invisible to every ray, including full-mask ones.
        primitives[i]->set_visibility_mask(0);
        TASK_CHECK(!primitives[i]->intersect(default_ray, Interval(0.001, 100), hit));
        primitives[i]->set_visibility_mask(0x1);
    }

    // UV and material survive a masked hit unchanged.
    Material tex_mat;
    tex_mat.has_texture = true;
    tex_mat.texture_width = 1;
    tex_mat.texture_height = 1;
    tex_mat.texture_data = {255, 0, 0};
    Sphere textured(Point3(0, 0, -3), 1.0, &tex_mat);
    textured.set_visibility_mask(0x2);
    IntersectionRecord masked_hit, plain_hit;
    TASK_CHECK(textured.intersect(Ray(kOrigin, kForward, 0x2), Interval(0.001, 100), masked_hit));
    textured.set_visibility_mask(kAll);
    TASK_CHECK(textured.intersect(default_ray, Interval(0.001, 100), plain_hit));
    TASK_CHECK(same_record(masked_hit, plain_hit));
    return 0;
}

int case_query_list_paths_and_hidden_near() {
    Material near_mat, tri_mat, far_mat, cyl_mat;
    far_mat.has_texture = true;  // exercise UV preservation through the list
    far_mat.texture_width = 1;
    far_mat.texture_height = 1;
    far_mat.texture_data = {0, 255, 0};

    auto near = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &near_mat);   // t = 2
    near->set_visibility_mask(0x1);
    auto tri = std::make_shared<Triangle>(Point3(-1, -1, -5), Point3(1, -1, -5),
                                          Point3(0, 1, -5), &tri_mat);        // t = 5
    tri->set_visibility_mask(0x4);
    auto far = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &far_mat);     // t = 6
    far->set_visibility_mask(0x2);
    auto cyl = std::make_shared<Cylinder>(Point3(0, 0, -9), Vector3(0, 1, 0),
                                          1.0, 1.0, &cyl_mat);                // t = 8
    cyl->set_visibility_mask(0x1);

    std::vector<std::shared_ptr<Object>> all = {near, tri, far, cyl};
    ObjectList world;
    for (const auto& o : all) world.add(o);
    world.build();

    const std::uint32_t masks[] = {kAll, 0x1, 0x2, 0x4, 0x6, 0x5, 0x8, 0};
    const double windows[] = {100, 7, 3, 5};
    for (std::uint32_t mask : masks) {
        for (double tmax : windows) {
            TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, mask, 0.001, tmax) == 0);
        }
    }

    // Hidden near surface must not shrink the t-window for later candidates:
    // with only layer 0x2 visible, the far sphere is hit at its own t = 6.
    Ray ray(kOrigin, kForward, 0x2);
    IntersectionRecord rec;
    world.use_bvh = UseBVH::ENABLE;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 6);
    TASK_CHECK(rec.mat_ptr == &far_mat);
    // Finite distance: the same hit falls outside a shorter window.
    TASK_CHECK(!world.intersect(ray, Interval(0.001, 3), rec));
    world.use_bvh = UseBVH::DISABLE;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 6);
    TASK_CHECK(!world.intersect(ray, Interval(0.001, 3), rec));
    world.use_bvh = UseBVH::ENABLE;
    return 0;
}

int case_query_duplicates_and_excluded() {
    Material dup_mat, back_mat, exc_mat;
    auto dup = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &dup_mat);      // t = 2
    auto back = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &back_mat);    // t = 6
    auto exc = std::make_shared<Sphere>(Point3(0, 0, -5), 1.0, &exc_mat, true); // t = 4, excluded

    ObjectList world;
    world.add(dup);
    world.add(dup);  // same shared_ptr twice: two leaf occurrences
    world.add(back);
    world.add(exc);
    world.build();
    TASK_CHECK(world.excluded_objects.size() == 1);

    std::vector<std::shared_ptr<Object>> all = {dup, dup, back, exc};

    // Default layers: duplicated object wins at t = 2 on every path.
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, kAll, 0.001, 100) == 0);

    // Hide the duplicated object from layer 0x1 and refresh the BVH caches.
    dup->set_visibility_mask(0x2);
    VisibilityRefreshStats stats = world.refresh_visibility({dup.get()});
    TASK_CHECK(stats.object_reads == 1);  // one read, two leaf occurrences
    // Both occurrences are gone for layer 0x1: the excluded object shows at t = 4.
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x1, 0.001, 100) == 0);
    Ray ray(kOrigin, kForward, 0x1);
    IntersectionRecord rec;
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 4);
    TASK_CHECK(rec.mat_ptr == &exc_mat);

    // Hide the excluded object as well: only the back sphere remains.
    exc->set_visibility_mask(0x4);
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x1, 0.001, 100) == 0);
    TASK_CHECK(world.intersect(ray, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 6);
    TASK_CHECK(rec.mat_ptr == &back_mat);
    return 0;
}

// ---------------------------------------------------------------------------
// refresh group
// ---------------------------------------------------------------------------

int case_refresh_empty_unknown_excluded() {
    Material mat_a, mat_b, mat_e, mat_x;
    auto a = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mat_a);  // t = 2
    auto b = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &mat_b);  // t = 6
    auto e = std::make_shared<Sphere>(Point3(0, 0, -5), 1.0, &mat_e, true);  // excluded, t = 4
    a->set_visibility_mask(0x2);
    b->set_visibility_mask(0x2);

    ObjectList world;
    world.add(a);
    world.add(b);
    world.add(e);
    world.build();
    std::vector<std::shared_ptr<Object>> all = {a, b, e};

    // Empty dirty set: nothing happens.
    VisibilityRefreshStats stats = world.refresh_visibility({});
    TASK_CHECK(stats.object_reads == 0 && stats.node_reductions == 0);

    // Object that was never added: no work.
    Sphere stranger(Point3(0, 0, -1), 1.0, &mat_x);
    stats = world.refresh_visibility({&stranger});
    TASK_CHECK(stats.object_reads == 0 && stats.node_reductions == 0);

    // Excluded objects are not in the geometry tree: refreshing them is a
    // no-op, but queries still see their new mask immediately.
    e->set_visibility_mask(0x1);
    stats = world.refresh_visibility({e.get()});
    TASK_CHECK(stats.object_reads == 0 && stats.node_reductions == 0);
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x1, 0.001, 100) == 0);
    Ray layer1(kOrigin, kForward, 0x1);
    IntersectionRecord rec;
    TASK_CHECK(world.intersect(layer1, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 4 && rec.mat_ptr == &mat_e);  // excluded object visible on its layer
    e->set_visibility_mask(0x2);
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x1, 0.001, 100) == 0);
    TASK_CHECK(!world.intersect(layer1, Interval(0.001, 100), rec));  // all hidden from 0x1

    // Duplicates in the dirty list are processed once: identical work to a
    // singleton refresh of the same object.
    a->set_visibility_mask(0x4);
    VisibilityRefreshStats single = world.refresh_visibility({a.get()});
    a->set_visibility_mask(0x8);
    VisibilityRefreshStats duplicated = world.refresh_visibility({a.get(), a.get(), a.get()});
    TASK_CHECK(single.object_reads == 1);
    TASK_CHECK(duplicated.object_reads == single.object_reads);
    TASK_CHECK(duplicated.node_reductions == single.node_reductions);
    return 0;
}

int case_refresh_single_leaf_local_work() {
    GridScene scene(0x1);

    // Snapshot the tree identity and geometry.
    const Node* root_before = scene.world.root.get();
    const Object* left_before = scene.world.root->left.get();
    const Object* right_before = scene.world.root->right.get();
    BoundingBox bbox_before = scene.world.root->bounding_box();

    Object* victim = scene.objects[100].get();
    victim->set_visibility_mask(0x2);
    VisibilityRefreshStats stats = scene.world.refresh_visibility({victim});

    // Work is local: one object read, a handful of node reductions for a
    // 4096-object tree — never a full scan.
    TASK_CHECK(stats.object_reads == 1);
    TASK_CHECK(stats.node_reductions >= 2);
    TASK_CHECK(stats.node_reductions <= 64);

    // Root pointer, node connections, and geometry boxes are preserved.
    TASK_CHECK(scene.world.root.get() == root_before);
    TASK_CHECK(scene.world.root->left.get() == left_before);
    TASK_CHECK(scene.world.root->right.get() == right_before);
    BoundingBox bbox_after = scene.world.root->bounding_box();
    TASK_CHECK(bbox_after.x.min == bbox_before.x.min && bbox_after.x.max == bbox_before.x.max);
    TASK_CHECK(bbox_after.y.min == bbox_before.y.min && bbox_after.y.max == bbox_before.y.max);
    TASK_CHECK(bbox_after.z.min == bbox_before.z.min && bbox_after.z.max == bbox_before.z.max);

    // Queries on both layers agree with the filtered reference afterwards.
    // objects[100] sits at (i, j, k) = (0, 6, 4) -> (-22.5, -4.5, -10.5).
    Point3 above(-22.5, -4.5, -0.5);
    TASK_CHECK(check_all_paths(scene.world, scene.objects, above, kForward, 0x2, 0.001, 100) == 0);
    TASK_CHECK(check_all_paths(scene.world, scene.objects, above, kForward, 0x1, 0.001, 100) == 0);
    Ray layer2(above, kForward, 0x2);
    IntersectionRecord rec;
    TASK_CHECK(scene.world.intersect(layer2, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 9);  // only the moved object is visible to layer 0x2
    return 0;
}

int case_refresh_sibling_leaves_union() {
    GridScene scene(0x1);

    const Node* parent = find_parent_of_leaves(scene.world.root.get());
    TASK_CHECK(parent != nullptr);
    const Node* leaf_a = dynamic_cast<const Node*>(parent->left.get());
    const Node* leaf_b = dynamic_cast<const Node*>(parent->right.get());
    TASK_CHECK(leaf_a != nullptr && leaf_b != nullptr);
    Object* obj_a = leaf_a->left.get();
    Object* obj_b = leaf_b->left.get();
    TASK_CHECK(obj_a != nullptr && obj_b != nullptr && obj_a != obj_b);

    // Single leaf change: one read, leaf plus its ancestor chain.
    obj_a->set_visibility_mask(0x2);
    VisibilityRefreshStats single = scene.world.refresh_visibility({obj_a});
    TASK_CHECK(single.object_reads == 1);
    TASK_CHECK(single.node_reductions >= 2);

    // Restore, then change both siblings at once.
    obj_a->set_visibility_mask(0x1);
    scene.world.refresh_visibility({obj_a});
    obj_a->set_visibility_mask(0x2);
    obj_b->set_visibility_mask(0x2);
    VisibilityRefreshStats both = scene.world.refresh_visibility({obj_a, obj_b});

    // The shared ancestors are reduced once: work grows with the union of
    // occurrences and ancestors, not with the sum.
    TASK_CHECK(both.object_reads == 2);
    TASK_CHECK(both.node_reductions == single.node_reductions + 1);
    return 0;
}

int case_refresh_early_stop() {
    GridScene scene(kAll);  // every cache starts at all-layers

    Object* victim = scene.objects[7].get();
    victim->set_visibility_mask(0xfffffffeu);  // clear one bit
    VisibilityRefreshStats stats = scene.world.refresh_visibility({victim});

    // The leaf changes, but the parent OR stays all-layers thanks to its
    // other child, so propagation stops immediately.
    TASK_CHECK(stats.object_reads == 1);
    TASK_CHECK(stats.node_reductions == 2);

    // The cleared bit is nevertheless hidden at query time.
    // objects[7] sits at (i, j, k) = (0, 0, 7) -> (-22.5, -22.5, -1.5).
    Point3 above(-22.5, -22.5, 30.0);
    TASK_CHECK(check_all_paths(scene.world, scene.objects, above, kForward, 0x1, 0.001, 100) == 0);
    return 0;
}

int case_refresh_shared_trees_toggle() {
    Material shared_mat, d1_mat, d2_mat;
    auto shared = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &shared_mat);  // t = 2
    auto d1 = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &d1_mat);          // t = 6
    auto d2 = std::make_shared<Sphere>(Point3(0, 0, -9), 1.0, &d2_mat);          // t = 8
    shared->set_visibility_mask(0x1);

    // The same shared_ptr appears twice in the first tree and once in a
    // second, independent tree.
    ObjectList w1;
    w1.add(shared);
    w1.add(shared);
    w1.add(d1);
    w1.build();
    ObjectList w2;
    w2.add(shared);
    w2.add(d2);
    w2.build();

    std::vector<std::shared_ptr<Object>> all1 = {shared, shared, d1};
    std::vector<std::shared_ptr<Object>> all2 = {shared, d2};

    for (int iter = 0; iter < 3; ++iter) {
        // Layer the shared object is currently on (toggles every iteration).
        const std::uint32_t on_layer = (iter % 2 == 0) ? 0x1u : 0x2u;
        const std::uint32_t off_layer = (iter % 2 == 0) ? 0x2u : 0x1u;

        TASK_CHECK(check_all_paths(w1, all1, kOrigin, kForward, on_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w1, all1, kOrigin, kForward, off_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w2, all2, kOrigin, kForward, on_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w2, all2, kOrigin, kForward, off_layer, 0.001, 100) == 0);
        Ray on_ray(kOrigin, kForward, on_layer);
        Ray off_ray(kOrigin, kForward, off_layer);
        IntersectionRecord rec;
        TASK_CHECK(w1.intersect(on_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 2 && rec.mat_ptr == &shared_mat);
        TASK_CHECK(w1.intersect(off_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 6 && rec.mat_ptr == &d1_mat);  // shared hidden from off_layer
        TASK_CHECK(w2.intersect(off_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 8 && rec.mat_ptr == &d2_mat);

        // Toggle the shared object's layer and refresh every scene using it.
        shared->set_visibility_mask(off_layer);
        VisibilityRefreshStats s1 = w1.refresh_visibility({shared.get()});
        VisibilityRefreshStats s2 = w2.refresh_visibility({shared.get()});
        TASK_CHECK(s1.object_reads == 1);  // two occurrences, one mask read
        TASK_CHECK(s1.node_reductions >= 2 && s1.node_reductions <= 8);
        TASK_CHECK(s2.object_reads == 1);

        // Both trees see the new layer afterwards.
        TASK_CHECK(check_all_paths(w1, all1, kOrigin, kForward, off_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w1, all1, kOrigin, kForward, on_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w2, all2, kOrigin, kForward, off_layer, 0.001, 100) == 0);
        TASK_CHECK(check_all_paths(w2, all2, kOrigin, kForward, on_layer, 0.001, 100) == 0);
        TASK_CHECK(w1.intersect(off_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 2 && rec.mat_ptr == &shared_mat);
        TASK_CHECK(w2.intersect(off_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 2 && rec.mat_ptr == &shared_mat);
        TASK_CHECK(w1.intersect(on_ray, Interval(0.001, 100), rec));
        TASK_CHECK(rec.t == 6 && rec.mat_ptr == &d1_mat);
    }
    return 0;
}

int case_refresh_rebuild_and_clear() {
    Material n_mat, e_mat, m_mat;
    auto n = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &n_mat);          // t = 2
    auto e = std::make_shared<Sphere>(Point3(0, 0, -5), 1.0, &e_mat, true);    // excluded, t = 4

    ObjectList world;
    world.add(n);
    world.add(e);
    world.build();
    TASK_CHECK(world.excluded_objects.size() == 1);
    world.build();  // rebuild replaces the excluded list instead of appending
    TASK_CHECK(world.excluded_objects.size() == 1);

    // Refresh works on the rebuilt tree.
    n->set_visibility_mask(0x1);
    VisibilityRefreshStats stats = world.refresh_visibility({n.get()});
    TASK_CHECK(stats.object_reads == 1);
    std::vector<std::shared_ptr<Object>> all = {n, e};
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x1, 0.001, 100) == 0);
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x2, 0.001, 100) == 0);

    // Adding an object and rebuilding installs a fresh index over the new tree.
    auto m = std::make_shared<Sphere>(Point3(0, 0, -7), 1.0, &m_mat);          // t = 6
    m->set_visibility_mask(0x4);
    world.add(m);
    world.build();
    TASK_CHECK(world.excluded_objects.size() == 1);
    all.push_back(m);
    n->set_visibility_mask(0x4);
    stats = world.refresh_visibility({n.get()});
    TASK_CHECK(stats.object_reads == 1);
    TASK_CHECK(check_all_paths(world, all, kOrigin, kForward, 0x4, 0.001, 100) == 0);
    Ray layer4(kOrigin, kForward, 0x4);
    IntersectionRecord rec;
    TASK_CHECK(world.intersect(layer4, Interval(0.001, 100), rec));
    TASK_CHECK(rec.t == 2 && rec.mat_ptr == &n_mat);

    // clear() drops the tree, the excluded list, and the index.
    world.clear();
    TASK_CHECK(world.root == nullptr);
    TASK_CHECK(world.excluded_objects.empty());
    stats = world.refresh_visibility({n.get()});  // stale pointer: no work, no crash
    TASK_CHECK(stats.object_reads == 0 && stats.node_reductions == 0);
    TASK_CHECK(!world.intersect(Ray(kOrigin, kForward), Interval(0.001, 100), rec));

    // The list is usable again afterwards.
    world.add(n);
    world.build();
    TASK_CHECK(world.root != nullptr);
    TASK_CHECK(check_all_paths(world, {n}, kOrigin, kForward, 0x4, 0.001, 100) == 0);
    return 0;
}

// ---------------------------------------------------------------------------
// transport group
// ---------------------------------------------------------------------------

int case_transport_camera_primary() {
    Material hidden_mat, rec_mat;
    auto hidden = std::make_shared<MaskRecorder>(Point3(0, 0, -2), 0.9, &hidden_mat);
    hidden->set_visibility_mask(0x1);
    auto recorder = std::make_shared<MaskRecorder>(Point3(0, 0, -5), 2.0, &rec_mat);
    ObjectList world;
    world.add(hidden);
    world.add(recorder);
    world.build();
    PointLight light(Point3(0, 5, -5), Radiance(1, 1, 1));

    // Camera querying only layer 0x10: primary rays pass through the hidden
    // sphere and reach the recorder carrying the camera's mask.
    Camera cam = make_test_camera();
    cam.visibility_mask = 0x10;
    std::vector<Radiance> pixels = cam.render_path_parallel(world, light, 1);
    TASK_CHECK(pixels.size() == 9);
    TASK_CHECK(all_seen_equal(*recorder, 0x10));
    TASK_CHECK(hidden->seen.empty());  // rejected before its intersection test

    // Control: a normal all-layer camera is blocked by the occluder.
    hidden->seen.clear();
    recorder->seen.clear();
    Camera control = make_test_camera();
    std::vector<Radiance> control_pixels = control.render_path_parallel(world, light, 1);
    TASK_CHECK(control_pixels.size() == 9);
    TASK_CHECK(all_seen_equal(*hidden, kAll));
    return 0;
}

int case_transport_phong_reflection_refraction() {
    PointLight light(Point3(0, 5, -5), Radiance(1, 1, 1));
    const auto tmp = std::filesystem::temp_directory_path();

    // (a) Reflection: mirror in front, hidden occluder and recorder behind
    // the camera along the reflection path.
    Material mirror_mat, occ_mat, rec_mat;
    mirror_mat.is_reflective = true;
    mirror_mat.reflectivity = 1.0;
    occ_mat.diffusecolor = Radiance(1, 0, 0);
    rec_mat.diffusecolor = Radiance(0, 0, 1);
    auto mirror = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &mirror_mat);
    auto occluder = std::make_shared<MaskRecorder>(Point3(0, 0, 2), 0.5, &occ_mat);
    occluder->set_visibility_mask(0x1);
    auto recorder = std::make_shared<MaskRecorder>(Point3(0, 0, 5), 1.0, &rec_mat);
    ObjectList world_a;
    world_a.add(mirror);
    world_a.add(occluder);
    world_a.add(recorder);
    world_a.build();

    Camera cam = make_test_camera();
    cam.image_width = 1;
    cam.image_height = 1;
    cam.render_mode = RenderMode::PHONG;
    cam.nbounces = 4;
    cam.visibility_mask = 0x10;
    cam.filename = (tmp / "visibility_phong_masked.ppm").string();
    cam.render(world_a, light);

    TASK_CHECK(all_seen_equal(*recorder, 0x10));   // reflected ray inherited the mask
    TASK_CHECK(occluder->seen.empty());            // hidden from the reflected ray

    occluder->seen.clear();
    recorder->seen.clear();
    Camera control = make_test_camera();
    control.image_width = 1;
    control.image_height = 1;
    control.render_mode = RenderMode::PHONG;
    control.nbounces = 4;
    control.filename = (tmp / "visibility_phong_control.ppm").string();
    control.render(world_a, light);

    TASK_CHECK(all_seen_equal(*occluder, kAll));  // full-layer control hits the occluder
    std::string masked_img = read_file(tmp / "visibility_phong_masked.ppm");
    std::string control_img = read_file(tmp / "visibility_phong_control.ppm");
    TASK_CHECK(!masked_img.empty() && !control_img.empty());
    TASK_CHECK(masked_img != control_img);  // mirror shows different objects

    // (b) Refraction: glass sphere, hidden occluder and recorder behind it.
    Material glass_mat, occ2_mat, rec2_mat;
    glass_mat.is_refractive = true;
    glass_mat.refractiveindex = 1.5;
    auto glass = std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &glass_mat);
    auto occluder2 = std::make_shared<MaskRecorder>(Point3(0, 0, -4.6), 0.4, &occ2_mat);
    occluder2->set_visibility_mask(0x1);
    auto recorder2 = std::make_shared<MaskRecorder>(Point3(0, 0, -6.5), 1.0, &rec2_mat);
    ObjectList world_b;
    world_b.add(glass);
    world_b.add(occluder2);
    world_b.add(recorder2);
    world_b.build();

    Camera cam_b = make_test_camera();
    cam_b.image_width = 1;
    cam_b.image_height = 1;
    cam_b.render_mode = RenderMode::PHONG;
    cam_b.nbounces = 4;
    cam_b.visibility_mask = 0x10;
    cam_b.filename = (tmp / "visibility_phong_refr_masked.ppm").string();
    cam_b.render(world_b, light);

    TASK_CHECK(all_seen_equal(*recorder2, 0x10));  // refracted ray inherited the mask
    TASK_CHECK(occluder2->seen.empty());

    occluder2->seen.clear();
    recorder2->seen.clear();
    Camera control_b = make_test_camera();
    control_b.image_width = 1;
    control_b.image_height = 1;
    control_b.render_mode = RenderMode::PHONG;
    control_b.nbounces = 4;
    control_b.filename = (tmp / "visibility_phong_refr_control.ppm").string();
    control_b.render(world_b, light);

    TASK_CHECK(all_seen_equal(*occluder2, kAll));

    std::filesystem::remove(tmp / "visibility_phong_masked.ppm");
    std::filesystem::remove(tmp / "visibility_phong_control.ppm");
    std::filesystem::remove(tmp / "visibility_phong_refr_masked.ppm");
    std::filesystem::remove(tmp / "visibility_phong_refr_control.ppm");
    return 0;
}

int case_transport_path_inheritance() {
    PointLight light(Point3(0, 5, -5), Radiance(1, 1, 1));
    Material mirror_mat, glass_mat, diffuse_mat, brdf_mat;
    mirror_mat.is_reflective = true;
    mirror_mat.reflectivity = 1.0;
    glass_mat.is_refractive = true;
    glass_mat.refractiveindex = 1.5;

    struct Scenario {
        Material* mat;
        bool allow_brdf;
    };
    Scenario scenarios[] = {
        {&mirror_mat, false},   // path-phong reflection
        {&glass_mat, false},    // path-phong refraction
        {&diffuse_mat, false},  // path-phong diffuse scatter
        {&brdf_mat, true},      // path-brdf scatter
    };

    for (const Scenario& scenario : scenarios) {
        // A huge recorder encloses the scene so every secondary ray lands on
        // it and reports the mask it carried.
        Material rec_mat;
        auto recorder = std::make_shared<MaskRecorder>(Point3(0, 0, 0), 50.0, &rec_mat);
        auto subject = std::make_shared<Sphere>(Point3(0, 0, -3), 1.5, scenario.mat);
        ObjectList world;
        world.add(subject);
        world.add(recorder);
        world.build();

        Camera cam = make_test_camera();
        cam.allow_brdf = scenario.allow_brdf;
        cam.visibility_mask = 0x10;
        cam.render_path_parallel(world, light, 1);
        TASK_CHECK(all_seen_equal(*recorder, 0x10));

        // Control: default all-layer camera.
        recorder->seen.clear();
        Camera control = make_test_camera();
        control.allow_brdf = scenario.allow_brdf;
        control.render_path_parallel(world, light, 1);
        TASK_CHECK(all_seen_equal(*recorder, kAll));
    }
    return 0;
}

int case_transport_point_light_shadow() {
    PointLight light(Point3(0, 0, 10), Radiance(1, 1, 1));
    const Point3 p(0, 0, 0);
    const Vector3 up(0, 0, 1);

    // Occluder between the shaded point and the light, on layer 0x1 only.
    Material occ_mat;
    auto occluder = std::make_shared<Sphere>(Point3(0, 0, 5), 1.0, &occ_mat);
    occluder->set_visibility_mask(0x1);
    ObjectList world;
    world.add(occluder);
    world.build();

    TASK_CHECK(light.shadow_factor(p, world, 0x10) == 1.0);   // occluder hidden from query
    TASK_CHECK(light.shadow_factor(p, world) == 0.1);         // old 2-arg call: still blocked
    TASK_CHECK(light.shadow_factor(p, world, kAll) == 0.1);
    TASK_CHECK(light.shadow_factor(p, world, 0x1) == 0.1);    // matching layer: blocked
    TASK_CHECK(light.shadow_factor(p, world, 0) == 1.0);      // zero query layer: nothing blocks

    // Shading entry points, old and new signatures.
    Material surf_mat;
    IntersectionRecord rec;
    rec.mat_ptr = &surf_mat;
    Radiance ambient(0.3, 0.3, 0.3);
    Radiance lit = light.phong_shading(p, up, up, rec, world, ambient, 0x10);
    Radiance shaded = light.phong_shading(p, up, up, rec, world, ambient);  // old call: full layers
    TASK_CHECK(lit.x() > shaded.x());
    TASK_CHECK(std::abs(shaded.x() - lit.x() * 0.1) < 1e-12);

    // LightList forwards the mask to its lights.
    LightList lights;
    lights.add(std::make_shared<PointLight>(Point3(0, 0, 10), Radiance(1, 1, 1)));
    Radiance via_list = lights.phong_shading(p, up, up, rec, world, ambient, 0x10);
    TASK_CHECK(std::abs(via_list.x() - lit.x()) < 1e-12);

    // Refraction continuation: the shadow ray refracts through a glass
    // sphere; the continuation segment must carry the query mask past a
    // hidden blocker surrounding the light.
    Material glass_mat, giant_mat;
    glass_mat.is_refractive = true;
    glass_mat.refractiveindex = 1.5;
    auto glass = std::make_shared<Sphere>(Point3(0, 0, 4), 1.0, &glass_mat);
    auto giant = std::make_shared<Sphere>(Point3(0, 0, 10), 3.0, &giant_mat);
    giant->set_visibility_mask(0x1);
    ObjectList world2;
    world2.add(glass);
    world2.add(giant);
    world2.build();

    double masked = light.shadow_factor(p, world2, 0x10);
    double full = light.shadow_factor(p, world2);
    TASK_CHECK(std::abs(masked - 4.0 / 9.0) < 1e-9);         // two refractions, blocker skipped
    TASK_CHECK(std::abs(full - 4.0 / 9.0 * 0.1) < 1e-9);     // continuation hits the blocker
    return 0;
}

int case_transport_rect_light_shadow() {
    RectangularLight light(Point3(0, 0, 10), Vector3(1, 0, 0), Vector3(0, 1, 0),
                           0.2, 0.2, Radiance(1, 1, 1), 4);
    const Point3 p(0, 0, 0);
    const Vector3 up(0, 0, 1);
    Material surf_mat;
    IntersectionRecord rec;
    rec.mat_ptr = &surf_mat;
    Radiance ambient(0.3, 0.3, 0.3);

    // Large occluder on layer 0x1 covering every sample of the small light.
    Material occ_mat;
    auto occluder = std::make_shared<Sphere>(Point3(0, 0, 5), 2.0, &occ_mat);
    occluder->set_visibility_mask(0x1);
    ObjectList world;
    world.add(occluder);
    world.build();

    generator.seed(1234);
    Radiance lit = light.phong_shading(p, up, up, rec, world, ambient, 0x10);
    generator.seed(1234);
    Radiance blocked = light.phong_shading(p, up, up, rec, world, ambient);  // old call: full layers
    TASK_CHECK(lit.x() > 0);
    TASK_CHECK(blocked.x() == 0);  // rectangular shadows are fully dark

    // Refraction continuation behind a glass sphere: hidden blocker around
    // the light is skipped only when the query layer says so.
    Material glass_mat, giant_mat;
    glass_mat.is_refractive = true;
    glass_mat.refractiveindex = 1.5;
    auto glass = std::make_shared<Sphere>(Point3(0, 0, 4), 1.0, &glass_mat);
    auto giant = std::make_shared<Sphere>(Point3(0, 0, 10), 3.0, &giant_mat);
    giant->set_visibility_mask(0x1);
    ObjectList world2;
    world2.add(glass);
    world2.add(giant);
    world2.build();

    generator.seed(5678);
    Radiance lit2 = light.phong_shading(p, up, up, rec, world2, ambient, 0x10);
    generator.seed(5678);
    Radiance blocked2 = light.phong_shading(p, up, up, rec, world2, ambient);
    TASK_CHECK(lit2.x() > 0);
    TASK_CHECK(blocked2.x() == 0);
    return 0;
}

int case_transport_no_rng_drift() {
    // Two identical scenes, except that the first contains an extra sphere
    // that is invisible to every layer (mask 0) and sits right in front of
    // the visible sphere. Filtering it out must not consume random numbers,
    // so both renders produce identical pixels and identical RNG state.
    Material diffuse_mat;
    ObjectList world_a, world_b;
    world_a.add(std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &diffuse_mat));
    auto ghost = std::make_shared<Sphere>(Point3(0, 0, -1), 0.5, &diffuse_mat);
    ghost->set_visibility_mask(0);
    world_a.add(ghost);
    world_a.build();
    world_b.add(std::make_shared<Sphere>(Point3(0, 0, -3), 1.0, &diffuse_mat));
    world_b.build();

    PointLight light(Point3(0, 5, -5), Radiance(1, 1, 1));
    const auto tmp = std::filesystem::temp_directory_path();

    Camera cam = make_test_camera();  // default all-layer camera
    cam.image_width = 2;
    cam.image_height = 2;
    cam.nbounces = 2;
    cam.render_mode = RenderMode::PATH;

    generator.seed(20261118u);
    cam.filename = (tmp / "visibility_rng_a.ppm").string();
    cam.render(world_a, light);
    double after_a = random_double();

    generator.seed(20261118u);
    cam.filename = (tmp / "visibility_rng_b.ppm").string();
    cam.render(world_b, light);
    double after_b = random_double();

    std::string img_a = read_file(tmp / "visibility_rng_a.ppm");
    std::string img_b = read_file(tmp / "visibility_rng_b.ppm");
    TASK_CHECK(!img_a.empty());
    TASK_CHECK(img_a == img_b);   // same pixels under the default layer
    TASK_CHECK(after_a == after_b);  // same random stream position

    std::filesystem::remove(tmp / "visibility_rng_a.ppm");
    std::filesystem::remove(tmp / "visibility_rng_b.ppm");
    return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"query", "defaults_and_primitives", case_query_defaults_and_primitives},
    {"query", "list_paths_and_hidden_near", case_query_list_paths_and_hidden_near},
    {"query", "duplicates_and_excluded", case_query_duplicates_and_excluded},
    {"refresh", "empty_unknown_excluded", case_refresh_empty_unknown_excluded},
    {"refresh", "single_leaf_local_work", case_refresh_single_leaf_local_work},
    {"refresh", "sibling_leaves_union", case_refresh_sibling_leaves_union},
    {"refresh", "early_stop", case_refresh_early_stop},
    {"refresh", "shared_trees_toggle", case_refresh_shared_trees_toggle},
    {"refresh", "rebuild_and_clear", case_refresh_rebuild_and_clear},
    {"transport", "camera_primary", case_transport_camera_primary},
    {"transport", "phong_reflection_refraction", case_transport_phong_reflection_refraction},
    {"transport", "path_inheritance", case_transport_path_inheritance},
    {"transport", "point_light_shadow", case_transport_point_light_shadow},
    {"transport", "rect_light_shadow", case_transport_rect_light_shadow},
    {"transport", "no_rng_drift", case_transport_no_rng_drift},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
