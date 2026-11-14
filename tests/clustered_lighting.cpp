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

#include "Renderer.h"
#include "ClusteredLighting.h"

#include "task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace swr;

namespace {

const float EPS = 1e-5f;

bool closef(float a, float b, float eps = EPS)
{
	return std::fabs(a - b) <= eps;
}

bool closev(const Vec3f &a, const Vec3f &b, float eps = EPS)
{
	return closef(a.x, b.x, eps) && closef(a.y, b.y, eps) && closef(a.z, b.z, eps);
}

Vec3f cross3(const Vec3f &a, const Vec3f &b)
{
	return Vec3f(a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x);
}

// Test frustum description.
struct Frustum {
	float l, r, b, t, n, f;
	int vx, vy, w, h;
};

ClusterConfig makeConfig(const Frustum &fr, int tileW, int tileH, int slices)
{
	ClusterConfig cfg;
	cfg.nearPlane = fr.n;
	cfg.farPlane = fr.f;
	cfg.left = fr.l;
	cfg.right = fr.r;
	cfg.bottom = fr.b;
	cfg.top = fr.t;
	cfg.viewportX = fr.vx;
	cfg.viewportY = fr.vy;
	cfg.viewportWidth = fr.w;
	cfg.viewportHeight = fr.h;
	cfg.tileWidth = tileW;
	cfg.tileHeight = tileH;
	cfg.depthSlices = slices;
	return cfg;
}

// View space point -> clip coordinates for the test frustum.
GBufferVertex viewVertex(float vx, float vy, float vz, float ndcZ,
	const Vec3f &nrm, const Vec3f &alb, const Frustum &fr)
{
	const float d = -vz;
	const float ndcX = (2.0f * fr.n / (fr.r - fr.l)) * (vx / d) -
		(fr.r + fr.l) / (fr.r - fr.l);
	const float ndcY = (2.0f * fr.n / (fr.t - fr.b)) * (vy / d) -
		(fr.t + fr.b) / (fr.t - fr.b);

	GBufferVertex g;
	g.clipX = ndcX * d;
	g.clipY = ndcY * d;
	g.clipZ = ndcZ * d;
	g.clipW = d;
	g.position = Vec3f(vx, vy, vz);
	g.normal = nrm;
	g.albedo = alb;
	return g;
}

// Vertex straight from normalized device coordinates at a constant depth.
GBufferVertex ndcVertex(float ndcX, float ndcY, float ndcZ, float depth,
	const Vec3f &nrm, const Vec3f &alb, const Frustum &fr)
{
	const float slopeX = (ndcX + (fr.r + fr.l) / (fr.r - fr.l)) * (fr.r - fr.l) / (2.0f * fr.n);
	const float slopeY = (ndcY + (fr.t + fr.b) / (fr.t - fr.b)) * (fr.t - fr.b) / (2.0f * fr.n);
	return viewVertex(slopeX * depth, slopeY * depth, -depth, ndcZ, nrm, alb, fr);
}

// A full viewport quad at a constant depth (two triangles), so every pixel
// center is covered.
void fullscreenTriangle(std::vector<GBufferVertex> &verts, std::vector<int> &idx,
	float depth, float ndcZ, const Vec3f &nrm, const Vec3f &alb, const Frustum &fr)
{
	const int base = static_cast<int>(verts.size());
	verts.push_back(ndcVertex(-1.0f, -1.0f, ndcZ, depth, nrm, alb, fr)); // bottom left
	verts.push_back(ndcVertex(1.0f, -1.0f, ndcZ, depth, nrm, alb, fr));  // bottom right
	verts.push_back(ndcVertex(1.0f, 1.0f, ndcZ, depth, nrm, alb, fr));   // top right
	verts.push_back(ndcVertex(-1.0f, 1.0f, ndcZ, depth, nrm, alb, fr));  // top left
	idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

void draw(ClusteredLighting &ctx, const std::vector<GBufferVertex> &verts,
	const std::vector<int> &idx)
{
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setVertexAttribPointer(0, sizeof(GBufferVertex), verts.data());
	vp.setVertexShader<GBufferVertexShader>();
	int *indices = const_cast<int*>(idx.data());
	vp.drawElements(DrawMode::Triangle, idx.size(), indices);
}

struct FragmentSnapshot {
	bool valid;
	Vec3f position;
	Vec3f normal;
	Vec3f albedo;
	float depth;
};

std::vector<FragmentSnapshot> snapshot(ClusteredLighting &ctx)
{
	std::vector<FragmentSnapshot> out;
	const int w = ctx.framebufferWidth();
	const int h = ctx.framebufferHeight();
	out.reserve(static_cast<size_t>(w) * h);
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			FragmentSnapshot s;
			s.valid = ctx.isValid(x, y);
			if (s.valid)
			{
				const GeometryFragment &f = ctx.fragmentAt(x, y);
				s.position = f.position;
				s.normal = f.normal;
				s.albedo = f.albedo;
				s.depth = f.depth;
			}
			else
			{
				s.depth = std::numeric_limits<float>::infinity();
			}
			out.push_back(s);
		}
	}
	return out;
}

// Compare shared pixels. Edge fill may differ between raster modes (the
// upstream span/block fill rules are not unified), so differing validity is
// tolerated; every pixel covered by both must carry identical values.
bool sameSnapshot(const std::vector<FragmentSnapshot> &a, const std::vector<FragmentSnapshot> &b,
	int *sharedCount = nullptr)
{
	if (a.size() != b.size())
		return false;
	int shared = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		if (a[i].valid != b[i].valid)
			continue; // edge coverage difference is allowed
		if (!a[i].valid)
			continue;
		++shared;
		if (!closev(a[i].position, b[i].position) ||
			!closev(a[i].normal, b[i].normal) ||
			!closev(a[i].albedo, b[i].albedo) ||
			!closef(a[i].depth, b[i].depth))
			return false;
	}
	if (sharedCount)
		*sharedCount = shared;
	return shared > 0;
}

// ---------------------------------------------------------------------------
// Independent reference cluster geometry and brute force assignment
// ---------------------------------------------------------------------------

struct RefPlane {
	Vec3f normal;
	float constant;
};

// View ray through a tile pixel BOUNDARY (not a pixel center), matching the
// cluster side planes that pass through the camera origin and the tile edges.
// Fractional coordinates are accepted so a pixel-center ray (x + 0.5) can be
// requested too.
Vec3f nearRay(const ClusterConfig &cfg, float pixelX, float pixelY)
{
	const float ndcX = 2.0f * (pixelX - cfg.viewportX) / cfg.viewportWidth - 1.0f;
	const float ndcY = 1.0f - 2.0f * (pixelY - cfg.viewportY) / cfg.viewportHeight;
	const float qx = ndcX * (cfg.right - cfg.left) / 2.0f +
		(cfg.right + cfg.left) / 2.0f;
	const float qy = ndcY * (cfg.top - cfg.bottom) / 2.0f +
		(cfg.top + cfg.bottom) / 2.0f;
	return normalize(Vec3f(qx, qy, -cfg.nearPlane));
}

