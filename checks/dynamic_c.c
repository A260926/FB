#include <bvh/v2/c_api/bvh.h>

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct triangle {
    struct bvh_vec3f a, b, c;
};

struct scene {
    struct bvh3f* tree;
    struct triangle* tris;
    size_t* ids;
    size_t count;
};

struct ray_hit {
    size_t id;
    float t;
};

static struct bvh_bbox3f bbox_of(struct triangle tri) {
    struct bvh_bbox3f box;
    box.min.x = fminf(tri.a.x, fminf(tri.b.x, tri.c.x));
    box.min.y = fminf(tri.a.y, fminf(tri.b.y, tri.c.y));
    box.min.z = fminf(tri.a.z, fminf(tri.b.z, tri.c.z));
    box.max.x = fmaxf(tri.a.x, fmaxf(tri.b.x, tri.c.x));
    box.max.y = fmaxf(tri.a.y, fmaxf(tri.b.y, tri.c.y));
    box.max.z = fmaxf(tri.a.z, fmaxf(tri.b.z, tri.c.z));
    return box;
}

static struct bvh_vec3f center_of(struct bvh_bbox3f box) {
    struct bvh_vec3f c = {
        (box.min.x + box.max.x) * 0.5f,
        (box.min.y + box.max.y) * 0.5f,
        (box.min.z + box.max.z) * 0.5f,
    };
    return c;
}

static struct bvh_vec3f sub(struct bvh_vec3f a, struct bvh_vec3f b) {
    struct bvh_vec3f r = { a.x - b.x, a.y - b.y, a.z - b.z };
    return r;
}

