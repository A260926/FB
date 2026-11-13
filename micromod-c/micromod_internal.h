
/*
	Internal instance-based replay core, shared by the legacy global API
	(micromod.c) and the opaque player API (micromod_player.c).
	There is a single playback engine; the two public front-ends only
	differ in how they obtain their micromod_player structure.
*/
#ifndef MICROMOD_INTERNAL_H
#define MICROMOD_INTERNAL_H

#include <stddef.h>

#define MM_MAX_CHANNELS 16
#define MM_FP_SHIFT 14
#define MM_FP_ONE   16384L
#define MM_FP_MASK  16383L

/* Passed as the data length by the legacy front-end, which has no length. */
#define MM_LEGACY_DATA_LENGTH ((size_t) -1)

struct mm_note {
	unsigned short key;
	unsigned char instrument, effect, param;
};

struct mm_instrument {
	unsigned char volume, fine_tune;
	unsigned long loop_start, loop_length;
	unsigned long sample_length, sample_offset;
	const signed char *sample_data;
};

struct mm_channel {
	struct mm_note note;
	unsigned short period, porta_period;
	unsigned long sample_offset, sample_idx, step;
	unsigned char volume, panning, fine_tune, ampl, mute;
	unsigned char id, instrument, assigned, porta_speed, pl_row, fx_count;
	unsigned char vibrato_type, vibrato_phase, vibrato_speed, vibrato_depth;
	unsigned char tremolo_type, tremolo_phase, tremolo_speed, tremolo_depth;
	signed char tremolo_add, vibrato_add, arpeggio_add;
	/* Sample DMA mode state. */
	unsigned char dma_active;       /* A segment is currently playing. */
	unsigned char pend_valid;       /* A loop reload is pending. */
	unsigned char pend_instrument;  /* Instrument the pending loop belongs to. */
	unsigned long seg_end;          /* Fixed-point end of the active segment. */
	/* Inverse-sample (funk) state. */
	unsigned char funk_speed, funk_acc;
	unsigned long funk_cursor;
};

struct micromod_player {
	const signed char *module_data;
	size_t data_length;
	unsigned flags;
	const unsigned char *pattern_data, *sequence;
	long song_length, restart, num_patterns, num_channels;
	struct mm_instrument instruments[ 32 ];
	/* Mutable per-player sample copy used in DMA mode; NULL otherwise. */
	signed char *sample_store;
	const signed char *sample_src;
	unsigned long sample_store_len;
	long sample_rate, gain, c2_rate, tick_len, tick_offset;
	long pattern, break_pattern, row, next_row, tick;
	long speed, pl_count, pl_channel, random_seed;
	struct mm_channel channels[ MM_MAX_CHANNELS ];
};

/* Returns -1 if the header is not recognised as a module. */
long mm_calculate_mod_file_len( const signed char *module_header );

/* Returns a string containing version information. */
const char *mm_get_version( void );

/*
	Initialise an instance. The structure is zeroed first.
	Returns 0 on success, -1 for an unrecognised/truncated module,
	-2 for a sampling rate below 8000hz and -3 for a mode/channel mismatch.
*/
long mm_core_initialise( struct micromod_player *player, const signed char *data,
		size_t length, long sampling_rate, unsigned flags );

/* Reset to row 0 of the given order, mirroring micromod_set_position. */
void mm_core_reset( struct micromod_player *player, long order );

void mm_core_get_string( struct micromod_player *player, long instrument, char *string );
long mm_core_calculate_song_duration( struct micromod_player *player );
long mm_core_mute_channel( struct micromod_player *player, long channel );
void mm_core_set_gain( struct micromod_player *player, long value );
void mm_core_get_audio( struct micromod_player *player, short *output_buffer, long count );

#endif
