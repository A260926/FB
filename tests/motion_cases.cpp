// Regression tests for per-pixel motion vectors.
//
// These tests register cases in three groups (projection, paths,
// lifecycle) using task_support/check.h's TASK_CHECK macro. They
// build an in-memory octree (programmatically, with deliberately
// shared data references in some cases), render it headlessly, and
// compare the produced motion vectors against an independent
// trilinear/projection reference implementation.

#include "discrete_mirage.h"
#include "task_support/check.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

// ===================================================== //
// Synthetic geometry
// ===================================================== //

#define TASK_CASE_BEGIN() do { ensure_lookups(); } while (0)

static void ensure_lookups(void) {
    static bool initialized = false;
    if (!initialized) {
        dmir_lookups_initialize();
        initialized = true;
    }
}

struct TestNode {
    uint8_t mask;
    uint64_t data;
    int level;
    int x, y, z;
};

static std::vector<TestNode> g_nodes;
static int g_tree_depth = 0;

static void tree_node_set(uint64_t id, int level, int x, int y, int z, uint8_t mask) {
    TestNode node;
    node.mask = mask;
    node.data = id;
    node.level = level;
    node.x = x;
    node.y = y;
    node.z = z;
    g_nodes[id] = node;
}

static void tree_build_recursive(uint64_t id, int level, int x, int y, int z) {
    uint8_t mask = (level == g_tree_depth) ? 0 : 255;
    tree_node_set(id, level, x, y, z, mask);
    if (mask == 0) return;
    for (int octant = 0; octant < 8; octant++) {
        tree_build_recursive(id * 8 + octant, level + 1,
            x * 2 + ((octant >> 0) & 1),
            y * 2 + ((octant >> 1) & 1),
            z * 2 + ((octant >> 2) & 1));
    }
}

static void tree_build(int depth) {
    g_tree_depth = depth;
    // Node ids occupy [8^level, 2*8^level), so 2*8^depth entries suffice.
    size_t capacity = (size_t)2 << (3 * depth);
    g_nodes.assign(capacity, TestNode{});
    tree_build_recursive(1, 0, 0, 0, 0);
}

static DMirBool test_traverse_start(const void*, DMirAddress node_ref, DMirAddress, DMirTraversal* dst) {
    if (node_ref >= g_nodes.size()) return 0;
    const TestNode& node = g_nodes[node_ref];
    if (node.level < 0) return 0;
    dst->mask[0] = node.mask;
    dst->node[0] = node_ref;
    dst->data[0] = node.data;
    return 1;
}

static DMirBool test_traverse_next(const void*, DMirTraversal* src, int32_t index, DMirTraversal* dst) {
    uint64_t node = src->node[index];
    uint8_t mask = src->mask[index];
    uint64_t base = node * 8;
    for (int octant = 0; octant < 8; octant++) {
        if ((mask & (1 << octant)) == 0) {
            dst->mask[octant] = 0;
            dst->node[octant] = 0;
            dst->data[octant] = 0;
            continue;
        }
        uint64_t child = base + octant;
        dst->mask[octant] = g_nodes[child].mask;
        dst->node[octant] = child;
        dst->data[octant] = g_nodes[child].data;
    }
    return 1;
}

static DMirGeometry test_geometry(void) {
    DMirGeometry geometry;
    geometry.traverse_start = test_traverse_start;
    geometry.traverse_next = test_traverse_next;
    geometry.evaluate = nullptr;
    geometry.max_level = g_tree_depth;
    return geometry;
}

// ===================================================== //
// Cages and reference projection
// ===================================================== //

static void make_cage(float* cage, float cx, float cy, float cz, float hx, float hy, float hz) {
    for (int corner = 0; corner < 8; corner++) {
        cage[3*corner+0] = cx + ((corner & 1) ? hx : -hx);
        cage[3*corner+1] = cy + ((corner & 2) ? hy : -hy);
        cage[3*corner+2] = cz + ((corner & 4) ? hz : -hz);
    }
}

static void cage_shift(float* cage, float dx, float dy, float dz) {
    for (int corner = 0; corner < 8; corner++) {
        cage[3*corner+0] += dx;
        cage[3*corner+1] += dy;
        cage[3*corner+2] += dz;
    }
}

// A deliberately non-affine deformation: corner positions depend on
// the corner index in a way that cannot be represented by an affine
// transform of the root box.
static void cage_deform(float* cage, float amount) {
    for (int corner = 0; corner < 8; corner++) {
        float sx = (corner & 1) ? 1.0f : -1.0f;
        float sy = (corner & 2) ? 1.0f : -1.0f;
        float sz = (corner & 4) ? 1.0f : -1.0f;
        cage[3*corner+0] += amount * sx * sy;
        cage[3*corner+1] += amount * sy * sz;
        cage[3*corner+2] += amount * sx * sz;
    }
}

struct RefProjection {
    double scale_xy;
    double scale_c;
    double scale_z;
    double offset_x;
    double offset_y;
};

static RefProjection ref_projection(DMirRect viewport, DMirFrustum frustum) {
    RefProjection p;
    double size_x = viewport.max_x - viewport.min_x + 1;
    double size_y = viewport.max_y - viewport.min_y + 1;
    double half_x = size_x * 0.5;
    double half_y = size_y * 0.5;
    p.scale_xy = ((half_x > half_y) ? half_x : half_y) / frustum.focal_extent;
    p.scale_c = 1.0 - frustum.perspective;
    p.scale_z = frustum.perspective / frustum.focal_depth;
    p.offset_x = viewport.min_x + half_x + frustum.offset_x;
    p.offset_y = viewport.min_y + half_y + frustum.offset_y;
    return p;
}

