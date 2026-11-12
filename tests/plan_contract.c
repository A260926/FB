/******************************************************************************

plan_contract.c - contract tests for GifMakeAnimationPlan()

Three groups:

  test_optimality - an independent brute-force enumerator builds every
  legal representation of small scenarios (the fixed two rectangles, two
  fill modes, three disposals), prices each whole sequence with the
  stock EGif compressor into a memory buffer, and checks the production
  planner picks the unique cheapest, lexicographically smallest result.
  The reference never calls the production planner.

  test_composite - DGifSlurp()s the produced bytes and re-implements GIF
  compositing and disposal itself, checking every displayed canvas,
  delay, the NETSCAPE2.0 loop field and the all-transparent end state.

  test_legacy - pins the legacy EGif/DGif byte stream and basic API
  behavior so the new code cannot disturb the existing compressor.

******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gif_lib.h"
#include "gif_plan.h"
#include "check.h"

/* ---- memory callbacks, identical in spirit to support/baseline.c ---- */

struct bytes {
    unsigned char *p;
    size_t n, pos;
};

static int
wr_cb(GifFileType *g, const GifByteType *p, int n)
{
    struct bytes *b = (struct bytes *) g->UserData;
    unsigned char *q = realloc(b->p, b->n + (size_t) n);

    if (q == NULL)
        return 0;
    b->p = q;
    memcpy(q + b->n, p, (size_t) n);
    b->n += (size_t) n;
    return n;
}

static int
rd_cb(GifFileType *g, GifByteType *p, int n)
{
    struct bytes *b = (struct bytes *) g->UserData;

    if ((size_t) n > b->n - b->pos)
        return 0;
    memcpy(p, b->p + b->pos, (size_t) n);
    b->pos += (size_t) n;
    return n;
}

static ColorMapObject *
make_map4(void)
{
    GifColorType colors[4] = {
        {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}
    };
    return GifMakeMapObject(4, colors);
}

/* ---- independent brute-force reference ---- */

typedef struct {
    int left, top, width, height;
} RefRect;

typedef struct {
    int left, top, width, height, disposal, transparentize;
} Tuple;

typedef struct {
    int w, h, nframes, loop;
    const unsigned char *inputs;   /* nframes dense canvases */
    const int *delays;
    ColorMapObject *cm;
    unsigned char *canvases[65];   /* entry canvas at each depth */
    unsigned char *display;        /* single scratch, depth-local use */
    Tuple best[64];
    long best_size;
    long sequences_priced;         /* full sequences sent through EGif */
    long transitions_legal;        /* legal partial transitions explored */
} RefCtx;

static const unsigned char *
ref_frame(const RefCtx *c, int i)
{
    return c->inputs + (size_t) i * (size_t) c->w * (size_t) c->h;
}

/* Independent implementation of the fixed rectangle rule. */
static int
ref_rects(const RefCtx *c, int i, RefRect out[2])
{
    static const unsigned char zero_canvas[256u * 256u];
    size_t npix = (size_t) c->w * (size_t) c->h;
    const unsigned char *cur = ref_frame(c, i);
    const unsigned char *prev = zero_canvas;
    const unsigned char *next = NULL;
    int minx = c->w, miny = c->h, maxx = -1, maxy = -1, count = 2;
    size_t p;

    if (i > 0)
        prev = ref_frame(c, i - 1);
    if (i + 1 < c->nframes)
        next = ref_frame(c, i + 1);

    for (p = 0; p < npix; p++) {
        int marked = cur[p] != prev[p];

        if (!marked && next != NULL && cur[p] != 0 && next[p] == 0)
            marked = 1;
        if (marked) {
            int x = (int) (p % (size_t) c->w);
            int y = (int) (p / (size_t) c->w);

            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x > maxx) maxx = x;
            if (y > maxy) maxy = y;
        }
    }

    out[0].left = 0;
    out[0].top = 0;
    out[0].width = c->w;
    out[0].height = c->h;
    if (maxx < 0) {
        out[1].left = out[1].top = 0;
        out[1].width = out[1].height = 1;
    } else {
        out[1].left = minx;
        out[1].top = miny;
        out[1].width = maxx - minx + 1;
        out[1].height = maxy - miny + 1;
    }
    if (out[1].left == out[0].left && out[1].top == out[0].top &&
        out[1].width == out[0].width && out[1].height == out[0].height)
        count = 1;                  /* duplicate rectangles merge */
    return count;
}

