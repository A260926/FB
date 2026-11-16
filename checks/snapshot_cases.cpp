// =====================================================================================
// Functional checks for persistent, forkable scene snapshots.
//
// Registered through task_cases/task_case_count and driven by task_support/driver.c;
// this translation unit deliberately defines no main().
// =====================================================================================

#include "task_support/check.h"

#include "rtweekend.h"
#include "scene_snapshot.h"
#include "bvh.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "material.h"
#include "hittable_list.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>


namespace {


constexpr double kEps = 1e-10;


bool near(double a, double b, double eps = kEps) {
    return std::fabs(a - b) <= eps * (1.0 + std::fmax(std::fabs(a), std::fabs(b)));
}

bool near_vec(const vec3& a, const vec3& b, double eps = kEps) {
    return near(a.x(), b.x(), eps) && near(a.y(), b.y(), eps) && near(a.z(), b.z(), eps);
}


// --- BVH diagnostics helpers (read-only, driven by the real bvh_node interface) ---

const bvh_node* as_node(const hittable& h) {
    return dynamic_cast<const bvh_node*>(&h);
}

void collect_ids(const bvh_node* node, std::unordered_set<const void*>& ids) {
    if (!node) return;
    ids.insert(node->node_id());
    if (node->is_leaf())
        return;
    collect_ids(dynamic_cast<const bvh_node*>(node->left_child().get()), ids);
    collect_ids(dynamic_cast<const bvh_node*>(node->right_child().get()), ids);
}

size_t count_nodes(const bvh_node* node) {
    std::unordered_set<const void*> ids;
    collect_ids(node, ids);
    return ids.size();
}

// Records the node ids on the root-to-leaf path ending at the given stable slot.
bool path_to_slot(const bvh_node* node, size_t slot, std::vector<const void*>& path) {
    path.push_back(node->node_id());
    if (node->is_leaf()) {
        if (node->leaf_slot() == slot)
            return true;
        path.pop_back();
        return false;
    }

    for (const auto& child : { node->left_child(), node->right_child() }) {
        if (const auto* child_node = dynamic_cast<const bvh_node*>(child.get())) {
            if (path_to_slot(child_node, slot, path))
                return true;
        }
    }
    path.pop_back();
    return false;
}


// A light prototype that records every sampling call and returns canned values,
// so mixture weights and rigid-transform forwarding can be checked exactly.
class recording_light : public hittable {
  public:
    explicit recording_light(double canned_pdf) : canned_pdf(canned_pdf) {
        bbox = aabb(point3(-0.5, -0.5, -0.5), point3(0.5, 0.5, 0.5));
    }

    bool hit(const ray&, interval, hit_record&) const override {
        return false;
    }

    aabb bounding_box() const override { return bbox; }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        ++pdf_calls;
        last_origin = origin;
        last_direction = direction;
        return canned_pdf;
    }

    vec3 random(const point3& origin) const override {
        ++random_calls;
        last_origin = origin;
        return vec3(0.0, 0.0, -1.0);
    }

    mutable int pdf_calls = 0;
    mutable int random_calls = 0;
    mutable point3 last_origin;
    mutable vec3 last_direction;
  private:
    double canned_pdf;
    aabb bbox;
};


// Independent object-space rotation matching rigid_rotate_y's convention.
point3 rotate_to_object(const vec3& v, double yaw_degrees) {
    auto rad = degrees_to_radians(yaw_degrees);
    double s = std::sin(rad), c = std::cos(rad);
    return point3(c * v.x() - s * v.z(), v.y(), s * v.x() + c * v.z());
}


// Build a flat, independently assembled reference world using the legacy
// translate/rotate_y wrappers (no snapshot, no BVH).
shared_ptr<hittable> flat_wrap(shared_ptr<hittable> object,
                               const vec3& translation, double yaw_degrees) {
    shared_ptr<hittable> node = object;
    if (yaw_degrees != 0.0)
        node = make_shared<rotate_y>(node, yaw_degrees);
    return make_shared<translate>(node, translation);
}


std::string render_to_string(camera& cam, const scene_snapshot& snapshot) {
    std::ostringstream out;
    auto* saved = std::cout.rdbuf(out.rdbuf());
    cam.render(snapshot);
    std::cout.rdbuf(saved);
    return out.str();
}

std::string render_to_string(camera& cam, const hittable& world, const hittable& lights) {
    std::ostringstream out;
    auto* saved = std::cout.rdbuf(out.rdbuf());
    cam.render(world, lights);
    std::cout.rdbuf(saved);
    return out.str();
}

