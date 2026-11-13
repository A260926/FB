//
// CPU (off-screen, software) back-end for NanoVG.
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

// Creates a NanoVG context that renders entirely on the CPU into a caller
// owned RGBA8 buffer. No window system or GPU libraries are needed.
//
// Parameters:
// - rgba:   pointer to the destination buffer, borrowed for the lifetime of
//           the context. Origin is the top-left corner, pixel order R,G,B,A.
//           The buffer holds premultiplied alpha and contains at least
//           height * stride bytes. The back-end only writes to it while a
//           frame is ended (nvgEndFrame); until then the bytes are left
//           untouched. Padding bytes between rows are never written.
// - width, height: size of the render target in physical pixels.
// - stride: bytes per row, must be at least 4 * width.
//
// The logical size passed to nvgBeginFrame() multiplied by the device pixel
// ratio must equal width/height. The caller guarantees this; coordinates and
// paints arriving through the back-end callbacks are already in physical
// pixels and are used as-is (the DPR is never applied a second time).
//
// Coverage is evaluated on a regular 4x4 sub-pixel grid: a physical pixel is
// covered by the fraction of the 16 sub-pixel centers lying inside the
// geometry. Fills use the non-zero winding rule over all sub-paths together
// (holes, multiple contours and self-intersecting paths keep working), strokes
// merge overlapping triangles into a coverage union within one renderStroke
// call, and triangle lists use half-open shared edges so glyph quad diagonals
// are never covered twice.
//
// Image data is copied on creation. NVG_IMAGE_GENERATE_MIPMAPS is accepted
// but ignored: the CPU back-end only implements nearest and texel-center
// bilinear sampling. REPEATX/Y, FLIPY, NEAREST and PREMULTIPLIED are honored
// and texture dimensions do not need to be powers of two.
//
// Returns NULL when rgba is NULL or width/height/stride are invalid.
NVGcontext* nvgCreateCPU(unsigned char* rgba, int width, int height, int stride);

// Destroys a CPU NanoVG context. The borrowed destination buffer is not freed.
void nvgDeleteCPU(NVGcontext* ctx);

#ifdef __cplusplus
}
#endif

#endif // NANOVG_CPU_H
