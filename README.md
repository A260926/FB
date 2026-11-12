![QOA Logo](https://qoaformat.org/qoa-logo-new.svg)

# QOA - The “Quite OK Audio Format” for fast, lossy audio compression

Single-file MIT licensed library for C/C++

See [qoa.h](https://github.com/phoboslab/qoa/blob/master/qoa.h) for
the documentation and format specification.

More info at: https://qoaformat.org

Audio samples in WAV & QOA format can be found at: https://qoaformat.org/samples/


⚠️ This implementation has not yet been fuzzed. Don't use it with untrusted input.


## Compiling

Call `make` to build `qoaconv` and `qoaplay`. By default `qoaconv` is compiled 
without MP3 and FLAC support. 

To compile `qoaconv` with MP3 and FLAC support, download the 
[dr_*.h files](https://github.com/mackron/dr_libs) and pass `HAS_DRLIBS=true`
to make:

```bash
curl https://raw.githubusercontent.com/mackron/dr_libs/refs/heads/master/dr_mp3.h -o dr_mp3.h
curl https://raw.githubusercontent.com/mackron/dr_libs/refs/heads/master/dr_flac.h -o dr_flac.h
make HAS_DRLIBS=true
```


## Beam search encoder

The regular `qoa_encode` / `qoa_encode_frame` pick a scalefactor for each slice
immediately and never revisit it. Because every slice changes the LMS state, a
scalefactor that looks best for one slice can be suboptimal for the slices that
follow. For offline compression where extra CPU is acceptable, `qoa.h` also
provides a beam search encoder that delays the decision until the end of a
frame:

```c
void *qoa_encode_beam(const short *sample_data, qoa_desc *qoa,
    unsigned int *out_len, unsigned int beam_width);

unsigned int qoa_encode_frame_beam(const short *sample_data, qoa_desc *qoa,
    unsigned int frame_len, unsigned char *bytes, unsigned int capacity,
    unsigned int beam_width);
```

`beam_width` ranges from 1 to 16. The search unit is one channel of one frame.
At every slice, each retained candidate LMS state is expanded with all 16
scalefactors; within a fixed scalefactor the original per-sample quantization,
reconstruction and LMS update rules are unchanged. Only the `beam_width` best
paths survive each slice, ranked by the cumulative squared error of the
reconstructed PCM measured from the start of the frame (the weight penalty is
not part of this metric); equal errors break toward the lexicographically
smaller scalefactor sequence. Keeping one state per scalefactor, or choosing
each slice independently and splicing the results, is not sufficient — the same
scalefactor produces different predictions from different histories, so full
paths (state plus scalefactor prefix) are retained.

A separate path runs the original greedy strategy — per-slice selection with
the weight penalty — independently from the same normalized frame-header state,
without inheriting any pruned search node. It is always evaluated as a complete
candidate, and the final choice is made on true reconstructed-PCM SSE: the beam
result's error can never exceed the greedy control's error, and an exact tie
selects the greedy path. Each channel is searched independently and the slices
are written back in the usual interleaved layout. `qoa_encode_beam` still
emits an ordinary QOA file, so any existing QOA decoder can read it.

All decoder-consumed arithmetic (the prediction products and accumulation in
their original order, the reconstruction and the weight updates) is checked to
fit in signed 32 bit using explicit wide-integer operations; a candidate that
does not fit is discarded rather than written, and the cost accumulators are
overflow-checked as well. If neither the search nor the greedy control yields a
complete safe path, encoding fails cleanly. Frame-to-frame state is normalized
to the signed 16 bit values actually stored in each frame header before the next
frame is searched.

The frame entry accepts `frame_len` from 1 to 5120, returns the number of bytes
written (0 on any failure), and only updates the LMS in `qoa` on success. The
whole-file entry returns a buffer released with `QOA_FREE`, matching
`qoa_encode`.


## Alternative Implementations of QOA

- [pfusik/qoa-fu](https://github.com/pfusik/qoa-fu) - Fusion, transpiling to
[C](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.c),
[C++](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.cpp),
[C#](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.cs),
[D](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.d),
[Java](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOADecoder.java),
[JavaScript](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.js),
[Python](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.py),
[Swift](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.swift)
and [TypeScript](https://github.com/pfusik/qoa-fu/blob/master/transpiled/QOA.ts)
- [qoa-format](https://github.com/mattdesl/qoa-format) - JavaScript encoder/decoder
- [JohannesFriedrich/qoa4R](https://github.com/JohannesFriedrich/qoa4R) - R
- [rafaelcaricio/qoaudio](https://github.com/rafaelcaricio/qoaudio) - Pure Rust zero-dependency decoder implementation
- [AuburnSounds/audio-formats](https://github.com/AuburnSounds/audio-formats) - D library, supports QOA
- [braheezy/goqoa](https://github.com/braheezy/goqoa) - Go library and CLI tool
- [HaxelWorks/qoa-python](https://github.com/HaxelWorks/qoa-python) - Python wrapper using cffi
- [Cl Qoa](https://shinmera.github.io/cl-qoa/) - Common Lisp
- [Resona QOA](https://github.com/MatusOllah/resona/tree/main/codec/qoa) - Pure Go decoder and encoder implementation
- [QOAMAX](https://github.com/BlobCodes/qoamax) - High-Quality QOA Encoder/Decoder Library and CLI tool

## QOA Support in Other Software

- [Godot Engine](https://godotengine.org) - supports compressing WAV files into QOA since 4.3
- [raylib](https://github.com/raysan5/raylib) - supports decoding QOA samples through its [raudio module](https://github.com/raysan5/raylib/blob/master/src/raudio.c)
- [SerenityOS](https://github.com/SerenityOS/serenity) supports QOA system wide through [QOALoader.h](https://github.com/SerenityOS/serenity/blob/master/Userland/Libraries/LibAudio/QOALoader.h)
- [Qmmp](https://github.com/TTK-qmmp/qmmp-qoa) - supports decoding QOA samples
- [Visual Studio Code](https://github.com/microsoft/vscode): supports playing QOA files with the [QOA Preview extension](https://github.com/braheezy/vscode-qoa-preview)
- [OpenCubicPlayer](https://github.com/mywave82/opencubicplayer) - supports playing QOA files
- [file](https://github.com/file/file): identify QOA files
  
