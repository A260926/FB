
#include "micromod.h"
#include "micromod_internal.h"

#include <stdlib.h>

/*
	Opaque, independently-stated player front-end. Each call to
	micromod_player_create allocates its own micromod_player structure
	(and, in sample-DMA mode, its own writable sample area), so players
	built from the same module data never share mutable state.
*/
micromod_player *micromod_player_create( const signed char *data, size_t length,
		long rate, unsigned flags ) {
	micromod_player *player;
	long result;
	if( data == NULL || length < 1084 ) return NULL;
	if( ( flags & ~(unsigned) MICROMOD_SAMPLE_DMA ) != 0 ) return NULL;
	if( rate < 8000 ) return NULL;
	if( mm_calculate_mod_file_len( data ) < 0 ) return NULL;
	player = calloc( 1, sizeof( *player ) );
	if( !player ) return NULL;
	result = mm_core_initialise( player, data, length, rate, flags );
	if( result != 0 ) {
		/* mm_core_initialise zeroed the structure on entry; any sample
		   store it allocated is the only thing that needs freeing. */
		free( player->sample_store );
		free( player );
		return NULL;
	}
	return player;
}

void micromod_player_free( micromod_player *player ) {
	if( player ) {
		free( player->sample_store );
		free( player );
	}
}

void micromod_player_reset( micromod_player *player, long order ) {
	if( player ) mm_core_reset( player, order );
}

void micromod_player_get_audio( micromod_player *player, short *output, long frames ) {
	if( player ) mm_core_get_audio( player, output, frames );
}

void micromod_player_set_gain( micromod_player *player, long gain ) {
	if( player ) mm_core_set_gain( player, gain );
}

long micromod_player_mute_channel( micromod_player *player, long channel ) {
	if( !player ) return 0;
	return mm_core_mute_channel( player, channel );
}

long micromod_player_duration( micromod_player *player ) {
	struct micromod_player scratch;
	long duration;
	if( !player ) return 0;
	/* Independent temporary state: walk the sequence on a stack copy.
	   The copy is given no writable sample area, so funk steps have
	   nowhere to write and the caller's samples, cursors, gain and mute
	   configuration are all left untouched. */
	scratch = *player;
	scratch.sample_store = NULL;
	duration = mm_core_calculate_song_duration( &scratch );
	return duration;
}
