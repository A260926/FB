/*
 * Contract tests for LNLib::TrimmedSurfaceMesh::Build.
 *
 * The three registered groups cover
 *   - domain:      parameter-domain handling, concave contours, bad input,
 *   - seam:        contours / holes crossing the U seam, seam-side adjacency,
 *   - refinement:  chord tolerance, maxDepth exhaustion, knot density.
 *
 * Every case checks the produced mesh independently of the implementation:
 * the unfolded area, hole exclusion, per-corner UV substitution, conforming
 * edge usage, T-junctions, contour coverage and the fixed chord samples.
 */

#include "TrimmedSurfaceMesh.h"

#include "NurbsCurve.h"
#include "NurbsSurface.h"
#include "Constants.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../task_support/check.h"

using namespace LNLib;

static bool g_ok = true;

#define SOFT_CHECK(condition) \
	do { if (!task_check(!!(condition), #condition, __FILE__, __LINE__)) g_ok = false; } while (0)

namespace
{
	struct UFace
	{
		int v[3];
		UV uv[3];
	};

	LN_NurbsSurface MakeCylinder(double radius, double height, double umin, double umax,
		const std::vector<double>* vKnots = nullptr, const std::vector<XYZW>* profilePoints = nullptr)
	{
		LN_NurbsCurve profile;
		if (profilePoints == nullptr)
		{
			NurbsCurve::CreateLine(XYZ(radius, 0, 0), XYZ(radius, 0, height), profile);
		}
		else
		{
			profile.Degree = static_cast<int>(vKnots->size()) - static_cast<int>(profilePoints->size()) - 1;
			profile.KnotVector = *vKnots;
			profile.ControlPoints = *profilePoints;
		}

		LN_NurbsSurface surface;
		bool ok = NurbsSurface::CreateRevolvedSurface(XYZ(0, 0, 0), XYZ(0, 0, 1),
			2.0 * Constants::Pi, profile, surface);
		SOFT_CHECK(ok);

		double a = surface.KnotVectorU.front();
		double span = surface.KnotVectorU.back() - a;
		for (double& knot : surface.KnotVectorU)
		{
			knot = umin + (knot - a) / span * (umax - umin);
		}
		return surface;
	}

	double LoopArea(const std::vector<UV>& loop)
	{
		double area = 0.0;
		for (std::size_t i = 0; i < loop.size(); ++i)
		{
			const UV& a = loop[i];
			const UV& b = loop[(i + 1) % loop.size()];
			area += a.GetU() * b.GetV() - b.GetU() * a.GetV();
		}
		return area * 0.5;
	}

	bool PointInLoop(const UV& point, const std::vector<UV>& loop)
	{
		bool inside = false;
		for (std::size_t i = 0; i < loop.size(); ++i)
		{
			const UV& a = loop[i];
			const UV& b = loop[(i + 1) % loop.size()];
			if ((a.GetV() > point.GetV()) != (b.GetV() > point.GetV()))
			{
				const double x = a.GetU() + (point.GetV() - a.GetV()) * (b.GetU() - a.GetU()) / (b.GetV() - a.GetV());
				if (point.GetU() < x)
				{
					inside = !inside;
				}
			}
		}
		return inside;
	}

	bool OnSegment(const UV& point, const UV& a, const UV& b, double eps)
	{
		const double dx = b.GetU() - a.GetU();
		const double dy = b.GetV() - a.GetV();
		const double length = std::sqrt(dx * dx + dy * dy);
		const double crossValue = dx * (point.GetV() - a.GetV()) - dy * (point.GetU() - a.GetU());
		if (std::fabs(crossValue) > eps * std::max(1.0, length))
		{
			return false;
		}
		const double dot = (point.GetU() - a.GetU()) * dx + (point.GetV() - a.GetV()) * dy;
		return dot >= -eps && dot <= dx * dx + dy * dy + eps;
	}

	std::vector<UFace> Unwrap(const LN_Mesh& mesh, double period)
	{
		double reference = 0.0;
		if (!mesh.UVs.empty())
		{
			for (const UV& uv : mesh.UVs)
			{
				reference += uv.GetU();
			}
			reference /= static_cast<double>(mesh.UVs.size());
		}

		std::vector<UFace> faces;
		faces.reserve(mesh.Faces.size());
		for (std::size_t f = 0; f < mesh.Faces.size(); ++f)
		{
			UFace face;
			face.v[0] = mesh.Faces[f][0];
			face.v[1] = mesh.Faces[f][1];
			face.v[2] = mesh.Faces[f][2];
			UV raw[3] = {
				mesh.UVs[mesh.UVIndices[3 * f + 0]],
				mesh.UVs[mesh.UVIndices[3 * f + 1]],
				mesh.UVs[mesh.UVIndices[3 * f + 2]]
			};
			const long long shift = static_cast<long long>(std::llround((reference - raw[0].GetU()) / period));
			const double u0 = raw[0].GetU() + static_cast<double>(shift) * period;
			face.uv[0] = UV(u0, raw[0].GetV());
			for (int k = 1; k < 3; ++k)
			{
				double u = raw[k].GetU();
				while (u - u0 > 0.5 * period) u -= period;
				while (u0 - u > 0.5 * period) u += period;
				face.uv[k] = UV(u, raw[k].GetV());
			}
			faces.push_back(face);
		}
		return faces;
	}

	void VerifyMesh(const LN_NurbsSurface& surface, const std::vector<std::vector<UV>>& loops,
		const TrimMeshOptions& options, const LN_Mesh& mesh)
	{
		const double a = surface.KnotVectorU.front();
		const double b = surface.KnotVectorU.back();
		const double period = b - a;
		const double vMin = surface.KnotVectorV.front();
		const double vMax = surface.KnotVectorV.back();
		const std::vector<UFace> faces = Unwrap(mesh, period);

		SOFT_CHECK(!faces.empty());
		SOFT_CHECK(mesh.UVs.size() == 3 * faces.size());
		SOFT_CHECK(mesh.UVIndices.size() == 3 * faces.size());
		SOFT_CHECK(mesh.Vertices.size() <= mesh.UVs.size());
		if (faces.empty())
		{
			return;
		}

		// Per-corner UVs, orientation and chord samples.  A seam vertex keeps the
		// a-side position, so a corner whose UV lies on the b side may differ from
		// it by up to seamTolerance.
		const double substitutionTolerance = 1e-9 + options.seamTolerance;
		for (std::size_t f = 0; f < faces.size(); ++f)
		{
			for (int k = 0; k < 3; ++k)
			{
				const UV& uv = mesh.UVs[mesh.UVIndices[3 * f + k]];
				SOFT_CHECK(uv.GetU() >= a - 1e-9 && uv.GetU() <= b + 1e-9);
				SOFT_CHECK(uv.GetV() > vMin - 1e-9 && uv.GetV() < vMax + 1e-9);
				const XYZ substituted = NurbsSurface::GetPointOnSurface(surface, uv);
				SOFT_CHECK(substituted.Distance(mesh.Vertices[faces[f].v[k]]) <= substitutionTolerance);
			}

			const UV& u0 = mesh.UVs[mesh.UVIndices[3 * f + 0]];
			const UV& u1 = mesh.UVs[mesh.UVIndices[3 * f + 1]];
			const UV& u2 = mesh.UVs[mesh.UVIndices[3 * f + 2]];
			const double orientation = (u1.GetU() - u0.GetU()) * (u2.GetV() - u0.GetV())
				- (u1.GetV() - u0.GetV()) * (u2.GetU() - u0.GetU());
			SOFT_CHECK(orientation > 0.0);

			const XYZ p0 = mesh.Vertices[faces[f].v[0]];
			const XYZ p1 = mesh.Vertices[faces[f].v[1]];
			const XYZ p2 = mesh.Vertices[faces[f].v[2]];
			const UV m0(0.5 * (u0.GetU() + u1.GetU()), 0.5 * (u0.GetV() + u1.GetV()));
			const UV m1(0.5 * (u1.GetU() + u2.GetU()), 0.5 * (u1.GetV() + u2.GetV()));
			const UV m2(0.5 * (u2.GetU() + u0.GetU()), 0.5 * (u2.GetV() + u0.GetV()));
			const UV centre((u0.GetU() + u1.GetU() + u2.GetU()) / 3.0,
				(u0.GetV() + u1.GetV() + u2.GetV()) / 3.0);
			const double tolerance = options.chordTolerance * (1.0 + 1e-9) + 1e-12;
			SOFT_CHECK(NurbsSurface::GetPointOnSurface(surface, m0).Distance(0.5 * (p0 + p1)) <= tolerance);
			SOFT_CHECK(NurbsSurface::GetPointOnSurface(surface, m1).Distance(0.5 * (p1 + p2)) <= tolerance);
			SOFT_CHECK(NurbsSurface::GetPointOnSurface(surface, m2).Distance(0.5 * (p2 + p0)) <= tolerance);
			SOFT_CHECK(NurbsSurface::GetPointOnSurface(surface, centre).Distance((p0 + p1 + p2) / 3.0) <= tolerance);
		}

		// Unfolded area equals the trimmed region area, so there are no gaps or
		// overlaps.
		double expectedArea = std::fabs(LoopArea(loops[0]));
		for (std::size_t i = 1; i < loops.size(); ++i)
		{
			expectedArea -= std::fabs(LoopArea(loops[i]));
		}
		double actualArea = 0.0;
		for (const UFace& face : faces)
		{
			actualArea += std::fabs((face.uv[1].GetU() - face.uv[0].GetU()) * (face.uv[2].GetV() - face.uv[0].GetV())
				- (face.uv[1].GetV() - face.uv[0].GetV()) * (face.uv[2].GetU() - face.uv[0].GetU())) * 0.5;
		}
		SOFT_CHECK(std::fabs(actualArea - expectedArea) <= 1e-9 * std::max(1.0, expectedArea));

		// Holes are excluded.
		for (std::size_t h = 1; h < loops.size(); ++h)
		{
			for (const UFace& face : faces)
			{
				const UV centroid((face.uv[0].GetU() + face.uv[1].GetU() + face.uv[2].GetU()) / 3.0,
					(face.uv[0].GetV() + face.uv[1].GetV() + face.uv[2].GetV()) / 3.0);
				for (long long shift = -2; shift <= 2; ++shift)
				{
					std::vector<UV> shifted;
					shifted.reserve(loops[h].size());
					for (const UV& uv : loops[h])
					{
						shifted.push_back(UV(uv.GetU() + static_cast<double>(shift) * period, uv.GetV()));
					}
					SOFT_CHECK(!PointInLoop(centroid, shifted));
				}
			}
		}

		// Conforming edge usage, plus the edge UVs.
		std::map<std::pair<int, int>, int> edgeUse;
		std::map<std::pair<int, int>, std::pair<UV, UV>> edgeUV;
		for (const UFace& face : faces)
		{
			for (int k = 0; k < 3; ++k)
			{
				const int x = face.v[k];
				const int y = face.v[(k + 1) % 3];
				const std::pair<int, int> key(std::min(x, y), std::max(x, y));
				edgeUse[key] += 1;
				if (edgeUV.find(key) == edgeUV.end())
				{
					edgeUV[key] = std::make_pair(face.uv[k], face.uv[(k + 1) % 3]);
				}
			}
		}
		for (const auto& entry : edgeUse)
		{
			SOFT_CHECK(entry.second == 1 || entry.second == 2);
		}

		// No T-junction: no vertex strictly inside a mesh edge.
		int tJunctions = 0;
		for (const auto& entry : edgeUV)
		{
			const UV& p0 = entry.second.first;
			const UV& p1 = entry.second.second;
			const double dx = p1.GetU() - p0.GetU();
			const double dy = p1.GetV() - p0.GetV();
			const double lengthSquared = dx * dx + dy * dy;
			if (lengthSquared <= 0.0)
			{
				continue;
			}
			for (const UFace& face : faces)
			{
				for (int k = 0; k < 3; ++k)
				{
					if (face.v[k] == entry.first.first || face.v[k] == entry.first.second)
					{
						continue;
					}
					const UV& q = face.uv[k];
					const double crossValue = dx * (q.GetV() - p0.GetV()) - dy * (q.GetU() - p0.GetU());
					if (std::fabs(crossValue) > 1e-12)
					{
						continue;
					}
					const double dot = (q.GetU() - p0.GetU()) * dx + (q.GetV() - p0.GetV()) * dy;
					if (dot > 1e-12 * lengthSquared && dot < lengthSquared * (1.0 - 1e-12))
					{
						++tJunctions;
					}
				}
			}
		}
		SOFT_CHECK(tJunctions == 0);

		// No triangle crosses an original knot line or a seam copy.
		std::vector<double> knotLines;
		for (double knot : surface.KnotVectorU)
		{
			if (knot > a + 1e-12 && knot < b - 1e-12)
			{
				knotLines.push_back(knot);
			}
		}
		for (double knot : knotLines)
		{
			for (long long shift = -3; shift <= 3; ++shift)
			{
				const double target = knot + static_cast<double>(shift) * period;
				for (const UFace& face : faces)
				{
					double low = 1e300;
					double high = -1e300;
					for (int k = 0; k < 3; ++k)
					{
						low = std::min(low, face.uv[k].GetU());
						high = std::max(high, face.uv[k].GetU());
					}
					SOFT_CHECK(!(low < target - 1e-9 && high > target + 1e-9));
				}
			}
		}
		for (double knot : surface.KnotVectorV)
		{
			if (!(knot > vMin + 1e-12 && knot < vMax - 1e-12))
			{
				continue;
			}
			for (const UFace& face : faces)
			{
				double low = 1e300;
				double high = -1e300;
				for (int k = 0; k < 3; ++k)
				{
					low = std::min(low, face.uv[k].GetV());
					high = std::max(high, face.uv[k].GetV());
				}
				SOFT_CHECK(!(low < knot - 1e-9 && high > knot + 1e-9));
			}
		}

		// Every contour segment is covered by mesh boundary edges.
		std::vector<std::pair<UV, UV>> boundaryEdges;
		for (const auto& entry : edgeUse)
		{
			if (entry.second == 1)
			{
				boundaryEdges.push_back(edgeUV[entry.first]);
			}
		}
		for (const std::vector<UV>& loop : loops)
		{
			for (std::size_t i = 0; i < loop.size(); ++i)
			{
				const UV& e0 = loop[i];
				const UV& e1 = loop[(i + 1) % loop.size()];
				double covered = 0.0;
				for (const auto& edge : boundaryEdges)
				{
					for (long long shift = -3; shift <= 3; ++shift)
					{
						const UV p0(edge.first.GetU() + static_cast<double>(shift) * period, edge.first.GetV());
						const UV p1(edge.second.GetU() + static_cast<double>(shift) * period, edge.second.GetV());
						if (OnSegment(p0, e0, e1, 1e-9) && OnSegment(p1, e0, e1, 1e-9))
						{
							covered += std::sqrt(std::pow(p1.GetU() - p0.GetU(), 2) + std::pow(p1.GetV() - p0.GetV(), 2));
						}
					}
				}
				const double full = std::sqrt(std::pow(e1.GetU() - e0.GetU(), 2) + std::pow(e1.GetV() - e0.GetV(), 2));
				SOFT_CHECK(covered >= full * (1.0 - 1e-9));
			}
		}
	}

	bool ThrowsInvalid(const LN_NurbsSurface& surface, const std::vector<std::vector<UV>>& loops,
		const TrimMeshOptions& options)
	{
		try
		{
			TrimmedSurfaceMesh::Build(surface, loops, options);
		}
		catch (const std::invalid_argument&)
		{
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
		return false;
	}

	bool ThrowsRuntime(const LN_NurbsSurface& surface, const std::vector<std::vector<UV>>& loops,
		const TrimMeshOptions& options)
	{
		try
		{
			TrimmedSurfaceMesh::Build(surface, loops, options);
		}
		catch (const std::runtime_error&)
		{
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
		return false;
	}

	const LN_Mesh BuildOrFail(const LN_NurbsSurface& surface, const std::vector<std::vector<UV>>& loops,
		const TrimMeshOptions& options)
	{
		try
		{
			return TrimmedSurfaceMesh::Build(surface, loops, options);
		}
		catch (const std::exception&)
		{
			SOFT_CHECK(false);
			return LN_Mesh();
		}
	}
} // namespace

// ---------------------------------------------------------------------------
// domain
// ---------------------------------------------------------------------------

static int RunDomainNonUnitParameterRange()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 10.0, 14.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(10.3, 0.2), UV(11.2, 0.2), UV(11.2, 0.7), UV(10.3, 0.7) });
	loops.push_back({ UV(10.6, 0.4), UV(10.6, 0.5), UV(10.9, 0.5), UV(10.9, 0.4) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);
	SOFT_CHECK(mesh.UVs.size() == 3 * mesh.Faces.size());
	TASK_CHECK(g_ok);
	return 0;
}

static int RunDomainConcaveContour()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.1, 0.1), UV(0.8, 0.1), UV(0.8, 0.4), UV(0.4, 0.4), UV(0.4, 0.8), UV(0.1, 0.8) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);
	TASK_CHECK(g_ok);
	return 0;
}

