/*
 * Trimmed surface triangulation for LNLib.
 *
 * Builds a conforming triangular mesh of the region delimited by an outer
 * loop (counter-clockwise) and any number of hole loops (clockwise).  The
 * loops are given in an unfolded UV domain: every loop edge is a straight
 * segment in that domain and the U coordinate of a loop may lie outside the
 * effective U domain of the surface.  The surface U direction is spatially
 * closed, so the region may cross the periodic seam.
 */

#pragma once

#include "LNLibDefinitions.h"
#include "LNObject.h"
#include <vector>

namespace LNLib
{
	struct LNLIB_EXPORT TrimMeshOptions
	{
		/// <summary>
		/// Maximum distance between the surface point and the linear triangle
		/// interpolation at the three edge midpoints and the centroid of every
		/// output triangle (in the unfolded parameter domain).
		/// </summary>
		double chordTolerance = 1e-3;

		/// <summary>
		/// Maximum distance between the two periodic representatives of a point
		/// that lies exactly on the U seam (U = a + k * P).
		/// </summary>
		double seamTolerance = 1e-9;

		/// <summary>
		/// Maximum adaptive subdivision depth of an initial constrained triangle.
		/// </summary>
		unsigned maxDepth = 12;
	};

	class LNLIB_EXPORT TrimmedSurfaceMesh
	{
	public:
		/// <summary>
		/// Triangulate the trimmed region of a clamped tensor-product NURBS
		/// surface.  loops[0] is the counter-clockwise outer contour, all other
		/// loops are clockwise holes.  A loop does not repeat its first point.
		/// Throws std::invalid_argument for unsupported input and
		/// std::runtime_error when a conforming mesh cannot be produced.
		/// </summary>
		static LN_Mesh Build(const LN_NurbsSurface& surface,
			const std::vector<std::vector<UV>>& loops,
			const TrimMeshOptions& options = TrimMeshOptions());
	};
}
