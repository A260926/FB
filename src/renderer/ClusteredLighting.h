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

/** @file
  CPU clustered deferred lighting path.

  The pipeline is:
  1. configure() a perspective frustum, viewport, tile size and depth layer count.
  2. beginGeometry() attaches the reusable G-buffer pixel shader to the existing
     Rasterizer / VertexProcessor pair.
  3. Draw visible surfaces with VertexProcessor::drawElements(). Only fragments
     that are not discarded and pass the depth test commit geometry and depth.
  4. setLights() and buildClusters() assign finite-radius point lights to the
     frustum clusters whose six closed half spaces intersect each light sphere.
  5. shade() walks only the lights of the cluster covering each valid pixel and
     returns an independent floating point RGB image.
*/

#include <array>
#include <cstdint>
#include <vector>

#include "Rasterizer.h"
#include "VertexProcessor.h"

namespace swr {

/// View-space point light. The camera looks down -Z, so view depth is d = -z.
struct PointLight {
	float position[3];  ///< View-space center.
	float radius;       ///< Positive influence radius.
	float intensity[3]; ///< Non-negative linear RGB intensity.
};

/// Finite regular perspective frustum plus the clustering layout.
struct ClusterConfig {
	float nearPlane = 1.0f;   ///< Near view depth, > 0.
	float farPlane = 100.0f;  ///< Far view depth, > near.
	float left = -1.0f;       ///< Near-plane left bound.
	float right = 1.0f;       ///< Near-plane right bound, > left.
	float bottom = -1.0f;     ///< Near-plane bottom bound.
	float top = 1.0f;         ///< Near-plane top bound, > bottom.

	int viewportX = 0;        ///< Integer viewport origin, may be non-zero.
	int viewportY = 0;
	int viewportWidth = 1;    ///< Positive integer viewport size.
	int viewportHeight = 1;

	int tileWidth = 8;        ///< Configured tile width in pixels.
	int tileHeight = 8;       ///< Configured tile height in pixels.
	int depthLayers = 1;      ///< Number K of logarithmic depth layers.
};

/// Closed half space. Interior points satisfy normal . p + constant >= 0.
struct ClusterPlane {
	float normal[3];
	float constant;
};

/// One frustum cluster: four side planes through the camera origin and two
/// depth planes, plus its assigned light list.
struct Cluster {
	ClusterPlane planes[6]; ///< left, right, top, bottom, depth near, depth far.
	int tileX = 0;
	int tileY = 0;
	int layer = 0;
	int pixelX0 = 0;        ///< Absolute window pixel range, x1/y1 exclusive.
	int pixelY0 = 0;
	int pixelX1 = 0;
	int pixelY1 = 0;
	std::vector<int> lights; ///< Original light indices, input order, unique.
};

/// Tile/layer counts and the derived depth boundaries.
struct ClusterGrid {
	int tilesX = 0;
	int tilesY = 0;
	int layers = 0;
	std::vector<float> depthBoundaries; ///< K + 1 values, near*(far/near)^(k/K).
};

/// Counters of work really performed; estimates are never stored here.
struct ClusterBuildStats {
	std::uint64_t clusterSphereTests = 0;     ///< Six-plane sphere tests executed.
	std::uint64_t lightingEvaluations = 0;    ///< Per-pixel light evaluations in shade().
};

/// Vertex consumed by GBufferVertexShader: clip-space position plus the view
/// space attributes that are interpolated perspective correct.
struct GBufferVertex {
	float clip[4];      ///< Clip-space x, y, z, w.
	float position[3];  ///< View-space position.
	float normal[3];    ///< View-space normal (normalized again per fragment).
	float albedo[3];    ///< Linear RGB albedo.
};

/// Reusable vertex shader for the G-buffer pass.
struct GBufferVertexShader : public VertexShaderBase<GBufferVertexShader> {
	static const int AttribCount = 1;
	static const int PVarCount = 9;

	static void processVertex(VertexShaderInput in, VertexShaderOutput *out);
};

/// Optional discard callback. Returning true discards the fragment before the
/// depth test, so neither geometry nor depth is committed.
using GBufferDiscardFn = bool (*)(
	int pixelX,
	int pixelY,
	const float viewPosition[3],
	const float normalizedNormal[3],
	const float albedo[3],
	float windowDepth);

/// Reusable G-buffer pixel shader. ClusteredLighting installs itself as the
/// active target in beginGeometry().
class GBufferPixelShader : public PixelShaderBase<GBufferPixelShader> {
public:
	static const bool InterpolateZ = true;
	static const bool InterpolateW = true;
	static const int PVarCount = 9;

	static class ClusteredLighting *activeTarget;