static void ref_trilinear(const float* cage, double tx, double ty, double tz, double* out) {
    double x = 0, y = 0, z = 0;
    for (int corner = 0; corner < 8; corner++) {
        double wx = (corner & 1) ? tx : (1.0 - tx);
        double wy = (corner & 2) ? ty : (1.0 - ty);
        double wz = (corner & 4) ? tz : (1.0 - tz);
        double w = wx * wy * wz;
        x += cage[3*corner+0] * w;
        y += cage[3*corner+1] * w;
        z += cage[3*corner+2] * w;
    }
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

static void ref_project(const RefProjection& p, const double* position, double* out) {
    double z = position[2];
    if (p.scale_z != 0.0) {
        double eye_z = -p.scale_c / p.scale_z;
        double clamp_z = eye_z + (std::max)(-eye_z, 1.0) * 1e-8;
        if (z < clamp_z) z = clamp_z;
    }
    double denom = p.scale_c + p.scale_z * z;
    out[0] = p.offset_x + position[0] * p.scale_xy / denom;
    out[1] = p.offset_y + position[1] * p.scale_xy / denom;
}

// Root-local [0,1]^3 center of a node.
static void node_local_center(uint64_t id, double* center) {
    const TestNode& node = g_nodes[id];
    double scale = std::ldexp(1.0, -node.level);
    center[0] = (node.x + 0.5) * scale;
    center[1] = (node.y + 0.5) * scale;
    center[2] = (node.z + 0.5) * scale;
}

// Independently compute the motion vector for the node that supplied a
// pixel's voxel reference.
static bool ref_motion_for_node(uint64_t id,
    const float* current_cage, const float* previous_cage,
    DMirRect current_viewport, DMirFrustum current_frustum,
    DMirRect previous_viewport, DMirFrustum previous_frustum,
    double* dx, double* dy)
{
    if (id >= g_nodes.size()) return false;
    if (g_nodes[id].level < 0) return false;

    double local[3];
    node_local_center(id, local);

    double current_position[3], previous_position[3];
    ref_trilinear(current_cage, local[0], local[1], local[2], current_position);
    ref_trilinear(previous_cage, local[0], local[1], local[2], previous_position);

    // Previous point must be inside the valid view depth range.
    if (previous_position[2] < previous_frustum.min_depth) return false;
    if (previous_position[2] > previous_frustum.max_depth) return false;

    RefProjection current_proj = ref_projection(current_viewport, current_frustum);
    RefProjection previous_proj = ref_projection(previous_viewport, previous_frustum);

    double current_screen[2], previous_screen[2];
    ref_project(current_proj, current_position, current_screen);
    ref_project(previous_proj, previous_position, previous_screen);

    *dx = previous_screen[0] - current_screen[0];
    *dy = previous_screen[1] - current_screen[1];
    return true;
}

// ===================================================== //
// Rendering helpers
// ===================================================== //

struct Instance {
    float cage[24];
    float previous_cage[24];
    uint32_t group;
    bool has_motion;
    bool has_previous;
};

static Instance make_instance(float cx, float cy, float cz, float hx, float hy, float hz, uint32_t group) {
    Instance instance;
    make_cage(instance.cage, cx, cy, cz, hx, hy, hz);
    memcpy(instance.previous_cage, instance.cage, sizeof(instance.cage));
    instance.group = group;
    instance.has_motion = false;
    instance.has_previous = true;
    return instance;
}

struct RenderTarget {
    DMirFramebuffer* framebuffer;
    DMirBatcher* batcher;
    DMirRenderer* renderer;
    DMirRect viewport;
    DMirFrustum frustum;
};

static RenderTarget target_make(uint32_t w, uint32_t h, DMirRect viewport, DMirFrustum frustum) {
    RenderTarget target;
    target.framebuffer = dmir_framebuffer_make(w, h);
    target.batcher = dmir_batcher_make();
    target.renderer = dmir_renderer_make();
    target.viewport = viewport;
    target.frustum = frustum;
    target.renderer->framebuffer = target.framebuffer;
    target.renderer->batcher = target.batcher;
    target.renderer->rect = viewport;
    return target;
}

static void target_free(RenderTarget& target) {
    dmir_renderer_free(target.renderer);
    dmir_batcher_free(target.batcher);
    dmir_framebuffer_free(target.framebuffer);
}

static void target_begin(RenderTarget& target) {
    dmir_framebuffer_clear(target.framebuffer);
    dmir_batcher_reset(target.batcher, target.viewport, target.frustum);
}

static void target_add(RenderTarget& target, Instance& instance, DMirGeometry* geometry,
    DMirAddress root, DMirEffects effects)
{
    if (instance.has_motion) {
        DMirMotionInput motion;
        memcpy(motion.previous_cage, instance.previous_cage, sizeof(motion.previous_cage));
        motion.previous_viewport = target.viewport;
        motion.previous_frustum = target.frustum;
        dmir_batcher_add_motion(target.batcher, target.framebuffer,
            instance.group, instance.cage, geometry, root, effects, &motion);
    } else {
        dmir_batcher_add(target.batcher, target.framebuffer,
            instance.group, instance.cage, geometry, root, effects);
    }
}

static void target_draw(RenderTarget& target) {
    dmir_batcher_sort(target.batcher);
    dmir_renderer_draw(target.renderer);
}

static DMirEffects rect_effects(void) {
    DMirEffects effects;
    effects.max_level = -1;
    effects.dilation_abs = 0.0f;
    effects.dilation_rel = 0.0f;
    effects.shape = DMIR_SHAPE_RECT;
    return effects;
}

static int pixel_index(RenderTarget& target, int x, int y) {
    return dmir_pixel_index(target.framebuffer, x, y);
}

// Render a single instance with motion and verify every visible pixel's
// motion against the independent reference implementation.
static int verify_single_instance_ex(Instance& instance,
    DMirRect viewport, DMirFrustum frustum,
    DMirRect previous_viewport, DMirFrustum previous_frustum, int* out_checked);

static int verify_single_instance(Instance& instance, DMirRect previous_viewport,
    DMirFrustum previous_frustum, int* out_checked)
{
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    return verify_single_instance_ex(instance, viewport, frustum,
        previous_viewport, previous_frustum, out_checked);
}

static int verify_single_instance_ex(Instance& instance,
    DMirRect viewport, DMirFrustum frustum,
    DMirRect previous_viewport, DMirFrustum previous_frustum, int* out_checked)
{
    tree_build(4);
    DMirGeometry geometry = test_geometry();

    RenderTarget target = target_make(32, 32, viewport, frustum);

    instance.has_motion = true;
    if (previous_viewport.min_x > previous_viewport.max_x) previous_viewport = viewport;

    target_begin(target);

    DMirMotionInput motion;
    memcpy(motion.previous_cage, instance.previous_cage, sizeof(motion.previous_cage));
    motion.previous_viewport = previous_viewport;
    motion.previous_frustum = previous_frustum;
    dmir_batcher_add_motion(target.batcher, target.framebuffer,
        instance.group, instance.cage, &geometry, 1, rect_effects(), &motion);

    target_draw(target);

    int checked = 0;
    int valid_count = 0;
    int mismatch = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            checked++;
            if (!target.framebuffer->motion_valid[index]) { mismatch++; continue; }
            valid_count++;

            double expected_x, expected_y;
            bool valid = ref_motion_for_node(ref.voxel_id,
                instance.cage, instance.previous_cage,
                viewport, frustum, previous_viewport, previous_frustum,
                &expected_x, &expected_y);

            if (!valid) { mismatch++; continue; }

            double actual_x = target.framebuffer->motion[2*index+0];
            double actual_y = target.framebuffer->motion[2*index+1];
            if (std::fabs(actual_x - expected_x) > 0.05) mismatch++;
            else if (std::fabs(actual_y - expected_y) > 0.05) mismatch++;
        }
    }

    *out_checked = checked;
    int ok = (checked > 0) && (valid_count > 0) && (mismatch == 0);
    target_free(target);
    return ok;
}

