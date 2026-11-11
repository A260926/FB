// Behaviour test for the in-place blit contract.
//
// Contract: when src == dest, every source pixel is read at the value it
// held when the call started, i.e. the result is identical to saving a
// source snapshot first and blitting it back through the same public entry
// point from an independent bitmap. Destination pixels still take part in
// blending exactly as they do for distinct bitmaps.
//
// This file is compiled twice under TIGR_HEADLESS (see check_overlap.sh):
// once against the release amalgamation tigr.c, once against the modular
// maintenance source src/tigr_bitmaps.c. Both runs execute these assertions.

#include "tigr.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);    \
            failures++;                                                \
        }                                                              \
    } while (0)

typedef enum {
    OP_BLIT,
    OP_ALPHA,
    OP_TINT
} OpKind;

typedef struct {
    OpKind kind;
    float alpha;
    TPixel tint;
} Op;

static const Op OP_B = { OP_BLIT, 0.0f, { 0, 0, 0, 0 } };

static void doOp(Tigr *dst, Tigr *src, const Op *op,
                 int dx, int dy, int sx, int sy, int w, int h) {
    switch (op->kind) {
    case OP_BLIT:
        tigrBlit(dst, src, dx, dy, sx, sy, w, h);
        break;
    case OP_ALPHA:
        tigrBlitAlpha(dst, src, dx, dy, sx, sy, w, h, op->alpha);
        break;
    case OP_TINT:
        tigrBlitTint(dst, src, dx, dy, sx, sy, w, h, op->tint);
        break;
    }
}

// Reference implementation of the contract: copy the whole input bitmap,
// then run the unchanged public entry point with that independent snapshot
// as the source.
static void refOp(Tigr *dst, const Op *op,
                  int dx, int dy, int sx, int sy, int w, int h) {
    Tigr *snap = tigrBitmap(dst->w, dst->h);
    memcpy(snap->pix, dst->pix, (size_t)dst->w * dst->h * sizeof(TPixel));
    doOp(dst, snap, op, dx, dy, sx, sy, w, h);
    tigrFree(snap);
}

static Tigr *cloneBitmap(const Tigr *b) {
    Tigr *out = tigrBitmap(b->w, b->h);
    memcpy(out->pix, b->pix, (size_t)b->w * b->h * sizeof(TPixel));
    tigrBlitMode(out, b->blitMode);
    return out;
}

static int samePixels(const Tigr *a, const Tigr *b) {
    return a->w == b->w && a->h == b->h &&
           memcmp(a->pix, b->pix, (size_t)a->w * a->h * sizeof(TPixel)) == 0;
}

// Runs the operation in-place on one copy and via the independent-snapshot
// reference on another, and requires identical results.
static void runCase(const char *name, const Tigr *input, const int clip[4],
                    const Op *op, int mode,
                    int dx, int dy, int sx, int sy, int w, int h) {
    Tigr *actual = cloneBitmap(input);
    Tigr *expect = cloneBitmap(input);

    if (clip) {
        tigrClip(actual, clip[0], clip[1], clip[2], clip[3]);
        tigrClip(expect, clip[0], clip[1], clip[2], clip[3]);
    }
    tigrBlitMode(actual, mode);
    tigrBlitMode(expect, mode);

    doOp(actual, actual, op, dx, dy, sx, sy, w, h);
    refOp(expect, op, dx, dy, sx, sy, w, h);

    if (!samePixels(actual, expect)) {
        printf("FAIL %s: in-place result differs from snapshot reference\n", name);
        failures++;
        for (int y = 0; y < input->h; y++) {
            for (int x = 0; x < input->w; x++) {
                TPixel p = actual->pix[y * input->w + x];
                TPixel q = expect->pix[y * input->w + x];
                if (memcmp(&p, &q, sizeof(TPixel)) != 0)
                    printf("  at (%d,%d): got %u,%u,%u,%u want %u,%u,%u,%u\n",
                           x, y, p.r, p.g, p.b, p.a, q.r, q.g, q.b, q.a);
            }
        }
    }

    tigrFree(actual);
    tigrFree(expect);
}

// Non-uniform RGBA pattern with semi-transparent and opaque pixels.
static void fillPattern(Tigr *b, int seed) {
    for (int y = 0; y < b->h; y++) {
        for (int x = 0; x < b->w; x++) {
            TPixel p;
            p.r = (unsigned char)(20 + seed * 7 + x * 37 + y * 53);
            p.g = (unsigned char)(90 + seed * 11 + x * 17 - y * 11);
            p.b = (unsigned char)(150 + seed * 13 + x * 23 + y * 29);
            p.a = (unsigned char)(30 + x * 55 + y * 45);
            b->pix[y * b->w + x] = p;
        }
    }
    // Guarantee at least one fully opaque source pixel.
    b->pix[0].a = 255;
}

// The exact scrolling example from the contract: 2x4, opaque rows with red
// channels 10, 20, 30, 40.
static Tigr *rowBitmap(void) {
    Tigr *b = tigrBitmap(2, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 2; x++)
            b->pix[y * 2 + x] = tigrRGB((unsigned char)((y + 1) * 10), 7, 3);
    return b;
}