RefPlane sidePlane(const Vec3f &rayA, const Vec3f &rayB, const Vec3f &interior)
{
	Vec3f n = normalize(cross3(rayA, rayB));
	if (dot(n, interior) < 0.0f)
		n = n * -1.0f;
	return RefPlane{ n, 0.0f };
}

// Six independently built planes for a cluster.
std::vector<RefPlane> referencePlanes(const ClusterConfig &cfg, const Cluster &cl)
{
	const Vec3f rLeft = nearRay(cfg, cl.minPixelX, cl.minPixelY);
	const Vec3f rRight = nearRay(cfg, cl.maxPixelX, cl.minPixelY);
	const Vec3f rTop = nearRay(cfg, cl.minPixelX, cl.minPixelY);
	const Vec3f rBottom = nearRay(cfg, cl.minPixelX, cl.maxPixelY);

	// A guaranteed interior point: the middle pixel's center ray, which lies
	// strictly inside even for a one pixel wide/tall tile.
	const float centerPixelX = cl.minPixelX + (cl.maxPixelX - cl.minPixelX - 1) / 2.0f + 0.5f;
	const float centerPixelY = cl.minPixelY + (cl.maxPixelY - cl.minPixelY - 1) / 2.0f + 0.5f;
	const float midDepth = (cl.depthNear + cl.depthFar) / 2.0f;
	const Vec3f centerRay = nearRay(cfg, centerPixelX, centerPixelY);
	const Vec3f interior = centerRay * (midDepth / -centerRay.z);

	std::vector<RefPlane> planes;
	planes.push_back(sidePlane(rLeft, nearRay(cfg, cl.minPixelX, cl.maxPixelY), interior));
	planes.push_back(sidePlane(nearRay(cfg, cl.maxPixelX, cl.maxPixelY), rRight, interior));
	planes.push_back(sidePlane(rBottom, nearRay(cfg, cl.maxPixelX, cl.maxPixelY), interior));
	planes.push_back(sidePlane(nearRay(cfg, cl.maxPixelX, cl.minPixelY), rTop, interior));

	RefPlane nearPlane{ Vec3f(0.0f, 0.0f, -1.0f), -cl.depthNear };
	RefPlane farPlane{ Vec3f(0.0f, 0.0f, 1.0f), cl.depthFar };
	if (dot(nearPlane.normal, interior) + nearPlane.constant < 0.0f)
		nearPlane.normal = nearPlane.normal * -1.0f, nearPlane.constant = -nearPlane.constant;
	if (dot(farPlane.normal, interior) + farPlane.constant < 0.0f)
		farPlane.normal = farPlane.normal * -1.0f, farPlane.constant = -farPlane.constant;
	planes.push_back(nearPlane);
	planes.push_back(farPlane);
	return planes;
}

bool refSphereIntersects(const std::vector<RefPlane> &planes, const PointLight &light)
{
	// Closed half spaces: dot(n, C) + c >= -r keeps tangent spheres.
	for (const RefPlane &p : planes)
	{
		if (dot(p.normal, light.position) + p.constant + light.radius < 0.0f)
			return false;
	}
	return true;
}

// Brute force every cluster/light pair.
std::vector<std::vector<int>> referenceAssignment(const ClusterConfig &cfg,
	const std::vector<Cluster> &clusters, const std::vector<PointLight> &lights)
{
	std::vector<std::vector<int>> result(clusters.size());
	for (size_t ci = 0; ci < clusters.size(); ++ci)
	{
		std::vector<RefPlane> planes = referencePlanes(cfg, clusters[ci]);
		for (size_t li = 0; li < lights.size(); ++li)
		{
			if (refSphereIntersects(planes, lights[li]))
				result[ci].push_back(static_cast<int>(li));
		}
	}
	return result;
}

bool assignmentsEqual(const std::vector<Cluster> &clusters,
	const std::vector<std::vector<int>> &reference)
{
	if (clusters.size() != reference.size())
		return false;
	for (size_t i = 0; i < clusters.size(); ++i)
	{
		if (clusters[i].lights != reference[i])
			return false;
	}
	return true;
}

// Exact spec lighting against all lights for one fragment.
Vec3f referenceLighting(const GeometryFragment &f, const std::vector<PointLight> &lights,
	const Vec3f &ambient)
{
	Vec3f color = ambient * f.albedo;
	const Vec3f &P = f.position;
	const Vec3f &N = f.normal;
	for (const PointLight &light : lights)
	{
		const Vec3f V = light.position - P;
		const float dist2 = dot(V, V);
		const float r2 = light.radius * light.radius;
		if (!(dist2 > 0.0f && dist2 < r2))
			continue;
		const float invDist = 1.0f / std::sqrt(dist2);
		const float nDotL = std::max(0.0f, dot(N, V * invDist));
		const float falloff = 1.0f - dist2 / r2;
		const float attenuation = falloff * falloff / std::max(dist2, 1e-6f);
		color += f.albedo * (light.intensity * (nDotL * attenuation));
	}
	return color;
}

// Build a geometry buffer with one fullscreen quad at the given depth. Block
// rasterization fills the whole quad including the boundary rows.
void buildSimpleGBuffer(ClusteredLighting &ctx, const Frustum &fr, float depth,
	const Vec3f &albedo, const Vec3f &normal)
{
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setRasterMode(RasterMode::Block);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);

	std::vector<GBufferVertex> verts;
	std::vector<int> idx;
	fullscreenTriangle(verts, idx, depth, 0.0f, normal, albedo, fr);
	draw(ctx, verts, idx);
	ctx.endGeometryPass();
}

} // unnamed namespace

// ===========================================================================
// capture group
// ===========================================================================

static int capture_basic_geometry()
{
	// Odd dimensions put a pixel center exactly on the central view ray.
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 0, 0, 17, 13 };
	ClusteredLighting ctx;
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setRasterMode(RasterMode::Block);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);

	std::vector<GBufferVertex> verts;
	std::vector<int> idx;
	const Vec3f nrm(0.0f, 0.0f, -1.0f);
	const Vec3f alb(0.25f, 0.5f, 0.75f);
	fullscreenTriangle(verts, idx, 5.0f, 0.0f, nrm, alb, fr);
	draw(ctx, verts, idx);
	ctx.endGeometryPass();

	int validCount = 0;
	for (int y = 0; y < fr.h; ++y)
		for (int x = 0; x < fr.w; ++x)
			if (ctx.isValid(x, y))
				++validCount;
	TASK_CHECK(validCount == fr.w * fr.h);

	const GeometryFragment &center = ctx.fragmentAt(fr.w / 2, fr.h / 2);
	TASK_CHECK(center.valid);
	TASK_CHECK(closev(center.position, Vec3f(0.0f, 0.0f, -5.0f), 1e-4f));
	TASK_CHECK(closef(length(center.normal), 1.0f, 1e-5f));
	TASK_CHECK(closev(center.normal, nrm, 1e-5f));
	TASK_CHECK(closev(center.albedo, alb, 1e-5f));
	TASK_CHECK(center.depth >= 0.0f && center.depth <= 1.0f);
	TASK_CHECK(closef(center.depth, 0.5f, 1e-5f));

	// A fresh second context must not see the first context's pixels.
	ClusteredLighting other;
	other.beginGeometryPass(fr.w, fr.h);
	TASK_CHECK(!other.isValid(fr.w / 2, fr.h / 2));
	other.endGeometryPass();
	return 0;
}

