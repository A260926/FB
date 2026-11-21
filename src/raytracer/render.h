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

struct Object;
struct ShellDiffusionSolution;
struct ShellDiffusionRenderOps;

void render_init(void);
void render(void);

/*
 * Active shell diffusion snapshot used for diffuse shading.
 *
 * set_shell_diffusion_solution adopts one reference to the snapshot and
 * releases the previously active one. The renderer keeps its own reference,
 * so the snapshot remains usable after the caller frees its reference.
 * Passing NULL clears the snapshot and restores the original shading and
 * random number sequence. Both are confined to the inter-frame boundary.
 *
 * set_shell_diffusion_solution / replace_shell_diffusion_solution are
 * equivalent atomic swaps; clear_shell_diffusion_solution is set(NULL).
 */
void render_set_shell_diffusion_solution(struct ShellDiffusionSolution *solution);
void render_replace_shell_diffusion_solution(struct ShellDiffusionSolution *solution);
void render_clear_shell_diffusion_solution(void);
struct ShellDiffusionSolution *render_get_shell_diffusion_solution(void);

/* Renderer-side ops registration; the diffusion module installs itself. */
void render_shell_register_ops(const struct ShellDiffusionRenderOps *ops);

extern v3 global_ambient_light_intensity;

#endif /* __RENDER_H__ */
