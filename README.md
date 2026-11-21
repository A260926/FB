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

Enable threads (optional)
------------
On pthread platforms the CPU side can render several viewports at once
without OpenGL. New files:

- ``headers/CPUFrame.h`` / ``CPUFrame.cpp``: GL-free multi viewport batch
  renderer, C++17 standard library only.
- ``examples/cpu_frame.cpp``: builds two octrees of different sizes, two
  cameras, and writes a PPM file with four worker threads.
- ``tests/cpu_frame.cpp``: ownership, query and frame tests.

Build the example and the tests without a window or a GPU::

    c++ -std=c++17 -O2 -pthread -I. -include headers/CPUFrame.h -c CPUFrame.cpp -o CPUFrame.o
    c++ -std=c++17 -O2 -pthread -I. examples/cpu_frame.cpp CPUFrame.o \
        Octree.cpp Octant.cpp Camera.cpp Options.cpp math.cpp voxeldisplays.cpp \
        -o cpu_frame_example
    ./cpu_frame_example out.ppm

    c++ -std=c++17 -O2 -pthread -I. task_support/driver.c tests/cpu_frame.cpp \
        CPUFrame.o Octree.cpp Octant.cpp Camera.cpp Options.cpp math.cpp \
        voxeldisplays.cpp -o cpu_frame_tests
    ./cpu_frame_tests

Each call to ``cpuFrameRender`` takes a list of ``CPUFrameViewport`` and one
``CPUFrameOptions``. Every viewport supplies its own octree, a camera basis
and position snapshot, positive even dimensions, ``pixelStep`` 1, a non
negative perspective factor, a positive zoom, the background RGBA and the
per channel transparency weights. Rendering rules are the existing ones:
pixel rays come from the ``Core::process`` geometry (orthographic, or
perspective through the zoom and perspective factor), depth darkening and
``MAX_VOXELS_BY_RAY`` clipping are unchanged, and blending runs back to
front. ``CPUFrameOptions`` also carries the transparency switch, the minimum
voxel proximity, ``maxVoxelsByRay`` and the scan patch switch, so a batch does
not follow later changes to the legacy ``Options`` defaults.

Read-only period and ownership
-----------------------------
An octree is writable only while it is being built. ``initRoot`` runs once
per instance and the instance remembers its own root size, insertion bounds,
ray length and accelerator grid, so trees of size 16 and 32 (depth 4 and 5)
can coexist, and inserting into one never consults another's bounds or the
global ``Options::OCTREE_SIZE``. Once ``initAccelerator`` has run, the tree is
read-only for the whole batch: no query mutates it, and queries may run
concurrently on the same tree from several worker threads. The accelerator is
allocated, addressed and released by the instance that built it, and the
same query implementation serves both the accelerated and the plain
traversal. Editing and querying at the same time is not supported.

``Octree::queryRay`` is the read-only entry point. It takes the origin, the
unit direction, a positive maximum distance and the transparency, proximity
and patch switches, and returns ``Octree::RayHit`` copies ordered by the
ray's stepping order. The caller's inputs are copied and never retained: the
tree keeps no pointer to them and no per ray scratch survives the call. A
returned hit holds a copy of the raw color, so it stays valid after the tree
is destroyed. The legacy ``setRay``, ``initBasis``, ``setPerspectiveRay`` and
``rayTrace`` entry points still work and still move the caller's ray; they use
the same stepping code, but they are not safe to call concurrently.

``cpuFrameRender`` validates the whole batch before starting any work: a null
tree, a non positive or odd dimension, a ``pixelStep`` other than 1, a
negative perspective factor, a non positive zoom, a non finite parameter or a
non orthonormal camera basis raises ``std::invalid_argument`` and no partial
frame is produced. Workers are between 1 and 32, and the work is split into
row blocks of ``tileRows`` rows; at least two workers query the same tree
whenever a viewport has more than one block. No thread is created per pixel
and task storage does not grow with the pixel count. Results come back in the
input viewport order, each with its own row-major RGBA buffer. The returned
``CPUFrameStats`` reports the completed viewport count, the pixel query count
and the number of worker threads created. Output is byte for byte identical
whatever the worker count, the block order or the interleaving with other
batches.

The legacy ``Core::process(pixels)`` path with a non null buffer and
``PIXEL_STEP == 1`` goes through the same shared pixel implementation; GL only
displays the resulting buffer. The quad list path and other pixel steps keep
their previous behaviour.

Enjoy!
