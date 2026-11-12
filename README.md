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
No history, temporal reuse, or temporal filtering is implemented.

Camera project/unproject, depth and varying raster interpolation, Lambert/checker
material shading, bilinear image sampling and PPM display conversion are independent
working modules. PPM writes only to caller supplied paths.
