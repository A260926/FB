# cgltf_retime

`examples/cgltf_retime.h` / `examples/cgltf_retime.c` add one entry point on top
of cgltf: re-time a single animation with a piecewise-linear clock, leaving the
motion trajectory untouched.

```c
cgltf_result cgltf_retime_animation(const cgltf_data *source,
                                    cgltf_size animation_index,
                                    const cgltf_float *source_times,
                                    const cgltf_float *output_times,
                                    cgltf_size count,
                                    cgltf_data **result);
```

## What it does

`source_times` and `output_times` are corresponding knots of a piecewise-linear
map from source seconds to output seconds:

- both arrays must be finite and strictly increasing,
- `count >= 2`,
- `output_times[0]` must be `0`,
- the first/last `source_times` select a span of the chosen animation and must
  stay inside the union of that animation's sampler input ranges,
- interior knots are speed-change boundaries.

The result is a fresh asset that the caller releases with `cgltf_free`. The
input is not modified. Interpolation stays STEP / LINEAR / CUBICSPLINE; nothing
is baked at a fixed frame rate. Every source key inside the span is kept, the
map knots and span endpoints are added, and pre-existing keys keep their values.
Outside a channel's own key span its value is constant.

Other animations, node hierarchy, array indices, skins (joints and
inverse-bind matrices), meshes and morph targets are copied unchanged. All
buffers of the result are self-contained data URIs, so the result stays valid
after the source is freed and after writing/reloading it.

## Details worth knowing

- Weights are treated as flattened SCALAR data, `targets_count` values per key.
- Accessor reads honour `byteOffset`, `byteStride` and sparse substitution;
  `min`/`max` are never used in place of real values.
- LINEAR rotation uses shortest-arc slerp; inserted rotation keys are unit
  length. CUBICSPLINE rotation keeps the glTF component Hermite semantics and
  normalized sampling.
- Cubic-spline translations, scales and weights store per-key in/out tangents
  relative to the new seconds. When a key is also a time-map break it gets
  different incoming and outgoing tangents.
- Because a normalized quaternion key cannot reproduce a cubic rotation Hermite
  arc exactly, every interior `source_times` knot must coincide with an
  existing key of each cubic rotation sampler, otherwise
  `cgltf_result_invalid_options` is returned. Constant extension outside the
  sampler's key span is allowed.
- Duplicate `(node, path)` targets, malformed key sequences or output shapes,
  and unreadable buffer data are errors. Compressed buffers (meshopt, Draco)
  are rejected with `cgltf_result_unknown_format`.
- On failure `*result` is `NULL`.

## Minimal usage

```c
cgltf_options options = {0};
cgltf_data *source = NULL, *result = NULL;
cgltf_float source_times[3] = {0.0f, 1.0f, 2.0f};
cgltf_float output_times[3] = {0.0f, 0.4f, 2.6f};

cgltf_parse_file(&options, "in.gltf", &source);
cgltf_load_buffers(&options, source, "in.gltf");

cgltf_result status = cgltf_retime_animation(source, 0, source_times,
                                             output_times, 3, &result);
if (status == cgltf_result_success)
{
    cgltf_free(source);              /* result owns all of its bytes */
    cgltf_validate(result);
    cgltf_write_file(&options, "out.gltf", result);  /* embedded data URIs */
    cgltf_free(result);
}
```

A complete program is in `examples/cgltf_retime_example.c`:

```
cc -std=c99 -I. examples/cgltf_retime_example.c examples/cgltf_retime.c \
   task_support/cgltf_impl.c -lm -o retime_example
./retime_example in.gltf out.gltf
```

The implementation is C99 and only needs the C library plus libm. It is meant
to be compiled as a separate translation unit from the one that defines
`CGLTF_IMPLEMENTATION` / `CGLTF_WRITE_IMPLEMENTATION`.
