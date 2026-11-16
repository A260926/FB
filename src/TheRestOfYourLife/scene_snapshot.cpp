#include "scene_snapshot.h"

#include "bvh.h"
#include "hittable_list.h"

#include <cmath>
#include <stdexcept>
#include <unordered_set>


namespace {


// A hittable that can never intersect anything. Used for an empty world and as
// the empty light set; its pdf_value is 0 and its random() is only a harmless
// fallback because the camera never samples a zero-light set.
class never_hittable : public hittable {
  public:
    bool hit(const ray&, interval, hit_record&) const override { return false; }
    aabb bounding_box() const override { return aabb::empty; }
    double pdf_value(const point3&, const vec3&) const override { return 0.0; }
    vec3 random(const point3&) const override { return vec3(1, 0, 0); }
};


// Translation that also forwards area-light sampling into object space.
class rigid_translate : public hittable {
  public:
    rigid_translate(shared_ptr<hittable> object, const vec3& offset)
      : object(object), offset(offset)
    {
        bbox = object->bounding_box() + offset;
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        ray offset_r(r.origin() - offset, r.direction(), r.time());
        if (!object->hit(offset_r, ray_t, rec))
            return false;
        rec.p += offset;
        return true;
    }

    aabb bounding_box() const override { return bbox; }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        return object->pdf_value(origin - offset, direction);
    }

    vec3 random(const point3& origin) const override {
        // Sampled object-space direction already accounts for the offset:
        // p' - (origin - offset) == (p' + offset) - origin.
        return object->random(origin - offset);
    }

  private:
    shared_ptr<hittable> object;
    vec3 offset;
    aabb bbox;
};


// Rotation about Y that also forwards area-light sampling. Ray/result geometry
// matches the legacy rotate_y; the sampling hooks rotate points and directions
// into object space and rotate sampled directions back.
class rigid_rotate_y : public hittable {
  public:
    rigid_rotate_y(shared_ptr<hittable> object, double angle)
      : object(object)
    {
        auto radians = degrees_to_radians(angle);
        sin_theta = std::sin(radians);
        cos_theta = std::cos(radians);
        bbox = object->bounding_box();

        point3 mn( infinity,  infinity,  infinity);
        point3 mx(-infinity, -infinity, -infinity);

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int k = 0; k < 2; k++) {
                    auto x = i*bbox.x.max + (1-i)*bbox.x.min;
                    auto y = j*bbox.y.max + (1-j)*bbox.y.min;
                    auto z = k*bbox.z.max + (1-k)*bbox.z.min;

                    auto tester = vec3(
                        cos_theta*x + sin_theta*z,
                        y,
                        -sin_theta*x + cos_theta*z
                    );

                    for (int c = 0; c < 3; c++) {
                        mn[c] = std::fmin(mn[c], tester[c]);
                        mx[c] = std::fmax(mx[c], tester[c]);
                    }
                }
            }
        }

        bbox = aabb(mn, mx);
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        auto origin = point3(
            (cos_theta * r.origin().x()) - (sin_theta * r.origin().z()),
            r.origin().y(),
            (sin_theta * r.origin().x()) + (cos_theta * r.origin().z())
        );
        auto direction = vec3(
            (cos_theta * r.direction().x()) - (sin_theta * r.direction().z()),
            r.direction().y(),
            (sin_theta * r.direction().x()) + (cos_theta * r.direction().z())
        );

        ray rotated_r(origin, direction, r.time());
        if (!object->hit(rotated_r, ray_t, rec))
            return false;

        rec.p = point3(
            (cos_theta * rec.p.x()) + (sin_theta * rec.p.z()),
            rec.p.y(),
            (-sin_theta * rec.p.x()) + (cos_theta * rec.p.z())
        );
        rec.normal = vec3(
            (cos_theta * rec.normal.x()) + (sin_theta * rec.normal.z()),
            rec.normal.y(),
            (-sin_theta * rec.normal.x()) + (cos_theta * rec.normal.z())
        );
        return true;
    }

    aabb bounding_box() const override { return bbox; }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        return object->pdf_value(to_object(origin), to_object(direction));
    }

    vec3 random(const point3& origin) const override {
        vec3 object_dir = object->random(to_object(origin));
        return to_world(object_dir);
    }

  private:
    shared_ptr<hittable> object;
    double sin_theta;
    double cos_theta;
    aabb bbox;

    point3 to_object(const vec3& v) const {
        return point3(
            (cos_theta * v.x()) - (sin_theta * v.z()),
            v.y(),
            (sin_theta * v.x()) + (cos_theta * v.z())
        );
    }

    point3 to_world(const vec3& v) const {
        return point3(
            (cos_theta * v.x()) + (sin_theta * v.z()),
            v.y(),
            (-sin_theta * v.x()) + (cos_theta * v.z())
        );
    }
};