static int capture_depth_occlusion_and_tie()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 0, 0, 12, 12 };
	ClusteredLighting ctx;
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);

	// Near red surface (window depth 0.25) and far blue surface (0.75).
	std::vector<GBufferVertex> nearVerts, farVerts;
	std::vector<int> nearIdx, farIdx;
	fullscreenTriangle(nearVerts, nearIdx, 4.0f, -0.5f, Vec3f(0, 0, -1), Vec3f(1, 0, 0), fr);
	fullscreenTriangle(farVerts, farIdx, 8.0f, 0.5f, Vec3f(0, 0, -1), Vec3f(0, 0, 1), fr);

	// Draw the far surface first; the nearer one must still win.
	draw(ctx, farVerts, farIdx);
	draw(ctx, nearVerts, nearIdx);
	ctx.endGeometryPass();

	const GeometryFragment &f = ctx.fragmentAt(6, 6);
	TASK_CHECK(closef(f.depth, 0.25f, 1e-5f));
	TASK_CHECK(closev(f.albedo, Vec3f(1, 0, 0), 1e-5f));

	// Equal depth: the later fragment wins.
	ClusteredLighting tie;
	tie.beginGeometryPass(fr.w, fr.h);
	tie.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &tvp = tie.vertexProcessor();
	tvp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	tvp.setCullMode(CullMode::None);
	std::vector<GBufferVertex> firstVerts, secondVerts;
	std::vector<int> firstIdx, secondIdx;
	fullscreenTriangle(firstVerts, firstIdx, 6.0f, 0.0f, Vec3f(0, 0, -1), Vec3f(0.2f, 0.2f, 0.2f), fr);
	fullscreenTriangle(secondVerts, secondIdx, 6.0f, 0.0f, Vec3f(0, 0, -1), Vec3f(0.8f, 0.8f, 0.8f), fr);
	draw(tie, firstVerts, firstIdx);
	draw(tie, secondVerts, secondIdx);
	tie.endGeometryPass();
	const GeometryFragment &tf = tie.fragmentAt(6, 6);
	TASK_CHECK(closev(tf.albedo, Vec3f(0.8f, 0.8f, 0.8f), 1e-5f));
	TASK_CHECK(closef(tf.depth, 0.5f, 1e-5f));
	return 0;
}

static int capture_discard()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 0, 0, 12, 12 };
	ClusteredLighting ctx;
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);

	// Discard every red fragment; the far blue surface must show through.
	ctx.setDiscardCallback([](const GeometryFragment &f) {
		return f.albedo.x > 0.5f && f.albedo.y < 0.1f;
	});

	std::vector<GBufferVertex> nearVerts, farVerts;
	std::vector<int> nearIdx, farIdx;
	fullscreenTriangle(nearVerts, nearIdx, 4.0f, -0.5f, Vec3f(0, 0, -1), Vec3f(1, 0, 0), fr);
	fullscreenTriangle(farVerts, farIdx, 8.0f, 0.5f, Vec3f(0, 0, -1), Vec3f(0, 0, 1), fr);
	draw(ctx, nearVerts, nearIdx);
	draw(ctx, farVerts, farIdx);
	ctx.endGeometryPass();

	const GeometryFragment &f = ctx.fragmentAt(6, 6);
	TASK_CHECK(closev(f.albedo, Vec3f(0, 0, 1), 1e-5f));
	TASK_CHECK(closef(f.depth, 0.75f, 1e-5f));

	// Without discarding the near red surface wins again.
	ClusteredLighting keep;
	keep.beginGeometryPass(fr.w, fr.h);
	keep.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &kvp = keep.vertexProcessor();
	kvp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	kvp.setCullMode(CullMode::None);
	draw(keep, nearVerts, nearIdx);
	draw(keep, farVerts, farIdx);
	keep.endGeometryPass();
	TASK_CHECK(closev(keep.fragmentAt(6, 6).albedo, Vec3f(1, 0, 0), 1e-5f));
	return 0;
}

static int capture_three_raster_modes_match()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 2, 3, 18, 14 };
	std::vector<FragmentSnapshot> reference;
	for (RasterMode mode : { RasterMode::Span, RasterMode::Block, RasterMode::Adaptive })
	{
		ClusteredLighting ctx;
		ctx.beginGeometryPass(fr.w, fr.h);
		ctx.rasterizer().setRasterMode(mode);
		ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
		VertexProcessor &vp = ctx.vertexProcessor();
		vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
		vp.setCullMode(CullMode::None);

		std::vector<GBufferVertex> verts;
		std::vector<int> idx;
		// A partial triangle so edge coverage is exercised.
		const int base = static_cast<int>(verts.size());
		verts.push_back(ndcVertex(-0.9f, -0.9f, 0.0f, 5.0f, Vec3f(0, 0, -1), Vec3f(0.3f, 0.6f, 0.9f), fr));
		verts.push_back(ndcVertex(0.9f, -0.6f, 0.0f, 5.0f, Vec3f(0, 0, -1), Vec3f(0.3f, 0.6f, 0.9f), fr));
		verts.push_back(ndcVertex(-0.4f, 0.9f, 0.0f, 5.0f, Vec3f(0, 0, -1), Vec3f(0.3f, 0.6f, 0.9f), fr));
		idx.insert(idx.end(), { base, base + 1, base + 2 });
		draw(ctx, verts, idx);
		ctx.endGeometryPass();

		std::vector<FragmentSnapshot> snap = snapshot(ctx);
		int valid = 0;
		for (const auto &s : snap)
			if (s.valid)
				++valid;
		TASK_CHECK(valid > 20);
		if (mode == RasterMode::Span)
			reference = std::move(snap);
		else
			TASK_CHECK(sameSnapshot(reference, snap));
	}
	return 0;
}

static int capture_resize_and_clear()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	buildSimpleGBuffer(ctx, fr, 5.0f, Vec3f(1, 1, 1), Vec3f(0, 0, -1));
	TASK_CHECK(ctx.isValid(8, 8));

	// Resizing must not leave stale pixels.
	ctx.beginGeometryPass(8, 6);
	TASK_CHECK(ctx.framebufferWidth() == 8 && ctx.framebufferHeight() == 6);
	TASK_CHECK(!ctx.isValid(4, 3));
	ctx.endGeometryPass();

	// Re-allocating at the old size starts empty again.
	ctx.beginGeometryPass(16, 16);
	TASK_CHECK(!ctx.isValid(8, 8));
	ctx.endGeometryPass();

	// Two contexts interleaved keep independent buffers.
	ClusteredLighting a, b;
	buildSimpleGBuffer(a, fr, 4.0f, Vec3f(1, 0, 0), Vec3f(0, 0, -1));
	buildSimpleGBuffer(b, fr, 7.0f, Vec3f(0, 1, 0), Vec3f(0, 0, -1));
	TASK_CHECK(closev(a.fragmentAt(8, 8).albedo, Vec3f(1, 0, 0), 1e-5f));
	TASK_CHECK(closev(b.fragmentAt(8, 8).albedo, Vec3f(0, 1, 0), 1e-5f));
	return 0;
}

