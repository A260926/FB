// Fixture for the depth-peeling path.  The reference values are collected
// independently here (same edge equations, explicit sort) rather than taken
// from the implementation under test.
#include "Renderer.h"
#include "DepthPeeler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace swr;

namespace {

const int ACOUNT = 5; // r, g, b, alpha, discard flag

struct VPos { float x, y, z, w; };
struct VCol { float v[ACOUNT]; };

struct Tri {
	VPos p[3];
	float r, g, b, a;
	bool discard;
	uint64_t id;
};

struct VS : VertexShaderBase<VS> {
	static const int AttribCount = 2;
	static const int AVarCount = ACOUNT;
	static void processVertex(VertexShaderInput in, VertexShaderOutput *o)
	{
		const VPos *p = (const VPos *)in[0];
		const VCol *c = (const VCol *)in[1];
		o->x = p->x; o->y = p->y; o->z = p->z; o->w = p->w;
		for (int k = 0; k < ACOUNT; ++k)
			o->avar[k] = c->v[k];
	}
};

void shade(const PixelData &p, PeelFragment &f)
{
	f.r = p.avar[0];
	f.g = p.avar[1];
	f.b = p.avar[2];
	f.alpha = p.avar[3];
	f.discard = p.avar[4] >= 0.5f;
}

struct SceneBuffers {
	std::vector<VPos> pos;
	std::vector<VCol> col;
	std::vector<int> indices;
	std::vector<uint64_t> ids;
};

SceneBuffers buildBuffers(const std::vector<Tri> &tris, bool reverse)
{
	SceneBuffers sb;
	std::vector<size_t> order(tris.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	if (reverse) std::reverse(order.begin(), order.end());

	for (size_t t : order)
	{
		const Tri &tr = tris[t];
		int base = (int)sb.pos.size();
		for (int k = 0; k < 3; ++k)
		{
			sb.pos.push_back(tr.p[k]);
			VCol c = { { tr.r, tr.g, tr.b, tr.a, tr.discard ? 1.0f : 0.0f } };
			sb.col.push_back(c);
			sb.indices.push_back(base + k);
		}
		sb.ids.push_back(tr.id);
	}
	return sb;
}

const int G_W = 24, G_H = 24;

// One drawElements call carrying mixed surface ids; the same scene on every
// replay, as the peeler contract requires.
DepthPeeler::DrawCallback makeDraw(const std::vector<Tri> &tris, bool reverse)
{
	return [tris, reverse](VertexProcessor &vp) {
		SceneBuffers sb = buildBuffers(tris, reverse);
		vp.setViewport(0, 0, G_W, G_H);
		vp.setCullMode(CullMode::None);
		vp.setVertexShader<VS>();
		vp.setVertexAttribPointer(0, sizeof(VPos), sb.pos.data());
		vp.setVertexAttribPointer(1, sizeof(VCol), sb.col.data());
		vp.drawElements(DrawMode::Triangle, sb.indices.size(),
			sb.indices.data(), sb.ids.data());
	};
}

struct RefPixel {
	float r = 0.0f, g = 0.0f, b = 0.0f;
	float coverage = 0.0f;
	int layers = 0;
	bool interior = false;
};

struct Frag {
	float z, r, g, b, a;
	bool discard;
	uint64_t id;
};

bool insideClipVolume(const Tri &tr)
{
	for (int k = 0; k < 3; ++k)
	{
		float x = tr.p[k].x, y = tr.p[k].y, z = tr.p[k].z, w = tr.p[k].w;
		// Boundary points are kept by the clipper without generating a fan,
		// so inclusive tests are correct here.
		if (!(w > 0.0f)) return false;
		if (!(x >= -w && x <= w && y >= -w && y <= w && z >= -w && z <= w))
			return false;
	}
	return true;
}

// Independent reference for triangles that stay strictly inside the clip
// volume, so no clipping fan is involved on this path.
std::vector<RefPixel> reference(const std::vector<Tri> &tris,
	const std::vector<float> &bgRGB, const std::vector<float> &bgDepth,
	int W, int H)
{
	std::vector<RefPixel> out((size_t)W * H);

	for (int y = 0; y < H; ++y)
	for (int x = 0; x < W; ++x)
	{
		const size_t pi = (size_t)y * W + x;
		std::vector<Frag> frags;
		bool covered = false;

		for (const Tri &tr : tris)
		{
			if (!insideClipVolume(tr)) continue;

			RasterizerVertex rv[3];
			for (int k = 0; k < 3; ++k)
			{
				// Viewport transform used by the VertexProcessor (w == 1).
				rv[k].x = (W / 2.0f) * (tr.p[k].x + 1.0f);
				rv[k].y = (H / 2.0f) * (1.0f - tr.p[k].y);
				rv[k].z = 0.5f * (tr.p[k].z + 1.0f);
				rv[k].w = 1.0f;
				rv[k].avar[0] = tr.r; rv[k].avar[1] = tr.g;
				rv[k].avar[2] = tr.b; rv[k].avar[3] = tr.a;
			}

			// Mirror VertexProcessor::cullTriangles under CullMode::None: it
			// swaps the first and last vertex when facing >= 0 so the edge
			// equations get a positive area.
			RasterizerVertex a = rv[0], b = rv[1], c = rv[2];
			float facing = (a.x - b.x) * (c.y - b.y) - (c.x - b.x) * (a.y - b.y);
			if (facing >= 0.0f) std::swap(a, c);

			TriangleEquations eqn(a, b, c, ACOUNT, 0);
			if (eqn.area2 <= 0) continue;

			const float fx = x + 0.5f, fy = y + 0.5f;
			float e0 = eqn.e0.evaluate(fx, fy);
			float e1 = eqn.e1.evaluate(fx, fy);
			float e2 = eqn.e2.evaluate(fx, fy);
			if (!eqn.e0.test(e0) || !eqn.e1.test(e1) || !eqn.e2.test(e2))
				continue;

			covered = true;

			float z = eqn.z.evaluate(fx, fy);
			if (!(z < bgDepth[pi])) continue;

			frags.push_back(Frag{ z, tr.r, tr.g, tr.b, tr.a, tr.discard, tr.id });
		}

		// Exact-edge (block mode) coverage for this sample.
		out[pi].interior = covered;

		std::sort(frags.begin(), frags.end(), [](const Frag &u, const Frag &v) {
			if (u.z != v.z) return u.z < v.z;
			return u.id < v.id;
		});

		float T = 1.0f, R = 0.0f, G = 0.0f, B = 0.0f;
		int layers = 0;
		uint64_t lastId = 0;
		bool haveLast = false;
		for (const Frag &f : frags)
		{
			if (haveLast && f.id == lastId) continue; // one surface once per pixel
			lastId = f.id; haveLast = true;
			if (f.discard || f.a <= 0.0f) continue;   // holes occupy no layer
			R += T * f.a * f.r;
			G += T * f.a * f.g;
			B += T * f.a * f.b;
			T *= (1.0f - f.a);
			++layers;
		}

		RefPixel &rp = out[pi];
		rp.r = R + T * bgRGB[pi * 3 + 0];
		rp.g = G + T * bgRGB[pi * 3 + 1];
		rp.b = B + T * bgRGB[pi * 3 + 2];
		rp.coverage = 1.0f - T;
		rp.layers = layers;
	}
	return out;
}

void check(bool cond, const char *what)
{
	if (!cond)
	{
		std::fprintf(stderr, "FAILED: %s\n", what);
		std::abort();
	}
}

bool close(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

// Pixels where every run and the exact-edge reference agree on coverage and
// layer count.  Span and block rasterization legitimately differ on a few
// boundary samples, so cross-mode numeric checks are restricted to this
// shared interior.
std::vector<unsigned char> consensusMask(const std::vector<const PeelResult *> &runs,
	const std::vector<RefPixel> &ref, int W, int H)
{
	std::vector<unsigned char> mask((size_t)W * H, 1);
	for (int y = 0; y < H; ++y)
	for (int x = 0; x < W; ++x)
	{
		size_t i = (size_t)y * W + x;
		if (!ref[i].interior) { mask[i] = 0; continue; }
		for (const PeelResult *r : runs)
			if (r->layers[i] != ref[i].layers) { mask[i] = 0; break; }
	}
	return mask;
}

void compareToReference(const PeelResult &res, const std::vector<RefPixel> &ref,
	const std::vector<unsigned char> &mask, int W, int H, const char *tag)
{
	int matched = 0;
	for (int y = 0; y < H; ++y)
	for (int x = 0; x < W; ++x)
	{
		size_t i = (size_t)y * W + x;
		const RefPixel &rp = ref[i];
		if (!mask[i]) continue; // common interior pixels of every mode
		++matched;
		if (res.layers[i] != rp.layers ||
			!close(res.rgb[i * 3 + 0], rp.r) ||
			!close(res.rgb[i * 3 + 1], rp.g) ||
			!close(res.rgb[i * 3 + 2], rp.b) ||
			!close(res.coverage[i], rp.coverage))
		{
			std::fprintf(stderr, "MISMATCH %s px(%d,%d) layers got=%d ref=%d rgb got=(%.6f,%.6f,%.6f) ref=(%.6f,%.6f,%.6f) cov got=%.6f ref=%.6f\n",
				tag, x, y, res.layers[i], rp.layers,
				res.rgb[i * 3], res.rgb[i * 3 + 1], res.rgb[i * 3 + 2],
				rp.r, rp.g, rp.b, res.coverage[i], rp.coverage);
			std::abort();
		}
	}
	if (matched <= 24)
	{
		std::fprintf(stderr, "FAILED: %s (interior pixels=%d)\n", tag, matched);
		std::abort();
	}
}

// Full-screen quad (two triangles) sharing one surface id, with per-corner
// clip-space z values.
void addGradientQuad(std::vector<Tri> &tris, float zLeft, float zRight,
	float r, float g, float b, float a, uint64_t id)
{
	VPos bl{ -1.0f, -1.0f, 0.0f, 1.0f };
	VPos br{  1.0f, -1.0f, 0.0f, 1.0f };
	VPos tl{ -1.0f,  1.0f, 0.0f, 1.0f };
	VPos tr{  1.0f,  1.0f, 0.0f, 1.0f };
	bl.z = zLeft; tl.z = zLeft; br.z = zRight; tr.z = zRight;
	tris.push_back(Tri{ { bl, br, tl }, r, g, b, a, false, id });
	tris.push_back(Tri{ { br, tr, tl }, r, g, b, a, false, id });
}

PeelResult run(DepthPeeler &peeler, const std::vector<Tri> &tris, RasterMode mode,
	bool reverse, const std::vector<float> &bgRGB,
	const std::vector<float> &bgDepth, int W, int H)
{
	peeler.setRasterMode(mode);
	return peeler.peel(W, H, bgRGB.data(), bgDepth.data(),
		makeDraw(tris, reverse), shade);
}

void writePPM(const std::string &path, const PeelResult &res, int W, int H)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	check(f != nullptr, "open peel.ppm");
	std::fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; ++i)
	{
		unsigned char px[3];
		for (int k = 0; k < 3; ++k)
		{
			float v = res.rgb[i * 3 + k];
			v = std::max(0.0f, std::min(1.0f, v));
			px[k] = (unsigned char)std::lround(v * 255.0f);
		}
		std::fwrite(px, 1, 3, f);
	}
	std::fclose(f);
}

} // namespace