// ===================================================== //
// Projection group
// ===================================================== //

// Same cage and projection in both frames: every motion vector is zero.
static int test_projection_identity(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 11);
    memcpy(instance.previous_cage, instance.cage, sizeof(instance.cage));

    tree_build(3);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    instance.has_motion = true;
    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    int visible = 0;
    int bad = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            if (target.framebuffer->voxel[index].affine_id < 0) continue;
            visible++;
            if (!target.framebuffer->motion_valid[index]) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+0]) > 1e-3) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1]) > 1e-3) bad++;
        }
    }

    TASK_CHECK(visible > 0);
    TASK_CHECK(bad == 0);
    target_free(target);
    return 0;
}

// A pure view-space translation of the previous cage produces the same
// pixel offset everywhere (orthographic projection).
static int test_projection_translation(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 12);
    cage_shift(instance.previous_cage, 0.25f, -0.125f, 0.0f);

    tree_build(4);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    instance.has_motion = true;
    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    double scale = (std::max)((viewport.max_x - viewport.min_x + 1) * 0.5,
        (viewport.max_y - viewport.min_y + 1) * 0.5) / frustum.focal_extent;
    double expected_x = 0.25 * scale;
    double expected_y = -0.125 * scale;

    int visible = 0;
    int bad = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            if (target.framebuffer->voxel[index].affine_id < 0) continue;
            visible++;
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }
            if (std::fabs(target.framebuffer->motion[2*index+0] - expected_x) > 0.05) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1] - expected_y) > 0.05) bad++;
        }
    }

    TASK_CHECK(visible > 0);
    TASK_CHECK(bad == 0);
    target_free(target);
    return 0;
}

