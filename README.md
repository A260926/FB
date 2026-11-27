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

## Predicated destinations (glsl120, Shader Model 3)

The `glsl120` profile translates predicated instructions from vs_3_0 and
ps_3_0 bytecode. A predicated instruction evaluates its full result once
into a temporary, then commits each destination channel only where the
predicate allows it; channels whose predicate is false (or that fall
outside the static write mask) keep their previous value. The predicate
can be the full `p#.xyzw` vector, a replicated component such as `p#.x`,
or either of those negated with `!`.

The supported predicated opcodes are:

- arithmetic: `MOV`, `ADD`, `SUB`, `MAD`, `MUL`, `DP3`, `DP4`, `MIN`, `MAX`
  (vertex and pixel shaders)
- `TEXLDL` (vertex and pixel shaders) and `TEXLDD` (pixel shaders only),
  currently for 2D samplers; the lod/explicit gradients are forwarded and
  the sampler is invoked exactly once per instruction
- `CMP` (pixel shaders only)

The existing stage restrictions are unchanged (`CMP` and `TEXLDD` remain
pixel-shader-only, `TEXLDL` stays available in both), and any other
predicated opcode—including control-flow instructions such as `IF`,
`CALL`, or `RET`—still fails to translate rather than silently ignoring
the predicate. Plain, non-predicated shaders and the other profiles are
unaffected. See `checks/check-predication.sh` for the test coverage.