	static void drawPixel(const PixelData &p);
};

/// Project a view-space point with a finite regular (possibly asymmetric)
/// perspective frustum described by its near-plane bounds.
void projectPerspective(const ClusterConfig &config, const float viewPosition[3], float clip[4]);

/// CPU clustered deferred lighting context.
class ClusteredLighting {
public:
	ClusteredLighting();
	explicit ClusteredLighting(const ClusterConfig &config);

	/// (Re)allocate all buffers for a new frustum/viewport/layout. Every
	/// previous G-buffer, cluster and light-table state is dropped.
	void configure(const ClusterConfig &config);

	const ClusterConfig &config() const { return m_config; }
	RasterMode rasterMode() const { return m_rasterMode; }
	void setRasterMode(RasterMode mode) { m_rasterMode = mode; }

	void setAmbient(float r, float g, float b);
	const std::array<float, 3> &ambient() const { return m_ambient; }

	void setDiscardFunction(GBufferDiscardFn fn) { m_discardFunction = fn; }
	GBufferDiscardFn discardFunction() const { return m_discardFunction; }

	/// Clear geometry and depth. Depth starts at +infinity, every pixel is
	/// invalid until a surviving fragment is committed.
	void clear();

	/// Install scissor, viewport and the G-buffer shader and make this context
	/// the render target. Afterwards draw with vertexProcessor().
	void beginGeometry();

	/// Convenience wrapper around VertexProcessor::drawElements().
	void drawElements(DrawMode mode, size_t count, int *indices);

	Rasterizer &rasterizer() { return m_rasterizer; }
	VertexProcessor &vertexProcessor() { return m_vertexProcessor; }

	/// Replace the light table. Copies the input; cluster lists are invalidated
	/// until buildClusters() runs again.
	void setLights(const std::vector<PointLight> &lights);
	void setLights(std::vector<PointLight> &&lights);
	const std::vector<PointLight> &lights() const { return m_lights; }

	/// Assign lights to clusters. No cluster/light cartesian product is tested:
	/// each light sphere first yields a conservative screen tile and depth
	/// layer range, and only clusters inside that range get the six-plane test.
	void buildClusters();

	const ClusterGrid &grid() const { return m_grid; }
	const std::vector<Cluster> &clusters() const { return m_clusters; }
	const ClusterBuildStats &stats() const { return m_stats; }

	/// Index of cluster (tileX, tileY, layer).
	size_t clusterIndex(int tileX, int tileY, int layer) const;

	/// G-buffer accessors. Window coordinates are absolute window pixels.
	bool isValid(int x, int y) const;
	const float *viewPosition(int x, int y) const;
	const float *normal(int x, int y) const;
	const float *albedo(int x, int y) const;
	float depth(int x, int y) const;

	/// Shade the completed G-buffer with the current light table. Reads only
	/// completed buffers and returns a freshly allocated W*H linear RGB image
	/// (invalid pixels are zero). Values are not clamped.
	std::vector<std::array<float, 3>> shade() const;

	/// Called by GBufferPixelShader; public so the shader stays a thin adapter.
	/// Returns true when the fragment survived the discard and depth test.
	bool submitFragment(
		int x,
		int y,
		float windowDepth,
		const float viewPosition[3],
		const float normalizedNormal[3],
		const float albedo[3]);

private:
	struct ScreenRange {
		int tileX0, tileX1;
		int tileY0, tileY1;
		int layer0, layer1;
	};

	void allocateBuffers();
	void buildGrid();
	ScreenRange conservativeRange(const PointLight &light) const;
	bool sphereIntersectsCluster(const PointLight &light, const Cluster &cluster) const;
	int depthLayer(float viewDepth) const;
	int tileForPixel(int pixel, int origin, int tileSize, int tileCount) const;

	ClusterConfig m_config;
	RasterMode m_rasterMode = RasterMode::Span;
	Rasterizer m_rasterizer;
	VertexProcessor m_vertexProcessor;

	ClusterGrid m_grid;
	std::vector<Cluster> m_clusters;
	std::vector<PointLight> m_lights;
	std::array<float, 3> m_ambient{{0.0f, 0.0f, 0.0f}};
	mutable ClusterBuildStats m_stats;
	GBufferDiscardFn m_discardFunction = nullptr;

	int m_width = 0;
	int m_height = 0;
	int m_originX = 0;
	int m_originY = 0;
	std::vector<std::uint8_t> m_valid;
	std::vector<float> m_depth;
	std::vector<std::array<float, 3>> m_positions;
	std::vector<std::array<float, 3>> m_normals;
	std::vector<std::array<float, 3>> m_albedos;
};

} // end namespace swr
