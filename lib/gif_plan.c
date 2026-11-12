/******************************************************************************

gif_plan.c - shortest-representation planning for full-canvas GIF89a
             animations

The search space is fixed and small per frame:

  * two candidate rectangles (the full canvas, and the bounding box of
    the changed-to-previous plus going-transparent-to-next positions);
  * two fill modes for a rectangle (verbatim, or rewrite the pixels equal
    to the entry canvas to the transparent index 0);
  * three disposal modes (do not / background / previous).

A dynamic program over frame boundaries keeps, for every distinct entry
canvas, only the cheapest way to reach it.  Equal canvases are merged, so
repeated flicker cannot make the frontier grow exponentially.  Every
candidate image is priced by the real EGif LZW compressor writing into a
memory buffer; the objective function is the rendered GIF byte count,
never a pixel or entropy estimate.

*****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "gif_lib.h"
#include "gif_plan.h"

#define GCE_BYTES 8             /* introducer+label, 4 data bytes, terminator */
#define NUM_DISPOSALS 3
#define NUM_FILLS 2
#define NUM_RECTS 2
#define NUM_CANDS (NUM_RECTS * NUM_DISPOSALS * NUM_FILLS)

typedef struct {
    int left, top, width, height;
} Rect;

typedef struct {
    Rect rect;
    int transparentize;       /* true: unchanged pixels become index 0 */
    int disposal;             /* DISPOSE_DO_NOT / BACKGROUND / PREVIOUS */
    long verbatim_price;      /* cached entry-independent image price */
} Cand;

typedef struct {
    unsigned char *p;
    size_t n;
} MemBuf;

typedef struct {
    int w, h, nframes, ncolors, loop;
    const ColorMapObject *cm;
    const GifPlanFrame *in;
    size_t npix;
    GifPixelType *inputs;     /* nframes dense canvases plus one zero slot */
    GifPixelType *line;       /* widest rectangle row */
    Cand cands[GIF_PLAN_MAX_FRAMES * NUM_CANDS];
    int ncand[GIF_PLAN_MAX_FRAMES];
    MemBuf mb;
    long compress_calls;
    int error;
} Planner;

typedef struct State {
    GifPixelType *canvas;     /* entry canvas for the next frame */
    long cost;
    int *path;                /* candidate index chosen for each past frame */
} State;

typedef struct {
    State *items;
    int count, cap;
} Frontier;

static int
mem_write(GifFileType *g, const GifByteType *p, int n)
{
    MemBuf *b = (MemBuf *) g->UserData;
    unsigned char *q;

    if (n <= 0)
        return n;
    q = (unsigned char *) realloc(b->p, b->n + (size_t) n);
    if (q == NULL)
        return 0;
    b->p = q;
    memcpy(q + b->n, p, (size_t) n);
    b->n += (size_t) n;
    return n;
}

static int
rect_equal(const Rect *a, const Rect *b)
{
    return a->left == b->left && a->top == b->top &&
           a->width == b->width && a->height == b->height;
}

/* Order rectangles the way the lexicographic tie-break reads them. */
static int
rect_less(const Rect *a, const Rect *b)
{
    if (a->left != b->left)
        return a->left < b->left;
    if (a->top != b->top)
        return a->top < b->top;
    if (a->width != b->width)
        return a->width < b->width;
    return a->height < b->height;
}

static const GifPixelType *
frame_canvas(const Planner *pl, int i)
{
    return pl->inputs + (size_t) i * pl->npix;
}

static GifPixelType *
frame_canvas_mut(Planner *pl, int i)
{
    return pl->inputs + (size_t) i * pl->npix;
}

/*
 * The two legal rectangles for frame i: the full canvas and the bounding
 * box of (positions changed versus the previous input) union (positions
 * that are non-zero now and zero in the next input).  The previous input
 * of the first frame is the all-transparent canvas; the last frame has
 * no successor.
 */
