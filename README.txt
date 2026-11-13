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

Editable octree
---------------

DualContouringSample/editable_octree.h and .cpp add EditableOctree, which
keeps the finest-level dual contouring geometry alive for the lifetime of the
object so that local sculpting does not require rebuilding the octree.

Construction:

	EditableOctree tree(min, size, density);

	- min        the lowest corner of the root box
	- size       a positive power of two; the finest cells stay 1x1x1
	- density    std::function<float(const glm::vec3&)>, called with world
	             space positions and kept alive for the object's lifetime

Update box promise:

	tree.Update(lo, hi);

	The caller announces that the density may have changed inside the closed
	box [lo, hi] and guarantees that it is unchanged outside.  Update()
	resamples only the unit cells whose evaluation footprint can be affected.
	That footprint is the cell itself plus the 0.001 central-difference halo
	used for normals, so a box may span cells and the octree's middle planes.
	Leaves outside the affected region are not rebuilt and their stored
	geometry is reused verbatim.

Sampling statistics:

	EditableOctree::Stats stats = tree.LastUpdateStats();

	density_calls   number of real calls made into the density closure during
	                the most recent constructor or Update() call
	rebuilt_leaves  number of unit cells rebuilt by that call

	Mesh() never calls the density function, so meshing and threshold changes
	leave these figures untouched.

Mesh(float threshold, VertexBuffer&, IndexBuffer&):

	Outputs the current model using the existing dual contouring connection
	rules and QEF placement.  A negative threshold disables simplification.
	Simplification is applied to a temporary view of the persisted geometry,
	never to the editable data, so requesting a coarse model and then
	returning to -1 always restores the complete fine mesh.

	The upstream BuildOctree() and the interactive sample's default behaviour
	are unchanged.

Building the CPU-only sample and regression tests
-------------------------------------------------

The editable path is built without SDL, OpenGL, a GPU or Python.  From the
repository root:

	sh support/target.sh

This compiles the production CPU sources, runs the baseline check and executes
test/editable.cpp (edit_geometry, edit_invalidation, edit_views).

The standalone sculpting example writes OBJ files before/after edits and
self-checks every state against a full rebuild plus analytic references:

	c++ -std=c++11 -O2 -DCHECK_HEADLESS -Ithird_party -IDualContouringSample \
	    DualContouringSample/octree.cpp DualContouringSample/qef.cpp \
	    DualContouringSample/svd.cpp DualContouringSample/density.cpp \
	    DualContouringSample/editable_octree.cpp \
	    DualContouringSample/editable_sample.cpp -o editable_sample
	./editable_sample

It writes sculpt_before.obj, sculpt_after.obj and sculpt_coarse.obj.

Not covered here: concurrency, persistence to disk, stitching separate roots
and changing the finest resolution after construction.