static struct bvh_vec3f cross(struct bvh_vec3f a, struct bvh_vec3f b) {
    struct bvh_vec3f r = {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
    return r;
}

static float dot(struct bvh_vec3f a, struct bvh_vec3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* Returns true and writes the intersection parameter when the ray hits the triangle. */
static bool intersect_triangle(const struct triangle* tri, const struct bvh_ray3f* ray, float* t) {
    struct bvh_vec3f e1 = sub(tri->b, tri->a);
    struct bvh_vec3f e2 = sub(tri->c, tri->a);
    struct bvh_vec3f p = cross(ray->dir, e2);
    float det = dot(e1, p);
    if (fabsf(det) < 1e-9f)
        return false;
    float inv_det = 1.f / det;
    struct bvh_vec3f s = sub(ray->org, tri->a);
    float u = dot(s, p) * inv_det;
    if (u < 0.f || u > 1.f)
        return false;
    struct bvh_vec3f q = cross(s, e1);
    float v = dot(ray->dir, q) * inv_det;
    if (v < 0.f || u + v > 1.f)
        return false;
    float dist = dot(e2, q) * inv_det;
    if (dist < ray->tmin || dist > ray->tmax)
        return false;
    *t = dist;
    return true;
}

/* Finds the triangle data for an external ID. */
static const struct triangle* find_triangle(const struct scene* scene, size_t id) {
    for (size_t i = 0; i < scene->count; ++i) {
        if (scene->ids[i] == id)
            return &scene->tris[i];
    }
    return NULL;
}

struct trace_state {
    struct scene* scene;
    struct bvh_ray3f* ray;
    struct ray_hit* hit;
};

static bool trace_user_fn(void* user_data, float* tmax, size_t begin, size_t end) {
    struct trace_state* state = (struct trace_state*)user_data;
    bool was_hit = false;
    for (size_t i = begin; i < end; ++i) {
        size_t id = bvh3f_get_prim_id(state->scene->tree, i);
        const struct triangle* tri = find_triangle(state->scene, id);
        if (!tri)
            return false;
        float t;
        if (intersect_triangle(tri, state->ray, &t) && t <= *tmax) {
            *tmax = t;
            state->hit->id = id;
            state->hit->t = t;
            was_hit = true;
        }
    }
    return was_hit;
}

/* Runs a closest-hit query through the C API. */
static void query(struct scene* scene, struct bvh_ray3f ray, struct ray_hit* hit) {
    struct trace_state state = { scene, &ray, hit };
    struct bvh_intersect_callbackf callback = { &state, trace_user_fn };
    hit->id = SIZE_MAX;
    hit->t = ray.tmax;
    bvh3f_intersect_ray_robust(scene->tree, &ray, &callback);
}

/* Brute-force oracle over the scene's live triangles. */
static void brute_force(const struct scene* scene, struct bvh_ray3f ray, struct ray_hit* hit) {
    hit->id = SIZE_MAX;
    hit->t = ray.tmax;
    for (size_t i = 0; i < scene->count; ++i) {
        float t;
        if (intersect_triangle(&scene->tris[i], &ray, &t) && t <= hit->t) {
            hit->t = t;
            hit->id = scene->ids[i];
        }
    }
}

static void make_box(struct bvh_bbox3f* box, float x, float y, float z, float half) {
    box->min.x = x - half; box->min.y = y - half; box->min.z = z - half;
    box->max.x = x + half; box->max.y = y + half; box->max.z = z + half;
}

static bool finite_ordered(struct bvh_bbox3f box) {
    return isfinite(box.min.x) && isfinite(box.min.y) && isfinite(box.min.z) &&
           isfinite(box.max.x) && isfinite(box.max.y) && isfinite(box.max.z) &&
           box.min.x <= box.max.x && box.min.y <= box.max.y && box.min.z <= box.max.z;
}

/* Validates that the tree is reachable, its leaves cover each ID once, and the boxes are unions. */
static int validate(struct scene* scene) {
    size_t node_count = bvh3f_get_node_count(scene->tree);
    size_t prim_count = bvh3f_get_prim_count(scene->tree);
    if (node_count == 0) {
        if (prim_count != 0 || scene->count != 0)
            return 0;
        return 1;
    }
    unsigned char* reached = (unsigned char*)calloc(node_count, 1);
    size_t* stack = (size_t*)malloc(sizeof(size_t) * node_count);
    size_t stack_size = 0;
    size_t visited = 0;
    stack[stack_size++] = 0;
    while (stack_size) {
        size_t id = stack[--stack_size];
        if (id >= node_count || reached[id]) {
            free(stack); free(reached); return 0;
        }
        reached[id] = 1;
        ++visited;
        struct bvh_node3f* node = bvh3f_get_node(scene->tree, id);
        if (!bvh_node3f_is_leaf(node)) {
            size_t left = bvh_node3f_get_first_id(node);
            if (left + 1 >= node_count) { free(stack); free(reached); return 0; }
            stack[stack_size++] = left + 0;
            stack[stack_size++] = left + 1;
        }
    }
    free(stack);
    free(reached);
    if (visited != node_count)
        return 0;

    for (size_t i = 0; i < scene->count; ++i) {
        if (!finite_ordered(bbox_of(scene->tris[i])))
            return 0;
    }

    /* Leaves reference slots in the internal primitive storage, which may contain free holes
       in editable mode; only the number of distinct external IDs must match the live set. */
    size_t slot_limit = 0;
    for (size_t i = 0; i < node_count; ++i) {
        struct bvh_node3f* node = bvh3f_get_node(scene->tree, i);
        if (bvh_node3f_is_leaf(node)) {
            size_t end_slot = bvh_node3f_get_first_id(node) + bvh_node3f_get_prim_count(node);
            if (end_slot > slot_limit)
                slot_limit = end_slot;
        }
    }

    unsigned char* seen_slots = (unsigned char*)calloc(slot_limit ? slot_limit : 1, 1);
    unsigned char* seen_ids = (unsigned char*)calloc(scene->count ? scene->count : 1, 1);
    if (!seen_slots || !seen_ids) {
        free(seen_slots);
        free(seen_ids);
        return 0;
    }

    size_t seen_count = 0;
    int ok = 1;
    for (size_t i = 0; i < node_count && ok; ++i) {
        struct bvh_node3f* node = bvh3f_get_node(scene->tree, i);
        if (!bvh_node3f_is_leaf(node))
            continue;
        size_t first = bvh_node3f_get_first_id(node);
        size_t count = bvh_node3f_get_prim_count(node);
        if (count == 0 || first > slot_limit || first + count > slot_limit) {
            ok = 0;
            break;
        }
        for (size_t j = first; j < first + count; ++j) {
            if (seen_slots[j]) {
                ok = 0;
                break;
            }
            seen_slots[j] = 1;
            size_t id = bvh3f_get_prim_id(scene->tree, j);
            int found = 0;
            for (size_t k = 0; k < scene->count; ++k) {
                if (scene->ids[k] == id) {
                    if (seen_ids[k]) {
                        ok = 0;
                        break;
                    }
                    seen_ids[k] = 1;
                    found = 1;
                    ++seen_count;
                    break;
                }
            }
            if (!found)
                ok = 0;
        }
    }
    free(seen_slots);
    free(seen_ids);
    if (!ok)
        return 0;
    return seen_count == scene->count;
}

//// Test cases ------------------------------------------------------------------------------------

/* Building, enabling, inserting an occluder, and querying the nearest surface. */
static int case_build_enable_insert(void) {
    struct triangle tris[2] = {
        { { -1, -1, 4 }, { 1, -1, 4 }, { 0, 1, 4 } },
        { { -3, -1, 7 }, { -1, -1, 7 }, { -2, 1, 7 } },
    };
    size_t ids[2] = { 0, 1 };
    struct bvh_bbox3f boxes[2];
    struct bvh_vec3f centers[2];
    for (size_t i = 0; i < 2; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 2, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 2));

    struct scene scene = { tree, tris, ids, 2 };
    TASK_CHECK(validate(&scene));

    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);