static int capture_perspective_correct_interpolation()
{
	// Non uniform w: three different view depths, checked against an
	// independent perspective-correct barycentric interpolation.
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 24, 24 };
	ClusteredLighting ctx;
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);

	struct VSpec { float ndcX, ndcY, depth; Vec3f alb; };
	VSpec spec[3] = {
		{ -0.8f, -0.8f, 3.0f, Vec3f(1.0f, 0.0f, 0.0f) },
		{ 0.8f, -0.8f, 9.0f, Vec3f(0.0f, 1.0f, 0.0f) },
		{ -0.8f, 0.8f, 15.0f, Vec3f(0.0f, 0.0f, 1.0f) }
	};

	std::vector<GBufferVertex> verts;
	std::vector<int> idx;
	for (const VSpec &s : spec)
	{
		verts.push_back(ndcVertex(s.ndcX, s.ndcY, 0.0f, s.depth,
			Vec3f(0, 0, -1), s.alb, fr));
	}
	idx = { 0, 1, 2 };
	draw(ctx, verts, idx);
	ctx.endGeometryPass();

	// Window coordinates of the three vertices (NDC -> pixel).
	auto winX = [&](float ndcX) { return fr.vx + fr.w * 0.5f * (ndcX + 1.0f); };
	auto winY = [&](float ndcY) { return fr.vy + fr.h * 0.5f * (1.0f - ndcY); };
	Vec3f p0(winX(spec[0].ndcX), winY(spec[0].ndcY), 0);
	Vec3f p1(winX(spec[1].ndcX), winY(spec[1].ndcY), 0);
	Vec3f p2(winX(spec[2].ndcX), winY(spec[2].ndcY), 0);

	// Pick a pixel guaranteed inside the triangle.
	const int px = 6;
	const int py = 14;
	TASK_CHECK(ctx.isValid(px, py));

	const Vec3f q(px + 0.5f, py + 0.5f, 0.0f);
	auto barycentric = [&](const Vec3f &a, const Vec3f &b, const Vec3f &c) {
		const float area = cross3(b - a, c - a).z;
		const float w0 = cross3(b - q, c - q).z / area;
		const float w1 = cross3(c - q, a - q).z / area;
		const float w2 = cross3(a - q, b - q).z / area;
		return Vec3f(w0, w1, w2);
	};
	const Vec3f bary = barycentric(p0, p1, p2);

	// Perspective correct weights using each vertex's w (= view depth).
	float invW[3] = { 1.0f / spec[0].depth, 1.0f / spec[1].depth, 1.0f / spec[2].depth };
	float denom = bary.x * invW[0] + bary.y * invW[1] + bary.z * invW[2];
	Vec3f expectedAlbedo(0, 0, 0);
	const float baryWeight[3] = { bary.x, bary.y, bary.z };
	const Vec3f vertexAlbedo[3] = { spec[0].alb, spec[1].alb, spec[2].alb };
	for (int i = 0; i < 3; ++i)
	{
		const float weight = baryWeight[i] * invW[i] / denom;
		expectedAlbedo += vertexAlbedo[i] * weight;
	}

	const GeometryFragment &f = ctx.fragmentAt(px, py);
	TASK_CHECK(closev(f.albedo, expectedAlbedo, 2e-4f));
	TASK_CHECK(closef(length(f.normal), 1.0f, 1e-5f));
	return 0;
}

// ===========================================================================
// clusters group
// ===========================================================================

static int clusters_grid_layout_and_depth_boundaries()
{
	// 18x14 viewport with 8x8 tiles -> non square last tile, K=4.
	Frustum fr{ -2.0f, 1.5f, -1.0f, 2.0f, 1.0f, 16.0f, 3, 5, 18, 14 };
	ClusteredLighting ctx;
	ClusterConfig cfg = makeConfig(fr, 8, 8, 4);
	ctx.configureClusters(cfg);

	TASK_CHECK(ctx.tileCountX() == 3 && ctx.tileCountY() == 2 && ctx.sliceCount() == 4);
	TASK_CHECK(ctx.clusterCount() == 3 * 2 * 4);

	const Cluster &last = ctx.cluster(2, 1, 3);
	TASK_CHECK(last.minPixelX == 3 + 16 && last.maxPixelX == 3 + 18);
	TASK_CHECK(last.minPixelY == 5 + 8 && last.maxPixelY == 5 + 14);
	TASK_CHECK(last.maxPixelX - last.minPixelX == 2);
	TASK_CHECK(last.maxPixelY - last.minPixelY == 6);

	// Logarithmic boundaries d_k = near*(far/near)^(k/K).
	const Cluster &first = ctx.cluster(0, 0, 0);
	TASK_CHECK(closef(first.depthNear, 1.0f, 1e-5f));
	TASK_CHECK(closef(last.depthFar, 16.0f, 1e-5f));
	const float d1 = 1.0f * std::pow(16.0f, 1.0f / 4.0f);
	TASK_CHECK(closef(first.depthFar, d1, 1e-4f));

	// far belongs to the last slice, near to the first.
	TASK_CHECK(ctx.clusterIndexForPixel(4, 6, 16.0f) % 4 == 3);
	TASK_CHECK(ctx.clusterIndexForPixel(4, 6, 1.0f) % 4 == 0);

	// An internal boundary belongs to the later cluster.
	const int atBoundary = ctx.clusterIndexForPixel(4, 6, d1) % 4;
	TASK_CHECK(atBoundary == 1);

	// Pixel outside the viewport has no cluster.
	TASK_CHECK(ctx.clusterIndexForPixel(0, 0, 5.0f) == -1);
	return 0;
}

static int clusters_exhaustive_symmetric()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 24, 20 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 6));

	std::vector<PointLight> lights = {
		{ Vec3f(0.0f, 0.0f, -8.0f), 1.5f, Vec3f(1, 1, 1) },
		{ Vec3f(3.0f, 2.0f, -12.0f), 2.0f, Vec3f(0.5f, 0.2f, 0.1f) },
		{ Vec3f(-4.0f, -3.0f, -20.0f), 3.0f, Vec3f(0.2f, 0.8f, 0.3f) },
		{ Vec3f(0.5f, -0.5f, -3.0f), 0.5f, Vec3f(1, 0, 1) },
	};
	ctx.buildClusters(lights);

	// Re-collect clusters through the public accessor for an independent check.
	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));

	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));

	// Lists are unique and in input order.
	for (const Cluster &c : copy)
	{
		for (size_t i = 1; i < c.lights.size(); ++i)
			TASK_CHECK(c.lights[i - 1] < c.lights[i]);
	}
	return 0;
}

