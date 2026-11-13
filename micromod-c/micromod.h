
#ifndef MICROMOD_H
#define MICROMOD_H

#include <stddef.h>

/*
	Returns a string containing version information.
*/
const char *micromod_get_version( void );

/*
	Calculate the length in bytes of a module file given the 1084-byte header.
	Returns -1 if the data is not recognised as a module.
*/
long micromod_calculate_mod_file_len( signed char *module_header );

/*
	Set the player to play the specified module data.
	The data array must not be less than the length given by micromod_calculate_mod_file_len().
	Returns -1 if the data is not recognised as a module.
	Returns -2 if the sampling rate is less than 8000hz.
*/
long micromod_initialise( signed char *data, long sampling_rate );

/*
	Obtains song and instrument names from the module.
	The song name is returned as instrument 0.
	The name is copied into the location pointed to by string,
	and is at most 23 characters long, including the trailing null.
*/
void micromod_get_string( long instrument, char *string );

/*
	Returns the total song duration in samples at the current sampling rate.
*/
long micromod_calculate_song_duration( void );

/*
	Jump directly to a specific pattern in the sequence.
*/
void micromod_set_position( long pos );

/*
	Mute the specified channel.
	If channel is negative, un-mute all channels.
	Returns the number of channels.
*/
long micromod_mute_channel( long channel );

/*
	Set the playback gain.
	For 4-channel modules, a value of 64 can be used without distortion.
	For 8-channel modules, a value of 32 or less is recommended.
*/
void micromod_set_gain( long value );

/*
	Calculate the specified number of stereo samples of audio.
	Output buffer must be zeroed.
*/
void micromod_get_audio( short *output_buffer, long count );

/* ----------------------------------------------------------------------- */
/* Instance-based player API.                                              */
/* ----------------------------------------------------------------------- */

/*
	An opaque replay instance. Every instance owns all of its playback
	state; two instances created from the same module data are fully
	independent and may be driven interleaved.
*/
typedef struct micromod_player micromod_player;

/*
	Enable the sample DMA / funk replay mode. In this mode sample segments
	keep playing across instrument changes, loops are reloaded only at
	segment ends, and the EFx inverse-sample effect writes into a private
	writable copy of the samples. Only accepted for four-channel modules.
	With flags set to zero, output is identical to the legacy API.
*/
#define MICROMOD_SAMPLE_DMA 1u

/*
	Create a player for the module in data. The data must remain valid and
	unmodified until the player is freed; it is only ever read.
	Returns NULL for an unknown flag, truncated or invalid data, a rate
	below 8000hz, a non-4-channel module with MICROMOD_SAMPLE_DMA, or if
	memory allocation fails.
*/
micromod_player *micromod_player_create( const signed char *data, size_t length,
		long rate, unsigned flags );

/*
	Free a player. Safe to call with NULL.
*/
void micromod_player_free( micromod_player *player );

/*
	Restart at row 0 of the given order. The player's original sample bytes
	and all playback state are restored, while gain and mute configuration
	are retained. An out-of-range order restarts at order 0.
*/
void micromod_player_reset( micromod_player *player, long order );

/*
	Calculate the specified number of interleaved stereo frames.
	A NULL output buffer advances the clock without mixing. The buffer,
	when provided, must be zeroed by the caller first. A zero frame count
	has no effect.
*/
void micromod_player_get_audio( micromod_player *player, short *output, long frames );

/*
	Set the playback gain (see micromod_set_gain).
*/
void micromod_player_set_gain( micromod_player *player, long gain );

/*
	Mute the specified channel; a negative channel un-mutes all channels.
	Returns the number of channels.
*/
long micromod_player_mute_channel( micromod_player *player, long channel );

/*
	Return the total song duration in frames, measured from order 0 using
	the original song-end rule. Does not modify the instance.
*/
long micromod_player_duration( micromod_player *player );

#endif