    struct triangle occluder = { { -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 } };
    struct bvh_bbox3f occluder_box = bbox_of(occluder);
    TASK_CHECK(bvh3f_insert_primitive(tree, 17, &occluder_box));

    /* Grow the scene's data by appending the occluder. */
    struct triangle grown[3];
    size_t grown_ids[3];
    memcpy(grown, tris, sizeof(tris));
    memcpy(grown_ids, ids, sizeof(ids));
    grown[2] = occluder;
    grown_ids[2] = 17;
    scene.tris = grown;
    scene.ids = grown_ids;
    scene.count = 3;

    TASK_CHECK(validate(&scene));
    struct ray_hit expected;
    brute_force(&scene, ray, &expected);
    query(&scene, ray, &hit);
    TASK_CHECK(expected.id == 17);
    TASK_CHECK(hit.id == expected.id);
    TASK_CHECK(fabsf(hit.t - expected.t) < 1e-5f);

    struct bvh_edit_stats stats = bvh3f_get_last_edit_stats(tree);
    TASK_CHECK(stats.node_visits > 0);
    TASK_CHECK(stats.primitive_visits > 0);
    TASK_CHECK(bvh3f_get_prim_count(tree) == 3);
    bvh3f_destroy(tree);
    return 0;
}

/* Erasing the nearest primitive, then reinserting it, and reusing its ID. */
static int case_erase_and_reinsert(void) {
    struct triangle tris[3] = {
        { { -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 } },
        { { -2, -2, 5 }, { 2, -2, 5 }, { 0, 2, 5 } },
        { { -2, -2, 9 }, { 2, -2, 9 }, { 0, 2, 9 } },
    };
    size_t ids[3] = { 0, 1, 2 };
    (void)ids;
    struct bvh_bbox3f boxes[3];
    struct bvh_vec3f centers[3];
    for (size_t i = 0; i < 3; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 3, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 3));

    struct scene scene = { tree, tris, ids, 3 };
    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);

    TASK_CHECK(bvh3f_erase_primitive(tree, 0));
    TASK_CHECK(!bvh3f_erase_primitive(tree, 0));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 2);
    /* Remove ID 0 from the scene's live set. */
    tris[0] = tris[1];
    ids[0] = ids[1];
    tris[1] = tris[2];
    ids[1] = ids[2];
    scene.count = 2;
    TASK_CHECK(validate(&scene));

    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 1);

    struct triangle back = { { -1, -1, 1 }, { 1, -1, 1 }, { 0, 1, 1 } };
    struct bvh_bbox3f back_box = bbox_of(back);
    TASK_CHECK(bvh3f_insert_primitive(tree, 0, &back_box));
    struct triangle grown[3];
    size_t grown_ids[3];
    memcpy(grown, tris, sizeof(struct triangle) * 2);
    memcpy(grown_ids, ids, sizeof(size_t) * 2);
    grown[2] = back;
    grown_ids[2] = 0;
    scene.tris = grown;
    scene.ids = grown_ids;
    scene.count = 3;
    TASK_CHECK(validate(&scene));
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);
    bvh3f_destroy(tree);
    return 0;
}