// A non-affine deformation must be sampled at each node's own center;
// nodes that share a data reference must not share a motion vector.
static int test_projection_nonaffine_shared(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 13);
    cage_deform(instance.previous_cage, 0.15f);

    tree_build(3);
    DMirGeometry geometry = test_geometry();

    // Two spatially distinct depth-3 leaves share the same data_ref.
    // Both are placed in the front (z = 0) slab so neither is occluded
    // by an unrelated leaf; their node boxes (and hence centers) differ.
    uint64_t shared_a = 1;
    uint64_t shared_b = 1;
    for (int level = 0; level < 3; level++) {
        shared_a = shared_a * 8 + 0; // (0,0,0) corner of its parent
        shared_b = shared_b * 8 + 2; // y-neighbour in the same front slab
    }
    TASK_CHECK(g_nodes[shared_a].level == 3);
    TASK_CHECK(g_nodes[shared_b].level == 3);
    uint64_t shared_data = 0xC0FFEE;
    g_nodes[shared_a].data = shared_data;
    g_nodes[shared_b].data = shared_data;

    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;

    // Compare rendering with motion against a motion-free rendering
    // (depth and voxel buffers must be identical).
    DMirFramebuffer* reference = dmir_framebuffer_make(32, 32);
    DMirBatcher* reference_batcher = dmir_batcher_make();
    DMirRenderer* reference_renderer = dmir_renderer_make();
    dmir_framebuffer_clear(reference);
    dmir_batcher_reset(reference_batcher, viewport, frustum);
    dmir_batcher_add(reference_batcher, reference, instance.group,
        instance.cage, &geometry, 1, rect_effects());
    dmir_batcher_sort(reference_batcher);
    reference_renderer->framebuffer = reference;
    reference_renderer->batcher = reference_batcher;
    reference_renderer->rect = viewport;
    dmir_renderer_draw(reference_renderer);

    RenderTarget target = target_make(32, 32, viewport, frustum);
    instance.has_motion = true;
    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    // Motion must not alter depth or voxel selection.
    int buffers_equal = memcmp(target.framebuffer->depth, reference->depth,
        32 * 32 * sizeof(DMirDepth)) == 0;
    buffers_equal &= memcmp(target.framebuffer->voxel, reference->voxel,
        32 * 32 * sizeof(DMirVoxelRef)) == 0;
    TASK_CHECK(buffers_equal);

    int shared_seen = 0;
    int shared_distinct = 0;
    float shared_motion[2] = {0, 0};
    bool have_shared_motion = false;
    int checked = 0;
    int bad = 0;

    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            if (!target.framebuffer->motion_valid[index]) continue;

            double actual_x = target.framebuffer->motion[2*index+0];
            double actual_y = target.framebuffer->motion[2*index+1];

            if (ref.voxel_id == shared_data) {
                shared_seen++;
                if (!have_shared_motion) {
                    shared_motion[0] = (float)actual_x;
                    shared_motion[1] = (float)actual_y;
                    have_shared_motion = true;
                } else if (std::fabs(actual_x - shared_motion[0]) > 0.25 ||
                           std::fabs(actual_y - shared_motion[1]) > 0.25) {
                    shared_distinct = 1;
                }

                double expected_ax, expected_ay, expected_bx, expected_by;
                bool valid_a = ref_motion_for_node(shared_a, instance.cage, instance.previous_cage,
                    viewport, frustum, viewport, frustum, &expected_ax, &expected_ay);
                bool valid_b = ref_motion_for_node(shared_b, instance.cage, instance.previous_cage,
                    viewport, frustum, viewport, frustum, &expected_bx, &expected_by);
                bool matches_a = valid_a && std::fabs(actual_x - expected_ax) <= 0.05
                    && std::fabs(actual_y - expected_ay) <= 0.05;
                bool matches_b = valid_b && std::fabs(actual_x - expected_bx) <= 0.05
                    && std::fabs(actual_y - expected_by) <= 0.05;
                checked++;
                if (!matches_a && !matches_b) bad++;
            } else {
                double expected_x, expected_y;
                if (!ref_motion_for_node(ref.voxel_id, instance.cage, instance.previous_cage,
                    viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
                checked++;
                if (std::fabs(actual_x - expected_x) > 0.05) bad++;
                else if (std::fabs(actual_y - expected_y) > 0.05) bad++;
            }
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);
    TASK_CHECK(shared_seen > 0);
    // The shared reference is used by two distinct spatial nodes; their
    // motion samples must not collapse into a single vector.
    TASK_CHECK(shared_distinct == 1);

    dmir_renderer_free(reference_renderer);
    dmir_batcher_free(reference_batcher);
    dmir_framebuffer_free(reference);
    target_free(target);
    return 0;
}

// Different previous viewport/frustum: the offset must reflect the
// previous projection parameters, not the current ones.
static int test_projection_different_frustum(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 14);

    DMirRect previous_viewport = {3, 2, 34, 33}; // same size, shifted
    DMirFrustum previous_frustum = DMIR_FRUSTUM_DEFAULT;
    previous_frustum.focal_extent = 1.0f;

    int checked = 0;
    bool ok = verify_single_instance(instance, previous_viewport, previous_frustum, &checked);
    // The shift changes only the projection offsets.
    TASK_CHECK(checked > 0);
    TASK_CHECK(ok);
    return 0;
}

// A perspective projection in both frames: the vector is not a uniform
// offset, so this exercises the per-node projection with real divide.
static int test_projection_perspective(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 0.5f, 0.5f, 0.5f, 15);
    cage_shift(instance.previous_cage, 0.15f, -0.05f, 0.3f);

    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    frustum.perspective = 1.0f;
    frustum.focal_depth = 2.0f;
    frustum.focal_extent = 1.0f;

    int checked = 0;
    bool ok = verify_single_instance_ex(instance, viewport, frustum,
        viewport, frustum, &checked);
    TASK_CHECK(checked > 0);
    TASK_CHECK(ok);
    return 0;
}

// ===================================================== //
// Paths group
// ===================================================== //
// Large nodes go through the cage-subdivision + orthographic traversal.
static int test_paths_ortho_large(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 21);
    cage_shift(instance.previous_cage, 0.1f, 0.05f, 0.0f);

    int checked = 0;
    bool ok = verify_single_instance(instance, DMirRect{0,0,-1,-1}, DMIR_FRUSTUM_DEFAULT, &checked);
    TASK_CHECK(checked > 0);
    TASK_CHECK(ok);
    return 0;
}

