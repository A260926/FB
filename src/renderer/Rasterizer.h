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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "IRasterizer.h"
#include "EdgeEquation.h"
#include "ParameterEquation.h"
#include "TriangleEquations.h"
#include "PixelData.h"
#include "EdgeData.h"
#include "PixelShaderBase.h"
#include "PixelQuad.h"

namespace swr {

#pragma warning (push)
#pragma warning (disable: 6294 6201)

/// Rasterizer mode.
enum class RasterMode {
	Span,
	Block,
	Adaptive
};

/// Rasterizer main class.
class Rasterizer : public IRasterizer {
private:
	int m_minX;
	int m_maxX;
	int m_minY;
	int m_maxY;

	RasterMode rasterMode;

	void (Rasterizer::*m_triangleFunc)(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const;
	void (Rasterizer::*m_lineFunc)(const RasterizerVertex &v0, const RasterizerVertex &v1) const;
	void (Rasterizer::*m_pointFunc)(const RasterizerVertex &v) const;

	// Quad shader state.
	bool m_quadMode;                         ///< True while a quad pixel shader is installed.
	void (*m_quadFlushFunc)(const PixelQuad &); ///< Invokes the shader's drawQuad.
	mutable uint64_t m_quadPrimitiveIndex;   ///< Index of the current original input triangle.
	mutable bool m_quadGroupOpen;            ///< Between beginQuadPrimitive and endQuadPrimitive.

	// Accumulated quads for the original input triangle currently processed.
	struct QuadKey {
		int x; ///< Even integer origin x of the quad.
		int y; ///< Even integer origin y of the quad.
		bool operator==(const QuadKey &o) const { return x == o.x && y == o.y; }
	};
	mutable std::vector<QuadKey> m_quadKeys;
	mutable std::vector<PixelQuad> m_quads;

public:
	/// Constructor.
	Rasterizer()
	{
		m_quadMode = false;
		m_quadFlushFunc = nullptr;
		m_quadPrimitiveIndex = 0;
		m_quadGroupOpen = false;
		setRasterMode(RasterMode::Span);
		setScissorRect(0, 0, 0, 0);
		setPixelShader<NullPixelShader>();
	}

	/// Set the raster mode. The default is RasterMode::Span.
	void setRasterMode(RasterMode mode)
	{
		rasterMode = mode;
	}

	/// Set the scissor rectangle.
	void setScissorRect(int x, int y, int width, int height)
	{
		m_minX = x;
		m_minY = y;
		m_maxX = x + width;
		m_maxY = y + height;
	}

	/// Set the pixel shader.
	template <class PixelShader>
	void setPixelShader()
	{
		m_quadMode = false;
		m_triangleFunc = &Rasterizer::drawTriangleModeTemplate<PixelShader>;
		m_lineFunc = &Rasterizer::drawLineTemplate<PixelShader>;
		m_pointFunc = &Rasterizer::drawPointTemplate<PixelShader>;
	}

	/// Set a quad pixel shader.
	/** The shader only needs to implement the static constants and
	  drawQuad(const PixelQuad&); drawPixel is not required. While installed,
	  points and lines throw std::logic_error and only triangles are drawn.
	  Call setPixelShader to return to the scalar behaviour. */
	template <class QuadShader>
	void setQuadPixelShader()
	{
		m_quadMode = true;
		m_quadFlushFunc = &Rasterizer::quadFlush<QuadShader>;
		m_triangleFunc = &Rasterizer::drawQuadTriangleDispatch<QuadShader>;
		m_lineFunc = &Rasterizer::drawQuadUnsupportedLine;
		m_pointFunc = &Rasterizer::drawQuadUnsupportedPoint;
	}

	/// Draw a single point.
	void drawPoint(const RasterizerVertex &v) const
	{
		(this->*m_pointFunc)(v);
	}

	/// Draw a single line.
	void drawLine(const RasterizerVertex &v0, const RasterizerVertex &v1) const
	{
		(this->*m_lineFunc)(v0, v1);
	}
	
	/// Draw a single triangle.
	void drawTriangle(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		(this->*m_triangleFunc)(v0, v1, v2);
	}

	void drawPointList(const RasterizerVertex *vertices, const int *indices, size_t indexCount) const
	{
		for (size_t i = 0; i < indexCount; ++i) {
			if (indices[i] == -1)
				continue;
			drawPoint(vertices[indices[i]]);
		}
	}

	void drawLineList(const RasterizerVertex *vertices, const int *indices, size_t indexCount) const
	{
		for (size_t i = 0; i + 2 <= indexCount; i += 2) {
			if (indices[i] == -1)
				continue;
			drawLine(vertices[indices[i]], vertices[indices[i + 1]]);
		}
	}

	void drawTriangleList(const RasterizerVertex *vertices, const int *indices, size_t indexCount) const
	{
		if (!m_quadMode)
		{
			for (size_t i = 0; i + 3 <= indexCount; i += 3) {
				if (indices[i] == -1)
					continue;
				drawTriangle(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]);
			}
			return;
		}

		// Quad mode. When driven by the VertexProcessor a primitive group is
		// already open, so fans simply accumulate. For a direct drawTriangleList
		// each input triple is its own primitive, numbered from zero.
		for (size_t i = 0; i + 3 <= indexCount; i += 3) {
			if (indices[i] == -1)
				continue;

			bool bracket = !m_quadGroupOpen;
			if (bracket)
				beginQuadPrimitive((uint64_t)(i / 3));

			drawTriangle(vertices[indices[i]], vertices[indices[i + 1]], vertices[indices[i + 2]]);

			if (bracket)
				endQuadPrimitive();
		}
	}