// BVH leaf that tags every successful hit with its stable instance slot. It
// forwards light sampling to the rigid-transform chain below it.
class instance_leaf : public hittable {
  public:
    instance_leaf(size_t instance_id, shared_ptr<hittable> child)
      : instance_id(instance_id), child(child), bbox(child->bounding_box()) {}

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        // Do not touch rec on a miss: leaf BVH nodes invoke this object twice
        // (left == right), and a boundary miss must not erase an earlier hit.
        // The wrapped primitive already resets instance_id on its own successful
        // hit, so here we only stamp the stable slot once a hit is confirmed.
        if (!child->hit(r, ray_t, rec))
            return false;
        rec.instance_id = instance_id;
        return true;
    }

    aabb bounding_box() const override { return bbox; }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        return child->pdf_value(origin, direction);
    }

    vec3 random(const point3& origin) const override {
        return child->random(origin);
    }

  private:
    size_t instance_id;
    shared_ptr<hittable> child;
    aabb bbox;
};


// A retained but disabled leaf: its slot and node stay in the tree (with valid
// bounds), yet it neither intersects nor is sampled as a light.
class disabled_leaf : public hittable {
  public:
    explicit disabled_leaf(shared_ptr<hittable> geometry)
      : geometry(geometry), bbox(geometry->bounding_box()) {}

    bool hit(const ray&, interval, hit_record&) const override { return false; }
    aabb bounding_box() const override { return bbox; }

  private:
    shared_ptr<hittable> geometry;
    aabb bbox;
};


bool finite_vec(const vec3& v) {
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}


// Build the per-instance rigid-transform chain around the shared prototype.
shared_ptr<hittable> build_geometry(const scene_prototype& prototype,
                                    const scene_instance_desc& desc) {
    shared_ptr<hittable> node = prototype.object;
    if (desc.yaw_degrees != 0.0)
        node = make_shared<rigid_rotate_y>(node, desc.yaw_degrees);
    node = make_shared<rigid_translate>(node, desc.translation);
    return node;
}


} // namespace


scene_snapshot::scene_snapshot(const std::vector<scene_prototype>& prototype_input,
                               const std::vector<scene_instance_desc>& instance_input)
  : prototypes(prototype_input),
    world_empty(make_shared<never_hittable>()),
    light_set(make_shared<never_hittable>())
{
    for (const auto& proto : prototypes) {
        if (!proto.object)
            throw std::invalid_argument("scene_snapshot: prototype has a null object");
    }

    instances.reserve(instance_input.size());
    for (const auto& desc : instance_input) {
        if (desc.prototype >= prototypes.size())
            throw std::invalid_argument("scene_snapshot: instance references missing prototype");
        if (!finite_vec(desc.translation) || !std::isfinite(desc.yaw_degrees))
            throw std::invalid_argument("scene_snapshot: instance transform is not finite");

        instance_state state;
        state.prototype = desc.prototype;
        state.translation = desc.translation;
        state.yaw_degrees = desc.yaw_degrees;
        state.enabled = desc.enabled;
        instances.push_back(state);
    }

    build_derived();
}


const hittable& scene_snapshot::world() const {
    return world_root ? *world_root : *world_empty;
}


const hittable& scene_snapshot::lights() const {
    return *light_set;
}


size_t scene_snapshot::light_count() const { return num_lights; }
size_t scene_snapshot::prototype_count() const { return prototypes.size(); }
size_t scene_snapshot::instance_count() const { return instances.size(); }