size_t count_ppm_pixels(const std::string& ppm) {
    std::istringstream in(ppm);
    std::string magic;
    int w, h, maxv;
    if (!(in >> magic >> w >> h >> maxv) || magic != "P3")
        return 0;
    size_t pixels = 0;
    int r, g, b;
    while (in >> r >> g >> b)
        ++pixels;
    return pixels;
}


// =====================================================================================
// persistence
// =====================================================================================

int persistence_flat_equivalence() {
    auto matte_a = make_shared<lambertian>(color(0.8, 0.2, 0.2));
    auto matte_b = make_shared<lambertian>(color(0.2, 0.8, 0.3));
    auto sphere_a = make_shared<sphere>(point3(0, 0, 0), 0.5, matte_a);
    auto sphere_b = make_shared<sphere>(point3(0, 0, 0), 0.7, matte_b);

    std::vector<scene_prototype> prototypes = {
        scene_prototype{sphere_a, false},
        scene_prototype{sphere_b, false},
    };
    std::vector<scene_instance_desc> instances = {
        scene_instance_desc{0, vec3(0, 0, -4), 0.0, true},
        scene_instance_desc{1, vec3(1.5, 0, -6), 35.0, true},
        scene_instance_desc{0, vec3(-3, 0, -5), 90.0, true},
        scene_instance_desc{1, vec3(0, -2, -8), 0.0, false}, // hidden: slot retained
    };

    scene_snapshot snapshot(prototypes, instances);
    TASK_CHECK(snapshot.prototype_count() == 2);
    TASK_CHECK(snapshot.instance_count() == 4);
    TASK_CHECK(snapshot.light_count() == 0);

    // Independent flat reference of the enabled instances, one wrapper per
    // stable slot so the nearest winner can be derived rather than assumed.
    hittable_list flat;
    std::vector<shared_ptr<hittable>> per_slot(instances.size());
    for (size_t slot = 0; slot < instances.size(); slot++) {
        if (!instances[slot].enabled)
            continue;
        auto wrapped = flat_wrap(prototypes[instances[slot].prototype].object,
                                 instances[slot].translation,
                                 instances[slot].yaw_degrees);
        per_slot[slot] = wrapped;
        flat.add(wrapped);
    }

    // Non-unit direction ray.
    ray r(point3(0.2, 0.1, 0.5), vec3(0.15, -0.07, -2.3), 0.4);

    // Expected nearest instance, computed from the independent wrappers.
    size_t expected_slot = invalid_instance_id;
    double nearest_t = infinity;
    for (size_t slot = 0; slot < per_slot.size(); slot++) {
        if (!per_slot[slot])
            continue;
        hit_record probe;
        if (per_slot[slot]->hit(r, interval(0.001, infinity), probe) && probe.t < nearest_t) {
            nearest_t = probe.t;
            expected_slot = slot;
        }
    }
    TASK_CHECK(expected_slot != invalid_instance_id);

    hit_record snap_rec, flat_rec;
    TASK_CHECK(snapshot.world().hit(r, interval(0.001, infinity), snap_rec));
    TASK_CHECK(flat.hit(r, interval(0.001, infinity), flat_rec));

    TASK_CHECK(near(snap_rec.t, flat_rec.t));
    TASK_CHECK(near_vec(snap_rec.p, flat_rec.p));
    TASK_CHECK(near_vec(snap_rec.normal, flat_rec.normal));
    TASK_CHECK(near(snap_rec.u, flat_rec.u));
    TASK_CHECK(near(snap_rec.v, flat_rec.v));
    TASK_CHECK(snap_rec.mat == flat_rec.mat);
    TASK_CHECK(snap_rec.front_face == flat_rec.front_face);

    // Identity is the original input slot of the independently derived winner.
    TASK_CHECK(snap_rec.instance_id == expected_slot);
    TASK_CHECK(flat_rec.instance_id == invalid_instance_id); // legacy shapes stay untagged

    // A ray aimed at the rotated instance reports its stable slot as well.
    ray r2(point3(-3, 0, 1), vec3(0, 0, -1.7), 0.0);
    hit_record rec2;
    TASK_CHECK(snapshot.world().hit(r2, interval(0.001, infinity), rec2));
    TASK_CHECK(rec2.instance_id == 2);

    // Occlusion: the near sphere on this line wins over anything behind it.
    // Origin z=1, center z=-6, radius 0.7 -> front face at z=-5.3, t=6.3/2.
    ray r3(point3(1.5, 0, 1), vec3(0, 0, -2.0), 0.0);
    hit_record rec3;
    TASK_CHECK(snapshot.world().hit(r3, interval(0.001, infinity), rec3));
    TASK_CHECK(rec3.instance_id == 1);
    TASK_CHECK(near(rec3.t, 3.15, 1e-9));

    // Disabled instance is unhittable even though its slot still exists.
    ray r4(point3(0, -2, 1), vec3(0, 0, -1.0), 0.0);
    hit_record rec4;
    TASK_CHECK(!snapshot.world().hit(r4, interval(0.001, infinity), rec4));
    TASK_CHECK(rec4.instance_id == invalid_instance_id);
    return 0;
}


