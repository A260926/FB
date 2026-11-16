/*
 * Copyright (c) 2021-2022 Wojciech Graj
 *
 * Licensed under the MIT license: https://opensource.org/licenses/MIT
 * Permission is granted to use, copy, modify, and redistribute the work.
 * Full license information available in the project LICENSE file.
 *
 * DESCRIPTION:
 *   Image rendering
 **/

#ifndef __RENDER_H__
#define __RENDER_H__

#include "type.h"

struct Lattice;

void render_init(void);
void render(void);

/*
 * Make the renderer use a built lattice for the next frame.  The lattice is
 * borrowed, not owned, and must outlive the frame (and any call to
 * render_clear_lattice).  `far_distance` is a finite positive distance:
 * primary and reflected rays only compare lattice hits closer than it with
 * ordinary objects.  Shadow rays are unaffected and keep terminating at the
 * actual light distance.
 *
 * Set or clear between frames; both calls are not thread-safe with a running
 * render().
 */
void render_set_lattice(struct Lattice *lattice, float far_distance);
void render_clear_lattice(void);

extern v3 global_ambient_light_intensity;

#endif /* __RENDER_H__ */