// A deformed current cage forces cage subdivision before the ortho
// traversal; motion sampling must survive the subdivision.
static int test_paths_deformed_cage(void) {
    TASK_CASE_BEGIN();
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 22);
    cage_deform(instance.cage, 0.35f);
    memcpy(instance.previous_cage, instance.cage, sizeof(instance.cage));
    cage_shift(instance.previous_cage, 0.05f, 0.0f, 0.02f);

    tree_build(4);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    instance.has_motion = true;
    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    int checked = 0;
    int bad = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }
            double expected_x, expected_y;
            if (!ref_motion_for_node(ref.voxel_id, instance.cage, instance.previous_cage,
                viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
            checked++;
            if (std::fabs(target.framebuffer->motion[2*index+0] - expected_x) > 0.05) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1] - expected_y) > 0.05) bad++;
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);
    target_free(target);
    return 0;
}

// Small viewport + deep tree exercises the 2x2 / 3x3 / 4x4 map paths.
// Motion must be sampled at the map-selected child node, not at the
// parent node's center. Several viewport sizes are tried so that all
// three map thresholds are hit.
static int test_paths_maps_one(int viewport_size) {
    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 23);
    cage_deform(instance.previous_cage, 0.2f);

    tree_build(5);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, viewport_size - 1, viewport_size - 1};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(viewport_size, viewport_size, viewport, frustum);

    instance.has_motion = true;
    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    int checked = 0;
    int bad = 0;
    int distinct = 0;
    uint64_t previous_voxel = ~(uint64_t)0;
    float previous_motion[2] = {0, 0};
    bool have_previous = false;

    for (int y = 0; y < viewport_size; y++) {
        for (int x = 0; x < viewport_size; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }

            double expected_x, expected_y;
            if (!ref_motion_for_node(ref.voxel_id, instance.cage, instance.previous_cage,
                viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
            checked++;

            double actual_x = target.framebuffer->motion[2*index+0];
            double actual_y = target.framebuffer->motion[2*index+1];
            if (std::fabs(actual_x - expected_x) > 0.05) bad++;
            else if (std::fabs(actual_y - expected_y) > 0.05) bad++;

            if (have_previous && ref.voxel_id != previous_voxel) {
                if (std::fabs(actual_x - previous_motion[0]) > 1e-3 ||
                    std::fabs(actual_y - previous_motion[1]) > 1e-3) distinct = 1;
            }
            previous_voxel = ref.voxel_id;
            previous_motion[0] = (float)actual_x;
            previous_motion[1] = (float)actual_y;
            have_previous = true;
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);
    TASK_CHECK(distinct == 1);
    target_free(target);
    return 0;
}

static int test_paths_maps(void) {
    TASK_CASE_BEGIN();
    // Empirically, viewport 20 hits the 2x2 and 3x3 maps, viewport 8
    // hits the 4x4 map, and viewport 12 hits the 3x3 map on its own.
    bool any_2px = false, any_3px = false, any_4px = false;
    const int sizes[] = {8, 12, 20};
    for (int size : sizes) {
        TASK_CHECK(test_paths_maps_one(size) == 0);
    }

    // Re-render at 20 to assert the 2x2 and 3x3 maps actually ran, and
    // at 8 for the 4x4 map; the checks above already validated samples.
    for (int size : sizes) {
        tree_build(5);
        DMirGeometry geometry = test_geometry();
        DMirRect viewport = {0, 0, size - 1, size - 1};
        DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
        RenderTarget target = target_make(size, size, viewport, frustum);
        Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 23);
        cage_deform(instance.previous_cage, 0.2f);
        instance.has_motion = true;
        target_begin(target);
        target_add(target, instance, &geometry, 1, rect_effects());
        target_draw(target);
        any_2px |= (target.framebuffer->stats[DMIR_SPLATS_2PX] > 0);
        any_3px |= (target.framebuffer->stats[DMIR_SPLATS_3PX] > 0);
        any_4px |= (target.framebuffer->stats[DMIR_SPLATS_4PX] > 0);
        target_free(target);
    }

    TASK_CHECK(any_2px);
    TASK_CHECK(any_3px);
    TASK_CHECK(any_4px);
    return 0;
}

// The same geometry instance submitted twice with independent motion;
// groups are intentionally equal.
static int test_paths_duplicate_instances(void) {
    TASK_CASE_BEGIN();
    tree_build(3);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 15};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 16, viewport, frustum);

    Instance left = make_instance(-1.0f, 0, 2.0f, 0.4f, 0.8f, 1, 77);
    Instance right = make_instance(1.0f, 0, 2.0f, 0.4f, 0.8f, 1, 77);
    cage_shift(left.previous_cage, 0.2f, 0.0f, 0.0f);
    cage_shift(right.previous_cage, -0.1f, 0.15f, 0.0f);
    left.has_motion = true;
    right.has_motion = true;

    target_begin(target);
    target_add(target, left, &geometry, 1, rect_effects());
    target_add(target, right, &geometry, 1, rect_effects());
    target_draw(target);

    int checked = 0;
    int bad = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }

            // Left instance covers x < 16, right instance x >= 16.
            Instance& source = (x < 16) ? left : right;
            double expected_x, expected_y;
            if (!ref_motion_for_node(ref.voxel_id, source.cage, source.previous_cage,
                viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
            checked++;
            if (std::fabs(target.framebuffer->motion[2*index+0] - expected_x) > 0.05) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1] - expected_y) > 0.05) bad++;
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);
    target_free(target);
    return 0;
}

