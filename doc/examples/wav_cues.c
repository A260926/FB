/*
	wav_cues.c: write a WAV file with named chapter marks

	This example plays a short, nonzero signal into a WAV file through the
	optional output buffer and registers named chapters while doing so. Run it
	with the name of the output file as the single argument.

	The chapter marks end up as standard cue and LIST/adtl chunks that audio
	editors can display as chapter markers.

	copyright 2026 by the mpg123 project - free software under the terms of
	the LGPL 2.1, see COPYING and AUTHORS files in distribution or
	http://mpg123.org
*/

#include "out123.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE     44100
#define CHANNELS 2
#define FRAMES   44100  /* one second */
#define HALF     (CHANNELS*FRAMES/2)

int main(int argc, char **argv)
{
	out123_handle *ao;
	short pcm[CHANNELS*FRAMES];
	const size_t halfbytes = sizeof(short)*HALF;
	int i;
	const char *outfile;

	if(argc != 2)
	{
		fprintf(stderr, "Usage: %s <output.wav>\n", argv[0]);
		return 1;
	}
	outfile = argv[1];

	/* A nonzero signal: left channel a tone, right channel a different one. */
	for(i=0; i<FRAMES; ++i)
	{
		pcm[2*i+0] = (short)(12000*sin(2.0*M_PI*440.0*i/RATE));
		pcm[2*i+1] = (short)(9000*sin(2.0*M_PI*660.0*i/RATE));
	}

	ao = out123_new();
	if(!ao)
	{
		fprintf(stderr, "Out of memory.\n");
		return 1;
	}
	/* Use the buffered output like a real application would. The chapter
	   positions still cover audio that is only queued in the buffer. */
	if(  out123_set_buffer(ao, 65536)
	  || out123_open(ao, "wav", outfile)
	  || out123_start(ao, RATE, CHANNELS, MPG123_ENC_SIGNED_16) )
	{
		fprintf(stderr, "Cannot set up WAV output: %s\n", out123_strerror(ao));
		out123_del(ao);
		return 1;
	}

	/* A chapter at the very start, then one after half a second of audio. */
	if(  out123_wav_cue(ao, 1, "Intro")
	  || out123_play(ao, pcm, halfbytes) != halfbytes
	  || out123_wav_cue(ao, 2, "Verse")
	  || out123_play(ao, pcm + HALF, halfbytes) != halfbytes
	  || out123_wav_cue(ao, 3, "Outro") )
	{
		fprintf(stderr, "Playback failed: %s\n", out123_strerror(ao));
		out123_del(ao);
		return 1;
	}

	/* Stop drains all audio and finalises the file, appending the chapters. */
	out123_stop(ao);
	if(out123_errcode(ao))
	{
		fprintf(stderr, "Stopping failed: %s\n", out123_strerror(ao));
		out123_del(ao);
		return 1;
	}
	out123_del(ao);
	printf("Wrote %s with %d frames and named chapters.\n", outfile, FRAMES);
	return 0;
}
