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

#include "ClusteredLighting.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace swr {

// ---------------------------------------------------------------------------
// Geometry buffer pixel shader binding
// ---------------------------------------------------------------------------

class ClusteredLighting *GBufferPixelShader::activeContext = nullptr;

void GBufferPixelShader::drawPixel(const PixelData &p)
{
	if (activeContext != nullptr)
		activeContext->writeFragment(p);
}

// ---------------------------------------------------------------------------
// Construction / reset
// ---------------------------------------------------------------------------

ClusteredLighting::ClusteredLighting()
	: m_vertexProcessor(&m_rasterizer)
	, m_width(0)
	, m_height(0)
	, m_configured(false)
	, m_tilesX(0)
	, m_tilesY(0)
	, m_slices(0)
	, m_sphereChecks(0)
	, m_lightCountAtBuild(0)
	, m_lightCalculations(0)
{
}

ClusteredLighting::~ClusteredLighting()
{
	if (GBufferPixelShader::activeContext == this)
		GBufferPixelShader::activeContext = nullptr;
}

void ClusteredLighting::clear()
{
	m_discard = nullptr;
	m_width = 0;
	m_height = 0;
	m_fragments.clear();
	m_depthBuffer.clear();

	m_configured = false;
	m_tilesX = m_tilesY = m_slices = 0;
	m_clusters.clear();
	m_depthBoundaries.clear();
	m_sphereChecks = 0;

	m_lightCountAtBuild = 0;
	m_lightCalculations = 0;

	if (GBufferPixelShader::activeContext == this)
		GBufferPixelShader::activeContext = nullptr;
}

// ---------------------------------------------------------------------------
// Geometry pass
// ---------------------------------------------------------------------------

void ClusteredLighting::resizeFramebuffer(int width, int height)
{
	m_width = width;
	m_height = height;
	m_fragments.assign(static_cast<size_t>(width) * height, GeometryFragment());
	m_depthBuffer.assign(static_cast<size_t>(width) * height, std::numeric_limits<float>::infinity());
}

void ClusteredLighting::clearFramebuffer()
{
	for (GeometryFragment &f : m_fragments)
	{
		f = GeometryFragment();
		f.valid = false;
		f.depth = std::numeric_limits<float>::infinity();
	}
	std::fill(m_depthBuffer.begin(), m_depthBuffer.end(), std::numeric_limits<float>::infinity());
}

void ClusteredLighting::beginGeometryPass(int width, int height)
{
	if (width <= 0 || height <= 0)
		throw std::invalid_argument("ClusteredLighting: framebuffer dimensions must be positive");

	if (m_width != width || m_height != height)
		resizeFramebuffer(width, height);
	clearFramebuffer();

	m_rasterizer.setPixelShader<GBufferPixelShader>();
	GBufferPixelShader::activeContext = this;
}

void ClusteredLighting::endGeometryPass()
{
	if (GBufferPixelShader::activeContext == this)
		GBufferPixelShader::activeContext = nullptr;
}

void ClusteredLighting::setDiscardCallback(const DiscardCallback &callback)
{
	m_discard = callback;
}

void ClusteredLighting::writeFragment(const PixelData &p)
{
	if (p.x < 0 || p.y < 0 || p.x >= m_width || p.y >= m_height)
		return;

	GeometryFragment fragment;
	fragment.position = Vec3f(p.pvar[0], p.pvar[1], p.pvar[2]);
	fragment.normal = normalize(Vec3f(p.pvar[3], p.pvar[4], p.pvar[5]));
	fragment.albedo = Vec3f(p.pvar[6], p.pvar[7], p.pvar[8]);
	fragment.depth = p.z;
	fragment.valid = true;

	// The discard test runs before the depth test; a discarded fragment
	// contributes neither geometry nor depth.
	if (m_discard && m_discard(fragment))
		return;

	const size_t index = static_cast<size_t>(p.y) * m_width + p.x;

	// Window depth [0, 1], smaller wins; on equality the later fragment wins.
	if (p.z <= m_depthBuffer[index])
	{
		m_depthBuffer[index] = p.z;
		m_fragments[index] = fragment;
	}
}

bool ClusteredLighting::isValid(int x, int y) const
{
	if (x < 0 || y < 0 || x >= m_width || y >= m_height)
		return false;
	return m_fragments[static_cast<size_t>(y) * m_width + x].valid;
}

const GeometryFragment& ClusteredLighting::fragmentAt(int x, int y) const
{
	assert(x >= 0 && y >= 0 && x < m_width && y < m_height);
	return m_fragments[static_cast<size_t>(y) * m_width + x];
}

// ---------------------------------------------------------------------------
// Cluster configuration
// ---------------------------------------------------------------------------

