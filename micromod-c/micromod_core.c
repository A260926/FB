
#include "micromod_internal.h"

#include <stdlib.h>
#include <string.h>

#define FP_SHIFT MM_FP_SHIFT
#define FP_ONE   MM_FP_ONE
#define FP_MASK  MM_FP_MASK
#define MAX_CHANNELS MM_MAX_CHANNELS

static const char *MM_VERSION = "Micromod Protracker replay 20260303 (c)mumart@gmail.com";

static const unsigned short fine_tuning[] = {
	4340, 4308, 4277, 4247, 4216, 4186, 4156, 4126,
	4096, 4067, 4037, 4008, 3979, 3951, 3922, 3894
};

static const unsigned short arp_tuning[] = {
	4096, 3866, 3649, 3444, 3251, 3069, 2896, 2734,
	2580, 2435, 2299, 2170, 2048, 1933, 1825, 1722
};

static const unsigned char sine_table[] = {
	  0,  24,  49,  74,  97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120,  97,  74,  49,  24
};

/* EFx inverse-sample step rates (bytes per tick). */
static const unsigned char funk_rates[] = {
	0, 5, 6, 7, 8, 10, 11, 13, 16, 19, 22, 26, 32, 43, 64, 128
};

static long calculate_num_patterns( const signed char *module_header ) {
	long num_patterns, order_entry, pattern;
	num_patterns = 0;
	for( pattern = 0; pattern < 128; pattern++ ) {
		order_entry = module_header[ 952 + pattern ] & 0x7F;
		if( order_entry >= num_patterns ) num_patterns = order_entry + 1;
	}
	return num_patterns;
}

static long calculate_num_channels( const signed char *module_header ) {
	long numchan;
	switch( ( module_header[ 1082 ] << 8 ) | module_header[ 1083 ] ) {
		case 0x4b2e: /* M.K. */
		case 0x4b21: /* M!K! */
		case 0x542e: /* N.T. */
		case 0x5434: /* FLT4 */
			numchan = 4;
			break;
		case 0x484e: /* xCHN */
			numchan = module_header[ 1080 ] - 48;
			break;
		case 0x4348: /* xxCH */
			numchan = ( ( module_header[ 1080 ] - 48 ) * 10 ) + ( module_header[ 1081 ] - 48 );
			break;
		default: /* Not recognised. */
			numchan = 0;
			break;
	}
	if( numchan > MAX_CHANNELS ) numchan = 0;
	return numchan;
}

static long unsigned_short_big_endian( const signed char *buf, long offset ) {
	return ( ( buf[ offset ] & 0xFF ) << 8 ) | ( buf[ offset + 1 ] & 0xFF );
}

const char *mm_get_version( void ) {
	return MM_VERSION;
}

long mm_calculate_mod_file_len( const signed char *module_header ) {
	long length, numchan, inst_idx;
	numchan = calculate_num_channels( module_header );
	if( numchan <= 0 ) return -1;
	length = 1084 + 4 * numchan * 64 * calculate_num_patterns( module_header );
	for( inst_idx = 1; inst_idx < 32; inst_idx++ )
		length += unsigned_short_big_endian( module_header, inst_idx * 30 + 12 ) * 2;
	return length;
}

static void set_tempo( struct micromod_player *player, long tempo ) {
	long sample_rate = player->sample_rate;
	player->tick_len = ( ( sample_rate << 1 ) + ( sample_rate >> 1 ) ) / tempo;
}

static void update_frequency( struct micromod_player *player, struct mm_channel *chan ) {
	long period, volume;
	unsigned long freq;
	period = chan->period + chan->vibrato_add;
	period = period * arp_tuning[ chan->arpeggio_add ] >> 11;
	period = ( period >> 1 ) + ( period & 1 );
	if( period < 14 ) period = 6848;
	freq = player->c2_rate * 428 / period;
	chan->step = ( freq << FP_SHIFT ) / player->sample_rate;
	volume = chan->volume + chan->tremolo_add;
	if( volume > 64 ) volume = 64;
	if( volume < 0 ) volume = 0;
	chan->ampl = ( volume * player->gain ) >> 5;
}

