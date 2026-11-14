# MojoShader

https://icculus.org/mojoshader/

MojoShader is a zlib-licensed library to work with Direct3D shaders on
alternate 3D APIs and non-Windows platforms. The primary motivation is
moving shaders to OpenGL languages on the fly. The developer deals with
"profiles" that represent various target languages, such as GLSL or
ARB_*_program.

This allows a developer to manage one set of shaders, presumably written
in Direct3D HLSL, and use them across multiple rendering backends. This
also means that the developer only has to worry about one (offline)
compiler to manage program complexity, while MojoShader itself deals with
the reduced complexity of the bytecode at runtime.

MojoShader provides both a simple API to convert bytecode to various
profiles, and (optionally) basic glue to rendering APIs to abstract the
management of the shaders at runtime.

The library is meant to be statically linked to an application (just add a
few .c files and headers to your build), allows the app to optionally
specify an allocator, and is thread safe (although OpenGL itself is not).
It is meant to be embedded in games with a minimum of fuss. 

To use this in your project:

- Add mojoshader*.c and mojoshader*.h to your project.
- Compile mojoshader*.c
- If you don't have a C99-compliant compiler, like Microsoft Visual Studio,
  you'll need to compile the .c files as C++ to get them to build.
- If you don't have cmake to generate mojoshader_version.h, you can either
  add a blank file with that name, or add MOJOSHADER_NO_VERSION_INCLUDE to
  your preprocessor definitions.

## MojoShader_assembler

This library used to have an preprocessor, parser, and assembler for
low-level D3D shader assembly language. These have been removed from
MojoShader, and moved over to a new repo for those that need it:

https://github.com/icculus/mojoshader_assembler

## Predicated SM3 instructions (glsl120 profile)

The `glsl120` profile translates destination-predicated Shader Model 3.0
instructions from legacy vertex and pixel shaders, so old assembly materials
keep working through `MOJOSHADER_parse()`. A predicate register (`p0`) is a
four-component boolean vector: without a swizzle each destination channel is
gated by its matching predicate component, a replicate swizzle (e.g.
`p0.x`) broadcasts one component to every channel, and `!` negates each
component. The static write mask and the predicate together decide the final
write; every channel that is masked off or predicate-closed keeps its
previous value.

Supported predicated opcodes, only in their originally legal stages, are:

- Vertex and pixel: `MOV`, `ADD`, `SUB`, `MUL`, `MAD`, `MIN`, `MAX`, `DP3`,
  `DP4`, `SETP`, and `TEXLDL`.
- Pixel only: `CMP` and `TEXLDD`.

Predicated texture fetch currently supports 2D samplers only. The whole
candidate result (including a texture sample) is evaluated once into a
block-local temporary and then committed per channel, so a texture is never
sampled more than once and source/destination register aliasing is evaluated
against the pre-instruction state. All other predicated instructions,
including control flow, are rejected explicitly rather than having their
predicate ignored. Non-predicated shaders, the other profiles, and the
reflection APIs are unchanged. Output stays within GLSL 1.20 (no
boolean-vector `mix`, which requires a later version).

The CPU-only checks live in `checks/`: `sh checks/baseline.sh` runs the
original non-predicated baseline, and `sh checks/check-predication.sh` builds
and runs the predicated translation tests plus an independent register/texture
execution model. No GPU, Python, or extra libraries are required.

