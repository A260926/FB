Shine: fast fixed-point mp3 encoding
====================================

[shine](https://github.com/savonet/shine) is a blazing fast mp3 encoding library implemented in 
fixed-point arithmetic. The library can thus be used to perform super fast mp3 encoding on architectures
without a FPU, such as `armel`, etc.. It is also super fast on architectures with a FPU!

How to use?
-----------

The encoding API should be quite straight forward:

```c
#include <shine/layer3.h>
  
(...)

/* See if samplerate and bitrate are valid */
if (shine_check_config(config.wave.samplerate, config.mpeg.bitr) < 0)
  error("Unsupported samplerate/bitrate configuration.");

/* Initiate encoder */
s = shine_initialise(&config);

/* Number of samples (per channel) to feed the encoder with. */
int samples_per_pass = shine_samples_per_pass(s);

/* All the magic happens here */
while (read(buffer, infile, samples_per_pass)) {
  data = shine_encode_buffer(s,buffer,&written);
  write(data, written);
}

/* Flush and write remaining data. */
data = shine_flush(s,&written);
write(written, data);

/* Close encoder. */
shine_close(s);
```

### Explicit window switching

If a caller already knows where transients are, it can drive the long/short
window decision directly with `shine_encode_buffer_blocks` (planar) or
`shine_encode_buffer_interleaved_blocks`. These take one block type per
granule and channel, laid out granule-major then channel-minor, so a stereo
MPEG-1 frame needs exactly `2 * channels = 4` entries. Types are
`0 = normal`, `1 = start`, `2 = short`, `3 = stop`.

Each channel begins in the normal state. The only legal successors are
`normal -> normal/start`, `start -> short`, `short -> short/stop` and
`stop -> normal/start`. State carries across frames and the two channels are
independent, so the same call may submit different legal sequences for the
left and right channels. The example below submits, across two frames, a
`start -> short` transition on the left and a `normal -> normal` frame on the
right:

```c
#include <shine/layer3.h>

shine_config_t config = {0};
config.wave.channels = PCM_STEREO;
config.wave.samplerate = 48000;
shine_set_config_mpeg_defaults(&config.mpeg);
config.mpeg.mode = STEREO;             /* plain STEREO, not joint stereo */
config.mpeg.bitr = 128;
shine_t s = shine_initialise(&config);
int spp = shine_samples_per_pass(s);

/* Frame 0: both channels normal (entries: g0L g0R g1L g1R). */
const uint8_t frame0[4] = {0, 0, 0, 0};
/* Frame 1: left goes start -> short, right stays normal -> normal. */
const uint8_t frame1[4] = {1, 0, 2, 0};

int16_t *left  = /* spp samples */;
int16_t *right = /* spp samples */;
int16_t *planar[2] = { left, right };

int written;
unsigned char *out = shine_encode_buffer_blocks(s, planar, frame0, 4, &written);
/* ... use out[0..written), if out != NULL ... */

out = shine_encode_buffer_blocks(s, planar, frame1, 4, &written);
/* ... */

/* Flush any buffered frame and close as usual. */
out = shine_flush(s, &written);
shine_close(s);
```

The block entry points only support **MPEG-1** (32, 44.1 and 48 kHz) with
mono or plain `STEREO`. They are not available for MPEG-2/2.5 rates, joint
stereo, dual channel or other modes.

Any invalid request is rejected **before** any PCM is analysed or the encoder
state is changed: an unsupported configuration, a wrong entry count, an
unknown block type, or an illegal transition makes the function return
`NULL` and set `written` to `-1`. A rejected call leaves the encoder exactly
as it was, so a subsequent valid call behaves as if the bad request had never
been made:

```c
int written;
/* Illegal: normal -> short is not an allowed transition. */
if (shine_encode_buffer_blocks(s, planar, frame1 /* {1,0,2,0} */, 4,
                               &written) == NULL) {
  /* written == -1 here; the encoder state is untouched. */
}
```

The returned pointer follows the same lifetime rules as the other encode
functions: it belongs to the library and is valid until the next encode call
or `shine_close`, and `written` is the number of delivered bytes. Because the
encoder keeps a small 32-bit cache between calls, the last few bits of a
frame may only be committed by the following call; consume only the
delivered bytes of each frame and use `shine_flush` for the tail.

How fast is it?
---------------

On a macbook pro (`arm64`/M1 pro, `FPU`, December 30, 2022):

Lame, `88.7x` realtime:
```bash
LAME 3.100 64bits (http://lame.sf.net)
Using polyphase lowpass filter, transition band: 16538 Hz - 17071 Hz
Encoding /tmp/decoded.wav to /tmp/lame.mp3
Encoding as 44.1 kHz j-stereo MPEG-1 Layer III (11x) 128 kbps qval=3
    Frame          |  CPU time/estim | REAL time/estim | play/CPU |    ETA
 12203/12203 (100%)|    0:03/    0:03|    0:04/    0:04|   88.773x|    0:00
---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
   kbps        LR    MS  %     long switch short %
  128.0       32.6  67.4        96.4   1.9   1.7
Writing LAME Tag...done
ReplayGain: -9.3dB
lame -b 128 /tmp/decoded.wav /tmp/lame.mp3  3.55s user 0.05s system 99% cpu 3.609 total
```

Shine, `318.0x` realtime:
```
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:18
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "/tmp/bla.wav" to "/tmp/shine.mp3"
Finished in 00:00:01 (318.0x realtime)
```

### ⚠ The following are outdated tests ⚠

On a [Raspberry Pi](http://www.raspberrypi.org/) (`ARM`, `FPU`):

Lame, `1.8x` realtime:
```bash
pi@raspberrypi ~ $ lame bla.wav bla.mp3
LAME 3.99.5 32bits (http://lame.sf.net)
Using polyphase lowpass filter, transition band: 16538 Hz - 17071 Hz
Encoding bla.wav to bla.mp3
Encoding as 44.1 kHz j-stereo MPEG-1 Layer III (11x) 128 kbps qval=3
    Frame          |  CPU time/estim | REAL time/estim | play/CPU |    ETA
 12987/12987 (100%)|    3:06/    3:06|    3:06/    3:06|   1.8216x|    0:00
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
   kbps        LR    MS  %     long switch short %
  128.0        0.1  99.9        89.1   6.1   4.9
Writing LAME Tag...done
ReplayGain: -10.5dB
```

Shine, `3.6x` realtime:
```bash
pi@raspberrypi ~ $ shineenc bla.wav bla.mp3
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:39
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "bla.wav" to "bla.mp3"
Finished in 00:01:35 (3.6x realtime)
```

On a Google Nexus 5 (`ARM`, `FPU`):

Shine, `14s`, `24.2x` realtime:
```bash
u0_a65@hammerhead:/mnt/sdcard $ shineenc bla.wav bla.mp3
shineenc (Liquidsoap version)
WAVE PCM Data, stereo 44100Hz 16bit, duration: 00:05:39
MPEG-I layer III, stereo  Psychoacoustic Model: Shine
Bitrate: 128 kbps  De-emphasis: none   Original
Encoding "bla.wav" to "bla.mp3"
Finished in 00:00:14 (24.2x realtime)
```

Limitations
-----------

The code for the encoder has been written a long time ago (see below) and 
the only work done on this fork consists of reorganizing the code and making a 
proper shared API out of it. Thus, the encoder may not be exempt of bugs.

Also, the encoding algorithm is rather simple. In particular, it does not
have any Psychoacoustic Model.

A bit of history
----------------

This code was dug out from the dusty crates of those times before internet 
and github. It apparently was created by Gabriel Bouvigne sometime around 
the end of the 20th century. The encoder was converted circa 2001 by Pete 
Everett to fixed-point arithmetic for the RISC OS. Last we know, Patrick 
Roberts had worked on the code to make it multi-platform and more library
oriented. That was around 2006.

You can consult `README.old` and the various source files for more 
informations on this code.