static void tone_portamento( struct mm_channel *chan ) {
	long source, dest;
	source = chan->period;
	dest = chan->porta_period;
	if( source < dest ) {
		source += chan->porta_speed;
		if( source > dest ) source = dest;
	} else if( source > dest ) {
		source -= chan->porta_speed;
		if( source < dest ) source = dest;
	}
	chan->period = source;
}

static void volume_slide( struct mm_channel *chan, long param ) {
	long volume;
	volume = chan->volume + ( param >> 4 ) - ( param & 0xF );
	if( volume < 0 ) volume = 0;
	if( volume > 64 ) volume = 64;
	chan->volume = volume;
}

static long waveform( struct micromod_player *player, long phase, long type ) {
	long amplitude = 0;
	switch( type & 0x3 ) {
		case 0: /* Sine. */
			amplitude = sine_table[ phase & 0x1F ];
			if( ( phase & 0x20 ) > 0 ) amplitude = -amplitude;
			break;
		case 1: /* Saw Down. */
			amplitude = 255 - ( ( ( phase + 0x20 ) & 0x3F ) << 3 );
			break;
		case 2: /* Square. */
			amplitude = 255 - ( ( phase & 0x20 ) << 4 );
			break;
		case 3: /* Random. */
			amplitude = ( player->random_seed >> 20 ) - 255;
			player->random_seed = ( player->random_seed * 65 + 17 ) & 0x1FFFFFFF;
			break;
	}
	return amplitude;
}

static void vibrato( struct micromod_player *player, struct mm_channel *chan ) {
	chan->vibrato_add = waveform( player, chan->vibrato_phase, chan->vibrato_type ) * chan->vibrato_depth >> 7;
}

static void tremolo( struct micromod_player *player, struct mm_channel *chan ) {
	chan->tremolo_add = waveform( player, chan->tremolo_phase, chan->tremolo_type ) * chan->tremolo_depth >> 6;
}

/* ----------------------------------------------------------------------- */
/* Sample DMA mode: segment ownership, pending loops and funk writes.      */
/* ----------------------------------------------------------------------- */

static int dma_mode( struct micromod_player *player ) {
	return ( player->flags & 1u ) != 0;
}

/* Refresh the loop reload descriptor to describe the currently selected
   instrument's loop. Called whenever the selected instrument changes and
   on every segment end. */
static void dma_refresh_pending( struct micromod_player *player, struct mm_channel *chan ) {
	long ins = chan->assigned;
	struct mm_instrument *inst;
	if( ins <= 0 || ins >= 32 ) {
		chan->pend_valid = 0;
		return;
	}
	inst = &player->instruments[ ins ];
	chan->pend_instrument = (unsigned char) ins;
	chan->pend_valid = inst->loop_length >= (unsigned long) ( 4 << FP_SHIFT );
}

/* Start the first segment of the selected instrument, honouring 9xx. */
static void dma_start_first( struct micromod_player *player, struct mm_channel *chan ) {
	long ins = chan->assigned;
	struct mm_instrument *inst;
	chan->dma_active = 0;
	chan->pend_valid = 0;
	if( ins <= 0 || ins >= 32 ) return;
	inst = &player->instruments[ ins ];
	chan->instrument = (unsigned char) ins;
	if( inst->loop_length >= (unsigned long) ( 4 << FP_SHIFT ) && inst->loop_start > 0 )
		chan->seg_end = inst->loop_start + inst->loop_length;
	else
		chan->seg_end = inst->sample_length << FP_SHIFT;
	chan->dma_active = 1;
	dma_refresh_pending( player, chan );
}

/* Reload at a segment end: keep the fractional distance past the boundary,
   re-fetch the latest pending loop descriptor, and cross as many rounds as
   the remaining distance covers. */
static void dma_reload( struct micromod_player *player, struct mm_channel *chan ) {
	unsigned long over;
	long ins;
	struct mm_instrument *inst;
	over = chan->sample_idx - chan->seg_end;
	for( ;; ) {
		if( !chan->pend_valid ) {
			/* No loop to reload: stop, retaining the overshoot is moot. */
			chan->dma_active = 0;
			chan->sample_idx = chan->seg_end;
			return;
		}
		ins = chan->pend_instrument;
		inst = &player->instruments[ ins ];
		chan->instrument = (unsigned char) ins;
		chan->sample_idx = inst->loop_start + over;
		chan->seg_end = inst->loop_start + inst->loop_length;
		dma_refresh_pending( player, chan );
		if( chan->sample_idx < chan->seg_end ) break;
		/* The carried distance crosses another whole round; keep the
		   fraction against the newest descriptor, never the old edges. */
		over = chan->sample_idx - chan->seg_end;
	}
}

