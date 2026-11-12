# vorbis

a native go vorbis decoder

Note that this package can only decode raw vorbis packets, for reading .ogg files, use [oggvorbis](https://github.com/jfreymuth/oggvorbis)

## usage

Read the three header packets (identification, comment, setup) with
`ReadHeader`, then decode audio packets with either:

- `Decode`, which allocates and returns a fresh interleaved `[]float32` for
  every packet. The returned slice is owned by the caller and remains valid
  across later calls.
- `DecodeInto`, which writes into a caller-supplied buffer of at least
  `BufferSize()` samples and returns a subslice of it. After the decoder has
  been warmed up (headers read and one packet decoded), `DecodeInto` performs
  **zero heap allocations per packet**. All scratch space — per-channel floor
  results, residue submap lists and classification vectors — is owned by the
  `Decoder`, allocated when the setup header is read (or lazily grown on first
  use), and reused for every subsequent packet. This makes `DecodeInto`
  suitable for real-time, low-latency decoding.

Ownership and lifetime:

- The caller owns the buffer passed to `DecodeInto` and every slice returned
  by `Decode`. The decoder never frees or reallocates them; it only writes
  into the buffer for the duration of the call.
- The decoder owns all internal workspaces and the overlap history. They stay
  valid for the decoder's whole lifetime and are reused across packets. Two
  separate `Decoder` values are fully independent. A single decoder must not
  be used concurrently.
- Call `Clear` between two non-consecutive packet sequences. It drops the
  overlap history and re-primes the decoder, but keeps the allocated
  workspace capacity, so decoding stays allocation-free afterwards.
- A call that fails because headers are missing or because the output buffer
  is too short returns an error without consuming any overlap history; the
  next valid call behaves as if the failed call had never happened.

[godoc](https://godocs.io/github.com/jfreymuth/vorbis)

[The vorbis specification](https://xiph.org/vorbis/doc/Vorbis_I_spec.html)
