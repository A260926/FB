/*
 * Runnable example for LNLib::NurbsSurface::JoinAlongU.
 *
 * Two non-planar rational NURBS surfaces are welded along a common spatial
 * seam whose homogeneous weight functions differ (a non-constant ratio). The
 * joined surface is evaluated only through the new entry and tessellated into
 * a single cross-seam OBJ mesh.
 *
 * Usage: join_example <output.obj>
 */

#include "NurbsSurface.h"
#include "NurbsCurve.h"
#include "MathUtils.h"
#include "XYZW.h"
#include "UV.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace LNLib;

namespace
{
	XYZW HomogeneousProduct(const XYZW& a, const XYZW& b)
	{
		return XYZW(a.GetWX() * b.GetWX(), a.GetWY() * b.GetWY(),
			a.GetWZ() * b.GetWZ(), a.GetW() * b.GetW());
	}

	std::vector<XYZW> BezierMultiply(const std::vector<XYZW>& a, const std::vector<XYZW>& b)
	{
		int degreeA = static_cast<int>(a.size()) - 1;
		int degreeB = static_cast<int>(b.size()) - 1;
		int degreeResult = degreeA + degreeB;
		std::vector<XYZW> result(degreeResult + 1, XYZW(0.0, 0.0, 0.0, 0.0));
		for (int k = 0; k <= degreeResult; ++k)
		{
			for (int j = std::max(0, k - degreeB); j <= std::min(degreeA, k); ++j)
			{
				double alpha = MathUtils::Binomial(degreeA, j) *
					MathUtils::Binomial(degreeB, k - j) /
					MathUtils::Binomial(degreeResult, k);
				result[k] += alpha * HomogeneousProduct(a[j], b[k - j]);
			}
		}
		return result;
	}

	// Refine every interior knot to full multiplicity, then read off Bezier
	// segments directly from the refined control polygon.
	std::vector<std::vector<XYZW>> ExtractBezierSegments(const LN_NurbsCurve& curve)
	{
		int degree = curve.Degree;
		const std::vector<double>& knots = curve.KnotVector;
		std::vector<double> inserts;
		int distinctInterior = 0;
		for (size_t i = static_cast<size_t>(degree + 1); i < knots.size() - (degree + 1);)
		{
			++distinctInterior;
			double knot = knots[i];
			size_t j = i + 1;
			while (j < knots.size() - (degree + 1) && knots[j] == knot) ++j;
			int multiplicity = static_cast<int>(j - i);
			for (int k = 0; k < degree - multiplicity; ++k) inserts.push_back(knot);
			i = j;
		}
		LN_NurbsCurve refined = curve;
		if (!inserts.empty())
		{
			LN_NurbsCurve temporary;
			NurbsCurve::RefineKnotVector(curve, inserts, temporary);
			refined = temporary;
		}
		int segmentCount = distinctInterior + 1;
		std::vector<std::vector<XYZW>> segments(segmentCount);
		for (int s = 0; s < segmentCount; ++s)
		{
			for (int k = 0; k <= degree; ++k)
			{
				segments[s].push_back(refined.ControlPoints[s * degree + k]);
			}
		}
		return segments;
	}

