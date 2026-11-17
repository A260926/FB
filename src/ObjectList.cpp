#include "object/ObjectList.h"
#include "bvh/Node.h"
#include "object/Sphere.h"
#include "object/Triangle.h"
#include "object/Cylinder.h"

ObjectList::ObjectList() {}

ObjectList::ObjectList(std::shared_ptr<Object> object) {
    add(object);
}

void ObjectList::clear() {
    objects.clear();
}

void ObjectList::add(std::shared_ptr<Object> object) {
    objects.push_back(object);
}

void ObjectList::build() {
    // Build the BVH
    std::vector<std::shared_ptr<Object>> bvh_objects;

    for (const auto& obj : objects) {
        if (obj->exclude_from_bvh()) {
            excluded_objects.push_back(obj);
        } else {
            bvh_objects.push_back(obj);
        }
    }
    if (!bvh_objects.empty()) {
        root = std::make_shared<Node>(bvh_objects, 0, bvh_objects.size());
    }
}

bool ObjectList::intersect_bvh_enabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
    // Check if the ray intersects the BVH
    IntersectionRecord temp_rec;
    bool intersected = false;
    auto nearest_t = ray_t.max;

    // Test BVH
    if (root && root->intersect(r, ray_t, temp_rec)) {
        intersected = true;
        nearest_t = temp_rec.t;
        rec = temp_rec;
    }

    // Test excluded objects
    for (const auto& object : excluded_objects) {
        if (object->intersect(r, Interval(ray_t.min, nearest_t), temp_rec)) {
            intersected = true;
            nearest_t = temp_rec.t;
            rec = temp_rec;
        }
    }

    return intersected;
}

// NOT USING BVH
bool ObjectList::intersect_bvh_disabled(const Ray& r, Interval ray_t, IntersectionRecord& rec) const {
    // Check if the ray intersects any object in the list
    IntersectionRecord temp_rec;
    bool intersected = false;
    auto nearest_t = ray_t.max;

    for (const auto& object : objects) {
        if (object->intersect(r, Interval(ray_t.min, nearest_t), temp_rec)) {
            intersected = true;
            nearest_t = temp_rec.t;
            rec = temp_rec;
        }
    }

    return intersected;
}


BoundingBox ObjectList::bounding_box() const {
    return root ? root->bounding_box() : BoundingBox();
}

void ObjectList::log_hit_counts() const {
    for (const auto& object : objects) {
        // Check if the object is a sphere
        if (auto sphere_ptr = std::dynamic_pointer_cast<Sphere>(object)) {
            std::cout << "Sphere hit_call_count: " << sphere_ptr->hit_call_count << std::endl;
        }
        // Check if the object is a triangle
        else if (auto triangle_ptr = std::dynamic_pointer_cast<Triangle>(object)) {
            std::cout << "Triangle hit_call_count: " << triangle_ptr->hit_call_count << std::endl;
        }
        // Check if the object is a cylinder
        else if (auto cylinder_ptr = std::dynamic_pointer_cast<Cylinder>(object)) {
            std::cout << "Cylinder hit_call_count: " << cylinder_ptr->hit_call_count << std::endl;
        }
        // Handle other hittable objects
        else {
            std::cout << "Unknown object type, skipping." << std::endl;
        }
    }
}