static int RunDomainRejectsBadInput()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 8;

	SOFT_CHECK(ThrowsInvalid(surface, {}, options));
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.1), UV(0.2, 0.1) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.1), UV(0.4, 0.4), UV(0.4, 0.1), UV(0.1, 0.4) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.4, 0.1), UV(0.1, 0.1), UV(0.1, 0.4), UV(0.4, 0.4) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, {
		{ UV(0.1, 0.1), UV(0.8, 0.1), UV(0.8, 0.8), UV(0.1, 0.8) },
		{ UV(0.9, 0.2), UV(0.9, 0.3), UV(0.95, 0.3), UV(0.95, 0.2) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, {
		{ UV(0.1, 0.1), UV(0.8, 0.1), UV(0.8, 0.8), UV(0.1, 0.8) },
		{ UV(0.2, 0.2), UV(0.2, 0.3), UV(0.3, 0.3), UV(0.3, 0.2) },
		{ UV(0.25, 0.2), UV(0.25, 0.3), UV(0.35, 0.3), UV(0.35, 0.2) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.0), UV(0.8, 0.0), UV(0.8, 0.8), UV(0.1, 0.8) } }, options));
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.1), UV(1.2, 0.1), UV(1.2, 0.8), UV(0.1, 0.8) } }, options));

	TrimMeshOptions zeroChord = options;
	zeroChord.chordTolerance = 0.0;
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, zeroChord));
	TrimMeshOptions negativeSeam = options;
	negativeSeam.seamTolerance = -1.0;
	SOFT_CHECK(ThrowsInvalid(surface, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, negativeSeam));

	LN_NurbsSurface openSurface = surface;
	openSurface.KnotVectorU.back() = 1.5;
	SOFT_CHECK(ThrowsInvalid(openSurface, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, options));

	LN_NurbsSurface zeroWeight = surface;
	zeroWeight.ControlPoints[0][0] = XYZW(1, 0, 0, 0);
	SOFT_CHECK(ThrowsInvalid(zeroWeight, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, options));

	LN_NurbsSurface ragged = surface;
	ragged.ControlPoints[0].pop_back();
	SOFT_CHECK(ThrowsInvalid(ragged, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, options));

	LN_NurbsSurface nonMonotone = surface;
	nonMonotone.KnotVectorV[0] = -1.0;
	nonMonotone.KnotVectorV[1] = 0.5;
	nonMonotone.KnotVectorV[2] = 0.4;
	SOFT_CHECK(ThrowsInvalid(nonMonotone, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, options));

	LN_NurbsSurface highDegree = surface;
	highDegree.DegreeU = 4;
	SOFT_CHECK(ThrowsInvalid(highDegree, { { UV(0.1, 0.1), UV(0.5, 0.1), UV(0.5, 0.5), UV(0.1, 0.5) } }, options));

	// The input objects must not be modified.
	const LN_NurbsSurface surfaceCopy = surface;
	const std::vector<std::vector<UV>> loopsCopy = { { UV(0.1, 0.2), UV(0.4, 0.2), UV(0.4, 0.6), UV(0.1, 0.6) } };
	TrimmedSurfaceMesh::Build(surface, loopsCopy, options);
	SOFT_CHECK(surface.KnotVectorU == surfaceCopy.KnotVectorU);
	SOFT_CHECK(surface.KnotVectorV == surfaceCopy.KnotVectorV);
	SOFT_CHECK(surface.DegreeU == surfaceCopy.DegreeU && surface.DegreeV == surfaceCopy.DegreeV);
	SOFT_CHECK(surface.ControlPoints.size() == surfaceCopy.ControlPoints.size());
	bool controlsEqual = true;
	for (std::size_t i = 0; i < surface.ControlPoints.size(); ++i)
	{
		for (std::size_t j = 0; j < surface.ControlPoints[i].size(); ++j)
		{
			for (int k = 0; k < 4; ++k)
			{
				if (surface.ControlPoints[i][j][k] != surfaceCopy.ControlPoints[i][j][k])
				{
					controlsEqual = false;
				}
			}
		}
	}
	SOFT_CHECK(controlsEqual);

	TASK_CHECK(g_ok);
	return 0;
}

