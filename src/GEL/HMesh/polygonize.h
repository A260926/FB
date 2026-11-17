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
     \brief Statistics gathered by the sparse (HGrid) isosurface extraction.

     A fresh instance is filled in on every call to volume_polygonize: the
     counters are reset at the beginning of the call.

     - visited_voxels is the number of voxels for which a per-voxel face
       determination was actually carried out. Voxels inside a uniform
       (coalesced) brick that is known not to border anything outside are not
       counted. For the dense RGrid entry point the counter is not exposed.
     - sample_reads is the number of scalar samples that were actually read
       from the grid. Reading the single representative value of a uniform
       brick counts as one read (it is not multiplied by the brick volume).
     */
    struct SparsePolygonizeStats
    {
        /** Number of voxels visited by per-voxel face determination. */
        size_t visited_voxels = 0;

        /** Number of scalar samples actually read from the grid. */
        size_t sample_reads = 0;
    };

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
     @brief Computes a polygonal mesh from a sparse volumetric isocontour
     @param xform is the transformation from voxel indices back to the domain of the implicit being polygonized.
     @param grid is the hierarchical (sparse) voxel grid.
     @param tau is the threshold (or isovalue)
     @param make_triangles tells whether we output triangles (default: true) or quads (false)
     @param high_is_inside tells whether values greater than @c tau (defalut: true) are interior or exterior
     @param dual_connectivity should be true if you want dual contouring and fals for marching cubes connectivity
     @param stats optional pointer to a statistics structure. If non-null, the counters are
     reset and populated during this call.
     @returns HMesh::Manifold - the resulting mesh.

     This overload produces exactly the same oriented surface as the dense
     overload would for the same scalar values. It traverses the top level
     bricks of the HGrid. Uniform bricks are examined through their single
     representative value, and only brick boundaries that can border a
     different value are processed voxel by voxel, so the interior of a
     uniform brick is never scanned. A partially covered brick is traversed
     per voxel as usual. After the quads have been generated, the same
     connectivity, vertex projection and triangulation code is used as for
     the dense overload.
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
