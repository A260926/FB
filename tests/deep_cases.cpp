// Deep layer capture regression cases. Registered via task_cases;
// the driver (task_support/driver.c) provides main().

#include "task_support/check.h"

#include <discrete_mirage.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------
// Test geometry: a full octree of a given depth. Node references
// use heap indexing (child = parent*8 + octant + 1). When "share"
// is set, every node's data_ref is 777 (simulating a DAG whose
// spatial nodes share voxel data).
// ---------------------------------------------------------------

struct TestGeom {
    DMirGeometry base;
    int levels;
    int share;
};

int tg_level_of(uint64_t n) {
    int level = 0;
    uint64_t start = 0, size = 1;
    while (n >= start + size) { start += size; size *= 8; level++; }
    return level;
}

DMirBool tg_start(const void* g, DMirAddress, DMirAddress, DMirTraversal* t) {
    const TestGeom* tg = (const TestGeom*)g;
    t->mask[0] = (tg->levels > 0) ? 255 : 0;
    t->node[0] = 0;
    t->data[0] = tg->share ? 777 : 0;
    return 1;
}

DMirBool tg_next(const void* g, DMirTraversal* src, int32_t index, DMirTraversal* dst) {
    const TestGeom* tg = (const TestGeom*)g;
    uint64_t parent = src->node[index];
    int pl = tg_level_of(parent);
    for (int o = 0; o < 8; o++) {
        uint64_t child = parent * 8 + (uint64_t)o + 1;
        dst->node[o] = child;
        dst->mask[o] = (pl + 1 < tg->levels) ? 255 : 0;
        dst->data[o] = tg->share ? 777 : child;
    }
    return 1;
}

TestGeom make_geom(int levels, int share) {
    TestGeom tg;
    tg.base.traverse_start = tg_start;
    tg.base.traverse_next = tg_next;
    tg.base.evaluate = 0;
    tg.base.max_level = -1;
    tg.levels = levels;
    tg.share = share;
    return tg;
}

// ---------------------------------------------------------------
// Procedural geometry (evaluate callback): a solid ball of voxels
// inside the root's [0,1]^3, splat at a fixed level.
// ---------------------------------------------------------------

struct EvalGeom {
    DMirGeometry base;
    int splat_level;
};

int32_t eval_ball(const void* g, DMirNodeBox* box, DMirAddress* data_ref) {
    const EvalGeom* eg = (const EvalGeom*)g;
    if ((int)box->level < eg->splat_level) {
        *data_ref = 0;
        return 1; // subdivide
    }
    float size = 1.0f / (float)(1u << box->level);
    float x = ((float)box->x + 0.5f) * size - 0.5f;
    float y = ((float)box->y + 0.5f) * size - 0.5f;
    float z = ((float)box->z + 0.5f) * size - 0.5f;
    if (x*x + y*y + z*z > 0.45f * 0.45f) return -1; // outside: skip
    *data_ref = 1 + box->x + ((uint64_t)box->y << 8) + ((uint64_t)box->z << 16);
    return 0; // splat
}

EvalGeom make_eval_geom(int splat_level) {
    EvalGeom eg;
    eg.base.traverse_start = 0;
    eg.base.traverse_next = 0;
    eg.base.evaluate = eval_ball;
    eg.base.max_level = -1;
    eg.splat_level = splat_level;
    return eg;
}

// ---------------------------------------------------------------
// Materials: constant linear non-premultiplied RGBA.
// ---------------------------------------------------------------

struct MatSpec { float rgba[4]; };

void mat_const(void* user, DMirAddress, float* rgba) {
    const MatSpec* m = (const MatSpec*)user;
    rgba[0] = m->rgba[0];
    rgba[1] = m->rgba[1];
    rgba[2] = m->rgba[2];
    rgba[3] = m->rgba[3];
}

MatSpec mat(float r, float g, float b, float a) {
    MatSpec m = {{r, g, b, a}};
    return m;
}

// ---------------------------------------------------------------
// Scene plumbing
// ---------------------------------------------------------------

struct Scene {
    DMirFramebuffer* fb;
    DMirBatcher* b;
    DMirRenderer* r;
    int w, h;
};

Scene scene_make(int w, int h) {
    Scene s;
    s.w = w;
    s.h = h;
    s.fb = dmir_framebuffer_make((uint32_t)w, (uint32_t)h);
    s.b = dmir_batcher_make();
    s.r = dmir_renderer_make();
    s.r->framebuffer = s.fb;
    s.r->batcher = s.b;
    s.r->rect.min_x = 0;
    s.r->rect.min_y = 0;
    s.r->rect.max_x = w - 1;
    s.r->rect.max_y = h - 1;
    return s;
}

void scene_free(Scene& s) {
    dmir_renderer_free(s.r);
    dmir_batcher_free(s.b);
    dmir_framebuffer_free(s.fb);
}

DMirRect viewport_of(const Scene& s) {
    DMirRect r = {0, 0, s.w - 1, s.h - 1};
    return r;
}

DMirFrustum frustum_ortho() {
    DMirFrustum f = DMIR_FRUSTUM_DEFAULT;
    f.min_depth = 0.1f;
    f.max_depth = 100.0f;
    f.focal_extent = 1.0f;
    f.focal_depth = 1.0f;
    f.perspective = 0.0f;
    return f;
}

