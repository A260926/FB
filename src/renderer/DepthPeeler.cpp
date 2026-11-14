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

#include "DepthPeeler.h"

#include <algorithm>
#include <cmath>

#include "SurfaceRasterizer.h"

namespace swr {

namespace {

/// Holds the constant set of per-pixel working images and the peel state for
/// one DepthPeeler.  Only a fixed number of images is kept: no per-frame
/// fragment lists are stored anywhere.
class PeelEngine {
public:
	int width = 0;
	int height = 0;

	DepthPeeler::ShaderCallback shadeFragment;

	// Surface id of the triangle currently being rasterized.
	uint64_t currentId = 0;

	// Last selected key per pixel.
	std::vector<float> keyZ;
	std::vector<uint64_t> keyId;
	std::vector<unsigned char> keyValid;

	// Best candidate found during the current pass.
	std::vector<float> candZ;
	std::vector<uint64_t> candId;
	std::vector<unsigned char> candValid;
	std::vector<float> candR, candG, candB, candA;

	// Surface ids already seen at this pixel in the current pass, so the fan
	// pieces of one original triangle count only once.
	std::vector<uint64_t> seenId;
	std::vector<unsigned char> seenValid;

	// Front-to-back accumulation state.
	std::vector<float> accumR, accumG, accumB;
	std::vector<float> transmittance;
	std::vector<int> layers;
	std::vector<unsigned char> active;

	// Copied opaque background.
	std::vector<float> bgR, bgG, bgB;
	std::vector<float> bgDepth;

	void allocate(int w, int h)
	{
		width = w;
		height = h;
		const size_t n = (size_t)w * h;

		keyZ.resize(n); keyId.resize(n); keyValid.assign(n, 0);
		candZ.resize(n); candId.resize(n); candValid.assign(n, 0);
		candR.resize(n); candG.resize(n); candB.resize(n); candA.resize(n);
		seenId.resize(n); seenValid.assign(n, 0);
		accumR.assign(n, 0.0f); accumG.assign(n, 0.0f); accumB.assign(n, 0.0f);
		transmittance.assign(n, 1.0f);
		layers.assign(n, 0);
		active.assign(n, 1);
		bgR.resize(n); bgG.resize(n); bgB.resize(n); bgDepth.resize(n);
	}

	void beginPass()
	{
		std::fill(candValid.begin(), candValid.end(), (unsigned char)0);
		std::fill(seenValid.begin(), seenValid.end(), (unsigned char)0);
	}

	void beginTriangle(uint64_t id) { currentId = id; }
	void endTriangle() {}

	void processFragment(const PixelData &p)
	{
		if (p.x < 0 || p.y < 0 || p.x >= width || p.y >= height)
			return;

		const size_t idx = (size_t)p.y * width + p.x;
		if (!active[idx])
			return;

		const float z = p.z;

		// Only fragments strictly in front of the opaque background.  The
		// written form is NaN-safe (inputs are promised finite anyway).
		if (!(z < bgDepth[idx]))
			return;

		const uint64_t id = currentId;

		// One original surface counts once per pixel, no matter how many
		// clipping fan pieces touch it.
		if (seenValid[idx] && seenId[idx] == id)
			return;

		// Strict successor of the previously selected key (depth, id).
		// No epsilon: an equal depth only advances when the id is larger.
		if (keyValid[idx])
		{
			const bool deeper = z > keyZ[idx];
			const bool sameDepthLargerId = (z == keyZ[idx]) && (id > keyId[idx]);
			if (!(deeper || sameDepthLargerId))
				return;
		}

		seenValid[idx] = 1;
		seenId[idx] = id;

		// Run the real pixel shader for this fragment.
		PeelFragment f;
		shadeFragment(p, f);

		// Discarded fragments neither occupy a layer nor block what is behind.
		if (f.discard)
			return;

		// Keep the minimum candidate by (depth, id).
		if (!candValid[idx] || z < candZ[idx] || (z == candZ[idx] && id < candId[idx]))
		{
			candValid[idx] = 1;
			candZ[idx] = z;
			candId[idx] = id;
			candR[idx] = f.r;
			candG[idx] = f.g;
			candB[idx] = f.b;
			candA[idx] = f.alpha;
		}
	}

