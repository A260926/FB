/*
 * normaliz0r.c
 * Copyright (C) 2017 Chungzuwalla
 * This file is a Frei0r plugin.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Normalize video (aka histogram stretching, contrast stretching).
 * See: https://en.wikipedia.org/wiki/Normalization_(image_processing)
 *
 * For each channel of each frame, the filter computes the input range and maps
 * it linearly to the user-specified output range.  The output range defaults
 * to the full dynamic range from pure black to pure white.
 *
 * The filter can apply temporal smoothing to the input range to reduce rapid
 * changes in brightness (flickering) caused by small dark or bright objects
 * entering or leaving the scene.  This effect is similar to the auto-exposure
 * on a video camera.
 *
 * The filter can normalize the R,G,B channels independently, which may cause
 * color shifting, or link them together as a single channel, which prevents
 * color shifting.  More precisely, linked normalization preserves hues (as
 * defined in HSV/HSL color spaces) while independent normalization does not.
 * Independent normalization can be used to remove color casts, such as the
 * blue cast from underwater video, restoring more natural colors.  The filter
 * can also combine independent and linked normalization in any ratio.
 *
 * Finally the overall strength of the filter can be adjusted, from no effect
 * to full normalization.
 *
 * The 5 user parameters are:
 *   BlackPt,   Colors which define the output range.  The minimum input value
 *   WhitePt    is mapped to the BlackPt.  The maximum input value is mapped to
 *              the WhitePt.  The defaults are black and white respectively.
 *              Specifying white for BlackPt and black for WhitePt will give
 *              color-inverted, normalized video.  Shades of grey can be used
 *              to reduce the dynamic range (contrast). Specifying saturated
 *              colors here can create some interesting effects.
 *
 *   Smoothing  The amount of temporal smoothing.  The parameter is converted
 *              into a number of frames N in the range [1,MAX_HISTORY_LEN], and
 *              the minimum and maximum input value of each channel are then
 *              smoothed using an arithmetic average over the last N retained
 *              frames.  Defaults to 0.0 (no temporal smoothing).
 *
 *   Independence
 *              Controls the ratio of independent (color shifting) channel
 *              normalization to linked (color preserving) normalization.  0.0
 *              is fully linked, 1.0 is fully independent.  Defaults to fully
 *              independent.
 *
 *   Strength   Overall strength of the filter.  1.0 is full strength.  0.0 is
 *              a rather expensive no-op.  Values in between can give a gentle
 *              boost to low-contrast video without creating an artificial
 *              over-processed look.  The default is full strength.
 *
 * Temporal history and host time handling
 * ---------------------------------------
 * The filter retains the per-channel extrema (never full frames) of the at
 * most MAX_HISTORY_LEN most recent distinct video times.  The video time
 * passed by the host is handled like this:
 *
 *   - a time strictly greater than the previous one appends a new entry;
 *   - the exact same time replaces the current entry, so a redraw is neither
 *     weighted twice nor occupies an extra history slot;
 *   - a time smaller than the previous one starts a new playback segment and
 *     discards every previously retained entry;
 *   - a non-finite time (NaN/infinity) is processed as a standalone frame and
 *     clears the history; the next finite time starts afresh.
 *
 * Changing the Smoothing parameter never clears the history: shrinking the
 * window takes effect immediately, while growing it can reuse entries that
 * are still retained.  Only the input extrema are smoothed; the linked /
 * independent mixing, black and white points, Strength, quantization and
 * clamping below keep operating on the current frame's pixels.
 */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#include "frei0r.h"
#include "frei0r/math.h"

#define MAX_HISTORY_LEN     128

/* Per-channel input extrema observed at one video time. */
typedef struct
{
  uint8_t min[3];
  uint8_t max[3];
} history_entry_t;

typedef struct
{
  int num_pixels;     // Number of pixels in a frame.

  // Ring of extrema, one entry per distinct video time.  The next append
  // writes at history_head; chronological entry j (0 = oldest) lives in slot
  // (history_head - history_count + j) mod MAX_HISTORY_LEN.
  history_entry_t history[MAX_HISTORY_LEN];
  int history_count;      // Entries currently stored, [0,MAX_HISTORY_LEN].
  int history_head;       // Slot where the next append is written.
  double last_time;       // Video time of the newest stored entry.
  int have_last_time;     // Whether the history currently holds any entry.

  // Per-extrema, per-channel frei0r params: target output values [0,255].
  struct
  {
    float param;
  } min[3], max[3];

  // Per-instance frei0r params, each a float in the range [0,1].
  int history_len;      // Smoothing window N, [1,MAX_HISTORY_LEN].
  float independence;   // Ratio of independent vs linked normalization [0,1].
  float strength;       // Mixing strength for the normalization [0,1].
} normaliz0r_instance_t;