int persistence_fork_rollback_lifetime() {
    auto matte = make_shared<lambertian>(color(0.4, 0.4, 0.6));
    auto glow_mat = make_shared<diffuse_light>(color(3, 3, 2));
    auto ball = make_shared<sphere>(point3(0, 0, 0), 0.6, matte);
    auto lamp = make_shared<quad>(point3(-1, 1, -1), vec3(2, 0, 0), vec3(0, 0, 2), glow_mat);

    // Build inside a scope so the description containers are destroyed before
    // the snapshot is ever queried.
    scene_snapshot parent = [&] {
        std::vector<scene_prototype> protos = {
            scene_prototype{ball, false},
            scene_prototype{lamp, true},
        };
        std::vector<scene_instance_desc> inst = {
            scene_instance_desc{0, vec3(0, 0, -4), 0.0, true},
            scene_instance_desc{0, vec3(2, 0, -4), 0.0, true}, // shares prototype 0
            scene_instance_desc{1, vec3(0, 3, -4), 0.0, true},
        };
        return scene_snapshot(protos, inst);
    }();

    TASK_CHECK(parent.instance_count() == 3);
    TASK_CHECK(parent.light_count() == 1);

    auto parent_root = as_node(parent.world())->node_id();
    size_t parent_nodes = count_nodes(as_node(parent.world()));

    // Branch A: move instance 0, hide instance 1.
    scene_snapshot branch_a = parent.updated(
        {}, {
            {0, scene_instance_desc{0, vec3(5, 0, -4), 0.0, true}},
            {1, scene_instance_desc{0, vec3(2, 0, -4), 0.0, false}},
        });

    // Branch B: replace the prototype shared by instances 0 and 1.
    auto bigger = make_shared<sphere>(point3(0, 0, 0), 1.1, matte);
    scene_snapshot branch_b = parent.updated(
        {{0, scene_prototype{bigger, false}}}, {});

    // Parent is untouched (no write-through): instance 0 still at the old spot.
    ray old_ray(point3(0, 0, 1), vec3(0, 0, -1), 0.0);
    hit_record rec;
    TASK_CHECK(parent.world().hit(old_ray, interval(0.001, infinity), rec));
    TASK_CHECK(rec.instance_id == 0);
    TASK_CHECK(near(rec.p.z(), -3.4, 1e-9)); // radius 0.6
    TASK_CHECK(as_node(parent.world())->node_id() == parent_root);
    TASK_CHECK(count_nodes(as_node(parent.world())) == parent_nodes);

    // Branch A: instance 0 moved away, instance 1 hidden -> the ray now misses.
    hit_record arec;
    TASK_CHECK(!branch_a.world().hit(old_ray, interval(0.001, infinity), arec));

    // Branch B still hits at the enlarged radius.
    hit_record brec;
    TASK_CHECK(branch_b.world().hit(old_ray, interval(0.001, infinity), brec));
    TASK_CHECK(brec.instance_id == 0);
    TASK_CHECK(near(brec.p.z(), -2.9, 1e-9)); // radius 1.1

    // Continue editing from both old branches independently.
    scene_snapshot branch_a2 = branch_a.updated(
        {}, {{1, scene_instance_desc{0, vec3(2, 0, -4), 0.0, true}}});
    scene_snapshot branch_b2 = branch_b.updated(
        {}, {{2, scene_instance_desc{1, vec3(0, 6, -4), 0.0, false}}});
    TASK_CHECK(branch_a2.instance_count() == 3);
    TASK_CHECK(branch_b2.light_count() == 0);
    TASK_CHECK(parent.light_count() == 1);      // parent still has its light
    TASK_CHECK(branch_a.light_count() == 1);

    // Empty update shares the same root and changes nothing.
    scene_snapshot same = parent.updated({}, {});
    TASK_CHECK(as_node(same.world())->node_id() == parent_root);

    // Failed updates roll back completely: every validation error leaves the
    // snapshot's geometry and light set unchanged.
    auto expect_rollback = [&](const std::vector<std::pair<size_t, scene_prototype>>& pu,
                               const std::vector<std::pair<size_t, scene_instance_desc>>& iu)
    -> int {
        bool threw = false;
        try {
            (void) parent.updated(pu, iu);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        TASK_CHECK(threw);
        hit_record r;
        TASK_CHECK(parent.world().hit(old_ray, interval(0.001, infinity), r));
        TASK_CHECK(r.instance_id == 0);
        TASK_CHECK(parent.light_count() == 1);
        TASK_CHECK(as_node(parent.world())->node_id() == parent_root);
        return 0;
    };

    if (expect_rollback({{7, scene_prototype{bigger, false}}}, {})) return 1;           // bad proto slot
    if (expect_rollback({}, {{9, scene_instance_desc{0, vec3(0, 0, 0), 0, true}}})) return 1; // bad instance slot
    if (expect_rollback({{0, scene_prototype{nullptr, false}}}, {})) return 1;          // null object
    if (expect_rollback({}, {{0, scene_instance_desc{0,
                    vec3(infinity, 0, 0), 0.0, true}}})) return 1;                       // non-finite translate
    if (expect_rollback({}, {{0, scene_instance_desc{0, vec3(0, 0, 0),
                    std::nan(""), true}}})) return 1;                                    // non-finite yaw
    if (expect_rollback({}, {{0, scene_instance_desc{42, vec3(0, 0, 0), 0, true}}})) return 1; // bad proto ref
    if (expect_rollback({{0, scene_prototype{bigger, false}},
                     {0, scene_prototype{ball, false}}}, {})) return 1;                  // duplicate slot
    if (expect_rollback({}, {{1, scene_instance_desc{0, vec3(0, 0, 0), 0, true}},
                         {1, scene_instance_desc{0, vec3(1, 0, 0), 0, true}}})) return 1; // duplicate slot

    // Constructor validation as well.
    bool ctor_threw = false;
    try {
        scene_snapshot bad({scene_prototype{nullptr, false}},
                           {scene_instance_desc{0, vec3(0, 0, 0), 0, true}});
    } catch (const std::invalid_argument&) {
        ctor_threw = true;
    }
    TASK_CHECK(ctor_threw);
    return 0;
}


int persistence_empty_and_identity_reset() {
    // Empty scene: world never hits and there are no lights.
    scene_snapshot empty({}, {});
    TASK_CHECK(empty.prototype_count() == 0);
    TASK_CHECK(empty.instance_count() == 0);
    TASK_CHECK(empty.light_count() == 0);
    ray r(point3(0, 0, 0), vec3(0, 0, -1), 0.0);
    hit_record rec;
    TASK_CHECK(!empty.world().hit(r, interval(0.001, infinity), rec));
    TASK_CHECK(empty.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1)) == 0.0);

    // A fresh record is invalid by default.
    hit_record fresh;
    TASK_CHECK(fresh.instance_id == invalid_instance_id);

    // Legacy primitive hits reset a stale identity back to invalid.
    auto matte = make_shared<lambertian>(color(0.5, 0.5, 0.5));
    auto ball = make_shared<sphere>(point3(0, 0, -3), 1.0, matte);
    fresh.instance_id = 123;
    TASK_CHECK(ball->hit(ray(point3(0, 0, 0), vec3(0, 0, -1), 0),
                         interval(0.001, infinity), fresh));
    TASK_CHECK(fresh.instance_id == invalid_instance_id);

    auto panel = make_shared<quad>(point3(-1, -1, -3), vec3(2, 0, 0),
                                   vec3(0, 2, 0), matte);
    fresh.instance_id = 456;
    TASK_CHECK(panel->hit(ray(point3(0, 0, 1), vec3(0, 0, -1), 0),
                          interval(0.001, infinity), fresh));
    TASK_CHECK(fresh.instance_id == invalid_instance_id);
    return 0;
}


