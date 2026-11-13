
#include "micromod.h"
#include "micromod_internal.h"

/*
	Legacy global-state front-end. All state lives in this one static
	instance; the replay engine itself is the shared instance-based core,
	so legacy callers and micromod_player instances never exchange state.
*/
static struct micromod_player global_player;

const char *micromod_get_version( void ) {
	return mm_get_version();
}

long micromod_calculate_mod_file_len( signed char *module_header ) {
	return mm_calculate_mod_file_len( module_header );
}

long micromod_initialise( signed char *data, long sampling_rate ) {
	return mm_core_initialise( &global_player, data, MM_LEGACY_DATA_LENGTH,
			sampling_rate, 0 );
}

void micromod_get_string( long instrument, char *string ) {
	mm_core_get_string( &global_player, instrument, string );
}

long micromod_calculate_song_duration( void ) {
	return mm_core_calculate_song_duration( &global_player );
}

void micromod_set_position( long pos ) {
	mm_core_reset( &global_player, pos );
}

long micromod_mute_channel( long channel ) {
	return mm_core_mute_channel( &global_player, channel );
}

void micromod_set_gain( long value ) {
	mm_core_set_gain( &global_player, value );
}

void micromod_get_audio( short *output_buffer, long count ) {
	mm_core_get_audio( &global_player, output_buffer, count );
}
