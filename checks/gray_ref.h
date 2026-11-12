/*
 * gray_ref.h -- independent reference model for 8-bit grayscale
 * reconstruction and the operations derived from it (h-dome, basin fill).
 *
 * This module is the ground truth for the contract tests in
 * gray_contract.c.  It is deliberately written as a self-contained
 * header so that the fixed build line in checks/target.sh (which
 * compiles only contract_main.c + gray_contract.c) picks it up without
 * any build-system changes.
 *
 * Independence notes:
 *   - No Leptonica function is called.  Only plain C arrays, malloc,
 *     and this file's own breadth-first graph traversal are used.
 *   - The reconstruction is computed straight from the threshold-set
 *     definition of grayscale reconstruction (see gray_ref_reconstruct):
 *     for every nonzero gray level t, the connected components of
 *     {p : mask(p) >= t} are found, and a component belongs to the
 *     result's level set at t iff it contains a pixel with seed >= t.
 *     The output gray value is the combination (here: the maximum,
 *     since the level sets are nested) of the levels reached.
 *   - It does NOT use the library's raster/anti-raster scan pair, its
 *     FIFO propagation queue, connected-component finders, morphology,
 *     or any priority-flood variant.  The per-level BFS below visits
 *     each pixel of each level set exactly once, directly from the
 *     definition.
 *   - 4- and 8-connectivity are computed independently from the same
 *     definition; neither result is derived from the other.
 *
 * All images are row-major uint8 arrays of w*h pixels.  Coordinates
 * are (x, y) with x the column; pixel index p = y*w + x.
 *
 * Every caller-visible function is "static" so the header can be
 * included into a single translation unit without linker concerns.
 */
#ifndef GRAY_REF_H
#define GRAY_REF_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Neighbor tables: first 4 entries are the 4-connected neighbors;
 * all 8 entries are the 8-connected neighborhood. */
static const int gray_ref_dx[8] = { -1, 1, 0, 0, -1, -1, 1, 1 };
static const int gray_ref_dy[8] = {  0, 0,-1, 1, -1,  1,-1, 1 };

/*
 * gray_ref_reconstruct() -- reference grayscale reconstruction of
 * `seed` under `mask` (must satisfy seed <= mask pointwise), written
 * to `out`.  `conn` is 4 or 8.
 *
 * Definition used (Vincent, Morphological Grayscale Reconstruction):
 * for each level t in 1..255, let A_t = { p : mask(p) >= t } and let
 * C_t be the connected components of A_t under the requested
 * connectivity.  The result's level set R_t is the union of those
 * components of C_t that contain at least one pixel q with
 * seed(q) >= t.  The level sets are nested, so the output at p is the
 * largest t with p in R_t; equivalently the sum over t of the
 * indicator of R_t.  Levels are processed in ascending order and the
 * last write wins, which yields exactly that maximum.
 */
static void
gray_ref_reconstruct(const uint8_t *seed, const uint8_t *mask,
                     int w, int h, int conn, uint8_t *out)
{
    const int n = w * h;
    const int nn = (conn == 8) ? 8 : 4;
    uint8_t *visited = (uint8_t *)malloc((size_t)n);
    int *queue = (int *)malloc((size_t)n * sizeof(int));
    int t, p, qh, qt, k;

    memset(out, 0, (size_t)n);
    for (t = 1; t <= 255; t++) {
        memset(visited, 0, (size_t)n);
        qh = qt = 0;
            /* Multi-source BFS seeded at every pixel of A_t whose
             * seed value reaches level t. */
        for (p = 0; p < n; p++) {
            if ((int)mask[p] >= t && (int)seed[p] >= t) {
                visited[p] = 1;
                queue[qt++] = p;
            }
        }
        while (qh < qt) {
            int x, y, q;
            p = queue[qh++];
            x = p % w;
            y = p / w;
            out[p] = (uint8_t)t;  /* ascending t: final write is the max */
            for (k = 0; k < nn; k++) {
                const int nx = x + gray_ref_dx[k];
                const int ny = y + gray_ref_dy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                q = ny * w + nx;
                if (!visited[q] && (int)mask[q] >= t) {
                    visited[q] = 1;
                    queue[qt++] = q;
                }
            }
        }
    }
    free(visited);
    free(queue);
}