// =====================================================================================
// sharing
// =====================================================================================

int sharing_single_leaf_budget() {
    constexpr size_t kCount = 4096;
    auto matte = make_shared<lambertian>(color(0.7, 0.7, 0.7));
    auto ball = make_shared<sphere>(point3(0, 0, 0), 0.3, matte);

    std::vector<scene_prototype> protos = { scene_prototype{ball, false} };
    std::vector<scene_instance_desc> inst;
    inst.reserve(kCount);
    // Widely separated grid placements so every leaf keeps its own bounds.
    for (size_t i = 0; i < kCount; i++) {
        double x = double(i % 64) * 4.0;
        double z = double(i / 64) * 4.0;
        inst.push_back(scene_instance_desc{0, vec3(x, 0, z), 0.0, true});
    }

    scene_snapshot snapshot(protos, inst);
    const bvh_node* old_root = as_node(snapshot.world());
    std::unordered_set<const void*> old_ids;
    collect_ids(old_root, old_ids);
    TASK_CHECK(old_ids.size() == kCount * 2 - 1); // full binary tree

    std::vector<const void*> path;
    TASK_CHECK(path_to_slot(old_root, 777, path));

    scene_snapshot moved = snapshot.updated(
        {}, {{777, scene_instance_desc{0, vec3(1000, 50, -1000), 0.0, true}}});

    const bvh_node* new_root = as_node(moved.world());
    std::unordered_set<const void*> new_ids;
    collect_ids(new_root, new_ids);

    size_t allocated = 0;
    for (const void* id : new_ids)
        if (!old_ids.count(id))
            ++allocated;

    // ceil(log2(4096)) + 2 = 12 + 2 = 14; a single root-to-leaf path is shorter.
    size_t budget = static_cast<size_t>(std::ceil(std::log2(double(kCount)))) + 2;
    TASK_CHECK(allocated <= budget);
    TASK_CHECK(allocated == path.size()); // exactly the distinct path positions

    // The moved instance is hittable far outside every old bounding box.
    ray far_ray(point3(1000, 50, 0), vec3(0, 0, -1.5), 0.0);
    hit_record rec;
    TASK_CHECK(moved.world().hit(far_ray, interval(0.001, infinity), rec));
    TASK_CHECK(rec.instance_id == 777);

    // Old tree connectivity is intact and every unaffected node is shared.
    TASK_CHECK(old_ids.size() == kCount * 2 - 1);
    TASK_CHECK(as_node(snapshot.world())->node_id() == old_root->node_id());
    size_t shared = 0;
    for (const void* id : old_ids)
        if (new_ids.count(id))
            ++shared;
    TASK_CHECK(shared == old_ids.size() - path.size());

    // Hide then re-show: the slot and leaf come back and still hit.
    scene_snapshot hidden = snapshot.updated(
        {}, {{777, scene_instance_desc{0, vec3(1000, 50, -1000), 0.0, false}}});
    TASK_CHECK(!hidden.world().hit(far_ray, interval(0.001, infinity), rec));
    scene_snapshot shown = hidden.updated(
        {}, {{777, scene_instance_desc{0, vec3(1000, 50, -1000), 0.0, true}}});
    TASK_CHECK(shown.world().hit(far_ray, interval(0.001, infinity), rec));
    TASK_CHECK(rec.instance_id == 777);
    return 0;
}