static int
tuple_less(const Tuple *a, const Tuple *b)
{
    if (a->left != b->left) return a->left < b->left;
    if (a->top != b->top) return a->top < b->top;
    if (a->width != b->width) return a->width < b->width;
    if (a->height != b->height) return a->height < b->height;
    if (a->disposal != b->disposal) return a->disposal < b->disposal;
    return a->transparentize < b->transparentize;   /* false (0) first */
}

static int
tuple_equal(const Tuple *a, const Tuple *b)
{
    return !tuple_less(a, b) && !tuple_less(b, a);
}

/* Lexicographic order of two equal-length candidate sequences. */
static int
seq_less(const Tuple *a, const Tuple *b, int len)
{
    int q;

    for (q = 0; q < len; q++) {
        if (tuple_less(&a[q], &b[q]))
            return 1;
        if (tuple_less(&b[q], &a[q]))
            return 0;
    }
    return 0;
}

/*
 * Encode one full legal sequence with the stock EGif compressor.
 * Returns 1 on success with *size_out set, 0 if the sequence fails the
 * composite or end-state rules, -1 if EGif itself reports an error.
 */
static int
ref_encode(RefCtx *c, const Tuple *seq, long *size_out)
{
    struct bytes b = {0};
    GifFileType *g;
    unsigned char *entry, *scan, *line;
    size_t npix = (size_t) c->w * (size_t) c->h;
    static const GifByteType ns_header[11] = {
        'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'
    };
    GifByteType ns_loop[3], ext[4];
    int err = 0, i, x, y, rc = -1;

    entry = calloc(npix, 1);
    scan = malloc(npix);
    line = malloc((size_t) c->w);
    g = EGifOpen(&b, wr_cb, &err);
    if (entry == NULL || scan == NULL || line == NULL || g == NULL)
        goto done;
    EGifSetGifVersion(g, true);
    if (EGifPutScreenDesc(g, c->w, c->h, GifBitSize(c->cm->ColorCount),
                          0, c->cm) == GIF_ERROR)
        goto done;
    if (EGifPutExtensionLeader(g, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR ||
        EGifPutExtensionBlock(g, 11, ns_header) == GIF_ERROR)
        goto done;
    ns_loop[0] = 1;
    ns_loop[1] = (GifByteType) (c->loop & 0xff);
    ns_loop[2] = (GifByteType) ((c->loop >> 8) & 0xff);
    if (EGifPutExtensionBlock(g, 3, ns_loop) == GIF_ERROR ||
        EGifPutExtensionTrailer(g) == GIF_ERROR)
        goto done;

    for (i = 0; i < c->nframes; i++) {
        const Tuple *t = &seq[i];
        const unsigned char *target = ref_frame(c, i);
        GraphicsControlBlock gcb;

        memcpy(scan, entry, npix);
        for (y = 0; y < t->height; y++) {
            for (x = 0; x < t->width; x++) {
                size_t p = (size_t) (t->top + y) * (size_t) c->w +
                           (size_t) (t->left + x);
                unsigned char v = target[p];

                if (t->transparentize && entry[p] == v)
                    v = 0;
                if (v != 0)
                    scan[p] = v;           /* 0 never erases an old pixel */
            }
        }
        if (memcmp(scan, target, npix) != 0) {
            rc = 0;
            goto done;
        }

        gcb.DisposalMode = t->disposal;
        gcb.UserInputFlag = false;
        gcb.DelayTime = c->delays[i];
        gcb.TransparentColor = 0;
        EGifGCBToExtension(&gcb, ext);
        if (EGifPutExtension(g, GRAPHICS_EXT_FUNC_CODE, 4, ext) == GIF_ERROR ||
            EGifPutImageDesc(g, t->left, t->top, t->width, t->height,
                             false, NULL) == GIF_ERROR)
            goto done;
        for (y = 0; y < t->height; y++) {
            for (x = 0; x < t->width; x++) {
                size_t p = (size_t) (t->top + y) * (size_t) c->w +
                           (size_t) (t->left + x);
                unsigned char v = target[p];

                if (t->transparentize && entry[p] == v)
                    v = 0;
                line[x] = v;
            }
            if (EGifPutLine(g, line, t->width) == GIF_ERROR)
                goto done;
        }

        /* disposal produces the next entry canvas */
        if (t->disposal == DISPOSE_PREVIOUS) {
            memcpy(scan, entry, npix);
        } else if (t->disposal == DISPOSE_BACKGROUND) {
            for (y = 0; y < t->height; y++)
                for (x = 0; x < t->width; x++)
                    scan[(size_t) (t->top + y) * (size_t) c->w +
                         (size_t) (t->left + x)] = 0;
        }
        memcpy(entry, scan, npix);
    }
    if (EGifCloseFile(g, &err) == GIF_ERROR) {
        g = NULL;
        goto done;
    }
    g = NULL;
    for (i = 0; i < (int) npix; i++) {
        if (entry[i] != 0) {          /* loop restart would not match */
            rc = 0;
            goto done;
        }
    }
    *size_out = (long) b.n;
    rc = 1;

done:
    if (g != NULL)
        EGifCloseFile(g, &err);
    free(entry);
    free(scan);
    free(line);
    free(b.p);
    return rc;
}

static void
ref_dfs(RefCtx *c, int depth, Tuple *seq)
{
    size_t npix = (size_t) c->w * (size_t) c->h;
    unsigned char *entry = c->canvases[depth];
    RefRect rects[2];
    int nrects, ri, disp, fill;

    if (depth == c->nframes) {
        int i;
        long size;
        int rc;

        for (i = 0; i < (int) npix; i++) {
            if (entry[i] != 0)
                return;               /* final canvas must be transparent */
        }
        rc = ref_encode(c, seq, &size);
        unit_check(rc >= 0, "reference EGif encoding never errors");
        if (rc <= 0)
            return;
        c->sequences_priced++;
        if (c->best_size < 0 || size < c->best_size ||
            (size == c->best_size && seq_less(seq, c->best, depth))) {
            c->best_size = size;
            memcpy(c->best, seq, (size_t) depth * sizeof(Tuple));
        }
        return;
    }

    nrects = ref_rects(c, depth, rects);
    for (ri = 0; ri < nrects; ri++) {
        for (disp = 1; disp <= 3; disp++) {
            for (fill = 0; fill < 2; fill++) {
                const unsigned char *target = ref_frame(c, depth);
                unsigned char *next_entry = c->canvases[depth + 1];
                int x, y;

                memcpy(c->display, entry, npix);
                for (y = 0; y < rects[ri].height; y++) {
                    for (x = 0; x < rects[ri].width; x++) {
                        size_t p = (size_t) (rects[ri].top + y) *
                                   (size_t) c->w +
                                   (size_t) (rects[ri].left + x);
                        unsigned char v = target[p];

                        if (fill && entry[p] == v)
                            v = 0;
                        if (v != 0)
                            c->display[p] = v;
                    }
                }
                if (memcmp(c->display, target, npix) != 0)
                    continue;

                seq[depth].left = rects[ri].left;
                seq[depth].top = rects[ri].top;
                seq[depth].width = rects[ri].width;
                seq[depth].height = rects[ri].height;
                seq[depth].disposal = disp;
                seq[depth].transparentize = fill;

                if (disp == DISPOSE_PREVIOUS) {
                    memcpy(next_entry, entry, npix);
                } else if (disp == DISPOSE_BACKGROUND) {
                    memcpy(next_entry, c->display, npix);
                    for (y = 0; y < rects[ri].height; y++)
                        for (x = 0; x < rects[ri].width; x++)
                            next_entry[(size_t) (rects[ri].top + y) *
                                       (size_t) c->w +
                                       (size_t) (rects[ri].left + x)] = 0;
                } else {
                    memcpy(next_entry, c->display, npix);
                }
                c->transitions_legal++;
                ref_dfs(c, depth + 1, seq);
            }
        }
    }
}

/*
 * Run planner and reference on one scenario, then compare.  Inputs are
 * dense w*h canvases, at most 4 frames so 12^nframes stays enumerable.
 */
static void
check_scenario(const char *name, int w, int h, int nframes,
               const unsigned char *inputs, const int *delays, int loop)
{
    ColorMapObject *cm = make_map4();
    GifPlanFrame pf[4];
    GifAnimationPlan *plan;
    RefCtx c;
    Tuple seq[4];
    int err = 0, i, d;
    char label[128];
    size_t npix = (size_t) w * (size_t) h;

    unit_check(cm != NULL, "reference colormap alloc");
    for (i = 0; i < nframes; i++) {
        pf[i].Pixels = inputs + (size_t) i * npix;
        pf[i].Stride = (size_t) w;
        pf[i].DelayTime = delays[i];
    }
    plan = GifMakeAnimationPlan(w, h, cm, pf, nframes, loop, &err);
    snprintf(label, sizeof(label), "%s: planner returned a plan", name);
    unit_check(plan != NULL, label);
    unit_check(err == E_GIF_SUCCEEDED, "planner error code is success");
    unit_check(plan != NULL && plan->FrameCount == nframes,
               "frame count preserved");

    memset(&c, 0, sizeof(c));
    c.w = w; c.h = h; c.nframes = nframes; c.loop = loop;
    c.inputs = inputs; c.delays = delays; c.cm = cm;
    c.best_size = -1;
    c.display = malloc(npix);
    unit_check(c.display != NULL, "reference display scratch alloc");
    for (d = 0; d <= nframes; d++) {
        c.canvases[d] = calloc(npix, 1);
        unit_check(c.canvases[d] != NULL, "reference depth canvas alloc");
    }
    ref_dfs(&c, 0, seq);
    snprintf(label, sizeof(label), "%s: reference found legal sequences",
             name);
    unit_check(c.best_size >= 0, label);
    snprintf(label, sizeof(label), "%s: byte size equals EGif minimum",
             name);
    unit_check(plan != NULL && (long) plan->Size == c.best_size, label);

    if (plan != NULL) {
        for (i = 0; i < nframes; i++) {
            Tuple t;

            t.left = plan->Steps[i].Left;
            t.top = plan->Steps[i].Top;
            t.width = plan->Steps[i].Width;
            t.height = plan->Steps[i].Height;
            t.disposal = plan->Steps[i].DisposalMode;
            t.transparentize = plan->Steps[i].Transparentize ? 1 : 0;
            snprintf(label, sizeof(label),
                     "%s frame %d: step tuple matches lexicographic winner",
                     name, i);
            unit_check(tuple_equal(&c.best[i], &t), label);
        }
        /* Every planner compression prices one legal transition; state
           merging means it can never outgrow the brute-force tree. */
        snprintf(label, sizeof(label),
                 "%s: merged DP prices no more transitions than the "
                 "exhaustive tree", name);
        unit_check(plan->CompressionCount <= c.transitions_legal, label);
        unit_check(plan->MaxStates >= 1, "frontier always non-empty");
        printf("REF  %-22s priced=%ld legal_transitions=%ld "
               "plan_bytes=%zu max_states=%ld compressions=%ld\n",
               name, c.sequences_priced, c.transitions_legal, plan->Size,
               plan->MaxStates, plan->CompressionCount);
    }

    for (d = 0; d <= nframes; d++)
        free(c.canvases[d]);
    free(c.display);
    GifFreeAnimationPlan(plan);
    GifFreeMapObject(cm);
}

void
test_optimality(void)
{
    static const int delays3[3] = {4, 5, 6};
    static const int delays1[1] = {9};
    static const int delays2[2] = {11, 12};

    /* static texture, temporary overlay, restore: disposal-3 lets the
       middle frame's 2-pixel overlay vanish without a clearing image */
    static const unsigned char seq_overlay[3 * 12] = {
        1,1,1,1, 1,1,1,1, 1,1,1,1,
        1,1,1,1, 1,2,1,1, 1,1,1,1,
        1,1,1,1, 1,1,1,1, 1,1,1,1
    };

    /* transparent hole: the lit pixel must be cleared before frame 1 */
    static const unsigned char seq_hole[2 * 4] = {
        1, 0, 0, 0,
        0, 0, 0, 0
    };

    /* flicker between two full patterns, then transparent */
    static const unsigned char seq_flick[3 * 4] = {
        1, 2, 1, 2,
        2, 1, 2, 1,
        0, 0, 0, 0
    };

    /* two identical input frames in a row, then transparent: the DP must
       merge the repeated canvas instead of branching exponentially */
    static const int delays_rep[3] = {7, 8, 9};
    static const unsigned char seq_repeat[3 * 4] = {
        1, 2, 2, 1,
        1, 2, 2, 1,
        0, 0, 0, 0
    };

    /* 1x1 transparent: every representation costs the same bytes */
    static const unsigned char seq_tie[1] = {0};

    check_scenario("overlay-restore", 4, 3, 3, seq_overlay, delays3, 0);
    check_scenario("transparent-hole", 2, 2, 2, seq_hole, delays2, 2);
    check_scenario("flicker-then-clear", 2, 2, 3, seq_flick, delays3, 1);
    check_scenario("repeated-frame", 2, 2, 3, seq_repeat, delays_rep, 4);

    /* exact tuple on the all-tie case */
    {
        ColorMapObject *cm = make_map4();
        GifPlanFrame pf = {seq_tie, 1, delays1[0]};
        GifAnimationPlan *plan;
        int err = 0;

        plan = GifMakeAnimationPlan(1, 1, cm, &pf, 1, 0, &err);
        unit_check(plan != NULL, "tie: plan built");
        unit_check(plan != NULL && plan->Steps[0].Left == 0 &&
                   plan->Steps[0].Top == 0 &&
                   plan->Steps[0].Width == 1 &&
                   plan->Steps[0].Height == 1, "tie: rect is origin 1x1");
        unit_check(plan != NULL &&
                   plan->Steps[0].DisposalMode == DISPOSE_DO_NOT,
                   "tie: disposal 1 wins lexicographically");
        unit_check(plan != NULL &&
                   plan->Steps[0].Transparentize == false,
                   "tie: non-transparentized fill wins (false first)");
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm);
    }
}