	/// Begin accumulating the clipped fans of one original input triangle.
	void beginQuadPrimitive(uint64_t primitiveIndex) const override
	{
		if (!m_quadMode)
			return;
		m_quadPrimitiveIndex = primitiveIndex;
		m_quadGroupOpen = true;
		m_quadKeys.clear();
		m_quads.clear();
	}

	/// Flush the accumulated quads of one original input triangle.
	void endQuadPrimitive() const override
	{
		if (!m_quadMode)
			return;
		for (const PixelQuad &q : m_quads)
			flushQuad(q);
		m_quadKeys.clear();
		m_quads.clear();
		m_quadGroupOpen = false;
	}

	/// Whether a quad pixel shader is currently installed.
	bool quadShaderEnabled() const override
	{
		return m_quadMode;
	}

private:
	void drawQuadUnsupportedPoint(const RasterizerVertex &) const
	{
		throw std::logic_error("quad pixel shaders only support triangles");
	}

	void drawQuadUnsupportedLine(const RasterizerVertex &, const RasterizerVertex &) const
	{
		throw std::logic_error("quad pixel shaders only support triangles");
	}

private:
	bool scissorTest(float x, float y) const
	{
		return (x >= m_minX && x < m_maxX && y >= m_minY && y < m_maxY);
	}

	// Round toward negative infinity to the nearest even integer.
	// Negative coordinates therefore align towards the negative direction.
	static int alignEvenDown(int v)
	{
		int r = v % 2;
		if (r < 0)
			r += 2;
		return v - r;
	}

	// Emit the quads of one clipped triangle fan, accumulating them into the
	// open primitive group so that several fans of the same input triangle are
	// merged into a single callback per quad.
	template <class QuadShader>
	void drawQuadTriangleTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		// Compute triangle equations.
		TriangleEquations eqn(v0, v1, v2, QuadShader::AVarCount, QuadShader::PVarCount);

		// Check if triangle is backfacing.
		if (eqn.area2 <= 0)
			return;

		// Screen bounding box of this fan, expressed in integer pixel coords.
		// The scan is limited to this box, not the whole viewport.
		int pxMinX = (int)std::floor(std::min(std::min(v0.x, v1.x), v2.x));
		int pxMaxX = (int)std::floor(std::max(std::max(v0.x, v1.x), v2.x));
		int pxMinY = (int)std::floor(std::min(std::min(v0.y, v1.y), v2.y));
		int pxMaxY = (int)std::floor(std::max(std::max(v0.y, v1.y), v2.y));

