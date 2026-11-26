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
#include <cmath>
#include <limits>

namespace swr {

namespace {

const float kPlaneEpsilon = 1e-5f;

void normalize3(float v[3])
{
	float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (len > 0.0f) {
		float inv = 1.0f / len;
		v[0] *= inv;
		v[1] *= inv;
		v[2] *= inv;
	}
}

int clampInt(int v, int lo, int hi)
{
	return std::max(lo, std::min(v, hi));
}

} // namespace

void projectPerspective(const ClusterConfig &config, const float viewPosition[3], float clip[4])
{
	const float d = -viewPosition[2];
	const float n = config.nearPlane;
	const float f = config.farPlane;
	const float rl = config.right - config.left;
	const float tb = config.top - config.bottom;

	clip[0] = (2.0f * n * viewPosition[0] + (config.right + config.left) * viewPosition[2]) / rl;
	clip[1] = (2.0f * n * viewPosition[1] + (config.top + config.bottom) * viewPosition[2]) / tb;
	clip[2] = ((f + n) * d - 2.0f * f * n) / (f - n);
	clip[3] = d;
}

void GBufferVertexShader::processVertex(VertexShaderInput in, VertexShaderOutput *out)
{
	const GBufferVertex &v = *static_cast<const GBufferVertex *>(in[0]);
	out->x = v.clip[0];
	out->y = v.clip[1];
	out->z = v.clip[2];
	out->w = v.clip[3];
	out->pvar[0] = v.position[0];
	out->pvar[1] = v.position[1];
	out->pvar[2] = v.position[2];
	out->pvar[3] = v.normal[0];
	out->pvar[4] = v.normal[1];
	out->pvar[5] = v.normal[2];
	out->pvar[6] = v.albedo[0];
	out->pvar[7] = v.albedo[1];
	out->pvar[8] = v.albedo[2];
}

ClusteredLighting *GBufferPixelShader::activeTarget = nullptr;

void GBufferPixelShader::drawPixel(const PixelData &p)
{
	if (activeTarget == nullptr)
		return;

	float viewPosition[3] = {p.pvar[0], p.pvar[1], p.pvar[2]};
	float normal[3] = {p.pvar[3], p.pvar[4], p.pvar[5]};
	float albedo[3] = {p.pvar[6], p.pvar[7], p.pvar[8]};

	float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
	if (len > 0.0f) {
		float inv = 1.0f / len;
		normal[0] *= inv;
		normal[1] *= inv;
		normal[2] *= inv;
	}

	activeTarget->submitFragment(p.x, p.y, p.z, viewPosition, normal, albedo);
}

ClusteredLighting::ClusteredLighting()
	: m_vertexProcessor(&m_rasterizer)
{
}

ClusteredLighting::ClusteredLighting(const ClusterConfig &config)
	: m_vertexProcessor(&m_rasterizer)
{
	this->configure(config);
}

void ClusteredLighting::configure(const ClusterConfig &config)
{
	m_config = config;
	m_lights.clear();
	m_stats = ClusterBuildStats();
	GBufferPixelShader::activeTarget = nullptr;
	m_discardFunction = nullptr;
	m_ambient = {{0.0f, 0.0f, 0.0f}};

	m_width = config.viewportWidth;
	m_height = config.viewportHeight;
	m_originX = config.viewportX;
	m_originY = config.viewportY;

	allocateBuffers();
	buildGrid();
	clear();
}

void ClusteredLighting::setAmbient(float r, float g, float b)
{
	m_ambient = {{r, g, b}};
}

void ClusteredLighting::allocateBuffers()
{
	const size_t pixels = (size_t)m_width * (size_t)m_height;
	m_valid.assign(pixels, 0);
	m_depth.assign(pixels, std::numeric_limits<float>::infinity());
	m_positions.assign(pixels, {{0.0f, 0.0f, 0.0f}});
	m_normals.assign(pixels, {{0.0f, 0.0f, 0.0f}});
	m_albedos.assign(pixels, {{0.0f, 0.0f, 0.0f}});
}

void ClusteredLighting::clear()
{
	std::fill(m_valid.begin(), m_valid.end(), 0);
	std::fill(m_depth.begin(), m_depth.end(), std::numeric_limits<float>::infinity());
}

void ClusteredLighting::beginGeometry()
{
	m_rasterizer.setRasterMode(m_rasterMode);
	m_rasterizer.setScissorRect(m_originX, m_originY, m_width, m_height);
	m_rasterizer.setPixelShader<GBufferPixelShader>();
	m_vertexProcessor.setViewport(m_originX, m_originY, m_width, m_height);
	m_vertexProcessor.setDepthRange(0.0f, 1.0f);
	m_vertexProcessor.setCullMode(CullMode::None);
	m_vertexProcessor.setVertexShader<GBufferVertexShader>();
	GBufferPixelShader::activeTarget = this;
}

void ClusteredLighting::drawElements(DrawMode mode, size_t count, int *indices)
{
	// Re-assert this context as the render target so interleaved draws on two
	// contexts never funnel fragments into the other context's G-buffer.
	GBufferPixelShader::activeTarget = this;
	m_vertexProcessor.drawElements(mode, count, indices);
}

bool ClusteredLighting::submitFragment(
	int x,
	int y,
	float windowDepth,
	const float viewPosition[3],
	const float normalizedNormal[3],
	const float albedo[3])
{
	const int lx = x - m_originX;
	const int ly = y - m_originY;
	if (lx < 0 || lx >= m_width || ly < 0 || ly >= m_height)
		return false;

	if (m_discardFunction
		&& m_discardFunction(x, y, viewPosition, normalizedNormal, albedo, windowDepth))
		return false;

	const size_t index = (size_t)ly * (size_t)m_width + (size_t)lx;

	// Smaller window depth wins; on exact ties the later fragment wins.
	if (windowDepth > m_depth[index])
		return false;

	m_depth[index] = windowDepth;
	m_valid[index] = 1;
	m_positions[index] = {{viewPosition[0], viewPosition[1], viewPosition[2]}};
	m_normals[index] = {{normalizedNormal[0], normalizedNormal[1], normalizedNormal[2]}};
	m_albedos[index] = {{albedo[0], albedo[1], albedo[2]}};
	return true;
}

void ClusteredLighting::setLights(const std::vector<PointLight> &lights)
{
	m_lights = lights;
	for (Cluster &cluster : m_clusters)
		cluster.lights.clear();
	m_stats = ClusterBuildStats();
}

void ClusteredLighting::setLights(std::vector<PointLight> &&lights)
{
	m_lights = std::move(lights);
	for (Cluster &cluster : m_clusters)
		cluster.lights.clear();
	m_stats = ClusterBuildStats();
}

void ClusteredLighting::buildGrid()
{
	const ClusterConfig &c = m_config;

	m_grid.tilesX = (m_width + c.tileWidth - 1) / c.tileWidth;
	m_grid.tilesY = (m_height + c.tileHeight - 1) / c.tileHeight;
	m_grid.layers = c.depthLayers;

	m_grid.depthBoundaries.resize((size_t)c.depthLayers + 1);
	const float ratio = c.farPlane / c.nearPlane;
	for (int k = 0; k <= c.depthLayers; ++k) {
		m_grid.depthBoundaries[k] =
			c.nearPlane * std::pow(ratio, (float)k / (float)c.depthLayers);
	}

	m_clusters.clear();
	m_clusters.reserve((size_t)m_grid.tilesX * m_grid.tilesY * m_grid.layers);

	const float invW = 1.0f / (float)m_width;
	const float invH = 1.0f / (float)m_height;

	for (int ty = 0; ty < m_grid.tilesY; ++ty) {
		const int py0 = m_originY + ty * c.tileHeight;
		const int py1 = std::min(m_originY + (ty + 1) * c.tileHeight, m_originY + m_height);
		const float fy0 = (float)(py0 - m_originY) * invH;
		const float fy1 = (float)(py1 - m_originY) * invH;
		const float nearB = c.bottom + (c.top - c.bottom) * fy0;
		const float nearT = c.bottom + (c.top - c.bottom) * fy1;

		for (int tx = 0; tx < m_grid.tilesX; ++tx) {
			const int px0 = m_originX + tx * c.tileWidth;
			const int px1 = std::min(m_originX + (tx + 1) * c.tileWidth, m_originX + m_width);
			const float fx0 = (float)(px0 - m_originX) * invW;
			const float fx1 = (float)(px1 - m_originX) * invW;
			const float nearL = c.left + (c.right - c.left) * fx0;
			const float nearR = c.left + (c.right - c.left) * fx1;

			for (int k = 0; k < m_grid.layers; ++k) {
				Cluster cluster;
				cluster.tileX = tx;
				cluster.tileY = ty;
				cluster.layer = k;
				cluster.pixelX0 = px0;
				cluster.pixelX1 = px1;
				cluster.pixelY0 = py0;
				cluster.pixelY1 = py1;

				const float n = c.nearPlane;

				// Side planes through the camera origin. Interior is n . p >= 0.
				cluster.planes[0] = {{1.0f, 0.0f, nearL / n}, 0.0f};       // left
				cluster.planes[1] = {{-1.0f, 0.0f, -nearR / n}, 0.0f};      // right
				cluster.planes[2] = {{0.0f, 1.0f, nearB / n}, 0.0f};        // bottom
				cluster.planes[3] = {{0.0f, -1.0f, -nearT / n}, 0.0f};      // top

				const float d0 = m_grid.depthBoundaries[k];
				const float d1 = m_grid.depthBoundaries[k + 1];
				cluster.planes[4] = {{0.0f, 0.0f, -1.0f}, -d0};             // d >= d0
				cluster.planes[5] = {{0.0f, 0.0f, 1.0f}, d1};               // d <= d1

				for (ClusterPlane &plane : cluster.planes)
					normalize3(plane.normal);

				m_clusters.push_back(std::move(cluster));
			}
		}
	}
}

size_t ClusteredLighting::clusterIndex(int tileX, int tileY, int layer) const
{
	return ((size_t)tileY * (size_t)m_grid.tilesX + (size_t)tileX) * (size_t)m_grid.layers
		+ (size_t)layer;
}

int ClusteredLighting::tileForPixel(int pixel, int origin, int tileSize, int tileCount) const
{
	return clampInt((pixel - origin) / tileSize, 0, tileCount - 1);
}

int ClusteredLighting::depthLayer(float viewDepth) const
{
	const ClusterConfig &c = m_config;
	if (viewDepth <= c.nearPlane)
		return 0;
	if (viewDepth >= c.farPlane)
		return c.depthLayers - 1;

	const float f = std::log(viewDepth / c.nearPlane) / std::log(c.farPlane / c.nearPlane);
	const int k = (int)std::floor((float)c.depthLayers * f);
	return clampInt(k, 0, c.depthLayers - 1);
}

ClusteredLighting::ScreenRange ClusteredLighting::conservativeRange(const PointLight &light) const
{
	const ClusterConfig &c = m_config;
	ScreenRange range;
	range.tileX0 = range.tileY0 = range.layer0 = 0;
	range.tileX1 = m_grid.tilesX - 1;
	range.tileY1 = m_grid.tilesY - 1;
	range.layer1 = m_grid.layers - 1;

	const float cx = light.position[0];
	const float cy = light.position[1];
	const float cz = light.position[2];
	const float r = light.radius;
	const float dCenter = -cz;

	// Depth layer span from the nearest/farthest points of the sphere. A
	// relative epsilon keeps a sphere tangent to a layer boundary conservative.
	const float depthEps = 1e-5f * std::max(std::fabs(dCenter), r);
	range.layer0 = depthLayer(dCenter - r - depthEps);
	range.layer1 = depthLayer(dCenter + r + depthEps);

	// Sphere reaching or crossing the eye plane z = 0 (also covers a center
	// behind the camera whose sphere still enters the visible region): no
	// bounded screen projection exists, so fall back to the full tile range.
	if (cz + r >= 0.0f)
		return range;

	// Entirely in front of the eye: exact slope range x/d and y/d of the
	// tangent cone from the camera origin to the sphere.
	const float a = dCenter * dCenter - r * r; // strictly positive here
	const float sx = std::sqrt(cx * cx + a);
	const float sy = std::sqrt(cy * cy + a);

	const float slopeXLo = (cx * dCenter - r * sx) / a;
	const float slopeXHi = (cx * dCenter + r * sx) / a;
	const float slopeYLo = (cy * dCenter - r * sy) / a;
	const float slopeYHi = (cy * dCenter + r * sy) / a;

	const float n = c.nearPlane;
	const float fx0 = (n * slopeXLo - c.left) / (c.right - c.left);
	const float fx1 = (n * slopeXHi - c.left) / (c.right - c.left);
	const float fy0 = (n * slopeYLo - c.bottom) / (c.top - c.bottom);
	const float fy1 = (n * slopeYHi - c.bottom) / (c.top - c.bottom);

	const float xMin = std::min(fx0, fx1) * (float)m_width;
	const float xMax = std::max(fx0, fx1) * (float)m_width;
	const float yMin = std::min(fy0, fy1) * (float)m_height;
	const float yMax = std::max(fy0, fy1) * (float)m_height;

	// Tight epsilon at the continuous boundaries: a sphere exactly tangent to a
	// tile edge must survive floating-point rounding, while lights far from
	// every boundary keep their minimal tile span. The precise six-plane test
	// still rejects any false positive this may add.
	const float tileEps = 1e-4f;
	range.tileX0 = clampInt((int)std::floor((xMin - tileEps) / c.tileWidth),
		0, m_grid.tilesX - 1);
	range.tileX1 = clampInt((int)std::floor((xMax + tileEps) / c.tileWidth),
		0, m_grid.tilesX - 1);
	range.tileY0 = clampInt((int)std::floor((yMin - tileEps) / c.tileHeight),
		0, m_grid.tilesY - 1);
	range.tileY1 = clampInt((int)std::floor((yMax + tileEps) / c.tileHeight),
		0, m_grid.tilesY - 1);
	return range;
}

bool ClusteredLighting::sphereIntersectsCluster(const PointLight &light, const Cluster &cluster) const
{
	// Conservative: the sphere survives all six closed half spaces. Corner
	// false positives are permitted; tangency counts as intersection.
	for (const ClusterPlane &plane : cluster.planes) {
		const float distance =
			plane.normal[0] * light.position[0]
			+ plane.normal[1] * light.position[1]
			+ plane.normal[2] * light.position[2]
			+ plane.constant;
		if (distance + light.radius < -kPlaneEpsilon)
			return false;
	}
	return true;
}

void ClusteredLighting::buildClusters()
{
	for (Cluster &cluster : m_clusters)
		cluster.lights.clear();

	m_stats = ClusterBuildStats();

	for (int lightIndex = 0; lightIndex < (int)m_lights.size(); ++lightIndex) {
		const PointLight &light = m_lights[lightIndex];
		const ScreenRange range = conservativeRange(light);

		for (int ty = range.tileY0; ty <= range.tileY1; ++ty) {
			for (int tx = range.tileX0; tx <= range.tileX1; ++tx) {
				for (int k = range.layer0; k <= range.layer1; ++k) {
					Cluster &cluster = m_clusters[clusterIndex(tx, ty, k)];
					++m_stats.clusterSphereTests;
					if (sphereIntersectsCluster(light, cluster))
						cluster.lights.push_back(lightIndex);
				}
			}
		}
	}
}

bool ClusteredLighting::isValid(int x, int y) const
{
	const int lx = x - m_originX;
	const int ly = y - m_originY;
	if (lx < 0 || lx >= m_width || ly < 0 || ly >= m_height)
		return false;
	return m_valid[(size_t)ly * (size_t)m_width + (size_t)lx] != 0;
}

const float *ClusteredLighting::viewPosition(int x, int y) const
{
	const size_t index = (size_t)(y - m_originY) * (size_t)m_width + (size_t)(x - m_originX);
	return m_positions[index].data();
}

const float *ClusteredLighting::normal(int x, int y) const
{
	const size_t index = (size_t)(y - m_originY) * (size_t)m_width + (size_t)(x - m_originX);
	return m_normals[index].data();
}

const float *ClusteredLighting::albedo(int x, int y) const
{
	const size_t index = (size_t)(y - m_originY) * (size_t)m_width + (size_t)(x - m_originX);
	return m_albedos[index].data();
}

float ClusteredLighting::depth(int x, int y) const
{
	const size_t index = (size_t)(y - m_originY) * (size_t)m_width + (size_t)(x - m_originX);
	return m_depth[index];
}

std::vector<std::array<float, 3>> ClusteredLighting::shade() const
{
	const size_t pixels = (size_t)m_width * (size_t)m_height;
	std::vector<std::array<float, 3>> image(pixels, {{0.0f, 0.0f, 0.0f}});
	m_stats.lightingEvaluations = 0;

	for (int ly = 0; ly < m_height; ++ly) {
		for (int lx = 0; lx < m_width; ++lx) {
			const size_t index = (size_t)ly * (size_t)m_width + (size_t)lx;
			if (!m_valid[index])
				continue;

			const std::array<float, 3> &P = m_positions[index];
			const std::array<float, 3> &N = m_normals[index];
			const std::array<float, 3> &albedo = m_albedos[index];

			std::array<float, 3> color = {{
				m_ambient[0] * albedo[0],
				m_ambient[1] * albedo[1],
				m_ambient[2] * albedo[2]
			}};

			const int wx = lx + m_originX;
			const int wy = ly + m_originY;
			const int tx = tileForPixel(wx, m_originX, m_config.tileWidth, m_grid.tilesX);
			const int ty = tileForPixel(wy, m_originY, m_config.tileHeight, m_grid.tilesY);
			const int k = depthLayer(-P[2]);
			const Cluster &cluster = m_clusters[clusterIndex(tx, ty, k)];

			for (int lightIndex : cluster.lights) {
				++m_stats.lightingEvaluations;
				const PointLight &light = m_lights[lightIndex];

				const float vx = light.position[0] - P[0];
				const float vy = light.position[1] - P[1];
				const float vz = light.position[2] - P[2];
				const float dist2 = vx * vx + vy * vy + vz * vz;
				const float dist = std::sqrt(dist2);

				if (!(dist > 0.0f) || !(dist < light.radius))
					continue;

				const float invDist = 1.0f / dist;
				const float ndl = std::max(
					0.0f,
					N[0] * vx * invDist + N[1] * vy * invDist + N[2] * vz * invDist);
				const float falloff = 1.0f - dist2 / (light.radius * light.radius);
				const float attenuation = falloff * falloff / std::max(dist2, 1e-6f);

				for (int channel = 0; channel < 3; ++channel) {
					color[channel] +=
						albedo[channel] * light.intensity[channel] * ndl * attenuation;
				}
			}

			image[index] = color;
		}
	}

	return image;
}

} // end namespace swr