void ClusteredLighting::configureClusters(const ClusterConfig &config)
{
	if (!(config.nearPlane > 0.0f && config.farPlane > config.nearPlane))
		throw std::invalid_argument("ClusteredLighting: require 0 < near < far");
	if (!(config.left < config.right && config.bottom < config.top))
		throw std::invalid_argument("ClusteredLighting: invalid near plane rectangle");
	if (config.viewportWidth <= 0 || config.viewportHeight <= 0)
		throw std::invalid_argument("ClusteredLighting: viewport dimensions must be positive");
	if (config.tileWidth <= 0 || config.tileHeight <= 0)
		throw std::invalid_argument("ClusteredLighting: tile dimensions must be positive");
	if (config.depthSlices <= 0)
		throw std::invalid_argument("ClusteredLighting: depthSlices must be positive");

	m_config = config;
	m_configured = true;

	m_tilesX = (config.viewportWidth + config.tileWidth - 1) / config.tileWidth;
	m_tilesY = (config.viewportHeight + config.tileHeight - 1) / config.tileHeight;
	m_slices = config.depthSlices;

	// Logarithmic depth boundaries d_k = near * (far/near)^(k/K).
	m_depthBoundaries.resize(m_slices + 1);
	const float ratio = config.farPlane / config.nearPlane;
	const float logRatio = std::log(ratio);
	for (int k = 0; k <= m_slices; ++k)
		m_depthBoundaries[k] = config.nearPlane * std::exp(logRatio * (static_cast<float>(k) / m_slices));
	// Pin the endpoints against rounding.
	m_depthBoundaries[0] = config.nearPlane;
	m_depthBoundaries[m_slices] = config.farPlane;

	buildClusterPlanes();
}

namespace {

ClusterPlane makePlane(const Vec3f &normal, float constant)
{
	ClusterPlane plane;
	const float invLength = 1.0f / length(normal);
	plane.normal = normal * invLength;
	plane.constant = constant * invLength;
	return plane;
}

} // unnamed namespace

void ClusteredLighting::buildClusterPlanes()
{
	const ClusterConfig &cfg = m_config;
	const float n = cfg.nearPlane;

	m_clusters.clear();
	m_clusters.reserve(clusterCount());

	const float slopeScaleX = (cfg.right - cfg.left) / (2.0f * n);
	const float slopeOffsetX = (cfg.right + cfg.left) / (cfg.right - cfg.left);
	const float slopeScaleY = (cfg.top - cfg.bottom) / (2.0f * n);
	const float slopeOffsetY = (cfg.top + cfg.bottom) / (cfg.top - cfg.bottom);

	// Pixel coordinate -> view space slope s = coord / (-z) on the near plane.
	auto slopeX = [&](int pixelX) {
		const float ndc = 2.0f * (pixelX - cfg.viewportX) / cfg.viewportWidth - 1.0f;
		return (ndc + slopeOffsetX) * slopeScaleX;
	};
	auto slopeY = [&](int pixelY) {
		const float ndc = 1.0f - 2.0f * (pixelY - cfg.viewportY) / cfg.viewportHeight;
		return (ndc + slopeOffsetY) * slopeScaleY;
	};

	for (int ty = 0; ty < m_tilesY; ++ty)
	{
		const int minY = cfg.viewportY + ty * cfg.tileHeight;
		const int maxY = std::min(minY + cfg.tileHeight, cfg.viewportY + cfg.viewportHeight);

		// Larger view y slope maps to the smaller pixel row.
		const float slopeTop = slopeY(minY);
		const float slopeBottom = slopeY(maxY);

		for (int tx = 0; tx < m_tilesX; ++tx)
		{
			const int minX = cfg.viewportX + tx * cfg.tileWidth;
			const int maxX = std::min(minX + cfg.tileWidth, cfg.viewportX + cfg.viewportWidth);

			const float slopeLeft = slopeX(minX);
			const float slopeRight = slopeX(maxX);

			for (int k = 0; k < m_slices; ++k)
			{
				Cluster cluster;
				cluster.tileX = tx;
				cluster.tileY = ty;
				cluster.slice = k;
				cluster.minPixelX = minX;
				cluster.minPixelY = minY;
				cluster.maxPixelX = maxX;
				cluster.maxPixelY = maxY;
				cluster.depthNear = m_depthBoundaries[k];
				cluster.depthFar = m_depthBoundaries[k + 1];

				// Four side planes through the camera origin. The interior of
				// a slab is slopeLeft <= x/(-z) < slopeRight and likewise for y.
				cluster.planes[0] = makePlane(Vec3f(1.0f, 0.0f, slopeLeft), 0.0f);    // left
				cluster.planes[1] = makePlane(Vec3f(-1.0f, 0.0f, -slopeRight), 0.0f); // right
				cluster.planes[2] = makePlane(Vec3f(0.0f, 1.0f, slopeBottom), 0.0f);  // bottom
				cluster.planes[3] = makePlane(Vec3f(0.0f, -1.0f, -slopeTop), 0.0f);  // top

				// Depth planes: near requires -z >= dNear, far requires -z <= dFar.
				cluster.planes[4] = makePlane(Vec3f(0.0f, 0.0f, -1.0f), -cluster.depthNear);
				cluster.planes[5] = makePlane(Vec3f(0.0f, 0.0f, 1.0f), cluster.depthFar);

				m_clusters.push_back(std::move(cluster));
			}
		}
	}
}

