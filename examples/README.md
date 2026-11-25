## Example: `examples/theora_edit.c`

Re-cuts an Ogg/Theora file without re-encoding. The example links against the
plan API in `include/ogg/theora_edit.h` (`src/theora_index.c`,
`src/theora_edit.c`) and the bundled libogg.

A cut is a zero-based logical stream number plus a half-open frame range
`[begin,end)`. `begin` must land on a keyframe; several cuts are concatenated
in the order given.

### Build

With the task build already configured (`.task-build/ogg/libogg.a`):

```sh
cc -std=c99 -Iinclude -I.task-build/ogg/include \
   examples/theora_edit.c src/theora_index.c src/theora_edit.c \
   .task-build/ogg/libogg.a -o theora_edit
```

### Run

```sh
./theora_edit tests/fixtures/frames.ogv out.ogv 0 0 2
```

This writes the first two frames (`frames.ogv` has logical stream `0`), with
the stream headers copied verbatim and the output clock rebased to start at
zero. Multiple cuts can be passed, in any order:

```sh
./theora_edit in.ogv out.ogv 0 8 12 1 0 3 0 4 6
```

Exit status is `0` on success and non-zero on failure.