static int clusters_asymmetric_frustum_origin()
{
	Frustum fr{ -2.5f, 1.0f, -0.5f, 2.5f, 2.0f, 40.0f, 7, 11, 20, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 6, 5, 5));

	std::vector<PointLight> lights;
	for (int i = 0; i < 12; ++i)
	{
		PointLight light;
		light.position = Vec3f(-2.0f + 0.35f * i, -1.5f + 0.3f * (i % 5), -5.0f - 2.0f * i);
		light.radius = 1.0f + 0.25f * (i % 4);
		light.intensity = Vec3f(0.4f, 0.4f, 0.4f);
		lights.push_back(light);
	}
	ctx.buildClusters(lights);

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));
	return 0;
}

static int clusters_side_tangency_kept()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 20.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 4));

	// Use the real left side plane of an edge cluster. Put the sphere center
	// strictly OUTSIDE that plane (signed distance negative) but size the
	// sphere so it reaches the plane (near tangent). Only the radius-aware
	// closed half space rule can keep it; a center-only test would drop it.
	const ClusterPlane &leftPlane = ctx.cluster(0, 0, 2).planes[0];
	const Vec3f center(-10.0f, 0.0f, -8.0f);
	const float signedDistance = dot(leftPlane.normal, center) + leftPlane.constant;
	TASK_CHECK(signedDistance < 0.0f);
	const float radius = -signedDistance + 1e-3f; // positive, reaches the plane
	TASK_CHECK(radius > 0.0f);
	PointLight light{ center, radius, Vec3f(1, 1, 1) };
	ctx.buildClusters({ light });

	bool anyCandidate = false;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				if (!ctx.cluster(tx, ty, k).lights.empty())
					anyCandidate = true;
	TASK_CHECK(anyCandidate);

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<PointLight> lights = { light };
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));
	return 0;
}

static int clusters_near_plane_crossing()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 30.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 5));

	// Center in front of the eye but the sphere extends across the near plane.
	PointLight light{ Vec3f(0.0f, 0.0f, -2.5f), 1.5f, Vec3f(1, 1, 1) };
	ctx.buildClusters({ light });

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<PointLight> lights = { light };
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));

	// The nearest slice must contain the light.
	bool inFirstSlice = false;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			if (!ctx.cluster(tx, ty, 0).lights.empty())
				inFirstSlice = true;
	TASK_CHECK(inFirstSlice);
	return 0;
}

static int clusters_eye_plane_crossing()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 30.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 5));

	// Center behind the camera (positive z) but the sphere reaches in front
	// of the eye and into the visible frustum.
	PointLight crossing{ Vec3f(0.0f, 0.0f, 1.0f), 3.0f, Vec3f(1, 1, 1) };
	ctx.buildClusters({ crossing });

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<PointLight> lights = { crossing };
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));
	TASK_CHECK(ctx.clusterSphereCheckCount() > 0);

	// A sphere fully behind the eye and short of the visible region gets no
	// candidate and performs no tests beyond an empty range.
	ClusteredLighting behind;
	behind.configureClusters(makeConfig(fr, 8, 8, 5));
	PointLight absent{ Vec3f(0.0f, 0.0f, 50.0f), 1.0f, Vec3f(1, 1, 1) };
	behind.buildClusters({ absent });
	bool any = false;
	for (int ty = 0; ty < behind.tileCountY(); ++ty)
		for (int tx = 0; tx < behind.tileCountX(); ++tx)
			for (int k = 0; k < behind.sliceCount(); ++k)
				if (!behind.cluster(tx, ty, k).lights.empty())
					any = true;
	TASK_CHECK(!any);
	return 0;
}

static int clusters_layer_boundary_lights()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 16.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 16, 16, 4));
	const float d1 = std::pow(16.0f, 1.0f / 4.0f);

	// Small sphere straddling the first internal depth boundary.
	PointLight light{ Vec3f(0.0f, 0.0f, -d1), 0.25f, Vec3f(1, 1, 1) };
	ctx.buildClusters({ light });

	bool inSlice0 = false, inSlice1 = false;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
		{
			if (!ctx.cluster(tx, ty, 0).lights.empty())
				inSlice0 = true;
			if (!ctx.cluster(tx, ty, 1).lights.empty())
				inSlice1 = true;
		}
	TASK_CHECK(inSlice0 && inSlice1);

	// Exact tangency: the sphere's nearest/farthest point lies exactly on the
	// internal boundary. Closed half spaces must keep the tangent neighbor
	// slice on both sides.
	const float tangentRadius = 0.3f;
	std::vector<PointLight> tangentLights = {
		light,
		PointLight{ Vec3f(0.0f, 0.0f, -(d1 + tangentRadius)), tangentRadius, Vec3f(1, 1, 1) },
		PointLight{ Vec3f(0.0f, 0.0f, -(d1 - tangentRadius)), tangentRadius, Vec3f(1, 1, 1) }
	};
	ctx.buildClusters(tangentLights);
	for (int li = 1; li <= 2; ++li)
	{
		bool nearSlice = false, farSlice = false;
		for (int ty = 0; ty < ctx.tileCountY(); ++ty)
			for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			{
				const Cluster &c0 = ctx.cluster(tx, ty, 0);
				const Cluster &c1 = ctx.cluster(tx, ty, 1);
				if (std::find(c0.lights.begin(), c0.lights.end(), li) != c0.lights.end())
					nearSlice = true;
				if (std::find(c1.lights.begin(), c1.lights.end(), li) != c1.lights.end())
					farSlice = true;
			}
		TASK_CHECK(nearSlice && farSlice);
	}

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, tangentLights);
	TASK_CHECK(assignmentsEqual(copy, reference));
	return 0;
}

static int clusters_many_overlapping_lights_no_drop()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 32, 32 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 6));

	// More overlapping lights than any reasonable fixed capacity. Place them
	// all on a ray through the interior pixel (12,12) of tile (1,1), at a fixed
	// depth, with a small radius so every light lands in the same single
	// cluster.
	const int interiorPixelX = 12;
	const int interiorPixelY = 12;
	const float lightDepth = 10.0f;
	const int packedIndex = ctx.clusterIndexForPixel(interiorPixelX, interiorPixelY, lightDepth);
	const int packedSlice = packedIndex % 6;

	// View space position on the ray through the interior pixel.
	const float ndcX = 2.0f * interiorPixelX / fr.w - 1.0f;
	const float ndcY = 1.0f - 2.0f * interiorPixelY / fr.h;
	const Vec3f anchor(ndcX * lightDepth, ndcY * lightDepth, -lightDepth);

	const int lightCount = 250;
	std::vector<PointLight> lights;
	for (int i = 0; i < lightCount; ++i)
	{
		PointLight light;
		light.position = Vec3f(anchor.x + 0.01f * (i % 7),
			anchor.y + 0.01f * (i % 5), anchor.z);
		light.radius = 0.4f;
		light.intensity = Vec3f(0.1f, 0.1f, 0.1f);
		lights.push_back(light);
	}
	ctx.buildClusters(lights);

	const Cluster &central = ctx.cluster(1, 1, packedSlice);
	TASK_CHECK(static_cast<int>(central.lights.size()) == lightCount);
	for (size_t i = 1; i < central.lights.size(); ++i)
		TASK_CHECK(central.lights[i - 1] < central.lights[i]);
	TASK_CHECK(central.lights.front() == 0 && central.lights.back() == lightCount - 1);

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	TASK_CHECK(assignmentsEqual(copy, reference));
	return 0;
}

