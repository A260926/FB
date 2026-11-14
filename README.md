HLSLParser
==========

This is a fork of [Unknownworld's hlslparser](https://github.com/unknownworlds/hlslparser) adapted to our needs in [The Witness](http://the-witness.net). We currently use it to translate pseudo-HLSL shaders (using the legacy D3D9 syntax) to HLSL10 and Metal Shading Language (MSL). There's also a GLSL translator available that we do not use yet, but that is being maintained by community contributions.

The HLSL parser has been extended with many HLSL10 features, but retaining the original HLSL C-based syntax.

For example, the following functions in our HLSL dialect:

```C
float tex2Dcmp(sampler2DShadow s, float3 texcoord_comparevalue);
float4 tex2DMSfetch(sampler2DMS s, int2 texcoord, int sample);
int2 tex2Dsize(sampler2D s);
```

Are equivalent to these methods in HLSL10:

```C++
float Texture2D::SampleCmp(SamplerComparisonState s, float2 texcoord, float comparevalue);
float4 Texture2DMS<float4>::Load(int2 texcoord, int sample);
void Texture2D<float4>::GetDimensions(out uint w, out uint h);
```



Here are the original release notes:


> HLSL Parser and GLSL code generator
>
> This is the code we used in Natural Selection 2 to convert HLSL shader code to
GLSL for use with OpenGL. The code is pulled from a larger codebase and has some
dependencies which have been replaced with stubs. These dependencies are all very
basic (array classes, memory allocators, etc.) so replacing them with our own
equivalent should be simple if you want to use this code.
>
> The parser is designed to work with HLSL code written in the legacy Direct3D 9
style (e.g. D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY should be used with D3D11).
The parser works with cbuffers for uniforms, so in addition to generating GLSL,
there is a class provided for generating D3D9-compatible HLSL which doesn't
support cbuffers. The GLSL code requires version 3.1 for support of uniform blocks.
The parser is designed to catch all errors and generate "clean" GLSL which can
then be compiled without any errors.
>
> The HLSL parsing is done though a basic recursive descent parser coded by hand
rather than using a parser generator. We believe makes the code easier to
understand and work with.
>
> To get consistent results from Direct3D and OpenGL, our engine renders in OpenGL
"upside down". This is automatically added into the generated GLSL vertex shaders.
>
> Although this code was written specifically for our use, we hope that it may be
useful as an educational tool or a base for someone who wants to do something
similar.

Constant buffer `packoffset`
----------------------------

When `GLSLGenerator` is run with `GLSLGenerator::Flag_EmulateConstantBuffer`,
each `cbuffer` is emitted as a single `uniform vec4` array and every field read
is rewritten into an index/swizzle into that array. Top level cbuffer fields may
now carry an explicit register position using the HLSL `packoffset` syntax:

```C
cbuffer Params
{
    float   alpha : packoffset(c0.x);   // register 0, component x
    float   beta  : packoffset(c0.w);   // register 0, component w
    float4  tint  : packoffset(c2);     // register 2, starts on .x
    float   extra;                      // no packoffset: laid out automatically
};
```

Accepted forms are `: packoffset(cN)` and `: packoffset(cN.x|y|z|w)`, where `N`
is a non-negative decimal register number. `packoffset` is only allowed on top
level cbuffer fields; structure members never carry one. The supported element
types are `float`, `float2/3/4`, `float4x4`, fixed length arrays of those, and
structures composed of them.

### Layout conventions

Offsets are linear float slots; one vec4 register is four slots (`cN.x` is slot
`4*N`, `.y` `4*N+1`, and so on). The rules for a buffer that contains at least
one explicit position are:

* Scalars and vectors cannot straddle a register; a vector that does not fit in
  the remaining components of a register moves to the next register.
* Arrays, structures and matrices can only start at a register (`.x`).
* Array elements and structures start and end on a four-float boundary, so the
  reserved stride rounds up to a whole register. A `float4x4` occupies four
  column vectors (16 slots).
* Explicit positions may leave holes and may point behind the current cursor.
* Fields without a `packoffset` continue from the running cursor. An explicit
  field never pulls the cursor backwards: the cursor always tracks the highest
  occupied end seen so far, so a later explicit field can fill an earlier hole
  but a later automatic field never backfills one.
* The buffer size is the maximum occupied end across all fields, rounded up to
  a register — not the end of the final declaration. Padding reserved by
  aggregates counts as occupied.
* Overlapping reservations (including aggregate padding) are an error that
  names both fields and their positions. Invalid components, out of range
  register arithmetic, non-positive constant array lengths and unsupported
  types are likewise diagnosed rather than asserted or emitted.

A buffer with **no** `packoffset` keeps the original declaration-order layout
and generated output unchanged. Explicit `packoffset` is rejected unless
`Flag_EmulateConstantBuffer` is set; the plain GLSL `std140` path for buffers
without explicit positions is unchanged.

### Querying the layout from C++

The layout is computed once into a standalone, queryable result in
`src/ConstantBufferLayout.h`, and both the generated uniform array length and
every member/array access use that same result:

```C++
ConstantBufferLayout* layout =
    BuildConstantBufferLayout(&allocator, &tree, buffer, errorCallback, userData);

// layout->numRegisters / layout->size (floats, register rounded)
for (int i = 0; i < layout->fields.GetSize(); ++i)
{
    const ConstantBufferFieldLayout& f = layout->fields[i];
    // f.name, f.offset (linear floats), f.size (reserved, includes padding),
    // f.arrayStride (floats, 0 for non-arrays), f.explicitOffset
}
```

`GetStructMemberOffset` and `GetConstantArrayStride` expose the same aggregate
math used internally, so dynamic indexing into structure arrays, nested member
reads and whole-structure reads all resolve to identical addresses. Dynamic
indices must be side-effect-free integer expressions; constant indices are
range checked against the array length. The HLSL generator writes `packoffset`
back out, and re-parsing its cbuffer reproduces the same layout.

The command line tool exposes the emulation path with `-emulatecbuffer`.

### Checks

`checks/check-layout.sh` compiles and runs the C++ checks in
`checks/check-layout.cpp` (holes, backwards explicit positions, automatic
continuation, conflict and cross-register rejection, structure arrays and
matrix reads evaluated against an independently listed register table, legacy
output and HLSL round-trip) and then runs `checks/baseline.sh`. Everything uses
only the project sources, standard C++11 and the system compiler — no GPU or
shader SDK is required.

