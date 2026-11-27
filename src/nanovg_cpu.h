//
// Copyright (c) 2013 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#ifndef NANOVG_CPU_H
#define NANOVG_CPU_H

#include "nanovg.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create a CPU rasterizer backend which renders into a caller owned buffer.
//
// The buffer is borrowed, never reallocated or freed by the backend:
//   rgba   - pointer to at least height*stride bytes, origin at the top-left
//   width  - physical width in pixels (must be > 0)
//   height - physical height in pixels (must be > 0)
//   stride - bytes per row (must be >= 4*width)
//
// The pixel format is premultiplied RGBA8 (R,G,B,A), one byte each.
// The contents of the buffer are left untouched between nvgBeginFrame() and
// nvgEndFrame(): all drawing is accumulated off-screen and published in call
// order when the frame ends. nvgCancelFrame() discards the frame and leaves
// the buffer at its previous contents.
//
// Geometry is rasterized directly from the triangles NanoVG already
// tessellated; edge anti-aliasing is disabled and coverage is evaluated at
// the centers of a regular 4x4 sub-pixel grid, i.e. 16 samples per pixel.
//
// The logical size passed to nvgBeginFrame() multiplied by the device pixel
// ratio must equal the physical width/height given here. Coordinates, paints
// and scissors are interpreted in the logical coordinates handed to the
// backend callbacks; the DPR is not applied a second time.
//
// Returns NULL if any argument is invalid. The caller guarantees that the
// buffer stays valid for the lifetime of the context and that its length
// covers height*stride bytes.
NVGcontext* nvgCreateCPU(unsigned char* rgba, int width, int height, int stride);

// Destroy a context created with nvgCreateCPU(). The borrowed buffer is not
// freed. Safe to call with NULL.
void nvgDeleteCPU(NVGcontext* ctx);

#ifdef __cplusplus
}
#endif

#endif // NANOVG_CPU_H