static int clusters_stats_are_real()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 40.0f, 0, 0, 32, 32 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 4, 4, 8));
	const size_t totalCombos = static_cast<size_t>(ctx.clusterCount()) * 4;

	std::vector<PointLight> lights = {
		{ Vec3f(0.0f, 0.0f, -10.0f), 0.75f, Vec3f(1, 1, 1) },
		{ Vec3f(2.0f, 2.0f, -20.0f), 0.75f, Vec3f(1, 1, 1) },
		{ Vec3f(-2.0f, -2.0f, -30.0f), 0.75f, Vec3f(1, 1, 1) },
		{ Vec3f(0.5f, 0.5f, -6.0f), 0.5f, Vec3f(1, 1, 1) }
	};
	ctx.buildClusters(lights);

	const size_t checks = ctx.clusterSphereCheckCount();
	TASK_CHECK(checks > 0);
	TASK_CHECK(checks < totalCombos / 4); // clearly sub-Cartesian for local lights

	// The tested set must cover every truly intersecting cluster.
	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<std::vector<int>> reference =
		referenceAssignment(ctx.clusterConfig(), copy, lights);
	size_t trueIntersections = 0;
	for (const auto &list : reference)
		trueIntersections += list.size();
	TASK_CHECK(checks >= trueIntersections);

	// Rebuilding is deterministic and resets the counter first.
	const size_t first = ctx.clusterSphereCheckCount();
	ctx.buildClusters(lights);
	TASK_CHECK(ctx.clusterSphereCheckCount() == first);
	return 0;
}

// Degenerate control: project only the sphere center (zero radius range) and
// assign a single slice with a linear window-z depth mapping. Both must
// compile, run, and be exposed as wrong by the content difference checks.
static std::vector<std::vector<int>> centerOnlyAssignment(const ClusterConfig &cfg,
	const std::vector<Cluster> &clusters, const std::vector<PointLight> &lights)
{
	std::vector<std::vector<int>> result(clusters.size());
	for (size_t li = 0; li < lights.size(); ++li)
	{
		const PointLight &light = lights[li];
		const float d = -light.position.z;
		if (d <= cfg.nearPlane || d > cfg.farPlane)
			continue;
		const float slopeX = light.position.x / d;
		const float slopeY = light.position.y / d;
		const float ndcX = 2.0f * cfg.nearPlane / (cfg.right - cfg.left) * slopeX -
			(cfg.right + cfg.left) / (cfg.right - cfg.left);
		const float ndcY = 2.0f * cfg.nearPlane / (cfg.top - cfg.bottom) * slopeY -
			(cfg.top + cfg.bottom) / (cfg.top - cfg.bottom);
		const int px = cfg.viewportX + static_cast<int>(0.5f * cfg.viewportWidth * (ndcX + 1.0f));
		const int py = cfg.viewportY + static_cast<int>(0.5f * cfg.viewportHeight * (1.0f - ndcY));
		const int localX = px - cfg.viewportX;
		const int localY = py - cfg.viewportY;
		if (localX < 0 || localY < 0 ||
			localX >= cfg.viewportWidth || localY >= cfg.viewportHeight)
			continue;
		const int tx = localX / cfg.tileWidth;
		const int ty = localY / cfg.tileHeight;
		const int tilesX = (cfg.viewportWidth + cfg.tileWidth - 1) / cfg.tileWidth;
		const int tilesY = (cfg.viewportHeight + cfg.tileHeight - 1) / cfg.tileHeight;
		const int k = static_cast<int>((d - cfg.nearPlane) /
			(cfg.farPlane - cfg.nearPlane) * cfg.depthSlices);
		if (tx < 0 || tx >= tilesX || ty < 0 || ty >= tilesY ||
			k < 0 || k >= cfg.depthSlices)
			continue;
		result[(ty * tilesX + tx) * cfg.depthSlices + k].push_back(static_cast<int>(li));
	}
	return result;
}

static int clusters_degenerate_controls_differ()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 30.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	ctx.configureClusters(makeConfig(fr, 8, 8, 5));

	// Sphere crossing the near plane: its center is behind the near plane,
	// so the center-only control drops it while the correct build keeps it.
	PointLight light{ Vec3f(0.0f, 0.0f, -1.0f), 2.5f, Vec3f(1, 1, 1) };
	ctx.buildClusters({ light });

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<PointLight> lights = { light };
	std::vector<std::vector<int>> degenerate =
		centerOnlyAssignment(ctx.clusterConfig(), copy, lights);

	size_t correctCount = 0;
	size_t degenerateCount = 0;
	for (size_t i = 0; i < copy.size(); ++i)
	{
		correctCount += copy[i].lights.size();
		degenerateCount += degenerate[i].size();
	}
	TASK_CHECK(correctCount > 0);
	TASK_CHECK(degenerateCount == 0);
	TASK_CHECK(correctCount != degenerateCount);

	// Window-z treated as the logarithmic depth: a light near the far plane
	// lands in a different slice than the logarithmic rule.
	ClusteredLighting logCtx;
	logCtx.configureClusters(makeConfig(fr, 16, 16, 4));
	const float farDepth = 15.0f;
	PointLight farLight{ Vec3f(0, 0, -farDepth), 0.2f, Vec3f(1, 1, 1) };
	logCtx.buildClusters({ farLight });
	const int logSlice = [&]() {
		for (int k = 0; k < logCtx.sliceCount(); ++k)
			if (!logCtx.cluster(0, 0, k).lights.empty())
				return k;
		return -1;
	}();
	const int linearSlice = static_cast<int>((farDepth - fr.n) / (fr.f - fr.n) * 4);
	TASK_CHECK(logSlice >= 0);
	TASK_CHECK(logSlice != linearSlice);
	return 0;
}

// ===========================================================================
// lighting group
// ===========================================================================