const Cluster& ClusteredLighting::cluster(int tileX, int tileY, int slice) const
{
	assert(m_configured);
	assert(tileX >= 0 && tileX < m_tilesX);
	assert(tileY >= 0 && tileY < m_tilesY);
	assert(slice >= 0 && slice < m_slices);
	const size_t index = (static_cast<size_t>(tileY) * m_tilesX + tileX) * m_slices + slice;
	return m_clusters[index];
}

int ClusteredLighting::sliceForDepth(float viewDepth) const
{
	if (viewDepth >= m_config.farPlane)
		return m_slices - 1; // far belongs to the last layer
	if (viewDepth <= m_config.nearPlane)
		return 0;

	const float t = std::log(viewDepth / m_config.nearPlane) /
		std::log(m_config.farPlane / m_config.nearPlane);
	int k = static_cast<int>(t * m_slices);
	// Internal boundaries belong to the later cluster; floor already gives
	// that, only clamp against float drift.
	if (k < 0)
		k = 0;
	if (k >= m_slices)
		k = m_slices - 1;
	return k;
}

int ClusteredLighting::clusterIndexForPixel(int x, int y, float viewDepth) const
{
	assert(m_configured);
	const ClusterConfig &cfg = m_config;
	const int localX = x - cfg.viewportX;
	const int localY = y - cfg.viewportY;
	if (localX < 0 || localY < 0 ||
		localX >= cfg.viewportWidth || localY >= cfg.viewportHeight)
		return -1;

	const int tx = std::min(localX / cfg.tileWidth, m_tilesX - 1);
	const int ty = std::min(localY / cfg.tileHeight, m_tilesY - 1);
	const int k = sliceForDepth(viewDepth);
	return (ty * m_tilesX + tx) * m_slices + k;
}

// ---------------------------------------------------------------------------
// Light assignment
// ---------------------------------------------------------------------------

namespace {

// Sphere vs closed half space. The interior is plane.normal . p + constant
// >= 0; the sphere reaches it when the closest sphere point is still inside
// or on the boundary, so tangency is kept.
bool sphereIntersectsHalfSpace(const ClusterPlane &plane, const Vec3f &center, float radius)
{
	return dot(plane.normal, center) + plane.constant + radius >= 0.0f;
}

bool sphereIntersectsCluster(const Cluster &cluster, const Vec3f &center, float radius)
{
	for (const ClusterPlane &plane : cluster.planes)
	{
		if (!sphereIntersectsHalfSpace(plane, center, radius))
			return false;
	}
	return true;
}

} // unnamed namespace

