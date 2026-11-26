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
void Texture2D::SampleCmp(SamplerComparisonState s, float2 texcoord, float comparevalue);
float4 Texture2DMS<float4>::Load(int2 texcoord, int sample);
void Texture2D<float4>::GetDimensions(out uint w, out uint h);
```

Explicit `packoffset` cbuffer layout
------------------------------------

In the constant-buffer emulation path (`GLSLGenerator::Flag_EmulateConstantBuffer`,
which emits one `uniform vec4 <BufferName>[]` and lowers every field access to
indices/components of that array), cbuffer fields accept an explicit register
placement:

```C
cbuffer Params
{
    float4x4 worldMatrix : packoffset(c0);      // c0 .. c3 (four column vectors)
    float    age         : packoffset(c4.x);
    float2   uv          : packoffset(c4.z);    // c4.zw
    Light    lights[3]   : packoffset(c8);      // arrays/structs start at .x
    float    autoValue;                         // continues at the high-water cursor
    float    holeFiller  : packoffset(c6.y);    // later explicit fields may fill holes
};
```

`N` in `packoffset(cN[.x|.y|.z|.w])` is a non-negative decimal register. Float
offsets used by the layout API count individual components: register `cN`
covers offsets `4*N .. 4*N+3`, and `.x/.y/.z/.w` are `0/1/2/3`.

Layout rules (for any cbuffer that contains at least one `packoffset`):

* `float`, `float2`, `float3`, `float4` never cross a register; an explicit
  component that would make the value straddle a register is an error.
* Arrays, structures and `float4x4` may only start at `.x`. Matrices keep the
  original four-column-vector representation (16 floats for `float4x4`).
* Every array element and every structure starts/ends on a four-float
  boundary: array strides and struct reserved sizes are rounded up to a
  multiple of four. The trailing struct padding is reserved and therefore
  participates in occupancy/conflict checks.
* Fields without an explicit position are placed from the current cursor using
  the original alignment rules. An explicit field never moves the cursor
  backwards: the cursor is always the highest occupancy end seen so far.
  Consequently a later explicit field can fill an earlier hole, but following
  automatic fields continue after the highest occupied end.
* Explicit positions may leave holes. Overlapping occupancy (including overlap
  with aggregate padding) is a hard error that names both fields and both
  positions; nothing is silently overwritten.
* The uniform array size is the maximum occupancy end of all fields rounded up
  to a whole register, not the end of the last declaration.
* Illegal components, register arithmetic that leaves the supported range,
* missing/non-positive/non-constant array lengths and unsupported field types
  are reported as normal diagnostics (`HLSLParser`/`GLSLGenerator` return
  failure) instead of asserting or emitting a possibly-misusable result.
* A `packoffset` field with the plain (non-emulated) GLSL path is explicitly
  rejected, because a `layout(std140)` block cannot honor an arbitrary
  register assignment. Buffers without any `packoffset` keep the historical
  layout and byte-for-byte generated output.

### Querying the layout from C++

`src/CBufferLayout.h` is a standalone, header-only, C++11-queryable result. It
is the same result `GLSLGenerator` builds up front and uses for the generated
uniform-array length and every top-level field address:

```C++
#include "HLSLParser.h"
#include "CBufferLayout.h"
using namespace M4;

Allocator    allocator;
HLSLTree     tree(&allocator);
HLSLParser   parser(&allocator, "shader.hlsl", source, strlen(source));
parser.Parse(&tree);

HLSLBuffer*    buffer = tree.FindBuffer("Params");
CBufferLayout  layout;
if (!layout.Build(buffer, &tree)) {
    // layout.GetError() names the field(s), position(s) and problem.
}

unsigned int registers = layout.GetRegisterCount(); // uniform vec4 Params[registers]
for (unsigned int i = 0; i < layout.GetFieldCount(); ++i) {
    const CBufferLayout::Field& f = layout.GetField(i);
    // f.name, f.offset (float components), f.size (reserved floats),
    // f.arrayStride / f.arrayCount, f.explicitOffset, ...
}
```

`HLSLGenerator` writes the explicit positions back as `: packoffset(...)`;
parsing that HLSL again yields the identical `CBufferLayout`.

### Checks

```sh
bash checks/check-layout.sh   # builds+runs the C++ layout/expression check, then baseline.sh
bash checks/baseline.sh       # original no-packoffset cbuffer output check
```

`checks/layout_check.cpp` evaluates the actual generated GLSL access
expressions (uniform indexing, swizzles, dynamic array indices, struct and
matrix constructors) against independently listed register data, in addition
to asserting the queried offsets/sizes/strides and the diagnostic cases. It
needs only the system C++11 compiler; there is no GPU or shader SDK dependency.


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