/* Sparse IDs and rejected edits keep the contents unchanged. */
static int case_sparse_and_rejects(void) {
    struct triangle tris[2] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 0, 1, 1 } },
        { { -1, -1, 3 }, { 1, -1, 3 }, { 0, 1, 3 } },
    };
    /* Editable mode is enabled with boxes for the original primitive numbers 0..N-1. */
    size_t ids[2] = { 0, 1 };
    struct bvh_bbox3f boxes[2];
    struct bvh_vec3f centers[2];
    for (size_t i = 0; i < 2; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 2, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 2));
    struct scene scene = { tree, tris, ids, 2 };

    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);

    /* After enabling, external IDs can be sparse. */
    struct triangle sparse_near = { { -0.5f, -0.5f, 0.5f }, { 0.5f, -0.5f, 0.5f }, { 0, 0.5f, 0.5f } };
    struct bvh_bbox3f sparse_box = bbox_of(sparse_near);
    TASK_CHECK(bvh3f_insert_primitive(tree, 900, &sparse_box));

    size_t nodes_before = bvh3f_get_node_count(tree);
    size_t prims_before = bvh3f_get_prim_count(tree);
    struct bvh_bbox3f junk;
    make_box(&junk, 0, 0, 2, 0.5f);
    TASK_CHECK(!bvh3f_insert_primitive(tree, 900, &junk));   /* already present */
    TASK_CHECK(!bvh3f_erase_primitive(tree, 1234));          /* absent */
    TASK_CHECK(!bvh3f_insert_primitive(tree, (size_t)1 << 31, &junk)); /* out of range */
    TASK_CHECK(bvh3f_get_node_count(tree) == nodes_before);
    TASK_CHECK(bvh3f_get_prim_count(tree) == prims_before);

    /* Sparse, far-away insertion. */
    struct triangle far = { { 50, 50, 2 }, { 51, 50, 2 }, { 50.5f, 51, 2 } };
    struct bvh_bbox3f far_box = bbox_of(far);
    TASK_CHECK(bvh3f_insert_primitive(tree, 100000, &far_box));
    struct triangle grown[4];
    size_t grown_ids[4];
    memcpy(grown, tris, sizeof(tris));
    memcpy(grown_ids, ids, sizeof(ids));
    grown[2] = sparse_near;
    grown_ids[2] = 900;
    grown[3] = far;
    grown_ids[3] = 100000;
    scene.tris = grown;
    scene.ids = grown_ids;
    scene.count = 4;
    TASK_CHECK(validate(&scene));

    /* Erase and reuse the sparse ID. */
    TASK_CHECK(bvh3f_erase_primitive(tree, 900));
    grown[2] = far;
    grown_ids[2] = 100000;
    scene.count = 3;
    TASK_CHECK(validate(&scene));
    TASK_CHECK(bvh3f_insert_primitive(tree, 900, &sparse_box));
    grown[3] = sparse_near;
    grown_ids[3] = 900;
    scene.count = 4;
    TASK_CHECK(validate(&scene));
    bvh3f_destroy(tree);
    return 0;
}