/*
 * gray_ref_hdome() -- reference model for pixHDome():
 * the seed is the mask lowered by `height` (saturating at 0), the
 * seed is reconstructed under the mask, and the result is the
 * residual mask - reconstruction (non-negative, since the
 * reconstruction never exceeds the mask).  height == 0 yields the
 * zero image, matching the library's early return.
 */
static void
gray_ref_hdome(const uint8_t *mask, int w, int h, int height,
               int conn, uint8_t *out)
{
    const int n = w * h;
    uint8_t *seed = (uint8_t *)malloc((size_t)n);
    uint8_t *rec = (uint8_t *)malloc((size_t)n);
    int p;

    for (p = 0; p < n; p++)
        seed[p] = ((int)mask[p] > height) ? (uint8_t)(mask[p] - height)
                                          : (uint8_t)0;
    gray_ref_reconstruct(seed, mask, w, h, conn, rec);
    for (p = 0; p < n; p++)
        out[p] = (uint8_t)(mask[p] - rec[p]);
    free(seed);
    free(rec);
}

/*
 * gray_ref_basin() -- reference model for pixSeedfillGrayBasin():
 *   - delta <= 0: the result is a plain copy of the mask.
 *   - otherwise the seed is min(255, mask + delta) at the binary seed
 *     locations (bin[] == 1) and 255 everywhere else; the result is
 *     255 - R_{255 - mask}(255 - seed), i.e. the documented
 *     invert / reconstruct / re-invert composition, with the
 *     reconstruction evaluated by this module's own model.
 */
static void
gray_ref_basin(const uint8_t *bin, const uint8_t *mask, int w, int h,
               int delta, int conn, uint8_t *out)
{
    const int n = w * h;
    uint8_t *s2, *m2, *rec;
    int p, sv;

    if (delta <= 0) {
        memcpy(out, mask, (size_t)n);
        return;
    }
    s2 = (uint8_t *)malloc((size_t)n);
    m2 = (uint8_t *)malloc((size_t)n);
    rec = (uint8_t *)malloc((size_t)n);
    for (p = 0; p < n; p++) {
        sv = bin[p] ? (int)mask[p] + delta : 255;
        if (sv > 255)
            sv = 255;
        s2[p] = (uint8_t)(255 - sv);
        m2[p] = (uint8_t)(255 - mask[p]);
    }
    gray_ref_reconstruct(s2, m2, w, h, conn, rec);
    for (p = 0; p < n; p++)
        out[p] = (uint8_t)(255 - rec[p]);
    free(s2);
    free(m2);
    free(rec);
}

/*
 * gray_ref_transform() -- coordinate transforms used by the
 * rotation/mirror/transpose invariance checks.  `kind`:
 *   0 = rotate 90 clockwise, 1 = rotate 180, 2 = rotate 270 clockwise,
 *   3 = mirror left-right, 4 = flip up-down, 5 = transpose.
 * All of them preserve both 4- and 8-connectivity, so the
 * reconstruction of a transformed image must equal the transformed
 * reconstruction.  Output dimensions are returned through ow and oh.
 */
static void
gray_ref_transform(const uint8_t *in, int w, int h, int kind,
                   uint8_t *out, int *ow, int *oh)
{
    int W = w, H = h;
    int x, y, nx, ny;

    if (kind == 0 || kind == 2 || kind == 5) {
        W = h;
        H = w;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            switch (kind) {
            case 0:  ny = x;          nx = h - 1 - y;  break;
            case 1:  ny = h - 1 - y;  nx = w - 1 - x;  break;
            case 2:  ny = w - 1 - x;  nx = y;          break;
            case 3:  ny = y;          nx = w - 1 - x;  break;
            case 4:  ny = h - 1 - y;  nx = x;          break;
            default: ny = x;          nx = y;          break;
            }
            out[ny * W + nx] = in[y * w + x];
        }
    }
    *ow = W;
    *oh = H;
}

/* Inverse transform kind: rot90 <-> rot270; the rest are involutions. */
static int
gray_ref_transform_inverse(int kind)
{
    if (kind == 0)
        return 2;
    if (kind == 2)
        return 0;
    return kind;
}

/* Legality check for test inputs: seed must not exceed the mask. */
static int
gray_ref_legal(const uint8_t *seed, const uint8_t *mask, int n)
{
    int p;
    for (p = 0; p < n; p++) {
        if (seed[p] > mask[p])
            return 0;
    }
    return 1;
}

#endif  /* GRAY_REF_H */