// Front/back occlusion: the winning (nearest) sample owns both the voxel
// reference and the motion vector.
static int test_paths_occlusion(void) {
    TASK_CASE_BEGIN();
    tree_build(3);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    Instance back = make_instance(0, 0, 4.0f, 1, 1, 1, 31);
    Instance front = make_instance(0, 0, 2.0f, 1, 1, 1, 32);
    cage_shift(back.previous_cage, 0.3f, 0.2f, 0.0f);
    cage_shift(front.previous_cage, -0.2f, 0.1f, 0.0f);
    back.has_motion = true;
    front.has_motion = true;

    target_begin(target);
    target_add(target, back, &geometry, 1, rect_effects());
    target_add(target, front, &geometry, 1, rect_effects());
    target_draw(target);

    DMirAffineInfo* affine_infos = nullptr;
    uint32_t affine_count = 0;
    dmir_batcher_affine_get(target.batcher, &affine_infos, &affine_count);

    int checked = 0;
    int bad = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }
            TASK_CHECK((uint32_t)ref.affine_id < affine_count);

            uint32_t group = affine_infos[ref.affine_id].group;
            Instance& source = (group == front.group) ? front : back;
            double expected_x, expected_y;
            if (!ref_motion_for_node(ref.voxel_id, source.cage, source.previous_cage,
                viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
            checked++;
            if (std::fabs(target.framebuffer->motion[2*index+0] - expected_x) > 0.05) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1] - expected_y) > 0.05) bad++;
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);

    // The front instance should win at the screen center.
    int center = pixel_index(target, 16, 16);
    DMirVoxelRef center_ref = target.framebuffer->voxel[center];
    TASK_CHECK(center_ref.affine_id >= 0);
    TASK_CHECK(affine_infos[center_ref.affine_id].group == front.group);
    TASK_CHECK(target.framebuffer->motion_valid[center] != 0);

    target_free(target);
    return 0;
}

// ===================================================== //
// Lifecycle group
// ===================================================== //

static int test_lifecycle_clear_resize(void) {
    TASK_CASE_BEGIN();
    tree_build(2);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 15, 15};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(16, 16, viewport, frustum);

    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 41);
    cage_shift(instance.previous_cage, 0.2f, 0.0f, 0.0f);
    instance.has_motion = true;

    target_begin(target);
    target_add(target, instance, &geometry, 1, rect_effects());
    target_draw(target);

    int motion_pixels = 0;
    for (int i = 0; i < 16 * 16; i++) {
        if (target.framebuffer->motion_valid[i]) motion_pixels++;
    }
    TASK_CHECK(motion_pixels > 0);

    dmir_framebuffer_clear(target.framebuffer);
    int dirty = 0;
    for (int i = 0; i < 16 * 16; i++) {
        if (target.framebuffer->motion_valid[i]) dirty++;
        if (target.framebuffer->motion[2*i+0] != 0.0f) dirty++;
        if (target.framebuffer->motion[2*i+1] != 0.0f) dirty++;
    }
    TASK_CHECK(dirty == 0);

    dmir_framebuffer_resize(target.framebuffer, 24, 20);
    dirty = 0;
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 24; x++) {
            int index = dmir_pixel_index(target.framebuffer, x, y);
            if (target.framebuffer->motion_valid[index]) dirty++;
            if (target.framebuffer->motion[2*index+0] != 0.0f) dirty++;
            if (target.framebuffer->motion[2*index+1] != 0.0f) dirty++;
        }
    }
    TASK_CHECK(dirty == 0);

    target_free(target);
    return 0;
}

// dmir_batcher_reset must discard motion history from previous submits.
static int test_lifecycle_reset_drops_history(void) {
    TASK_CASE_BEGIN();
    tree_build(2);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 15, 15};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(16, 16, viewport, frustum);

    Instance with_motion = make_instance(0, 0, 2.0f, 1, 1, 1, 42);
    cage_shift(with_motion.previous_cage, 0.2f, 0.0f, 0.0f);
    with_motion.has_motion = true;

    Instance plain = make_instance(0, 0, 2.0f, 1, 1, 1, 43);

    target_begin(target);
    target_add(target, with_motion, &geometry, 1, rect_effects());

    // Reset before drawing: the pending motion submission is discarded.
    dmir_batcher_reset(target.batcher, viewport, frustum);
    target_add(target, plain, &geometry, 1, rect_effects());
    target_draw(target);

    int visible = 0;
    int valid = 0;
    for (int i = 0; i < 16 * 16; i++) {
        if (target.framebuffer->voxel[i].affine_id < 0) continue;
        visible++;
        if (target.framebuffer->motion_valid[i]) valid++;
        if (target.framebuffer->motion[2*i+0] != 0.0f) valid++;
        if (target.framebuffer->motion[2*i+1] != 0.0f) valid++;
    }

    TASK_CHECK(visible > 0);
    TASK_CHECK(valid == 0);
    target_free(target);
    return 0;
}

