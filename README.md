# Fast-Quadric-Mesh-Simplification


**Summary**

Since I couldn't find any code that's fast, memory efficient, free and for high quality, I developed my version of the quadric based edge collapse mesh simplification method. It uses a threshold to determine which triangles to delete, which avoids sorting but might lead to lesser quality. It is about four times faster than Meshlab. Note that this algorithm is fast and works well for watertight volumes without without thin sections.

![img](https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification/blob/master/screenshot.png?raw=true)

**Compiling and Usage**

You can compile and execute the minimal command line program:

```
git clone git@github.com:sp4cerat/Fast-Quadric-Mesh-Simplification.git
cd Fast-Quadric-Mesh-Simplification/src.cmd
make
./simplify ../data/wall.obj ./simple.obj 0.05
```

Running the `simplify` executable without any arguments will provide further helpful usage instructions.

This simple program simplifies meshes stored in [OBJ](https://en.wikipedia.org/wiki/Wavefront_.obj_file) format. This is a simple reader/writer, so don't try to use this tool when multiple objects are embedded into one file.

Note that you can compile with `make sanitize` to create an executable with debugging capabilities. Finally, you can compile with `make wasm` to create WebAssembly code, as showcased [live demo web page](https://neurolabusc.github.io/simplifyjs/).

**Reflection symmetry constraint**

The command line tool accepts an optional `--symmetry <file>` option. The
symmetry file is a flat whitespace separated list:

```
normal.x normal.y normal.z      # unit normal of the mirror plane
offset                          # plane equation: normal . p + offset = 0
input_tolerance                 # max distance between a vertex and its mirror
vertex_count                    # number of vertices in the OBJ
mate[0] mate[1] ...             # one mate index per vertex, 0 based
```

`mate` must be an involution (`mate[mate[i]] == i`); a self mate must lie on
the plane. The input face set has to be invariant under reflection with
reversed winding, and the coordinates only have to match their mirror image up
to `input_tolerance`. Malformed input makes the tool exit non-zero. When the
option is used the compacted mate is also written to `<output>.symmetry` in the
same format, so a decimated symmetric mesh can be fed back in.

```c++
#include "src.cmd/Simplify.h"

Simplify::SymmetryContext sym;      // normal, offset, input_tolerance, mate
Simplify::load_obj("dome.obj", true);
// fill sym.mate for every loaded vertex (mate[mate[i]] == i)
try {
    Simplify::simplify_mesh(1000, 7.0, false, &sym);
} catch (const std::invalid_argument &e) {
    // malformed symmetry description or an input that does not mirror
}
Simplify::write_obj("dome_decimated.obj");
// sym.mate now uses the compacted indices and sym.original_to_vertex maps
// every pre-call vertex to its final representative (or -1).
```

Reading the result back is the same as reading the OBJ; the `.symmetry` file
lists the compacted vertex pairs so the next call can reuse it.

**For Developers**

The functionality is contained in Simplify.h. The function to call is *simplify_mesh(target_count)*. The code is kept pretty slim, so the main method has just around 400 lines of code.

**Translations, Implementations and Live Demos**

 - [C++](https://github.com/sp4cerat/Fast-Quadric-Mesh-Simplification) (this page).
 - [C++ in Slicer](https://github.com/Slicer/SlicerSurfaceToolbox/blob/09d5bf61655c1f276541797f6d2593a61eabf98d/Decimation/Simplify.h) is available as a [Linux, MacOS or Windows](https://download.slicer.org/) desktop application.
 - [C](https://github.com/neurolabusc/nii2mesh) translation with Marching Cubes.
 - [C# for Unity mesh simplification](https://github.com/Whinarn/UnityMeshSimplifier).
 - [C# for .NET mesh simplification](https://github.com/Whinarn/MeshDecimator).
 - [Java mesh simplifier by Jayfella](https://hub.jmonkeyengine.org/t/isosurface-mesh-simplifier/41046).
 - JavaScript [ThreeJS](https://github.com/timknip/mesh-decimate) for native [JavaScript](https://gist.github.com/zz85/a317597912d68cf046558006d7647381) and Web Assembly implementations, illustrated with a [live demo web page](https://neurolabusc.github.io/simplifyjs/).
 - JavaScript [live demo](https://github.com/neurolabusc/niivue-simplify) that compares four implementations of this algorithm including C++ WASM (this page), C WASM (nii2mesh), [mXrap](https://mxrap.com/js_docs/lib_QuadricMeshSimplification.html) pure JavaScript and NiiVue pure javascript. 
 - JavaScript [NiiVue](https://github.com/niivue/niivue-mesh) for converting voxels to meshes and simplifying meshes with [live demo web page](https://niivue.github.io/niivue-mesh/).
 - [JavaScript translation](https://github.com/ataber/mesh-simplify) available from [npm](https://www.npmjs.com/package/mesh-simplify).
 - [JavaScript WebAssembly](https://github.com/MyMiniFactory/Fast-Quadric-Mesh-Simplification) port with [live demo](https://myminifactory.github.io/Fast-Quadric-Mesh-Simplification/).
 - [Pascal](https://github.com/neurolabusc).
 - Pascal [Surf-Ice](https://www.nitrc.org/plugins/mwiki/index.php/surfice:MainPage) desktop application for Windows, Linux and MacOS.
 - [Python pyfqmr is a Cython wrapper for fast quadric mesh reduction](https://github.com/Kramer84/pyfqmr-Fast-Quadric-Mesh-Reduction).