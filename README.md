cloud-of-voxels
===============
```
This is a basic CPU side Voxels to Pixels rendering (voxel raycaster) project,
using a process over an Octree to accelerate the ray. For each Pixel a Ray is
generated, and goes through the Octree searching for the first Voxel on its path.
When the Ray hits the Voxel, the corresponding Pixel is drawn to the screen.

By default the octree size is set to 256. That size is also the maximun size your
object could have with this configuration. You need Blender 2.78c to export your
model. Start Blender from command line and open the file from which you want to
export the model. If your mesh has been transformed, make sure to apply Location,
Rotation & Scale (CTRL+A). In the middle of the scene add a lattice, then on your
mesh add a "Remesh" modifier. Choose blocks, and change the octree depth value,
util the size of the blocks fits with the lattice, modify the scale value in the
modifier if needed. Apply the modifier. Edit the mesh, select all the faces and
choose "Shade Smooth" from the Faces menu (CTRL+F). Then, add the modifiers
"Triangulate" and "Smooth". Increase the values in smooth if necessary. In bake,
select "Bake to Vertex Color". Put your mesh in "Vertex Paint" mode then run Bake
in "Full Render" mode. Disable the modifiers before running the Python script
(given in the repo). Once it's done, replace the voxelmodels.cpp and voxelmodels.h
by the generated files.

From Windows compile the code with: mingw32-make -f Makefile-Mingw clean;
mingw32-make -f Makefile-Mingw or from Linux: make clean; make;
Launch the program with: ./bin/cov no-motion no-logs or with
./bin/cov no-motion no-logs stepx:2 if it's too slow.

Available options:
no-logs: ............... Disable useless log
no-motion: ............. Disable the default motion
accelerate: ............ Enable the pre-searching process
transparency: .......... Enable transparency
smooth-siblings: ....... Add additionnal voxels arround each voxel to reduce gaps
avoid-scan-glitches: ... Try to avoid glitches in real-time
w: ..................... Sets the view render width
h: ..................... Sets the view render height
z: ..................... Sets the camera z position
r: ..................... Changes the default value of the camera step angle
ray: ................... Set the max ray length
fps: ................... Changes the default max fps value
step: .................. Changes the default scan x and y step pixel
octree-size: ........... Changes the default octree size
voxel-shell-type: ...... Activates transparent voxel shell (xl, normal, lite)
voxel-shell-rgba: ...... Affects a specific RGBA color to the voxel shell (0xFFFFFFFF)
min-voxels-proximity: .. Minimum distance between the voxels for blending effect.
max-voxels-by-ray: ..... Maximum of voxels used for blending effect in one ray.
                         The default value is 1, and corresponds to max ray length.


With the given voxelmodels.cpp, once compiled on linux test the program with:
./bin/cov no-motion step:1 w:128 h:128 octree-size:256 z:-70 ray:80 no-logs \
    voxel-shell-type:xl voxel-shell-rgba:0xFF10F040 min-voxels-proximity:7 \
    accelerate fps:30
    
or with powershell:
./bin/cov no-motion step:1 w:128 h:128 octree-size:256 z:-70 ray:80 no-logs `
    voxel-shell-type:xl voxel-shell-rgba:0xFF10F040 min-voxels-proximity:7 `
    accelerate fps:30

Enjoy!

CPU paged rendering
===================

`headers/PagedOctree.h` / `PagedOctree.cpp` add an out-of-core CPU path for
octrees whose node data does not fit in memory.  It is independent of the GL
window path: `Octree::rayTrace` and the accelerator keep working exactly as
before, and the paged code uses only the C++17 standard library (no OpenGL,
display or network).

Snapshot export
---------------

`Octree::exportPagedSnapshot(path, recordsPerPage, &error)` walks the live
octree and writes an immutable snapshot:

* the existing spatial partition (each node's half-open cube is derived from
  the root grid and the child-slot path), empty nodes and leaf colours are
  kept;
* only values and node numbers are stored - no pointers are serialised;
* `recordsPerPage` controls how many node records share a page.

`paged::SnapshotReader::open()` reads only the 64-byte header and the page
directory.  Node payloads are read lazily, one page at a time.  A short or
corrupt header/directory is rejected; a truncated page payload is reported as
a load failure.

File format (all integers little-endian, fixed width):

```
Header, 64 bytes
  0  u32 magic "PCVX" (bytes 'P','C','V','X')
  4  u16 version (1)
  6  u16 headerBytes (64)
  8  u32 rootHalf        root cube is [-rootHalf, +rootHalf)
 12  u32 maxDepth        root->depth
 16  u32 pageRecords     maximum records per page
 20  u32 flags           must be 0
 24  u64 nodeCount
 32  u64 pageCount
 40  u64 directoryBytes  pageCount * 16
 48  u64 reserved        must be 0
 56  u32 reserved        must be 0
 60  u32 crc32           CRC-32 of bytes [0,60)

