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

Streaming encoder for audio callbacks
-------------------------------------

`LTCStreamEncoder` generates LTC samples on demand, without allocating
memory or keeping a whole frame of PCM internally.  It is intended for
fixed-size audio callbacks (1 sample, 257 samples, 1024 samples, ...) and
low-memory hosts.

Pulling samples from the stream encoder produces the same audio as
encoding whole frames with `LTCEncoder` for the same settings.  Chunk
boundaries may fall anywhere — on a bit edge, halfway through a bit cell,
or in the middle of a filtered edge — and the concatenated output is
always identical.

```c
#include <ltc.h>
#include <string.h>

static LTCStreamEncoder *g_ltc;

/* Call once, from a non-realtime thread. */
void ltc_callback_init(double sample_rate)
{
	SMPTETimecode start;
	memset(&start, 0, sizeof(start));
	strcpy(start.timezone, "+0000");
	start.years  = 24;
	start.months = 1;
	start.days   = 1;
	start.hours  = 0;
	start.mins   = 0;
	start.secs   = 0;
	start.frame  = 0;

	/* +1 forwards, -1 for reverse playback.  Fixed for the lifetime. */
	g_ltc = ltc_stream_encoder_create(
			sample_rate, 25,      /* sample rate, fps (24/25/30/29.97) */
			LTC_TV_625_50, 0,     /* TV standard, flags */
			&start, +1,           /* start timecode, direction */
			-3.0,                 /* volume in dBFS */
			40.0);                /* rise time in us, 0 = square wave */
}

/* Audio callback: no allocation, any chunk size, unsigned 8 bit mono. */
void audio_callback(ltcsnd_sample_t *out, size_t nframes)
{
	/* Writes exactly nframes; advances the timecode per frame. */
	ltc_stream_encoder_read(g_ltc, out, nframes);
}

/* Optional: change the user bits of the next frame (rejected with
 * LTC_USE_DATE, which stores the date in the user bits). */
void set_next_user_bits(unsigned long bits)
{
	ltc_stream_encoder_set_user_bits(g_ltc, bits);
}

/* Query the timecode of the next sample to be output. */
void get_timecode(SMPTETimecode *tc)
{
	ltc_stream_encoder_get_timecode(g_ltc, tc);
}

/* Call once, from a non-realtime thread. */
void ltc_callback_cleanup(void)
{
	ltc_stream_encoder_free(g_ltc);
	g_ltc = NULL;
}
```

`ltc_stream_encoder_read` returns `0` on success (all samples written) and
`-1` on invalid arguments (a NULL handle, or a NULL buffer with a non-zero
count).  A zero count is a no-op and accepts a NULL buffer.  The caller
owns the encoder returned by `ltc_stream_encoder_create` and releases it
with `ltc_stream_encoder_free`; `create` returns NULL for invalid
parameters.