	// Left patch: U degree 2, V degree 2 with one interior break, rational and
	// strongly non-planar. Its max-U edge is the shared seam.
	LN_NurbsSurface MakeLeft()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 2;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 0.0, 0.0, 0.0, 1.0, 1.0, 1.0 };
		surface.KnotVectorV = { 0.0, 0.0, 0.0, 0.5, 1.0, 1.0, 1.0 };

		const double seamWeights[3] = { 1.0, 1.8, 0.7 };
		for (int i = 0; i < 3; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < 4; ++j)
			{
				double v = j / 3.0;
				XYZ point(0.8 * i,
					v,
					0.6 * std::sin(1.3 * i + 2.0 * v) + 0.25 * i * v);
				double weight = (i == 2) ? seamWeights[std::min(j, 2)] : (0.8 + 0.1 * j);
				row.emplace_back(XYZW(point, weight));
			}
			surface.ControlPoints.push_back(row);
		}
		return surface;
	}

	// Right patch: U degree 3, V degree 3. Its min-U edge represents the same
	// Euclidean seam but with a genuinely different, non-constant weight
	// function c(v) = 1.2 + 0.9v multiplied into the left homogeneous seam.
	LN_NurbsSurface MakeRight(const LN_NurbsSurface& left)
	{
		LN_NurbsCurve seam;
		seam.Degree = left.DegreeV;
		seam.KnotVector = left.KnotVectorV;
		for (const XYZW& point : left.ControlPoints.back())
		{
			seam.ControlPoints.push_back(point);
		}

		std::vector<std::vector<XYZW>> beziers = ExtractBezierSegments(seam);
		std::vector<double> distinct;
		for (double knot : seam.KnotVector)
		{
			if (distinct.empty() || distinct.back() != knot) distinct.push_back(knot);
		}

		std::vector<XYZW> merged;
		std::vector<double> breaks;
		const int resultDegree = seam.Degree + 1;
		for (int s = 0; s < static_cast<int>(beziers.size()); ++s)
		{
			double v0 = distinct[s];
			double v1 = distinct[s + 1];
			double c0 = 1.2 + 0.9 * v0;
			double c1 = 1.2 + 0.9 * v1;
			std::vector<XYZW> scalar = {
				XYZW(c0, c0, c0, c0), XYZW(c1, c1, c1, c1)
			};
			std::vector<XYZW> product = BezierMultiply(beziers[s], scalar);
			if (s == 0)
			{
				merged = product;
			}
			else
			{
				breaks.push_back(v0);
				for (int k = 1; k <= resultDegree; ++k) merged.push_back(product[k]);
			}
		}

		LN_NurbsSurface right;
		right.DegreeU = 3;
		right.DegreeV = resultDegree;
		right.KnotVectorU = { 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0 };
		for (int i = 0; i <= resultDegree; ++i) right.KnotVectorV.push_back(0.0);
		for (double breakValue : breaks)
		{
			for (int i = 0; i < resultDegree; ++i) right.KnotVectorV.push_back(breakValue);
		}
		for (int i = 0; i <= resultDegree; ++i) right.KnotVectorV.push_back(1.0);

		int rows = static_cast<int>(right.KnotVectorU.size()) - right.DegreeU - 1;
		int columns = static_cast<int>(merged.size());
		right.ControlPoints.assign(rows, std::vector<XYZW>(columns));
		for (int j = 0; j < columns; ++j)
		{
			right.ControlPoints[0][j] = merged[j];
		}
		for (int i = 1; i < rows; ++i)
		{
			for (int j = 0; j < columns; ++j)
			{
				double v = static_cast<double>(j) / (columns - 1);
				XYZ point(2.4 + 0.7 * i,
					v,
					0.5 * std::cos(1.1 * i + 1.7 * v) - 0.2 * i * v);
				right.ControlPoints[i][j] = XYZW(point, 0.9 + 0.05 * j);
			}
		}
		return right;
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: " << argv[0] << " <output.obj>\n";
		return 2;
	}

	LN_NurbsSurface left = MakeLeft();
	LN_NurbsSurface right = MakeRight(left);

	LN_NurbsSurface joined;
	constexpr double tolerance = 1e-10;
	bool ok = NurbsSurface::JoinAlongU(left, right, false, tolerance, joined);
	if (!ok)
	{
		std::cerr << "join failed: seams are not compatible within tolerance\n";
		return 1;
	}

	const int divisionsU = 48;
	const int divisionsV = 24;
	std::vector<XYZ> vertices;
	vertices.reserve((divisionsU + 1) * (divisionsV + 1));
	for (int i = 0; i <= divisionsU; ++i)
	{
		double u = 2.0 * i / divisionsU;
		for (int j = 0; j <= divisionsV; ++j)
		{
			double v = static_cast<double>(j) / divisionsV;
			vertices.push_back(NurbsSurface::GetPointOnSurface(joined, UV(u, v)));
		}
	}

	std::ofstream obj(argv[1]);
	if (!obj)
	{
		std::cerr << "cannot open output file: " << argv[1] << "\n";
		return 1;
	}
	obj << "# joined rational NURBS surface across u=1\n";
	for (const XYZ& point : vertices)
	{
		obj << "v " << point.GetX() << ' ' << point.GetY() << ' ' << point.GetZ() << '\n';
	}

	const int stride = divisionsV + 1;
	int triangleCount = 0;
	for (int i = 0; i < divisionsU; ++i)
	{
		for (int j = 0; j < divisionsV; ++j)
		{
			int a = i * stride + j + 1;
			int b = (i + 1) * stride + j + 1;
			int c = i * stride + (j + 1) + 1;
			int d = (i + 1) * stride + (j + 1) + 1;
			obj << "f " << a << ' ' << b << ' ' << c << '\n';
			obj << "f " << b << ' ' << d << ' ' << c << '\n';
			triangleCount += 2;
		}
	}

	std::cout << "vertices=" << vertices.size() << " triangles=" << triangleCount << '\n';
	std::cout << "wrote " << argv[1] << '\n';
	return 0;
}