static void trigger( struct micromod_player *player, struct mm_channel *channel ) {
	long period, ins;
	ins = channel->note.instrument;
	if( ins > 0 && ins < 32 ) {
		channel->assigned = (unsigned char) ins;
		channel->sample_offset = 0;
		channel->fine_tune = player->instruments[ ins ].fine_tune;
		channel->volume = player->instruments[ ins ].volume;
		if( dma_mode( player ) ) {
			/* Selection changes immediately; the playing segment is untouched. */
			dma_refresh_pending( player, channel );
			channel->funk_cursor = player->instruments[ ins ].loop_start >> FP_SHIFT;
		} else if( player->instruments[ ins ].loop_length > 0 && channel->instrument > 0 ) {
			channel->instrument = (unsigned char) ins;
		}
	}
	if( channel->note.effect == 0x09 ) {
		channel->sample_offset = ( channel->note.param & 0xFF ) << 8;
	} else if( channel->note.effect == 0x15 ) {
		channel->fine_tune = channel->note.param;
	}
	if( channel->note.key > 0 ) {
		period = ( channel->note.key * fine_tuning[ channel->fine_tune & 0xF ] ) >> 11;
		channel->porta_period = ( period >> 1 ) + ( period & 1 );
		if( channel->note.effect != 0x3 && channel->note.effect != 0x5 ) {
			channel->period = channel->porta_period;
			channel->sample_idx = ( channel->sample_offset << FP_SHIFT );
			if( dma_mode( player ) ) {
				dma_start_first( player, channel );
				if( channel->sample_idx >= channel->seg_end )
					dma_reload( player, channel );
			} else {
				channel->instrument = channel->assigned;
			}
			if( channel->vibrato_type < 4 ) channel->vibrato_phase = 0;
			if( channel->tremolo_type < 4 ) channel->tremolo_phase = 0;
		}
	}
}