scene_snapshot scene_snapshot::updated(
    const std::vector<std::pair<size_t, scene_prototype>>& prototype_updates,
    const std::vector<std::pair<size_t, scene_instance_desc>>& instance_updates
) const {
    // ---- Validate everything before touching any existing state. ----
    std::unordered_set<size_t> seen_proto;
    for (const auto& update : prototype_updates) {
        if (update.first >= prototypes.size())
            throw std::invalid_argument("scene_snapshot::updated: prototype slot out of range");
        if (!seen_proto.insert(update.first).second)
            throw std::invalid_argument("scene_snapshot::updated: duplicate prototype slot");
        if (!update.second.object)
            throw std::invalid_argument("scene_snapshot::updated: prototype has a null object");
    }

    std::unordered_set<size_t> seen_instance;
    for (const auto& update : instance_updates) {
        if (update.first >= instances.size())
            throw std::invalid_argument("scene_snapshot::updated: instance slot out of range");
        if (!seen_instance.insert(update.first).second)
            throw std::invalid_argument("scene_snapshot::updated: duplicate instance slot");
        const auto& desc = update.second;
        if (desc.prototype >= prototypes.size())
            throw std::invalid_argument("scene_snapshot::updated: instance references missing prototype");
        if (!finite_vec(desc.translation) || !std::isfinite(desc.yaw_degrees))
            throw std::invalid_argument("scene_snapshot::updated: instance transform is not finite");
    }

    // ---- Build the new description copies; the parent stays untouched. ----
    scene_snapshot next;
    next.prototypes = prototypes;
    next.world_empty = world_empty;
    for (const auto& update : prototype_updates)
        next.prototypes[update.first] = update.second;

    next.instances.reserve(instances.size());
    for (const auto& state : instances) {
        instance_state copy;
        copy.prototype = state.prototype;
        copy.translation = state.translation;
        copy.yaw_degrees = state.yaw_degrees;
        copy.enabled = state.enabled;
        copy.leaf = state.leaf;  // shared until proven changed
        next.instances.push_back(copy);
    }
    for (const auto& update : instance_updates) {
        auto& state = next.instances[update.first];
        state.prototype = update.second.prototype;
        state.translation = update.second.translation;
        state.yaw_degrees = update.second.yaw_degrees;
        state.enabled = update.second.enabled;
        state.leaf.reset();
    }

    // A prototype change reaches every instance still referencing that slot.
    std::unordered_set<size_t> changed_slots = seen_instance;
    for (size_t slot = 0; slot < next.instances.size(); slot++) {
        if (seen_proto.count(next.instances[slot].prototype))
            changed_slots.insert(slot);
    }

    // Rebuild only the affected leaves.
    std::vector<std::pair<size_t, shared_ptr<hittable>>> replacements;
    replacements.reserve(changed_slots.size());
    for (size_t slot : changed_slots) {
        auto& state = next.instances[slot];
        scene_instance_desc desc;
        desc.prototype = state.prototype;
        desc.translation = state.translation;
        desc.yaw_degrees = state.yaw_degrees;
        desc.enabled = state.enabled;

        auto geometry = build_geometry(next.prototypes[state.prototype], desc);
        state.leaf = state.enabled
                   ? static_cast<shared_ptr<hittable>>(make_shared<instance_leaf>(slot, geometry))
                   : static_cast<shared_ptr<hittable>>(make_shared<disabled_leaf>(geometry));
        replacements.emplace_back(slot, state.leaf);
    }

    // Persistent BVH update: only replaced leaves and their ancestor paths are
    // newly allocated; an empty change set reuses the very same root.
    next.world_root = world_root
                    ? bvh_node::replace_leaves(world_root, replacements)
                    : nullptr;

    // The light set is per version: one entry per enabled emissive instance.
    auto new_lights = make_shared<hittable_list>();
    for (size_t slot = 0; slot < next.instances.size(); slot++) {
        const auto& state = next.instances[slot];
        if (state.enabled && next.prototypes[state.prototype].emissive)
            new_lights->add(state.leaf);
    }
    next.light_set = new_lights->objects.empty()
                   ? static_cast<shared_ptr<hittable>>(make_shared<never_hittable>())
                   : static_cast<shared_ptr<hittable>>(new_lights);
    next.num_lights = new_lights->objects.size();

    return next;
}


void scene_snapshot::build_derived() {
    std::vector<shared_ptr<hittable>> leaves;
    leaves.reserve(instances.size());

    auto new_lights = make_shared<hittable_list>();

    for (size_t slot = 0; slot < instances.size(); slot++) {
        auto& state = instances[slot];
        scene_instance_desc desc;
        desc.prototype = state.prototype;
        desc.translation = state.translation;
        desc.yaw_degrees = state.yaw_degrees;
        desc.enabled = state.enabled;

        auto geometry = build_geometry(prototypes[state.prototype], desc);
        state.leaf = state.enabled
                   ? static_cast<shared_ptr<hittable>>(make_shared<instance_leaf>(slot, geometry))
                   : static_cast<shared_ptr<hittable>>(make_shared<disabled_leaf>(geometry));
        leaves.push_back(state.leaf);

        if (state.enabled && prototypes[state.prototype].emissive)
            new_lights->add(state.leaf);
    }

    world_root = leaves.empty() ? nullptr : bvh_node::build_slotted(leaves);

    light_set = new_lights->objects.empty()
              ? static_cast<shared_ptr<hittable>>(make_shared<never_hittable>())
              : static_cast<shared_ptr<hittable>>(new_lights);
    num_lights = new_lights->objects.size();
}