static void
clear_history (normaliz0r_instance_t* inst)
{
  inst->history_count = 0;
  inst->history_head = 0;
  inst->have_last_time = 0;
}

/* Append a new video time, overwriting the oldest entry once the ring is
 * full.  Called only for a strictly increasing, finite time. */
static void
append_history (normaliz0r_instance_t* inst, const uint8_t mn[3],
                const uint8_t mx[3])
{
  history_entry_t* entry = &inst->history[inst->history_head];
  int c;
  for (c = 0; c < 3; c++)
  {
    entry->min[c] = mn[c];
    entry->max[c] = mx[c];
  }
  inst->history_head = (inst->history_head + 1) % MAX_HISTORY_LEN;
  if (inst->history_count < MAX_HISTORY_LEN)
    inst->history_count++;
}

/* Replace the extrema of the current (newest) video time, without changing
 * the number of retained entries. */
static void
replace_newest_history (normaliz0r_instance_t* inst, const uint8_t mn[3],
                        const uint8_t mx[3])
{
  int slot = (inst->history_head - 1 + MAX_HISTORY_LEN) % MAX_HISTORY_LEN;
  history_entry_t* entry = &inst->history[slot];
  int c;
  for (c = 0; c < 3; c++)
  {
    entry->min[c] = mn[c];
    entry->max[c] = mx[c];
  }
}

int
f0r_init ()
{
  return 1;
}

void
f0r_deinit ()
{
}

void
f0r_get_plugin_info (f0r_plugin_info_t* info)
{
  info->name = "Normaliz0r";
  info->author = "Chungzuwalla (chungzuwalla\100rling.com)";
  info->plugin_type = F0R_PLUGIN_TYPE_FILTER;
  info->color_model = F0R_COLOR_MODEL_RGBA8888;
  info->frei0r_version = FREI0R_MAJOR_VERSION;
  info->major_version = 0;
  info->minor_version = 1;
  info->num_params = 5;
  info->explanation = "Normalize (aka histogram stretch, contrast stretch)";
}

static char *param_infos[] =
{
  "BlackPt",      "Output color to which darkest input color is mapped (default black)",
  "WhitePt",      "Output color to which brightest input color is mapped (default white)",
  "Smoothing",    "Amount of temporal smoothing of the input range, to reduce flicker (default 0.0)",
  "Independence", "Proportion of independent to linked channel normalization (default 1.0)",
  "Strength",     "Strength of filter, from no effect to full normalization (default 1.0)",
};

void
f0r_get_param_info (f0r_param_info_t* info, int param_index)
{
  info->name = param_infos[param_index * 2];
  info->type = (param_index <= 1) ? F0R_PARAM_COLOR : F0R_PARAM_DOUBLE;
  info->explanation = param_infos[param_index * 2 + 1];
}

f0r_instance_t
f0r_construct (unsigned int width, unsigned int height)
{
  normaliz0r_instance_t* inst = (normaliz0r_instance_t*)calloc(1, sizeof(*inst));
  int c;
  inst->num_pixels = width * height;
  clear_history(inst);
  for (c = 0; c < 3; c++)
  {
    // Set default per-channel frei0r param values.
    inst->min[c].param = 0.0;   // black
    inst->max[c].param = 255.0; // white
  }
  // Set default per-instance frei0r param values.
  inst->history_len = 1;        // [1,MAX_HISTORY_LEN]; default is no smoothing
  inst->independence = 1.0;     // [0,1]; default is fully independent
  inst->strength = 1.0;         // [0,1]; default is full strength
  return (f0r_instance_t)inst;
}

void
f0r_destruct (f0r_instance_t instance)
{
  free (instance);
}

void
f0r_set_param_value (f0r_instance_t instance, f0r_param_t param,
                     int param_index)
{
  assert(instance);
  normaliz0r_instance_t* inst = (normaliz0r_instance_t*)instance;
  float val;

  switch (param_index)
    {
    case 0:
      inst->min[0].param = ((f0r_param_color_t*)param)->r * 255.0;
      inst->min[1].param = ((f0r_param_color_t*)param)->g * 255.0;
      inst->min[2].param = ((f0r_param_color_t*)param)->b * 255.0;
      break;
    case 1:
      inst->max[0].param = ((f0r_param_color_t*)param)->r * 255.0;
      inst->max[1].param = ((f0r_param_color_t*)param)->g * 255.0;
      inst->max[2].param = ((f0r_param_color_t*)param)->b * 255.0;
      break;
    case 2:
      val = (float)CLAMP(*((double* )param), 0.0, 1.0);
      // Map [0,1] <-> [1,MAX_HISTORY_LEN].  Only the window size changes;
      // the retained history is left untouched.
      inst->history_len = (int)(val * (MAX_HISTORY_LEN - 1)) + 1;
      break;
    case 3:
      val = (float)CLAMP(*((double* )param), 0.0, 1.0);
      inst->independence = val;
      break;
    case 4:
      val = (float)CLAMP(*((double* )param), 0.0, 1.0);
      inst->strength = val;
      break;
    }
}