static void
frame_rects(Planner *pl, int i, Rect out[NUM_RECTS])
{
    const GifPixelType *cur = frame_canvas(pl, i);
    const GifPixelType *prev = frame_canvas(pl, pl->nframes);
    const GifPixelType *next = NULL;
    int minx = pl->w, miny = pl->h, maxx = -1, maxy = -1;
    size_t p;

    if (i > 0)
        prev = frame_canvas(pl, i - 1);
    if (i + 1 < pl->nframes)
        next = frame_canvas(pl, i + 1);

    for (p = 0; p < pl->npix; p++) {
        int marked = cur[p] != prev[p];

        if (!marked && next != NULL && cur[p] != 0 && next[p] == 0)
            marked = 1;
        if (marked) {
            int x = (int) (p % (size_t) pl->w);
            int y = (int) (p / (size_t) pl->w);

            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x > maxx) maxx = x;
            if (y > maxy) maxy = y;
        }
    }

    out[0].left = 0;
    out[0].top = 0;
    out[0].width = pl->w;
    out[0].height = pl->h;
    if (maxx < 0) {                /* empty set: origin 1x1 */
        out[1].left = 0;
        out[1].top = 0;
        out[1].width = 1;
        out[1].height = 1;
    } else {
        out[1].left = minx;
        out[1].top = miny;
        out[1].width = maxx - minx + 1;
        out[1].height = maxy - miny + 1;
    }
}

/*
 * Price one image section with the real EGif compressor: image
 * descriptor (10 bytes), the LZW minimum code size byte, every sub-block
 * and the block terminator.  The transparentized raster depends on the
 * entry canvas, so this must be invoked for the actual transition being
 * priced; the verbatim raster is entry-independent and the caller caches
 * its price.  Returns -1 and records pl->error on failure.
 */
static long
price_image(Planner *pl, int i, const Cand *c, const GifPixelType *entry)
{
    GifFileType *g;
    int err = 0;
    size_t prefix;
    long cost;
    int yy, xx;

    pl->mb.p = NULL;
    pl->mb.n = 0;
    g = EGifOpen(&pl->mb, mem_write, &err);
    if (g == NULL) {
        pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
        return -1;
    }
    EGifSetGifVersion(g, true);
    if (EGifPutScreenDesc(g, pl->w, pl->h, GifBitSize(pl->ncolors),
                          0, pl->cm) == GIF_ERROR)
        goto fail;
    prefix = pl->mb.n;
    if (EGifPutImageDesc(g, c->rect.left, c->rect.top,
                         c->rect.width, c->rect.height,
                         false, NULL) == GIF_ERROR)
        goto fail;
    pl->compress_calls++;

    for (yy = 0; yy < c->rect.height; yy++) {
        int y = c->rect.top + yy;

        for (xx = 0; xx < c->rect.width; xx++) {
            int x = c->rect.left + xx;
            size_t p = (size_t) y * (size_t) pl->w + (size_t) x;
            GifPixelType v = frame_canvas(pl, i)[p];

            if (c->transparentize && entry[p] == v)
                v = GIF_PLAN_TRANSPARENT;
            pl->line[xx] = v;
        }
        if (EGifPutLine(g, pl->line, c->rect.width) == GIF_ERROR)
            goto fail;
    }
    if (EGifCloseFile(g, &err) == GIF_ERROR) {
        g = NULL;
        free(pl->mb.p);
        pl->mb.p = NULL;
        pl->mb.n = 0;
        pl->error = E_GIF_ERR_CLOSE_FAILED;
        return -1;
    }
    cost = (long) (pl->mb.n - prefix - 1);      /* minus the trailer byte */
    free(pl->mb.p);
    pl->mb.p = NULL;
    pl->mb.n = 0;
    return cost;

fail:
    err = g->Error;
    EGifCloseFile(g, &err);
    free(pl->mb.p);
    pl->mb.p = NULL;
    pl->mb.n = 0;
    pl->error = err ? err : E_GIF_ERR_WRITE_FAILED;
    return -1;
}

/*
 * Apply a candidate to an entry canvas and verify the displayed canvas is
 * exactly frame i.  On success, fill next_entry (the canvas after the
 * candidate's disposal takes effect at the next frame boundary).
 *
 * Index 0 only means "do not cover": it can never erase an old pixel.
 * DISPOSE_PREVIOUS restores the canvas held from before this frame.
 */