static int lighting_matches_brute_force()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 20, 16 };
	ClusteredLighting ctx;
	buildSimpleGBuffer(ctx, fr, 8.0f, Vec3f(0.6f, 0.7f, 0.8f), Vec3f(0, 0, -1));
	ctx.configureClusters(makeConfig(fr, 5, 5, 5));

	std::vector<PointLight> lights = {
		{ Vec3f(0.0f, 0.0f, -7.0f), 3.0f, Vec3f(2.0f, 1.0f, 0.5f) },
		{ Vec3f(3.0f, -2.0f, -10.0f), 4.0f, Vec3f(0.3f, 0.9f, 0.2f) },
		{ Vec3f(-4.0f, 3.0f, -18.0f), 5.0f, Vec3f(0.5f, 0.2f, 0.9f) },
		{ Vec3f(0.5f, 0.5f, -8.0f), 0.3f, Vec3f(1.0f, 1.0f, 1.0f) }
	};
	ctx.buildClusters(lights);

	const Vec3f ambient(0.1f, 0.15f, 0.2f);
	std::vector<Color3f> image = ctx.shade(lights, ambient);
	TASK_CHECK(image.size() == static_cast<size_t>(fr.w * fr.h));

	for (int y = 0; y < fr.h; ++y)
	{
		for (int x = 0; x < fr.w; ++x)
		{
			const size_t idx = static_cast<size_t>(y) * fr.w + x;
			const GeometryFragment &f = ctx.fragmentAt(x, y);
			const Vec3f expected = referenceLighting(f, lights, ambient);
			const float tol = 1e-4f * std::max(1.0f, std::fabs(expected.x));
			TASK_CHECK(closef(image[idx].r, expected.x, tol));
			TASK_CHECK(closef(image[idx].g, expected.y, tol));
			TASK_CHECK(closef(image[idx].b, expected.z, tol));
		}
	}

	// High intensity must not be clamped away. Place the light beyond the
	// surface (farther from the eye) so the -Z facing normal points toward it.
	std::vector<PointLight> bright = {
		{ Vec3f(0, 0, -11.0f), 5.0f, Vec3f(100.0f, 100.0f, 100.0f) }
	};
	ctx.buildClusters(bright);
	std::vector<Color3f> brightImage = ctx.shade(bright, Vec3f(0, 0, 0));
	bool aboveOne = false;
	for (const Color3f &c : brightImage)
		if (c.r > 1.0f)
			aboveOne = true;
	TASK_CHECK(aboveOne);
	return 0;
}

static int lighting_background_and_edge_cases()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 18, 18 };
	ClusteredLighting ctx;
	ctx.beginGeometryPass(fr.w, fr.h);
	ctx.rasterizer().setScissorRect(fr.vx, fr.vy, fr.w, fr.h);
	VertexProcessor &vp = ctx.vertexProcessor();
	vp.setViewport(fr.vx, fr.vy, fr.w, fr.h);
	vp.setCullMode(CullMode::None);
	// Small central triangle leaves a background border.
	std::vector<GBufferVertex> verts;
	std::vector<int> idx;
	const int base = static_cast<int>(verts.size());
	verts.push_back(ndcVertex(-0.4f, -0.4f, 0.0f, 6.0f, Vec3f(0, 0, -1), Vec3f(0.5f, 0.5f, 0.5f), fr));
	verts.push_back(ndcVertex(0.4f, -0.4f, 0.0f, 6.0f, Vec3f(0, 0, -1), Vec3f(0.5f, 0.5f, 0.5f), fr));
	verts.push_back(ndcVertex(-0.4f, 0.4f, 0.0f, 6.0f, Vec3f(0, 0, -1), Vec3f(0.5f, 0.5f, 0.5f), fr));
	idx.insert(idx.end(), { base, base + 1, base + 2 });
	draw(ctx, verts, idx);
	ctx.endGeometryPass();
	ctx.configureClusters(makeConfig(fr, 6, 6, 4));

	std::vector<PointLight> lights = {
		{ Vec3f(0, 0, -6.0f), 5.0f, Vec3f(1, 1, 1) } // center coincides with surface points
	};
	ctx.buildClusters(lights);
	std::vector<Color3f> image = ctx.shade(lights, Vec3f(0.2f, 0.2f, 0.2f));

	// A corner pixel is background: stays black and counts no light work.
	TASK_CHECK(!ctx.isValid(1, 1));
	TASK_CHECK(closef(image[1 * fr.w + 1].r, 0.0f) &&
		closef(image[1 * fr.w + 1].g, 0.0f) &&
		closef(image[1 * fr.w + 1].b, 0.0f));

	// Ambient still applies on valid pixels even though the point light is at
	// |V| = 0 for the center and contributes nothing there.
	const GeometryFragment &center = ctx.fragmentAt(fr.w / 2, fr.h / 2);
	const Vec3f expectedCenter = referenceLighting(center, lights, Vec3f(0.2f, 0.2f, 0.2f));
	const Color3f &got = image[(fr.h / 2) * fr.w + fr.w / 2];
	TASK_CHECK(closef(got.r, expectedCenter.x, 1e-5f));
	TASK_CHECK(closef(got.r, 0.1f, 1e-5f)); // ambient * albedo only

	// A light beyond its radius and a back-facing normal both add nothing.
	GeometryFragment probe;
	probe.valid = true;
	probe.position = Vec3f(0, 0, -10);
	probe.normal = normalize(Vec3f(0, 0, -1));
	probe.albedo = Vec3f(1, 1, 1);
	probe.depth = 0.5f;
	std::vector<PointLight> edge = {
		{ Vec3f(0, 0, -5.0f), 4.0f, Vec3f(1, 1, 1) } // distance 5 > radius 4
	};
	const Vec3f edgeColor = referenceLighting(probe, edge, Vec3f(0, 0, 0));
	TASK_CHECK(closef(edgeColor.x, 0.0f) && closef(edgeColor.y, 0.0f) && closef(edgeColor.z, 0.0f));
	return 0;
}

static int lighting_calculation_statistics()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 20, 16 };
	ClusteredLighting ctx;
	buildSimpleGBuffer(ctx, fr, 9.0f, Vec3f(0.7f, 0.7f, 0.7f), Vec3f(0, 0, -1));
	ctx.configureClusters(makeConfig(fr, 5, 5, 4));

	std::vector<PointLight> lights = {
		{ Vec3f(0, 0, -8.5f), 1.0f, Vec3f(1, 1, 1) },
		{ Vec3f(4, 4, -9.0f), 1.0f, Vec3f(1, 1, 1) },
		{ Vec3f(-4, -4, -9.0f), 1.0f, Vec3f(1, 1, 1) }
	};
	ctx.buildClusters(lights);
	ctx.shade(lights, Vec3f(0.1f, 0.1f, 0.1f));

	// Independently sum the per-pixel cluster list sizes.
	size_t expected = 0;
	size_t validPixels = 0;
	for (int y = 0; y < fr.h; ++y)
	{
		for (int x = 0; x < fr.w; ++x)
		{
			if (!ctx.isValid(x, y))
				continue;
			++validPixels;
			const GeometryFragment &f = ctx.fragmentAt(x, y);
			const int ci = ctx.clusterIndexForPixel(x, y, -f.position.z);
			TASK_CHECK(ci >= 0);
			expected += ctx.cluster(
				(ci / 4) % ctx.tileCountX(),
				ci / (4 * ctx.tileCountX()),
				ci % 4).lights.size();
		}
	}
	TASK_CHECK(validPixels == fr.w * fr.h);
	TASK_CHECK(ctx.lightCalculationCount() == expected);
	TASK_CHECK(ctx.lightCalculationCount() < validPixels * lights.size());
	return 0;
}