void ClusteredLighting::buildClusters(const std::vector<PointLight> &lights)
{
	if (!m_configured)
		throw std::logic_error("ClusteredLighting: configureClusters() before buildClusters()");

	for (Cluster &cluster : m_clusters)
		cluster.lights.clear();

	m_sphereChecks = 0;
	m_lightCountAtBuild = lights.size();

	const ClusterConfig &cfg = m_config;
	const float n = cfg.nearPlane;
	const float f = cfg.farPlane;

	for (size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
	{
		const PointLight &light = lights[lightIndex];
		if (!(light.radius > 0.0f))
			throw std::invalid_argument("ClusteredLighting: light radius must be positive");
		if (light.intensity.x < 0.0f || light.intensity.y < 0.0f || light.intensity.z < 0.0f)
			throw std::invalid_argument("ClusteredLighting: light intensity must be non negative");

		const Vec3f &C = light.position;
		const float r = light.radius;
		const float centerDepth = -C.z; // eye space depth d = -z

		// --- Conservative depth layer range -------------------------------
		const float depthLo = std::max(n, centerDepth - r);
		const float depthHi = std::min(f, centerDepth + r);
		if (depthHi < n || depthLo > f)
			continue; // the sphere cannot reach the frustum depth range

		int sliceFirst = sliceForDepth(depthLo);
		int sliceLast = sliceForDepth(depthHi);
		if (sliceLast < sliceFirst)
			std::swap(sliceFirst, sliceLast);

		// A sphere extent that lands exactly on an internal boundary is mapped
		// to the later slice, but the closed half space rule also keeps the
		// tangent neighbor slice. Widen by one on each side and let the exact
		// six plane test reject the ones that do not actually intersect.
		sliceFirst = std::max(0, sliceFirst - 1);
		sliceLast = std::min(m_slices - 1, sliceLast + 1);

		// --- Conservative screen tile range -------------------------------
		// Every side plane passes through the camera origin, and the column
		// left/right planes (and row bottom/top planes) are strictly ordered
		// across the screen. Test the sphere against each column's two x
		// planes and each row's two y planes; the intersecting columns/rows
		// form one contiguous range. This is exact even for a sphere whose
		// center is behind the eye, needs no cone projection, and never
		// rejects a cluster that passes the full six plane test.
		auto sideHit = [&](const ClusterPlane &plane) {
			return dot(plane.normal, C) + plane.constant + r >= 0.0f;
		};

		int tileFirstX = m_tilesX - 1;
		int tileLastX = 0;
		bool anyColumn = false;
		for (int tx = 0; tx < m_tilesX; ++tx)
		{
			// Slice 0 cluster of this column; its x planes are column wide.
			const Cluster &probe = m_clusters[static_cast<size_t>(tx) * m_slices];
			if (sideHit(probe.planes[0]) && sideHit(probe.planes[1]))
			{
				if (!anyColumn) { tileFirstX = tx; anyColumn = true; }
				tileLastX = tx;
			}
		}

		int tileFirstY = m_tilesY - 1;
		int tileLastY = 0;
		bool anyRow = false;
		for (int ty = 0; ty < m_tilesY; ++ty)
		{
			// Slice 0 cluster at (ty, 0); its y planes are row wide.
			const Cluster &probe =
				m_clusters[(static_cast<size_t>(ty) * m_tilesX) * m_slices];
			if (sideHit(probe.planes[2]) && sideHit(probe.planes[3]))
			{
				if (!anyRow) { tileFirstY = ty; anyRow = true; }
				tileLastY = ty;
			}
		}

		if (!anyColumn || !anyRow)
			continue; // no screen column/row is reached, hence no cluster

		// --- Six plane tests, only inside the conservative ranges ---------
		for (int ty = tileFirstY; ty <= tileLastY; ++ty)
		{
			for (int tx = tileFirstX; tx <= tileLastX; ++tx)
			{
				for (int k = sliceFirst; k <= sliceLast; ++k)
				{
					const size_t index = (static_cast<size_t>(ty) * m_tilesX + tx) * m_slices + k;
					++m_sphereChecks;
					Cluster &cluster = m_clusters[index];
					if (sphereIntersectsCluster(cluster, C, r))
						cluster.lights.push_back(static_cast<int>(lightIndex));
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

std::vector<Color3f> ClusteredLighting::shade(const std::vector<PointLight> &lights, const Vec3f &ambient)
{
	if (!m_configured)
		throw std::logic_error("ClusteredLighting: configureClusters() before shade()");
	if (lights.size() != m_lightCountAtBuild)
		throw std::logic_error("ClusteredLighting: light table changed since buildClusters()");

	std::vector<Color3f> image(static_cast<size_t>(m_width) * m_height, Color3f());
	m_lightCalculations = 0;

	for (int y = 0; y < m_height; ++y)
	{
		for (int x = 0; x < m_width; ++x)
		{
			const size_t pixelIndex = static_cast<size_t>(y) * m_width + x;
			const GeometryFragment &fragment = m_fragments[pixelIndex];
			if (!fragment.valid)
				continue; // background stays black and counts nothing

			const int clusterIndex = clusterIndexForPixel(x, y, -fragment.position.z);
			if (clusterIndex < 0)
				continue;

			const Cluster &cluster = m_clusters[clusterIndex];
			const Vec3f &P = fragment.position;
			const Vec3f &N = fragment.normal;

			Vec3f color = ambient * fragment.albedo;

			for (int lightIndex : cluster.lights)
			{
				++m_lightCalculations;
				const PointLight &light = lights[lightIndex];

				const Vec3f V = light.position - P;
				const float dist2 = dot(V, V);
				const float r2 = light.radius * light.radius;
				if (!(dist2 > 0.0f && dist2 < r2))
					continue;

				const float invDist = 1.0f / std::sqrt(dist2);
				const float nDotL = std::max(0.0f, dot(N, V * invDist));
				const float falloff = 1.0f - dist2 / r2;
				const float attenuation = falloff * falloff / std::max(dist2, 1e-6f);

				const Vec3f radiance = light.intensity * (nDotL * attenuation);
				color += fragment.albedo * radiance;
			}

			image[pixelIndex] = Color3f(color.x, color.y, color.z);
		}
	}

	return image;
}

} // end namespace swr