static int
apply_cand(Planner *pl, int i, const Cand *c, const GifPixelType *entry,
           GifPixelType *display, GifPixelType *next_entry)
{
    const GifPixelType *target = frame_canvas(pl, i);
    int yy, xx;

    memcpy(display, entry, pl->npix);
    for (yy = 0; yy < c->rect.height; yy++) {
        int y = c->rect.top + yy;

        for (xx = 0; xx < c->rect.width; xx++) {
            int x = c->rect.left + xx;
            size_t p = (size_t) y * (size_t) pl->w + (size_t) x;
            GifPixelType v = target[p];

            if (c->transparentize && entry[p] == v)
                v = GIF_PLAN_TRANSPARENT;
            if (v != GIF_PLAN_TRANSPARENT)
                display[p] = v;
        }
    }
    if (memcmp(display, target, pl->npix) != 0)
        return 0;

    switch (c->disposal) {
    case DISPOSE_DO_NOT:
        memcpy(next_entry, display, pl->npix);
        break;
    case DISPOSE_BACKGROUND:
        memcpy(next_entry, display, pl->npix);
        for (yy = 0; yy < c->rect.height; yy++) {
            int y = c->rect.top + yy;

            for (xx = 0; xx < c->rect.width; xx++) {
                int x = c->rect.left + xx;
                next_entry[(size_t) y * (size_t) pl->w + (size_t) x] =
                    GIF_PLAN_TRANSPARENT;
            }
        }
        break;
    case DISPOSE_PREVIOUS:
        memcpy(next_entry, entry, pl->npix);
        break;
    default:
        return 0;
    }
    return 1;
}

static int
frontier_grow(Frontier *f)
{
    int ncap = f->cap ? f->cap * 2 : 8;
    State *ni = (State *) reallocarray(f->items, (size_t) ncap,
                                       sizeof(State));

    if (ni == NULL)
        return -1;
    f->items = ni;
    f->cap = ncap;
    return 0;
}

static void
frontier_free(Frontier *f)
{
    int k;

    for (k = 0; k < f->count; k++) {
        free(f->items[k].canvas);
        free(f->items[k].path);
    }
    free(f->items);
    f->items = NULL;
    f->count = f->cap = 0;
}

/* Lexicographic comparison of equal-length candidate-index paths. */
static int
path_less(const int *a, const int *b, int len)
{
    int k;

    for (k = 0; k < len; k++) {
        if (a[k] != b[k])
            return a[k] < b[k];
    }
    return 0;
}

static int
validate_args(int width, int height, const ColorMapObject *cm,
              const GifPlanFrame *frames, int frame_count, int loop_count)
{
    int i;
    size_t p;

    if (width < GIF_PLAN_MIN_DIMENSION || width > GIF_PLAN_MAX_DIMENSION ||
        height < GIF_PLAN_MIN_DIMENSION || height > GIF_PLAN_MAX_DIMENSION)
        return -1;
    if (frame_count < GIF_PLAN_MIN_FRAMES ||
        frame_count > GIF_PLAN_MAX_FRAMES || frames == NULL)
        return -1;
    if (cm == NULL || cm->Colors == NULL ||
        cm->ColorCount < GIF_PLAN_MIN_COLORS ||
        cm->ColorCount > GIF_PLAN_MAX_COLORS ||
        cm->ColorCount != (1 << GifBitSize(cm->ColorCount)))
        return -1;
    if (loop_count < 0 || loop_count > GIF_PLAN_MAX_LOOP)
        return -1;
    for (i = 0; i < frame_count; i++) {
        if (frames[i].Pixels == NULL ||
            frames[i].Stride < (size_t) width ||
            frames[i].DelayTime < 0 ||
            frames[i].DelayTime > GIF_PLAN_MAX_DELAY)
            return -1;
    }
    for (i = 0; i < frame_count; i++) {
        const GifPixelType *row = frames[i].Pixels;

        for (p = 0; p < (size_t) width * (size_t) height; p++) {
            GifPixelType v = row[(p / (size_t) width) * frames[i].Stride +
                                 (p % (size_t) width)];
            if ((int) v >= cm->ColorCount)
                return -1;
        }
    }
    return 0;
}

/*
 * Build the compact candidate table for every frame.  Duplicate
 * rectangles merge into one group; slot ordering inside a frame matches
 * the tie-break tuple:
 * (rect lexicographic, disposal 1..3, transparentize false then true).
 */
