This is a self-contained CPU transcoder source export from Basis Universal.
The original `transcoder/` files, `LICENSE`, and `NOTICE` are preserved unchanged.
KTX2 and Zstandard are disabled by compiler flags for this build. No packages,
GPU runtime, encoder, Python runtime, or network access are needed to build it.

`bash task_support/run.sh baseline` compiles the actual transcoder and decodes
the supplied opaque and alpha ETC1S videos into RGBA32, printing pixel and
reference-index observations. A C++17 compiler and Bash are required.

`bash task_support/run.sh target` requires and compiles the new production
header and implementation, then runs the same original whole-frame baseline.
It does not exercise the new playback API. No new tests or examples are requested.
No implementation of that API is included in this starting tree.

`assets/upstream-video.basis` is upstream `webgl/video_test/6.1.01.basis`.
`assets/alpha-video.basis` is a five-frame, 67 by 37 ETC1S video produced by the
same fixed upstream CPU encoder from deterministic RGBA input. It contains
independent RGB and alpha slices and one I-frame followed by four P-frames.
The encoder is only used to produce this asset, and is not a runtime dependency.
