/******************************************************************************

gif_plan.h - whole-animation planning for GIF89a output

Given a sequence of full indexed display canvases that share one global
color map, GifMakeAnimationPlan() searches a fixed set of legal GIF frame
representations and returns the representation whose rendered GIF89a byte
stream is shortest.  Ties are resolved by a documented lexicographic rule,
so the result is unique for any legal input.

The input frames are complete canvases (what the display should show at
that instant), not difference images.  Index 0 in the global color map is
reserved for transparency: a zero input pixel means "transparent here" and
any non-zero index names a real color.  No quantization, palette merging,
or lossy simplification is performed; the rendered output composites to
the input canvases pixel for pixel.

*****************************************************************************/

#ifndef _GIF_PLAN_H_
#define _GIF_PLAN_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>
#include "gif_lib.h"

/* Limits of the planner, matching the task contract. */
#define GIF_PLAN_MIN_DIMENSION    1
#define GIF_PLAN_MAX_DIMENSION    256
#define GIF_PLAN_MIN_FRAMES       1
#define GIF_PLAN_MAX_FRAMES       64
#define GIF_PLAN_MIN_COLORS       2
#define GIF_PLAN_MAX_COLORS       256
#define GIF_PLAN_TRANSPARENT      0
#define GIF_PLAN_MAX_LOOP         0xffff
#define GIF_PLAN_MAX_DELAY        0xffff

/*
 * One input frame: the complete indexed canvas that must be on display
 * while this frame is showing.  Pixels is read-only and is never modified;
 * rows are Stride bytes apart (Stride may exceed Width).  DelayTime is in
 * hundredths of a second, matching GraphicsControlBlock.DelayTime.
 */
typedef struct GifPlanFrame {
    const GifPixelType *Pixels;
    size_t Stride;
    int DelayTime;
} GifPlanFrame;

/*
 * The representation chosen for one input frame.  The fields describe
 * exactly what was emitted into the returned byte stream:
 *
 *   Left, Top, Width, Height - image descriptor rectangle (always one of
 *                              the two candidate rectangles for the frame)
 *   DisposalMode             - DISPOSE_DO_NOT (1), DISPOSE_BACKGROUND (2)
 *                              or DISPOSE_PREVIOUS (3)
 *   Transparentize           - true if pixels inside the rectangle equal
 *                              to the canvas on entry were rewritten to
 *                              index 0 (transparent coverage), false to
 *                              copy the rectangle verbatim
 *   DelayTime                - the input delay, written unchanged
 */
typedef struct GifPlanStep {
    int Left, Top, Width, Height;
    int DisposalMode;
    bool Transparentize;
    int DelayTime;
} GifPlanStep;

/*
 * The finished plan.  Data points at Size bytes of a complete GIF89a
 * stream (header, global color map, NETSCAPE2.0 loop extension, one image
 * per input frame, trailer) and Steps has FrameCount entries describing
 * each emitted image.  Both allocations, and the structure itself, are
 * released with GifFreeAnimationPlan().
 */
typedef struct GifAnimationPlan {
    unsigned char *Data;
    size_t Size;
    GifPlanStep *Steps;
    int FrameCount;
    int LoopCount;
    /* Planner telemetry: widest frontier of distinct canvas states held
       at any frame boundary, and number of times the EGif LZW compressor
       was invoked to price a candidate image.  Both stay linear in the
       frame count; the search never enumerates full action sequences. */
    long MaxStates;
    long CompressionCount;
} GifAnimationPlan;

/*
 * Build the shortest legal GIF89a representation of a full-canvas
 * animation.
 *
 *   Width, Height  - canvas dimensions, 1..256 each
 *   ColorMap       - global color map, 2..256 entries, power of two;
 *                    index 0 is the transparent index
 *   Frames         - FrameCount complete indexed canvases, never modified
 *   FrameCount     - 1..64
 *   LoopCount      - NETSCAPE2.0 loop count, 0..65535 (0 = loop forever)
 *
 * Returns a malloc(3)-held plan, or NULL with *ErrorCode set to an
 * E_GIF_ERR_* value from gif_lib.h on a bad argument or allocation
 * failure.  Release a non-NULL result with GifFreeAnimationPlan().
 */
GifAnimationPlan *GifMakeAnimationPlan(int Width, int Height,
                                       const ColorMapObject *ColorMap,
                                       const GifPlanFrame *Frames,
                                       int FrameCount, int LoopCount,
                                       int *ErrorCode);

/* Free a plan returned by GifMakeAnimationPlan(); NULL is accepted. */
void GifFreeAnimationPlan(GifAnimationPlan *Plan);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* _GIF_PLAN_H_ */

/* end */