static int
build_candidates(Planner *pl)
{
    int i;

    for (i = 0; i < pl->nframes; i++) {
        Rect rects[NUM_RECTS], unique[NUM_RECTS];
        int nrect, ri, fill, disp, slot = 0;
        Cand *base = pl->cands + (size_t) i * NUM_CANDS;

        frame_rects(pl, i, rects);
        if (rect_less(&rects[0], &rects[1])) {
            unique[0] = rects[0];
            unique[1] = rects[1];
        } else {
            unique[0] = rects[1];
            unique[1] = rects[0];
        }
        nrect = rect_equal(&unique[0], &unique[1]) ? 1 : NUM_RECTS;
        for (ri = 0; ri < nrect; ri++) {
            for (disp = 1; disp <= NUM_DISPOSALS; disp++) {
                for (fill = 0; fill < NUM_FILLS; fill++) {
                    Cand *c = base + slot;

                    c->rect = unique[ri];
                    c->disposal = disp;
                    c->transparentize = fill;
                    c->verbatim_price = -1;
                    slot++;
                }
            }
        }
        pl->ncand[i] = slot;          /* 6 or 12 */
    }
    return 0;
}

/* Image-section price of one transition, with verbatim caching. */
static long
transition_price(Planner *pl, int i, Cand *c,
                 const GifPixelType *entry)
{
    if (!c->transparentize) {
        if (c->verbatim_price < 0) {
            long v = price_image(pl, i, c, entry);

            if (v < 0)
                return -1;
            c->verbatim_price = v;
        }
        return c->verbatim_price;
    }
    return price_image(pl, i, c, entry);
}

/* Run the frame-boundary dynamic program; returns the winning state. */
static State *
run_search(Planner *pl, long *max_states, GifPixelType *display,
           GifPixelType *next_entry)
{
    Frontier cur = {0}, nxt = {0};
    State first, *winner = NULL;
    int i, s, k, m;

    first.canvas = (GifPixelType *) calloc(pl->npix, sizeof(GifPixelType));
    first.path = NULL;
    if (first.canvas == NULL || frontier_grow(&cur) < 0) {
        free(first.canvas);
        pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }
    first.cost = 0;
    cur.items[cur.count++] = first;
    *max_states = 1;

    for (i = 0; i < pl->nframes; i++) {
        Cand *base = pl->cands + (size_t) i * NUM_CANDS;

        nxt.count = 0;
        for (s = 0; s < cur.count; s++) {
            State *st = &cur.items[s];

            for (k = 0; k < pl->ncand[i]; k++) {
                Cand *c = base + k;
                long image_bytes, new_cost;
                int found;
                GifPixelType *canvas_copy;
                int *path_copy;

                if (!apply_cand(pl, i, c, st->canvas, display,
                                next_entry))
                    continue;
                image_bytes = transition_price(pl, i, c, st->canvas);
                if (image_bytes < 0)
                    goto fail;
                new_cost = st->cost + image_bytes + GCE_BYTES;

                found = -1;
                for (m = 0; m < nxt.count; m++) {
                    if (memcmp(nxt.items[m].canvas, next_entry,
                               pl->npix * sizeof(GifPixelType)) == 0) {
                        found = m;
                        break;
                    }
                }

                if (found < 0) {
                    State *dst;

                    if (nxt.count == nxt.cap && frontier_grow(&nxt) < 0) {
                        pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
                        goto fail;
                    }
                    canvas_copy = (GifPixelType *) malloc(
                        pl->npix * sizeof(GifPixelType));
                    path_copy = (int *) malloc(
                        ((size_t) i + 1) * sizeof(int));
                    if (canvas_copy == NULL || path_copy == NULL) {
                        free(canvas_copy);
                        free(path_copy);
                        pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
                        goto fail;
                    }
                    memcpy(canvas_copy, next_entry,
                           pl->npix * sizeof(GifPixelType));
                    if (i > 0)
                        memcpy(path_copy, st->path,
                               (size_t) i * sizeof(int));
                    path_copy[i] = k;
                    dst = &nxt.items[nxt.count++];
                    dst->canvas = canvas_copy;
                    dst->cost = new_cost;
                    dst->path = path_copy;
                } else {
                    State *dst = &nxt.items[found];

                    /* Full lexicographic order of the candidate-index
                       path, including the choice being made at frame i. */
                    int lex = k - dst->path[i];

                    if (lex == 0 && i > 0) {
                        if (path_less(st->path, dst->path, i))
                            lex = -1;
                        else if (path_less(dst->path, st->path, i))
                            lex = 1;
                    }
                    if (new_cost < dst->cost ||
                        (new_cost == dst->cost && lex < 0)) {
                        path_copy = (int *) realloc(dst->path,
                            ((size_t) i + 1) * sizeof(int));
                        if (path_copy == NULL) {
                            pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
                            goto fail;
                        }
                        dst->path = path_copy;
                        if (i > 0)
                            memcpy(dst->path, st->path,
                                   (size_t) i * sizeof(int));
                        dst->path[i] = k;
                        dst->cost = new_cost;
                    }
                }
            }
        }

        frontier_free(&cur);
        cur = nxt;
        memset(&nxt, 0, sizeof(nxt));
        if (cur.count > *max_states)
            *max_states = cur.count;
        if (cur.count == 0) {
            pl->error = E_GIF_PLAN_ERR_NO_REPRESENTATION;
            return NULL;
        }
    }

    /* The canvas after the final disposal must be fully transparent, so a
       loop restart sees the same initial all-transparent canvas. */
    for (s = 0; s < cur.count; s++) {
        State *st = &cur.items[s];
        int clear = 1;

        for (m = 0; m < (int) pl->npix; m++) {
            if (st->canvas[m] != GIF_PLAN_TRANSPARENT) {
                clear = 0;
                break;
            }
        }
        if (!clear)
            continue;
        if (winner == NULL || st->cost < winner->cost ||
            (st->cost == winner->cost &&
             path_less(st->path, winner->path, pl->nframes)))
            winner = st;
    }
    if (winner == NULL) {
        pl->error = E_GIF_PLAN_ERR_NO_REPRESENTATION;
        frontier_free(&cur);
        return NULL;
    }

    /* Detach the winner so frontier_free does not release its memory. */
    {
        int wi = (int) (winner - cur.items);
        State *out = (State *) malloc(sizeof(State));

        if (out == NULL) {
            pl->error = E_GIF_ERR_NOT_ENOUGH_MEM;
            frontier_free(&cur);
            return NULL;
        }
        *out = *winner;
        cur.items[wi].canvas = NULL;
        cur.items[wi].path = NULL;
        frontier_free(&cur);
        return out;
    }

fail:
    frontier_free(&cur);
    frontier_free(&nxt);
    return NULL;
}

