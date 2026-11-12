# Offline HDR raster seed

Original C++17 CPU renderer for deterministic offline sequences. No dependencies.
Run `bash tools/current.sh` for the existing camera, G-buffer and image sampling checks.

`draw` accepts opaque triangles wholly in front of the near plane; partial near clipping,
transparency, shared-edge watertight rasterization and full material models are outside
this seed. A translated local triangle can have separate current and previous offsets.
The camera uses a right-handed basis with positive forward view depth. Pixel centers
are `(x+.5,y+.5)`, origin at upper left. Jitter is an additive projection displacement
in pixel units. Motion is previous **unjittered** projection minus current unjittered
projection, also in pixels. `previous_depth` is the same surface point's depth in the
previous camera, before jitter. Invalid previous projection sets depth zero and motion
outside the viewport. `object==0` is background. Color is linear radiance multiplied
by `Frame.exposure`; values above one remain intact until PPM output tone mapping.

`temporal.hpp` adds `TemporalAccumulator::resolve(frame, settings)` on top of the
unmodified rasterizer. Reprojection adds the two frames' jitter difference to the
unjittered motion vector. History color is bilinearly rebuilt from four texels, each
validated independently by object id and relative depth, with the surviving weights
renormalized. Validated history is exposure-rescaled, clamped in YCoCg against the
current 3x3 same-object neighborhood, and linearly blended with `history_weight`
(default 0.9). No clamping to [0,1], gamma, or tone mapping is applied. First frame,
size change, `camera_cut`, non-consecutive `sequence`, `reset()`, and background
(`object==0`) pixels use the current color only. Run `bash tools/check-temporal.sh <dir>`
for the temporal checks and PPM outputs.

Camera project/unproject, depth and varying raster interpolation, Lambert/checker
material shading, bilinear image sampling and PPM display conversion are independent
working modules. PPM writes only to caller supplied paths.
