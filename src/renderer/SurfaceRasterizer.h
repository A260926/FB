/*
MIT License

Copyright (c) 2017-2020 Markus Trenkwalder

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/** @file */

#include <cstddef>
#include <cstdint>

#include "IRasterizer.h"

namespace swr {

/// Optional rasterizer extension that receives a stable surface id next to
/// each output triangle.
/** The ordinary IRasterizer path is unchanged.  A VertexProcessor that was
  given per-triangle surface ids uses this sink when the rasterizer
  implements it, and silently falls back to the id-less path otherwise. */
class ISurfaceIdSink {
public:
	virtual ~ISurfaceIdSink() {}

	/// Draw a triangle list where surfaceIds[i / 3] is the surface id of
	/// triangle i.  Triangles with indices == -1 are ignored, exactly like
	/// IRasterizer::drawTriangleList.
	virtual void drawTriangleListIds(const RasterizerVertex *vertices,
		const int *indices, size_t indexCount, const uint64_t *surfaceIds) const = 0;
};

} // end namespace swr