/* ---- DGifSlurp-based compositing checks ---- */

static int
find_loop_count(GifFileType *g, int *loop_out)
{
    SavedImage *sp = &g->SavedImages[0];
    int i;

    for (i = 0; i < sp->ExtensionBlockCount; i++) {
        ExtensionBlock *ep = &sp->ExtensionBlocks[i];

        if (ep->Function == APPLICATION_EXT_FUNC_CODE &&
            ep->ByteCount == 11 &&
            memcmp(ep->Bytes, "NETSCAPE2.0", 11) == 0 &&
            i + 1 < sp->ExtensionBlockCount &&
            sp->ExtensionBlocks[i + 1].Function ==
                CONTINUE_EXT_FUNC_CODE &&
            sp->ExtensionBlocks[i + 1].ByteCount == 3) {
            GifByteType *d = sp->ExtensionBlocks[i + 1].Bytes;

            if (d[0] != 1)
                return 0;
            *loop_out = (int) d[1] | ((int) d[2] << 8);
            return 1;
        }
    }
    return 0;
}

/* The test's own canvas/disposal implementation, driven by slurped data. */
static void
check_roundtrip(const char *name, int w, int h, int nframes,
                const GifPlanFrame *pf, int loop)
{
    ColorMapObject *cm = make_map4();
    GifAnimationPlan *plan;
    struct bytes b;
    GifFileType *g;
    unsigned char *canvas, *saved;
    int err = 0, i, x, y, got_loop = -1;
    size_t npix = (size_t) w * (size_t) h;
    char label[160];

    unit_check(cm != NULL, "roundtrip colormap alloc");
    plan = GifMakeAnimationPlan(w, h, cm, pf, nframes, loop, &err);
    snprintf(label, sizeof(label), "%s: plan built", name);
    unit_check(plan != NULL, label);
    unit_check(plan != NULL && plan->Size > 13 &&
               memcmp(plan->Data, "GIF89a", 6) == 0,
               "output is a GIF89a stream");
    unit_check(plan != NULL && plan->Data[plan->Size - 1] == 0x3b,
               "output ends with the GIF trailer");

    b.p = plan->Data;
    b.n = plan->Size;
    b.pos = 0;
    g = DGifOpen(&b, rd_cb, &err);
    unit_check(g != NULL, "DGifOpen on plan bytes");
    unit_check(g != NULL && DGifSlurp(g) == GIF_OK, "DGifSlurp plan bytes");
    if (g == NULL || plan == NULL) {
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm);
        return;
    }
    unit_check(g->ImageCount == nframes, "slurped image count");
    unit_check(g->SWidth == w && g->SHeight == h, "slurped canvas size");
    unit_check(find_loop_count(g, &got_loop), "NETSCAPE2.0 block present");
    snprintf(label, sizeof(label), "%s: loop field round trips", name);
    unit_check(got_loop == loop, label);

    canvas = calloc(npix, 1);
    saved = malloc(npix);
    unit_check(canvas != NULL && saved != NULL, "composite scratch alloc");
    if (canvas == NULL || saved == NULL) {
        free(canvas);
        free(saved);
        DGifCloseFile(g, &err);
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm);
        return;
    }

    for (i = 0; i < nframes; i++) {
        SavedImage *si = &g->SavedImages[i];
        GraphicsControlBlock gcb;
        const GifPixelType *want = pf[i].Pixels;

        unit_check(DGifSavedExtensionToGCB(g, i, &gcb) == GIF_OK,
                   "frame has a graphics control block");
        snprintf(label, sizeof(label),
                 "%s frame %d: delay round trips", name, i);
        unit_check(gcb.DelayTime == pf[i].DelayTime, label);
        snprintf(label, sizeof(label),
                 "%s frame %d: disposal matches chosen step", name, i);
        unit_check(gcb.DisposalMode == plan->Steps[i].DisposalMode, label);
        unit_check(gcb.TransparentColor == 0,
                   "transparent index is always 0");

        memcpy(saved, canvas, npix);
        for (y = 0; y < si->ImageDesc.Height; y++) {
            for (x = 0; x < si->ImageDesc.Width; x++) {
                GifPixelType v = si->RasterBits[(size_t) y *
                                                si->ImageDesc.Width + x];
                if (v != (GifPixelType) gcb.TransparentColor) {
                    canvas[(size_t) (si->ImageDesc.Top + y) *
                           (size_t) w +
                           (size_t) (si->ImageDesc.Left + x)] = v;
                }
            }
        }
        snprintf(label, sizeof(label),
                 "%s frame %d: displayed canvas is pixel-exact", name, i);
        {
            int row_exact = 1, ry, rx;

            for (ry = 0; ry < h && row_exact; ry++)
                for (rx = 0; rx < w; rx++) {
                    if (canvas[(size_t) ry * (size_t) w + rx] !=
                        want[(size_t) ry * pf[i].Stride + rx]) {
                        row_exact = 0;
                        break;
                    }
                }
            unit_check(row_exact, label);
        }

        /* disposal takes effect before the next frame is displayed */
        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            for (y = 0; y < si->ImageDesc.Height; y++)
                for (x = 0; x < si->ImageDesc.Width; x++)
                    canvas[(size_t) (si->ImageDesc.Top + y) *
                           (size_t) w +
                           (size_t) (si->ImageDesc.Left + x)] = 0;
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
            memcpy(canvas, saved, npix);
        }
    }
    {
        int clear = 1;

        for (i = 0; i < (int) npix; i++)
            if (canvas[i] != 0)
                clear = 0;
        snprintf(label, sizeof(label),
                 "%s: canvas after final disposal is transparent", name);
        unit_check(clear, label);
    }

    free(canvas);
    free(saved);
    DGifCloseFile(g, &err);
    GifFreeAnimationPlan(plan);
    GifFreeMapObject(cm);
}

