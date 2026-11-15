A sample Dual Contouring implementation. For more info see http://ngildea.blogspot.co.uk/2014/11/implementing-dual-contouring.html

The octree code is based on the original reference code by Tao Ju. The QEF implementation was written by /u/unzret (on reddit.com).
All other code is written by Nicholas Gildea and free for any purpose, assuming that does not conflict with another license.

The sample depends on SDL2, GLM and GLEW. I don't think the particular version matters (beyond the paths in the .vcxproj file). 

Only build materials for VS2013 are provided but it should be easy to move to another environment.

The controls are:
	- hold left mouse and drag to rotate the view
	- use the mouse wheel to zoom in/out
	- press F1 to render a wireframe
	- press F2 to regenerate the octree using a new error threshold (and generate a new mesh)

Send any questions to nick.gildea@gmail.com or @ngildea85 on Twitter


View-dependent LOD (ViewLod, DualContouringSample/view_lod.h / view_lod.cpp)
----------------------------------------------------------------------------

The original BuildOctree/SimplifyOctree path is unchanged: SimplifyOctree
still destroys children when it collapses a node, so coarsening there is
irreversible without resampling.  ViewLod keeps the whole unit-cell tree that
is sampled once at construction.  Every internal node stores the candidate it
would have produced under the old bottom-up rule (accumulated corners, QEF and
average normal, with the same six-crossing-edge cap, normal finite difference,
QEF solve and superbox fallback) plus an axis-aligned bounding box over all of
its non-empty finest descendant vertices.  update() only changes which nodes
form the current frontier; it never calls the density function and never
copies or destroys the tree.

Projected error.  A node with candidate vertex q has an AABB over its finest
descendant vertices.  With an orthonormal camera basis (eye, right, up,
forward) a world point p projects to

    focalPixels * (dot(p-eye,right), dot(p-eye,up)) / dot(p-eye,forward).

The node's error E is the largest 2D distance between the projection of q and
the projections of the eight corners of that AABB.  A node may only newly
collapse when E <= collapsePixels, and it may never collapse when the smallest
camera-space depth of its AABB is at or below nearDistance (the near plane
reaches the box) or when its accumulated corners are all air or all solid.
No frustum culling is performed and the unit leaves are always retained.

Hysteresis.  Collapse and split use different thresholds, so small camera
motions do not thrash the frontier: a node stays collapsed while E remains
below splitPixels and only expands once E > splitPixels; after expanding, its
descendants are re-selected with the same E <= collapsePixels rule.  The
collapsed/expanded state is stored inside the ViewLod object, so each frame
continues from the previous one instead of restarting from an empty state.
After selection, adjacent non-empty frontier cells are closed under face
neighbour 2:1 balance: cells sharing a face of positive area (edge- or
corner-only contact is ignored) may differ in size by at most a factor of
two, and any coarser neighbour is repeatedly forced finer.  Balance takes
priority over hysteresis within one update: a node forced finer cannot be
re-collapsed in the same pass.  Empty branches never get fabricated vertices.

mesh() runs the original mixed-depth ContourCell/Face/EdgeProc recursion over
the current frontier, so coarse and fine neighbours stay connected, and
repeated calls leave the frontier and the construction-time directed geometry
unchanged.  frontier() returns the active cells sorted by
(min.x, min.y, min.z, size); lastStats() reports the construction-only
density sample count together with the projection error tests and forced
balance refinements performed by the most recent successful update.  Invalid
cameras or thresholds make update() throw std::invalid_argument without
disturbing the previous frontier, mesh or stats.

The headless demonstration in examples/view_lod_headless.cpp (no SDL, window
or GPU) writes .task-build/lod-near.obj and .task-build/lod-far.obj from three
viewpoints; tests/view_lod_target.cpp consumes both files.  Build and run
everything with:

    bash task_support/run.sh target