void box_cage(float* c, float x0, float y0, float z0, float x1, float y1, float z1) {
    for (int i = 0; i < 8; i++) {
        c[i*3+0] = (i & 1) ? x1 : x0;
        c[i*3+1] = (i & 2) ? y1 : y0;
        c[i*3+2] = (i & 4) ? z1 : z0;
    }
}

DMirEffects rect_effects(int max_level, float dilation) {
    DMirEffects e;
    e.max_level = max_level;
    e.dilation_abs = dilation;
    e.dilation_rel = 0.0f;
    e.shape = DMIR_SHAPE_RECT;
    return e;
}

// ---------------------------------------------------------------
// Layer helpers
// ---------------------------------------------------------------

bool approx_eq(float a, float b) {
    return std::fabs(a - b) <= 1e-6f;
}

// The reference ordering: near to far, ties broken by
// submission key and node box level/x/y/z.
bool layer_less(const DMirLayer& a, const DMirLayer& b) {
    if (a.depth != b.depth) return a.depth < b.depth;
    if (a.submission_key != b.submission_key) return a.submission_key < b.submission_key;
    if (a.node_box.level != b.node_box.level) return a.node_box.level < b.node_box.level;
    if (a.node_box.x != b.node_box.x) return a.node_box.x < b.node_box.x;
    if (a.node_box.y != b.node_box.y) return a.node_box.y < b.node_box.y;
    return a.node_box.z < b.node_box.z;
}

bool layer_same(const DMirLayer& a, const DMirLayer& b) {
    return a.submission_key == b.submission_key
        && a.node_box.level == b.node_box.level
        && a.node_box.x == b.node_box.x
        && a.node_box.y == b.node_box.y
        && a.node_box.z == b.node_box.z
        && a.data_ref == b.data_ref
        && a.depth == b.depth
        && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Every pixel: at most K layers, strictly sorted (which also
// proves there are no duplicate sample identities).
int check_all_sorted(DMirFramebuffer* fb, int w, int h, uint32_t K) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t n = dmir_deep_count(fb, x, y);
            TASK_CHECK(n <= K);
            for (uint32_t i = 1; i < n; i++) {
                const DMirLayer* a = dmir_deep_get(fb, x, y, i - 1);
                const DMirLayer* b = dmir_deep_get(fb, x, y, i);
                TASK_CHECK(a != 0 && b != 0);
                TASK_CHECK(layer_less(*a, *b));
            }
        }
    }
    return 0;
}

// Independent over-compositing of a small ordered list of layers.
void manual_composite(DMirFramebuffer* fb, int x, int y, const float* bg,
    float* out, float* out_t)
{
    float c0 = 0, c1 = 0, c2 = 0, t = 1.0f;
    uint32_t n = dmir_deep_count(fb, x, y);
    for (uint32_t i = 0; i < n; i++) {
        const DMirLayer* L = dmir_deep_get(fb, x, y, i);
        float a = L->a;
        c0 += t * a * L->r;
        c1 += t * a * L->g;
        c2 += t * a * L->b;
        t *= 1.0f - a;
    }
    out[0] = c0 + t * bg[0];
    out[1] = c1 + t * bg[1];
    out[2] = c2 + t * bg[2];
    *out_t = t;
}

int check_composite_matches(DMirFramebuffer* fb, int w, int h) {
    float bg[3] = {0.1f, 0.2f, 0.3f};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float expect[3], expect_t, got[3];
            manual_composite(fb, x, y, bg, expect, &expect_t);
            float got_t = dmir_deep_composite(fb, x, y, bg, got);
            TASK_CHECK(approx_eq(got[0], expect[0]));
            TASK_CHECK(approx_eq(got[1], expect[1]));
            TASK_CHECK(approx_eq(got[2], expect[2]));
            TASK_CHECK(approx_eq(got_t, expect_t));
        }
    }
    return 0;
}

struct Dump {
    int w, h;
    uint32_t K;
    std::vector<uint32_t> count;
    std::vector<DMirLayer> layers;
};

Dump dump_capture(DMirFramebuffer* fb, int w, int h, uint32_t K) {
    Dump d;
    d.w = w; d.h = h; d.K = K;
    d.count.assign((size_t)w * h, 0);
    d.layers.assign((size_t)w * h * K, DMirLayer());
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t n = dmir_deep_count(fb, x, y);
            d.count[(size_t)y * w + x] = n;
            for (uint32_t i = 0; i < n; i++) {
                d.layers[((size_t)y * w + x) * K + i] = *dmir_deep_get(fb, x, y, i);
            }
        }
    }
    return d;
}

int dumps_equal(const Dump& a, const Dump& b) {
    TASK_CHECK(a.w == b.w && a.h == b.h && a.K == b.K);
    for (size_t p = 0; p < a.count.size(); p++) {
        TASK_CHECK(a.count[p] == b.count[p]);
        for (uint32_t i = 0; i < a.count[p]; i++) {
            TASK_CHECK(layer_same(a.layers[p * a.K + i], b.layers[p * b.K + i]));
        }
    }
    return 0;
}

// ---------------------------------------------------------------
// layers group
// ---------------------------------------------------------------

