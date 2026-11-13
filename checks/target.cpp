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

#include "checks/suite.h"

#include "Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace swr;

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
	return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

// Recording quad shader. It deliberately also implements drawPixel so the
// tests can prove the scalar entry point is never used in quad mode.
struct QuadRecorder {
	static std::vector<PixelQuad> log;
	static int scalarHits;
	static void clear() { log.clear(); scalarHits = 0; }
	static void drawQuad(const PixelQuad &q) { log.push_back(q); }
	static void drawPixel(const PixelData &) { ++scalarHits; }
};
std::vector<PixelQuad> QuadRecorder::log;
int QuadRecorder::scalarHits = 0;

struct QuadShader : PixelShaderBase<QuadShader> {
	static const int AVarCount = 1;
	static const int PVarCount = 1;
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static void drawQuad(const PixelQuad &q) { QuadRecorder::drawQuad(q); }
	static void drawPixel(const PixelData &p) { QuadRecorder::drawPixel(p); }
};

int g_scalarCalls = 0;
struct ScalarShader : PixelShaderBase<ScalarShader> {
	static const int AVarCount = 1;
	static const int PVarCount = 1;
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static void drawPixel(const PixelData &) { ++g_scalarCalls; }
};

struct VS : VertexShaderBase<VS> {
	static const int AttribCount = 1;
	static const int AVarCount = 1;
	static const int PVarCount = 1;
	static void processVertex(VertexShaderInput in, VertexShaderOutput *o)
	{
		const float *p = (const float *)in[0];
		o->x = p[0];
		o->y = p[1];
		o->z = p[2];
		o->w = p[3];
		o->avar[0] = p[4];
		o->pvar[0] = p[5];
	}
};

RasterizerVertex rv(float x, float y, float z, float w, float av, float pv)
{
	RasterizerVertex v{};
	v.x = x;
	v.y = y;
	v.z = z;
	v.w = w;
	v.avar[0] = av;
	v.pvar[0] = pv;
	return v;
}

const PixelQuad *findQuad(int x, int y)
{
	for (const PixelQuad &q : QuadRecorder::log)
		if (q.lanes[0].x == x && q.lanes[0].y == y)
			return &q;
	return nullptr;
}

// Independent unequal-w barycentric evaluation. This does not reuse the
// rasterizer edge/parameter equations; it recomputes barycentric weights from
// the screen-space vertex positions and the per-vertex w and attributes.
struct RefSample {
	float invw, w, pcorr, aff, z;
};

RefSample reference(const RasterizerVertex &v0, const RasterizerVertex &v1,
	const RasterizerVertex &v2, float sx, float sy)
{
	float denom = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y);
	float l0 = ((v1.y - v2.y) * (sx - v2.x) + (v2.x - v1.x) * (sy - v2.y)) / denom;
	float l1 = ((v2.y - v0.y) * (sx - v2.x) + (v0.x - v2.x) * (sy - v2.y)) / denom;
	float l2 = 1.0f - l0 - l1;

	float iw0 = 1.0f / v0.w, iw1 = 1.0f / v1.w, iw2 = 1.0f / v2.w;
	RefSample r;
	r.invw = l0 * iw0 + l1 * iw1 + l2 * iw2;
	r.w = 1.0f / r.invw;
	r.pcorr = (l0 * (v0.pvar[0] / v0.w) + l1 * (v1.pvar[0] / v1.w) + l2 * (v2.pvar[0] / v2.w)) / r.invw;
	r.aff = l0 * v0.avar[0] + l1 * v1.avar[0] + l2 * v2.avar[0];
	r.z = l0 * v0.z + l1 * v1.z + l2 * v2.z;
	return r;
}

const int laneDx[4] = { 0, 1, 0, 1 };
const int laneDy[4] = { 0, 0, 1, 1 };

