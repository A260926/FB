// This file is part of meshoptimizer library; see meshoptimizer.h for version/license details
#pragma once

#include <stddef.h>

struct meshopt_SurfaceErrorStats;

namespace meshopt
{

// A single vertex move performed by an edge collapse candidate; wedge linkage is fully expanded by the caller
struct SurfaceErrorMove
{
	unsigned int from;
	unsigned int to;
};

// Upper bound on the number of simultaneous vertex moves a single candidate can perform; wedge loops are small in practice
static const size_t kSurfaceMoveCapacity = 64;

// Maintains the two-sided surface distance certificates for meshopt_simplifyWithSurfaceError
//
// The original surface S is captured by init() and never changes. The active surface T is tracked as a set of stable triangle records
// that are updated as collapses are committed. Both directions of the Hausdorff bound are represented by adaptive triangle-domain
// subdivisions: every leaf cell stores a witness triangle such that all of the cell's points are provably within the accepted bound of
// the witness. Because the distance to a single convex triangle is a convex function, the supremum over a cell is attained at one of the
// cell's three corners, so a leaf is certified by checking its corners against one shared witness. Interior points are covered by this
// convexity argument, not by sampling.
class SurfaceErrorCertifier
{
public:
	SurfaceErrorCertifier(float max_surface_error, unsigned int max_depth);
	~SurfaceErrorCertifier();

	// vertex_positions/sparse_remap describe the (possibly sparse-remapped) vertex array; indices are the initial triangle list
	// position_remap maps each vertex to a canonical vertex with an identical position and is used for degeneracy detection
	void init(const float* vertex_positions, size_t vertex_positions_stride, const unsigned int* sparse_remap, size_t vertex_count, const unsigned int* indices, size_t index_count, const unsigned int* position_remap);

	// rebuilds the active-surface spatial index from live records; called at pass boundaries only
	void reorganize();

	// simulates the complete set of wedge moves for one collapse candidate and commits it if both distance directions can be certified
	// returns false (without changing any committed state) when the candidate must be rejected
	bool simulate(const SurfaceErrorMove* moves, size_t move_count);

	// fills final statistics; certified_error is the conservative two-sided bound for the committed surface
	void finalize(struct meshopt_SurfaceErrorStats* stats);

	// number of live triangles currently tracked (must match the simplifier's result triangle count)
	size_t liveTriangles() const;

private:
	SurfaceErrorCertifier(const SurfaceErrorCertifier&);
	SurfaceErrorCertifier& operator=(const SurfaceErrorCertifier&);

	struct Impl;
	Impl* impl;
};

} // namespace meshopt
