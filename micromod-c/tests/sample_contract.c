
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "micromod.h"
#include "check.h"
#include "fixture.h"

/*
	Contract tests for the instance-based player and the sample DMA / funk
	mode. Synthetic modules are built with the task_support fixture helpers;
	every assertion below checks interleaved stereo PCM (or source bytes),
	never internal player fields.

	All notes use the neutral finetune (header nibble 0), so an encoded key P
	produces replay period P. At 8287hz, key 428 steps one sample byte per
	frame, key 856 half a byte per frame and key 214 two bytes per frame. A
	single unmuted channel 0/2 mixes at exactly 128 times the sample byte
	into the left/right output.
*/

#define RATE 8287L
#define TICK 165L              /* tick_len at 125 bpm, 8287hz. */
#define ROW ( 6 * TICK )       /* 990 frames at the default speed of 6. */

static void mod_head( signed char *m, int orders, int patterns ) {
	int i;
	memset( m, 0, 1084 + patterns * 1024 );
	memcpy( m, "Public synthetic MOD", 20 );
	m[ 950 ] = (signed char) orders;
	memcpy( m + 1080, "M.K.", 4 );
	for( i = 0; i < orders; ++i ) m[ 952 + i ] = (signed char) i;
}

static long sample_base( int patterns ) {
	return 1084L + patterns * 1024L;
}

static void mod_inst( signed char *m, int patterns, int idx, int length, int volume,
		int loop_start, int loop_length, int fill_seed ) {
	int h = idx * 30;
	long off;
	int i;
	fixture_word( m + h + 12, length / 2 );
	m[ h + 14 ] = 0;                                           /* neutral finetune (8). */
	m[ h + 15 ] = (signed char) volume;
	fixture_word( m + h + 16, loop_start / 2 );
	fixture_word( m + h + 18, loop_length / 2 );
	off = sample_base( patterns );
	for( i = 1; i < idx; ++i )
		off += ( m[ i * 30 + 12 ] << 8 | ( m[ i * 30 + 13 ] & 0xFF ) ) * 2L;
	for( i = 0; i < length; ++i )
		m[ off + i ] = (signed char)( ( fill_seed + i ) & 0x7F );
}

static long inst_offset( signed char *m, int patterns, int idx ) {
	long off = sample_base( patterns );
	int i;
	for( i = 1; i < idx; ++i )
		off += ( m[ i * 30 + 12 ] << 8 | ( m[ i * 30 + 13 ] & 0xFF ) ) * 2L;
	return off;
}

static long mod_len( signed char *m, int patterns, int last_inst ) {
	return inst_offset( m, patterns, last_inst )
		+ ( m[ last_inst * 30 + 12 ] << 8 | ( m[ last_inst * 30 + 13 ] & 0xFF ) ) * 2L;
}

static void render( micromod_player *p, short *buf, long frames ) {
	memset( buf, 0, (size_t) frames * 2 * sizeof( short ) );
	micromod_player_get_audio( p, buf, frames );
}

/* Listen to one channel alone: 0/2 are full left/right, 1/3 are right/left. */
static void solo( micromod_player *p, int channel ) {
	int c;
	micromod_player_mute_channel( p, -1 );
	for( c = 0; c < 4; ++c )
		if( c != channel ) micromod_player_mute_channel( p, c );
}

/* ----------------------------------------------------------------------- */
/* Group 1: first segments, loop reloads, carries, delay and retrig.       */
/* ----------------------------------------------------------------------- */

