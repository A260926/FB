# aiff codec

[![GoDoc](https://godoc.org/github.com/go-audio/aiff?status.svg)](https://godoc.org/github.com/go-audio/aiff)
[![Go Report Card](https://goreportcard.com/badge/github.com/go-audio/aiff)](https://goreportcard.com/report/github.com/go-audio/aiff)
[![Coverage Status](https://codecov.io/gh/go-audio/aiff/graph/badge.svg)](https://codecov.io/gh/go-audio/aiff)
[![Build Status](https://travis-ci.org/go-audio/aiff.svg)](https://travis-ci.org/go-audio/aiff)

Support for int PCM data wrapped in aiff format.
AIFC/sowt format supported.
Apple IMA4 (`ima4`) compressed AIFC is supported for 16-bit mono and stereo
files whose COMM chunk precedes SSND.

See [GoDoc](https://godoc.org/github.com/go-audio/aiff) for more details and examples.

## IMA4 length units and seeking

For `ima4` files, `COMM.NumSampleFrames` counts **compressed packet groups**,
not PCM frames, and the public `NumSampleFrames` field preserves that raw
value. One group is one 34-byte packet per channel and decompresses to 64 PCM
frames:

* `PCMLen()` reports the **uncompressed** byte count,
  `groups * 64 * channels * 2`.
* `Duration()` and `SeekPCM` use the **uncompressed** frame count,
  `groups * 64`.
* The physical size of a group is `34 * channels` bytes; the SSND `blockSize`
  field is only an alignment hint and is ignored for length purposes.

`SeekPCM(frame int64) error` positions the decoder at an uncompressed frame
(zero-based) and is only available for `ima4`. Exact positioning is not
constant time: packet headers store only the top 9 bits of the predictor, so
the decoder replays packet groups from the start of the sound data to rebuild
the exact state rather than restarting from an approximate header predictor.
Seeking to the total frame count reports end of file; seeking back to a valid
frame afterwards replays from the beginning again.

The original `Seek(offset, whence)` still positions the underlying reader by
raw bytes. Mixing arbitrary raw-byte seeks with compressed `ima4` PCM reads is
not supported; use `SeekPCM` (or `Rewind`) instead.

