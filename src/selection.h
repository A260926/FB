/*
 * MyTinyGL - OpenGL 1.x Software Renderer
 * Copyright (c) 2025 zbufferoverflow (Eliezer Solinger)
 * https://github.com/zbufferoverflow/MyTinyGL
 * SPDX-License-Identifier: MIT
 *
 * selection.h - GL_SELECT selection mode
 *
 * This header is force-included (-include src/selection.h) into every
 * translation unit of selection-enabled builds. It defines the
 * MYTINYGL_SELECTION marker that compiles the pipeline hooks in gl_api.c
 * and declares those hooks. Builds without this header (e.g. the plain
 * baseline) compile the hooks out and do not need src/selection.c.
 */

#ifndef MYTINYGL_SELECTION_H
#define MYTINYGL_SELECTION_H

/* Presence marker: core sources compile selection hooks only when set */
#define MYTINYGL_SELECTION 1

#include "GL/gl.h"

struct GLState;

#ifdef __cplusplus
extern "C" {
#endif

/* Pipeline hooks implemented in src/selection.c.
 * Called from gl_api.c when MYTINYGL_SELECTION is defined. */

/* Returns 1 if the primitive mode may be drawn in GL_SELECT mode */
int mtgl_select_mode_supported(GLenum mode);

/* Selection-mode replacement for the rasterization flush: consumes the
 * accumulated vertex buffer of the current glBegin/glEnd pair and turns
 * surviving primitives into hit records instead of pixels. */
void mtgl_select_flush(struct GLState *ctx, GLenum mode);

#ifdef __cplusplus
}
#endif

#endif /* MYTINYGL_SELECTION_H */