int case_two_layer_stack() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec red = mat(1, 0, 0, 0.5f);
    MatSpec blue = mat(0, 0, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 11, mat_const, &red, 1, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 4.0f, 0.9f, 0.9f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 22, mat_const, &blue, 1, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 2);
    const DMirLayer* L0 = dmir_deep_get(s.fb, 8, 8, 0);
    const DMirLayer* L1 = dmir_deep_get(s.fb, 8, 8, 1);
    TASK_CHECK(L0 != 0 && L1 != 0);
    TASK_CHECK(L0->submission_key == 11);
    TASK_CHECK(L1->submission_key == 22);
    TASK_CHECK(L0->depth == dmir_z_to_depth(s.b, 2.25f));
    TASK_CHECK(L1->depth == dmir_z_to_depth(s.b, 4.25f));
    TASK_CHECK(L0->node_box.level == 0 && L0->node_box.x == 0 &&
               L0->node_box.y == 0 && L0->node_box.z == 0);
    TASK_CHECK(L1->node_box.level == 0);
    TASK_CHECK(L0->r == 1.0f && L0->g == 0.0f && L0->b == 0.0f && L0->a == 0.5f);
    TASK_CHECK(L1->b == 1.0f && L1->a == 0.5f);

    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out[3];
    float t = dmir_deep_composite(s.fb, 8, 8, bg, out);
    TASK_CHECK(approx_eq(out[0], 0.5f + 0.25f * 0.1f));
    TASK_CHECK(approx_eq(out[1], 0.25f * 0.2f));
    TASK_CHECK(approx_eq(out[2], 0.25f + 0.25f * 0.3f));
    TASK_CHECK(approx_eq(t, 0.25f));

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 8) == 0);
    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_zero_and_full_alpha() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec ghost = mat(1, 1, 0, 0.0f); // alpha 0: must not occupy a layer
    MatSpec solid = mat(0, 1, 0, 1.0f); // alpha 1
    MatSpec back = mat(0, 0, 1, 0.5f);  // behind the alpha=1 layer
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &ghost, 7, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 3.0f, 0.9f, 0.9f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &solid, 7, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 4.0f, 0.9f, 0.9f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 3, mat_const, &back, 7, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // The alpha=0 submission left no layers; the layer behind the
    // alpha=1 one is still recorded.
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 2);
    const DMirLayer* L0 = dmir_deep_get(s.fb, 8, 8, 0);
    const DMirLayer* L1 = dmir_deep_get(s.fb, 8, 8, 1);
    TASK_CHECK(L0 != 0 && L1 != 0);
    TASK_CHECK(L0->submission_key == 2);
    TASK_CHECK(L1->submission_key == 3);
    TASK_CHECK(L1->b == 1.0f && L1->a == 0.5f);

    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out[3];
    float t = dmir_deep_composite(s.fb, 8, 8, bg, out);
    TASK_CHECK(approx_eq(t, 0.0f));
    TASK_CHECK(approx_eq(out[0], 0.0f) && approx_eq(out[1], 1.0f) && approx_eq(out[2], 0.0f));

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 8) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_same_depth_ordering() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g0 = make_geom(0, 0);
    TestGeom g1 = make_geom(1, 0);
    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 1, 0, 0.5f);
    MatSpec m3 = mat(0, 0, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 16);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    // Two submissions with identical cages: ties broken by key,
    // regardless of the submission order (30 added before 20).
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 30, mat_const, &m1, 0, cage, &g0.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_add_deep(s.b, s.fb, 20, mat_const, &m2, 0, cage, &g0.base, 0, rect_effects(0, 1.0f));
    // A level-1 octree: 8 leaves cover the center, ties broken
    // by node box level/x/y/z.
    box_cage(cage, -0.9f, -0.9f, 4.0f, 0.9f, 0.9f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 7, mat_const, &m3, 0, cage, &g1.base, 0, rect_effects(1, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 10);
    const DMirLayer* L[10];
    for (int i = 0; i < 10; i++) {
        L[i] = dmir_deep_get(s.fb, 8, 8, (uint32_t)i);
        TASK_CHECK(L[i] != 0);
    }
    TASK_CHECK(L[0]->submission_key == 20);
    TASK_CHECK(L[1]->submission_key == 30);
    TASK_CHECK(L[0]->depth == L[1]->depth);

    // Key 7's eight leaves: 4 front (z=0) then 4 back (z=1),
    // each group ordered by x, then y.
    static const uint32_t ex[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    static const uint32_t ey[8] = {0, 1, 0, 1, 0, 1, 0, 1};
    static const uint32_t ez[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    static const uint64_t ed[8] = {1, 3, 2, 4, 5, 7, 6, 8};
    for (int i = 0; i < 8; i++) {
        TASK_CHECK(L[2 + i]->submission_key == 7);
        TASK_CHECK(L[2 + i]->node_box.level == 1);
        TASK_CHECK(L[2 + i]->node_box.x == ex[i]);
        TASK_CHECK(L[2 + i]->node_box.y == ey[i]);
        TASK_CHECK(L[2 + i]->node_box.z == ez[i]);
        TASK_CHECK(L[2 + i]->data_ref == ed[i]);
    }
    TASK_CHECK(L[2]->depth == L[5]->depth);
    TASK_CHECK(L[6]->depth == L[9]->depth);
    TASK_CHECK(L[2]->depth < L[6]->depth);

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 16) == 0);
    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_shared_data_ref_distinct_nodes() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(1, 1); // all nodes share data_ref 777
    MatSpec m = mat(0.8f, 0.4f, 0.2f, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 16);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    // The same geometry submitted twice: identical data_refs and
    // identical spatial nodes, but different submission keys, so
    // nothing may be merged.
    dmir_batcher_add_deep(s.b, s.fb, 5, mat_const, &m, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    dmir_batcher_add_deep(s.b, s.fb, 6, mat_const, &m, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 16);
    // Order: key5 front x4, key6 front x4, key5 back x4, key6
    // back x4 (depth groups, then key, then box x/y/z).
    static const uint32_t ex[4] = {0, 0, 1, 1};
    static const uint32_t ey[4] = {0, 1, 0, 1};
    for (int i = 0; i < 16; i++) {
        const DMirLayer* L = dmir_deep_get(s.fb, 8, 8, (uint32_t)i);
        TASK_CHECK(L != 0);
        // shared data_ref does not merge distinct spatial nodes
        TASK_CHECK(L->data_ref == 777);
        TASK_CHECK(L->submission_key == ((i < 4 || (i >= 8 && i < 12)) ? 5 : 6));
        TASK_CHECK(L->node_box.level == 1);
        TASK_CHECK(L->node_box.x == ex[i % 4]);
        TASK_CHECK(L->node_box.y == ey[i % 4]);
        TASK_CHECK(L->node_box.z == (i < 8 ? 0u : 1u));
    }

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_capacity_truncation() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec mats[5];
    for (int i = 0; i < 5; i++) {
        mats[i] = mat(0.1f * (i + 1), 0.0f, 1.0f - 0.1f * i, 0.5f);
    }
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 3); // K = 3, five stacked layers
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    for (int i = 0; i < 5; i++) {
        float z = 2.0f + (float)i;
        box_cage(cage, -0.9f, -0.9f, z, 0.9f, 0.9f, z + 0.4f);
        dmir_batcher_add_deep(s.b, s.fb, (uint64_t)(11 + i), mat_const, &mats[i],
            0, cage, &g.base, 0, rect_effects(0, 1.0f));
    }
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // Only the K nearest layers are kept; the rest is dropped.
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 3);
    for (int i = 0; i < 3; i++) {
        const DMirLayer* L = dmir_deep_get(s.fb, 8, 8, (uint32_t)i);
        TASK_CHECK(L != 0);
        TASK_CHECK(L->submission_key == (uint64_t)(11 + i));
        if (i > 0) {
            const DMirLayer* P = dmir_deep_get(s.fb, 8, 8, (uint32_t)(i - 1));
            TASK_CHECK(P->depth < L->depth);
        }
    }

    // Dropped far layers are treated as air by the compositing.
    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out[3];
    float t = dmir_deep_composite(s.fb, 8, 8, bg, out);
    TASK_CHECK(approx_eq(t, 0.125f)); // (1-0.5)^3
    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);
    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 3) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_composite_independent() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec m1 = mat(1, 0, 0, 0.25f);
    MatSpec m2 = mat(0, 1, 0, 0.5f);
    MatSpec m3 = mat(0, 0, 1, 1.0f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.4f, -0.4f, 2.0f, 0.4f, 0.4f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.4f, -0.4f, 3.0f, 0.4f, 0.4f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.4f, -0.4f, 4.0f, 0.4f, 0.4f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 3, mat_const, &m3, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // A pixel outside the projections: no layers, pure background.
    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out[3];
    TASK_CHECK(dmir_deep_count(s.fb, 0, 0) == 0);
    float t = dmir_deep_composite(s.fb, 0, 0, bg, out);
    TASK_CHECK(t == 1.0f);
    TASK_CHECK(out[0] == bg[0] && out[1] == bg[1] && out[2] == bg[2]);

    // Center: C = 0.25*red + 0.75*0.5*green + 0.75*0.5*1*blue, T = 0.
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 3);
    t = dmir_deep_composite(s.fb, 8, 8, bg, out);
    TASK_CHECK(approx_eq(t, 0.0f));
    TASK_CHECK(approx_eq(out[0], 0.25f));
    TASK_CHECK(approx_eq(out[1], 0.375f));
    TASK_CHECK(approx_eq(out[2], 0.375f));

    // Repeating the compositing must not change the stored layers.
    Dump before = dump_capture(s.fb, 16, 16, 8);
    float out2[3];
    float t2 = dmir_deep_composite(s.fb, 8, 8, bg, out2);
    TASK_CHECK(approx_eq(t, t2));
    TASK_CHECK(out[0] == out2[0] && out[1] == out2[1] && out[2] == out2[2]);
    Dump after = dump_capture(s.fb, 16, 16, 8);
    TASK_CHECK(dumps_equal(before, after) == 0);

    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