void contract_dma( void ) {
	signed char m[ 4096 ];
	signed char src[ 4096 ];
	micromod_player *p;
	short buf[ 6000 * 2 ];
	long len, f;
	mod_head( m, 1, 1 );
	/* I1: 64 bytes, no valid loop. I2: 48 bytes, loop [16,32).
	   I3: 32 bytes, loop [0,32). I4: 48 bytes, loop [16,28). */
	mod_inst( m, 1, 1, 64, 64, 0, 0, 1 );
	mod_inst( m, 1, 2, 48, 64, 16, 16, 70 );
	mod_inst( m, 1, 3, 32, 64, 0, 32, 40 );
	mod_inst( m, 1, 4, 48, 64, 16, 12, 100 );
	len = mod_len( m, 1, 4 );
	memcpy( src, m, (size_t) len );

	/* One-shot instrument: first segment is the whole sample, then silence.
	   Key 428 steps one sample byte per frame. */
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 128 );
	CHECK( buf[ 0 ] == 128 * (signed char)( 1 ) );
	CHECK( buf[ 5 * 2 ] == 128 * (signed char)( 1 + 5 ) );       /* byte 5. */
	CHECK( buf[ 31 * 2 ] == 128 * (signed char)( 1 + 31 ) );     /* byte 31. */
	CHECK( buf[ 63 * 2 ] == 128 * (signed char)( 1 + 63 ) );     /* last byte. */
	CHECK( buf[ 64 * 2 ] == 0 );                                 /* end at byte 64. */
	CHECK( buf[ 100 * 2 ] == 0 );
	CHECK( buf[ 100 * 2 + 1 ] == 0 );
	micromod_player_free( p );

	/* Non-zero loop start: first segment ends at loop_start + loop_length
	   (32); bytes 32..47 are only reachable through a later descriptor. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 2, 428, 0, 0 );
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 128 );
	CHECK( buf[ 0 ] == 128 * (signed char)( 70 ) );              /* I2 byte 0. */
	CHECK( buf[ 15 * 2 ] == 128 * (signed char)( 70 + 15 ) );    /* byte 15. */
	CHECK( buf[ 16 * 2 ] == 128 * (signed char)( 70 + 16 ) );    /* reload to byte 16. */
	CHECK( buf[ 23 * 2 ] == 128 * (signed char)( 70 + 23 ) );
	CHECK( buf[ 31 * 2 ] == 128 * (signed char)( 70 + 31 ) );
	CHECK( buf[ 32 * 2 ] == 128 * (signed char)( 70 + 16 ) );    /* second round. */
	micromod_player_free( p );

	/* Instrument-only change: the active I2 segment and phase keep moving;
	   only the pending descriptor changes to I3. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 2, 428, 0, 0 );
	fixture_note( m, 1, 0, 3, 0, 0, 0 );                          /* select I3, no note. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, ROW + 64 );
	/* At row 1 the I2 loop [16,32) phase is byte 30; the edge two frames
	   later reloads the I3 loop. */
	CHECK( buf[ ( ROW + 0 ) * 2 ] == 128 * (signed char)( 70 + 30 ) );
	CHECK( buf[ ( ROW + 1 ) * 2 ] == 128 * (signed char)( 70 + 31 ) );
	CHECK( buf[ ( ROW + 2 ) * 2 ] == 128 * (signed char)( 40 + 0 ) );
	CHECK( buf[ ( ROW + 3 ) * 2 ] == 128 * (signed char)( 40 + 1 ) );
	CHECK( buf[ ( ROW + 9 ) * 2 ] == 128 * (signed char)( 40 + 7 ) );
	micromod_player_free( p );

	/* 9xx offset past the first-segment tail: the carried distance crosses
	   segment boundaries under the same rules, possibly many rounds.
	   I4 first segment ends at byte 28, loop [16,28) length 12. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 4, 428, 9, 30 );                      /* offset 7680 bytes. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 64 );
	/* (7680 - 28) mod 12 = 8, so playback resumes at loop byte 16+8 = 24. */
	CHECK( buf[ 0 ] == 128 * (signed char)( 100 + 24 ) );
	CHECK( buf[ 1 * 2 ] == 128 * (signed char)( 100 + 25 ) );
	CHECK( buf[ 2 * 2 ] == 128 * (signed char)( 100 + 26 ) );
	CHECK( buf[ 4 * 2 ] == 128 * (signed char)( 100 + 16 ) );   /* edge reload. */
	micromod_player_free( p );

	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 4, 428, 9, 200 );                     /* offset 51200 bytes. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 64 );
	/* (51200 - 28) mod 12 = 4, so playback resumes at loop byte 20. */
	CHECK( buf[ 0 ] == 128 * (signed char)( 100 + 20 ) );
	CHECK( buf[ 1 * 2 ] == 128 * (signed char)( 100 + 21 ) );
	micromod_player_free( p );

	/* E9 retrig restarts the first segment of the selected instrument. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 2, 428, 0xE, 0x92 );                 /* E92: retrig every 2 ticks. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 2 * TICK + 8 );
	f = 2 * TICK;                                                /* retrig tick. */
	/* Frame 329 sits on loop byte 25; the retrig frame restarts at byte 0. */
	CHECK( buf[ ( f - 1 ) * 2 ] == 128 * (signed char)( 70 + 25 ) );
	CHECK( buf[ f * 2 ] == 128 * (signed char)( 70 + 0 ) );     /* back at first segment. */
	CHECK( buf[ ( f + 1 ) * 2 ] == 128 * (signed char)( 70 + 1 ) );
	micromod_player_free( p );

	/* ED note delay: selection and segment start wait for the trigger tick. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 2, 428, 0xE, 0xD2 );                 /* ED2: note delay 2 ticks. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 2 * TICK + 8 );
	CHECK( buf[ ( 2 * TICK - 1 ) * 2 ] == 0 );
	CHECK( buf[ ( 2 * TICK ) * 2 ] == 128 * (signed char)( 70 + 0 ) );
	CHECK( buf[ ( 2 * TICK + 1 ) * 2 ] == 128 * (signed char)( 70 + 1 ) );
	micromod_player_free( p );

	/* Batching requests must give identical PCM across many loop crossings;
	   a dropped fractional step would accumulate and diverge. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 2, 428, 0, 0 );
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	{
		short one[ 2000 * 2 ], many[ 2000 * 2 ];
		long off = 0, n;
		render( p, one, 2000 );
		micromod_player_reset( p, 0 );
		solo( p, 0 );
		memset( many, 0, sizeof( many ) );
		while( off < 2000 ) {
			n = 1 + ( off * 31 + 7 ) % 199;
			if( n > 2000 - off ) n = 2000 - off;
			micromod_player_get_audio( p, many + off * 2, n );
			off += n;
		}
		CHECK( memcmp( one, many, sizeof( one ) ) == 0 );
	}
	micromod_player_free( p );
	/* Only the sample area could be written; the pattern area changed when
	   the notes were added after the snapshot. */
	CHECK( memcmp( m + sample_base( 1 ), src + sample_base( 1 ),
			(size_t)( len - sample_base( 1 ) ) ) == 0 );
}