static void channel_row( struct micromod_player *player, struct mm_channel *chan ) {
	long effect, param, volume, period;
	effect = chan->note.effect;
	param = chan->note.param;
	chan->vibrato_add = chan->tremolo_add = chan->arpeggio_add = chan->fx_count = 0;
	if( !( effect == 0x1D && param > 0 ) ) {
		/* Not note delay. */
		trigger( player, chan );
	}
	switch( effect ) {
		case 0x3: /* Tone Portamento.*/
			if( param > 0 ) chan->porta_speed = (unsigned char) param;
			break;
		case 0x4: /* Vibrato.*/
			if( ( param & 0xF0 ) > 0 ) chan->vibrato_speed = param >> 4;
			if( ( param & 0x0F ) > 0 ) chan->vibrato_depth = param & 0xF;
			vibrato( player, chan );
			break;
		case 0x6: /* Vibrato + Volume Slide.*/
			vibrato( player, chan );
			break;
		case 0x7: /* Tremolo.*/
			if( ( param & 0xF0 ) > 0 ) chan->tremolo_speed = param >> 4;
			if( ( param & 0x0F ) > 0 ) chan->tremolo_depth = param & 0xF;
			tremolo( player, chan );
			break;
		case 0x8: /* Set Panning (0-127). Not for 4-channel Protracker. */
			if( player->num_channels != 4 ) {
				chan->panning = ( param < 128 ) ? (unsigned char) param : 127;
			}
			break;
		case 0xB: /* Pattern Jump.*/
			if( player->pl_count < 0 ) {
				player->break_pattern = param;
				player->next_row = 0;
			}
			break;
		case 0xC: /* Set Volume.*/
			chan->volume = param > 64 ? 64 : (unsigned char) param;
			break;
		case 0xD: /* Pattern Break.*/
			if( player->pl_count < 0 ) {
				if( player->break_pattern < 0 ) player->break_pattern = player->pattern + 1;
				player->next_row = ( param >> 4 ) * 10 + ( param & 0xF );
				if( player->next_row >= 64 ) player->next_row = 0;
			}
			break;
		case 0xF: /* Set Speed.*/
			if( param > 0 ) {
				if( param < 32 ) player->tick = player->speed = param;
				else set_tempo( player, param );
			}
			break;
		case 0x11: /* Fine Portamento Up.*/
			period = chan->period - param;
			chan->period = period < 0 ? 0 : (unsigned short) period;
			break;
		case 0x12: /* Fine Portamento Down.*/
			period = chan->period + param;
			chan->period = period > 65535 ? 65535 : (unsigned short) period;
			break;
		case 0x14: /* Set Vibrato Waveform.*/
			if( param < 8 ) chan->vibrato_type = (unsigned char) param;
			break;
		case 0x16: /* Pattern Loop.*/
			if( param == 0 ) /* Set loop marker on this channel. */
				chan->pl_row = (unsigned char) player->row;
			if( chan->pl_row < player->row && player->break_pattern < 0 ) { /* Marker valid. */
				if( player->pl_count < 0 ) { /* Not already looping, begin. */
					player->pl_count = param;
					player->pl_channel = chan->id;
				}
				if( player->pl_channel == chan->id ) { /* Next Loop.*/
					if( player->pl_count == 0 ) { /* Loop finished. */
						/* Invalidate current marker. */
						chan->pl_row = (unsigned char) ( player->row + 1 );
					} else { /* Loop. */
						player->next_row = chan->pl_row;
					}
					player->pl_count--;
				}
			}
			break;
		case 0x17: /* Set Tremolo Waveform.*/
			if( param < 8 ) chan->tremolo_type = (unsigned char) param;
			break;
		case 0x1A: /* Fine Volume Up.*/
			volume = chan->volume + param;
			chan->volume = volume > 64 ? 64 : (unsigned char) volume;
			break;
		case 0x1B: /* Fine Volume Down.*/
			volume = chan->volume - param;
			chan->volume = volume < 0 ? 0 : (unsigned char) volume;
			break;
		case 0x1C: /* Note Cut.*/
			if( param <= 0 ) chan->volume = 0;
			break;
		case 0x1D: /* Note Delay.*/
			player->tick = player->speed + player->speed * param;
			break;
		case 0x1E: /* Pattern Delay.*/
			player->tick = player->speed + player->speed * param;
			break;
	}
	if( dma_mode( player ) && effect == 0x1F ) {
		/* EFx Inverse-Sample (funk) speed. 0 stops; non-zero persists. */
		chan->funk_speed = (unsigned char) param;
	}
	update_frequency( player, chan );
}

static void channel_tick( struct micromod_player *player, struct mm_channel *chan ) {
	long effect, param, period;
	effect = chan->note.effect;
	param = chan->note.param;
	chan->fx_count++;
	switch( effect ) {
		case 0x1: /* Portamento Up.*/
			period = chan->period - param;
			chan->period = period < 0 ? 0 : (unsigned short) period;
			break;
		case 0x2: /* Portamento Down.*/
			period = chan->period + param;
			chan->period = period > 65535 ? 65535 : (unsigned short) period;
			break;
		case 0x3: /* Tone Portamento.*/
			tone_portamento( chan );
			break;
		case 0x4: /* Vibrato.*/
			chan->vibrato_phase += chan->vibrato_speed;
			vibrato( player, chan );
			break;
		case 0x5: /* Tone Porta + Volume Slide.*/
			tone_portamento( chan );
			volume_slide( chan, param );
			break;
		case 0x6: /* Vibrato + Volume Slide.*/
			chan->vibrato_phase += chan->vibrato_speed;
			vibrato( player, chan );
			volume_slide( chan, param );
			break;
		case 0x7: /* Tremolo.*/
			chan->tremolo_phase += chan->tremolo_speed;
			tremolo( player, chan );
			break;
		case 0xA: /* Volume Slide.*/
			volume_slide( chan, param );
			break;
		case 0xE: /* Arpeggio.*/
			if( chan->fx_count > 2 ) chan->fx_count = 0;
			if( chan->fx_count == 0 ) chan->arpeggio_add = 0;
			if( chan->fx_count == 1 ) chan->arpeggio_add = (signed char) ( param >> 4 );
			if( chan->fx_count == 2 ) chan->arpeggio_add = (signed char) ( param & 0xF );
			break;
		case 0x19: /* Retrig.*/
			if( chan->fx_count >= param ) {
				chan->fx_count = 0;
				if( dma_mode( player ) ) {
					/* Restart the first segment of the selected instrument. */
					chan->sample_idx = 0;
					dma_start_first( player, chan );
				} else {
					chan->sample_idx = 0;
				}
			}
			break;
		case 0x1C: /* Note Cut.*/
			if( param == chan->fx_count ) chan->volume = 0;
			break;
		case 0x1D: /* Note Delay.*/
			if( param == chan->fx_count ) trigger( player, chan );
			break;
	}
	if( effect > 0 ) update_frequency( player, chan );
}