/* Editing a saved and reloaded tree without re-enabling; handles stay independent. */
static int case_save_load_editable(void) {
    struct triangle tris[3] = {
        { { -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 } },
        { { -1, -1, 4 }, { 1, -1, 4 }, { 0, 1, 4 } },
        { { -1, -1, 6 }, { 1, -1, 6 }, { 0, 1, 6 } },
    };
    /* Editable mode is enabled with boxes for the original primitive numbers 0..N-1. */
    size_t ids[3] = { 0, 1, 2 };
    (void)ids;
    struct bvh_bbox3f boxes[3];
    struct bvh_vec3f centers[3];
    for (size_t i = 0; i < 3; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 3, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 3));

    struct triangle extra = { { -1, -1, 1 }, { 1, -1, 1 }, { 0, 1, 1 } };
    struct bvh_bbox3f extra_box = bbox_of(extra);
    TASK_CHECK(bvh3f_insert_primitive(tree, 21, &extra_box));

    FILE* file = tmpfile();
    TASK_CHECK(file != NULL);
    bvh3f_save(tree, file);
    rewind(file);
    struct bvh3f* restored = bvh3f_load(file);
    TASK_CHECK(restored != NULL);

    struct triangle live[4] = { tris[0], tris[1], tris[2], extra };
    size_t live_ids[4] = { 0, 1, 2, 21 };
    struct scene scene = { restored, live, live_ids, 4 };
    TASK_CHECK(validate(&scene));
    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 21);

    /* Continue editing after load without calling enable_edits again. */
    TASK_CHECK(bvh3f_erase_primitive(restored, 21));
    scene.count = 3;
    TASK_CHECK(validate(&scene));
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);

    /* The saved handle is independent from the restored one. */
    struct scene original = { tree, live, live_ids, 4 };
    query(&original, ray, &hit);
    TASK_CHECK(hit.id == 21);
    TASK_CHECK(bvh3f_get_prim_count(tree) == 4);
    TASK_CHECK(bvh3f_get_prim_count(restored) == 3);

    /* The loaded handle can be saved again and still support edits. */
    FILE* second = tmpfile();
    TASK_CHECK(second != NULL);
    bvh3f_save(restored, second);
    rewind(second);
    struct bvh3f* reloaded = bvh3f_load(second);
    TASK_CHECK(reloaded != NULL);
    TASK_CHECK(bvh3f_insert_primitive(reloaded, 77, &extra_box));
    struct triangle reloaded_live[4] = { tris[0], tris[1], tris[2], extra };
    size_t reloaded_ids[4] = { 0, 1, 2, 77 };
    struct scene reloaded_scene = { reloaded, reloaded_live, reloaded_ids, 4 };
    TASK_CHECK(validate(&reloaded_scene));
    fclose(second);
    fclose(file);

    bvh3f_destroy(reloaded);
    bvh3f_destroy(restored);
    bvh3f_destroy(tree);
    return 0;
}

/* Multi-primitive leaves: with max_leaf_size > 1, a callback receives leaf slot ranges and must
   use bvh3f_get_prim_id to recover external IDs. Insertions grow and split leaves. */
static int case_multi_primitive_leaves(void) {
    struct triangle tris[10];
    struct bvh_bbox3f boxes[6];
    struct bvh_vec3f centers[6];
    size_t ids[10];
    for (size_t i = 0; i < 6; ++i) {
        float z = 2.f + (float)i;
        tris[i] = (struct triangle) { { -1, -1, z }, { 1, -1, z }, { 0, 1, z } };
        ids[i] = i;
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 4, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 6, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 6));
    struct scene scene = { tree, tris, ids, 6 };
    TASK_CHECK(validate(&scene));

    /* A leaf must report a nonzero slot span via the callback. */
    for (size_t i = 0; i < bvh3f_get_node_count(tree); ++i) {
        struct bvh_node3f* node = bvh3f_get_node(tree, i);
        if (bvh3f_get_prim_count(tree) > 0 && bvh_node3f_is_leaf(node))
            TASK_CHECK(bvh_node3f_get_prim_count(node) >= 1);
    }

    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);

    /* Insert into the same leaf several times and check all data stays addressable. */
    for (size_t i = 6; i < 10; ++i) {
        float z = 1.5f + (float)i * 0.1f;
        tris[i] = (struct triangle) { { -1, -1, z }, { 1, -1, z }, { 0, 1, z } };
        ids[i] = i;
        struct bvh_bbox3f box = bbox_of(tris[i]);
        TASK_CHECK(bvh3f_insert_primitive(tree, i, &box));
    }
    scene.count = 10;
    TASK_CHECK(validate(&scene));
    struct ray_hit expected;
    brute_force(&scene, ray, &expected);
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == expected.id);
    TASK_CHECK(fabsf(hit.t - expected.t) < 1e-5f);

    /* Shrink the cluster, then verify the stored primitive count tracks the live set. */
    for (size_t i = 2; i < 10; ++i) {
        TASK_CHECK(bvh3f_erase_primitive(tree, i));
    }
    scene.count = 2;
    TASK_CHECK(validate(&scene));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 2);
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);
    bvh3f_destroy(tree);
    return 0;
}