	/// Commit every per-pixel candidate.  Returns false when no pixel had a
	/// successor, which terminates the peeling loop.
	bool mergePass()
	{
		bool any = false;
		const size_t n = (size_t)width * height;

		for (size_t idx = 0; idx < n; ++idx)
		{
			if (!candValid[idx])
				continue;

			any = true;

			const float z = candZ[idx];
			const uint64_t id = candId[idx];
			const float alpha = candA[idx];

			// The key always advances to a strictly greater key, guaranteeing
			// termination even for zero-alpha (non-occluding) surfaces.
			keyValid[idx] = 1;
			keyZ[idx] = z;
			keyId[idx] = id;

			if (alpha <= 0.0f)
				continue; // Does not occupy a layer and does not block.

			const float t = transmittance[idx];
			accumR[idx] += t * alpha * candR[idx];
			accumG[idx] += t * alpha * candG[idx];
			accumB[idx] += t * alpha * candB[idx];
			transmittance[idx] = t * (1.0f - alpha);
			layers[idx] += 1;

			if (alpha >= 1.0f)
				active[idx] = 0; // Fully opaque: nothing behind can contribute.
		}

		return any;
	}
};

/// Pixel shader plugged into the reused Rasterizer.  It forces z/w
/// interpolation and fills every varying slot, then forwards each covered
/// sample to the peel engine, which runs the caller's pixel shader.  The
/// fixed maximum varying counts let any scalar user shader read the affine
/// or perspective-correct inputs its vertex shader wrote.
class PeelShader : public PixelShaderBase<PeelShader> {
public:
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static const int AVarCount = MaxAVars;
	static const int PVarCount = MaxPVars;

	static PeelEngine *engine;

	static void drawPixel(const PixelData &p)
	{
		if (engine != nullptr)
			engine->processFragment(p);
	}
};

PeelEngine *PeelShader::engine = nullptr;

/// Rasterizer that keeps the existing span/block/adaptive machinery and only
/// adds a stable surface id around each drawn triangle.
class PeelRasterizer : public Rasterizer, public ISurfaceIdSink {
public:
	PeelEngine *engine = nullptr;

	void drawTriangleListIds(const RasterizerVertex *vertices, const int *indices,
		size_t indexCount, const uint64_t *surfaceIds) const override
	{
		for (size_t i = 0; i + 3 <= indexCount; i += 3)
		{
			if (indices[i] == -1)
				continue;
			engine->beginTriangle(surfaceIds[i / 3]);
			drawTriangle(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]);
			engine->endTriangle();
		}
	}
};

} // namespace

struct DepthPeeler::Impl {
	PeelEngine engine;
	PeelRasterizer rasterizer;
	VertexProcessor vp;
	RasterMode mode = RasterMode::Span;

	Impl() : vp(&rasterizer)
	{
		rasterizer.engine = &engine;
		rasterizer.setPixelShader<PeelShader>();
	}
};

DepthPeeler::DepthPeeler()
	: m_impl(new Impl())
{
}

DepthPeeler::~DepthPeeler()
{
	delete m_impl;
}

void DepthPeeler::setRasterMode(RasterMode mode)
{
	m_impl->mode = mode;
	m_impl->rasterizer.setRasterMode(mode);
}

RasterMode DepthPeeler::rasterMode() const
{
	return m_impl->mode;
}

PeelResult DepthPeeler::peel(int width, int height,
	const float *backgroundRGB, const float *backgroundDepth,
	const DrawCallback &drawScene,
	const ShaderCallback &shadeFragment)
{
	PeelResult result;
	result.width = width;
	result.height = height;

	const size_t n = (size_t)width * height;
	if (width <= 0 || height <= 0 || n == 0)
		return result;

	Impl &impl = *m_impl;
	PeelEngine &engine = impl.engine;

	engine.allocate(width, height);
	engine.shadeFragment = shadeFragment;

	for (size_t i = 0; i < n; ++i)
	{
		engine.bgR[i] = backgroundRGB[i * 3 + 0];
		engine.bgG[i] = backgroundRGB[i * 3 + 1];
		engine.bgB[i] = backgroundRGB[i * 3 + 2];
		engine.bgDepth[i] = backgroundDepth[i];
	}

	impl.rasterizer.setRasterMode(impl.mode);
	impl.rasterizer.setScissorRect(0, 0, width, height);
	impl.rasterizer.setPixelShader<PeelShader>();
	PeelShader::engine = &engine;

	// Each pass replays the whole scene against the frozen per-pixel keys.
	// There is no fixed layer cap: the loop stops only when a pass finds no
	// strict successor for any pixel.
	for (;;)
	{
		engine.beginPass();
		drawScene(impl.vp);
		if (!engine.mergePass())
			break;
	}

	PeelShader::engine = nullptr;

	result.rgb.resize(n * 3);
	result.coverage.resize(n);
	result.layers.resize(n);

	for (size_t i = 0; i < n; ++i)
	{
		const float t = engine.transmittance[i];
		result.rgb[i * 3 + 0] = engine.accumR[i] + t * engine.bgR[i];
		result.rgb[i * 3 + 1] = engine.accumG[i] + t * engine.bgG[i];
		result.rgb[i * 3 + 2] = engine.accumB[i] + t * engine.bgB[i];
		result.coverage[i] = 1.0f - t;
		result.layers[i] = engine.layers[i];
	}

	return result;
}

} // end namespace swr
