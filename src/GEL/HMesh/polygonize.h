//
//  polygonize.h
//  PointReconstruction
//
//  Created by J. Andreas Bærentzen on 16/03/13.
//  Copyright (c) 2013 J. Andreas Bærentzen. All rights reserved.
//

#ifndef __PointReconstruction__polygonize__
#define __PointReconstruction__polygonize__

#include <cstddef>
#include <iostream>

#include <GEL/Geometry/XForm.h>
#include <GEL/Geometry/RGrid.h>
#include <GEL/Geometry/HGrid.h>
#include <GEL/HMesh/Manifold.h>

namespace HMesh
{
    /**
     @brief Computes a polygonal mesh from a volumetric isocontour
     @param xform is the transformation from voxel indices back to the domain of the implicit being polygonized.
     @param grid is the voxel grid.
     @param tau is the threshold (or isovalue)
     @param make_triangles tells whether we output triangles (default: true) or quads (false)
     @param high_is_inside tells whether values greater than @c tau (defalut: true) are interior or exterior
     @param dual_connectivity should be true if you want dual contouring and fals for marching cubes connectivity
     @returns HMesh::Manifold - the resulting mesh.

     This function computes an iso surface using the method of dual contouring. For each voxel that is inside,
     it visits the six neighbors and outputs a quad if that neighbor is outside. This leads to a cuberille mesh.
     Afterwards, vertices are  placed on the isocontour by taking the average of the intersections of each of the four
     cube diagonals with the isosurface (approximated via linear interpolation). Dual contouring is very simple and leads
     to bette triangles than marching cubes. On the flip side, the vertex placement is arguably a bit more ad hoc.
     */
    HMesh::Manifold volume_polygonize(const Geometry::XForm& xform,
                                      const Geometry::RGrid<float>& grid,
                                      float tau=0,
                                      bool make_triangles=true,
                                      bool high_is_inside=true,
                                      bool dual_connectivity=false);

    /**
     @brief Tallies of the work performed by the sparse (HGrid) volume_polygonize overload.

     Both counters are reset to zero at the beginning of every call that is passed
     a non-null pointer, and then count what actually happened during that call:
     @c visited_voxels is incremented wherever a voxel is individually examined for
     inside/outside face determination, and @c sample_reads counts the scalar samples
     actually read - reading the single representative value of a uniform (coalesced)
     brick counts as one read, not as one read per voxel in the brick.
     */
    struct SparsePolygonizeStats {
        size_t visited_voxels = 0; ///< Voxels individually tested for face generation.
        size_t sample_reads = 0;   ///< Scalar samples read (uniform brick representative counts once).
    };

    /**
     @brief Sparse counterpart of the RGrid volume_polygonize.
     @param xform is the transformation from voxel indices back to the domain of the implicit being polygonized.
     @param grid is the hierarchical (sparse) voxel grid.
     @param tau is the threshold (or isovalue)
     @param make_triangles tells whether we output triangles (default: true) or quads (false)
     @param high_is_inside tells whether values greater than @c tau (defalut: true) are interior or exterior
     @param dual_connectivity should be true if you want dual contouring and fals for marching cubes connectivity
     @param stats optional (may be null) recipient of a SparsePolygonizeStats work tally.
     @returns HMesh::Manifold - the resulting mesh.

     Produces the same oriented surface as the dense overload for the same scalar field,
     but traverses the top level bricks of the HGrid instead of individual voxels: uniform
     (coalesced) bricks contribute only their boundary faces and are never scanned voxel by
     voxel in their interior, and the grid is not converted to a dense RGrid. Face
     generation is followed by exactly the same connectivity, vertex projection and
     triangulation code as the dense version.
     */
    HMesh::Manifold volume_polygonize(const Geometry::XForm& xform,
                                      const Geometry::HGrid<float>& grid,
                                      float tau=0,
                                      bool make_triangles=true,
                                      bool high_is_inside=true,
                                      bool dual_connectivity=false,
                                      SparsePolygonizeStats* stats=nullptr);
}

#endif /* defined(__PointReconstruction__polygonize__) */