// ---------------------------------------------------------------------------
// seam
// ---------------------------------------------------------------------------

static int RunSeamOuterContourCrossesSeam()
{
	g_ok = true;
	const double a = 0.0;
	const double b = 1.0;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, a, b);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(-0.2, 0.2), UV(0.3, 0.2), UV(0.3, 0.6), UV(-0.2, 0.6) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);

	// Seam-side adjacency.  Collect, for every position index, the per-corner
	// UVs it carries.  A seam position is one that appears with U == a or
	// U == b; because both sides of the seam share one position index, such an
	// index must carry both representatives, and the two substitution points
	// must agree within seamTolerance.
	std::map<int, std::set<std::pair<double, double>>> positionUVs;
	std::map<std::pair<int, int>, int> edgeUse;
	for (std::size_t f = 0; f < mesh.Faces.size(); ++f)
	{
		for (int k = 0; k < 3; ++k)
		{
			const int position = mesh.Faces[f][k];
			const UV& uv = mesh.UVs[mesh.UVIndices[3 * f + k]];
			positionUVs[position].insert(std::make_pair(uv.GetU(), uv.GetV()));
			const int next = mesh.Faces[f][(k + 1) % 3];
			edgeUse[std::make_pair(std::min(position, next), std::max(position, next))] += 1;
		}
	}

	int sharedSeamPositions = 0;
	for (const auto& entry : positionUVs)
	{
		bool hasA = false;
		bool hasB = false;
		double vA = 0.0;
		double vB = 0.0;
		for (const auto& uv : entry.second)
		{
			if (std::fabs(uv.first - a) <= 1e-9)
			{
				hasA = true;
				vA = uv.second;
			}
			if (std::fabs(uv.first - b) <= 1e-9)
			{
				hasB = true;
				vB = uv.second;
			}
		}
		if (!(hasA && hasB))
		{
			continue;
		}
		++sharedSeamPositions;
		SOFT_CHECK(std::fabs(vA - vB) <= 1e-12);
		const XYZ onA = NurbsSurface::GetPointOnSurface(surface, UV(a, vA));
		const XYZ onB = NurbsSurface::GetPointOnSurface(surface, UV(b, vB));
		SOFT_CHECK(onA.Distance(onB) <= options.seamTolerance);
		// The shared vertex uses the a-side representative.
		SOFT_CHECK(onA.Distance(mesh.Vertices[entry.first]) <= 1e-9);
		SOFT_CHECK(onA.Distance(mesh.Vertices[entry.first]) <= onB.Distance(mesh.Vertices[entry.first]) + 1e-18);
	}
	SOFT_CHECK(sharedSeamPositions > 0);

	// Every edge whose two endpoints lie on a seam line is an interior mesh
	// edge (shared by exactly two triangles) and its edge midpoint agrees on
	// both sides of the seam within seamTolerance.
	auto seamV = [&positionUVs, a, b](int position, double& value) -> bool
	{
		const auto found = positionUVs.find(position);
		if (found == positionUVs.end())
		{
			return false;
		}
		for (const auto& uv : found->second)
		{
			if (std::fabs(uv.first - a) <= 1e-9 || std::fabs(uv.first - b) <= 1e-9)
			{
				value = uv.second;
				return true;
			}
		}
		return false;
	};
	int interiorSeamEdges = 0;
	for (const auto& entry : edgeUse)
	{
		double v0 = 0.0;
		double v1 = 0.0;
		if (!seamV(entry.first.first, v0) || !seamV(entry.first.second, v1))
		{
			continue;
		}
		SOFT_CHECK(entry.second == 2);
		++interiorSeamEdges;
		const double midpointV = 0.5 * (v0 + v1);
		const XYZ onA = NurbsSurface::GetPointOnSurface(surface, UV(a, midpointV));
		const XYZ onB = NurbsSurface::GetPointOnSurface(surface, UV(b, midpointV));
		SOFT_CHECK(onA.Distance(onB) <= options.seamTolerance);
	}
	SOFT_CHECK(interiorSeamEdges > 0);

	TASK_CHECK(g_ok);
	return 0;
}

