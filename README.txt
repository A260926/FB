This is Gmsh, an automatic three-dimensional finite element mesh generator with
built-in pre- and post-processing facilities.

Gmsh is copyright (C) 1997-2026 C. Geuzaine and J.-F. Remacle, and is
distributed under the terms of the GNU General Public License, Version 2 or
later, with an exception to allow for easier linking with external
libraries. See LICENSE.txt and CREDITS.txt for more information.

See the doc/ and tutorials/ directories for documentation and tutorials. The
reference manual is located in doc/texinfo/. See the examples/ directory and the
web site https://gmsh.info for additional examples.

Building Gmsh from source code requires a C++ compiler and CMake
(http://cmake.org). Building the graphical user interface requires FLTK 1.4.0 or
higher (http://fltk.org), configured with OpenGL support. Support for boolean
operations, constructive solid geometry features and STEP file import requires
OpenCASCADE 6.9 or higher (version 7.2 or higher is highly recommended;
http://www.opencascade.com). Step-by-step compilation instructions are available
on the Gmsh wiki: https://gitlab.onelab.info/gmsh/gmsh/-/wikis/Gmsh-compilation.


Build Gmsh from the command line
--------------------------------

* Create a build directory, for example as a subdirectory of Gmsh's source
  directory:

    mkdir build

* To build the monolithic Gmsh app with the default build options, run cmake
  from within the build directory, pointing to Gmsh's source directory, then run
  "make"

    cd build
    cmake ..
    make

  Optionally running

    make install

  will install the Gmsh app in the standard system location (controlled by
  CMAKE_INSTALL_PREFIX - see below).

* To build the Gmsh app dynamically linked to the shared Gmsh library, which can
  then also be used by external codes through the C++, C, Python, Julia and
  Fortran Gmsh API, run

    cd build
    cmake -DENABLE_BUILD_DYNAMIC=1 ..
    make
    make install

  This will install the Gmsh app and the shared Gmsh library, as well as the C++
  and C include files, and the Python, Julia and Fortran modules.

* The JavaScript Gmsh API needs Gmsh compiled to WebAssembly with Emscripten:
  see contrib/webapp/README.txt.

* To change build options you can use "ccmake" instead of "cmake", e.g.:

    ccmake ..

  or you can specify options directly on the command line. For example, you can
  use

    cmake -DCMAKE_PREFIX_PATH=/opt/local ..

  to specify the location of external packages installed in non-standard
  directories. You can use

    cmake -DCMAKE_INSTALL_PREFIX=/opt

  to change the installation directory. Or you could use

    cmake -DENABLE_FLTK=0 ..

  to build a version of Gmsh without the FLTK graphical interface. The list of
  all available configuration options is given in the reference manual.

* To see a detailed compilation log use

    make VERBOSE=1


Build Gmsh using CMake's graphical user interface
-------------------------------------------------

* Launch CMake and fill-in the two top input fields (telling where the Gmsh
  source directory is located and where you want the Gmsh binary to be created).

* Click on "Add entry" and define the variable CMAKE_PREFIX_PATH, of type
  "PATH", pointing to the location(s) of any external package(s) (FLTK,
  BLAS/LAPACK, etc.) installed in non-standard directories.

* Click on "Configure" and choose your compiler.

* Optionally change some configuration options (re-run "Configure" every time
  you change some options).

* Once you are happy with all the configuration options, click on "Generate".

* Go to the build directory and build Gmsh using your chosen compiler.

Periodic CAD meshes and MSH 4
-----------------------------

A periodic mesh can be written to MSH 4 and read back into a new model that
contains the same CAD entities (e.g. the same built-in `geo` surfaces, curves
and their boundaries) but no mesh: `gmsh::merge()` reads the nodes, the
elements and the `$Periodic` section onto those entities. `setPeriodic()` does
not need to be called again, and the reader does not create a separate mesh or
re-pair the nodes from their coordinates.

Node roles. The nodes of a periodic entity are kept in two disjoint
correspondence maps:

  * `correspondingVertices`: the nodes that are corners (primary vertices) of
    the linear skeleton of the elements that actually use them;
  * `correspondingHighOrderVertices`: the other nodes used by those elements.

A node is a primary node if, and only if, it is a corner of at least one
element of the current mesh; the role never follows from the node tag, from a
distance between coordinates or from the polynomial order stored in the
`MVertex`. `gmsh::model::mesh::getPeriodicNodes()` with `includeHighOrderNodes
= false` reports the primary pairs only, and with `true` the union of the two
maps. The maps of a periodic entity point only to nodes that the model owns and
that its elements use; a node shared with a periodic boundary entity (a point
of a curve bounding two periodic surfaces, for instance) is the same object in
both correspondences, and both entities pair it with the same master node.

Reading presupposes that the CAD entities of the destination model have the
dimensions and tags of the file and that they carry no mesh before the merge.
The reader keeps the node tags, coordinates and parametric coordinates of the
file as they are; it does not move nodes and does not rebuild the CAD geometry
from the mesh alone. The pairs are classified with the elements of the model,
so the `$Periodic` section can contain high-order pairs written with
`Mesh.HighOrderSavePeriodic` (`Mesh.SaveParametric` keeps the parameters).

Order changes and refinement. `gmsh::model::mesh::setOrder()` and
`gmsh::model::mesh::refine()` rebuild the correspondence from the known corner
relations and the local reference positions of the line, triangle and
quadrangle nodes. Reversed edges, cyclic rotations and flips of the elements
of the master side are handled by mapping the reference positions, so the
master elements are never reversed to match (they can be shared). Lowering the
order removes the high-order nodes that the elements no longer use, together
with every reference to them; raising it pairs the new nodes. A pair is
identified by the corner correspondence, not by the closest node: with
`Mesh.HighOrderPeriodic` off, a node whose coordinates were changed on purpose
keeps its periodic counterpart. With `Mesh.Renumber` off, a pure order change
keeps the primary nodes and their tags. The periodic section written to MSH 4
(ASCII and binary) points to the same live nodes as the node block and the
element connectivity, so the file can be merged again and the order changed
further.
