// Example: join two non-planar rational NURBS surfaces along U with
// NurbsSurface::JoinAlongU and tessellate the joined surface into an OBJ mesh.
//
// The two patches share the straight seam (1, v, 0) in their normalized
// parameters, but the right patch carries the non-constant seam weight
// function wR(v) = 1 + 0.5v while the left seam weight function is 1, so the
// join has to run on a common-denominator representation.
//
// Usage: join_surface <output.obj>

#include "LNObject.h"
#include "NurbsSurface.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <fstream>
#include <iostream>
#include <vector>

using namespace LNLib;

namespace
{
	// Left patch: degreeU=2 on [0,1], degreeV=2 on [-2,4] with one interior knot.
	// Non-planar (z bumps) and rational (varying positive weights away from the seam).
	LN_NurbsSurface BuildLeft()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 2;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 0, 0, 0, 1, 1, 1 };
		surface.KnotVectorV = { -2, -2, -2, 0, 4, 4, 4 };
		// Greville abscissae of the normalized V knot vector [0,0,0,1/3,1,1,1].
		const double greville[4] = { 0.0, 1.0 / 6.0, 2.0 / 3.0, 1.0 };
		surface.ControlPoints.assign(3, std::vector<XYZW>(4));
		for (int i = 0; i < 3; ++i)
		{
			double x = 0.5 * i;
			for (int j = 0; j < 4; ++j)
			{
				double y = greville[j];
				double z = (1.0 - x) * (0.25 + 0.1 * j);
				double w = (i < 2) ? (0.9 + 0.3 * i + 0.1 * j) : 1.0;
				surface.ControlPoints[i][j] = XYZW(x * w, y * w, z * w, w);
			}
		}
		return surface;
	}

	// Right patch: degreeU=3 on [10,20], degreeV=2 on [5,9].
	// Its seam row is the homogeneous form of (1, v, 0) with weight 1 + 0.5v.
	LN_NurbsSurface BuildRight()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 3;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 10, 10, 10, 10, 20, 20, 20, 20 };
		surface.KnotVectorV = { 5, 5, 5, 9, 9, 9 };
		surface.ControlPoints.assign(4, std::vector<XYZW>(3));
		surface.ControlPoints[0][0] = XYZW(1.0, 0.0, 0.0, 1.0);
		surface.ControlPoints[0][1] = XYZW(1.25, 0.5, 0.0, 1.25);
		surface.ControlPoints[0][2] = XYZW(1.5, 1.5, 0.0, 1.5);
		for (int i = 1; i < 4; ++i)
		{
			double x = 1.0 + i / 3.0;
			for (int j = 0; j < 3; ++j)
			{
				double y = 0.5 * j;
				double z = (x - 1.0) * (0.2 + 0.15 * j);
				double w = 0.7 + 0.25 * i + 0.1 * j;
				surface.ControlPoints[i][j] = XYZW(x * w, y * w, z * w, w);
			}
		}
		return surface;
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: join_surface <output.obj>\n";
		return 2;
	}

	LN_NurbsSurface left = BuildLeft();
	LN_NurbsSurface right = BuildRight();

	LN_NurbsSurface joined;
	if (!NurbsSurface::JoinAlongU(left, right, false, 1e-10, joined))
	{
		std::cerr << "the two surfaces do not share a compatible seam\n";
		return 1;
	}

	const int samplesU = 81;
	const int samplesV = 41;

	std::ofstream obj(argv[1]);
	if (!obj)
	{
		std::cerr << "cannot open output file: " << argv[1] << "\n";
		return 1;
	}

	// Evaluate the joined surface with the standard point evaluator on a grid
	// spanning the seam (u in [0,2]).
	for (int i = 0; i < samplesU; ++i)
	{
		double u = 2.0 * i / (samplesU - 1);
		for (int j = 0; j < samplesV; ++j)
		{
			double v = static_cast<double>(j) / (samplesV - 1);
			XYZ point = NurbsSurface::GetPointOnSurface(joined, UV(u, v));
			obj << "v " << point.GetX() << " " << point.GetY() << " " << point.GetZ() << "\n";
		}
	}
	for (int i = 0; i + 1 < samplesU; ++i)
	{
		for (int j = 0; j + 1 < samplesV; ++j)
		{
			int a = i * samplesV + j + 1;
			int b = (i + 1) * samplesV + j + 1;
			int c = (i + 1) * samplesV + j + 2;
			int d = i * samplesV + j + 2;
			obj << "f " << a << " " << b << " " << c << "\n";
			obj << "f " << a << " " << c << " " << d << "\n";
		}
	}

	int vertices = samplesU * samplesV;
	int triangles = 2 * (samplesU - 1) * (samplesV - 1);
	std::cout << "joined degreeU=" << joined.DegreeU << " degreeV=" << joined.DegreeV << "\n";
	std::cout << "vertices=" << vertices << " triangles=" << triangles << "\n";
	return 0;
}