/* One inverse-sample step for a channel, run after normal row/tick work. */
static void funk_step( struct micromod_player *player, struct mm_channel *chan ) {
	long rate, steps, ins, idx;
	struct mm_instrument *inst;
	unsigned char *store;
	rate = chan->funk_speed;
	if( rate == 0 ) return;
	steps = funk_rates[ rate & 0xF ];
	chan->funk_acc = (unsigned char)( chan->funk_acc + steps );
	if( chan->funk_acc < 128 ) return;
	chan->funk_acc = 0; /* No remainder is kept. */
	ins = chan->assigned;
	if( ins <= 0 || ins >= 32 ) return;
	inst = &player->instruments[ ins ];
	if( inst->loop_length < (unsigned long)( 4 << FP_SHIFT ) ) return; /* No valid loop. */
	store = (unsigned char *) player->sample_store;
	if( !store ) return;
	chan->funk_cursor++;
	if( chan->funk_cursor >= ( inst->loop_start >> FP_SHIFT ) + ( inst->loop_length >> FP_SHIFT ) )
		chan->funk_cursor = inst->loop_start >> FP_SHIFT;
	idx = inst->sample_offset + chan->funk_cursor;
	store[ idx ] = (unsigned char)( -1 - (signed char) store[ idx ] );
}

static long sequence_row( struct micromod_player *player ) {
	long song_end, chan_idx, pat_offset;
	long effect, param;
	struct mm_note *note;
	song_end = 0;
	if( player->next_row < 0 ) {
		player->break_pattern = player->pattern + 1;
		player->next_row = 0;
	}
	if( player->break_pattern >= 0 ) {
		if( player->break_pattern >= player->song_length ) player->break_pattern = player->next_row = 0;
		if( player->break_pattern <= player->pattern ) song_end = 1;
		player->pattern = player->break_pattern;
		for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ )
			player->channels[ chan_idx ].pl_row = 0;
		player->break_pattern = -1;
	}
	player->row = player->next_row;
	player->next_row = player->row + 1;
	if( player->next_row >= 64 ) player->next_row = -1;
	pat_offset = ( ( player->sequence[ player->pattern ] & 0x7F ) * 64 + player->row )
			* player->num_channels * 4;
	for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ ) {
		note = &player->channels[ chan_idx ].note;
		note->key  = ( player->pattern_data[ pat_offset ] & 0xF ) << 8;
		note->key |=   player->pattern_data[ pat_offset + 1 ];
		note->instrument  = player->pattern_data[ pat_offset + 2 ] >> 4;
		note->instrument |= player->pattern_data[ pat_offset ] & 0x10;
		effect = player->pattern_data[ pat_offset + 2 ] & 0xF;
		param = player->pattern_data[ pat_offset + 3 ];
		pat_offset += 4;
		if( effect == 0xE ) {
			effect = 0x10 | ( param >> 4 );
			param &= 0xF;
		}
		if( effect == 0 && param > 0 ) effect = 0xE;
		note->effect = (unsigned char) effect;
		note->param = (unsigned char) param;
		channel_row( player, &player->channels[ chan_idx ] );
	}
	if( dma_mode( player ) ) {
		for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ )
			funk_step( player, &player->channels[ chan_idx ] );
	}
	return song_end;
}

