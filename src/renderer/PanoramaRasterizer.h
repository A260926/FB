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

#include "IRasterizer.h"

namespace swr {

/// Statistics collected by a panorama rasterization pass.
struct PanoramaStats {
	/// Number of triangle references inserted into the 8x8 candidate bins.
	size_t bin_references;

	/// Number of (triangle, candidate pixel) ray tests performed.
	size_t candidate_tests;

	/// Number of fragments passed to the selected pixel shader.
	size_t emitted_fragments;
};

/// Receives a single panorama fragment.
/**
The barycentric coordinates refer to the physical hit point on the original
triangle. The three source vertices are provided so that the pixel shader
adapter can interpolate the vertex varyings. The sink is called in input
triangle order for every pixel.
*/
typedef void (*PanoramaFragmentSink)(
	void *context,
	int x,
	int y,
	float radialDistance,
	int primitiveIndex,
	float b0,
	float b1,
	float b2,
	const RasterizerVertex *vertex0,
	const RasterizerVertex *vertex1,
	const RasterizerVertex *vertex2);

/// Rasterize a camera-space triangle list as an equirectangular panorama.
/**
\param vertices Vertex array, in camera space. x is right, y is up, z is
forward, w must be 1. Varyings are not pre-divided by w.
\param indices Three indices per triangle. A triangle whose first index is -1
is skipped, but still occupies a primitive index.
\param indexCount Number of entries in \a indices.
\param width Width of the panorama image in pixels.
\param height Height of the panorama image in pixels.
\param nearRadius Near radial clip distance, inclusive.
\param farRadius Far radial clip distance, exclusive.
\param sink Fragment callback.
\param sinkContext Opaque context passed to \a sink.
\param stats Optional statistics output; it is cleared at the start of the call.
*/
void rasterizePanoramaTriangleList(
	const RasterizerVertex *vertices,
	const int *indices,
	size_t indexCount,
	int width,
	int height,
	float nearRadius,
	float farRadius,
	PanoramaFragmentSink sink,
	void *sinkContext,
	PanoramaStats *stats);

/// Same as rasterizePanoramaTriangleList but restricted to the half open
/// rectangle [clipMinX, clipMaxX) x [clipMinY, clipMaxY).  The clip rectangle
/// is intersected with the image bounds.  Used by Rasterizer to honor the
/// scissor rectangle without changing the geometry path.
void rasterizePanoramaTriangleListClipped(
	const RasterizerVertex *vertices,
	const int *indices,
	size_t indexCount,
	int width,
	int height,
	float nearRadius,
	float farRadius,
	PanoramaFragmentSink sink,
	void *sinkContext,
	PanoramaStats *stats,
	int clipMinX,
	int clipMinY,
	int clipMaxX,
	int clipMaxY);

} // end namespace swr