int sharing_batch_ancestors_cloned_once() {
    constexpr size_t kCount = 128;
    auto matte = make_shared<lambertian>(color(0.6, 0.6, 0.6));
    auto ball = make_shared<sphere>(point3(0, 0, 0), 0.3, matte);

    std::vector<scene_prototype> protos = { scene_prototype{ball, false} };
    std::vector<scene_instance_desc> inst;
    inst.reserve(kCount);
    for (size_t i = 0; i < kCount; i++)
        inst.push_back(scene_instance_desc{0, vec3(double(i) * 3.0, 0, 0), 0.0, true});

    scene_snapshot snapshot(protos, inst);
    const bvh_node* old_root = as_node(snapshot.world());

    const std::vector<size_t> replaced = { 3, 4, 5, 6 };

    // Independent expectation: the distinct node positions on the union of the
    // replaced leaves' root-to-leaf paths in the real old tree.
    std::set<const void*> expected_positions;
    for (size_t slot : replaced) {
        std::vector<const void*> path;
        TASK_CHECK(path_to_slot(old_root, slot, path));
        for (const void* id : path)
            expected_positions.insert(id);
    }

    std::vector<std::pair<size_t, scene_instance_desc>> updates;
    for (size_t slot : replaced)
        updates.push_back({slot, scene_instance_desc{0, vec3(double(slot) * 3.0, 8, 0),
                                                     0.0, true}});
    scene_snapshot next = snapshot.updated({}, updates);

    std::unordered_set<const void*> old_ids, new_ids;
    collect_ids(old_root, old_ids);
    collect_ids(as_node(next.world()), new_ids);

    size_t allocated = 0;
    for (const void* id : new_ids)
        if (!old_ids.count(id))
            ++allocated;

    // One allocation per distinct position proves a shared ancestor is cloned a
    // single time rather than once per replaced leaf.
    TASK_CHECK(allocated == expected_positions.size());

    // Naive per-leaf cloning would allocate the sum of path lengths; sharing
    // must save at least one node because all leaves share the root.
    size_t naive_sum = 0;
    for (size_t slot : replaced) {
        std::vector<const void*> path;
        TASK_CHECK(path_to_slot(old_root, slot, path));
        naive_sum += path.size();
    }
    TASK_CHECK(allocated < naive_sum);

    // Total node count is unchanged and unaffected subtrees are still shared.
    TASK_CHECK(new_ids.size() == old_ids.size());
    TASK_CHECK(new_ids.size() == kCount * 2 - 1);
    return 0;
}