static long sequence_tick( struct micromod_player *player ) {
	long song_end, chan_idx;
	song_end = 0;
	if( --player->tick <= 0 ) {
		player->tick = player->speed;
		song_end = sequence_row( player );
	} else {
		for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ )
			channel_tick( player, &player->channels[ chan_idx ] );
		if( dma_mode( player ) ) {
			for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ )
				funk_step( player, &player->channels[ chan_idx ] );
		}
	}
	return song_end;
}

/* Legacy resampler: the channel's instrument is swapped immediately, so the
   loop points always describe the playing sample. */
static void resample_legacy( struct micromod_player *player, struct mm_channel *chan,
		short *buf, long offset, long count ) {
	unsigned long epos;
	unsigned long buf_idx = offset << 1;
	unsigned long buf_end = ( offset + count ) << 1;
	unsigned long sidx = chan->sample_idx;
	unsigned long step = chan->step;
	unsigned long llen = player->instruments[ chan->instrument ].loop_length;
	unsigned long lep1 = player->instruments[ chan->instrument ].loop_start + llen;
	const signed char *sdat = player->instruments[ chan->instrument ].sample_data;
	short ampl = buf && !chan->mute ? chan->ampl : 0;
	short lamp = ampl * ( 127 - chan->panning ) >> 5;
	short ramp = ampl * chan->panning >> 5;
	while( buf_idx < buf_end ) {
		if( sidx >= lep1 ) {
			/* Handle loop. */
			if( llen <= FP_ONE ) {
				/* One-shot sample. */
				sidx = lep1;
				break;
			}
			/* Subtract loop-length until within loop points. */
			while( sidx >= lep1 ) sidx -= llen;
		}
		/* Calculate sample position at end. */
		epos = sidx + ( ( buf_end - buf_idx ) >> 1 ) * step;
		/* Most of the cpu time is spent here. */
		if( lamp || ramp ) {
			/* Only mix to end of current loop. */
			if( epos > lep1 ) epos = lep1;
			if( lamp && ramp ) {
				/* Mix both channels. */
				while( sidx < epos ) {
					ampl = sdat[ sidx >> FP_SHIFT ];
					buf[ buf_idx++ ] += ampl * lamp >> 2;
					buf[ buf_idx++ ] += ampl * ramp >> 2;
					sidx += step;
				}
			} else {
				/* Only mix one channel. */
				if( ramp ) buf_idx++;
				while( sidx < epos ) {
					buf[ buf_idx ] += sdat[ sidx >> FP_SHIFT ] * ampl;
					buf_idx += 2;
					sidx += step;
				}
				buf_idx &= -2;
			}
		} else {
			/* No need to mix.*/
			buf_idx = buf_end;
			sidx = epos;
		}
	}
	chan->sample_idx = sidx;
}

/* DMA resampler: the active segment owns its own end position and its
   instrument may differ from the currently selected one. */
static void resample_dma( struct micromod_player *player, struct mm_channel *chan,
		short *buf, long offset, long count ) {
	unsigned long epos;
	unsigned long buf_idx = offset << 1;
	unsigned long buf_end = ( offset + count ) << 1;
	unsigned long sidx = chan->sample_idx;
	unsigned long step = chan->step;
	unsigned long seg_end;
	const signed char *sdat;
	short ampl = buf && !chan->mute ? chan->ampl : 0;
	short lamp = ampl * ( 127 - chan->panning ) >> 5;
	short ramp = ampl * chan->panning >> 5;
	if( !chan->dma_active || chan->instrument == 0 ) {
		chan->sample_idx = sidx;
		return;
	}
	sdat = player->instruments[ chan->instrument ].sample_data;
	seg_end = chan->seg_end;
	while( buf_idx < buf_end ) {
		if( sidx >= seg_end ) {
			chan->sample_idx = sidx;
			dma_reload( player, chan );
			if( !chan->dma_active ) return;
			sidx = chan->sample_idx;
			sdat = player->instruments[ chan->instrument ].sample_data;
			seg_end = chan->seg_end;
		}
		epos = sidx + ( ( buf_end - buf_idx ) >> 1 ) * step;
		if( lamp || ramp ) {
			if( epos > seg_end ) epos = seg_end;
			if( lamp && ramp ) {
				while( sidx < epos ) {
					ampl = sdat[ sidx >> FP_SHIFT ];
					buf[ buf_idx++ ] += ampl * lamp >> 2;
					buf[ buf_idx++ ] += ampl * ramp >> 2;
					sidx += step;
				}
			} else {
				if( ramp ) buf_idx++;
				while( sidx < epos ) {
					buf[ buf_idx ] += sdat[ sidx >> FP_SHIFT ] * ampl;
					buf_idx += 2;
					sidx += step;
				}
				buf_idx &= -2;
			}
		} else {
			buf_idx = buf_end;
			sidx = epos;
		}
		if( sidx < seg_end || buf_idx >= buf_end ) break;
		/* Reached the segment edge inside this request. */
	}
	chan->sample_idx = sidx;
}

