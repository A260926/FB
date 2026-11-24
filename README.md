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


Headless CPU multi-viewport rendering
-------------------------------------
The GL-free renderer lives in `headers/CPUFrame.h` / `CPUFrame.cpp` and uses
only the C++17 standard library, so no window or GPU is required.

### Per-instance ownership and the read-only period
- An `Octree` owns its own root bounds (`getSize`/`getHalf`), ray length and
  accelerator. The accelerator is sized, addressed and released from the
  tree that built it instead of the global `Options::OCTREE_SIZE`, so trees
  of different edges (for example 16/depth 4 and 32/depth 5) can coexist.
  `initRoot` may be called only once per tree; inserts afterwards keep using
  that tree's own bounds.
- A `Camera` accumulates its own rotation matrix; rotating one camera never
  changes another camera's basis.
- Building and `initAccelerator` are the only mutating phase. Once a tree and
  its accelerator are prepared, they stay read-only for the whole batch. The
  new read-only query does not use any instance scratch, so several worker
  threads may trace rays through the same `const Octree` at the same time.
  Concurrent editing is not required.
- `Options` still provides the defaults for the old window entry point. The
  new API takes every behavior switch (transparency, minimum layer spacing,
  scan glitch repair, perspective, zoom, ...) on the query/viewport, so later
  changes to the `Options` defaults do not affect a call already configured.

### Read-only ray query
`Octree::traceRay(const RayQuery&) const` takes the tree's own origin, unit
direction, positive maximum step distance, transparency switch, minimum
layer spacing and scan glitch repair switch. It returns depth/color copies
(`RayHit`) ordered by the original stepping sequence. The caller's inputs are
copied and never modified, and no scratch is retained for the next ray. It
shares the exact `getBit`, `avoidScanGlitches` and `getNextEntryDot` stepping
rules (minimum step, depth darkening, transparent layer filtering) with the
legacy `setRay`/`initBasis`/`setPerspectiveRay`/`rayTrace` path; both the
plain octree walk and the accelerator walk go through this entry. The legacy
entry points still work and may still mutate the ray they are given; only the
new entry supports concurrency.

### Batch frames
`cpuframe::renderBatch` renders several viewports in one call. Each
`ViewportInput` carries a `const Octree*`, a snapshot of the camera basis and
position, positive even width/height, `PIXEL_STEP == 1`, a non-negative
perspective factor, a positive zoom, a background RGBA and the blending
parameters. Every viewport is validated (sizes, finite parameters, non-zero
orthonormal basis) before any thread starts; invalid input throws
`std::invalid_argument` and no partial frame is returned.

Workers (1 to 32) pull configurable positive row blocks, never one thread per
pixel, and the extra scheduling storage scales with row blocks rather than
pixels. Worker count (1/2/4) and block order never change the pixels. The
returned `BatchResult` holds one `ViewportFrame` per input viewport, in input
order; each frame owns its own row-major RGBA buffer (`4 * width * height`
bytes) and the buffers are never shared. `BatchStats` reports the real number
of completed viewports, pixel queries and worker threads created.

`Core::process` now routes its `pixels != NULL` with `PIXEL_STEP == 1` path
through this shared pixel implementation; GL only displays the buffer. The
quads path and any other `PIXEL_STEP` keep their original behavior.

### Example and tests
`examples/cpu_frame.cpp` builds two trees and two cameras and renders them
with four workers into the PPM path given on the command line:

    c++ -std=c++17 -O2 -pthread -I. -include headers/CPUFrame.h \
        examples/cpu_frame.cpp CPUFrame.cpp Octree.cpp Octant.cpp \
        Camera.cpp Options.cpp math.cpp voxeldisplays.cpp -o cpu_frame
    ./cpu_frame frame.ppm

The headless checks (ownership, read-only queries and batch frames) run with:

    bash task_support/run.sh target