int sharing_bvh_prototype() {
    // A prototype may itself be a bvh_node over legacy spheres/quads. Its BLAS is
    // built once and shared by every instance; the snapshot never deep-copies it.
    auto matte = make_shared<lambertian>(color(0.3, 0.4, 0.7));
    hittable_list parts;
    auto part_a = make_shared<sphere>(point3(-0.6, 0, 0), 0.4, matte);
    auto part_b = make_shared<sphere>(point3(0.6, 0, 0), 0.4, matte);
    parts.add(part_a);
    parts.add(part_b);
    auto blas = make_shared<bvh_node>(parts);
    const void* blas_id = blas.get();

    std::vector<scene_prototype> protos = { scene_prototype{blas, false} };
    std::vector<scene_instance_desc> inst = {
        scene_instance_desc{0, vec3(0, 0, -4), 0.0, true},
        scene_instance_desc{0, vec3(3, 0, -5), 45.0, true},
    };
    scene_snapshot snapshot(protos, inst);
    TASK_CHECK(snapshot.prototype_count() == 1);
    TASK_CHECK(snapshot.instance_count() == 2);

    // Both copies hit and report their own stable slot through the shared BLAS.
    ray r0(point3(-0.6, 0, 1), vec3(0, 0, -1.5), 0.0);
    hit_record rec0;
    TASK_CHECK(snapshot.world().hit(r0, interval(0.001, infinity), rec0));
    TASK_CHECK(rec0.instance_id == 0);

    // Instance 1 is rotated 45 deg, so aim straight down at sub-sphere A's
    // rotated world center rather than at the instance center.
    const double yaw = 45.0;
    const double cs = std::cos(degrees_to_radians(yaw));
    const double sn = std::sin(degrees_to_radians(yaw));
    const point3 part_a_obj(-0.6, 0, 0);
    point3 part_a_world(
        cs * part_a_obj.x() + sn * part_a_obj.z(),
        0.0,
        -sn * part_a_obj.x() + cs * part_a_obj.z());
    part_a_world += vec3(3, 0, -5);
    ray r1(point3(part_a_world.x(), 0, 1), vec3(0, 0, -1.5), 0.0);
    hit_record rec1;
    TASK_CHECK(snapshot.world().hit(r1, interval(0.001, infinity), rec1));
    TASK_CHECK(rec1.instance_id == 1);
    TASK_CHECK(rec1.mat == matte);

    // Replacing an instance transform keeps the same BLAS prototype alive and
    // shared (its node identity is unchanged); only the wrapper/BVH path differs.
    scene_snapshot moved = snapshot.updated(
        {}, {{1, scene_instance_desc{0, vec3(-4, 0, -5), 45.0, true}}});
    point3 moved_target = part_a_world + vec3(-4 - 3, 0, 0);
    ray r1b(point3(moved_target.x(), 0, 1), vec3(0, 0, -1.5), 0.0);
    hit_record rec1b;
    TASK_CHECK(moved.world().hit(r1b, interval(0.001, infinity), rec1b));
    TASK_CHECK(rec1b.instance_id == 1);
    TASK_CHECK(blas.get() == blas_id); // BLAS resource still the same object
    TASK_CHECK(blas.use_count() >= 2); // retained by both snapshots
    return 0;
}


// =====================================================================================
// lighting
// =====================================================================================