// Plain and motion-enabled submissions may be mixed; only the pixels
// produced by the motion-enabled submission carry valid motion.
static int test_lifecycle_mixed_entries(void) {
    TASK_CASE_BEGIN();
    tree_build(3);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 15};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 16, viewport, frustum);

    Instance left = make_instance(-1.0f, 0, 2.0f, 0.4f, 0.8f, 1, 44);
    Instance right = make_instance(1.0f, 0, 2.0f, 0.4f, 0.8f, 1, 45);
    cage_shift(right.previous_cage, 0.2f, 0.0f, 0.0f);
    left.has_motion = false;
    right.has_motion = true;

    target_begin(target);
    target_add(target, left, &geometry, 1, rect_effects());
    target_add(target, right, &geometry, 1, rect_effects());
    target_draw(target);

    int left_visible = 0, left_valid = 0;
    int right_visible = 0, right_valid = 0;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            if (target.framebuffer->voxel[index].affine_id < 0) continue;
            if (x < 16) {
                left_visible++;
                if (target.framebuffer->motion_valid[index]) left_valid++;
            } else {
                right_visible++;
                if (target.framebuffer->motion_valid[index]) right_valid++;
            }
        }
    }

    TASK_CHECK(left_visible > 0);
    TASK_CHECK(right_visible > 0);
    TASK_CHECK(left_valid == 0);
    TASK_CHECK(right_valid == right_visible);
    target_free(target);
    return 0;
}

// The batcher must copy the motion input; mutating/freeing the caller's
// data before drawing must not affect the result.
static int test_lifecycle_input_copied(void) {
    TASK_CASE_BEGIN();
    tree_build(3);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;

    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 46);
    cage_shift(instance.previous_cage, 0.2f, -0.1f, 0.0f);

    // Reference render with a pristine motion input held on the stack.
    RenderTarget reference = target_make(32, 32, viewport, frustum);
    instance.has_motion = true;
    target_begin(reference);
    target_add(reference, instance, &geometry, 1, rect_effects());
    target_draw(reference);

    // Same render, but the input is heap-allocated, mutated and freed
    // immediately after dmir_batcher_add_motion.
    RenderTarget target = target_make(32, 32, viewport, frustum);
    target_begin(target);

    DMirMotionInput* motion = new DMirMotionInput();
    memcpy(motion->previous_cage, instance.previous_cage, sizeof(motion->previous_cage));
    motion->previous_viewport = viewport;
    motion->previous_frustum = frustum;

    Instance current = instance;
    current.has_motion = true;
    dmir_batcher_add_motion(target.batcher, target.framebuffer,
        current.group, current.cage, &geometry, 1, rect_effects(), motion);

    memset(motion, 0, sizeof(*motion));
    delete motion;

    target_draw(target);

    int motion_equal = memcmp(target.framebuffer->motion, reference.framebuffer->motion,
        32 * 32 * 2 * sizeof(float)) == 0;
    motion_equal &= memcmp(target.framebuffer->motion_valid, reference.framebuffer->motion_valid,
        32 * 32 * sizeof(uint8_t)) == 0;
    TASK_CHECK(motion_equal);

    target_free(reference);
    target_free(target);
    return 0;
}

// Enabling motion must not change depth or voxel buffers for an
// otherwise identical scene.
static int test_lifecycle_buffers_unchanged(void) {
    TASK_CASE_BEGIN();
    tree_build(4);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;

    Instance back = make_instance(0, 0, 4.0f, 1, 1, 1, 47);
    Instance front = make_instance(0, 0, 2.0f, 1, 1, 1, 48);

    RenderTarget plain = target_make(32, 32, viewport, frustum);
    target_begin(plain);
    target_add(plain, back, &geometry, 1, rect_effects());
    target_add(plain, front, &geometry, 1, rect_effects());
    target_draw(plain);

    RenderTarget with_motion = target_make(32, 32, viewport, frustum);
    target_begin(with_motion);
    back.has_motion = true;
    front.has_motion = true;
    cage_shift(back.previous_cage, 0.2f, 0.1f, 0.0f);
    cage_shift(front.previous_cage, -0.2f, 0.1f, 0.0f);
    target_add(with_motion, back, &geometry, 1, rect_effects());
    target_add(with_motion, front, &geometry, 1, rect_effects());
    target_draw(with_motion);

    TASK_CHECK(memcmp(plain.framebuffer->depth, with_motion.framebuffer->depth,
        32 * 32 * sizeof(DMirDepth)) == 0);
    TASK_CHECK(memcmp(plain.framebuffer->voxel, with_motion.framebuffer->voxel,
        32 * 32 * sizeof(DMirVoxelRef)) == 0);
    TASK_CHECK(memcmp(plain.framebuffer->motion, with_motion.framebuffer->motion,
        32 * 32 * 2 * sizeof(float)) != 0);

    target_free(plain);
    target_free(with_motion);
    return 0;
}