// ---------------------------------------------------------------
// traversal group
// ---------------------------------------------------------------

int case_non_affine_cage() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(1, 0);
    MatSpec m1 = mat(1, 0, 0, 0.6f);
    MatSpec m2 = mat(0, 0, 1, 0.6f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 16);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    // Frustum-shaped (non-affine) cage: small front, big back.
    for (int i = 0; i < 8; i++) {
        float e = (i & 4) ? 0.9f : 0.3f;
        cage[i*3+0] = (i & 1) ? e : -e;
        cage[i*3+1] = (i & 2) ? e : -e;
        cage[i*3+2] = (i & 4) ? 3.0f : 2.0f;
    }
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 4.0f, 0.9f, 0.9f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // The deformed cage really went through cage subdivision.
    TASK_CHECK(s.fb->stats[DMIR_NODES_BATCHED] > 8);
    TASK_CHECK(s.fb->stats[DMIR_NODES_CAGE] > 0);
    TASK_CHECK(s.fb->stats[DMIR_NODES_ORTHO] > 0);

    uint32_t n = dmir_deep_count(s.fb, 8, 8);
    TASK_CHECK(n >= 2);
    const DMirLayer* first = dmir_deep_get(s.fb, 8, 8, 0);
    const DMirLayer* last = dmir_deep_get(s.fb, 8, 8, n - 1);
    TASK_CHECK(first != 0 && last != 0);
    TASK_CHECK(first->submission_key == 1);
    TASK_CHECK(last->submission_key == 2);
    // Node boxes are carried down through the cage splits.
    bool saw_level1 = false;
    for (uint32_t i = 0; i < n; i++) {
        const DMirLayer* L = dmir_deep_get(s.fb, 8, 8, i);
        TASK_CHECK(L != 0);
        if (L->submission_key == 1) {
            TASK_CHECK(L->node_box.level == 1);
            saw_level1 = true;
        }
    }
    TASK_CHECK(saw_level1);

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 16) == 0);
    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_perspective_and_ortho() {
    dmir_lookups_initialize();
    for (int mode = 0; mode < 2; mode++) {
        Scene s = scene_make(16, 16);
        TestGeom g = make_geom(0, 0);
        MatSpec m1 = mat(1, 0, 0, 0.5f);
        MatSpec m2 = mat(0, 1, 0, 0.5f);
        DMirFrustum fr = frustum_ortho();
        fr.perspective = mode ? 1.0f : 0.0f;
        float cage[24];

        dmir_framebuffer_clear(s.fb);
        dmir_deep_begin(s.fb, 4);
        dmir_batcher_reset(s.b, viewport_of(s), fr);
        box_cage(cage, -0.7f, -0.7f, 2.0f, 0.7f, 0.7f, 2.5f);
        dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
        box_cage(cage, -0.7f, -0.7f, 4.0f, 0.7f, 0.7f, 4.5f);
        dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
        dmir_batcher_sort(s.b);
        dmir_renderer_draw(s.r);

        TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 2);
        const DMirLayer* L0 = dmir_deep_get(s.fb, 8, 8, 0);
        const DMirLayer* L1 = dmir_deep_get(s.fb, 8, 8, 1);
        TASK_CHECK(L0 != 0 && L1 != 0);
        TASK_CHECK(L0->submission_key == 1);
        TASK_CHECK(L1->submission_key == 2);
        TASK_CHECK(L0->depth < L1->depth);
        TASK_CHECK(L0->r == 1.0f && L1->g == 1.0f);
        TASK_CHECK(check_all_sorted(s.fb, 16, 16, 4) == 0);
        TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

        dmir_deep_end(s.fb);
        scene_free(s);
    }
    return 0;
}