int runQuads()
{
	// --- Single covered lane; helpers carry extrapolated unequal-w data. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		// Deliberately unequal w.
		RasterizerVertex v0 = rv(2.2f, 2.2f, 0.2f, 1.0f, 1.0f, 10.0f);
		RasterizerVertex v1 = rv(3.0f, 2.2f, 0.4f, 2.0f, 2.0f, 20.0f);
		RasterizerVertex v2 = rv(2.2f, 3.0f, 0.8f, 0.5f, 3.0f, 30.0f);
		r.drawTriangle(v0, v1, v2);

		UNIT_CHECK(QuadRecorder::log.size() == 1);
		const PixelQuad *q = findQuad(2, 2);
		UNIT_CHECK(q != nullptr);
		UNIT_CHECK(q->coverageMask == 0x1u); // only top-left covered
		UNIT_CHECK(q->primitiveIndex == 0);

		// Lane order: top-left, top-right, bottom-left, bottom-right.
		UNIT_CHECK(q->lanes[0].x == 2 && q->lanes[0].y == 2);
		UNIT_CHECK(q->lanes[1].x == 3 && q->lanes[1].y == 2);
		UNIT_CHECK(q->lanes[2].x == 2 && q->lanes[2].y == 3);
		UNIT_CHECK(q->lanes[3].x == 3 && q->lanes[3].y == 3);

		// Helpers must hold distinct, extrapolated values, not copies.
		UNIT_CHECK(!near(q->lanes[0].pvar[0], q->lanes[1].pvar[0]));
		UNIT_CHECK(!near(q->lanes[1].pvar[0], q->lanes[3].pvar[0]));
		UNIT_CHECK(!near(q->lanes[2].pvar[0], q->lanes[3].pvar[0]));

		for (int l = 0; l < 4; ++l) {
			float sx = 2.0f + laneDx[l] + 0.5f;
			float sy = 2.0f + laneDy[l] + 0.5f;
			RefSample ref = reference(v0, v1, v2, sx, sy);
			const PixelData &p = q->lanes[l];
			UNIT_CHECK(near(p.invw, ref.invw));
			UNIT_CHECK(near(p.w, ref.w));
			UNIT_CHECK(near(p.pvar[0], ref.pcorr));
			UNIT_CHECK(near(p.avar[0], ref.aff));
			UNIT_CHECK(near(p.z, ref.z));
			UNIT_CHECK(std::isfinite(p.invw) && p.invw != 0.0f);
		}

		// The scalar path must never run for a quad shader.
		UNIT_CHECK(QuadRecorder::scalarHits == 0);
	}

	// --- Fine / coarse derivative semantics on arbitrary scalars. ---
	{
		float v[4] = { 1.0f, 3.0f, 2.0f, 6.0f };
		// fine x: right-minus-left on the lane's own row.
		UNIT_CHECK(near(quadDdxFine(v, 0), 2.0f));
		UNIT_CHECK(near(quadDdxFine(v, 1), 2.0f));
		UNIT_CHECK(near(quadDdxFine(v, 2), 4.0f));
		UNIT_CHECK(near(quadDdxFine(v, 3), 4.0f));
		// fine y: bottom-minus-top on the lane's own column.
		UNIT_CHECK(near(quadDdyFine(v, 0), 1.0f));
		UNIT_CHECK(near(quadDdyFine(v, 2), 1.0f));
		UNIT_CHECK(near(quadDdyFine(v, 1), 3.0f));
		UNIT_CHECK(near(quadDdyFine(v, 3), 3.0f));
		// coarse: every lane returns the top-left differences.
		for (int l = 0; l < 4; ++l) {
			UNIT_CHECK(near(quadDdxCoarse(v, l), 2.0f));
			UNIT_CHECK(near(quadDdyCoarse(v, l), 1.0f));
		}
	}

	// --- Nonlinear shader-computed scalar uses real finite differences. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		// Unequal w; the quad anchored at (4,4) is fully inside the triangle.
		r.drawTriangle(rv(4, 4, 0.1f, 1.0f, 0.0f, 1.0f),
			rv(12, 4, 0.2f, 2.5f, 0.0f, 4.0f),
			rv(4, 12, 0.3f, 0.7f, 0.0f, 7.0f));

		const PixelQuad *q = findQuad(4, 4);
		UNIT_CHECK(q != nullptr);
		UNIT_CHECK(q->coverageMask == 0xFu);

		float s[4], t[4];
		for (int l = 0; l < 4; ++l) {
			s[l] = q->lanes[l].pvar[0];
			t[l] = s[l] * s[l]; // nonlinear operation after interpolation
		}

		// The reported derivative is the difference of the actual nonlinear
		// per-lane results, not an analytic UV derivative.
		UNIT_CHECK(near(quadDdxFine(t, 0), t[1] - t[0]));
		UNIT_CHECK(near(quadDdyFine(t, 1), t[3] - t[1]));
		UNIT_CHECK(!near(t[1] - t[0], s[1] - s[0])); // differs from linear ddx
		UNIT_CHECK(near(quadDdxCoarse(t, 3), t[1] - t[0]));
		UNIT_CHECK(near(quadDdyCoarse(t, 2), t[2] - t[0]));
	}

	// --- Two primitives occupying the same quad stay separate and correct. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		RasterizerVertex vs[4] = {
			rv(0, 0, 0, 1, 0, 100), rv(2, 0, 0, 1, 0, 100),
			rv(0, 2, 0, 1, 0, 100), rv(2, 2, 0, 1, 0, 200)
		};
		int idx[6] = { 0, 1, 2, 1, 3, 2 };
		r.drawTriangleList(vs, idx, 6);

		UNIT_CHECK(QuadRecorder::log.size() == 2);
		std::set<uint64_t> ids;
		for (const PixelQuad &q : QuadRecorder::log)
			ids.insert(q.primitiveIndex);
		UNIT_CHECK(ids == std::set<uint64_t>({ 0, 1 }));

		const PixelQuad *q0 = nullptr;
		const PixelQuad *q1 = nullptr;
		for (const PixelQuad &q : QuadRecorder::log) {
			if (q.primitiveIndex == 0) q0 = &q;
			if (q.primitiveIndex == 1) q1 = &q;
		}
		UNIT_CHECK(q0 && q1);
		UNIT_CHECK(q0->lanes[0].x == q1->lanes[0].x && q0->lanes[0].y == q1->lanes[0].y);
		// No data from the neighbouring primitive leaks in.
		UNIT_CHECK(!near(q0->lanes[0].pvar[0], q1->lanes[0].pvar[0]));
	}

	// --- A zero perspective denominator throws domain_error. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		// invw is linear and crosses zero on the sampled quad.
		bool threw = false;
		try {
			r.drawTriangle(rv(0, 0, 0, 1.0f, 0, 1),
				rv(8, 0, 0, 1.0f, 0, 2),
				rv(0, 8, 0, -3.0f / 13.0f, 0, 3));
		} catch (const std::domain_error &) {
			threw = true;
		}
		UNIT_CHECK(threw);
	}

	// --- Odd scissor rectangle cuts edge lanes precisely. ---
	{
		Rasterizer r;
		r.setScissorRect(1, 1, 7, 7); // x,y in [1, 8)
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		RasterizerVertex a = rv(0, 0, 0, 1, 0, 1);
		RasterizerVertex b = rv(10, 0, 0, 1, 0, 2);
		RasterizerVertex c = rv(0, 10, 0, 1, 0, 3);
		r.drawTriangle(a, b, c);
		UNIT_CHECK(!QuadRecorder::log.empty());

		TriangleEquations eqn(a, b, c, QuadShader::AVarCount, QuadShader::PVarCount);
		bool sawOddBoundaryCovered = false;
		bool sawHelperInside = false;

		for (const PixelQuad &q : QuadRecorder::log) {
			for (int l = 0; l < 4; ++l) {
				const PixelData &p = q.lanes[l];
				float cx = p.x + 0.5f, cy = p.y + 0.5f;
				bool edge = eqn.e0.test(cx, cy) && eqn.e1.test(cx, cy) && eqn.e2.test(cx, cy);
				bool scissor = (p.x >= 1 && p.x < 8 && p.y >= 1 && p.y < 8);
				bool covered = ((q.coverageMask >> l) & 1u) != 0;

				// Coverage is exactly edge rule AND scissor.
				UNIT_CHECK(covered == (edge && scissor));

				if (covered && (p.x == 1 || p.y == 1))
					sawOddBoundaryCovered = true;
				if (!covered && scissor)
					sawHelperInside = true;
			}
		}
		// The odd boundary really participates, and helpers are not simply
		// removed by shrinking the traversal box.
		UNIT_CHECK(sawOddBoundaryCovered);
		UNIT_CHECK(sawHelperInside);
	}

	// --- Negative viewport: origins align toward negative infinity. ---
	{
		Rasterizer r;
		r.setScissorRect(-8, -8, 16, 16); // x,y in [-8, 8)
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(-8, -8, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		float a[3][6] = {
			{ -1, -1, 0, 1, 0, 1 },
			{ 1, -1, 0, 1, 0, 1 },
			{ -1, 1, 0, 1, 0, 1 }
		};
		int ids[3] = { 0, 1, 2 };
		vp.setVertexAttribPointer(0, sizeof(a[0]), a);
		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, 3, ids);
		UNIT_CHECK(!QuadRecorder::log.empty());

		bool sawNegativeOrigin = false;
		for (const PixelQuad &q : QuadRecorder::log) {
			int ox = q.lanes[0].x;
			int oy = q.lanes[0].y;
			// Origins are even integers, also for negative coordinates.
			UNIT_CHECK(ox % 2 == 0 && oy % 2 == 0);
			if (ox < 0 || oy < 0)
				sawNegativeOrigin = true;
			for (int l = 0; l < 4; ++l) {
				const PixelData &p = q.lanes[l];
				if ((q.coverageMask >> l) & 1u)
					UNIT_CHECK(p.x >= -8 && p.x < 8 && p.y >= -8 && p.y < 8);
			}
		}
		UNIT_CHECK(sawNegativeOrigin);
		UNIT_CHECK(findQuad(-8, -8) != nullptr);
	}

	// --- All three raster modes produce the identical quad set. ---
	{
		struct Snap {
			int x, y;
			unsigned mask;
			float p0, a0, invw;
		};
		auto snapshot = [](std::vector<Snap> &out) {
			out.clear();
			for (const PixelQuad &q : QuadRecorder::log)
				out.push_back({ q.lanes[0].x, q.lanes[0].y, q.coverageMask,
					q.lanes[0].pvar[0], q.lanes[0].avar[0], q.lanes[0].invw });
			std::sort(out.begin(), out.end(), [](const Snap &s, const Snap &t) {
				return s.x != t.x ? s.x < t.x : s.y < t.y;
			});
		};

		std::vector<Snap> ref;
		RasterizerVertex a = rv(1, 1, 0.1f, 1.0f, 1.0f, 10.0f);
		RasterizerVertex b = rv(9, 1, 0.2f, 2.5f, 2.0f, 20.0f);
		RasterizerVertex c = rv(1, 9, 0.3f, 0.7f, 3.0f, 30.0f);

		for (int mode = 0; mode < 3; ++mode) {
			Rasterizer r;
			r.setRasterMode(static_cast<RasterMode>(mode));
			r.setScissorRect(0, 0, 16, 16);
			r.setQuadPixelShader<QuadShader>();
			QuadRecorder::clear();
			r.drawTriangle(a, b, c);

			std::vector<Snap> cur;
			snapshot(cur);
			if (mode == 0) {
				ref = cur;
			} else {
				UNIT_CHECK(cur.size() == ref.size());
				for (size_t k = 0; k < cur.size(); ++k) {
					UNIT_CHECK(cur[k].x == ref[k].x && cur[k].y == ref[k].y);
					UNIT_CHECK(cur[k].mask == ref[k].mask);
					UNIT_CHECK(near(cur[k].p0, ref[k].p0));
					UNIT_CHECK(near(cur[k].a0, ref[k].a0));
					UNIT_CHECK(near(cur[k].invw, ref[k].invw));
				}
			}
		}
		UNIT_CHECK(!ref.empty());
	}

	return unit_checks;
}