// Deferred writes must pick motion from the same winning fragment as
// depth and voxel data, including when the winner changes.
static int test_lifecycle_deferred_winner(void) {
    TASK_CASE_BEGIN();
    tree_build(4);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    Instance back = make_instance(0, 0, 4.0f, 1, 1, 1, 51);
    Instance front = make_instance(0, 0, 2.0f, 1, 1, 1, 52);
    cage_shift(back.previous_cage, 0.4f, 0.2f, 0.0f);
    cage_shift(front.previous_cage, -0.3f, 0.15f, 0.0f);
    back.has_motion = true;
    front.has_motion = true;

    target_begin(target);
    // Submit the far instance first so the near one must overwrite it.
    target_add(target, back, &geometry, 1, rect_effects());
    target_add(target, front, &geometry, 1, rect_effects());
    target_draw(target);

    DMirAffineInfo* affine_infos = nullptr;
    uint32_t affine_count = 0;
    dmir_batcher_affine_get(target.batcher, &affine_infos, &affine_count);

    int bad = 0;
    int checked = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int index = pixel_index(target, x, y);
            DMirVoxelRef ref = target.framebuffer->voxel[index];
            if (ref.affine_id < 0) continue;
            TASK_CHECK((uint32_t)ref.affine_id < affine_count);
            uint32_t group = affine_infos[ref.affine_id].group;
            Instance& source = (group == front.group) ? front : back;

            // The motion vector must correspond to the same instance that
            // supplied the voxel reference.
            if (!target.framebuffer->motion_valid[index]) { bad++; continue; }
            double expected_x, expected_y;
            if (!ref_motion_for_node(ref.voxel_id, source.cage, source.previous_cage,
                viewport, frustum, viewport, frustum, &expected_x, &expected_y)) continue;
            checked++;
            if (std::fabs(target.framebuffer->motion[2*index+0] - expected_x) > 0.05) bad++;
            else if (std::fabs(target.framebuffer->motion[2*index+1] - expected_y) > 0.05) bad++;
        }
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(bad == 0);
    target_free(target);
    return 0;
}

// A plain (motion-less) instance that wins over a previously drawn
// motion-enabled instance must clear the stale motion vector.
static int test_lifecycle_overwrite_clears_motion(void) {
    TASK_CASE_BEGIN();
    tree_build(2);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    Instance far = make_instance(0, 0, 4.0f, 1, 1, 1, 61);
    cage_shift(far.previous_cage, 0.3f, 0.2f, 0.0f);
    far.has_motion = true;

    target_begin(target);
    target_add(target, far, &geometry, 1, rect_effects());
    target_draw(target);

    int center = pixel_index(target, 16, 16);
    TASK_CHECK(target.framebuffer->voxel[center].affine_id >= 0);
    TASK_CHECK(target.framebuffer->motion_valid[center] != 0);

    // Second pass: a nearer plain instance overwrites the center while
    // the framebuffer is intentionally not cleared, to exercise stale
    // motion being cleared by a winning plain sample.
    dmir_batcher_reset(target.batcher, viewport, frustum);
    Instance near_instance = make_instance(0, 0, 2.0f, 1, 1, 1, 62);
    near_instance.has_motion = false;
    target_add(target, near_instance, &geometry, 1, rect_effects());
    target_draw(target);

    TASK_CHECK(target.framebuffer->voxel[center].affine_id >= 0);
    TASK_CHECK(target.framebuffer->motion_valid[center] == 0);
    TASK_CHECK(target.framebuffer->motion[2*center+0] == 0.0f);
    TASK_CHECK(target.framebuffer->motion[2*center+1] == 0.0f);

    target_free(target);
    return 0;
}

// Non-rect shapes retain their original rendering path and report no
// valid motion.
static int test_lifecycle_other_shapes(void) {
    TASK_CASE_BEGIN();
    tree_build(2);
    DMirGeometry geometry = test_geometry();
    DMirRect viewport = {0, 0, 31, 31};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    RenderTarget target = target_make(32, 32, viewport, frustum);

    Instance instance = make_instance(0, 0, 2.0f, 1, 1, 1, 63);
    cage_shift(instance.previous_cage, 0.2f, 0.0f, 0.0f);
    instance.has_motion = true;

    DMirEffects effects = rect_effects();
    effects.shape = DMIR_SHAPE_CIRCLE;

    target_begin(target);
    target_add(target, instance, &geometry, 1, effects);
    target_draw(target);

    int visible = 0;
    int valid = 0;
    for (int i = 0; i < 32 * 32; i++) {
        if (target.framebuffer->voxel[i].affine_id < 0) continue;
        visible++;
        if (target.framebuffer->motion_valid[i]) valid++;
    }

    TASK_CHECK(visible > 0);
    TASK_CHECK(valid == 0);
    target_free(target);
    return 0;
}

// ===================================================== //
// Registration
// ===================================================== //

const struct task_case task_cases[] = {
    {"projection", "identity_zero", test_projection_identity},
    {"projection", "translation_uniform", test_projection_translation},
    {"projection", "nonaffine_shared_ref", test_projection_nonaffine_shared},
    {"projection", "different_frustum", test_projection_different_frustum},
    {"projection", "perspective", test_projection_perspective},
    {"paths", "ortho_large", test_paths_ortho_large},
    {"paths", "deformed_cage", test_paths_deformed_cage},
    {"paths", "maps_small_nodes", test_paths_maps},
    {"paths", "duplicate_instances", test_paths_duplicate_instances},
    {"paths", "front_back_occlusion", test_paths_occlusion},
    {"lifecycle", "clear_resize", test_lifecycle_clear_resize},
    {"lifecycle", "reset_drops_history", test_lifecycle_reset_drops_history},
    {"lifecycle", "mixed_entries", test_lifecycle_mixed_entries},
    {"lifecycle", "input_copied", test_lifecycle_input_copied},
    {"lifecycle", "buffers_unchanged", test_lifecycle_buffers_unchanged},
    {"lifecycle", "deferred_winner", test_lifecycle_deferred_winner},
    {"lifecycle", "overwrite_clears_motion", test_lifecycle_overwrite_clears_motion},
    {"lifecycle", "other_shapes", test_lifecycle_other_shapes},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