/* Emit the complete GIF89a stream described by the winning path. */
static int
emit_gif(Planner *pl, const int *path, GifAnimationPlan *out)
{
    GifFileType *g;
    GifPixelType *entry, *scan;
    static const GifByteType ns_header[11] = {
        'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'
    };
    GifByteType ns_loop[3];
    int err = 0, i, yy, xx;
    size_t expected;

    entry = (GifPixelType *) calloc(pl->npix, sizeof(GifPixelType));
    scan = (GifPixelType *) malloc(pl->npix * sizeof(GifPixelType));
    if (entry == NULL || scan == NULL) {
        free(entry);
        free(scan);
        return E_GIF_ERR_NOT_ENOUGH_MEM;
    }

    pl->mb.p = NULL;
    pl->mb.n = 0;
    g = EGifOpen(&pl->mb, mem_write, &err);
    if (g == NULL) {
        free(entry);
        free(scan);
        return E_GIF_ERR_NOT_ENOUGH_MEM;
    }
    EGifSetGifVersion(g, true);
    if (EGifPutScreenDesc(g, pl->w, pl->h, GifBitSize(pl->ncolors),
                          0, pl->cm) == GIF_ERROR)
        goto emit_fail;

    if (EGifPutExtensionLeader(g, APPLICATION_EXT_FUNC_CODE) == GIF_ERROR ||
        EGifPutExtensionBlock(g, (int) sizeof(ns_header),
                              ns_header) == GIF_ERROR)
        goto emit_fail;
    ns_loop[0] = 1;
    ns_loop[1] = (GifByteType) (pl->loop & 0xff);
    ns_loop[2] = (GifByteType) ((pl->loop >> 8) & 0xff);
    if (EGifPutExtensionBlock(g, (int) sizeof(ns_loop), ns_loop) == GIF_ERROR ||
        EGifPutExtensionTrailer(g) == GIF_ERROR)
        goto emit_fail;

    expected = pl->mb.n;
    for (i = 0; i < pl->nframes; i++) {
        Cand *c = &pl->cands[(size_t) i * NUM_CANDS + path[i]];
        GraphicsControlBlock gcb;
        GifByteType ext[4];
        size_t before;

        gcb.DisposalMode = c->disposal;
        gcb.UserInputFlag = false;
        gcb.DelayTime = pl->in[i].DelayTime;
        gcb.TransparentColor = GIF_PLAN_TRANSPARENT;
        EGifGCBToExtension(&gcb, ext);
        before = pl->mb.n;
        if (EGifPutExtension(g, GRAPHICS_EXT_FUNC_CODE, 4, ext) == GIF_ERROR ||
            EGifPutImageDesc(g, c->rect.left, c->rect.top,
                             c->rect.width, c->rect.height,
                             false, NULL) == GIF_ERROR)
            goto emit_fail;
        for (yy = 0; yy < c->rect.height; yy++) {
            int y = c->rect.top + yy;

            for (xx = 0; xx < c->rect.width; xx++) {
                int x = c->rect.left + xx;
                size_t p = (size_t) y * (size_t) pl->w + (size_t) x;
                GifPixelType v = frame_canvas(pl, i)[p];

                if (c->transparentize && entry[p] == v)
                    v = GIF_PLAN_TRANSPARENT;
                pl->line[xx] = v;
            }
            if (EGifPutLine(g, pl->line, c->rect.width) == GIF_ERROR)
                goto emit_fail;
        }
        /* mb.n - before is exactly this frame's GCE plus image section. */
        expected += pl->mb.n - before;

        /* Replay the disposal to obtain the next frame's entry canvas. */
        memcpy(scan, entry, pl->npix);
        for (yy = 0; yy < c->rect.height; yy++) {
            int y = c->rect.top + yy;

            for (xx = 0; xx < c->rect.width; xx++) {
                int x = c->rect.left + xx;
                size_t p = (size_t) y * (size_t) pl->w + (size_t) x;
                GifPixelType v = frame_canvas(pl, i)[p];

                if (c->transparentize && entry[p] == v)
                    v = GIF_PLAN_TRANSPARENT;
                if (v != GIF_PLAN_TRANSPARENT)
                    scan[p] = v;
            }
        }
        if (c->disposal == DISPOSE_PREVIOUS) {
            memcpy(scan, entry, pl->npix);
        } else if (c->disposal == DISPOSE_BACKGROUND) {
            for (yy = 0; yy < c->rect.height; yy++) {
                int y = c->rect.top + yy;

                for (xx = 0; xx < c->rect.width; xx++) {
                    int x = c->rect.left + xx;
                    scan[(size_t) y * (size_t) pl->w + (size_t) x] =
                        GIF_PLAN_TRANSPARENT;
                }
            }
        }
        memcpy(entry, scan, pl->npix);
    }
    if (EGifCloseFile(g, &err) == GIF_ERROR) {
        g = NULL;
        free(entry);
        free(scan);
        free(pl->mb.p);
        pl->mb.p = NULL;
        return E_GIF_ERR_CLOSE_FAILED;
    }
    g = NULL;
    free(entry);
    free(scan);

    /* Pricing and emission use the same encoder calls; they must agree. */
    if (pl->mb.n != expected + 1) {
        free(pl->mb.p);
        pl->mb.p = NULL;
        return E_GIF_ERR_WRITE_FAILED;
    }
    out->Data = pl->mb.p;
    out->Size = pl->mb.n;
    pl->mb.p = NULL;
    return 0;

emit_fail:
    err = g->Error;
    EGifCloseFile(g, &err);
    free(entry);
    free(scan);
    free(pl->mb.p);
    pl->mb.p = NULL;
    pl->mb.n = 0;
    return err ? err : E_GIF_ERR_WRITE_FAILED;
}