long mm_core_initialise( struct micromod_player *player, const signed char *data,
		size_t length, long sampling_rate, unsigned flags ) {
	struct mm_instrument *inst;
	long sample_data_offset, inst_idx;
	long sample_length, volume, fine_tune, loop_start, loop_length;
	long expected_len;
	memset( player, 0, sizeof( *player ) );
	if( ( flags & ~1u ) != 0 ) return -1;
	player->num_channels = calculate_num_channels( data );
	if( player->num_channels <= 0 ) {
		player->num_channels = 0;
		return -1;
	}
	if( sampling_rate < 8000 ) return -2;
	if( ( flags & 1u ) != 0 && player->num_channels != 4 ) return -3;
	expected_len = mm_calculate_mod_file_len( data );
	if( expected_len < 0 ) return -1;
	if( length != MM_LEGACY_DATA_LENGTH && (unsigned long) expected_len > length )
		return -1;
	player->module_data = data;
	player->data_length = length;
	player->flags = flags;
	player->sample_rate = sampling_rate;
	player->song_length = data[ 950 ] & 0x7F;
	player->restart = data[ 951 ] & 0x7F;
	if( player->restart >= player->song_length ) player->restart = 0;
	player->sequence = (const unsigned char *) data + 952;
	player->pattern_data = (const unsigned char *) data + 1084;
	player->num_patterns = calculate_num_patterns( data );
	sample_data_offset = 1084 + player->num_patterns * 64 * player->num_channels * 4;
	player->sample_store_len = 0;
	for( inst_idx = 1; inst_idx < 32; inst_idx++ ) {
		sample_length = unsigned_short_big_endian( data, inst_idx * 30 + 12 ) * 2;
		player->sample_store_len += (unsigned long) sample_length;
	}
	if( dma_mode( player ) ) {
		/* Each player owns a private, writable copy of every sample byte. */
		player->sample_store = malloc( player->sample_store_len ? player->sample_store_len : 1 );
		if( !player->sample_store ) return -1;
		memcpy( player->sample_store, data + sample_data_offset, player->sample_store_len );
		player->sample_src = player->sample_store;
	} else {
		player->sample_store = NULL;
		player->sample_src = data + sample_data_offset;
	}
	for( inst_idx = 1; inst_idx < 32; inst_idx++ ) {
		inst = &player->instruments[ inst_idx ];
		sample_length = unsigned_short_big_endian( data, inst_idx * 30 + 12 ) * 2;
		fine_tune = data[ inst_idx * 30 + 14 ] & 0xF;
		inst->fine_tune = (unsigned char)( ( fine_tune & 0x7 ) - ( fine_tune & 0x8 ) + 8 );
		volume = data[ inst_idx * 30 + 15 ] & 0x7F;
		inst->volume = (unsigned char)( volume > 64 ? 64 : volume );
		loop_start = unsigned_short_big_endian( data, inst_idx * 30 + 16 ) * 2;
		loop_length = unsigned_short_big_endian( data, inst_idx * 30 + 18 ) * 2;
		if( loop_start + loop_length > sample_length ) {
			if( loop_start / 2 + loop_length <= sample_length ) {
				/* Some old modules have loop start in bytes. */
				loop_start = loop_start / 2;
			} else {
				loop_length = sample_length - loop_start;
			}
		}
		if( loop_length < 4 ) {
			loop_start = sample_length;
			loop_length = 0;
		}
		inst->sample_length = (unsigned long) sample_length;
		inst->loop_start = (unsigned long) loop_start << FP_SHIFT;
		inst->loop_length = (unsigned long) loop_length << FP_SHIFT;
		inst->sample_offset = (unsigned long)( sample_data_offset - ( 1084
				+ player->num_patterns * 64 * player->num_channels * 4 ) );
		inst->sample_data = player->sample_src + inst->sample_offset;
		sample_data_offset += sample_length;
	}
	player->c2_rate = ( player->num_channels > 4 ) ? 8363 : 8287;
	player->gain = ( player->num_channels > 4 ) ? 32 : 64;
	mm_core_mute_channel( player, -1 );
	mm_core_reset( player, 0 );
	return 0;
}