void
test_composite(void)
{
    static const int delays_overlay[3] = {5, 17, 65535};
    static const unsigned char seq_overlay[3 * 16] = {
        1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1,
        1,1,2,2, 1,1,2,2, 1,1,1,1, 1,1,1,1,
        1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1
    };
    GifPlanFrame overlay_pf[3];
    int i;

    static const int delays_hole[3] = {3, 4, 5};
    static const unsigned char seq_hole[3 * 9] = {
        1,2,0, 0,0,0, 0,0,0,
        0,0,0, 0,2,0, 0,0,0,
        0,0,0, 0,0,0, 0,0,0
    };
    GifPlanFrame hole_pf[3];

    for (i = 0; i < 3; i++) {
        overlay_pf[i].Pixels = seq_overlay + (size_t) i * 16;
        overlay_pf[i].Stride = 4;
        overlay_pf[i].DelayTime = delays_overlay[i];
        hole_pf[i].Pixels = seq_hole + (size_t) i * 9;
        hole_pf[i].Stride = 3;
        hole_pf[i].DelayTime = delays_hole[i];
    }
    check_roundtrip("overlay-restore-4x4", 4, 4, 3, overlay_pf, 3);
    check_roundtrip("transparent-hole-3x3", 3, 3, 3, hole_pf, 0);

    /* padded stride: row stride wider than the canvas; the generic
       roundtrip checker reads pixels through Stride and replays
       disposal, so this exercises both at once. */
    {
        static unsigned char padded[2][18];
        GifPlanFrame pf[2];
        static const int padded_delays[2] = {20, 21};
        int x, y;

        memset(padded, 0, sizeof(padded));
        for (y = 0; y < 3; y++)
            for (x = 0; x < 4; x++)
                padded[0][y * 6 + x] = (unsigned char) (1 + ((x + y) & 1));
        padded[1][0] = 1;          /* one corner lit, second frame */
        pf[0].Pixels = padded[0];
        pf[0].Stride = 6;
        pf[0].DelayTime = padded_delays[0];
        pf[1].Pixels = padded[1];
        pf[1].Stride = 6;
        pf[1].DelayTime = padded_delays[1];
        check_roundtrip("padded-stride-4x3", 4, 3, 2, pf, 7);
    }

    /* long repeated flicker: state merging must keep the search linear */
    {
        enum { LONG_N = 64, W = 4, H = 4 };
        static unsigned char big[LONG_N][W * H];
        GifPlanFrame long_pf[LONG_N];
        ColorMapObject *cm = make_map4();
        GifAnimationPlan *plan;
        int err = 0, f, p;

        for (f = 0; f < LONG_N - 1; f++) {
            for (p = 0; p < W * H; p++)
                big[f][p] = (unsigned char) (1 + ((f + p) & 1));
            long_pf[f].Pixels = big[f];
            long_pf[f].Stride = W;
            long_pf[f].DelayTime = (f % 100) + 1;
        }
        memset(big[LONG_N - 1], 0, W * H);   /* transparent ending frame */
        long_pf[LONG_N - 1].Pixels = big[LONG_N - 1];
        long_pf[LONG_N - 1].Stride = W;
        long_pf[LONG_N - 1].DelayTime = 65535;

        plan = GifMakeAnimationPlan(W, H, cm, long_pf, LONG_N, 0, &err);
        unit_check(plan != NULL, "64-frame flicker plan built");
        unit_check(plan != NULL && plan->FrameCount == LONG_N,
                   "no frames added or dropped");
        if (plan != NULL) {
            /* A merged frontier of distinct canvases stays tiny even
               though an exhaustive tree would hold 12^64 sequences. */
            unit_check(plan->MaxStates <= 16,
                       "frontier stays merged on repeated flicker");
            unit_check(plan->CompressionCount <=
                       (long) LONG_N * plan->MaxStates * 12 + 2 * LONG_N,
                       "compression count linear in frames times states");
            printf("LONG frames=%d max_states=%ld compressions=%ld "
                   "exhaustive_tree_would_be=12^%d bytes=%zu\n",
                   LONG_N, plan->MaxStates, plan->CompressionCount,
                   LONG_N, plan->Size);
            check_roundtrip("flicker-64", W, H, LONG_N, long_pf, 0);
        }
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm);
    }
}