static int RunSeamHoleCrossesSeam()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(-0.3, 0.1), UV(0.5, 0.1), UV(0.5, 0.9), UV(-0.3, 0.9) });
	loops.push_back({ UV(-0.05, 0.4), UV(-0.05, 0.6), UV(0.05, 0.6), UV(0.05, 0.4) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);

	// The hole lies across the seam: no triangle centroid may fall inside any
	// periodic copy of the hole.
	int insideHole = 0;
	const std::vector<UFace> faces = Unwrap(mesh, 1.0);
	for (const UFace& face : faces)
	{
		const UV centroid((face.uv[0].GetU() + face.uv[1].GetU() + face.uv[2].GetU()) / 3.0,
			(face.uv[0].GetV() + face.uv[1].GetV() + face.uv[2].GetV()) / 3.0);
		for (long long shift = -2; shift <= 2; ++shift)
		{
			std::vector<UV> shifted;
			for (const UV& uv : loops[1])
			{
				shifted.push_back(UV(uv.GetU() + static_cast<double>(shift), uv.GetV()));
			}
			if (PointInLoop(centroid, shifted))
			{
				++insideHole;
			}
		}
	}
	SOFT_CHECK(insideHole == 0);
	TASK_CHECK(g_ok);
	return 0;
}

static int RunSeamTangentThroughContourVertex()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	// Two loop vertices lie exactly on the seam U = a; the region touches but
	// does not cross it.
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.0, 0.2), UV(0.5, 0.2), UV(0.5, 0.7), UV(0.0, 0.7) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);

	int seamCorners = 0;
	for (const UV& uv : mesh.UVs)
	{
		if (std::fabs(uv.GetU()) <= 1e-9)
		{
			++seamCorners;
		}
	}
	SOFT_CHECK(seamCorners > 0);
	TASK_CHECK(g_ok);
	return 0;
}

