/*
 * Example: triangulate a windowed cylinder and write it to OBJ.
 *
 * The surface is a rational full-turn cylinder built with the existing
 * CreateRevolvedSurface entry point.  The trimmed region is a window that
 * crosses the periodic U seam and contains a circular hole that also crosses
 * it.  LNLib::TrimmedSurfaceMesh::Build produces a conforming mesh whose
 * vertices are shared across the seam and whose per-corner UVs are mapped back
 * into the original effective U domain.
 *
 * The OBJ file writes independent v (position) and vt (texture / parameter)
 * indices, so every triangle can reference the same position from both sides
 * of the seam while using different UV coordinates.
 */

#include "TrimmedSurfaceMesh.h"

#include "NurbsCurve.h"
#include "NurbsSurface.h"
#include "Constants.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <vector>

using namespace LNLib;

namespace
{
	LN_NurbsSurface MakeCylinder(double radius, double height)
	{
		LN_NurbsCurve profile;
		NurbsCurve::CreateLine(XYZ(radius, 0, 0), XYZ(radius, 0, height), profile);

		LN_NurbsSurface surface;
		const bool ok = NurbsSurface::CreateRevolvedSurface(XYZ(0, 0, 0), XYZ(0, 0, 1),
			2.0 * Constants::Pi, profile, surface);
		if (!ok)
		{
			throw std::runtime_error("failed to create the revolved cylinder surface.");
		}
		return surface;
	}

	// A closed regular polygon approximating a circle in the unfolded domain.
	std::vector<UV> MakeCircle(double centreU, double centreV, double radius, int segments, bool clockwise)
	{
		std::vector<UV> loop;
		loop.reserve(static_cast<std::size_t>(segments));
		for (int i = 0; i < segments; ++i)
		{
			const double angle = 2.0 * Constants::Pi * static_cast<double>(i) / static_cast<double>(segments);
			const double u = centreU + radius * std::cos(angle);
			const double v = centreV + radius * std::sin(angle);
			loop.push_back(UV(u, v));
		}
		if (clockwise)
		{
			std::reverse(loop.begin(), loop.end());
		}
		return loop;
	}

	bool WriteObj(const std::string& path, const LN_Mesh& mesh)
	{
		std::ofstream out(path);
		if (!out)
		{
			return false;
		}

		out << "# LNLib trimmed surface mesh\n";
		out << "# vertices: " << mesh.Vertices.size() << ", triangles: " << mesh.Faces.size() << "\n";
		for (const XYZ& vertex : mesh.Vertices)
		{
			out << "v " << vertex.X() << ' ' << vertex.Y() << ' ' << vertex.Z() << "\n";
		}
		for (const UV& uv : mesh.UVs)
		{
			out << "vt " << uv.GetU() << ' ' << uv.GetV() << "\n";
		}
		for (std::size_t f = 0; f < mesh.Faces.size(); ++f)
		{
			const std::vector<int>& face = mesh.Faces[f];
			if (face.size() != 3 || mesh.UVIndices.size() < 3 * (f + 1))
			{
				return false;
			}
			out << "f";
			for (int k = 0; k < 3; ++k)
			{
				const int positionIndex = face[k] + 1;
				const int uvIndex = mesh.UVIndices[3 * f + k] + 1;
				out << ' ' << positionIndex << '/' << uvIndex;
			}
			out << "\n";
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	const std::string path = (argc > 1) ? argv[1] : "trim_mesh.obj";

	try
	{
		const LN_NurbsSurface surface = MakeCylinder(2.0, 3.0);

		// The window spans U in [-0.35, 0.35]; the surface U domain is
		// [0, 1], so the window crosses the seam U = 0 / U = 1.
		std::vector<std::vector<UV>> loops;
		loops.push_back({
			UV(-0.35, 0.1), UV(0.35, 0.1),
			UV(0.35, 0.85), UV(-0.35, 0.85)
		});
		// Circular hole centred on the seam, also crossing it.
		loops.push_back(MakeCircle(0.0, 0.475, 0.18, 24, true));

		TrimMeshOptions options;
		options.chordTolerance = 1e-3;
		options.seamTolerance = 1e-9;
		options.maxDepth = 12;

		const LN_Mesh mesh = TrimmedSurfaceMesh::Build(surface, loops, options);

		if (!WriteObj(path, mesh))
		{
			std::fprintf(stderr, "failed to write %s\n", path.c_str());
			return 1;
		}

		std::printf("trim_mesh: vertices=%zu triangles=%zu uvs=%zu\n",
			mesh.Vertices.size(), mesh.Faces.size(), mesh.UVs.size());
		std::printf("trim_mesh: wrote %s\n", path.c_str());
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "trim_mesh failed: %s\n", error.what());
		return 1;
	}
}
