/*
 * Independent reference model for normaliz0r temporal history.
 *
 * Deliberately straightforward implementation matching the header contract;
 * independent from the production plugin's code and helpers.
 */
#include "normaliz_ref.h"

#include <math.h>

void
nz_ref_init(nz_ref_t* ref, int window)
{
  ref->count = 0;
  ref->head = 0;
  ref->have_last_time = 0;
  ref->window = window;
}

void
nz_ref_set_window(nz_ref_t* ref, int window)
{
  ref->window = window;
}

static void
scan_extrema(const uint32_t* frame, int num_pixels,
             uint8_t mn[3], uint8_t mx[3])
{
  const uint8_t* p = (const uint8_t*)frame;
  int c;
  int i;
  for (c = 0; c < 3; c++)
  {
    mn[c] = p[c];
    mx[c] = p[c];
  }
  for (i = 1; i < num_pixels; i++)
  {
    for (c = 0; c < 3; c++)
    {
      uint8_t v = p[4 * i + c];
      if (v < mn[c]) mn[c] = v;
      if (v > mx[c]) mx[c] = v;
    }
  }
}

static void
ring_append(nz_ref_t* ref, const uint8_t mn[3], const uint8_t mx[3])
{
  nz_ref_entry_t* entry = &ref->ring[ref->head];
  int c;
  for (c = 0; c < 3; c++)
  {
    entry->mn[c] = mn[c];
    entry->mx[c] = mx[c];
  }
  ref->head = (ref->head + 1) % NZ_REF_MAX_HISTORY;
  if (ref->count < NZ_REF_MAX_HISTORY)
    ref->count++;
}

static void
ring_replace_newest(nz_ref_t* ref, const uint8_t mn[3], const uint8_t mx[3])
{
  int slot = (ref->head - 1 + NZ_REF_MAX_HISTORY) % NZ_REF_MAX_HISTORY;
  nz_ref_entry_t* entry = &ref->ring[slot];
  int c;
  for (c = 0; c < 3; c++)
  {
    entry->mn[c] = mn[c];
    entry->mx[c] = mx[c];
  }
}

int
nz_ref_observe(nz_ref_t* ref, double time, const uint32_t* frame,
               int num_pixels, uint8_t cur_min[3], uint8_t cur_max[3])
{
  scan_extrema(frame, num_pixels, cur_min, cur_max);

  if (!isfinite(time))
  {
    ref->count = 0;
    ref->head = 0;
    ref->have_last_time = 0;
    return 1;
  }

  if (!ref->have_last_time)
  {
    ring_append(ref, cur_min, cur_max);
    ref->last_time = time;
    ref->have_last_time = 1;
    return 0;
  }

  if (time == ref->last_time)
  {
    ring_replace_newest(ref, cur_min, cur_max);
    return 0;
  }

  if (time < ref->last_time)
  {
    ref->count = 0;
    ref->head = 0;
    ring_append(ref, cur_min, cur_max);
    ref->last_time = time;
    return 0;
  }

  ring_append(ref, cur_min, cur_max);
  ref->last_time = time;
  return 0;
}

void
nz_ref_smoothed(const nz_ref_t* ref, int standalone,
                const uint8_t cur_min[3], const uint8_t cur_max[3],
                double smoothed_min[3], double smoothed_max[3])
{
  int c;
  int j;
  int n;
  int start;

  if (standalone)
  {
    for (c = 0; c < 3; c++)
    {
      smoothed_min[c] = cur_min[c];
      smoothed_max[c] = cur_max[c];
    }
    return;
  }

  n = ref->window < ref->count ? ref->window : ref->count;
  if (n < 1) n = 1;
  start = (ref->head - n + NZ_REF_MAX_HISTORY) % NZ_REF_MAX_HISTORY;

  for (c = 0; c < 3; c++)
  {
    long sum_min = 0;
    long sum_max = 0;
    for (j = 0; j < n; j++)
    {
      const nz_ref_entry_t* entry =
          &ref->ring[(start + j) % NZ_REF_MAX_HISTORY];
      sum_min += entry->mn[c];
      sum_max += entry->mx[c];
    }
    smoothed_min[c] = (double)sum_min / (double)n;
    smoothed_max[c] = (double)sum_max / (double)n;
  }
}
