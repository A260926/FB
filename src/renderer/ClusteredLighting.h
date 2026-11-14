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

  Usage order:
  1. beginGeometryPass() to (re)allocate and clear the geometry buffer.
  2. Draw visible surfaces with the owned VertexProcessor/Rasterizer using
     drawElements(). Only fragments that pass the discard test and the
     depth test are written.
  3. endGeometryPass().
  4. configureClusters() with the perspective frustum, viewport and grid.
  5. buildClusters() with the point light table; fills the cluster light
     lists and the cluster-sphere test statistics.
  6. shade() to compute a standalone linear RGB image from the completed
     geometry buffer and cluster lists.
*/

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "Rasterizer.h"
#include "VertexProcessor.h"
#include "PixelShaderBase.h"
#include "VertexShaderBase.h"

namespace swr {

/// Minimal 3 component vector.
struct Vec3f {
	float x;
	float y;
	float z;

	Vec3f() : x(0.0f), y(0.0f), z(0.0f) {}
	Vec3f(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

	Vec3f& operator+=(const Vec3f &o) { x += o.x; y += o.y; z += o.z; return *this; }
	Vec3f& operator-=(const Vec3f &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
	Vec3f& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline Vec3f operator+(const Vec3f &a, const Vec3f &b) { return Vec3f(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3f operator-(const Vec3f &a, const Vec3f &b) { return Vec3f(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3f operator*(const Vec3f &a, float s) { return Vec3f(a.x * s, a.y * s, a.z * s); }
inline Vec3f operator*(float s, const Vec3f &a) { return a * s; }
inline Vec3f operator*(const Vec3f &a, const Vec3f &b) { return Vec3f(a.x * b.x, a.y * b.y, a.z * b.z); }
inline float dot(const Vec3f &a, const Vec3f &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3f &a) { return std::sqrt(dot(a, a)); }
inline Vec3f normalize(const Vec3f &a)
{
	float l = length(a);
	if (l <= 0.0f)
		return Vec3f(0.0f, 0.0f, 0.0f);
	return a * (1.0f / l);
}

/// Linear RGB color, never clamped by the lighting path.
struct Color3f {
	float r;
	float g;
	float b;

	Color3f() : r(0.0f), g(0.0f), b(0.0f) {}
	Color3f(float r_, float g_, float b_) : r(r_), g(g_), b(b_) {}
};

/// Finite radius point light in view space.
struct PointLight {
	Vec3f position;   ///< View space center.
	float radius;     ///< Positive influence radius.
	Vec3f intensity;  ///< Non negative linear RGB intensity.
};

/// Perspective frustum and cluster grid configuration.
struct ClusterConfig {
	float nearPlane;  ///< Near distance d (view space, positive).
	float farPlane;   ///< Far distance d (view space, positive).

	// Near plane rectangle.
	float left;
	float right;
	float bottom;
	float top;

	// Integer viewport, supports a non zero origin.
	int viewportX;
	int viewportY;
	int viewportWidth;
	int viewportHeight;

	// Tile size in pixels. The last tile is clipped to the viewport.
	int tileWidth;
	int tileHeight;

	int depthSlices; ///< Number K of logarithmic depth layers.
};

/// One captured surface sample. Background pixels are not valid.
struct GeometryFragment {
	Vec3f position; ///< Perspective corrected view space position.
	Vec3f normal;   ///< Re-normalized interpolated view space normal.
	Vec3f albedo;   ///< Perspective corrected linear RGB albedo.
	float depth;    ///< Window depth in [0, 1].
	bool valid;     ///< False for background.
};

/// Closed half space defined as normal . p + constant >= 0 (normal unit length).
struct ClusterPlane {
	Vec3f normal;
	float constant;
};

/// A single cluster: four origin side planes and two depth planes.
struct Cluster {
	int tileX;
	int tileY;
	int slice;

	// Pixel rectangle, half open: [minPixelX, maxPixelX) x [minPixelY, maxPixelY).
	int minPixelX;
	int minPixelY;
	int maxPixelX;
	int maxPixelY;

	float depthNear; ///< d of the near depth plane.
	float depthFar;  ///< d of the far depth plane.

	ClusterPlane planes[6]; ///< left, right, bottom, top, near, far.

	std::vector<int> lights; ///< Original light indices, input order, unique.
};

/// Input vertex for the default geometry buffer vertex shader.
struct GBufferVertex {
	float clipX, clipY, clipZ, clipW; ///< Clip space position.
	Vec3f position;                   ///< View space position.
	Vec3f normal;                     ///< View space normal.
	Vec3f albedo;                     ///< Linear RGB albedo.
};

/// Default vertex shader: forwards the clip position and the 9 perspective
/// correct varyings (position.xyz, normal.xyz, albedo.rgb).
class GBufferVertexShader : public VertexShaderBase<GBufferVertexShader> {
public:
	static const int AttribCount = 1;
	static const int AVarCount = 0;
	static const int PVarCount = 9;

	static void processVertex(VertexShaderInput in, VertexShaderOutput *out)
	{
		const GBufferVertex *v = static_cast<const GBufferVertex*>(in[0]);
		out->x = v->clipX;
		out->y = v->clipY;
		out->z = v->clipZ;
		out->w = v->clipW;
		out->pvar[0] = v->position.x;
		out->pvar[1] = v->position.y;
		out->pvar[2] = v->position.z;
		out->pvar[3] = v->normal.x;
		out->pvar[4] = v->normal.y;
		out->pvar[5] = v->normal.z;
		out->pvar[6] = v->albedo.x;
		out->pvar[7] = v->albedo.y;
		out->pvar[8] = v->albedo.z;
	}
};

/// Optional per fragment discard test. Returning true discards the fragment:
/// neither geometry nor depth are written.
using DiscardCallback = std::function<bool (const GeometryFragment &)>;

/// Geometry buffer pixel shader. Writes into the active ClusteredLighting
/// context. The active context is installed by beginGeometryPass().
class GBufferPixelShader : public PixelShaderBase<GBufferPixelShader> {
public:
	static const int InterpolateZ = 1;
	static const int InterpolateW = 1;
	static const int AVarCount = 0;
	static const int PVarCount = 9;

	static class ClusteredLighting *activeContext;

	static void drawPixel(const PixelData &p);
};

/// CPU clustered deferred lighting context.
class ClusteredLighting {
public:
	ClusteredLighting();
	~ClusteredLighting();

	// ----- Geometry pass -------------------------------------------------

	/// The rasterizer used to draw visible surfaces.
	Rasterizer& rasterizer() { return m_rasterizer; }

	/// The vertex processor used to draw visible surfaces.
	VertexProcessor& vertexProcessor() { return m_vertexProcessor; }

	/// (Re)allocate and clear the geometry buffer and install the geometry
	/// buffer pixel shader on the owned rasterizer.
	void beginGeometryPass(int width, int height);

	/// Release the geometry buffer pixel shader binding.
	void endGeometryPass();

	/// Install a discard callback (nullptr disables discarding).
	void setDiscardCallback(const DiscardCallback &callback);

	/// Internal entry point used by GBufferPixelShader.
	void writeFragment(const PixelData &p);

	// ----- Geometry buffer access ---------------------------------------

	int framebufferWidth() const { return m_width; }
	int framebufferHeight() const { return m_height; }

	/// True if the pixel holds a captured surface fragment.
	bool isValid(int x, int y) const;

	/// Captured fragment for a pixel; only valid when isValid() is true.
	const GeometryFragment& fragmentAt(int x, int y) const;

	// ----- Cluster build -------------------------------------------------

	/// Set the frustum and cluster grid. Validates the parameters.
	void configureClusters(const ClusterConfig &config);

	const ClusterConfig& clusterConfig() const { return m_config; }

	/// Assign every light to the clusters its sphere conservatively
	/// intersects. Rebuilding clears all previous lists and statistics.
	void buildClusters(const std::vector<PointLight> &lights);

	int tileCountX() const { return m_tilesX; }
	int tileCountY() const { return m_tilesY; }
	int sliceCount() const { return m_slices; }
	int clusterCount() const { return m_tilesX * m_tilesY * m_slices; }

	/// Cluster at grid coordinates.
	const Cluster& cluster(int tileX, int tileY, int slice) const;

	/// Cluster index of a framebuffer pixel and view space depth.
	int clusterIndexForPixel(int x, int y, float viewDepth) const;

	/// Number of six plane sphere/cluster tests executed by the last
	/// buildClusters() call.
	std::size_t clusterSphereCheckCount() const { return m_sphereChecks; }

	// ----- Lighting ------------------------------------------------------

	/// Shade the completed geometry buffer with the lights stored at the
	/// last build. Returns an independent image; background stays zero.
	/// The light table must match the one given to buildClusters().
	std::vector<Color3f> shade(const std::vector<PointLight> &lights, const Vec3f &ambient);

	/// Number of per pixel light evaluations performed by the last shade().
	std::size_t lightCalculationCount() const { return m_lightCalculations; }

	/// Full reset of every buffer, list and statistic.
	void clear();

private:
	void resizeFramebuffer(int width, int height);
	void clearFramebuffer();
	void buildClusterPlanes();
	int sliceForDepth(float viewDepth) const;

	Rasterizer m_rasterizer;
	VertexProcessor m_vertexProcessor;
	DiscardCallback m_discard;

	int m_width;
	int m_height;
	std::vector<GeometryFragment> m_fragments;
	std::vector<float> m_depthBuffer;

	ClusterConfig m_config;
	bool m_configured;
	int m_tilesX;
	int m_tilesY;
	int m_slices;
	std::vector<Cluster> m_clusters;
	std::vector<float> m_depthBoundaries; ///< K + 1 logarithmic boundaries.
	std::size_t m_sphereChecks;

	std::size_t m_lightCountAtBuild;
	std::size_t m_lightCalculations;
};

} // end namespace swr
