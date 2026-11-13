..
     Copyright 2024 DreamWorks Animation LLC

     Licensed under the terms set forth in the LICENSE.txt file available at
     https://opensubdiv.org/license.

Crease Sensitivity Table
========================

Overview
--------

When editing a semi-sharp crease it is useful to know, for every refined
vertex, both its subdivided position and the first-order derivative of that
position with respect to the current sharpness of a single coarse crease
edge.  ``Far::CreaseSensitivityTable`` provides these two quantities together
as a pair of sparse stencils over the original coarse control vertices:

* ``weights`` reproduce the ordinary Catmull-Clark subdivision position.
* ``sharpnessWeights`` give the derivative of that position with respect to
  the sharpness of the selected coarse edge.

Both stencils share one sparse support in standard CSR layout.  The rows are
ordered exactly like the vertices produced by
``TopologyRefiner::RefineUniform`` at the requested level, the columns are
coarse vertex indices in increasing order, and duplicate columns are merged.

The derivatives are propagated **analytically** through the uniform creasing
and the smooth/crease/corner blending rules.  No mesh is rebuilt and no finite
differencing is performed.  Once the selected edge's sharpness decays to zero,
later smooth subdivision continues to carry the accumulated geometric
sensitivity, so the derivative field is never cleared simply because the crease
has been exhausted.

Each position row sums to one and each derivative row sums to zero.  Applying
a global translation to the control vertices therefore translates every
refined value while leaving all sharpness derivatives unchanged.

Supported range
---------------

The current implementation supports:

* the Catmull-Clark scheme (``Sdc::SCHEME_CATMARK``);
* uniform creasing (``Sdc::Options::CREASE_UNIFORM``);
* edge-only boundary interpolation
  (``Sdc::Options::VTX_BOUNDARY_EDGE_ONLY``);
* uniform refinement levels 1 through 4;
* oriented manifold face meshes without holes or isolated vertices, with
  faces of 3 to 8 corners (open boundaries and extraordinary vertices are
  allowed);
* zero explicit vertex sharpness; edge sharpness in ``[0, 10]``, where
  ``10`` retains its infinitely-sharp meaning.

The differentiated (selected) coarse edge must be an interior edge with
finite sharpness strictly between ``0`` and ``10`` and at least ``0.05`` away
from any integer.  Neighboring edges may carry other semi-sharp or infinitely
sharp values, including two or three sharp edges meeting at one vertex and
combinations whose sharpness decays in different refinement rounds.

Chaikin creasing, face-varying data and limit-surface derivatives are not
covered.  Any unsupported option, topology, level or sharpness, or an invalid
edge index, causes ``Create`` to throw ``std::invalid_argument``.

The table owns copies of all of its arrays.  It does not retain a reference to
the ``TopologyRefiner`` and remains evaluatable after that refiner has been
destroyed.

Usage example
-------------

The following example builds the table for one interior crease edge and
evaluates positions and sharpness derivatives for interleaved three-channel
control data.  The code is valid C++17 and uses only the CPU Far interface.

.. code-block:: cpp

    #include <opensubdiv/far/topologyDescriptor.h>
    #include <opensubdiv/far/topologyRefinerFactory.h>
    #include <opensubdiv/far/creaseSensitivityTable.h>

    #include <memory>
    #include <vector>

    using namespace OpenSubdiv;

    //
    //  meshCounters/meshVertices describe a manifold Catmull-Clark face mesh.
    //  creasePairs/creaseWeights describe the coarse crease edges as vertex
    //  index pairs and their float sharpness values.
    //
    std::unique_ptr<Far::CreaseSensitivityTable const>
    buildCreaseSensitivity(Far::TopologyDescriptor const& descriptor,
                           int refinementLevel,
                           Far::Index coarseEdgeIndex) {

        Sdc::Options options;
        options.SetVtxBoundaryInterpolation(
            Sdc::Options::VTX_BOUNDARY_EDGE_ONLY);

        std::unique_ptr<Far::TopologyRefiner> refiner(
            Far::TopologyRefinerFactory<Far::TopologyDescriptor>::Create(
                descriptor, {Sdc::SCHEME_CATMARK, options}));

        //  Create() refines internally and leaves the caller's refiner at
        //  its single base level.  The returned table owns all of its data.
        return std::unique_ptr<Far::CreaseSensitivityTable const>(
            Far::CreaseSensitivityTable::Create(
                *refiner, refinementLevel, coarseEdgeIndex));
    }

    void evaluate(Far::CreaseSensitivityTable const& table,
                  std::vector<double> const& controlXYZ) {

        const size_t channels = 3;  // vertex-major, interleaved x/y/z

        std::vector<double> positions;
        std::vector<double> sharpnessDerivatives;
        table.Evaluate(controlXYZ, channels,
                       positions, sharpnessDerivatives);

        //  positions[i*3+c]             refined value for vertex i, channel c
        //  sharpnessDerivatives[i*3+c]  d(value)/d(selected edge sharpness)
    }

The same table can be reused for any control data and any positive channel
count.  The sparse CSR data is also available directly through
``GetNumControlVertices``, ``GetOffsets``, ``GetIndices``, ``GetWeights`` and
``GetSharpnessWeights`` for callers that want to apply the stencils inside
their own data structures.