int lighting_transform_forward_and_mixture() {
    auto recorder = make_shared<recording_light>(0.25);
    std::vector<scene_prototype> protos = { scene_prototype{recorder, true} };

    // Two lights from the same prototype must remain two independently selected
    // lights, never merged into one.
    std::vector<scene_instance_desc> inst = {
        scene_instance_desc{0, vec3(2, 0, -3), 0.0, true},
        scene_instance_desc{0, vec3(-4, 1, -2), 0.0, true},
    };
    scene_snapshot lights_only(protos, inst);
    TASK_CHECK(lights_only.light_count() == 2);

    // Uniform mixture of two equal canned values.
    double mixed = lights_only.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1));
    TASK_CHECK(near(mixed, 0.25));
    TASK_CHECK(recorder->pdf_calls == 2);

    // Over many fixed-seed selections both instance transforms are reached,
    // proving per-instance uniform selection rather than a merged single light.
    std::srand(2024);
    std::set<std::string> origins;
    for (int i = 0; i < 300; i++) {
        recorder->random_calls = 0;
        vec3 dir = lights_only.lights().random(point3(0, 0, 0));
        TASK_CHECK(recorder->random_calls == 1);
        TASK_CHECK(near_vec(dir, vec3(0, 0, -1)));
        std::ostringstream key;
        key << recorder->last_origin.x() << "," << recorder->last_origin.z();
        origins.insert(key.str());
    }
    TASK_CHECK(origins.size() == 2);

    // Translation forwarding on a single light: prototype sees origin - translation.
    scene_snapshot translated(
        protos, { scene_instance_desc{0, vec3(2, 0, -3), 0.0, true} });
    point3 origin(1, 2, 3), expected(1 - 2, 2 - 0, 3 - (-3));
    translated.lights().pdf_value(origin, vec3(0, 0, -1));
    TASK_CHECK(near_vec(recorder->last_origin, expected));

    // Rotation forwarding with a single rotated instance.
    scene_snapshot rotated(
        protos, { scene_instance_desc{0, vec3(0, 0, 0), 90.0, true} });
    point3 probe(0.7, -0.2, 1.3);
    rotated.lights().pdf_value(probe, vec3(0.3, 0.1, -0.9));
    TASK_CHECK(near_vec(recorder->last_origin, rotate_to_object(probe, 90.0)));
    TASK_CHECK(near_vec(recorder->last_direction,
                        rotate_to_object(vec3(0.3, 0.1, -0.9), 90.0)));

    // Combined rotate-then-translate forwarding on a single light.
    scene_snapshot combined(
        protos, { scene_instance_desc{0, vec3(1.5, -2.0, 0.5), 90.0, true} });
    combined.lights().pdf_value(probe, vec3(0.3, 0.1, -0.9));
    TASK_CHECK(near_vec(recorder->last_origin,
                        rotate_to_object(probe - vec3(1.5, -2.0, 0.5), 90.0)));
    return 0;
}


int lighting_deletion_and_versioned_pdf() {
    auto light_a = make_shared<recording_light>(0.2);
    auto light_b = make_shared<recording_light>(0.4);
    std::vector<scene_prototype> protos = {
        scene_prototype{light_a, true},
        scene_prototype{light_b, true},
    };
    std::vector<scene_instance_desc> inst = {
        scene_instance_desc{0, vec3(1, 0, -3), 0.0, true},
        scene_instance_desc{1, vec3(-1, 0, -3), 0.0, true},
    };
    scene_snapshot parent(protos, inst);
    TASK_CHECK(parent.light_count() == 2);
    TASK_CHECK(near(parent.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1)), 0.3));

    // Delete one light: the mixture collapses to the survivor's value.
    scene_snapshot child = parent.updated(
        {}, {{0, scene_instance_desc{0, vec3(1, 0, -3), 0.0, false}}});
    TASK_CHECK(child.light_count() == 1);
    TASK_CHECK(near(child.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1)), 0.4));

    // Delete the other too: empty set, pdf 0, sampling never reaches a prototype.
    scene_snapshot dark = child.updated(
        {}, {{1, scene_instance_desc{1, vec3(-1, 0, -3), 0.0, false}}});
    TASK_CHECK(dark.light_count() == 0);
    TASK_CHECK(dark.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1)) == 0.0);
    int before_a = light_a->random_calls, before_b = light_b->random_calls;
    (void)dark.lights().random(point3(0, 0, 0));
    TASK_CHECK(light_a->random_calls == before_a);
    TASK_CHECK(light_b->random_calls == before_b);

    // Parent's light set is unchanged by the forks.
    TASK_CHECK(parent.light_count() == 2);
    TASK_CHECK(near(parent.lights().pdf_value(point3(0, 0, 0), vec3(0, 0, -1)), 0.3));
    return 0;
}