static int RunSeamPeriodShiftInvariance()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.1, 0.2), UV(0.6, 0.2), UV(0.6, 0.7), UV(0.1, 0.7) });
	loops.push_back({ UV(0.3, 0.35), UV(0.3, 0.45), UV(0.4, 0.45), UV(0.4, 0.35) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh base = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, base);

	for (long long shift = -2; shift <= 2; ++shift)
	{
		if (shift == 0)
		{
			continue;
		}
		std::vector<std::vector<UV>> shifted;
		for (const auto& loop : loops)
		{
			std::vector<UV> copy;
			for (const UV& uv : loop)
			{
				copy.push_back(UV(uv.GetU() + static_cast<double>(shift), uv.GetV()));
			}
			shifted.push_back(copy);
		}
		const LN_Mesh shiftedMesh = BuildOrFail(surface, shifted, options);
		SOFT_CHECK(shiftedMesh.Faces.size() == base.Faces.size());
		SOFT_CHECK(shiftedMesh.Vertices.size() == base.Vertices.size());
		double maximumDistance = 0.0;
		if (shiftedMesh.Vertices.size() == base.Vertices.size())
		{
			for (std::size_t i = 0; i < base.Vertices.size(); ++i)
			{
				maximumDistance = std::max(maximumDistance, base.Vertices[i].Distance(shiftedMesh.Vertices[i]));
			}
		}
		SOFT_CHECK(maximumDistance <= 1e-9);
	}

	// Repeating the same input must produce the same output order.
	const LN_Mesh repeat = BuildOrFail(surface, loops, options);
	SOFT_CHECK(repeat.Faces == base.Faces);
	SOFT_CHECK(repeat.UVIndices == base.UVIndices);
	SOFT_CHECK(repeat.UVs.size() == base.UVs.size());

	TASK_CHECK(g_ok);
	return 0;
}

