PolyVox - The voxel management and manipulation library
=======================================================
PolyVox is the core technology which lies behind our games. It is a fast, lightweight C++ library for the storage and processing of volumetric (voxel-based) environments. It has applications in both games and medical/scientific visualisation, and is released under the terms of the `MIT license <https://www.tldrlegal.com/l/mit>`_.

PolyVox is a relatively low-level library, and you will need experience in C++ and computer graphics/shaders to use it effectively. It is designed to be easily integrated into existing applications and is independent of your chosen graphics API. For more details please see `this page <http://www.volumesoffun.com/polyvox-about/>`_ on our website.

Dynamic light volumes
=====================

``PolyVox/LightVolume.h`` adds dynamic sky and RGB block lighting on top of an
existing ``RawVolume<uint8_t>`` or ``PagedVolume<uint8_t>``. ``PolyVox/LitCubicSurfaceExtractor.h``
turns the volume and its light field into a ``PolyVox::Mesh`` whose vertices carry
positions, normals, materials and four light channels.

Both headers are header-only and only depend on the C++17 standard library.

LightVolume
-----------

.. code-block:: cpp

    #include "PolyVox/RawVolume.h"
    #include "PolyVox/LightVolume.h"
    #include "PolyVox/LitCubicSurfaceExtractor.h"

    std::array<PolyVox::LightMaterial, 256> materials = {};
    materials[1] = PolyVox::LightMaterial(/*opaque*/ true,  /*red*/ 15, /*green*/ 0, /*blue*/ 0);
    materials[2] = PolyVox::LightMaterial(false, 0, 8, 15);

    PolyVox::Region region(0, 0, 0, 127, 127, 127);
    PolyVox::RawVolume<uint8_t> volume(region);
    PolyVox::LightVolume<PolyVox::RawVolume<uint8_t> > light(&volume, region, materials);

``LightMaterial`` has an ``opaque`` flag and three emission components (red,
green and blue), each in ``[0, 15]``. Material 0 is always air: non-opaque and
non-emitting, whatever the table says. The ``Region`` uses the usual PolyVox
inclusive-bounds convention and light outside it is zero.

The constructor performs one scan of the region. After that, submit every voxel
edit through ``apply()``:

.. code-block:: cpp

    std::vector<PolyVox::LightEdit> edits;
    edits.push_back(PolyVox::LightEdit(64, 8, 64, 1)); // x, y, z, material
    edits.push_back(PolyVox::LightEdit(64, 8, 65, 0));
    std::vector<PolyVox::Vector3DInt32> changed = light.apply(edits);

Within one batch the last edit for a coordinate wins, and the underlying volume
is written once per distinct coordinate using that final material. ``apply()``
returns the coordinates whose final light value differs from before the call, in
``(x, y, z)`` lexicographic order. Duplicate coordinates, and coordinates whose
light changed and then changed back inside the batch, are not reported. A batch
whose final materials are all unchanged performs no volume writes and no
propagation work.

Read the result with ``getLight(x, y, z)`` (a ``LightValue`` of four channels
``sky``/``red``/``green``/``blue``), ``getLight(x, y, z, channel)`` for a single
channel, or ``getLight(Vector3DInt32)``.

The work done by the most recent ``apply()`` is available through ``lastStats()``,
``materialReads()`` and ``queuePops()``. The two counters are reset at the start of
each batch and count actual material reads from the volume and actual propagation
queue pops respectively. Work is proportional to the affected area rather than the
size of the region: removing a single lamp from a large roofed cave costs a few
thousand queue pops, not a full rescan.

Lighting rules
--------------

* Block light (red, green, blue) is computed independently per channel as the
  maximum of a voxel's own emission and each of its six neighbours' value minus
  one. Opaque voxels keep only their own emission but still emit to neighbours.
* Skylight only exists in non-opaque voxels. A non-opaque voxel on the top layer
  of the region is a source of 15. Light travelling to the voxel directly below it
  does not decay; every other step decays by one. Opaque voxels have zero skylight.
* All values are ``max`` over reachable sources along legal paths, so no sourceless
  residual light rings are left behind.

Lit cubic meshes
----------------

.. code-block:: cpp

    auto mesh = PolyVox::extractLitCubicMesh(&volume, light, region);

``extractLitCubicMesh`` uses the same surface test and winding as
``extractCubicMesh`` (a face is emitted where a non-zero voxel meets a zero voxel)
and does not change the existing extractor or its compressed vertex format. Its
mesh uses ``LitCubicVertex<uint8_t>``:

* ``position`` -- absolute world-space position (the mesh offset is zero),
* ``normal`` -- outward axis-aligned face normal,
* ``data`` -- the material of the voxel owning the face,
* ``light[4]`` -- sky, red, green and blue in ``[0, 1]`` (the stored ``0..15``
  value divided by 15).

The light of a face corner is the average of the four voxels immediately outside
that corner: the outside voxel plus the two voxels one step along the face
tangents towards the corner, plus the diagonal combination. Voxels outside the
``LightVolume`` region count as zero light. Two corner positions share a vertex
only when the face normal, material and all four light channels also match.

With ``bMergeQuads = true`` (the default) flat coplanar unit faces of the same
material merge into larger rectangles. A rectangle is only formed when every one
of its original unit-face corners has the same four light values, so lighting
gradients keep their unit faces and are never averaged away.

Colour mapping
--------------

The example ``examples/lit_voxels.cpp`` writes ``.task-build/lit.ply``. Its ASCII
PLY vertex colours are produced from the four channels as

.. code-block:: text

    red   = clamp(light[1] + 0.10 * light[0]) * 255
    green = clamp(light[2] + 0.10 * light[0]) * 255
    blue  = clamp(light[3] + 0.10 * light[0]) * 255

i.e. the block light colour plus a small skylight term. The example builds a
roofed room with a red and a blue lamp, extracts the lit mesh, turns the blue lamp
off and back on, and finally opens a hole in the roof, showing how the light and
the mesh update with each edit.

Limitations
-----------

``LightVolume`` does not support concurrent access, direct writes to the bound
volume which bypass ``apply()``, or re-entrant ``Pager`` callbacks. All of these
are outside the intended usage.