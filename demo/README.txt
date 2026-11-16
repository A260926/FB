This demo contains sample code for loading the IQM format. It requires GLUT or freeglut to build and run, and should compile on any platform for which they are available with the included Makefile. On Windows, you can just run the pre-built demo.exe and gpu-demo.exe. 

The GPU Skinning demo (gpu-demo) shows an example of how one can animate meshes stored on the GPU in vertex buffer objects using GLSL shaders.

The demos are run as follows:

./demo [options] [file.iqm] [anim.iqm]
./gpu-demo [options] [file.iqm] [anim.iqm]

If no "file.iqm" is specified, then "mrfixit.iqm" is loaded by default. If "file.iqm" contains animations, these animations will be used. If an extra "anim.iqm" is specified containing animations, these animations will be used instead along with the meshes in "file.iqm".

Options can be any of the following command-line switches:

-s N
  Displays the model with scale N.

-r N
  Rotates the model by N degrees.

-t Z
-t X,Y,Z
  Translates model by X,Y,Z units.


Continuous skin surface mode (skin_surface.h/.cpp)
---------------------------------------------------

The normal demo only transforms the original vertices, so each triangle stays
flat between its corners.  The optional continuous skin surface instead treats
every bound triangle as a parametric patch: bind-pose positions and the byte
bone weights (divided by 255 and merged per bone) are interpolated inside the
triangle, and the current frame palette (the same outframe matrices computed by
animateiqm, with no extra inverse-bind multiply) is applied to the interpolated
data.  The result F(b) is quadratic in barycentric coordinate b, so bent
regions curve smoothly instead of creasing at edges.

It is off by default and the original loader, the classic four-attribute CPU
skinning path and the GPU demo keep their exact behavior.  Enable it and pick a
refinement budget using the C++ API:

    SkinSurfaceOptions opt;
    opt.chordTolerance = 1e-4;  // positive finite max deviation, world units
    opt.maxDepth       = 12;    // non-negative deepest uniform dyadic level
    opt.maxTriangles   = 200000;// positive total output triangle budget
    setSkinSurfaceOptions(opt);
    setSkinSurfaceEnabled(true);

Each animateiqm() then re-evaluates the whole surface for that frame's palette.
Every emitted parameter triangle carries an errorBound that is a conservative
whole-closed-triangle upper bound (from the quadratic Bernstein control net) on
the deviation of F from the triangle's linear interpolation; the bound never
exceeds chordTolerance.

The latest successful output is a read-only snapshot:

    const SkinSurfaceMesh* mesh = getSkinSurfaceDrawMesh();

with shared positions, three position indices and a sourceTriangle per output
triangle, and per-corner barycentric coordinates, analytic unit normals and
interpolated UVs; materialRanges maps each original iqmmesh triangle range onto
the corresponding contiguous run of output triangles so the original textures
are reused.  When the mode is on, renderiqm draws this snapshot through those
material segments; the GPU demo takes the same branch inside its existing GL
conditional-compilation boundary.

Budget and failure example:

    if(!evaluateSkinSurface(binding, outframe, numjoints, opt))
    {
        // keep drawing the last good getSkinSurfaceDrawMesh() snapshot, or
        // relax the budget: raise maxDepth/maxTriangles or chordTolerance.
    }

If the tolerance cannot be reached within maxDepth, the output would exceed
maxTriangles, or a sampled derivative degenerates, evaluation fails and the
previous complete snapshot is retained (a new positions array is never paired
with old indices/material tables).  Two triangles sharing an original vertex
edge share one position-index string along that edge (no T-junctions), even
across material boundaries; coincident-but-unindexed seams are not welded.  A
disconnected component whose patch is affine (e.g. a single-bone region) stays
one triangle no matter how tightly the curved parts are refined.


*** LICENSE INFO ***

The demo source code is licensed as specified by the LICENSE file included with this distribution.

For GL/ and freeglut file licenses, please refer to the licenses included within the files. 

The "mrfixit.iqm" model (and "Head.tga"/"Body.tga" skins) is subject to the following license:

Mr. Fixit conceptualized, modeled and animated by geartrooper a.k.a. John Siar ironantknight@gmail.com
Mr. Fixit textures, normal maps and displacement by Acord a.k.a Anthony Cord tonycord@gmail.com
mask textures by Nieb

Mr. Fixit is released under the CC-BY-NC (Creative Commons Attribution Non-Commercial) license. See http://creativecommons.org/licenses/by-nc/3.0/legalcode for more info. Please contact Geartrooper for permission for uses not within the scope of this license.