static void testScrollExample(void) {
    // Shift down one row: tigrBlit(b,b,0,1,0,0,2,3) -> 10,10,20,30.
    Tigr *down = rowBitmap();
    tigrBlit(down, down, 0, 1, 0, 0, 2, 3);
    static const unsigned char wantDown[4] = { 10, 10, 20, 30 };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 2; x++)
            CHECK(down->pix[y * 2 + x].r == wantDown[y]);
    }
    tigrFree(down);

    // Shift up one row: tigrBlit(b,b,0,-1,0,0,2,4) -> 20,30,40,40.
    // Clipping turns this into dest rows 0..2 reading source rows 1..3,
    // while the last row is never written and keeps its original value.
    Tigr *up = rowBitmap();
    tigrBlit(up, up, 0, -1, 0, 0, 2, 4);
    static const unsigned char wantUp[4] = { 20, 30, 40, 40 };
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 2; x++)
            CHECK(up->pix[y * 2 + x].r == wantUp[y]);
    }
    tigrFree(up);
}

static void testBlitGeometry(void) {
    Tigr *input4 = tigrBitmap(4, 4);
    fillPattern(input4, 1);
    Tigr *input3 = tigrBitmap(3, 3);
    fillPattern(input3, 2);

    // Vertical, horizontal and diagonal overlap, including rectangles with
    // negative source/dest co-ordinates.
    static const struct {
        int dx, dy, sx, sy, w, h;
    } cases4[] = {
        { 0, 1, 0, 0, 4, 3 },   // scroll down
        { 0, -1, 0, 1, 4, 3 },  // scroll up
        { 1, 0, 0, 0, 3, 4 },   // shift right
        { -1, 0, 1, 0, 3, 4 },  // shift left
        { 1, 1, 0, 0, 3, 3 },   // diagonal down-right
        { -1, -1, 1, 1, 3, 3 }, // diagonal up-left, negative dest
        { -1, 1, 0, 0, 4, 3 },  // target partly off the left edge
        { 1, -1, 0, 1, 3, 4 },  // negative dest y
        { 0, 0, -1, -1, 4, 4 }, // negative source origin
    };
    for (unsigned i = 0; i < sizeof(cases4) / sizeof(cases4[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "blit4[%u]", i);
        runCase(name, input4, 0, &OP_B, TIGR_BLEND_ALPHA,
                cases4[i].dx, cases4[i].dy, cases4[i].sx, cases4[i].sy,
                cases4[i].w, cases4[i].h);
    }
    runCase("blit3-diag", input3, 0, &OP_B, TIGR_BLEND_ALPHA,
            1, 1, 0, 0, 2, 2);

    tigrFree(input4);
    tigrFree(input3);
}

static void testClipping(void) {
    Tigr *input = tigrBitmap(4, 4);
    fillPattern(input, 3);

    // Legal in-bitmap destination clip combined with an overlapping blit.
    static const int clip[4] = { 1, 1, 2, 2 };
    runCase("clipped-overlap", input, clip, &OP_B, TIGR_BLEND_ALPHA,
            1, 1, 0, 0, 3, 3);
    runCase("clipped-shift", input, clip, &OP_B, TIGR_BLEND_ALPHA,
            -1, 0, 1, 0, 3, 4);

    // A clip disjoint from the target rectangle clips the call out entirely:
    // not one byte may be written.
    Tigr *b = cloneBitmap(input);
    Tigr *before = cloneBitmap(input);
    static const int tight[4] = { 0, 0, 1, 1 };
    tigrClip(b, tight[0], tight[1], tight[2], tight[3]);
    tigrBlit(b, b, 2, 2, 0, 0, 2, 2);
    CHECK(samePixels(b, before));
    tigrFree(b);
    tigrFree(before);

    tigrFree(input);
}

static void testBlending(void) {
    Tigr *input4 = tigrBitmap(4, 4);
    fillPattern(input4, 4);
    Tigr *input3 = tigrBitmap(3, 3);
    fillPattern(input3, 5);

    static const int modes[2] = { TIGR_KEEP_ALPHA, TIGR_BLEND_ALPHA };

    Op ops[5];
    ops[0] = (Op){ OP_ALPHA, 0.25f, { 0, 0, 0, 0 } };
    ops[1] = (Op){ OP_ALPHA, 0.5f, { 0, 0, 0, 0 } };
    ops[2] = (Op){ OP_ALPHA, 1.0f, { 0, 0, 0, 0 } };
    ops[3] = (Op){ OP_TINT, 0.0f, { 200, 100, 50, 180 } }; // non-white tint
    ops[4] = (Op){ OP_TINT, 0.0f, { 64, 128, 200, 128 } };

    static const struct {
        int dx, dy, sx, sy, w, h;
    } overlap4[] = {
        { 1, 1, 0, 0, 3, 3 }, // diagonal overlap
        { 1, 0, 0, 0, 3, 4 }, // horizontal overlap
        { 0, 1, 0, 0, 4, 3 }, // vertical overlap
        { -1, 1, 0, 0, 4, 3 } // negative dest offset
    };

    for (int m = 0; m < 2; m++) {
        for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
            for (unsigned c = 0; c < sizeof(overlap4) / sizeof(overlap4[0]); c++) {
                char name[64];
                snprintf(name, sizeof(name), "blend4-m%d-o%u-c%u", m, o, c);
                runCase(name, input4, 0, &ops[o], modes[m],
                        overlap4[c].dx, overlap4[c].dy,
                        overlap4[c].sx, overlap4[c].sy,
                        overlap4[c].w, overlap4[c].h);
            }
        }
    }

    // 3x3 inputs, both modes, including a clipped, offset overlap.
    static const int clip[4] = { 1, 1, 2, 2 };
    for (int m = 0; m < 2; m++) {
        runCase("blend3-diag-keep", input3, 0, &ops[1], modes[m],
                1, 1, 0, 0, 2, 2);
        runCase("blend3-clip", input3, clip, &ops[3], modes[m],
                1, 1, 0, 0, 2, 2);
        runCase("blend3-tint", input3, 0, &ops[4], modes[m],
                -1, 0, 1, 0, 2, 3);
    }

    // Alpha clamping must be preserved: out-of-range alpha behaves like the
    // independent-source implementation.
    Op clampLow = { OP_ALPHA, -1.0f, { 0, 0, 0, 0 } };
    Op clampHigh = { OP_ALPHA, 1.5f, { 0, 0, 0, 0 } };
    runCase("alpha-clamp-low", input4, 0, &clampLow, TIGR_BLEND_ALPHA,
            1, 1, 0, 0, 3, 3);
    runCase("alpha-clamp-high", input4, 0, &clampHigh, TIGR_KEEP_ALPHA,
            1, 1, 0, 0, 3, 3);

    // On a fully opaque bitmap, white opaque tint reduces to plain copy
    // semantics (blend factor is exactly 1), even when done in-place.
    Op whiteOpaque = { OP_TINT, 0.0f, { 255, 255, 255, 255 } };
    Tigr *opaque = tigrBitmap(4, 4);
    fillPattern(opaque, 9);
    for (int i = 0; i < 16; i++)
        opaque->pix[i].a = 255;
    Tigr *a = cloneBitmap(opaque);
    Tigr *b = cloneBitmap(opaque);
    tigrBlit(a, a, 1, 1, 0, 0, 3, 3);
    tigrBlitTint(b, b, 1, 1, 0, 0, 3, 3, whiteOpaque.tint);
    CHECK(samePixels(a, b));
    tigrFree(a);
    tigrFree(b);
    tigrFree(opaque);

    tigrFree(input4);
    tigrFree(input3);
}