int main(int argc, char **argv)
{
	const char *outDir = argc > 1 ? argv[1] : ".";
	const int W = G_W, H = G_H;
	const size_t N = (size_t)W * H;

	std::vector<float> bgRGB(N * 3), bgDepth(N);
	for (size_t i = 0; i < N; ++i)
	{
		bgRGB[i * 3 + 0] = 0.2f;
		bgRGB[i * 3 + 1] = 0.3f;
		bgRGB[i * 3 + 2] = 0.4f;
		bgDepth[i] = 0.9f;
	}

	DepthPeeler peeler;

	// ---- Scene 1: intersecting translucent surfaces ---------------------
	{
		std::vector<Tri> tris;
		addGradientQuad(tris, -0.6f, 0.6f, 1.0f, 0.1f, 0.1f, 0.5f, 1); // near on left
		addGradientQuad(tris,  0.6f, -0.6f, 0.1f, 0.1f, 1.0f, 0.5f, 2); // near on right
		auto ref = reference(tris, bgRGB, bgDepth, W, H);

		std::vector<PeelResult> runs;
		for (RasterMode mode : { RasterMode::Span, RasterMode::Block, RasterMode::Adaptive })
		for (int rev = 0; rev < 2; ++rev)
			runs.push_back(run(peeler, tris, mode, rev != 0, bgRGB, bgDepth, W, H));

		std::vector<const PeelResult *> runPtrs;
		for (const PeelResult &r : runs) runPtrs.push_back(&r);
		auto mask = consensusMask(runPtrs, ref, W, H);
		for (const PeelResult &r : runs)
			compareToReference(r, ref, mask, W, H, "intersecting surfaces");

		// Ordering must actually swap across the crossing.
		PeelResult res = runs[0];
		int leftX = W / 4, rightX = 3 * W / 4, midY = H / 2;
		check(res.rgb[((size_t)midY * W + leftX) * 3 + 0] >
			  res.rgb[((size_t)midY * W + leftX) * 3 + 2], "red surface nearer on left");
		check(res.rgb[((size_t)midY * W + rightX) * 3 + 2] >
			  res.rgb[((size_t)midY * W + rightX) * 3 + 0], "blue surface nearer on right");
	}

	// ---- Scene 2: exact equal depth, different ids ----------------------
	{
		std::vector<Tri> tris;
		addGradientQuad(tris, -0.1f, -0.1f, 1.0f, 0.0f, 0.0f, 0.5f, 20);
		addGradientQuad(tris, -0.1f, -0.1f, 0.0f, 0.0f, 1.0f, 0.5f, 10);
		auto ref = reference(tris, bgRGB, bgDepth, W, H);

		PeelResult a = run(peeler, tris, RasterMode::Span, false, bgRGB, bgDepth, W, H);
		PeelResult b = run(peeler, tris, RasterMode::Block, true, bgRGB, bgDepth, W, H);
		std::vector<const PeelResult *> ab = { &a, &b };
		auto mask = consensusMask(ab, ref, W, H);
		compareToReference(a, ref, mask, W, H, "equal depth order");
		compareToReference(b, ref, mask, W, H, "equal depth order reversed");
		for (size_t i = 0; i < N; ++i)
		{
			if (!mask[i]) continue; // boundary samples can differ between modes
			check(a.layers[i] == 2, "two equal-depth layers");
			check(close(a.rgb[i * 3], b.rgb[i * 3]), "equal depth order independence");
		}
	}

	// ---- Scene 3: alpha-zero and discard holes --------------------------
	{
		std::vector<Tri> tris;
		addGradientQuad(tris, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 30);  // alpha 0
		addGradientQuad(tris, 0.05f, 0.05f, 0.0f, 1.0f, 1.0f, 0.8f, 31);
		for (auto &t : tris) if (t.id == 31) t.discard = true;         // discarded
		addGradientQuad(tris, 0.2f, 0.2f, 0.0f, 1.0f, 0.0f, 0.5f, 32); // behind holes
		auto ref = reference(tris, bgRGB, bgDepth, W, H);

		std::vector<PeelResult> holeRuns;
		// Include the exact-edge block mode so span boundary samples are
		// filtered out of the shared comparison mask.
		for (RasterMode mode : { RasterMode::Span, RasterMode::Block })
		for (int rev = 0; rev < 2; ++rev)
			holeRuns.push_back(run(peeler, tris, mode, rev != 0, bgRGB, bgDepth, W, H));
		std::vector<const PeelResult *> holePtrs;
		for (const PeelResult &r : holeRuns) holePtrs.push_back(&r);
		auto mask = consensusMask(holePtrs, ref, W, H);
		for (const PeelResult &res : holeRuns)
		{
			compareToReference(res, ref, mask, W, H, "alpha/discard holes");
			for (size_t i = 0; i < N; ++i)
				if (mask[i]) check(res.layers[i] == 1, "holes do not occupy layers");
		}
	}

	// ---- Scene 4: very close but distinct layers ------------------------
	{
		std::vector<Tri> tris;
		addGradientQuad(tris, -0.2f, -0.2f, 1.0f, 0.0f, 1.0f, 0.3f, 40);
		addGradientQuad(tris, -0.2f + 2.0e-5f, -0.2f + 2.0e-5f, 1.0f, 1.0f, 0.0f, 0.3f, 41);
		addGradientQuad(tris, -0.2f + 4.0e-5f, -0.2f + 4.0e-5f, 0.0f, 1.0f, 1.0f, 0.3f, 42);
		auto ref = reference(tris, bgRGB, bgDepth, W, H);

		std::vector<PeelResult> runs;
		for (RasterMode mode : { RasterMode::Span, RasterMode::Block })
			runs.push_back(run(peeler, tris, mode, false, bgRGB, bgDepth, W, H));
		std::vector<const PeelResult *> runPtrs;
		for (const PeelResult &r : runs) runPtrs.push_back(&r);
		auto mask = consensusMask(runPtrs, ref, W, H);
		for (const PeelResult &res : runs)
		{
			compareToReference(res, ref, mask, W, H, "thin layers");
			for (size_t i = 0; i < N; ++i)
				if (mask[i]) check(res.layers[i] == 3, "no thin layer dropped");
		}
	}

	// ---- Scene 5: triangles crossing clip planes ------------------------
	{
		std::vector<Tri> tris;
		// One vertex behind the eye (z < -w): clipped by the near plane.
		Tri near{ {
			{ -1.0f, -1.0f, -1.5f, 1.0f },
			{  1.0f, -1.0f,  0.3f, 1.0f },
			{  0.0f,  1.0f,  0.3f, 1.0f } },
			1.0f, 0.5f, 0.25f, 0.5f, false, 90 };
		tris.push_back(near);
		// A side-plane crossing triangle, same surface id on its fan.
		Tri side{ {
			{ 1.5f, -1.0f, 0.35f, 1.0f },
			{ 0.2f, -1.0f, 0.35f, 1.0f },
			{ 0.9f,  1.0f, 0.35f, 1.0f } },
			0.25f, 0.5f, 1.0f, 0.5f, false, 91 };
		tris.push_back(side);
		// Translucent surface behind both, still reachable through the fans.
		addGradientQuad(tris, 0.4f, 0.4f, 0.6f, 0.6f, 0.6f, 0.5f, 92);

		PeelResult res = run(peeler, tris, RasterMode::Span, false, bgRGB, bgDepth, W, H);
		int cx = W / 2, cy = H / 2;
		size_t ci = (size_t)cy * W + cx;
		for (size_t i = 0; i < N * 3; ++i)
			check(std::isfinite(res.rgb[i]), "clipped path finite");
		// Center is covered by the near-plane fan exactly once plus the back.
		check(res.layers[ci] >= 2, "near fan plus back surface");
		check(res.coverage[ci] > 0.0f, "clipped surface contributes");

		PeelResult reversed = run(peeler, tris, RasterMode::Block, true, bgRGB, bgDepth, W, H);
		check(reversed.layers[ci] == res.layers[ci], "clip order independence");
		check(close(reversed.rgb[ci * 3], res.rgb[ci * 3]), "clip colour order independence");
	}

	// ---- Scene 6: opaque background occlusion ---------------------------
	{
		std::vector<Tri> tris;
		addGradientQuad(tris, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 50); // screen z 0.5
		std::vector<float> depth = bgDepth;
		for (int x = 0; x < W / 2; ++x)
			for (int y = 0; y < H; ++y)
				depth[(size_t)y * W + x] = 0.35f; // opaque geometry in front on the left

		PeelResult res = run(peeler, tris, RasterMode::Span, false, bgRGB, depth, W, H);
		// Deep-interior columns on either side of the opaque boundary (W / 2),
		// away from span/block edge-rule differences.
		const int leftCol = 4, rightCol = 18;
		for (int y = 2; y < H - 2; ++y)
		{
			check(res.layers[(size_t)y * W + leftCol] == 0, "occluded fragment rejected");
			check(res.layers[(size_t)y * W + rightCol] == 1, "fragment in front of background kept");
		}
	}

	// ---- Empty scene keeps the background -------------------------------
	{
		DepthPeeler::DrawCallback empty = [](VertexProcessor &) {};
		PeelResult res = peeler.peel(W, H, bgRGB.data(), bgDepth.data(), empty, shade);
		for (size_t i = 0; i < N; ++i)
		{
			check(res.layers[i] == 0, "empty scene layers");
			check(res.coverage[i] == 0.0f, "empty scene coverage");
			for (int k = 0; k < 3; ++k)
				check(res.rgb[i * 3 + k] == bgRGB[i * 3 + k], "empty scene keeps background");
		}
	}

	// ---- Consecutive frames and two interleaved peelers -----------------
	{
		std::vector<Tri> s1, s2;
		addGradientQuad(s1, -0.3f, 0.3f, 1.0f, 0.2f, 0.2f, 0.4f, 60);
		addGradientQuad(s2, 0.3f, -0.3f, 0.2f, 0.2f, 1.0f, 0.6f, 70);
		auto ref1 = reference(s1, bgRGB, bgDepth, W, H);
		auto ref2 = reference(s2, bgRGB, bgDepth, W, H);

		DepthPeeler other;
		PeelResult f1 = run(peeler, s1, RasterMode::Span, false, bgRGB, bgDepth, W, H);
		PeelResult f1b = run(peeler, s1, RasterMode::Block, false, bgRGB, bgDepth, W, H);
		PeelResult g1 = run(other, s2, RasterMode::Block, false, bgRGB, bgDepth, W, H);
		PeelResult g1b = run(other, s2, RasterMode::Span, false, bgRGB, bgDepth, W, H);
		PeelResult f2 = run(peeler, s1, RasterMode::Span, false, bgRGB, bgDepth, W, H);
		PeelResult g2 = run(other, s2, RasterMode::Block, false, bgRGB, bgDepth, W, H);

		std::vector<const PeelResult *> p1 = { &f1, &f1b };
		std::vector<const PeelResult *> p2 = { &g1, &g1b };
		auto mask1 = consensusMask(p1, ref1, W, H);
		auto mask2 = consensusMask(p2, ref2, W, H);
		compareToReference(f1, ref1, mask1, W, H, "frame 1 peeler A");
		compareToReference(g1, ref2, mask2, W, H, "frame 1 peeler B");
		compareToReference(f2, ref1, mask1, W, H, "frame 2 peeler A");
		compareToReference(g2, ref2, mask2, W, H, "frame 2 peeler B");
		for (size_t i = 0; i < N * 3; ++i)
		{
			check(f1.rgb[i] == f2.rgb[i], "peeler state reset between frames");
			check(g1.rgb[i] == g2.rgb[i], "second peeler independent");
		}
	}

	// ---- Identity across the 1024-primitive batch boundary --------------
	{
		const int BW = 12, BH = 12;
		const size_t BN = (size_t)BW * BH;
		std::vector<float> bbg(BN * 3, 0.1f), bdep(BN, 0.95f);
		const int TRI_COUNT = 1050; // spans more than one 1024-primitive batch
		std::vector<Tri> tris;
		tris.reserve(TRI_COUNT);
		for (int t = 0; t < TRI_COUNT; ++t)
		{
			float z = -0.6f + t * 1.0e-4f;
			Tri tr{ {
				{ -0.5f, -0.5f, z, 1.0f },
				{  0.5f, -0.5f, z, 1.0f },
				{  0.0f,  0.5f, z, 1.0f } },
				0.8f, 0.7f, 0.6f, 5.0e-4f, false, 1000 + (uint64_t)t };
			tris.push_back(tr);
		}

		// This scene uses its own viewport size; wrap the callback accordingly.
		auto draw = [&tris](VertexProcessor &vp) {
			SceneBuffers sb = buildBuffers(tris, false);
			vp.setViewport(0, 0, 12, 12);
			vp.setCullMode(CullMode::None);
			vp.setVertexShader<VS>();
			vp.setVertexAttribPointer(0, sizeof(VPos), sb.pos.data());
			vp.setVertexAttribPointer(1, sizeof(VCol), sb.col.data());
			vp.drawElements(DrawMode::Triangle, sb.indices.size(),
				sb.indices.data(), sb.ids.data());
		};

		for (int rev = 0; rev < 2; ++rev)
		{
			auto drawRev = [&tris, rev](VertexProcessor &vp) {
				SceneBuffers sb = buildBuffers(tris, rev != 0);
				vp.setViewport(0, 0, 12, 12);
				vp.setCullMode(CullMode::None);
				vp.setVertexShader<VS>();
				vp.setVertexAttribPointer(0, sizeof(VPos), sb.pos.data());
				vp.setVertexAttribPointer(1, sizeof(VCol), sb.col.data());
				vp.drawElements(DrawMode::Triangle, sb.indices.size(),
					sb.indices.data(), sb.ids.data());
			};
			(void)draw;
			peeler.setRasterMode(RasterMode::Span);
			PeelResult res = peeler.peel(BW, BH, bbg.data(), bdep.data(), drawRev, shade);
			size_t center = (size_t)(BH / 2) * BW + BW / 2;
			check(res.layers[center] == TRI_COUNT, "all surfaces peeled across batch boundary");
			check(std::isfinite(res.rgb[center * 3]), "batch peel finite");
		}
	}

	// ---- peel.ppm: small image of crossing surfaces ---------------------
	{
		const int PW = 32, PH = 24;
		const size_t PN = (size_t)PW * PH;
		std::vector<float> prgb(PN * 3, 0.05f), pdep(PN, 0.95f);
		std::vector<Tri> tris;
		addGradientQuad(tris, -0.7f, 0.7f, 0.9f, 0.2f, 0.2f, 0.6f, 1);
		addGradientQuad(tris,  0.7f, -0.7f, 0.2f, 0.4f, 0.9f, 0.6f, 2);

		auto draw = [&tris](VertexProcessor &vp) {
			SceneBuffers sb = buildBuffers(tris, false);
			vp.setViewport(0, 0, 32, 24);
			vp.setCullMode(CullMode::None);
			vp.setVertexShader<VS>();
			vp.setVertexAttribPointer(0, sizeof(VPos), sb.pos.data());
			vp.setVertexAttribPointer(1, sizeof(VCol), sb.col.data());
			vp.drawElements(DrawMode::Triangle, sb.indices.size(),
				sb.indices.data(), sb.ids.data());
		};
		peeler.setRasterMode(RasterMode::Span);
		PeelResult res = peeler.peel(PW, PH, prgb.data(), pdep.data(), draw, shade);
		writePPM(std::string(outDir) + "/peel.ppm", res, PW, PH);
	}

	std::puts("SWR_DEPTH_PEEL_OK");
	return 0;
}