static int RunSeamMismatchIsRuntimeError()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(-0.2, 0.2), UV(0.3, 0.2), UV(0.3, 0.6), UV(-0.2, 0.6) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-300;
	options.maxDepth = 6;

	SOFT_CHECK(ThrowsRuntime(surface, loops, options));
	TASK_CHECK(g_ok);
	return 0;
}

// ---------------------------------------------------------------------------
// refinement
// ---------------------------------------------------------------------------

static int RunRefinementMaxDepthZero()
{
	g_ok = true;
	// A small region whose initial pieces are already within a loose tolerance
	// must be emitted at depth 0 without any refinement.
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);

	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.1, 0.2), UV(0.2, 0.2), UV(0.2, 0.4), UV(0.1, 0.4) });

	TrimMeshOptions options;
	options.chordTolerance = 1.0;
	options.seamTolerance = 1e-9;
	options.maxDepth = 0;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);
	SOFT_CHECK(!mesh.Faces.empty());

	// Tightening the tolerance with depth 0 must fail instead of returning a
	// half mesh.
	TrimMeshOptions tight = options;
	tight.chordTolerance = 1e-7;
	SOFT_CHECK(ThrowsRuntime(surface, loops, tight));
	TASK_CHECK(g_ok);
	return 0;
}

static int RunRefinementReachesChordTolerance()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.05, 0.1), UV(0.55, 0.1), UV(0.55, 0.9), UV(0.05, 0.9) });

	TrimMeshOptions coarse;
	coarse.chordTolerance = 1e-1;
	coarse.seamTolerance = 1e-9;
	coarse.maxDepth = 4;

	TrimMeshOptions fine;
	fine.chordTolerance = 1e-3;
	fine.seamTolerance = 1e-9;
	fine.maxDepth = 12;

	const LN_Mesh coarseMesh = BuildOrFail(surface, loops, coarse);
	const LN_Mesh fineMesh = BuildOrFail(surface, loops, fine);
	SOFT_CHECK(coarseMesh.Faces.size() > 0);
	SOFT_CHECK(fineMesh.Faces.size() > coarseMesh.Faces.size());
	SOFT_CHECK(fineMesh.UVs.size() == 3 * fineMesh.Faces.size());
	SOFT_CHECK(fineMesh.UVIndices.size() == 3 * fineMesh.Faces.size());

	// Every fine triangle must satisfy the four chord samples.
	VerifyMesh(surface, loops, fine, fineMesh);
	TASK_CHECK(g_ok);
	return 0;
}