int runClipping()
{
	// --- Union of two fans of one primitive into a single quad callback. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();

		// Simulate the clipped-fan grouping directly: two fans touching
		// different lanes of the same quad are bracketed as one primitive.
		r.beginQuadPrimitive(7);
		r.drawTriangle(rv(2, 2, 0, 1, 0, 1), rv(6, 2, 0, 1, 0, 1), rv(2, 3, 0, 1, 0, 1)); // top row
		r.drawTriangle(rv(2, 3, 0, 1, 0, 1), rv(6, 3, 0, 1, 0, 1), rv(2, 4, 0, 1, 0, 1)); // bottom row
		r.endQuadPrimitive();

		UNIT_CHECK(QuadRecorder::log.size() == 1);
		const PixelQuad &q = QuadRecorder::log.front();
		UNIT_CHECK(q.lanes[0].x == 2 && q.lanes[0].y == 2);
		UNIT_CHECK(q.coverageMask == 0xFu); // union of the two fans
		UNIT_CHECK(q.primitiveIndex == 7);
	}

	// --- A real clipped triangle: deduplicated quads, all index 0. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		// Crosses the left clip plane; the clipper emits a fan.
		float a[3][6] = {
			{ -2, 0, 0, 1, 0, 1 },
			{ 0.8f, -0.8f, 0, 1, 0, 2 },
			{ 0.8f, 0.8f, 0, 1, 0, 3 }
		};
		int ids[3] = { 0, 1, 2 };
		vp.setVertexAttribPointer(0, sizeof(a[0]), a);
		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, 3, ids);

		UNIT_CHECK(!QuadRecorder::log.empty());
		std::set<std::pair<int, int> > keys;
		for (const PixelQuad &q : QuadRecorder::log) {
			keys.insert(std::make_pair(q.lanes[0].x, q.lanes[0].y));
			UNIT_CHECK(q.primitiveIndex == 0);
		}
		// No quad is reported twice for the one primitive.
		UNIT_CHECK(keys.size() == QuadRecorder::log.size());
	}

	// --- Numbering across the 1024 primitive batch boundary. ---
	{
		const int N = 1027; // triangles 0..1026; triangle 5 is fully clipped
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		std::vector<float> verts;
		std::vector<int> idx;
		for (int t = 0; t < N; ++t) {
			float tri[3][6];
			if (t == 5) {
				float off[3][6] = {
					{ -50, -50, 0, 1, 0, 1 },
					{ -40, -50, 0, 1, 0, 1 },
					{ -50, -40, 0, 1, 0, 1 }
				};
				for (int k = 0; k < 3; ++k)
					for (int d = 0; d < 6; ++d)
						tri[k][d] = off[k][d];
			} else {
				float in[3][6] = {
					{ -1, -1, 0, 1, 0, 1 },
					{ 1, -1, 0, 1, 0, 1 },
					{ -1, 1, 0, 1, 0, 1 }
				};
				for (int k = 0; k < 3; ++k)
					for (int d = 0; d < 6; ++d)
						tri[k][d] = in[k][d];
			}
			for (int k = 0; k < 3; ++k) {
				idx.push_back((int)(verts.size() / 6));
				for (int d = 0; d < 6; ++d)
					verts.push_back(tri[k][d]);
			}
		}
		vp.setVertexAttribPointer(0, 6 * sizeof(float), verts.data());
		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, (int)idx.size(), idx.data());

		std::set<uint64_t> ids;
		for (const PixelQuad &q : QuadRecorder::log)
			ids.insert(q.primitiveIndex);

		UNIT_CHECK(ids.count(0) == 1);
		UNIT_CHECK(ids.count(4) == 1);
		UNIT_CHECK(ids.count(5) == 0); // clipped: keeps its number, emits nothing
		UNIT_CHECK(ids.count(6) == 1);
		UNIT_CHECK(ids.count(1024) == 1); // crosses the batch boundary
		UNIT_CHECK(ids.count(1025) == 1);
		UNIT_CHECK(ids.count(1026) == 1);
		UNIT_CHECK(ids.size() == (size_t)(N - 1));
	}

	// --- Entire input clipped away: no quad callback at all. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		float a[3][6] = {
			{ -50, -50, 0, 1, 0, 1 },
			{ -40, -50, 0, 1, 0, 2 },
			{ -50, -40, 0, 1, 0, 3 }
		};
		int ids[3] = { 0, 1, 2 };
		vp.setVertexAttribPointer(0, sizeof(a[0]), a);
		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, 3, ids);
		UNIT_CHECK(QuadRecorder::log.empty());
	}

	// --- Primitive numbering restarts at zero for each drawElements call. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		float a[3][6] = {
			{ -1, -1, 0, 1, 0, 1 },
			{ 1, -1, 0, 1, 0, 1 },
			{ -1, 1, 0, 1, 0, 1 }
		};
		int ids[3] = { 0, 1, 2 };
		vp.setVertexAttribPointer(0, sizeof(a[0]), a);

		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, 3, ids);
		UNIT_CHECK(!QuadRecorder::log.empty());
		for (const PixelQuad &q : QuadRecorder::log)
			UNIT_CHECK(q.primitiveIndex == 0);

		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, 3, ids);
		UNIT_CHECK(!QuadRecorder::log.empty());
		for (const PixelQuad &q : QuadRecorder::log)
			UNIT_CHECK(q.primitiveIndex == 0);
	}

	// --- A culled triangle also keeps its number. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		VertexProcessor vp(&r);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::CCW); // culls the reversed middle triangle
		vp.setVertexShader<VS>();

		// NDC coords; the middle triangle has the opposite winding.
		float t[3][3][6] = {
			{ { -0.8f, -0.8f, 0, 1, 0, 1 }, { -0.8f, -0.4f, 0, 1, 0, 1 }, { -0.4f, -0.8f, 0, 1, 0, 1 } },
			{ {  0.0f, -0.8f, 0, 1, 0, 2 }, {  0.4f, -0.8f, 0, 1, 0, 2 }, {  0.4f, -0.4f, 0, 1, 0, 2 } },
			{ { -0.8f,  0.2f, 0, 1, 0, 3 }, { -0.8f,  0.6f, 0, 1, 0, 3 }, { -0.4f,  0.2f, 0, 1, 0, 3 } }
		};
		std::vector<float> verts;
		std::vector<int> idx;
		for (int ti = 0; ti < 3; ++ti)
			for (int k = 0; k < 3; ++k) {
				idx.push_back((int)(verts.size() / 6));
				for (int d = 0; d < 6; ++d)
					verts.push_back(t[ti][k][d]);
			}
		vp.setVertexAttribPointer(0, 6 * sizeof(float), verts.data());
		QuadRecorder::clear();
		vp.drawElements(DrawMode::Triangle, (int)idx.size(), idx.data());

		std::set<uint64_t> ids;
		for (const PixelQuad &q : QuadRecorder::log)
			ids.insert(q.primitiveIndex);
		UNIT_CHECK(ids.count(0) == 1);
		UNIT_CHECK(ids.count(1) == 0); // culled: reserves the number, emits nothing
		UNIT_CHECK(ids.count(2) == 1);
	}

	return unit_checks;
}

