libLTC
------

Linear (or Longitudinal) Timecode (LTC) is an encoding of SMPTE timecode data
as a Manchester-Biphase encoded audio signal.
The audio signal is commonly recorded on a VTR track or other storage media.

libltc provides functionality to encode and decode LTC audio from/to
SMPTE or EBU timecode, including SMPTE date support.

libltc is the successor of [libltcsmpte](https://sourceforge.net/projects/ltcsmpte/).
For more information, please see the FAQ in the documentation.

Documentation
-------------

The API reference, examples, as well as introduction can be found at

https://x42.github.io/libltc/

This site is part or the source-code in the doc/ folder.

Streaming encoder for fixed-size audio callbacks
------------------------------------------------

`LTCEncoder` encodes a whole frame into an internal PCM buffer and the
host then copies it out and keeps track of the remainder. When the audio
backend asks for an arbitrary number of samples (1, 257, 1024, ...) at a
time, that bookkeeping is inconvenient and the extra frame buffer is
wasteful.

`LTCStreamEncoder` generates exactly the samples that are requested, in
place, without allocating memory inside `ltc_stream_encoder_read()` and
without buffering a frame:

```c
#include <string.h>
#include <ltc.h>

/* fixed-size audio callback state - no heap use while streaming */
typedef struct {
    LTCStreamEncoder *encoder;
} LtcOutput;

int ltc_output_init(LtcOutput *out)
{
    SMPTETimecode start = {0};

    strcpy(start.timezone, "+0000");
    start.years  = 24;
    start.months = 2;
    start.days   = 28;
    start.hours  = 12;
    start.mins   = 0;
    start.secs   = 0;
    start.frame  = 0;

    /* 48 kHz, 25 fps, EBU 625/50, no flags, forward, -3 dBFS, 40 us */
    out->encoder = ltc_stream_encoder_create(
            48000, 25, LTC_TV_625_50, 0, &start, 1, -3.0, 40.0);
    return out->encoder ? 0 : -1;
}

void ltc_output_free(LtcOutput *out)
{
    ltc_stream_encoder_free(out->encoder);
    out->encoder = NULL;
}

/* Called by the audio device with nframes in [1, 1024]. */
int ltc_output_process(LtcOutput *out, unsigned char *pcm, size_t nframes)
{
    /* writes exactly nframes 8-bit unsigned mono samples */
    return ltc_stream_encoder_read(out->encoder, pcm, nframes);
}

/* Optional, e.g. once per video frame, to label the stream. Only valid
 * for an encoder without LTC_USE_DATE. May be called from another thread
 * only if the host synchronizes it with the audio callback. */
int ltc_output_set_user_bits(LtcOutput *out, unsigned long bits)
{
    return ltc_stream_encoder_set_user_bits(out->encoder, bits);
}

/* The timecode owning the next sample to be written. It advances to the
 * next frame only after the last sample of the current one went out. */
void ltc_output_now(LtcOutput *out, SMPTETimecode *tc)
{
    ltc_stream_encoder_get_timecode(out->encoder, tc);
}
```

The same settings produce byte-identical PCM to the legacy encoder:
`ltc_encoder_encode_frame()` followed by `ltc_encoder_copy_buffer()` for
forward playback (and `ltc_encoder_encode_reversed_frame()` /
`ltc_encoder_dec_timecode()` for direction `-1`). Splitting the output
anywhere - on a bit edge, half a bit, or in the middle of a filtered
edge - yields the same continuous stream, and `ltc_stream_encoder_read`
is safe to call from a realtime callback.

Unlike the legacy `LTCEncoder` setter, `ltc_stream_encoder_set_user_bits`
also refreshes the frame's parity bit (unless `LTC_NO_PARITY` is set) so
that a frame's first and second half always share one coherent 80-bit
word with a valid correction/flag bit. With `LTC_USE_DATE` the user
fields hold the date, so the call is refused.