/* ----------------------------------------------------------------------- */
/* Group 2: EFx funk timing, shared writes, cancellation and ownership.    */
/* ----------------------------------------------------------------------- */

void contract_funk( void ) {
	signed char m[ 8192 ];
	signed char src[ 8192 ];
	micromod_player *p, *control;
	short buf[ 9000 * 2 ];
	long len;
	mod_head( m, 1, 1 );
	/* I1: 32 bytes, loop [8,24). */
	mod_inst( m, 1, 1, 32, 64, 8, 16, 1 );
	len = mod_len( m, 1, 1 );
	memcpy( src, m, (size_t) len );

	/* Channel 0 plays I1 at one byte/frame (key 428); channel 1 selects
	   I1 and runs EF1 (5 bytes/tick). Funk also runs on the row-0 tick, so
	   the accumulator (5/tick) first reaches 128 on the 26th step at frame
	   25*165 = 4125, inverting loop byte 9; channel 0 reads it at frame 4137. */
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	fixture_note( m, 0, 1, 1, 0, 0xE, 0xF1 );                   /* EF1. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 8800 );
	/* Byte 9 (value 10) is read at frames 9 + 16k. */
	CHECK( buf[ 9 * 2 ] == 128 * 10 );
	CHECK( buf[ 4121 * 2 ] == 128 * 10 );                       /* last read before the flip. */
	CHECK( buf[ 4137 * 2 ] == 128 * ( -1 - 10 ) );              /* first read after the flip. */
	micromod_player_free( p );

	/* A muted funk channel still writes: mixing is skipped, clocks are not. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	micromod_player_mute_channel( p, -1 );
	micromod_player_mute_channel( p, 1 );                        /* only channel 0 audible. */
	micromod_player_mute_channel( p, 2 );
	micromod_player_mute_channel( p, 3 );
	render( p, buf, 8800 );
	CHECK( buf[ 4137 * 2 ] == 128 * ( -1 - 10 ) );
	micromod_player_free( p );

	/* NULL output keeps the funk timeline advancing. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	micromod_player_get_audio( p, NULL, 4125 );                  /* reaches the first write. */
	render( p, buf, 128 );
	CHECK( buf[ 12 * 2 ] == 128 * ( -1 - 10 ) );                /* overall frame 4137. */
	micromod_player_free( p );

	/* EF0 stops further inversions; the tick-4125 write is kept. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	fixture_note( m, 0, 1, 1, 0, 0xE, 0xF1 );
	fixture_note( m, 6, 1, 1, 0, 0xE, 0xF0 );                   /* EF0 at row 6 (frame 5940). */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 8800 );
	CHECK( buf[ 4137 * 2 ] == 128 * ( -1 - 10 ) );              /* first write kept. */
	CHECK( buf[ 5945 * 2 ] == 128 * ( -1 - 10 ) );              /* byte 9 stays flipped. */
	CHECK( buf[ 5946 * 2 ] == 128 * 11 );                       /* byte 10 never flipped. */
	micromod_player_free( p );

	/* Two channels inverting the same byte in the same tick cancel. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	fixture_note( m, 0, 1, 1, 0, 0xE, 0xF1 );
	fixture_note( m, 0, 2, 1, 0, 0xE, 0xF1 );
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 8800 );
	CHECK( buf[ 4137 * 2 ] == 128 * 10 );                        /* two flips cancel. */
	micromod_player_free( p );

	/* reset restores the original sample bytes. */
	memcpy( m, src, (size_t) len );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	fixture_note( m, 0, 1, 1, 0, 0xE, 0xF1 );
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 4400 );
	CHECK( buf[ 4137 * 2 ] == 128 * ( -1 - 10 ) );
	micromod_player_reset( p, 0 );
	solo( p, 0 );
	render( p, buf, 64 );
	CHECK( buf[ 9 * 2 ] == 128 * 10 );
	micromod_player_free( p );

	/* Instances are isolated: a funk-free player on the same data is clean. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	control = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL && control != NULL );
	solo( p, 0 );
	solo( control, 0 );
	micromod_player_get_audio( p, NULL, 4400 );                 /* p flips bytes. */
	render( control, buf, 64 );
	CHECK( buf[ 9 * 2 ] == 128 * 10 );                          /* control unaffected. */
	micromod_player_free( p );
	micromod_player_free( control );
	/* Only the sample area is writable; notes changed the pattern area. */
	CHECK( memcmp( m + sample_base( 1 ), src + sample_base( 1 ),
			(size_t)( len - sample_base( 1 ) ) ) == 0 );

	/* Ownership scenario in one fragment. Channel 0 plays a long one-shot A;
	   at row 1 an instrument-only row selects B and starts EF1 while A is
	   still the active segment. Channel 2 plays B. The funk writes must
	   target the *selected* B loop, never the still-playing A segment. */
	memset( m, 0, sizeof( m ) );
	mod_head( m, 1, 1 );
	mod_inst( m, 1, 1, 5200, 64, 0, 0, 1 );                     /* A: long one-shot. */
	mod_inst( m, 1, 2, 32, 64, 8, 16, 70 );                     /* B: loop [8,24). */
	len = mod_len( m, 1, 2 );
	{
		signed char orig[ 8192 ];
		fixture_note( m, 0, 0, 1, 428, 0, 0 );                  /* A note on channel 0. */
		fixture_note( m, 0, 2, 2, 428, 0, 0 );                  /* B on channel 2 (right). */
		fixture_note( m, 1, 0, 2, 0, 0xE, 0xF1 );               /* select B + EF1, no note. */
		memcpy( orig, m, (size_t) len );
		p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
		CHECK( p != NULL );
		micromod_player_mute_channel( p, -1 );
		micromod_player_mute_channel( p, 1 );                   /* unused channel silent. */
		micromod_player_mute_channel( p, 3 );
		render( p, buf, 6000 );
		CHECK( buf[ 20 * 2 ] == 128 * (signed char)( 1 + 20 ) );          /* left: A byte 20. */
		CHECK( buf[ 20 * 2 + 1 ] == 128 * (signed char)( 70 + 20 ) );     /* right: B byte 20. */
		/* The EF1 row is read at frame 990; the first funk write lands at
		   frame 5115 on selected B's byte 9. Channel 0 is still inside A's
		   long segment and reads it unflipped; channel 2 reads the inverted
		   B byte 9 (-80) at frame 5129. */
		CHECK( buf[ 5129 * 2 ] == 128 * 10 );
		CHECK( buf[ 5129 * 2 + 1 ] == 128 * ( -1 - ( 70 + 9 ) ) );
		/* A's segment ends at frame 5200 and hands over to the pending B loop. */
		CHECK( buf[ 5199 * 2 ] == 128 * 80 );
		CHECK( buf[ 5200 * 2 ] == 128 * ( 70 + 8 ) );
		micromod_player_free( p );
		CHECK( memcmp( m, orig, (size_t) len ) == 0 );          /* source never modified. */
	}

	/* A stopped segment with no pending loop is never revived by an
	   instrument-only row; a fresh note is required. */
	memset( m, 0, sizeof( m ) );
	mod_head( m, 1, 1 );
	mod_inst( m, 1, 1, 64, 64, 0, 0, 1 );                       /* one-shot. */
	mod_inst( m, 1, 2, 32, 64, 0, 0, 40 );                      /* one-shot, no valid loop. */
	mod_inst( m, 1, 3, 32, 64, 0, 32, 5 );                      /* looped. */
	len = mod_len( m, 1, 3 );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );                      /* I1 note. */
	fixture_note( m, 2, 0, 2, 0, 0, 0 );                        /* select I2, no note. */
	fixture_note( m, 4, 0, 3, 428, 0, 0 );                      /* I3 note revives. */
	p = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( p != NULL );
	solo( p, 0 );
	render( p, buf, 5 * ROW );
	CHECK( buf[ 31 * 2 ] != 0 );                                /* I1 plays. */
	CHECK( buf[ 64 * 2 ] == 0 );                                /* then stops. */
	CHECK( buf[ ( 2 * ROW + 5 ) * 2 ] == 0 );                   /* selection does not revive. */
	CHECK( buf[ ( 4 * ROW ) * 2 ] == 128 * 5 );                /* new note starts I3. */
	micromod_player_free( p );
}

