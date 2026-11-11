# Normaliz0r

Normalize video (histogram stretching / contrast stretching). For each frame
the input range of every channel is mapped linearly onto a configurable
output range, with optional temporal smoothing of the input range, linked or
independent channel normalization, and an adjustable overall strength.

Written by Chungzuwalla and released under the GNU GPL.

## Parameters

The five parameters keep their original indices, defaults and value mapping:

1. **BlackPt** (color, default black): output color for the darkest input.
2. **WhitePt** (color, default white): output color for the brightest input.
3. **Smoothing** (double, default 0.0): amount of temporal smoothing of the
   input range. The value maps linearly to a window length
   `N = 1 + round(value * 127)`, i.e. `[0,1] -> [1,128]` frames.
4. **Independence** (double, default 1.0): 0.0 fully links the R/G/B ranges
   together (hue preserving), 1.0 normalizes each channel independently.
5. **Strength** (double, default 1.0): mix between the unmodified input and
   the full normalization.

## Temporal smoothing and parameter animation

Only the per-channel input extrema (minimum and maximum) are retained, never
full frames: the instance stores the extrema of at most the 128 most recent
distinct video times. For each output frame the smoothed input range is the
arithmetic mean of the extrema over the most recent `N` retained entries,
using however many entries exist when fewer than `N` are available.

Smoothing applies to the *input* extrema only. The linked/independent mix,
black and white points, Strength, quantization, clamping and alpha passthrough
keep operating on the current frame with the original formulas; already
processed output frames are never blended.

Animating **Smoothing** never clears the retained history:

- shrinking the window takes effect on the next frame;
- growing the window immediately reuses extrema that are still retained
  (up to the 128-entry limit);
- changing any other parameter also leaves the history untouched.

## Host time behavior

The video time passed to `f0r_update` follows the same rules as the other
temporal filters in this package:

- a time strictly greater than the previous one appends a new entry;
- the exact same time replaces the current entry, so a redraw of the same
  frame is weighted exactly once and does not occupy an extra history slot;
- a time smaller than the previous one starts a new playback segment and
  discards all previously retained entries;
- a non-finite time (NaN or infinity) is processed as a standalone single
  frame and clears the history; the next finite time starts afresh.

Input and output buffers may overlap (in-place updates), multiple instances
may be interleaved, and each construction starts with an empty history.