void
test_legacy(void)
{
    /* Reproduce support/baseline.c: the legacy compressor byte stream must
       remain byte-for-byte stable now that gif_plan.c is in the build. */
    struct bytes b = {0};
    int err;
    GifColorType colors[4] = {
        {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}
    };
    ColorMapObject *m = GifMakeMapObject(4, colors);
    GifFileType *g;
    GifByteType pixels[256], ext[4];
    int f, i;

    unit_check(m != NULL, "legacy colormap alloc");
    g = EGifOpen(&b, wr_cb, &err);
    unit_check(g != NULL, "legacy EGifOpen");
    if (g == NULL) {
        GifFreeMapObject(m);
        return;
    }
    EGifSetGifVersion(g, true);
    unit_check(EGifPutScreenDesc(g, 16, 16, 2, 0, m) == GIF_OK,
               "legacy screen desc");
    for (f = 0; f < 2; f++) {
        GraphicsControlBlock ctl = {DISPOSE_DO_NOT, false, 7 + f, 0};

        EGifGCBToExtension(&ctl, ext);
        unit_check(EGifPutExtension(g, GRAPHICS_EXT_FUNC_CODE, 4, ext) ==
                   GIF_OK, "legacy GCE write");
        unit_check(EGifPutImageDesc(g, 0, 0, 16, 16, false, NULL) ==
                   GIF_OK, "legacy image desc");
        for (i = 0; i < 256; i++)
            pixels[i] = (GifByteType) (1 + (i + f) % 3);
        unit_check(EGifPutLine(g, pixels, 256) == GIF_OK,
                   "legacy compressed line");
    }
    unit_check(EGifCloseFile(g, &err) == GIF_OK, "legacy close");
    unit_check(b.n == 118, "legacy stream is still exactly 118 bytes");

    b.pos = 0;
    g = DGifOpen(&b, rd_cb, &err);
    unit_check(g != NULL, "legacy DGifOpen");
    unit_check(g != NULL && DGifSlurp(g) == GIF_OK, "legacy DGifSlurp");
    if (g != NULL) {
        unit_check(g->ImageCount == 2, "legacy frame count");
        for (f = 0; f < 2; f++) {
            GraphicsControlBlock c;

            unit_check(DGifSavedExtensionToGCB(g, f, &c) == GIF_OK,
                       "legacy GCE read");
            unit_check(c.DelayTime == 7 + f, "legacy delay read");
            for (i = 0; i < 256; i++) {
                unit_check(g->SavedImages[f].RasterBits[i] ==
                           1 + (i + f) % 3, "legacy pixel read");
            }
        }
        DGifCloseFile(g, &err);
    }
    free(b.p);
    GifFreeMapObject(m);

    /* argument validation follows giflib error-return conventions */
    {
        ColorMapObject *cm = make_map4();
        GifPlanFrame pf;
        static unsigned char one[4] = {1, 1, 1, 1};
        GifAnimationPlan *plan;
        int code = 0;

        pf.Pixels = one;
        pf.Stride = 2;
        pf.DelayTime = 0;
        unit_check(GifMakeAnimationPlan(0, 2, cm, &pf, 1, 0, &code) == NULL,
                   "zero width rejected");
        unit_check(code == 20, "bad argument error code");
        unit_check(GifMakeAnimationPlan(2, 257, cm, &pf, 1, 0, &code) ==
                   NULL, "oversize height rejected");
        unit_check(GifMakeAnimationPlan(2, 2, cm, &pf, 65, 0, &code) ==
                   NULL, "too many frames rejected");
        unit_check(GifMakeAnimationPlan(2, 2, cm, &pf, 1, 65536, &code) ==
                   NULL, "loop out of range rejected");
        {
            GifPlanFrame bad = pf;

            bad.DelayTime = 70000;
            unit_check(GifMakeAnimationPlan(2, 2, cm, &bad, 1, 0, &code) ==
                       NULL, "delay out of range rejected");
        }
        {
            static unsigned char illegal[4] = {1, 4, 1, 1};
            GifPlanFrame bad;

            bad.Pixels = illegal;
            bad.Stride = 2;
            bad.DelayTime = 0;
            unit_check(GifMakeAnimationPlan(2, 2, cm, &bad, 1, 0, &code) ==
                       NULL, "out-of-palette index rejected");
        }
        plan = GifMakeAnimationPlan(2, 2, cm, &pf, 1, 65535, &code);
        unit_check(plan != NULL, "boundary values accepted");
        GifFreeAnimationPlan(plan);
        GifFreeAnimationPlan(NULL);   /* NULL free is a no-op */
        GifFreeMapObject(cm);
    }

    /* palette size boundaries: 2 and 256 entries, both powers of two */
    {
        GifColorType colors256[256];
        ColorMapObject *cm2, *cm256;
        static unsigned char two_pix[2] = {1, 0};
        static unsigned char big_pix[256];
        GifPlanFrame pf2, pf256;
        GifAnimationPlan *plan;
        int err = 0, q;

        for (q = 0; q < 256; q++) {
            colors256[q].Red = (GifByteType) q;
            colors256[q].Green = (GifByteType) (255 - q);
            colors256[q].Blue = 0;
            big_pix[q] = (GifByteType) q;   /* exercises index 255 */
        }
        cm2 = GifMakeMapObject(2, colors256);
        cm256 = GifMakeMapObject(256, colors256);
        unit_check(cm2 != NULL && cm256 != NULL,
                   "2- and 256-entry colormaps allocate");
        pf2.Pixels = two_pix;
        pf2.Stride = 2;
        pf2.DelayTime = 0;
        plan = GifMakeAnimationPlan(2, 1, cm2, &pf2, 1, 0, &err);
        unit_check(plan != NULL, "2-color plan accepted");
        GifFreeAnimationPlan(plan);
        pf256.Pixels = big_pix;
        pf256.Stride = 256;
        pf256.DelayTime = 65535;
        plan = GifMakeAnimationPlan(256, 1, cm256, &pf256, 1, 65535, &err);
        unit_check(plan != NULL, "256-color plan with index 255 accepted");
        unit_check(plan != NULL && plan->Data[plan->Size - 1] == 0x3b,
                   "256-color stream terminates correctly");
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm2);
        GifFreeMapObject(cm256);
    }

    /* the planner never mutates its input canvases */
    {
        ColorMapObject *cm = make_map4();
        static const unsigned char src[2][6] = {
            {1, 2, 0, 1, 0, 0},
            {0, 2, 2, 1, 0, 0}
        };
        unsigned char snapshot[2][6];
        GifPlanFrame pf[2];
        GifAnimationPlan *plan;
        int err = 0;

        memcpy(snapshot, src, sizeof(src));
        pf[0].Pixels = src[0];
        pf[0].Stride = 3;
        pf[0].DelayTime = 13;
        pf[1].Pixels = src[1];
        pf[1].Stride = 3;
        pf[1].DelayTime = 14;
        plan = GifMakeAnimationPlan(3, 2, cm, pf, 2, 5, &err);
        unit_check(plan != NULL, "immutability plan built");
        unit_check(memcmp(src, snapshot, sizeof(src)) == 0,
                   "input pixel bytes are never modified");
        GifFreeAnimationPlan(plan);
        GifFreeMapObject(cm);
    }

    /* the two plan error codes have giflib-style descriptions */
    unit_check(GifErrorString(E_GIF_PLAN_ERR_BAD_ARGUMENT) != NULL,
               "bad-argument error has a description");
    unit_check(GifErrorString(E_GIF_PLAN_ERR_NO_REPRESENTATION) != NULL,
               "no-representation error has a description");
}
