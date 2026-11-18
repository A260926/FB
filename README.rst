PolyVox - The voxel management and manipulation library
=======================================================
PolyVox is the core technology which lies behind our games. It is a fast, lightweight C++ library for the storage and processing of volumetric (voxel-based) environments. It has applications in both games and medical/scientific visualisation, and is released under the terms of the `MIT license <https://www.tldrlegal.com/l/mit>`_.

PolyVox is a relatively low-level library, and you will need experience in C++ and computer graphics/shaders to use it effectively. It is designed to be easily integrated into existing applications and is independent of your chosen graphics API. For more details please see `this page <http://www.volumesoffun.com/polyvox-about/>`_ on our website.

LodSurface: locally switchable voxel surface resource
-----------------------------------------------------
``include/PolyVox/LodSurface.h`` provides ``PolyVox::LodSurface``, a header-only
resource that turns a scalar field into a crack-free mesh whose resolution can be
switched independently in different regions. It needs only C++14 and the
standard library (no Qt, display or GPU); see
``examples/LodSurfaceHeadless.cpp`` for a runnable demonstration.

Data model
~~~~~~~~~~
* ``LodSurface(const Region& samples, const std::vector<float>& values)`` takes
  the full source field, laid out with ``x`` fastest, then ``y``, then ``z``.
  The count must match the region and every value must be finite. The resource
  keeps its own independent copy in a ``RawVolume<float>`` and always extracts
  the **zero** isosurface.
* A layout is a vector of ``LodBlock{id, origin, cells, step}``. Each block
  covers the half-open cell box ``[origin, origin + cells*step)``; ``step`` is a
  positive power of two, ``cells`` is even and at least four, and all endpoints
  align to ``step``. The blocks must exactly tile one box without overlap, and
  coplanar neighbours use the same step or steps differing by exactly a factor
  of two. One layer of source samples outside the block box is reserved for
  central-difference normals.
* ``setLayout`` validates the whole layout first; an invalid layout throws
  ``std::invalid_argument`` and leaves the existing resource untouched.

Transition layer
~~~~~~~~~~~~~~~~
Ordinary cells use the same Marching Cubes corner, edge and triangle
conventions as the existing extractor, so a uniform layout reproduces the
stock geometry (at full floating-point precision). Where a cell borders a finer
neighbour it is subdivided onto the half-step source grid, and a fixed-point
red-green relaxation forces both sides of every **surface-carrying** face to the
same render step. Subdivision can cascade through several levels (for example
step 4 → 2 → 1) and across block boundaries, but it never averages density:
every introduced node is an original source grid point. Seam vertices are
merged by world position, so contact lines share one vertex index and one
finite, unit normal; a closed field inside the domain yields a closed, oriented
two-manifold with no T-junctions, duplicate faces or zero-area triangles. A
plane leaving the domain is open only on the outer domain boundary.

Normals are central differences of the **finest source grid** (never the block
spacing), interpolated along each edge and normalised, exactly as in the
Marching Cubes extractor. Fields whose level passes through a sample point or
with alternating-sign cell faces may be rejected with ``std::invalid_argument``.

Editing and incremental rebuilds
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
* ``updateSamples(std::vector<SampleEdit>)`` applies a batch; the batch is
  validated before any change, repeated positions take the last value, and a
  failed call rolls the source back so the old resource survives.
* Rebuilds are incremental. A layout change re-extracts only the blocks whose
  description or transition pattern changed (plus their seam partners); an
  edit re-extracts only blocks that read the edited sample, including the
  normal neighbourhood even when no corner sign changes. Distant blocks keep
  their geometry and revision. An empty edit or an identical (even reordered)
  layout performs zero rebuild and zero source reads.
* ``blockRevision(id)`` returns the per-block revision counter, and
  ``lastStats()`` reports the number of distinct blocks re-extracted
  (``rebuiltBlocks``) and the number of source density samples actually read
  (``sourceReads``); failed calls preserve the previous statistics.
* The new mesh and block revisions are published together, so ``mesh()`` never
  observes a half-updated state. ``mesh()`` returns an independent
  ``Mesh<Vertex<float>>`` in world coordinates with a zero offset and never
  re-reads the source.
