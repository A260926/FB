// Headless deep-capture example: two overlapping translucent
// voxel slabs are captured as per-pixel layers and composited
// over a background, then written to a binary PPM (P6) file.
// No window or GPU is used. Usage: deep_headless <output.ppm>

#include <discrete_mirage.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A full octree of a fixed depth; node references use heap
// indexing, and the voxel data reference is the node index
// (used to vary the color per voxel).
struct DemoGeom {
    DMirGeometry base;
    int levels;
};

int node_level(uint64_t n) {
    int level = 0;
    uint64_t start = 0, size = 1;
    while (n >= start + size) { start += size; size *= 8; level++; }
    return level;
}

DMirBool geom_start(const void* g, DMirAddress, DMirAddress, DMirTraversal* t) {
    const DemoGeom* dg = (const DemoGeom*)g;
    t->mask[0] = (dg->levels > 0) ? 255 : 0;
    t->node[0] = 0;
    t->data[0] = 0;
    return 1;
}

DMirBool geom_next(const void* g, DMirTraversal* src, int32_t index, DMirTraversal* dst) {
    const DemoGeom* dg = (const DemoGeom*)g;
    uint64_t parent = src->node[index];
    int pl = node_level(parent);
    for (int o = 0; o < 8; o++) {
        uint64_t child = parent * 8 + (uint64_t)o + 1;
        dst->node[o] = child;
        dst->mask[o] = (pl + 1 < dg->levels) ? 255 : 0;
        dst->data[o] = child;
    }
    return 1;
}

// Per-submission material: a base color modulated by the voxel
// index so that individual voxels are distinguishable.
struct Material {
    float rgba[4];
};

void material_cb(void* user, DMirAddress data_ref, float* rgba) {
    const Material* m = (const Material*)user;
    float shade = 0.7f + 0.3f * (float)((data_ref * 2654435761u >> 8) % 101) / 100.0f;
    rgba[0] = m->rgba[0] * shade;
    rgba[1] = m->rgba[1] * shade;
    rgba[2] = m->rgba[2] * shade;
    rgba[3] = m->rgba[3];
}

void box_cage(float* c, float x0, float y0, float z0, float x1, float y1, float z1) {
    for (int i = 0; i < 8; i++) {
        c[i*3+0] = (i & 1) ? x1 : x0;
        c[i*3+1] = (i & 2) ? y1 : y0;
        c[i*3+2] = (i & 4) ? z1 : z0;
    }
}

unsigned char to_byte(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    // simple gamma encoding for a more natural look
    return (unsigned char)(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
        return 2;
    }

    const int W = 160;
    const int H = 120;

    dmir_lookups_initialize();

    DMirFramebuffer* fb = dmir_framebuffer_make(W, H);
    DMirBatcher* batcher = dmir_batcher_make();
    DMirRenderer* renderer = dmir_renderer_make();
    if (!fb || !batcher || !renderer) {
        std::fprintf(stderr, "allocation failed\n");
        return 1;
    }

    DemoGeom geom = {{geom_start, geom_next, 0, -1}, 3};

    DMirRect viewport = {0, 0, W - 1, H - 1};
    DMirFrustum frustum = DMIR_FRUSTUM_DEFAULT;
    frustum.min_depth = 0.1f;
    frustum.max_depth = 50.0f;
    frustum.focal_extent = 1.0f;
    frustum.focal_depth = 1.5f;
    frustum.perspective = 0.6f;

    Material warm = {{0.95f, 0.25f, 0.10f, 0.55f}};
    Material cold = {{0.10f, 0.45f, 0.95f, 0.45f}};

    DMirEffects effects;
    effects.max_level = 3;
    effects.dilation_abs = 1.0f;
    effects.dilation_rel = 0.0f;
    effects.shape = DMIR_SHAPE_RECT;

    dmir_framebuffer_clear(fb);

    // Capture up to 8 layers per pixel; two translucent slabs
    // are submitted with unique keys and composited afterwards.
    dmir_deep_begin(fb, 8);

    dmir_batcher_reset(batcher, viewport, frustum);

    float cage[24];
    box_cage(cage, -0.85f, -0.55f, 2.0f, 0.35f, 0.65f, 2.8f);
    dmir_batcher_add_deep(batcher, fb, 101, material_cb, &warm,
        1, cage, &geom.base, 0, effects);
    box_cage(cage, -0.35f, -0.7f, 3.2f, 0.85f, 0.5f, 4.2f);
    dmir_batcher_add_deep(batcher, fb, 202, material_cb, &cold,
        1, cage, &geom.base, 0, effects);

    dmir_batcher_sort(batcher);

    renderer->framebuffer = fb;
    renderer->batcher = batcher;
    renderer->rect = viewport;
    dmir_renderer_draw(renderer);

    // Composite the captured layers over a dark background and
    // write the image.
    FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", W, H);

    float background[3] = {0.06f, 0.07f, 0.09f};
    unsigned layers_total = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            layers_total += dmir_deep_count(fb, x, y);
            float rgb[3];
            dmir_deep_composite(fb, x, y, background, rgb);
            std::fputc(to_byte(rgb[0]), out);
            std::fputc(to_byte(rgb[1]), out);
            std::fputc(to_byte(rgb[2]), out);
        }
    }
    std::fclose(out);

    dmir_deep_end(fb);

    std::printf("deep_headless: %dx%d, %u layers captured, wrote %s\n",
        W, H, layers_total, argv[1]);

    dmir_renderer_free(renderer);
    dmir_batcher_free(batcher);
    dmir_framebuffer_free(fb);
    return 0;
}