// A legacy rasterizer implementing only the original three pure virtuals.
struct LegacyRasterizer : IRasterizer {
	mutable int points = 0;
	mutable int lines = 0;
	mutable int triangles = 0;

	void drawPointList(const RasterizerVertex *, const int *indices, size_t n) const override
	{
		for (size_t i = 0; i < n; ++i)
			if (indices[i] != -1)
				++points;
	}
	void drawLineList(const RasterizerVertex *, const int *indices, size_t n) const override
	{
		for (size_t i = 0; i + 2 <= n; i += 2)
			if (indices[i] != -1)
				++lines;
	}
	void drawTriangleList(const RasterizerVertex *, const int *indices, size_t n) const override
	{
		for (size_t i = 0; i + 3 <= n; i += 3)
			if (indices[i] != -1)
				++triangles;
	}
};

int runCompat()
{
	// --- Legacy IRasterizer still compiles and runs through the pipeline. ---
	{
		LegacyRasterizer legacy;
		// The new virtuals have usable defaults.
		UNIT_CHECK(legacy.quadShaderEnabled() == false);
		legacy.beginQuadPrimitive(3);
		legacy.endQuadPrimitive();

		VertexProcessor vp(&legacy);
		vp.setViewport(0, 0, 16, 16);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();

		float a[3][6] = {
			{ -1, -1, 0, 1, 0, 1 },
			{ 1, -1, 0, 1, 0, 1 },
			{ -1, 1, 0, 1, 0, 1 }
		};
		int ids[3] = { 0, 1, 2 };
		vp.setVertexAttribPointer(0, sizeof(a[0]), a);
		vp.drawElements(DrawMode::Triangle, 3, ids);
		UNIT_CHECK(legacy.triangles >= 1);
	}

	// --- Points/lines throw logic_error while a quad shader is installed. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		RasterizerVertex v = rv(1, 1, 0, 1, 0, 1);
		RasterizerVertex v2 = rv(2, 2, 0, 1, 0, 1);

		bool pThrow = false, lThrow = false;
		try { r.drawPoint(v); } catch (const std::logic_error &) { pThrow = true; }
		try { r.drawLine(v, v2); } catch (const std::logic_error &) { lThrow = true; }
		UNIT_CHECK(pThrow && lThrow);

		int pi[1] = { 0 };
		int li[2] = { 0, 1 };
		bool plThrow = false, llThrow = false;
		try { r.drawPointList(&v, pi, 1); } catch (const std::logic_error &) { plThrow = true; }
		try { r.drawLineList(&v, li, 2); } catch (const std::logic_error &) { llThrow = true; }
		UNIT_CHECK(plThrow && llThrow);
	}

	// --- Switch back to a scalar shader restores the original behaviour. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setQuadPixelShader<QuadShader>();
		QuadRecorder::clear();
		r.drawTriangle(rv(1, 1, 0, 1, 0, 1), rv(9, 1, 0, 1, 0, 1), rv(1, 9, 0, 1, 0, 1));
		UNIT_CHECK(!QuadRecorder::log.empty());
		size_t quadCount = QuadRecorder::log.size();

		// Back to the scalar path.
		r.setPixelShader<ScalarShader>();
		g_scalarCalls = 0;
		r.drawTriangle(rv(1, 1, 0, 1, 0, 1), rv(9, 1, 0, 1, 0, 1), rv(1, 9, 0, 1, 0, 1));
		UNIT_CHECK(g_scalarCalls > 0);
		UNIT_CHECK(QuadRecorder::log.size() == quadCount); // no new quad callbacks

		// Points and lines work again without throwing.
		RasterizerVertex v = rv(1, 1, 0, 1, 0, 1);
		bool threw = false;
		try {
			r.drawPoint(v);
			r.drawLine(v, rv(2, 2, 0, 1, 0, 1));
		} catch (const std::logic_error &) {
			threw = true;
		}
		UNIT_CHECK(!threw);
	}

	// --- Default scalar (null) shader remains usable. ---
	{
		Rasterizer r;
		r.setScissorRect(0, 0, 16, 16);
		r.setPixelShader<NullPixelShader>();
		bool threw = false;
		try {
			r.drawTriangle(rv(1, 1, 0, 1, 0, 1), rv(9, 1, 0, 1, 0, 1), rv(1, 9, 0, 1, 0, 1));
		} catch (...) {
			threw = true;
		}
		UNIT_CHECK(!threw);
	}

	return unit_checks;
}

} // namespace

int unit_run_group(const char *name)
{
	if (std::string(name) == "quads")
		return runQuads();
	if (std::string(name) == "clipping")
		return runClipping();
	if (std::string(name) == "compat")
		return runCompat();
	return -1;
}
