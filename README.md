The **_Visualization and Computer Graphics Library_** (VCGlib for short) is an open source, portable, and templated library written in C++, with no external dependencies, for manipulation, processing, cleaning, and simplifying triangle meshes.

[![BuildExamples](https://github.com/cnr-isti-vclab/vcglib/actions/workflows/BuildExamples.yml/badge.svg)](https://github.com/cnr-isti-vclab/vcglib/actions/workflows/BuildExamples.yml)

The library, composed by more than 100k lines of code, is released under the GPL license, and it is the base of most of the software tools of the [Visual Computing Lab](http://vcg.isti.cnr.it) of the Italian National Research Council Institute - ISTI, like [MeshLab](http://www.meshlab.net/), [Metro](http://vcg.isti.cnr.it/vcglib/metro.html) and many others.

The VCG library is tailored to mostly manage triangular meshes: The library is fairly large and offers many state-of-the-art capabilities for processing meshes, such as:

- high quality quadric-error edge-collapse based simplfication
- efficient spatial query structures (uniform grids, hashed grids, kdtree, etc)
- advanced smoothing and fairing algorithms
- computation of curvature
- optimization of texture coordinates
- Hausdorff distance computation
- geodesic paths
- mesh repairing capabilities
- isosurface extraction and advancing front meshing algorithms
- Poisson Disk sampling and other tools to sample point distributions over meshes
- subdivision surfaces

## Notable Applications

A number of applications have been developed using the VCGlib:

- MeshLab: the renowed open source mesh processing software
- Metro, the tool for measuring differences between meshes
- The first high quality out-of-core mesh simplifier that was used by the Stanford Digital Michelangelo project to process their huge 3D scanned models.

## Contacts

### Tufted intrinsic Laplacian for non manifold triangle meshes

`vcg::tri::TuftedOperator<Mesh>` (see
`vcg/complex/algorithms/tufted_laplacian.h`) provides an owning Laplacian
operator for compact triangle meshes that may carry non manifold edges,
boundaries, multiple connected components, inconsistent winding or face fans
touching at a single point.

It differs from the ordinary spatial cotangent harmonic operator
(`vcg::tri::Harmonic`, which walks face-face adjacency and can therefore
produce negative boundary weights):

- every input triangle is duplicated into a front and a back logical copy and
  the copies are stitched into an intrinsic *tufted cover*, so every cover
  edge has exactly two incident faces without welding, removing or repairing
  any input face; spatially coincident vertices keep distinct identities;
- intrinsic Delaunay edge flips are run on that cover, with new diagonal
  lengths obtained by unfolding the two adjacent triangles. Edge identity,
  halfedge pairing and the mapping back to the original vertices survive the
  flips; parallel cover edges are never merged and intrinsic self loops keep
  a positive length;
- the cotangent stiffness matrix and the barycentric (area thirds) mass
  matrix are gathered back into the original vertex space and divided by two,
  so the mass of every connected component equals its original 3D area, the
  stiffness is invariant under uniform scaling and the mass scales with the
  area;
- weights are never clamped and uniform graph weights are never used: if the
  intrinsic flow stalls, construction fails explicitly.

The operator is constructed from a const mesh, keeps its own independent copy
of the cover, never modifies the source mesh and can be reused with different
constraints. `vcg::tri::Harmonic<Mesh,double>::ComputeScalarField` has an
overload taking a prebuilt operator that imposes Dirichlet constraints by
exact elimination over the real connected components of the mesh. The
original four parameter / biharmonic entry point and its spatial cotangent
behavior are unchanged. A usage example lives in
`examples/tufted_laplacian.cpp`.

For any info about licensing (portions of) the library please contact us:
Paolo Cignoni (p.cignoni@isti.cnr.it)
Visual Computing Lab of the Italian National Research Council - ISTI

In case of bugs please report them [here](https://github.com/cnr-isti-vclab/vcglib/issues).
