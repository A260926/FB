/*
 * Independent reference model for normaliz0r temporal history.
 *
 * This is a deliberately simple, self-contained reimplementation of the
 * shared host-time rules and of the per-channel extrema smoothing.  It stores
 * extrema only (never full frames) in a flat ring of at most 128 entries.
 *
 * It does not include normaliz0r, does not read any production instance and
 * shares no helper with the plugin; tests compare the plugin's public f0r_*
 * behavior against this model.
 */
#ifndef NORMALIZ_REF_H
#define NORMALIZ_REF_H

#include <stdint.h>

#define NZ_REF_MAX_HISTORY 128

typedef struct
{
  uint8_t mn[3];
  uint8_t mx[3];
} nz_ref_entry_t;

typedef struct
{
  nz_ref_entry_t ring[NZ_REF_MAX_HISTORY];
  int count;          /* Stored entries, [0,NZ_REF_MAX_HISTORY]. */
  int head;           /* Slot receiving the next append. */
  double last_time;   /* Video time of the newest entry. */
  int have_last_time;
  int window;         /* Smoothing window N, [1,NZ_REF_MAX_HISTORY]. */
} nz_ref_t;

void nz_ref_init(nz_ref_t* ref, int window);
void nz_ref_set_window(nz_ref_t* ref, int window);

/* Feed one frame, applying the host-time rules (append / same-time replace /
 * backwards reset / non-finite standalone).  The current frame's per-channel
 * extrema are written to cur_min/cur_max.  Returns 1 for a non-finite time,
 * which must be treated as a standalone single-frame update. */
int nz_ref_observe(nz_ref_t* ref, double time, const uint32_t* frame,
                   int num_pixels, uint8_t cur_min[3], uint8_t cur_max[3]);

/* Arithmetic mean of the most recent N retained extrema (all of them when
 * fewer than N exist).  For a standalone update only cur_min/cur_max from the
 * matching nz_ref_observe() call are meaningful. */
void nz_ref_smoothed(const nz_ref_t* ref, int standalone,
                     const uint8_t cur_min[3], const uint8_t cur_max[3],
                     double smoothed_min[3], double smoothed_max[3]);

#endif
