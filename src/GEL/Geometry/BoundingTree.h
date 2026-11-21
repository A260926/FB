/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

/**
 * @file BoundingTree.h
 * @brief Template representing a bounding hierarchy.
 */

#ifndef __GEOMETRY_BOUNDINGTREE_H
#define __GEOMETRY_BOUNDINGTREE_H

#include <array>
#include <cstdint>
#include <vector>
#include <GEL/CGLA/Vec3d.h>
#include <GEL/Geometry/BoundingNode.h>
#include <GEL/Geometry/BoundingLNode.h>
#include <GEL/Geometry/BoundingINode.h>
#include <GEL/Geometry/Ray.h>

namespace Geometry
{

/** A single triangle for the double precision surface hierarchy. The
    vertices are kept in double precision; only the bounding boxes of the
    hierarchy use float. The source id must be unique and non-zero. */
struct SurfaceTriangle
{
    uint64_t source_id = 0;
    CGLA::Vec3d v0;
    CGLA::Vec3d v1;
    CGLA::Vec3d v2;

    SurfaceTriangle() = default;
    SurfaceTriangle(uint64_t id, const CGLA::Vec3d& p0,
                    const CGLA::Vec3d& p1, const CGLA::Vec3d& p2):
        source_id(id), v0(p0), v1(p1), v2(p2) {}
};

/// Result of a closest point query on a double precision surface hierarchy.
struct ClosestPointResult
{
    uint64_t source_id = 0;                 ///< source id of the triangle holding the closest point
    CGLA::Vec3d point;                      ///< closest point on the surface (double precision)
    std::array<double, 3> barycentric{};    ///< weights corresponding to v0, v1, v2 of that triangle
    double sq_dist = 0.0;                   ///< squared distance from the query point to point
    std::size_t node_visits = 0;            ///< interior/leaf nodes actually visited
    std::size_t triangle_tests = 0;         ///< leaf triangles actually tested
};

/** Template representing a bounding hierarchy. The argument should be the bounding box type -
    either ABox or OOBox */
template<class BoxType>
class BoundingTree
{
 public:

	typedef BoundingNode<BoxType> Node;
	typedef BoundingLNode<BoxType> LeafNode;
	typedef BoundingINode<BoxType> IntNode;

	Node* root;

 public:

	BoundingTree(): root(0) {}

	~BoundingTree() {delete root;}

    bool empty() const {return root == 0 && surface_root == nullptr;}

	void build(std::vector<Triangle>& triangles);

	bool intersect(const CGLA::Vec3f&,const CGLA::Vec3f&,float&) const;

	void intersect(Ray& r) const;

	int intersect_cnt(const CGLA::Vec3f&,const CGLA::Vec3f&) const;

	float compute_signed_distance(const CGLA::Vec3f& p,	float=FLT_MAX) const;

	/** @name Double precision surface support (AABox only).
	    The leaves retain the original double geometry and a user supplied
	    source id; float bounding boxes are only used to prune the traversal
	    and are inflated conservatively. */
	//@{

	/// Build a surface hierarchy. Replaces the tree only on success.
	/// @throws std::invalid_argument on a zero/duplicate id, a non-finite
	/// vertex or a degenerate triangle.
	void build_surface(std::vector<SurfaceTriangle> triangles);

	/** Find the closest point of the surface to @p p. On a tie of squared
	    distance the triangle with the smaller source id wins. Returns false
	    (without touching @p witness) when the tree is empty. Throws
	    std::invalid_argument for a non-finite query point. */
	bool closest_point(const CGLA::Vec3d& p, ClosestPointResult& witness) const;

	//@}

 private:

	// Double precision surface hierarchy, kept separate from the float
	// Triangle path so the legacy OBox/AABox behaviour is unchanged.
	struct SurfaceNode
	{
		CGLA::Vec3f box_min;
		CGLA::Vec3f box_max;
		SurfaceNode* left = nullptr;
		SurfaceNode* right = nullptr;
		std::size_t leaf = static_cast<std::size_t>(-1); // index into surface_triangles, leaves only

		SurfaceNode() = default;
		~SurfaceNode() { delete left; delete right; }

		double min_sq_dist(const CGLA::Vec3d& p) const;
	};

	SurfaceNode* surface_root = nullptr;
	std::vector<SurfaceTriangle> surface_triangles;

	SurfaceNode* build_surface_recursive(const std::vector<SurfaceTriangle>& tris,
	                                     std::vector<std::size_t>& work,
	                                     std::size_t begin, std::size_t end);
};

}

#endif