Page directory, pageCount entries of 16 bytes
  u64 pageOffset   absolute offset of the page payload
  u32 recordCount  records in this page (<= pageRecords)
  u32 reserved     must be 0

Page payload
  u32 payloadBytes (recordCount * 32)
  u32 crc32        CRC-32 of the records
  recordCount records of 32 bytes
    u32 nodeIndex  absolute node number (root = 0)
    u32 childBase  absolute index of child slot 0, or 0xFFFFFFFF
    f32 minX       cube minimum corner, x
    f32 minY       cube minimum corner, y
    f32 minZ       cube minimum corner, z
    f32 half       cube half extent; cube is [min, min + 2*half)
    u32 color      packed RGBA (0xRRGGBBAA)
    u8  flags      bit0 leaf, bit1 has colour
    u8  level      remaining depth of this node (0 = leaf level)
    u16 reserved   must be 0
```

Node `i` lives on page `i / pageRecords` at position `i % pageRecords`.  An
internal node owns exactly eight consecutive child records starting at
`childBase`, in slot order `x + 2*y + 4*z`.  Every cube is half-open
`[min, min + 2*half)`; `Voxel::coordinates` is never used as geometry.

Bounded page cache and resumable rays
-------------------------------------

```cpp
paged::SnapshotReader reader;
reader.open("scene.pcoat");                       // header + directory only

// Synchronous real-file loader; max one resident page.
std::unique_ptr<paged::PageLoader> loader = reader.createFileLoader();
paged::PageCache cache(1, std::move(loader));

paged::PagedRay ray;
ray.init(paged::RayRequest(ox, oy, oz, dx, dy, dz, tmin, tmax),
         reader.header());

paged::PageId need = paged::INVALID_PAGE;
paged::RayResult result;
switch(ray.advance(&cache, &need, &result))
{
    case paged::TRACE_NEED_PAGE: /* make `need` resident, then advance again */
    case paged::TRACE_HIT:       /* result.t, result.leaf, result.r/g/b/a */
    case paged::TRACE_MISS:
    case paged::TRACE_ERROR:
}
```

* Capacity is clamped to at least one page; resident pages never exceed it.
* Two rays asking for the same page share one loader request.
* A `RequestId` is unique for the whole process.  Duplicate, out-of-order and
  unknown completions are rejected without advancing anything.
* `PagedRay` keeps only node indices, cube geometry and the remaining
  `[tIn,tOut]` interval - never a page address.  After `NeedPage`, any number
  of other advances, evictions or reordered replies, the ray continues from
  its own stack and does not re-walk consumed branches or treat a missing
  page as an empty voxel.
* Rays use `O + tD` with a finite `[tmin,tmax]`; `D` may be non-zero with zero
  components.  Only leaves with a strictly positive clipped interval count,
  cubes are half-open, so adjacent leaves never both claim a shared face,
  edge or corner.  A pure touch is a miss.  The nearest coloured leaf wins.

Batch frames
------------

`PagedFrame` drives many rays round-robin through one bounded cache:

```cpp
std::vector<paged::RayRequest> rays = ...;
std::unique_ptr<paged::PagedFrame> frame =
    paged::PagedFrame::openBatch(&reader, rays.data(), rays.size(),
                                 /*pageBudget=*/1);

frame->start();
while(!frame->step(/*maxAdvances=*/0))
{
    double p = frame->progress();        // in [0,1], rays terminal / total
    if(frame->hasFailure()) break;       // load failure: never "complete"
}
std::vector<paged::RayResult> results;
frame->results(&results);                // input order
const paged::CacheStats& s = frame->stats();
```

* Rays interleave: each turn consumes at most one hierarchy node, so no ray
  is fully loaded before the others start.
* `isComplete()` becomes true only after every ray reached a terminal state
  and no page load failed.  `progress()` is available while incomplete.
* `cancel()` ends the frame: its wait tokens are detached and its outstanding
  requests are revoked, so later replies with old request ids neither fill
  the cache nor resume a ray.  A new frame (with its own cache and loader)
  can be started afterwards.
* `deliver(requestId, page)` and `failRequest(requestId)` let an external or
  asynchronous loader drive completions; duplicate deliveries are no-ops.
* `CacheStats` reports real counters: `nodeVisits`, `pageRequests`,
  `pageHits`, `pageLoads`, `pageEvictions` and the `peakResident` high-water
  mark.

Traversal follows only intersected tree branches - it never expands the tree
into dense voxels and never scans leaves one by one.

Example and tests
-----------------

`examples/paged_frame.cpp` builds a real octree, exports a snapshot, reads it
through a one-page cache and writes a non-empty PPM:

```
./.task-build/paged_frame out.ppm [width] [height] [pageBudget]
```

`tests/paged_octree.cpp` registers the `geometry`, `paging` and `lifecycle`
groups through `task_support/check.h` and cross-checks colours, leaf identity
and `t` against an independent integer-cube intersection and an all-resident
walk.  Run `bash task_support/run.sh target` to build the new production files,
run the example, and execute the three test groups.
