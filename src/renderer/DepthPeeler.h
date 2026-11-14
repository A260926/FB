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
#include <functional>
#include <vector>

#include "Rasterizer.h"
#include "VertexProcessor.h"

namespace swr {

/// Per-pixel result of a depth-peeling run.
struct PeelResult {
	std::vector<float> rgb;       ///< Final linear RGB, 3 channels per pixel, row-major.
	std::vector<float> coverage;  ///< Accumulated alpha (1 - T), one entry per pixel.
	std::vector<int> layers;      ///< Number of surfaces actually selected per pixel.
	int width = 0;
	int height = 0;
};

/// Outputs produced by the user pixel shader for each fragment.
struct PeelFragment {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float alpha = 0.0f;
	bool discard = false;
};

/// Reusable depth peeler for translucent triangles.
/** The peeler owns its own Rasterizer and routes the user's VertexProcessor
  through it.  Each peel pass replays the whole scene via the supplied
  callback, running the real vertex and pixel shaders again, and keeps, per
  pixel, the strict successor of the previously selected key
  (depth, surface id).  Only a constant number of per-pixel images is kept;
  no per-frame fragment lists are stored.

  Minimal usage:
  @code
    swr::DepthPeeler peeler;
    peeler.setRasterMode(swr::RasterMode::Block);

    // Replays one fixed scene; the same geometry and surface ids on every
    // pass.  ids has one entry per triangle and may mix freely.
    auto draw = [&](swr::VertexProcessor &vp) {
        vp.setViewport(0, 0, W, H);
        vp.setVertexShader<MyVS>();
        vp.setVertexAttribPointer(0, sizeof(Vertex), vertices);
        vp.drawElements(swr::DrawMode::Triangle, indexCount, indices, ids);
    };

    auto shade = [](const swr::PixelData &p, swr::PeelFragment &f) {
        f.r = p.avar[0]; f.g = p.avar[1]; f.b = p.avar[2];
        f.alpha = p.avar[3];
        f.discard = false;
    };

    swr::PeelResult out = peeler.peel(W, H, backgroundRGB, backgroundDepth,
                                      draw, shade);
    // out.rgb (linear), out.coverage (1 - transmittance), out.layers.
  @endcode */
class DepthPeeler {
public:
	/// Callback that replays one fixed scene.
	/** It must submit the same geometry and the same surface ids on every
	  invocation, using the given VertexProcessor and the id-aware
	  drawElements overload. */
	using DrawCallback = std::function<void(VertexProcessor &)>;

	/// Pixel shader callback evaluated for every surviving fragment.
	/** Implementations read interpolated inputs from PixelData exactly as a
	  normal pixel shader would and fill the fragment colour, alpha and
	  discard flag. */
	using ShaderCallback = std::function<void(const PixelData &, PeelFragment &)>;

	DepthPeeler();
	~DepthPeeler();

	/// Rasterization mode used for every replay.  All three RasterModes are
	/// supported; the default is RasterMode::Span.
	void setRasterMode(RasterMode mode);
	RasterMode rasterMode() const;

	/// Peel the translucent surfaces and composite them front to back.
	/** @param width, height  Image dimensions in pixels.
	  * @param backgroundRGB  Linear float opaque background, 3 channels per pixel.
	  * @param backgroundDepth Opaque background depth in [0, 1], per pixel.
	  *   Only fragments strictly in front of this depth are considered.
	  * @param drawScene  Replays the same scene on every pass.
	  * @param shadeFragment  Evaluates the pixel shader for one fragment.
	  * @return The composited image, owned independently of this object. */
	PeelResult peel(int width, int height,
		const float *backgroundRGB, const float *backgroundDepth,
		const DrawCallback &drawScene,
		const ShaderCallback &shadeFragment);

private:
	struct Impl;
	Impl *m_impl;
};

} // end namespace swr