/* ----------------------------------------------------------------------- */
/* Group 3: interleaved instances, reset, duration and mute/NULL skips.    */
/* ----------------------------------------------------------------------- */

void contract_instances( void ) {
	signed char m[ 4096 ];
	signed char src[ 4096 ];
	signed char two[ 4096 ];
	micromod_player *a, *b;
	short ref[ 3000 * 2 ], out[ 3000 * 2 ];
	long len, dur, legacy_dur;
	mod_head( m, 1, 1 );
	mod_inst( m, 1, 1, 64, 64, 16, 32, 3 );
	mod_inst( m, 1, 2, 48, 64, 8, 16, 50 );
	len = mod_len( m, 1, 2 );
	memcpy( src, m, (size_t) len );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	fixture_note( m, 2, 0, 2, 428, 0, 0 );
	fixture_note( m, 0, 2, 2, 856, 0, 0 );

	/* Interleaved driving of two same-source players matches solo renders. */
	a = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	b = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( a != NULL && b != NULL && a != b );
	{
		micromod_player *solo_a = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
		micromod_player *solo_b = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
		long i;
		CHECK( solo_a != NULL && solo_b != NULL );
		memset( ref, 0, sizeof( ref ) );
		micromod_player_get_audio( solo_a, ref, 3000 );
		memset( out, 0, sizeof( out ) );
		for( i = 0; i < 3000; i += 73 ) {
			long n = 73;
			if( n > 3000 - i ) n = 3000 - i;
			micromod_player_get_audio( a, out + i * 2, n );
			micromod_player_get_audio( b, NULL, n );
		}
		CHECK( memcmp( ref, out, sizeof( ref ) ) == 0 );
		memset( ref, 0, sizeof( ref ) );
		micromod_player_get_audio( solo_b, ref, 3000 );
		/* b already consumed 3000 frames via NULL skips; restart it. */
		micromod_player_reset( b, 0 );
		memset( out, 0, sizeof( out ) );
		micromod_player_get_audio( b, out, 3000 );
		CHECK( memcmp( ref, out, sizeof( ref ) ) == 0 );
		micromod_player_free( solo_a );
		micromod_player_free( solo_b );
	}
	micromod_player_free( a );
	micromod_player_free( b );

	/* duration matches the legacy song-end rule and leaves state untouched. */
	a = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( a != NULL );
	CHECK( micromod_initialise( m, RATE ) == 0 );
	legacy_dur = micromod_calculate_song_duration();
	dur = micromod_player_duration( a );
	CHECK( dur == legacy_dur );
	CHECK( dur > 0 );
	render( a, ref, 1000 );
	dur = micromod_player_duration( a );                          /* must not move cursors. */
	CHECK( dur == legacy_dur );
	render( a, out, 1000 );
	{
		micromod_player *c = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
		short again[ 2000 * 2 ];
		CHECK( c != NULL );
		render( c, again, 2000 );
		CHECK( memcmp( again, ref, 1000 * 2 * sizeof( short ) ) == 0 );
		CHECK( memcmp( again + 1000 * 2, out, 1000 * 2 * sizeof( short ) ) == 0 );
		micromod_player_free( c );
	}
	micromod_player_free( a );

	/* Reset between orders and out-of-range fallback. */
	memset( two, 0, sizeof( two ) );
	mod_head( two, 2, 2 );
	mod_inst( two, 2, 1, 64, 64, 0, 0, 3 );
	len = mod_len( two, 2, 1 );
	fixture_note( two, 0, 0, 1, 428, 0, 0 );                    /* order 0: one byte/frame. */
	fixture_note( two, 64, 0, 1, 214, 0, 0 );                   /* order 1: two bytes/frame. */
	a = micromod_player_create( two, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( a != NULL );
	solo( a, 0 );
	render( a, ref, 64 );
	micromod_player_reset( a, 1 );
	solo( a, 0 );
	render( a, out, 64 );
	CHECK( out[ 1 * 2 ] == 128 * 5 );                           /* order 1 frame 1 reads byte 2. */
	CHECK( ref[ 1 * 2 ] == 128 * 4 );                           /* order 0 frame 1 reads byte 1. */
	micromod_player_reset( a, 99 );                             /* falls back to order 0. */
	solo( a, 0 );
	render( a, out, 64 );
	CHECK( out[ 1 * 2 ] == 128 * 4 );

	/* Gain and mute survive reset. Frame 0 reads sample byte 3 (seed 3). */
	micromod_player_set_gain( a, 64 );
	micromod_player_mute_channel( a, -1 );
	micromod_player_mute_channel( a, 2 );
	micromod_player_reset( a, 0 );
	render( a, ref, 64 );
	CHECK( ref[ 0 ] == 3 * ( 64 * 64 >> 5 ) );                  /* byte 3 * ampl 128. */
	CHECK( ref[ 1 ] == 0 );                                     /* right bus muted. */
	micromod_player_set_gain( a, 32 );
	micromod_player_reset( a, 0 );                              /* mute is retained. */
	render( a, out, 64 );
	CHECK( out[ 0 ] == 3 * ( 64 * 32 >> 5 ) );                  /* byte 3 * ampl 64. */
	CHECK( out[ 1 ] == 0 );
	micromod_player_free( a );

	/* NULL-output skip advances exactly like rendered audio. */
	memcpy( m, src, (size_t) sizeof( src ) );
	fixture_note( m, 0, 0, 1, 428, 0, 0 );
	len = mod_len( m, 1, 2 );
	a = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	b = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
	CHECK( a != NULL && b != NULL );
	solo( a, 0 );
	solo( b, 0 );
	micromod_player_get_audio( a, NULL, 555 );
	render( a, out, 700 );
	render( b, ref, 1255 );
	CHECK( memcmp( ref + 555 * 2, out, 700 * 2 * sizeof( short ) ) == 0 );
	/* Zero-frame calls change nothing: a synchronised reference player that
	   never sees them produces identical subsequent PCM. */
	{
		micromod_player *c = micromod_player_create( m, (size_t) len, RATE, MICROMOD_SAMPLE_DMA );
		short z[ 100 * 2 ], expect[ 100 * 2 ], discard[ 1355 * 2 ];
		CHECK( c != NULL );
		solo( c, 0 );
		render( b, ref, 100 );                              /* b advances to 1355. */
		micromod_player_get_audio( b, NULL, 0 );
		micromod_player_get_audio( b, out, 0 );
		render( b, z, 100 );                                /* b frames 1355..1454. */
		render( c, discard, 1355 );                         /* advance c to frame 1355. */
		render( c, expect, 100 );                           /* c frames 1355..1454. */
		CHECK( memcmp( z, expect, sizeof( z ) ) == 0 );
		micromod_player_free( c );
	}
	micromod_player_free( a );
	micromod_player_free( b );
}

/* ----------------------------------------------------------------------- */
/* Group 4: legacy API and flags=0 stay sample-identical.                  */
/* ----------------------------------------------------------------------- */

void contract_compatibility( void ) {
	signed char m[ FIXTURE_BYTES ];
	signed char src[ FIXTURE_BYTES ];
	short legacy[ 4096 * 2 ], modern[ 4096 * 2 ];
	micromod_player *p, *q;
	long duration, kind;
	for( kind = 0; kind <= 2; ++kind ) {
		fixture_mod( m, (int) kind );
		memcpy( src, m, sizeof( m ) );
		CHECK( micromod_initialise( m, RATE ) == 0 );
		memset( legacy, 0, sizeof( legacy ) );
		micromod_get_audio( legacy, 4096 );
		p = micromod_player_create( m, FIXTURE_BYTES, RATE, 0 );
		CHECK( p != NULL );
		render( p, modern, 4096 );
		CHECK( memcmp( legacy, modern, sizeof( legacy ) ) == 0 );
		micromod_player_free( p );
		CHECK( memcmp( m, src, sizeof( m ) ) == 0 );
	}

	/* Legacy global state is not disturbed by instance players. */
	fixture_mod( m, 0 );
	CHECK( micromod_initialise( m, RATE ) == 0 );
	p = micromod_player_create( m, FIXTURE_BYTES, RATE, MICROMOD_SAMPLE_DMA );
	q = micromod_player_create( m, FIXTURE_BYTES, RATE, 0 );
	CHECK( p != NULL && q != NULL );
	render( p, modern, 2048 );
	render( q, modern, 2048 );
	memset( legacy, 0, sizeof( legacy ) );
	micromod_get_audio( legacy, 2048 );
	{
		micromod_player *r = micromod_player_create( m, FIXTURE_BYTES, RATE, 0 );
		short ref[ 2048 * 2 ];
		CHECK( r != NULL );
		render( r, ref, 2048 );
		CHECK( memcmp( ref, legacy, sizeof( ref ) ) == 0 );
		micromod_player_free( r );
	}
	micromod_player_free( p );
	micromod_player_free( q );

	/* Duration agrees with the legacy calculation (fixture song: 10560). */
	p = micromod_player_create( m, FIXTURE_BYTES, RATE, 0 );
	CHECK( p != NULL );
	duration = micromod_player_duration( p );
	CHECK( duration == 10560 );
	CHECK( micromod_calculate_song_duration() == 10560 );
	micromod_player_free( p );

	/* Creation rejection paths. */
	CHECK( micromod_player_create( m, FIXTURE_BYTES, RATE, 2u ) == NULL );     /* unknown flag. */
	CHECK( micromod_player_create( m, FIXTURE_BYTES, 7999, 0 ) == NULL );     /* rate too low. */
	CHECK( micromod_player_create( m, FIXTURE_BYTES - 1, RATE, 0 ) == NULL ); /* truncated. */
	CHECK( micromod_player_create( m, 10, RATE, 0 ) == NULL );               /* too short. */
	CHECK( micromod_player_create( NULL, FIXTURE_BYTES, RATE, 0 ) == NULL );
	{
		signed char bad[ FIXTURE_BYTES ];
		memcpy( bad, m, sizeof( bad ) );
		bad[ 1082 ] = 'X'; bad[ 1083 ] = 'X';                              /* unknown tag. */
		CHECK( micromod_player_create( bad, FIXTURE_BYTES, RATE, 0 ) == NULL );
	}
	{
		/* Eight-channel module: valid for flags=0, rejected for DMA. */
		signed char eight[ 4096 ];
		long elen;
		memset( eight, 0, sizeof( eight ) );
		memcpy( eight, "eight channel", 14 );
		eight[ 950 ] = 1;
		memcpy( eight + 1080, "8CHN", 4 );
		elen = micromod_calculate_mod_file_len( eight );
		CHECK( elen > 0 && elen < (long) sizeof( eight ) );
		CHECK( micromod_player_create( eight, (size_t) elen, RATE, 0 ) != NULL );
		CHECK( micromod_player_create( eight, (size_t) elen, RATE, MICROMOD_SAMPLE_DMA ) == NULL );
	}
	micromod_player_free( NULL );                                              /* must be safe. */
}