GifAnimationPlan *
GifMakeAnimationPlan(int Width, int Height, const ColorMapObject *ColorMap,
                     const GifPlanFrame *Frames, int FrameCount,
                     int LoopCount, int *ErrorCode)
{
    Planner pl;
    GifAnimationPlan *plan;
    State *winner;
    GifPixelType *display, *next_entry;
    long max_states = 0;
    int i, rc;

    if (ErrorCode != NULL)
        *ErrorCode = E_GIF_SUCCEEDED;
    if (validate_args(Width, Height, ColorMap, Frames, FrameCount,
                      LoopCount) < 0) {
        if (ErrorCode != NULL)
            *ErrorCode = E_GIF_PLAN_ERR_BAD_ARGUMENT;
        return NULL;
    }

    memset(&pl, 0, sizeof(pl));
    pl.w = Width;
    pl.h = Height;
    pl.nframes = FrameCount;
    pl.ncolors = ColorMap->ColorCount;
    pl.loop = LoopCount;
    pl.cm = ColorMap;
    pl.in = Frames;
    pl.npix = (size_t) Width * (size_t) Height;
    pl.error = E_GIF_SUCCEEDED;

    /* Dense copies of the input canvases; the extra slot stays zero and
       serves as the all-transparent previous canvas and baseline. */
    pl.inputs = (GifPixelType *) calloc(((size_t) FrameCount + 1) * pl.npix,
                                        sizeof(GifPixelType));
    display = (GifPixelType *) malloc(pl.npix * sizeof(GifPixelType));
    next_entry = (GifPixelType *) malloc(pl.npix * sizeof(GifPixelType));
    pl.line = (GifPixelType *) malloc((size_t) Width * sizeof(GifPixelType));
    if (pl.inputs == NULL || display == NULL || next_entry == NULL ||
        pl.line == NULL) {
        rc = E_GIF_ERR_NOT_ENOUGH_MEM;
        goto cleanup;
    }
    for (i = 0; i < FrameCount; i++) {
        int y;

        for (y = 0; y < Height; y++) {
            memcpy(frame_canvas_mut(&pl, i) + (size_t) y * Width,
                   Frames[i].Pixels + (size_t) y * Frames[i].Stride,
                   (size_t) Width);
        }
    }

    if (build_candidates(&pl) < 0) {
        rc = pl.error;
        goto cleanup;
    }
    winner = run_search(&pl, &max_states, display, next_entry);
    if (winner == NULL) {
        rc = pl.error;
        goto cleanup;
    }

    plan = (GifAnimationPlan *) calloc(1, sizeof(GifAnimationPlan));
    if (plan == NULL) {
        rc = E_GIF_ERR_NOT_ENOUGH_MEM;
        goto cleanup_winner;
    }
    plan->Steps = (GifPlanStep *) reallocarray(NULL, (size_t) FrameCount,
                                               sizeof(GifPlanStep));
    if (plan->Steps == NULL) {
        rc = E_GIF_ERR_NOT_ENOUGH_MEM;
        free(plan);
        goto cleanup_winner;
    }
    rc = emit_gif(&pl, winner->path, plan);
    if (rc != 0) {
        free(plan->Steps);
        free(plan);
        goto cleanup_winner;
    }
    for (i = 0; i < FrameCount; i++) {
        Cand *c = &pl.cands[(size_t) i * NUM_CANDS + winner->path[i]];

        plan->Steps[i].Left = c->rect.left;
        plan->Steps[i].Top = c->rect.top;
        plan->Steps[i].Width = c->rect.width;
        plan->Steps[i].Height = c->rect.height;
        plan->Steps[i].DisposalMode = c->disposal;
        plan->Steps[i].Transparentize = c->transparentize ? true : false;
        plan->Steps[i].DelayTime = Frames[i].DelayTime;
    }
    plan->FrameCount = FrameCount;
    plan->LoopCount = LoopCount;
    plan->MaxStates = max_states;
    plan->CompressionCount = pl.compress_calls;

    rc = 0;

cleanup_winner:
    free(winner->canvas);
    free(winner->path);
    free(winner);
cleanup:
    free(pl.inputs);
    free(display);
    free(next_entry);
    free(pl.line);
    free(pl.mb.p);
    if (rc != 0) {
        if (ErrorCode != NULL)
            *ErrorCode = rc;
        return NULL;
    }
    return plan;
}

void
GifFreeAnimationPlan(GifAnimationPlan *Plan)
{
    if (Plan == NULL)
        return;
    free(Plan->Data);
    free(Plan->Steps);
    free(Plan);
}

/* end */