int case_small_nodes_back_layers() {
    dmir_lookups_initialize();
    Scene s = scene_make(32, 32);
    TestGeom g = make_geom(2, 0); // 2x2x2 px leaves: map territory in opaque mode
    MatSpec m1 = mat(1, 0.2f, 0, 0.5f);
    MatSpec m2 = mat(0, 0.2f, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 32);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.25f, -0.25f, 2.0f, 0.25f, 0.25f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(2, 0.5f));
    box_cage(cage, -0.25f, -0.25f, 3.0f, 0.25f, 0.25f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(2, 0.5f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // Back layers survive: some pixel holds both submissions,
    // and multiple layers from the same submission exist.
    bool both = false;
    uint32_t max_count = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            uint32_t n = dmir_deep_count(s.fb, x, y);
            if (n > max_count) max_count = n;
            bool has1 = false, has2 = false;
            for (uint32_t i = 0; i < n; i++) {
                const DMirLayer* L = dmir_deep_get(s.fb, x, y, i);
                TASK_CHECK(L != 0);
                if (L->submission_key == 1) has1 = true;
                if (L->submission_key == 2) has2 = true;
            }
            if (has1 && has2) both = true;
        }
    }
    TASK_CHECK(both);
    TASK_CHECK(max_count >= 2);

    TASK_CHECK(check_all_sorted(s.fb, 32, 32, 32) == 0);
    TASK_CHECK(check_composite_matches(s.fb, 32, 32) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_eval_mode_capture() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    EvalGeom g = make_eval_geom(3);
    MatSpec m1 = mat(1, 0.5f, 0, 0.5f);
    MatSpec m2 = mat(0, 0.5f, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 16);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.8f, -0.8f, 2.0f, 0.8f, 0.8f, 2.8f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(3, 0.5f));
    box_cage(cage, -0.8f, -0.8f, 3.5f, 0.8f, 0.8f, 4.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(3, 0.5f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // The procedural (evaluate) chain captures layers too: both
    // submissions show up behind the same pixels, with node boxes
    // carried down to the splat level.
    bool both = false;
    bool saw_level3 = false;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            uint32_t n = dmir_deep_count(s.fb, x, y);
            bool has1 = false, has2 = false;
            for (uint32_t i = 0; i < n; i++) {
                const DMirLayer* L = dmir_deep_get(s.fb, x, y, i);
                TASK_CHECK(L != 0);
                TASK_CHECK(L->node_box.level == 3);
                TASK_CHECK(L->data_ref ==
                    1 + L->node_box.x + ((uint64_t)L->node_box.y << 8) +
                    ((uint64_t)L->node_box.z << 16));
                if (L->submission_key == 1) has1 = true;
                if (L->submission_key == 2) has2 = true;
                saw_level3 = true;
            }
            if (has1 && has2) both = true;
        }
    }
    TASK_CHECK(both);
    TASK_CHECK(saw_level3);

    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 16) == 0);
    TASK_CHECK(check_composite_matches(s.fb, 16, 16) == 0);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_repeat_draw_dedup() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(1, 0);
    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 1, 0, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 16);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 3.0f, 0.9f, 0.9f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
    dmir_batcher_sort(s.b);

    dmir_renderer_draw(s.r);
    Dump first = dump_capture(s.fb, 16, 16, 16);
    // Consecutive draws accumulate into the same capture, but the
    // same node covering the same pixel again must not deepen the
    // colors: the second draw is a no-op.
    dmir_renderer_draw(s.r);
    Dump second = dump_capture(s.fb, 16, 16, 16);
    TASK_CHECK(dumps_equal(first, second) == 0);

    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out1[3], out2[3];
    float t1 = dmir_deep_composite(s.fb, 8, 8, bg, out1);
    float t2 = dmir_deep_composite(s.fb, 8, 8, bg, out2);
    TASK_CHECK(t1 == t2);
    TASK_CHECK(out1[0] == out2[0] && out1[1] == out2[1] && out1[2] == out2[2]);

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_rect_split_consistent() {
    dmir_lookups_initialize();
    Scene a = scene_make(16, 16);
    Scene b = scene_make(16, 16);
    TestGeom g = make_geom(1, 0);
    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 0, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();

    for (int pass = 0; pass < 2; pass++) {
        Scene& s = pass ? b : a;
        float cage[24];
        dmir_framebuffer_clear(s.fb);
        dmir_deep_begin(s.fb, 16);
        dmir_batcher_reset(s.b, viewport_of(s), fr);
        box_cage(cage, -0.95f, -0.95f, 2.0f, 0.95f, 0.95f, 2.5f);
        dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
        box_cage(cage, -0.95f, -0.95f, 3.0f, 0.95f, 0.95f, 3.5f);
        dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(1, 1.0f));
        dmir_batcher_sort(s.b);
    }

    // Run A: the whole viewport in one draw.
    a.r->rect.min_x = 0; a.r->rect.min_y = 0;
    a.r->rect.max_x = 15; a.r->rect.max_y = 15;
    dmir_renderer_draw(a.r);

    // No seams: every pixel got at least one layer.
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            TASK_CHECK(dmir_deep_count(a.fb, x, y) > 0);
        }
    }

    // Run B: the renderer rect split into two non-overlapping
    // strips, drawn as two accumulating draws.
    b.r->rect.min_x = 0; b.r->rect.min_y = 0;
    b.r->rect.max_x = 7; b.r->rect.max_y = 15;
    dmir_renderer_draw(b.r);
    b.r->rect.min_x = 8; b.r->rect.min_y = 0;
    b.r->rect.max_x = 15; b.r->rect.max_y = 15;
    dmir_renderer_draw(b.r);

    Dump da = dump_capture(a.fb, 16, 16, 16);
    Dump db = dump_capture(b.fb, 16, 16, 16);
    TASK_CHECK(dumps_equal(da, db) == 0);

    float bg[3] = {0.1f, 0.2f, 0.3f};
    for (int p = 0; p < 16; p += 5) {
        float oa[3], ob[3];
        float ta = dmir_deep_composite(a.fb, p, 8, bg, oa);
        float tb = dmir_deep_composite(b.fb, p, 8, bg, ob);
        TASK_CHECK(ta == tb);
        TASK_CHECK(oa[0] == ob[0] && oa[1] == ob[1] && oa[2] == ob[2]);
    }

    dmir_deep_end(a.fb);
    dmir_deep_end(b.fb);
    scene_free(a);
    scene_free(b);
    return 0;
}

