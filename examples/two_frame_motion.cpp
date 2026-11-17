// Headless two-frame motion-vector example.
//
// This example renders a small octree in two consecutive frames and
// reports, per visible pixel, the motion vector (previous screen
// position minus current screen position, in pixels). It needs no
// window and no graphics API: run it and read stdout.
//
// Build (from the repository root, after `bash task_support/build.sh`):
//
//     c++ -std=c++20 -O1 -Idiscrete_mirage/include \
//         examples/two_frame_motion.c .task-build/libdmir.a -lm \
//         -o .task-build/two_frame_motion
//     .task-build/two_frame_motion
//
// Or via CMake: it is registered as the `two_frame_motion` target.

#include "discrete_mirage.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------- //
// A tiny in-memory octree with a shared-data DAG.
// ---------------------------------------------------------------- //

struct ExampleNode {
    uint8_t mask;
    uint64_t data_ref;
};

static std::vector<ExampleNode> g_nodes;

static void build_octree_recursive(uint64_t id, int level, int depth) {
    if (level == depth) {
        // Many spatial nodes share one data reference (a trivial DAG).
        g_nodes[id] = ExampleNode{0, 0xABCD};
        return;
    }
    g_nodes[id] = ExampleNode{255, id};
    for (int octant = 0; octant < 8; octant++) {
        build_octree_recursive(id * 8 + octant, level + 1, depth);
    }
}

static void build_octree(int depth) {
    // A full octree of this depth has 2 * 8^depth nodes (indices
    // [8^level, 2*8^level) at each level); node 0 is reserved invalid.
    g_nodes.assign((size_t)2 << (3 * depth), ExampleNode{0, 0});
    build_octree_recursive(1, 0, depth);
}

static DMirBool example_traverse_start(const void*, DMirAddress node_ref, DMirAddress, DMirTraversal* dst) {
    if (node_ref == 0 || node_ref >= g_nodes.size()) return 0;
    dst->mask[0] = g_nodes[node_ref].mask;
    dst->node[0] = node_ref;
    dst->data[0] = g_nodes[node_ref].data_ref;
    return 1;
}

static DMirBool example_traverse_next(const void*, DMirTraversal* src, int32_t index, DMirTraversal* dst) {
    uint64_t node = src->node[index];
    uint8_t mask = src->mask[index];
    for (int octant = 0; octant < 8; octant++) {
        if ((mask & (1 << octant)) == 0) {
            dst->mask[octant] = 0;
            dst->node[octant] = 0;
            dst->data[octant] = 0;
            continue;
        }
        uint64_t child = node * 8 + octant;
        dst->mask[octant] = g_nodes[child].mask;
        dst->node[octant] = child;
        dst->data[octant] = g_nodes[child].data_ref;
    }
    return 1;
}

// ---------------------------------------------------------------- //
// Scene setup
// ---------------------------------------------------------------- //

// The cage layout expected by the library: 8 corner triples ordered
// by (x, y, z) bits, in view space.
static void make_cage(float* cage, float cx, float cy, float cz,
    float hx, float hy, float hz)
{
    for (int corner = 0; corner < 8; corner++) {
        cage[3 * corner + 0] = cx + ((corner & 1) ? hx : -hx);
        cage[3 * corner + 1] = cy + ((corner & 2) ? hy : -hy);
        cage[3 * corner + 2] = cz + ((corner & 4) ? hz : -hz);
    }
}

struct Frame {
    float cage[24];
    DMirRect viewport;
    DMirFrustum frustum;
};

int main() {
    dmir_lookups_initialize();
    build_octree(4);

    DMirGeometry geometry;
    geometry.traverse_start = example_traverse_start;
    geometry.traverse_next = example_traverse_next;
    geometry.evaluate = nullptr;
    geometry.max_level = 4;

    const uint32_t width = 64;
    const uint32_t height = 48;

    DMirFramebuffer* framebuffer = dmir_framebuffer_make(width, height);
    DMirBatcher* batcher = dmir_batcher_make();
    DMirRenderer* renderer = dmir_renderer_make();
    renderer->framebuffer = framebuffer;
    renderer->batcher = batcher;

    DMirEffects effects;
    effects.max_level = -1;
    effects.dilation_abs = 0.0f;
    effects.dilation_rel = 0.0f;
    effects.shape = DMIR_SHAPE_RECT;

    // Frame 1 (previous): box is to the left and slightly smaller.
    Frame previous;
    previous.viewport = DMirRect{0, 0, (int32_t)width - 1, (int32_t)height - 1};
    previous.frustum = DMIR_FRUSTUM_DEFAULT;
    make_cage(previous.cage, -0.25f, 0.0f, 2.0f, 0.4f, 0.4f, 0.4f);

    // Frame 2 (current): box moved right and forward.
    Frame current;
    current.viewport = previous.viewport;
    current.frustum = previous.frustum;
    make_cage(current.cage, 0.20f, 0.05f, 1.6f, 0.4f, 0.4f, 0.4f);

    // Render the current frame, submitting the previous-frame state so
    // the framebuffer can report a motion vector per pixel.
    dmir_framebuffer_clear(framebuffer);
    dmir_batcher_reset(batcher, current.viewport, current.frustum);

    DMirMotionInput motion;
    memcpy(motion.previous_cage, previous.cage, sizeof(motion.previous_cage));
    motion.previous_viewport = previous.viewport;
    motion.previous_frustum = previous.frustum;

    dmir_batcher_add_motion(batcher, framebuffer, /*group*/ 1,
        current.cage, &geometry, /*root*/ 1, effects, &motion);

    dmir_batcher_sort(batcher);
    renderer->rect = current.viewport;
    dmir_renderer_draw(renderer);

    // Report a few sampled pixels plus overall statistics.
    int visible = 0;
    int with_motion = 0;
    double sum_dx = 0.0, sum_dy = 0.0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            int index = dmir_pixel_index(framebuffer, (int32_t)x, (int32_t)y);
            if (framebuffer->voxel[index].affine_id < 0) continue;
            visible++;
            if (!framebuffer->motion_valid[index]) continue;
            with_motion++;
            sum_dx += framebuffer->motion[2 * index + 0];
            sum_dy += framebuffer->motion[2 * index + 1];
        }
    }

    printf("TWO_FRAME_MOTION visible=%d with_motion=%d\n", visible, with_motion);
    if (visible > 0 && with_motion > 0) {
        printf("TWO_FRAME_MOTION mean_dx=%.4f mean_dy=%.4f\n",
            sum_dx / with_motion, sum_dy / with_motion);
    }

    // Center pixel detail (if the box covers it).
    int center = dmir_pixel_index(framebuffer, width / 2, height / 2);
    if (framebuffer->voxel[center].affine_id >= 0) {
        printf("TWO_FRAME_MOTION center valid=%u dx=%.4f dy=%.4f voxel_id=%llu affine_id=%d\n",
            (unsigned)framebuffer->motion_valid[center],
            framebuffer->motion[2 * center + 0],
            framebuffer->motion[2 * center + 1],
            (unsigned long long)framebuffer->voxel[center].voxel_id,
            framebuffer->voxel[center].affine_id);
    }

    dmir_renderer_free(renderer);
    dmir_batcher_free(batcher);
    dmir_framebuffer_free(framebuffer);

    return (visible > 0 && with_motion > 0) ? 0 : 1;
}