void mm_core_get_string( struct micromod_player *player, long instrument, char *string ) {
	long index, offset, length, character;
	if( player->num_channels <= 0 ) {
		string[ 0 ] = 0;
		return;
	}
	offset = 0;
	length = 20;
	if( instrument > 0 && instrument < 32 ) {
		offset = ( instrument - 1 ) * 30 + 20;
		length = 22;
	}
	for( index = 0; index < length; index++ ) {
		character = player->module_data[ offset + index ];
		if( character < 32 || character > 126 ) character = ' ';
		string[ index ] = (char) character;
	}
	string[ length ] = 0;
}

long mm_core_calculate_song_duration( struct micromod_player *player ) {
	long duration, song_end;
	duration = 0;
	if( player->num_channels > 0 ) {
		mm_core_reset( player, 0 );
		song_end = 0;
		while( !song_end ) {
			duration += player->tick_len;
			song_end = sequence_tick( player );
		}
		mm_core_reset( player, 0 );
	}
	return duration;
}

void mm_core_reset( struct micromod_player *player, long pos ) {
	long chan_idx;
	struct mm_channel *chan;
	unsigned char mute[ MAX_CHANNELS ];
	if( player->num_channels <= 0 ) return;
	if( pos >= player->song_length ) pos = 0;
	/* Gain and mute configuration survive the reset. */
	for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ )
		mute[ chan_idx ] = player->channels[ chan_idx ].mute;
	/* Restore the original sample bytes and rebuild writable pointers. */
	if( player->sample_store ) {
		long sample_data_base = 1084 + player->num_patterns * 64 * player->num_channels * 4;
		memcpy( player->sample_store, player->module_data + sample_data_base,
				player->sample_store_len );
	}
	player->break_pattern = pos;
	player->next_row = 0;
	player->tick = 1;
	player->speed = 6;
	set_tempo( player, 125 );
	player->pl_count = player->pl_channel = -1;
	player->random_seed = 0xABCDEF;
	player->tick_offset = 0;
	for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ ) {
		chan = &player->channels[ chan_idx ];
		memset( chan, 0, sizeof( *chan ) );
		chan->id = (unsigned char) chan_idx;
		chan->mute = mute[ chan_idx ];
		switch( chan_idx & 0x3 ) {
			case 0: case 3: chan->panning = 0; break;
			case 1: case 2: chan->panning = 127; break;
		}
	}
	sequence_tick( player );
	player->tick_offset = 0;
}

long mm_core_mute_channel( struct micromod_player *player, long channel ) {
	long chan_idx;
	if( channel < 0 ) {
		for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ ) {
			player->channels[ chan_idx ].mute = 0;
		}
	} else if( channel < player->num_channels ) {
		player->channels[ channel ].mute = 1;
	}
	return player->num_channels;
}

void mm_core_set_gain( struct micromod_player *player, long value ) {
	player->gain = value;
}

void mm_core_get_audio( struct micromod_player *player, short *output_buffer, long count ) {
	long offset, remain, chan_idx;
	if( player->num_channels <= 0 || count <= 0 ) return;
	offset = 0;
	while( count > 0 ) {
		remain = player->tick_len - player->tick_offset;
		if( remain > count ) remain = count;
		for( chan_idx = 0; chan_idx < player->num_channels; chan_idx++ ) {
			if( dma_mode( player ) )
				resample_dma( player, &player->channels[ chan_idx ], output_buffer, offset, remain );
			else
				resample_legacy( player, &player->channels[ chan_idx ], output_buffer, offset, remain );
		}
		player->tick_offset += remain;
		if( player->tick_offset == player->tick_len ) {
			sequence_tick( player );
			player->tick_offset = 0;
		}
		offset += remain;
		count -= remain;
	}
}
