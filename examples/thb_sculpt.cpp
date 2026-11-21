/*
 * thb_sculpt: localized sculpting on a truncated hierarchical NURBS surface.
 *
 * Builds an open-clamped rational NURBS surface, wraps it in a ThbSurface,
 * performs a non-rectangular two-level refinement, edits one local
 * homogeneous control coefficient, and tessellates the hierarchy directly
 * through the hierarchical Triangulate overload into a Wavefront OBJ file
 * (v / vt / f records).
 *
 * Usage: thb_sculpt <output.obj>
 */

#include "ThbSurface.h"
#include "NurbsSurface.h"
#include "LNObject.h"
#include "XYZW.h"
#include "UV.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

using namespace LNLib;

namespace
{
	LN_NurbsSurface createSculptSurface()
	{
		// Degree (3, 2), nonuniform simple interior knots, non-unit domain and
		// non-constant positive weights.
		LN_NurbsSurface surface;
		surface.DegreeU = 3;
		surface.DegreeV = 2;
		const std::vector<double> distinctU = { 0.0, 0.3, 0.65, 1.0 };
		const std::vector<double> distinctV = { 0.0, 0.45, 1.0 };
		surface.KnotVectorU = { 0, 0, 0, 0, 0.3, 0.65, 1, 1, 1, 1 };
		surface.KnotVectorV = { 0, 0, 0, 0.45, 1, 1, 1 };

		const int controlU = static_cast<int>(distinctU.size()) + surface.DegreeU - 1; // 6
		const int controlV = static_cast<int>(distinctV.size()) + surface.DegreeV - 1; // 4
		for (int i = 0; i < controlU; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < controlV; ++j)
			{
				double x = 1.2 * i;
				double y = 1.5 * j;
				double z = 0.35 * i * i - 0.28 * j * j + 0.18 * i * j;
				double weight = 0.85 + 0.06 * i + 0.09 * j; // strictly positive
				row.emplace_back(XYZ(x, y, z), weight);
			}
			surface.ControlPoints.push_back(row);
		}
		NurbsSurface::Check(surface);
		return surface;
	}

	void writeObj(const std::string& path, const LN_Mesh& mesh,
	              double minU, double maxU, double minV, double maxV)
	{
		std::FILE* file = std::fopen(path.c_str(), "w");
		if (file == nullptr)
		{
			throw std::runtime_error("thb_sculpt: cannot open output file: " + path);
		}
		std::fprintf(file, "# Truncated hierarchical NURBS sculpt sample\n");
		std::fprintf(file, "# vertices=%zu triangles=%zu\n", mesh.Vertices.size(), mesh.Faces.size());
		for (const XYZ& p : mesh.Vertices)
		{
			std::fprintf(file, "v %.10g %.10g %.10g\n", p.X(), p.Y(), p.Z());
		}
		for (const UV& uv : mesh.UVs)
		{
			double tu = (uv.GetU() - minU) / (maxU - minU);
			double tv = (uv.GetV() - minV) / (maxV - minV);
			std::fprintf(file, "vt %.10g %.10g\n", tu, tv);
		}
		for (const std::vector<int>& face : mesh.Faces)
		{
			// OBJ indices are 1-based and reference matching v/vt records.
			std::fprintf(file, "f %d/%d %d/%d %d/%d\n",
			             face[0] + 1, face[0] + 1,
			             face[1] + 1, face[1] + 1,
			             face[2] + 1, face[2] + 1);
		}
		std::fclose(file);
	}
}

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::fprintf(stderr, "usage: %s <output.obj>\n", argv[0]);
		return 2;
	}
	try
	{
		LN_NurbsSurface source = createSculptSurface();
		ThbSurface hierarchy(source, 6);

		// Non-rectangular two-level refinement.  First an L-shaped cluster of
		// level-0 leaves, then a single level-1 child for extra local depth.
		hierarchy.Refine({ {0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 0, 1} });
		hierarchy.Refine({ {1, 0, 0} });

		// Local coefficient edit on a fine active key; the change stays inside
		// the support of that truncated basis function.
		ThbKey sculptKey{ 1, 1, 1 };
		XYZW original = hierarchy.Coefficient(sculptKey);
		hierarchy.SetCoefficient(sculptKey,
		                         XYZW(original.WX(), original.WY(), original.WZ() + 2.5,
		                              original.W()));

		const int resolutionU = 24;
		const int resolutionV = 18;
		LN_Mesh mesh = NurbsSurface::Triangulate(hierarchy, resolutionU, resolutionV);
		writeObj(argv[1], mesh,
		         hierarchy.MinU(), hierarchy.MaxU(),
		         hierarchy.MinV(), hierarchy.MaxV());

		std::printf("thb_sculpt leaves=%zu active=%zu vertices=%zu triangles=%zu -> %s\n",
		            hierarchy.Leaves().size(), hierarchy.Active().size(),
		            mesh.Vertices.size(), mesh.Faces.size(), argv[1]);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "thb_sculpt failed: %s\n", e.what());
		return 1;
	}
}