static int RunRefinementDepthExhaustion()
{
	g_ok = true;
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0);
	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.1, 0.1), UV(0.9, 0.1), UV(0.9, 0.9), UV(0.1, 0.9) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-12;
	options.seamTolerance = 1e-9;
	options.maxDepth = 2;

	SOFT_CHECK(ThrowsRuntime(surface, loops, options));
	TASK_CHECK(g_ok);
	return 0;
}

static int RunRefinementDifferentKnotDensity()
{
	g_ok = true;
	// A cylinder whose V direction has an interior knot (degree 1 profile with
	// a knot at 0.5) and whose U knots are remapped to a non-uniform layout.
	std::vector<double> vKnots = { 0, 0, 0.5, 1, 1 };
	std::vector<XYZW> profilePoints = {
		XYZW(2, 0, 0, 1),
		XYZW(1.5, 0, 1.5, 1),
		XYZW(2, 0, 3, 1)
	};
	LN_NurbsSurface surface = MakeCylinder(2.0, 3.0, 0.0, 1.0, &vKnots, &profilePoints);

	// Non-uniform U knot spacing (multiplicity 2, degree 2).
	std::vector<double> remapped = { 0, 0, 0, 0.1, 0.1, 0.35, 0.35, 0.8, 0.8, 1, 1, 1 };
	SOFT_CHECK(remapped.size() == surface.KnotVectorU.size());
	surface.KnotVectorU = remapped;

	std::vector<std::vector<UV>> loops;
	loops.push_back({ UV(0.2, 0.2), UV(0.7, 0.2), UV(0.7, 0.8), UV(0.2, 0.8) });
	loops.push_back({ UV(0.35, 0.4), UV(0.35, 0.5), UV(0.5, 0.5), UV(0.5, 0.4) });

	TrimMeshOptions options;
	options.chordTolerance = 1e-2;
	options.seamTolerance = 1e-9;
	options.maxDepth = 10;

	const LN_Mesh mesh = BuildOrFail(surface, loops, options);
	VerifyMesh(surface, loops, options, mesh);

	// V knot lines at 0.5 and the U node cells must be respected: no triangle
	// may span v = 0.5.
	const std::vector<UFace> faces = Unwrap(mesh, 1.0);
	for (const UFace& face : faces)
	{
		double low = 1e300;
		double high = -1e300;
		for (int k = 0; k < 3; ++k)
		{
			low = std::min(low, face.uv[k].GetV());
			high = std::max(high, face.uv[k].GetV());
		}
		SOFT_CHECK(!(low < 0.5 - 1e-9 && high > 0.5 + 1e-9));
	}
	TASK_CHECK(g_ok);
	return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
	{ "domain", "non_unit_parameter_range", RunDomainNonUnitParameterRange },
	{ "domain", "concave_contour", RunDomainConcaveContour },
	{ "domain", "rejects_bad_input", RunDomainRejectsBadInput },
	{ "seam", "outer_contour_crosses_seam", RunSeamOuterContourCrossesSeam },
	{ "seam", "hole_crosses_seam", RunSeamHoleCrossesSeam },
	{ "seam", "tangent_through_contour_vertex", RunSeamTangentThroughContourVertex },
	{ "seam", "period_shift_invariance", RunSeamPeriodShiftInvariance },
	{ "seam", "mismatch_is_runtime_error", RunSeamMismatchIsRuntimeError },
	{ "refinement", "max_depth_zero", RunRefinementMaxDepthZero },
	{ "refinement", "reaches_chord_tolerance", RunRefinementReachesChordTolerance },
	{ "refinement", "depth_exhaustion", RunRefinementDepthExhaustion },
	{ "refinement", "different_knot_density", RunRefinementDifferentKnotDensity },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