// ---------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------

int voxels_equal(DMirFramebuffer* fb, const std::vector<DMirVoxelRef>& saved, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = dmir_pixel_index(fb, x, y);
            const DMirVoxelRef& a = fb->voxel[i];
            const DMirVoxelRef& b = saved[(size_t)y * w + x];
            if (a.affine_id != b.affine_id) return 0;
            if (a.voxel_id != b.voxel_id) return 0;
        }
    }
    return 1;
}

int case_opaque_buffers_preserved() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(1, 0);
    TestGeom g0 = make_geom(0, 0);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    // Opaque pre-pass through the old entry.
    dmir_framebuffer_clear(s.fb);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add(s.b, s.fb, 5, cage, &g.base, 0, rect_effects(1, 0.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    std::vector<DMirDepth> depth0((size_t)16 * 16);
    std::vector<DMirVoxelRef> voxel0((size_t)16 * 16);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = dmir_pixel_index(s.fb, x, y);
            depth0[(size_t)y * 16 + x] = s.fb->depth[i];
            voxel0[(size_t)y * 16 + x] = s.fb->voxel[i];
        }
    }

    // Capture episode: one layer in front of, one behind the
    // opaque content.
    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 0, 1, 0.5f);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 1.0f, 0.9f, 0.9f, 1.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g0.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 3.0f, 0.9f, 0.9f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g0.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    dmir_deep_end(s.fb);

    // The capture respected the old depth...
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 1);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->submission_key == 1);

    // ...and did not modify the old depth/voxel contents.
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = dmir_pixel_index(s.fb, x, y);
            TASK_CHECK(s.fb->depth[i] == depth0[(size_t)y * 16 + x]);
        }
    }
    TASK_CHECK(voxels_equal(s.fb, voxel0, 16, 16));

    // After the capture, an ordinary opaque redraw produces the
    // same buffers as if the capture had never happened.
    dmir_framebuffer_clear(s.fb);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add(s.b, s.fb, 5, cage, &g.base, 0, rect_effects(1, 0.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int i = dmir_pixel_index(s.fb, x, y);
            TASK_CHECK(s.fb->depth[i] == depth0[(size_t)y * 16 + x]);
        }
    }
    TASK_CHECK(voxels_equal(s.fb, voxel0, 16, 16));

    scene_free(s);
    return 0;
}