static void testUntouchedPaths(void) {
    // Distinct source and destination bitmaps keep their old behaviour.
    Tigr *src = tigrBitmap(4, 4);
    Tigr *dst1 = tigrBitmap(4, 4);
    Tigr *dst2 = tigrBitmap(4, 4);
    fillPattern(src, 6);
    fillPattern(dst1, 7);
    memcpy(dst2->pix, dst1->pix, (size_t)16 * sizeof(TPixel));

    Tigr *snap = cloneBitmap(src);
    tigrBlit(dst1, src, 1, 0, 0, 1, 3, 2);
    tigrBlit(dst2, snap, 1, 0, 0, 1, 3, 2);
    CHECK(samePixels(dst1, dst2));
    tigrFree(snap);

    Op tint = { OP_TINT, 0.0f, { 200, 100, 50, 180 } };
    Tigr *d1 = cloneBitmap(dst1);
    Tigr *d2 = cloneBitmap(dst1);
    Tigr *s2 = cloneBitmap(src);
    tigrBlitTint(d1, src, 0, 1, 1, 0, 2, 3, tint.tint);
    tigrBlitTint(d2, s2, 0, 1, 1, 0, 2, 3, tint.tint);
    CHECK(samePixels(d1, d2));
    tigrFree(d1);
    tigrFree(d2);
    tigrFree(s2);

    // Same bitmap, but source and destination rectangles do not overlap:
    // the direct path must produce the snapshot result and leave the source
    // rectangle untouched.
    Tigr *b = tigrBitmap(4, 4);
    fillPattern(b, 8);
    Tigr *saved = cloneBitmap(b);
    runCase("same-bitmap-disjoint", saved, 0, &OP_B, TIGR_BLEND_ALPHA,
            2, 0, 0, 0, 2, 4);
    tigrBlit(b, b, 2, 0, 0, 0, 2, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 2; x++)
            CHECK(memcmp(&b->pix[y * 4 + x], &saved->pix[y * 4 + x],
                         sizeof(TPixel)) == 0);
    tigrFree(b);
    tigrFree(saved);

    tigrFree(src);
    tigrFree(dst1);
    tigrFree(dst2);
}

int main(void) {
    testScrollExample();
    testBlitGeometry();
    testClipping();
    testBlending();
    testUntouchedPaths();

    if (failures) {
        printf("%d OVERLAP CHECK(S) FAILED\n", failures);
        return 1;
    }
    puts("ALL OVERLAP CONTRACT TESTS PASSED");
    return 0;
}
