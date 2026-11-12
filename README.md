This implementation is 100% Public Domain.

Feel free to use.
 
C++11 is needed to compile it.

Basic usage:

	#include "quickhull/quickhull.hpp"

	using namespace quickhull;
	QuickHull<float> qh; // Could be double as well
	std::vector<Vector3<float>> pointCloud;
	// Add points to point cloud
	...
	auto hull = qh.getConvexHull(pointCloud, true, false);
	const auto& indexBuffer = hull.getIndexBuffer();
	const auto& vertexBuffer = hull.getVertexBuffer();
	// Do what you want with the convex triangle mesh

Vertex data can be passed as a pointer to float/double as long as the data is in X_0,Y_0,Z_0,X_1,Y_1,Z_1,...,X_N,Y_N_Z_N format:

	auto hull = qh.getConvexHull(&pointCloud[0].x, pointCloud.size(), true, false);

The first boolean parameter of getConvexHull specifies whether the resulting mesh should have its triangles in CCW orientation.

The second boolean parameter specifies whether the mesh should use vertex indices of the original point cloud. If it is false, a new vertex buffer is generated which consists only of those vertices that are part of the convex hull. In this case, the new vertex buffer is owned by the returned ConvexHull object. Otherwise, the original point cloud is used as vertex buffer and since the vertices are not copied, make sure you don't call ConvexHull::getVertexBuffer after releasing the memory that contains the original point cloud data.

This implementation is fast, because the convex hull is internally built using a half edge mesh representation which provides quick access to adjacent faces. It is also possible to get the output convex hull as a half edge mesh:

	auto mesh = qh.getConvexHullAsMesh(&pointCloud[0].x, pointCloud.size(), true);

Numerical scale, epsilon and robustness
---------------------------------------

The hull is built entirely from the coordinates you provide; the inputs are
never recentered or rescaled internally. All geometric decisions are made from
coordinate differences, so translating the whole point cloud to a location far
from the origin does not change the result.

Scale and the meaning of epsilon.
The geometric size of a point cloud is the largest of its three axis-aligned
bounding box spans, i.e. max(maxX-minX, maxY-minY, maxZ-minZ). Only coordinate
differences are used, never absolute coordinate values, which is what makes the
size translation invariant. The `eps` argument is a *relative* distance
tolerance: the working tolerance used internally is `epsilon * scale`, a plain
length. Multiplying every coordinate by a constant therefore leaves the
effective tolerance relative to the geometry unchanged, and translating the
cloud leaves it completely unchanged.

Units of the geometric tests.
The three kinds of quantities that appear are kept dimensionally consistent:

* Point-to-line tests use a squared distance (length^2) and are compared
  against `epsilon^2`.
* Point-to-plane tests use the plane residual `N . (Q - P)`, whose unit is
  `|N|` times length. The normalized comparison is against
  `epsilon^2 * |N|^2`, so no division by the normal length is required and
  unnormalized triangle normals can be used directly. Where an actual Euclidean
  distance is needed (the initial tetrahedron's fourth vertex), the residual is
  divided by `|N|` before being compared to `epsilon`.

Translation-safe plane sides.
Every plane stores one of its defining points `P` in addition to its normal.
Side tests evaluate `N . (Q - P)` instead of `N . Q + D`. Written the old way,
`N . Q` and `D = -N . P` are two large world-space terms whose subtraction can
cancel and erase the small local residual when the geometry sits far from the
origin. Subtracting the anchor point first keeps only the local difference. The
same anchored residual is used consistently by the initial simplex, the planar
degenerate branch, face-visibility traversal and outer-point assignment.

Representable range.
Robustness is bounded by what the floating point type can express, not by
distance from the origin. A local feature of size `s` placed near a coordinate
of magnitude `c` is representable only while `s` is comfortably larger than
`c * machineEpsilon` (roughly `c * 2.2e-16` for `double`, `c * 1.2e-7` for
`float`). Inputs whose local detail has already been rounded away by the input
floating point representation cannot be reconstructed. Choose the `float`
versus `double` instantiation and the working magnitude so that the smallest
features remain above this threshold.

Low dimensional and zero size data.
If the points are coincident (zero bounding box span), genuinely collinear, or
genuinely coplanar within the tolerance, the existing low dimensional fallback
is used: a zero-volume triangle mesh is returned using indices into the supplied
vertices (a temporary lift vertex is used internally and never exposed). No new
public vertices are invented. Truly three dimensional clouds are never collapsed
into a zero-thickness shell merely because they are small or far from the
origin, and increasing `eps` is never used to paper over missing faces.