		int qMinX = alignEvenDown(pxMinX);
		int qMaxX = alignEvenDown(pxMaxX);
		int qMinY = alignEvenDown(pxMinY);
		int qMaxY = alignEvenDown(pxMaxY);

		for (int qy = qMinY; qy <= qMaxY; qy += 2)
		{
			for (int qx = qMinX; qx <= qMaxX; qx += 2)
			{
				unsigned coverage = 0;

				// Lane offsets: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right.
				static const int dx[4] = { 0, 1, 0, 1 };
				static const int dy[4] = { 0, 0, 1, 1 };

				float invwLane[4];

				for (int lane = 0; lane < 4; ++lane)
				{
					float cx = (float)(qx + dx[lane]) + 0.5f;
					float cy = (float)(qy + dy[lane]) + 0.5f;

					bool inside = eqn.e0.test(cx, cy) && eqn.e1.test(cx, cy) && eqn.e2.test(cx, cy);
					if (inside && scissorTest(cx, cy))
						coverage |= (1u << lane);

					invwLane[lane] = eqn.invw.evaluate(cx, cy);
				}

				if (coverage == 0)
					continue;

				// A zero (or non finite) perspective denominator at any lane is
				// a domain error rather than silently feeding float to integer.
				for (int lane = 0; lane < 4; ++lane)
				{
					if (invwLane[lane] == 0.0f || !std::isfinite(invwLane[lane]))
						throw std::domain_error("quad interpolation has a zero or invalid w denominator");
				}

				QuadKey key{ qx, qy };
				PixelQuad *slot = nullptr;
				for (size_t k = 0; k < m_quadKeys.size(); ++k)
				{
					if (m_quadKeys[k] == key)
					{
						slot = &m_quads[k];
						break;
					}
				}

				if (slot)
				{
					// Same original triangle plane: union the coverage, keep the
					// already extrapolated lane interpolation.
					slot->coverageMask |= coverage;
					continue;
				}

				PixelQuad quad;
				quad.coverageMask = coverage;
				quad.primitiveIndex = m_quadPrimitiveIndex;

				for (int lane = 0; lane < 4; ++lane)
				{
					float cx = (float)(qx + dx[lane]) + 0.5f;
					float cy = (float)(qy + dy[lane]) + 0.5f;

					PixelData &p = quad.lanes[lane];
					p.x = qx + dx[lane];
					p.y = qy + dy[lane];
					p.init(eqn, cx, cy, QuadShader::AVarCount, QuadShader::PVarCount,
						QuadShader::InterpolateZ, QuadShader::InterpolateW);

					// Every lane, helpers included, carries depth and w
					// extrapolated from the original triangle plane.
					p.z = eqn.z.evaluate(cx, cy);
					p.invw = invwLane[lane];
					p.w = 1.0f / invwLane[lane];

					// The equations object is local to this fan and must not
					// outlive the traversal, so drop the borrowed pointer.
					p.equations = nullptr;
				}

				m_quadKeys.push_back(key);
				m_quads.push_back(quad);
			}
		}
	}

	// Triangle entry point while a quad shader is installed.
	template <class QuadShader>
	void drawQuadTriangleDispatch(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		if (m_quadGroupOpen)
		{
			// One fan of the original input triangle currently being accumulated.
			drawQuadTriangleTemplate<QuadShader>(v0, v1, v2);
		}
		else
		{
			// Direct drawTriangle: a single primitive with index 0.
			beginQuadPrimitive(0);
			drawQuadTriangleTemplate<QuadShader>(v0, v1, v2);
			endQuadPrimitive();
		}
	}

	void flushQuad(const PixelQuad &quad) const
	{
		m_quadFlushFunc(quad);
	}

	template <class QuadShader>
	static void quadFlush(const PixelQuad &quad)
	{
		QuadShader::drawQuad(quad);
	}