void
f0r_get_param_value (f0r_instance_t instance, f0r_param_t param,
                     int param_index)
{
  assert(instance);
  normaliz0r_instance_t* inst = (normaliz0r_instance_t*)instance;
  switch (param_index)
    {
    case 0:
      ((f0r_param_color_t*)param)->r = inst->min[0].param / 255.0;
      ((f0r_param_color_t*)param)->g = inst->min[1].param / 255.0;
      ((f0r_param_color_t*)param)->b = inst->min[2].param / 255.0;
      break;
    case 1:
      ((f0r_param_color_t*)param)->r = inst->max[0].param / 255.0;
      ((f0r_param_color_t*)param)->g = inst->max[1].param / 255.0;
      ((f0r_param_color_t*)param)->b = inst->max[2].param / 255.0;
      break;
    case 2:
      // Map [0,1] <-> [1,MAX_HISTORY_LEN]
      *((double*)param) = (double)(inst->history_len - 1) / (MAX_HISTORY_LEN - 1);
      break;
    case 3:
      *((double*)param) = inst->independence;
      break;
    case 4:
      *((double*)param) = inst->strength;
      break;
    }
}

void
f0r_update (f0r_instance_t instance, double time, const uint32_t* inframe,
            uint32_t* outframe)
{
  assert(instance);
  normaliz0r_instance_t* inst = (normaliz0r_instance_t*)instance;
  int c;

  // Per-extrema, per-channel local variables.
  struct
  {
    uint8_t in;                 // Original input byte value for this frame.
    float smoothed;             // Smoothed input value [0,255].
    float out;                  // Output value [0,255].
  } min[3], max[3];             // Min and max for each channel in {R,G,B}.

  // First, scan the input frame to find, for each channel, the minimum
  // (min.in) and maximum (max.in) values present in the channel.
  {
    const uint8_t *in = (const uint8_t*)inframe;

#define INIT(c)     (min[c].in = max[c].in = in[c])
#define EXTEND(c)   (min[c].in = MIN(min[c].in, in[c])), \
                    (max[c].in = MAX(max[c].in, in[c]))
    INIT(0);
    INIT(1);
    INIT(2);
    in += 4;
    // Process (num_pixels - 1), as we used the first pixel to initialize
    unsigned int num_pixels = inst->num_pixels - 1;
    while (num_pixels--)
    {
      EXTEND(0);
      EXTEND(1);
      EXTEND(2);
      in += 4;
    }
  }

  // Apply the shared host-time rules.  A non-finite time is a standalone
  // frame: the history is cleared and this frame alone feeds the smoothing.
  uint8_t cur_min[3] = { min[0].in, min[1].in, min[2].in };
  uint8_t cur_max[3] = { max[0].in, max[1].in, max[2].in };
  int standalone = !isfinite(time);
  if (standalone)
  {
    clear_history(inst);
  }
  else if (!inst->have_last_time)
  {
    append_history(inst, cur_min, cur_max);
    inst->last_time = time;
    inst->have_last_time = 1;
  }
  else if (time == inst->last_time)
  {
    // Redraw of the same video time: replace, do not count twice.
    replace_newest_history(inst, cur_min, cur_max);
  }
  else if (time < inst->last_time)
  {
    // Time went backwards: a new playback segment, old frames are gone.
    clear_history(inst);
    append_history(inst, cur_min, cur_max);
    inst->last_time = time;
    inst->have_last_time = 1;
  }
  else
  {
    append_history(inst, cur_min, cur_max);
    inst->last_time = time;
  }

  // Average the retained input extrema over the most recent N entries (or all
  // of them if fewer than N are available).  The sums are rebuilt from the
  // ring on every call so that resizing the window never inherits stale
  // totals, and a redrawn frame is weighted exactly once.
  {
    int num_history_vals;
    int start_slot;
    if (standalone)
    {
      num_history_vals = 1;
      start_slot = -1;      // Flag: use the current frame's extrema directly.
    }
    else
    {
      num_history_vals = MIN(inst->history_len, inst->history_count);
      start_slot = (inst->history_head - num_history_vals + MAX_HISTORY_LEN)
                   % MAX_HISTORY_LEN;
    }

    for (c = 0; c < 3; c++)
    {
      uint32_t min_sum = 0;
      uint32_t max_sum = 0;
      int j;
      if (standalone)
      {
        min[c].smoothed = (float)min[c].in;
        max[c].smoothed = (float)max[c].in;
        continue;
      }
      for (j = 0; j < num_history_vals; j++)
      {
        const history_entry_t* entry =
            &inst->history[(start_slot + j) % MAX_HISTORY_LEN];
        min_sum += entry->min[c];
        max_sum += entry->max[c];
      }
      min[c].smoothed = (float)min_sum / (float)num_history_vals;
      max[c].smoothed = (float)max_sum / (float)num_history_vals;
    }
  }

  // Determine the input range for linked normalization.  This is simply the
  // minimum of the per-channel minimums, and the maximum of the per-channel
  // maximums.
  float rgb_min_smoothed = min[0].smoothed;
  float rgb_max_smoothed = max[0].smoothed;
  rgb_min_smoothed = MIN(rgb_min_smoothed, min[1].smoothed);
  rgb_max_smoothed = MAX(rgb_max_smoothed, max[1].smoothed);
  rgb_min_smoothed = MIN(rgb_min_smoothed, min[2].smoothed);
  rgb_max_smoothed = MAX(rgb_max_smoothed, max[2].smoothed);

  // Now, process each channel to determine the input and output range and
  // build the lookup tables.
  uint8_t lut[3][256];
  for (c = 0; c < 3; c++)
  {
    // Adjust the input range for this channel [min.smoothed,max.smoothed] by
    // mixing in the correct proportion of the linked normalization input range
    // [rgb_min_smoothed,rgb_max_smoothed].
    min[c].smoothed = (min[c].smoothed * inst->independence)
        + (rgb_min_smoothed * (1.0 - inst->independence));
    max[c].smoothed = (max[c].smoothed * inst->independence)
        + (rgb_max_smoothed * (1.0 - inst->independence));

    // Calculate the output range [min.out,max.out] as a ratio of the full-
    // strength output range [min.param,max.param] and the original input
    // range [min.in,max.in], based on the user-specified filter strength.
    min[c].out = (inst->min[c].param * inst->strength)
        + ((float)min[c].in * (1.0 - inst->strength));
    max[c].out = (inst->max[c].param * inst->strength)
        + ((float)max[c].in * (1.0 - inst->strength));

    // Now, build a lookup table which linearly maps the adjusted input range
    // [min.smoothed,max.smoothed] to the output range [min.out,max.out].
    // Perform the linear interpolation for each x:
    //     lut[x] = (int)(float(x - min.smoothed) * scale + max.out + 0.5)
    // where scale = (max.out - min.out) / (max.smoothed - min.smoothed)
    if (min[c].smoothed == max[c].smoothed)
    {
      // There is no dynamic range to expand.  No mapping for this channel.
      int in_val;
      for (in_val = min[c].in; in_val <= max[c].in; in_val++)
        lut[c][in_val] = min[c].out;
    }
    else
    {
      // We must set lookup values for all values in the original input range
      // [min.in,max.in].  Since the original input range may be larger than
      // [min.smoothed,max.smoothed], some output values may fall outside the
      // [0,255] dynamic range.  We need to CLAMP() them.
      float scale = (max[c].out - min[c].out) / (max[c].smoothed - min[c].smoothed);
      int in_val;
      for (in_val = min[c].in; in_val <= max[c].in; in_val++)
      {
        int out_val = ROUND((in_val - min[c].smoothed) * scale + min[c].out);
        lut[c][in_val] = CLAMP(out_val, 0, 255);
      }
    }
  }

  // Finally, process the pixels of the input frame using the lookup tables.
  // Copy alpha as-is.  The LUTs are fully built before any output is written,
  // so inframe and outframe may point at the same buffer.
  {
    const uint8_t *in = (const uint8_t*)inframe;
    uint8_t *out = (uint8_t*)outframe;

#define MAP(c)      (out[c] = lut[c][in[c]])

    unsigned int num_pixels = inst->num_pixels;
    while (num_pixels--)
    {
      MAP(0);
      MAP(1);
      MAP(2);
      out[3] = in[3];  // copy alpha
      in += 4;
      out += 4;
    }
  }
}