static int lighting_independent_image_and_reset()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 30.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	buildSimpleGBuffer(ctx, fr, 7.0f, Vec3f(0.5f, 0.6f, 0.7f), Vec3f(0, 0, -1));
	ctx.configureClusters(makeConfig(fr, 8, 8, 4));
	std::vector<PointLight> lights = {
		{ Vec3f(0, 0, -6.0f), 3.0f, Vec3f(1, 1, 1) }
	};
	ctx.buildClusters(lights);

	std::vector<Color3f> first = ctx.shade(lights, Vec3f(0.1f, 0.1f, 0.1f));
	std::vector<Color3f> second = ctx.shade(lights, Vec3f(0.1f, 0.1f, 0.1f));
	TASK_CHECK(first.size() == second.size());
	for (size_t i = 0; i < first.size(); ++i)
		TASK_CHECK(closef(first[i].r, second[i].r) &&
			closef(first[i].g, second[i].g) &&
			closef(first[i].b, second[i].b));

	// Mutating a returned image cannot corrupt the next result.
	first[0].r = 999.0f;
	TASK_CHECK(std::fabs(second[0].r - 999.0f) > 1.0f);

	// A changed light table is rejected rather than reading stale lists.
	std::vector<PointLight> changed = lights;
	changed.push_back(PointLight{ Vec3f(1, 1, -7), 1.0f, Vec3f(1, 1, 1) });
	bool threw = false;
	try
	{
		ctx.shade(changed, Vec3f(0.1f, 0.1f, 0.1f));
	}
	catch (const std::logic_error &)
	{
		threw = true;
	}
	TASK_CHECK(threw);

	// Clearing and rebuilding with a new table leaves no previous lights.
	ctx.buildClusters(changed);
	std::vector<Color3f> rebuilt = ctx.shade(changed, Vec3f(0, 0, 0));
	TASK_CHECK(rebuilt.size() == first.size());
	return 0;
}

static int lighting_degenerate_control_differs()
{
	Frustum fr{ -1.0f, 1.0f, -1.0f, 1.0f, 2.0f, 30.0f, 0, 0, 16, 16 };
	ClusteredLighting ctx;
	// Eye-facing normal so a light between the eye and the surface lights it.
	buildSimpleGBuffer(ctx, fr, 6.0f, Vec3f(0.8f, 0.8f, 0.8f), Vec3f(0, 0, 1));
	ctx.configureClusters(makeConfig(fr, 8, 8, 5));

	// Near-crossing light: its center sits behind the near plane, so the
	// center-only control assigns it nowhere while the correct path lights
	// the surface. Radius must bridge from d = 1 past the surface at d = 6.
	PointLight light{ Vec3f(0.0f, 0.0f, -1.0f), 6.5f, Vec3f(3.0f, 3.0f, 3.0f) };
	std::vector<PointLight> lights = { light };
	ctx.buildClusters(lights);
	std::vector<Color3f> correct = ctx.shade(lights, Vec3f(0, 0, 0));

	std::vector<Cluster> copy;
	for (int ty = 0; ty < ctx.tileCountY(); ++ty)
		for (int tx = 0; tx < ctx.tileCountX(); ++tx)
			for (int k = 0; k < ctx.sliceCount(); ++k)
				copy.push_back(ctx.cluster(tx, ty, k));
	std::vector<std::vector<int>> degenerate =
		centerOnlyAssignment(ctx.clusterConfig(), copy, lights);

	// Manual shading driven by the degenerate lists.
	Vec3f degenerateSum(0, 0, 0);
	Vec3f correctSum(0, 0, 0);
	for (int y = 0; y < fr.h; ++y)
	{
		for (int x = 0; x < fr.w; ++x)
		{
			const GeometryFragment &f = ctx.fragmentAt(x, y);
			correctSum += Vec3f(correct[y * fr.w + x].r, correct[y * fr.w + x].g, correct[y * fr.w + x].b);

			const int ci = ctx.clusterIndexForPixel(x, y, -f.position.z);
			Vec3f color(0, 0, 0);
			for (int li : degenerate[ci])
			{
				const PointLight &pl = lights[li];
				const Vec3f V = pl.position - f.position;
				const float dist2 = dot(V, V);
				const float r2 = pl.radius * pl.radius;
				if (!(dist2 > 0.0f && dist2 < r2))
					continue;
				const float nDotL = std::max(0.0f, dot(f.normal, V * (1.0f / std::sqrt(dist2))));
				const float falloff = 1.0f - dist2 / r2;
				color += f.albedo * (pl.intensity * (nDotL * falloff * falloff / std::max(dist2, 1e-6f)));
			}
			degenerateSum += color;
		}
	}
	TASK_CHECK(correctSum.x > 0.0f);
	TASK_CHECK(degenerateSum.x == 0.0f);
	TASK_CHECK(std::fabs(correctSum.x - degenerateSum.x) > 1e-4f);
	return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

extern const struct task_case task_cases[] = {
	{ "capture", "basic_geometry", &capture_basic_geometry },
	{ "capture", "depth_occlusion_and_tie", &capture_depth_occlusion_and_tie },
	{ "capture", "discard", &capture_discard },
	{ "capture", "three_raster_modes_match", &capture_three_raster_modes_match },
	{ "capture", "resize_and_clear", &capture_resize_and_clear },
	{ "capture", "perspective_correct_interpolation", &capture_perspective_correct_interpolation },

	{ "clusters", "grid_layout_and_depth_boundaries", &clusters_grid_layout_and_depth_boundaries },
	{ "clusters", "exhaustive_symmetric", &clusters_exhaustive_symmetric },
	{ "clusters", "asymmetric_frustum_origin", &clusters_asymmetric_frustum_origin },
	{ "clusters", "side_tangency_kept", &clusters_side_tangency_kept },
	{ "clusters", "near_plane_crossing", &clusters_near_plane_crossing },
	{ "clusters", "eye_plane_crossing", &clusters_eye_plane_crossing },
	{ "clusters", "layer_boundary_lights", &clusters_layer_boundary_lights },
	{ "clusters", "many_overlapping_lights_no_drop", &clusters_many_overlapping_lights_no_drop },
	{ "clusters", "stats_are_real", &clusters_stats_are_real },
	{ "clusters", "degenerate_controls_differ", &clusters_degenerate_controls_differ },

	{ "lighting", "matches_brute_force", &lighting_matches_brute_force },
	{ "lighting", "background_and_edge_cases", &lighting_background_and_edge_cases },
	{ "lighting", "calculation_statistics", &lighting_calculation_statistics },
	{ "lighting", "independent_image_and_reset", &lighting_independent_image_and_reset },
	{ "lighting", "degenerate_control_differs", &lighting_degenerate_control_differs }
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
