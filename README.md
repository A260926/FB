FFTConvolver
============

FFTConvolver is a C++ library for highly efficient convolution of
audio data (e.g. for usage in real-time convolution reverbs etc.).

- Partitioned convolution algorithm (using uniform block sizes)
- Optional support for non-uniform block sizes (TwoStageFFTConvolver)
- No external dependencies (FFT already included)
- Optional optimization for SSE (enabled by defining FFTCONVOLVER_USE_SSE)
- Self-contained state snapshots for saving/restoring a running convolver
  (see below)


State snapshots (exportState / importState)
-------------------------------------------

Both `FFTConvolver` and `TwoStageFFTConvolver` offer

```cpp
bool exportState(std::vector<uint8_t>& out) const;
bool importState(const uint8_t* data, size_t size);
```

A snapshot contains the impulse response *and* the complete processing
state (accumulated overlap, frequency-domain input history and, for the
two-stage convolver, all pre-calculated tail blocks and their timing).
After importing, the convolver continues at exactly the absolute sample
position where the snapshot was taken: feeding the same signal produces
the same output samples, with no added or lost partition of latency. The
original instance may be destroyed and the original impulse-response
memory released before importing; nothing in the snapshot references
process addresses, object registries or external files.

### Guarantees

- `exportState` does not process audio. Two consecutive exports of the
  same instance with no `process()` call in between produce byte-identical
  output.
- `importState` works on a freshly constructed object or on one that has
  already processed another signal (its block size may differ too).
- On any corrupt or incompatible input, `importState` returns `false` and
  leaves the target, its current impulse response and its sound completely
  untouched. All lengths are validated against the number of bytes present
  before any memory is allocated, so no read can go out of bounds and no
  allocation can exceed the input blob.
- The real-time contract of `process()` is unchanged: it performs no heap
  allocation or deallocation and adds no extra block latency, including for
  the first block after an import. Export/import themselves may allocate.
- Snapshot size and export/import work scale with the impulse-response
  length and partition sizes only, never with the number of samples
  processed since start; no input history is accumulated.
- Subclasses that override the background-processing hooks of
  `TwoStageFFTConvolver` are not supported by the snapshot interface.

### Wire format

The format is defined independently of any in-memory C++ layout, so it
never dumps struct padding or pointers.

- **Version:** format version `1` (single byte).
- **Byte order:** all multi-byte integers are unsigned **little-endian**.
- **Sample representation:** every sample is an IEEE-754 binary32 float
  (the project's `float` `Sample` type), written as its exact 32-bit bit
  pattern in little-endian order. No denormal/NaN/Inf value is accepted on
  import; every stored sample must be finite.
- **Length rules:** every variable-length array is prefixed by a `uint32`
  element count (not a byte count), followed by that many floats. Every
  section is prefixed by a one-byte tag and a `uint32` body length. A
  reader verifies each declared length against the bytes actually available
  before allocating storage. The outer payload length must match the file
  size exactly; trailing bytes are rejected.

File layout:

```
magic        4 bytes  "FCV1" (FFTConvolver) or "F2S1" (TwoStageFFTConvolver)
version      uint8    currently 1
class        uint8    1 = FFTConvolver, 2 = TwoStageFFTConvolver
reserved     uint16   must be 0
payloadLen   uint32   number of bytes following this header
payload      sections, each: tag(uint8) bodyLen(uint32) body(bytes)
```

The distinct magic values make the two classes mutually incompatible: an
`FFTConvolver` snapshot cannot be imported by a `TwoStageFFTConvolver` and
vice versa.

`FFTConvolver` section body (tag 1):

```
flags            uint32   bit 0 set => empty state, body ends here
blockSize        uint32   power of two
segCount         uint32   number of partitions, > 0
current          uint32   ring index, < segCount
inputFill        uint32   valid samples in the partial block, <= blockSize
ir               floats   trimmed impulse response;
                          (segCount-1)*blockSize < count <= segCount*blockSize
segments         floats   segCount * 2 * (blockSize+1), per partition
                          [real(blockSize+1)][imag(blockSize+1)]
preMultiplied    floats   2 * (blockSize+1)
overlap          floats   blockSize
inputBuffer      floats   blockSize
```

`TwoStageFFTConvolver` payload: one body section (tag 2) followed by nested
`FFTConvolver` sections for the head (tag 3), the first tail stage (tag 4,
if present) and the long tail (tag 5, if present). The body stores the
block sizes, the equal `tailInputFill`/`precalculatedPos` positions, the
full trimmed impulse response and the seven tail sample buffers
(`tailOutput0`, `tailPrecalculated0`, `tailOutput`, `tailPrecalculated`,
`tailInput`, `backgroundProcessingInput`, each with its own count). The
nested sections' impulse responses must match the corresponding slices of
the full impulse response, which is verified on import.

### Edge states

- **Uninitialized object** and **just-`reset()` object** export the same
  small empty snapshot; importing it yields an object that outputs silence,
  and it can also clear an already working instance.
- **Empty / all-zero impulse response** is trimmed to length zero during
  `init()` and is therefore identical to the empty state above.
- **Trailing zero segment trimmed by the `init()` threshold**
  (`|sample| < 1e-6`) is not stored: a convolver initialized from a padded
  impulse response exports exactly what one initialized from the trimmed
  response exports.
- Every non-empty snapshot ends in a stored impulse-response sample whose
  magnitude is at least the trim threshold, matching what `init()` would
  retain.

### Rejected inputs

Import returns `false` (without modifying the target) for truncated blobs,
unknown versions or class tags, a non-zero reserved field, payload/section
lengths that contradict the actual size or each other, block sizes that are
not a power of two, indices outside their valid ranges, impulse-response
lengths inconsistent with the partition count or two-stage presence flags,
and any non-finite sample. Random garbage and snapshots of the wrong class
are likewise rejected.