/* Clears the tree and checks the C intersection entry point on the empty tree. */
static int case_clear_and_empty_query(void) {
    struct triangle tris[2] = {
        { { -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 } },
        { { -1, -1, 4 }, { 1, -1, 4 }, { 0, 1, 4 } },
    };
    size_t ids[2] = { 0, 1 };
    struct bvh_bbox3f boxes[2];
    struct bvh_vec3f centers[2];
    for (size_t i = 0; i < 2; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 2, &config);
    TASK_CHECK(tree != NULL);
    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 2));

    TASK_CHECK(bvh3f_erase_primitive(tree, 0));
    TASK_CHECK(bvh3f_erase_primitive(tree, 1));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 0);
    TASK_CHECK(bvh3f_get_node_count(tree) == 0);

    struct scene scene = { tree, tris, ids, 0 };
    TASK_CHECK(validate(&scene));

    struct bvh_ray3f ray = { { 0, 0, 0 }, { 0, 0, 1 }, 0, 100 };
    struct ray_hit hit;
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == SIZE_MAX);

    struct bvh_bbox3f revived_box;
    struct triangle revived = { { -1, -1, 3 }, { 1, -1, 3 }, { 0, 1, 3 } };
    revived_box = bbox_of(revived);
    TASK_CHECK(bvh3f_insert_primitive(tree, 0, &revived_box));
    struct triangle grown[1] = { revived };
    size_t grown_ids[1] = { 0 };
    scene.tris = grown;
    scene.ids = grown_ids;
    scene.count = 1;
    TASK_CHECK(validate(&scene));
    query(&scene, ray, &hit);
    TASK_CHECK(hit.id == 0);
    bvh3f_destroy(tree);
    return 0;
}

/* Misuse before enabling: all edit entry points fail and leave the tree untouched. */
static int case_not_enabled(void) {
    struct triangle tris[2] = {
        { { -1, -1, 2 }, { 1, -1, 2 }, { 0, 1, 2 } },
        { { -1, -1, 4 }, { 1, -1, 4 }, { 0, 1, 4 } },
    };
    struct bvh_bbox3f boxes[2];
    struct bvh_vec3f centers[2];
    for (size_t i = 0; i < 2; ++i) {
        boxes[i] = bbox_of(tris[i]);
        centers[i] = center_of(boxes[i]);
    }
    struct bvh_build_config config = { BVH_BUILD_QUALITY_LOW, 1, 1, 1024 };
    struct bvh3f* tree = bvh3f_build(NULL, boxes, centers, 2, &config);
    TASK_CHECK(tree != NULL);

    struct bvh_bbox3f box;
    make_box(&box, 0, 0, 3, 0.5f);
    TASK_CHECK(!bvh3f_insert_primitive(tree, 10, &box));
    TASK_CHECK(!bvh3f_erase_primitive(tree, 0));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 2);
    TASK_CHECK(bvh3f_get_node_count(tree) == 3);
    struct bvh_edit_stats stats = bvh3f_get_last_edit_stats(tree);
    TASK_CHECK(stats.node_visits == 0 && stats.primitive_visits == 0);

    TASK_CHECK(bvh3f_enable_edits(tree, boxes, 2));
    TASK_CHECK(!bvh3f_enable_edits(tree, boxes, 2));

    /* An empty tree is obtained by erasing the last primitive; it can be reinserted into. */
    TASK_CHECK(bvh3f_erase_primitive(tree, 0));
    TASK_CHECK(bvh3f_erase_primitive(tree, 1));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 0);
    TASK_CHECK(bvh3f_get_node_count(tree) == 0);
    TASK_CHECK(bvh3f_insert_primitive(tree, 5, &box));
    TASK_CHECK(bvh3f_get_prim_count(tree) == 1);
    bvh3f_destroy(tree);
    return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    { "c_api", "build_enable_insert", case_build_enable_insert },
    { "c_api", "erase_and_reinsert", case_erase_and_reinsert },
    { "c_api", "sparse_and_rejects", case_sparse_and_rejects },
    { "c_api", "save_load_editable", case_save_load_editable },
    { "c_api", "multi_primitive_leaves", case_multi_primitive_leaves },
    { "c_api", "clear_and_empty_query", case_clear_and_empty_query },
    { "c_api", "not_enabled", case_not_enabled },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
