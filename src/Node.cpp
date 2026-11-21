#include "bvh/Node.h"

void Node::random_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end) {
    // RANDOM SPLITS
    // Check if no objects
    if (objects.empty()) return;
    // Check if only one object (leaf node)
    size_t object_span = end - start;
    if (object_span == 1) {
        left = objects[start];
        right = nullptr;
        bbox = objects[start]->bounding_box();
        return;
    }
    // Split objects by their centroids along an axis
    int axis = random_int(0, 2);  // Choose splitting axis
    auto comparator = [axis](const shared_ptr<Object>& a, const shared_ptr<Object>& b) {
        return a->bounding_box().axis_interval(axis).min < b->bounding_box().axis_interval(axis).min;
    };
    std::sort(objects.begin() + start, objects.begin() + end, comparator);
    size_t mid = start + object_span / 2;
    // Recursively create left and right nodes
    left = make_shared<Node>(objects, start, mid);
    right = make_shared<Node>(objects, mid, end);
    // Compute the bounding box of this node
    bbox = BoundingBox(left->bounding_box(), right->bounding_box());
}

void Node::la_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end){
    // LONGEST AXIS SPLITS
    if (objects.empty()) return;
    size_t object_span = end - start;
    if (object_span == 1) {
        left = objects[start];
        right = nullptr;
        bbox = objects[start]->bounding_box();
        return;
    }
    // Calculate the bounding box for all objects in this range
    BoundingBox overall_bbox;
    for (size_t i = start; i < end; i++) {
        overall_bbox = BoundingBox(overall_bbox, objects[i]->bounding_box());
    }
    // Choose the axis with the largest extent
    Vector3 extents = Vector3(overall_bbox.x.size(), overall_bbox.y.size(), overall_bbox.z.size());
    int axis = extents[0] > extents[1] ? (extents[0] > extents[2] ? 0 : 2) : (extents[1] > extents[2] ? 1 : 2);
    auto comparator = [axis](const shared_ptr<Object>& a, const shared_ptr<Object>& b) {
        return a->bounding_box().axis_interval(axis).min < b->bounding_box().axis_interval(axis).min;
    };
    std::sort(objects.begin() + start, objects.begin() + end, comparator);
    size_t mid = start + object_span / 2;
    left = make_shared<Node>(objects, start, mid);
    right = make_shared<Node>(objects, mid, end);
    bbox = BoundingBox(left->bounding_box(), right->bounding_box());
}

void Node::sah_splits(std::vector<shared_ptr<Object>>& objects, size_t start, size_t end){
    // SAH SPLITS
    if (objects.empty()) return;
    size_t object_span = end - start;
    if (object_span == 1) {
        left = objects[start];
        right = nullptr;
        bbox = objects[start]->bounding_box();
        return;
    }
    // Compute overall bounding box
    BoundingBox overall_bbox;
    for (size_t i = start; i < end; i++) {
        overall_bbox = BoundingBox(overall_bbox, objects[i]->bounding_box());
    }
    double best_cost = std::numeric_limits<double>::infinity();
    int best_axis = -1;
    size_t best_split = start;
    // Iterate over all axes
    for (int axis = 0; axis < 3; axis++) {
        // Sort objects along the current axis
        auto comparator = [axis](const shared_ptr<Object>& a, const shared_ptr<Object>& b) {
            return a->bounding_box().axis_interval(axis).min < b->bounding_box().axis_interval(axis).min;
        };
        std::sort(objects.begin() + start, objects.begin() + end, comparator);
        // Precompute bounding boxes for left and right groups
        std::vector<BoundingBox> left_boxes(object_span), right_boxes(object_span);
        BoundingBox left_bbox, right_bbox;
        for (size_t i = start; i < end; i++) {
            left_bbox = BoundingBox(left_bbox, objects[i]->bounding_box());
            left_boxes[i - start] = left_bbox;
        }
        for (size_t i = end - 1; i > start; i--) {
            right_bbox = BoundingBox(right_bbox, objects[i]->bounding_box());
            right_boxes[i - start - 1] = right_bbox;
        }
        // Compute SAH cost for each split point
        for (size_t i = start + 1; i < end; i++) {
            double left_area = left_boxes[i - start - 1].surface_area();
            double right_area = right_boxes[i - start - 1].surface_area();
            size_t left_count = i - start;
            size_t right_count = end - i;
            double parent_area = overall_bbox.surface_area();
            double cost = (left_area * left_count + right_area * right_count) / parent_area;
            if (cost < best_cost) {
                best_cost = cost;
                best_axis = axis;
                best_split = i;
            }
        }
    }
    // Sort objects along the best axis
    auto comparator = [best_axis](const shared_ptr<Object>& a, const shared_ptr<Object>& b) {
        return a->bounding_box().axis_interval(best_axis).min < b->bounding_box().axis_interval(best_axis).min;
    };
    std::sort(objects.begin() + start, objects.begin() + end, comparator);
    // Split at the best position
    left = make_shared<Node>(objects, start, best_split);
    right = make_shared<Node>(objects, best_split, end);
    bbox = BoundingBox(left->bounding_box(), right->bounding_box());
}