int case_old_depth_occlusion() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    // Opaque pre-pass covering the left half at z in [2, 2.5].
    dmir_framebuffer_clear(s.fb);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.0f, 0.9f, 2.5f);
    dmir_batcher_add(s.b, s.fb, 9, cage, &g.base, 0, rect_effects(0, 0.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(s.fb->depth[dmir_pixel_index(s.fb, 4, 8)] == dmir_z_to_depth(s.b, 2.25f));

    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 1, 0, 0.5f);
    MatSpec m3 = mat(0, 0, 1, 0.5f);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 1.0f, 0.9f, 0.9f, 1.5f); // in front
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f); // equal depth
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    box_cage(cage, -0.9f, -0.9f, 3.0f, 0.9f, 0.9f, 3.5f); // behind
    dmir_batcher_add_deep(s.b, s.fb, 3, mat_const, &m3, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // Left (opaque-covered) pixel: only the strictly nearer
    // sample is captured; equal and farther depths are rejected.
    TASK_CHECK(dmir_deep_count(s.fb, 4, 8) == 1);
    const DMirLayer* L = dmir_deep_get(s.fb, 4, 8, 0);
    TASK_CHECK(L != 0);
    TASK_CHECK(L->submission_key == 1);
    TASK_CHECK(L->depth == dmir_z_to_depth(s.b, 1.25f));

    // Right (empty old depth) pixel: no opaque occlusion at all.
    TASK_CHECK(dmir_deep_count(s.fb, 12, 8) == 3);
    for (uint32_t i = 0; i < 3; i++) {
        const DMirLayer* Li = dmir_deep_get(s.fb, 12, 8, i);
        TASK_CHECK(Li != 0);
        TASK_CHECK(Li->submission_key == i + 1);
    }

    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_clear_resize_discard() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec m = mat(1, 0, 0, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];
    float bg[3] = {0.1f, 0.2f, 0.3f};
    float out[3];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 4);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) > 0);

    // clear discards the capture
    dmir_framebuffer_clear(s.fb);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 0);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0) == 0);
    float t = dmir_deep_composite(s.fb, 8, 8, bg, out);
    TASK_CHECK(t == 1.0f);
    TASK_CHECK(out[0] == bg[0] && out[1] == bg[1] && out[2] == bg[2]);

    // capture again, then resize discards it
    dmir_deep_begin(s.fb, 4);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) > 0);
    dmir_framebuffer_resize(s.fb, 8, 8);
    TASK_CHECK(dmir_deep_count(s.fb, 4, 4) == 0);
    TASK_CHECK(dmir_deep_get(s.fb, 4, 4, 0) == 0);

    scene_free(s);
    return 0;
}

int case_rebegin_accumulate() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec m1 = mat(1, 0, 0, 0.5f);
    MatSpec m2 = mat(0, 1, 0, 0.5f);
    MatSpec m3 = mat(0, 0, 1, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 4);
    // First batch + draw.
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 1, mat_const, &m1, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    // Second batch + draw: accumulates into the same capture.
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 3.0f, 0.9f, 0.9f, 3.5f);
    dmir_batcher_add_deep(s.b, s.fb, 2, mat_const, &m2, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 2);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->submission_key == 1);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 1)->submission_key == 2);
    dmir_deep_end(s.fb);

    // Re-beginning resets the captured content.
    dmir_deep_begin(s.fb, 4);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 0);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add_deep(s.b, s.fb, 3, mat_const, &m3, 0, cage, &g.base, 0, rect_effects(0, 1.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 1);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->submission_key == 3);
    dmir_deep_end(s.fb);

    // Ordinary opaque rendering still works afterwards.
    dmir_framebuffer_clear(s.fb);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    box_cage(cage, -0.9f, -0.9f, 2.0f, 0.9f, 0.9f, 2.5f);
    dmir_batcher_add(s.b, s.fb, 4, cage, &g.base, 0, rect_effects(0, 0.0f));
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(s.fb->depth[dmir_pixel_index(s.fb, 8, 8)] < DMIR_MAX_DEPTH);

    scene_free(s);
    return 0;
}

