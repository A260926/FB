
/*
	two_players.c - instance-based Micromod player example.

	Builds a tiny four-channel MOD in memory and drives two independent
	players from the same module bytes:

	  * player A uses the classic replay mode (flags = 0), whose output is
	    identical to the legacy micromod_get_audio() API;
	  * player B enables MICROMOD_SAMPLE_DMA, so sample segments survive
	    instrument changes and the EFx inverse-sample effect is honoured.

	The example uses only the C standard library: no SDL or other
	dependency. It also documents the input-lifetime and reset rules.

	Compile:
	  cc -std=c99 -Wall -I.. micromod.c micromod_core.c micromod_player.c \
	     ../examples/two_players.c -o two_players
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "micromod.h"

#define RATE 8287L
#define MOD_SIZE ( 1084 + 1024 + 128 )   /* header + one pattern + 128 sample bytes. */

static void put_word( signed char *p, unsigned value ) {
	p[ 0 ] = (signed char)( value >> 8 );
	p[ 1 ] = (signed char) value;
}

/* Write one Protracker note cell (raw effect nibble, e.g. 0xE for EFx). */
static void put_note( signed char *m, int row, int chan, int instrument,
		int period, int effect, int param ) {
	signed char *p = m + 1084 + ( row * 4 + chan ) * 4;
	p[ 0 ] = (signed char)( ( instrument & 16 ) | ( period >> 8 ) );
	p[ 1 ] = (signed char) period;
	p[ 2 ] = (signed char)( ( instrument << 4 ) | effect );
	p[ 3 ] = (signed char) param;
}

static void build_module( signed char *m ) {
	int i;
	memset( m, 0, MOD_SIZE );
	memcpy( m, "two-players example", 19 );
	m[ 950 ] = 1;                         /* one order in the sequence. */
	memcpy( m + 1080, "M.K.", 4 );        /* four-channel module. */
	/* Instrument 1: 128 bytes, default volume, loop over the last 64. */
	put_word( m + 42, 64 );               /* sample length in words. */
	m[ 45 ] = 64;                         /* volume. */
	put_word( m + 46, 32 );               /* loop start in words. */
	put_word( m + 48, 32 );               /* loop length in words. */
	for( i = 0; i < 128; ++i )
		m[ 2108 + i ] = (signed char)( i - 64 );
	/* Row 0: key 428 on channel 0; channel 1 selects the instrument and
	   starts EF1 (raw param 0xF1), which only matters in DMA mode. */
	put_note( m, 0, 0, 1, 428, 0, 0 );
	put_note( m, 0, 1, 1, 0, 0xE, 0xF1 );
}

int main( void ) {
	/*
		The input buffer. Every player created from it only ever reads it,
		so it must stay allocated and unchanged until *all* those players
		have been freed. DMA players keep their own private, writable copy
		of the sample bytes and never modify this buffer.
	*/
	static signed char module[ MOD_SIZE ];
	micromod_player *classic, *dma;
	short classic_buf[ 256 * 2 ], dma_buf[ 256 * 2 ];
	long duration;
	int i;

	build_module( module );

	classic = micromod_player_create( module, MOD_SIZE, RATE, 0 );
	dma = micromod_player_create( module, MOD_SIZE, RATE, MICROMOD_SAMPLE_DMA );
	if( !classic || !dma ) {
		fputs( "failed to create players\n", stderr );
		return 1;
	}

	/* The two players hold independent state; interleaving calls is safe. */
	memset( classic_buf, 0, sizeof( classic_buf ) );
	memset( dma_buf, 0, sizeof( dma_buf ) );
	micromod_player_get_audio( classic, classic_buf, 256 );
	micromod_player_get_audio( dma, dma_buf, 256 );
	printf( "frame 0: classic=%d dma=%d\n", classic_buf[ 0 ], dma_buf[ 0 ] );
	printf( "frame 64: classic=%d dma=%d\n", classic_buf[ 64 * 2 ], dma_buf[ 64 * 2 ] );

	/* Muting one player never affects the other. */
	micromod_player_mute_channel( dma, 0 );
	memset( dma_buf, 0, sizeof( dma_buf ) );
	micromod_player_get_audio( dma, dma_buf, 16 );
	printf( "muted dma frame 0 left=%d right=%d\n", dma_buf[ 0 ], dma_buf[ 1 ] );

	/* Reset restarts at order 0 row 0, restores DMA sample bytes and keeps
	   gain/mute configuration; an out-of-range order falls back to order 0. */
	micromod_player_reset( dma, 999 );
	memset( dma_buf, 0, sizeof( dma_buf ) );
	micromod_player_get_audio( dma, dma_buf, 16 );
	printf( "after reset dma frame 0 left=%d (mute retained)\n", dma_buf[ 0 ] );

	/* Duration is computed from a throwaway state and moves no cursors. */
	duration = micromod_player_duration( classic );
	printf( "song duration: %ld frames\n", duration );

	/* NULL output advances clocks (including DMA segment/funk timing) but
	   skips mixing; batching requests yields identical PCM to one big read. */
	micromod_player_get_audio( classic, NULL, 37 );
	memset( classic_buf, 0, sizeof( classic_buf ) );
	micromod_player_get_audio( classic, classic_buf, 64 );
	for( i = 0; i < 4; ++i )
		printf( "classic frame %d left=%d\n", i, classic_buf[ i * 2 ] );

	micromod_player_free( classic );
	micromod_player_free( dma );
	/* Only now may the module buffer be released or reused. */
	puts( "TWO_PLAYERS_OK" );
	return 0;
}
