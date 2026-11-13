#ifndef		HAS_EDITABLE_OCTREE_H_BEEN_INCLUDED
#define		HAS_EDITABLE_OCTREE_H_BEEN_INCLUDED

#include "mesh.h"

#include <cstddef>
#include <functional>
#include <memory>

#include <glm/glm.hpp>

// ----------------------------------------------------------------------------
//
// EditableOctree keeps the finest-level geometry of a dual-contouring model
// alive for the lifetime of the object.  Density changes are announced with
// Update(), and only the unit cells whose sampling footprint touches the
// update box are rebuilt.  Meshing at a given QEF error threshold is a
// non-destructive operation: it works on a temporary simplified view, so
// switching to a coarse threshold and back to -1 always returns the full,
// fine mesh.
//
// The density callback receives world-space positions.  It must remain valid
// for the lifetime of the EditableOctree and must only return new values
// inside the boxes passed to Update().

struct EditableOctreeStats
{
	EditableOctreeStats()
		: density_calls(0)
		, rebuilt_leaves(0)
	{
	}

	std::size_t	density_calls;
	std::size_t	rebuilt_leaves;
};

// ----------------------------------------------------------------------------

class EditableOctree
{
public:

	typedef EditableOctreeStats Stats;
	typedef EditableOctreeStats UpdateStats;

	// size must be a positive power of two.  The finest cells are 1x1x1.
	EditableOctree(const glm::ivec3& min,
	               int size,
	               std::function<float(const glm::vec3&)> density);
	~EditableOctree();

	EditableOctree(EditableOctree&& other);
	EditableOctree& operator=(EditableOctree&& other);

	// Notifies the tree that the density may have changed inside the closed
	// box [lo, hi].  The caller guarantees that density outside that box is
	// unchanged since the previous construction / update.
	void Update(const glm::vec3& lo, const glm::vec3& hi);

	// Generates the current model.  This never calls the density function.
	// A threshold of -1 (or any negative value) disables simplification.
	void Mesh(float threshold, VertexBuffer& vertexBuffer, IndexBuffer& indexBuffer) const;

	// Statistics for the most recent constructor run or Update() call.
	EditableOctreeStats LastUpdateStats() const;

private:

	EditableOctree(const EditableOctree&);
	EditableOctree& operator=(const EditableOctree&);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// ----------------------------------------------------------------------------

#endif	// HAS_EDITABLE_OCTREE_H_BEEN_INCLUDED