int run_stacked_scene(const int* add_order, Dump* out) {
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec mats[3];
    mats[0] = mat(1, 0, 0, 0.5f); // key 1, z [2,2.5]
    mats[1] = mat(0, 1, 0, 0.5f); // key 2, z [3,3.5]
    mats[2] = mat(0, 0, 1, 0.5f); // key 3, z [2,2.5] (same as key 1)
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 8);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    for (int i = 0; i < 3; i++) {
        int k = add_order[i];
        float z = (k == 2) ? 3.0f : 2.0f;
        box_cage(cage, -0.9f, -0.9f, z, 0.9f, 0.9f, z + 0.5f);
        dmir_batcher_add_deep(s.b, s.fb, (uint64_t)k, mat_const, &mats[k - 1],
            0, cage, &g.base, 0, rect_effects(0, 1.0f));
    }
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);

    // Same-depth ties are broken by the submission key.
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 3);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->submission_key == 1);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 1)->submission_key == 3);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 2)->submission_key == 2);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->depth ==
               dmir_deep_get(s.fb, 8, 8, 1)->depth);

    *out = dump_capture(s.fb, 16, 16, 8);
    dmir_deep_end(s.fb);
    scene_free(s);
    return 0;
}

int case_order_independence() {
    dmir_lookups_initialize();
    int order_a[3] = {1, 2, 3};
    int order_b[3] = {3, 2, 1};
    Dump da, db;
    TASK_CHECK(run_stacked_scene(order_a, &da) == 0);
    TASK_CHECK(run_stacked_scene(order_b, &db) == 0);
    TASK_CHECK(dumps_equal(da, db) == 0);
    return 0;
}

int case_capacity_clamp() {
    dmir_lookups_initialize();
    Scene s = scene_make(16, 16);
    TestGeom g = make_geom(0, 0);
    MatSpec m = mat(1, 0, 0, 0.5f);
    DMirFrustum fr = frustum_ortho();
    float cage[24];

    // Capacity below 1 clamps to 1.
    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 0);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    for (int i = 0; i < 3; i++) {
        float z = 2.0f + (float)i;
        box_cage(cage, -0.9f, -0.9f, z, 0.9f, 0.9f, z + 0.4f);
        dmir_batcher_add_deep(s.b, s.fb, (uint64_t)(10 + i), mat_const, &m,
            0, cage, &g.base, 0, rect_effects(0, 1.0f));
    }
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 1);
    TASK_CHECK(dmir_deep_get(s.fb, 8, 8, 0)->submission_key == 10);
    dmir_deep_end(s.fb);

    // Capacity above 32 clamps to 32.
    dmir_framebuffer_clear(s.fb);
    dmir_deep_begin(s.fb, 100);
    dmir_batcher_reset(s.b, viewport_of(s), fr);
    for (int i = 0; i < 40; i++) {
        float z = 2.0f + 0.5f * i;
        box_cage(cage, -0.9f, -0.9f, z, 0.9f, 0.9f, z + 0.4f);
        dmir_batcher_add_deep(s.b, s.fb, (uint64_t)(i + 1), mat_const, &m,
            0, cage, &g.base, 0, rect_effects(0, 1.0f));
    }
    dmir_batcher_sort(s.b);
    dmir_renderer_draw(s.r);
    TASK_CHECK(dmir_deep_count(s.fb, 8, 8) == 32);
    for (uint32_t i = 0; i < 32; i++) {
        const DMirLayer* L = dmir_deep_get(s.fb, 8, 8, i);
        TASK_CHECK(L != 0);
        TASK_CHECK(L->submission_key == i + 1);
    }
    TASK_CHECK(check_all_sorted(s.fb, 16, 16, 32) == 0);
    dmir_deep_end(s.fb);

    scene_free(s);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"layers", "two_layer_stack", case_two_layer_stack},
    {"layers", "zero_and_full_alpha", case_zero_and_full_alpha},
    {"layers", "same_depth_ordering", case_same_depth_ordering},
    {"layers", "shared_data_ref_distinct_nodes", case_shared_data_ref_distinct_nodes},
    {"layers", "capacity_truncation", case_capacity_truncation},
    {"layers", "composite_independent", case_composite_independent},
    {"traversal", "non_affine_cage", case_non_affine_cage},
    {"traversal", "perspective_and_ortho", case_perspective_and_ortho},
    {"traversal", "small_nodes_back_layers", case_small_nodes_back_layers},
    {"traversal", "eval_mode_capture", case_eval_mode_capture},
    {"traversal", "repeat_draw_dedup", case_repeat_draw_dedup},
    {"traversal", "rect_split_consistent", case_rect_split_consistent},
    {"lifecycle", "opaque_buffers_preserved", case_opaque_buffers_preserved},
    {"lifecycle", "old_depth_occlusion", case_old_depth_occlusion},
    {"lifecycle", "clear_resize_discard", case_clear_resize_discard},
    {"lifecycle", "rebegin_accumulate", case_rebegin_accumulate},
    {"lifecycle", "order_independence", case_order_independence},
    {"lifecycle", "capacity_clamp", case_capacity_clamp},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