int lighting_camera_pixel_match() {
    auto ground_mat = make_shared<lambertian>(color(0.5, 0.6, 0.5));
    auto block_mat = make_shared<lambertian>(color(0.9, 0.3, 0.2));
    auto glow_mat = make_shared<diffuse_light>(color(6, 5, 4));

    auto unit_ball = make_shared<sphere>(point3(0, 0, 0), 0.8, block_mat);
    auto ground = make_shared<quad>(point3(-8, -1.2, -8), vec3(16, 0, 0),
                                    vec3(0, 0, 16), ground_mat);
    auto lamp = make_shared<quad>(point3(-1.5, 3, -4.5), vec3(3, 0, 0),
                                  vec3(0, 0, 2), glow_mat);
    // The light is translated (no rotation). The flat reference places an
    // equivalent raw quad directly at the shifted position, so its world-space
    // pdf_value/random are exactly those of the snapshot's translation-forwarding
    // (a translated quad's sampling reduces to the shifted quad's sampling).
    const vec3 light_translation(0.5, 0.0, 0.5);
    auto flat_lamp = make_shared<quad>(point3(-1.5, 3, -4.5) + light_translation,
                                       vec3(3, 0, 0), vec3(0, 0, 2), glow_mat);

    std::vector<scene_prototype> protos = {
        scene_prototype{unit_ball, false},
        scene_prototype{ground, false},
        scene_prototype{lamp, true},
    };
    std::vector<scene_instance_desc> inst = {
        scene_instance_desc{0, vec3(0.8, 0, -3.5), 25.0, true},  // occluder, rotated
        scene_instance_desc{1, vec3(0, 0, 0), 0.0, true},        // diffuse receiver
        scene_instance_desc{2, light_translation, 0.0, true},    // translated area light
    };
    scene_snapshot snapshot(protos, inst);

    auto configure = [] {
        camera cam;
        cam.aspect_ratio = 1.0;
        cam.image_width = 16;
        cam.samples_per_pixel = 4;
        cam.max_depth = 4;
        cam.background = color(0.05, 0.05, 0.08);
        cam.vfov = 50;
        cam.lookfrom = point3(0, 1.5, 2.5);
        cam.lookat = point3(0, 0, -3);
        return cam;
    };

    std::srand(98765);
    auto cam_snap = configure();
    std::string snapshot_ppm = render_to_string(cam_snap, snapshot);

    // Independent flat reference. The occluder and receiver use the legacy rigid
    // wrappers (non-emissive, so only their identical hit geometry matters); the
    // light is the equivalent shifted raw quad with correct world-space sampling.
    hittable_list flat_world;
    auto flat_ball = flat_wrap(unit_ball, inst[0].translation, inst[0].yaw_degrees);
    auto flat_ground = flat_wrap(ground, inst[1].translation, inst[1].yaw_degrees);
    flat_world.add(flat_ball);
    flat_world.add(flat_ground);
    flat_world.add(flat_lamp);
    hittable_list flat_lights(flat_lamp);

    std::srand(98765);
    auto cam_flat = configure();
    std::string flat_ppm = render_to_string(cam_flat, flat_world, flat_lights);

    TASK_CHECK(count_ppm_pixels(snapshot_ppm) == 256);
    TASK_CHECK(count_ppm_pixels(flat_ppm) == 256);
    // Same seed, equivalent geometry and sampling, identical RNG consumption.
    TASK_CHECK(snapshot_ppm == flat_ppm);

    // Zero-light render uses the material PDF only, never samples an empty light,
    // and still produces a complete image.
    auto probe = make_shared<recording_light>(1.0);
    scene_snapshot no_light(
        { scene_prototype{unit_ball, false},
          scene_prototype{ground, false},
          scene_prototype{probe, true} },
        { scene_instance_desc{0, vec3(0.8, 0, -3.5), 25.0, true},
          scene_instance_desc{1, vec3(0, 0, 0), 0.0, true},
          scene_instance_desc{2, vec3(0, 0, 0), 0.0, false} });
    TASK_CHECK(no_light.light_count() == 0);

    std::srand(13579);
    auto cam_dark = configure();
    std::string dark_ppm = render_to_string(cam_dark, no_light);
    TASK_CHECK(count_ppm_pixels(dark_ppm) == 256);
    TASK_CHECK(probe->random_calls == 0);
    TASK_CHECK(probe->pdf_calls == 0);
    return 0;
}


} // namespace


const struct task_case task_cases[] = {
    { "persistence", "flat_equivalence",          persistence_flat_equivalence },
    { "persistence", "fork_rollback_lifetime",    persistence_fork_rollback_lifetime },
    { "persistence", "empty_and_identity_reset",  persistence_empty_and_identity_reset },
    { "sharing",     "single_leaf_path_budget",   sharing_single_leaf_budget },
    { "sharing",     "batch_ancestors_cloned_once", sharing_batch_ancestors_cloned_once },
    { "sharing",     "bvh_prototype_sharing",     sharing_bvh_prototype },
    { "lighting",    "transform_forward_mixture", lighting_transform_forward_and_mixture },
    { "lighting",    "deletion_and_versioned_pdf", lighting_deletion_and_versioned_pdf },
    { "lighting",    "camera_pixel_match",        lighting_camera_pixel_match },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