	template <class PixelShader>
	void drawPointTemplate(const RasterizerVertex &v) const
	{
		// Check scissor rect
		if (!scissorTest(v.x, v.y))
			return;

		PixelData p = pixelDataFromVertex<PixelShader>(v);
		PixelShader::drawPixel(p);
	}

	template<class PixelShader>
	PixelData pixelDataFromVertex(const RasterizerVertex & v) const
	{
		PixelData p;
		p.x = (int)v.x;
		p.y = (int)v.y;
		if (PixelShader::InterpolateZ) p.z = v.z;
		if (PixelShader::InterpolateW) { p.w = v.w; p.invw = 1.0f / v.w; }
		for (int i = 0; i < PixelShader::AVarCount; ++i)
			p.avar[i] = v.avar[i];
		for (int i = 0; i < PixelShader::PVarCount; ++i)
			p.pvar[i] = v.pvar[i];
		return p;
	}
	
	template <class PixelShader>
	void drawLineTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1) const
	{
		int adx = std::abs((int)v1.x - (int)v0.x);
		int ady = std::abs((int)v1.y - (int)v0.y);
		int steps = std::max(adx, ady);

		RasterizerVertex step = computeVertexStep<PixelShader>(v0, v1, steps);

		RasterizerVertex v = v0;
		while (steps-- > 0)
		{
			PixelData p = pixelDataFromVertex<PixelShader>(v);

			if (scissorTest(v.x, v.y))
				PixelShader::drawPixel(p);
			
			stepVertex<PixelShader>(v, step);
		}
	}

	template<class PixelShader>
	void stepVertex(RasterizerVertex &v, RasterizerVertex &step) const
	{
		v.x += step.x;
		v.y += step.y;
		if (PixelShader::InterpolateZ) v.z += step.z;
		if (PixelShader::InterpolateW) v.w += step.w;
		for (int i = 0; i < PixelShader::AVarCount; ++i)
			v.avar[i] += step.avar[i];
		for (int i = 0; i < PixelShader::PVarCount; ++i)
			v.pvar[i] += step.pvar[i];
	}

	template<class PixelShader>
	RasterizerVertex computeVertexStep(const RasterizerVertex &v0, const RasterizerVertex &v1, int adx) const
	{
		RasterizerVertex step;
		step.x = (v1.x - v0.x) / adx;
		step.y = (v1.y - v0.y) / adx;
		if (PixelShader::InterpolateZ) step.z = (v1.z - v0.z) / adx;
		if (PixelShader::InterpolateW) step.w = (v1.w - v0.w) / adx;
		for (int i = 0; i < PixelShader::AVarCount; ++i)
			step.avar[i] = (v1.avar[i] - v0.avar[i]) / adx;
		for (int i = 0; i < PixelShader::PVarCount; ++i)
			step.pvar[i] = (v1.pvar[i] - v0.pvar[i]) / adx;
		return step;
	}

	template <class PixelShader>
	void drawTriangleBlockTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		// Compute triangle equations.
		TriangleEquations eqn(v0, v1, v2, PixelShader::AVarCount, PixelShader::PVarCount);

		// Check if triangle is backfacing.
		if (eqn.area2 <= 0)
			return;

		// Compute triangle bounding box.
		int minX = (int)std::min(std::min(v0.x, v1.x), v2.x);
		int maxX = (int)std::max(std::max(v0.x, v1.x), v2.x);
		int minY = (int)std::min(std::min(v0.y, v1.y), v2.y);
		int maxY = (int)std::max(std::max(v0.y, v1.y), v2.y);

		// Clip to scissor rect.
		minX = std::max(minX, m_minX);
		maxX = std::min(maxX, m_maxX);
		minY = std::max(minY, m_minY);
		maxY = std::min(maxY, m_maxY);

		// Round to block grid.
		minX = minX & ~(BlockSize - 1);
		maxX = maxX & ~(BlockSize - 1);
		minY = minY & ~(BlockSize - 1);
		maxY = maxY & ~(BlockSize - 1);

		float s = BlockSize - 1;

		int stepsX = (maxX - minX) / BlockSize + 1;
		int stepsY = (maxY - minY) / BlockSize + 1;

		#pragma omp parallel for
		for (int i = 0; i < stepsX * stepsY; ++i)
		{
			int sx = i % stepsX;
			int sy = i / stepsX;

			// Add 0.5 to sample at pixel centers.
			int x = minX + sx * BlockSize;
			int y = minY + sy * BlockSize;

			float xf = x + 0.5f;
			float yf = y + 0.5f;

			// Test if block is inside or outside triangle or touches it.
			EdgeData e00; e00.init(eqn, xf, yf);
			EdgeData e01 = e00; e01.stepY(eqn, s);
			EdgeData e10 = e00; e10.stepX(eqn, s);
			EdgeData e11 = e01; e11.stepX(eqn, s);

			bool e00_0 = eqn.e0.test(e00.ev0), e00_1 = eqn.e1.test(e00.ev1), e00_2 = eqn.e2.test(e00.ev2), e00_all = e00_0 && e00_1 && e00_2;
			bool e01_0 = eqn.e0.test(e01.ev0), e01_1 = eqn.e1.test(e01.ev1), e01_2 = eqn.e2.test(e01.ev2), e01_all = e01_0 && e01_1 && e01_2;
			bool e10_0 = eqn.e0.test(e10.ev0), e10_1 = eqn.e1.test(e10.ev1), e10_2 = eqn.e2.test(e10.ev2), e10_all = e10_0 && e10_1 && e10_2;
			bool e11_0 = eqn.e0.test(e11.ev0), e11_1 = eqn.e1.test(e11.ev1), e11_2 = eqn.e2.test(e11.ev2), e11_all = e11_0 && e11_1 && e11_2;

			int result = e00_all + e01_all + e10_all + e11_all;

			// Potentially all out.
			if (result == 0)
			{
				// Test for special case.
				bool e00Same = e00_0 == e00_1 == e00_2;
				bool e01Same = e01_0 == e01_1 == e01_2;
				bool e10Same = e10_0 == e10_1 == e10_2;
				bool e11Same = e11_0 == e11_1 == e11_2;

				if (!e00Same || !e01Same || !e10Same || !e11Same)
					PixelShader::template drawBlock<true>(eqn, x, y);
			}
			else if (result == 4)
			{
				// Fully Covered.
				PixelShader::template drawBlock<false>(eqn, x, y);
			}
			else
			{
				// Partially Covered.
				PixelShader::template drawBlock<true>(eqn, x, y);
			}
		}
	}

	template <class PixelShader>
	void drawTriangleSpanTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		// Compute triangle equations.
		TriangleEquations eqn(v0, v1, v2, PixelShader::AVarCount, PixelShader::PVarCount);

		// Check if triangle is backfacing.
		if (eqn.area2 <= 0)
			return;

		const RasterizerVertex *t = &v0;
		const RasterizerVertex *m = &v1;
		const RasterizerVertex *b = &v2;

		// Sort vertices from top to bottom.
		if (t->y > m->y) std::swap(t, m);
		if (m->y > b->y) std::swap(m, b);
		if (t->y > m->y) std::swap(t, m);

		float dy = (b->y - t->y);
		float iy = (m->y - t->y);

		if (m->y == t->y)
		{
			const RasterizerVertex *l = m, *r = t;
			if (l->x > r->x) std::swap(l, r);
			drawTopFlatTriangle<PixelShader>(eqn, *l, *r, *b);
		}
		else if (m->y == b->y)
		{
			const RasterizerVertex *l = m, *r = b;
			if (l->x > r->x) std::swap(l, r);
			drawBottomFlatTriangle<PixelShader>(eqn, *t, *l, *r);
		} 
		else
		{
			RasterizerVertex v4;
			v4.y = m->y;
			v4.x = t->x + ((b->x - t->x) / dy) * iy;
			if (PixelShader::InterpolateZ) v4.z = t->z + ((b->z - t->z) / dy) * iy;
			if (PixelShader::InterpolateW) v4.w = t->w + ((b->w - t->w) / dy) * iy;
			for (int i = 0; i < PixelShader::AVarCount; ++i)
				v4.avar[i] = t->avar[i] + ((b->avar[i] - t->avar[i]) / dy) * iy;

			const RasterizerVertex *l = m, *r = &v4;
			if (l->x > r->x) std::swap(l, r);

			drawBottomFlatTriangle<PixelShader>(eqn, *t, *l, *r);
			drawTopFlatTriangle<PixelShader>(eqn, *l, *r, *b);
		}
	}

	template <class PixelShader>
	void drawBottomFlatTriangle(const TriangleEquations &eqn, const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		float invslope1 = (v1.x - v0.x) / (v1.y - v0.y);
		float invslope2 = (v2.x - v0.x) / (v2.y - v0.y);

		//float curx1 = v0.x;
		//float curx2 = v0.x;

		#pragma omp parallel for
		for (int scanlineY = int(v0.y + 0.5f); scanlineY < int(v1.y + 0.5f); scanlineY++)
		{
			float dy = (scanlineY - v0.y) + 0.5f;
			float curx1 = v0.x + invslope1 * dy + 0.5f;
			float curx2 = v0.x + invslope2 * dy + 0.5f;

			// Clip to scissor rect
			int xl = std::max(m_minX, (int)curx1);
			int xr = std::min(m_maxX, (int)curx2);

			PixelShader::drawSpan(eqn, xl, scanlineY, xr);
			
			// curx1 += invslope1;
			// curx2 += invslope2;
		}
	}

	template <class PixelShader>
	void drawTopFlatTriangle(const TriangleEquations &eqn, const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		float invslope1 = (v2.x - v0.x) / (v2.y - v0.y);
		float invslope2 = (v2.x - v1.x) / (v2.y - v1.y);

		// float curx1 = v2.x;
		// float curx2 = v2.x;

		#pragma omp parallel for
		for (int scanlineY = int(v2.y - 0.5f); scanlineY > int(v0.y - 0.5f); scanlineY--)
		{
			float dy = (scanlineY - v2.y) + 0.5f;
			float curx1 = v2.x + invslope1 * dy + 0.5f;
			float curx2 = v2.x + invslope2 * dy + 0.5f;

			// Clip to scissor rect
			int xl = std::max(m_minX, (int)curx1);
			int xr = std::min(m_maxX, (int)curx2);

			PixelShader::drawSpan(eqn, xl, scanlineY, xr);
			// curx1 -= invslope1;
			// curx2 -= invslope2;
		}
	}

	template <class PixelShader>
	void drawTriangleAdaptiveTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		// Compute triangle bounding box.
		float minX = (float)std::min(std::min(v0.x, v1.x), v2.x);
		float maxX = (float)std::max(std::max(v0.x, v1.x), v2.x);
		float minY = (float)std::min(std::min(v0.y, v1.y), v2.y);
		float maxY = (float)std::max(std::max(v0.y, v1.y), v2.y);

		float orient = (maxX - minX) / (maxY - minY);

		if (orient > 0.4 && orient < 1.6)
			drawTriangleBlockTemplate<PixelShader>(v0, v1, v2);
		else
			drawTriangleSpanTemplate<PixelShader>(v0, v1, v2);
	}

	template <class PixelShader>
	void drawTriangleModeTemplate(const RasterizerVertex &v0, const RasterizerVertex &v1, const RasterizerVertex &v2) const
	{
		switch (rasterMode)
		{
			case RasterMode::Span:
				drawTriangleSpanTemplate<PixelShader>(v0, v1, v2);
				break;
			case RasterMode::Block:
				drawTriangleBlockTemplate<PixelShader>(v0, v1, v2);
				break;
			case RasterMode::Adaptive:
				drawTriangleAdaptiveTemplate<PixelShader>(v0, v1, v2);
				break;
		}
	}
};

#pragma warning (pop)

} // end namespace swr