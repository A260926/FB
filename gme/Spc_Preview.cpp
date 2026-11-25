// Speculative SPC preview with editable timed CPU input ports.

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#include "Spc_Preview.h"

#include "Snes_Spc.h"
#include "Spc_Filter.h"
#include "Fir_Resampler.h"

#include <algorithm>
#include <string.h>

/* Copyright (C) 2024 Shay Green. This module is free software; you
can redistribute it and/or modify it under the terms of the GNU Lesser
General Public License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version. This
module is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
details. You should have received a copy of the GNU Lesser General Public
License along with this module; if not, write to the Free Software Foundation,
Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA */

#include "blargg_source.h"

namespace {

enum {
	native_rate     = Snes_Spc::sample_rate, // 32000
	max_event_keys  = 256,
	max_event_frame = 1 << 30,
	max_read_frames = 4096,
	min_lookahead   = 512,
	max_lookahead   = 4096,
	fir_buffer_size = 3200, // native samples, matches the production SPC chain
	fir_width       = 24,
	fir_write_off   = fir_width * 2 - 2, // Fir_Resampler_::write_offset
	fir_total       = fir_buffer_size + fir_write_off,
	native_chunk    = 1600  // native stereo pairs per Snes_Spc::play() batch
};

} // namespace

// One complete emulation site: SPC700/CPU and timers, the DSP (BRR state,
// envelopes, echo history), the SNES output filter and the FIR resampler.
// Every member is POD-like with no virtual functions; the only out-of-object
// pointers are the DSP's RAM pointer (into apu itself) and the FIR input
// buffer pointer (into a long-lived heap block). Snapshots are byte copies
// restored back into the same object, so those pointers always stay valid.
struct Spc_Preview::Engine {
	Snes_Spc apu;
	SPC_Filter filter;
	Fir_Resampler<24> fir;
	bool resampling;

	Engine() : resampling( false ) { }
};

struct Spc_Preview::Event {
	uint64_t frame;
	unsigned port;
	unsigned value;
};

Spc_Preview::Spc_Preview() :
	rate_( 0 ),
	lookahead_( 0 ),
	delivered_( 0 ),
	committed_native_( 0 ),
	speculative_native_( 0 ),
	synthesized_( 0 ),
	rerenders_( 0 ),
	pending_frames_( 0 ),
	begun_( false ),
	live_( 0 )
{
}

Spc_Preview::~Spc_Preview()
{
	reset();
}

void Spc_Preview::reset()
{
	delete live_;
	live_ = 0;
	file_.clear();
	commit_blob_.clear();
	temp_blob_.clear();
	commit_fir_.clear();
	temp_fir_.clear();
	events_.clear();
	pending_.clear();
	work_.clear();
	rate_ = 0;
	lookahead_ = 0;
	delivered_ = 0;
	committed_native_ = 0;
	speculative_native_ = 0;
	synthesized_ = 0;
	rerenders_ = 0;
	pending_frames_ = 0;
	begun_ = false;
}

Spc_Preview::Status Spc_Preview::status() const
{
	Status s;
	s.delivered_frames          = delivered_;
	s.committed_native_frame    = committed_native_;
	s.speculative_native_frame  = speculative_native_;
	s.synthesized_native_frames = synthesized_;
	s.rerenders                 = rerenders_;
	s.pending_frames            = pending_frames_;
	s.queued_events             = (unsigned) events_.size();
	return s;
}

void Spc_Preview::save_site( std::vector<unsigned char>& blob,
		std::vector<short>& fir ) const
{
	blob.resize( sizeof( Engine ) );
	memcpy( static_cast<void*>( blob.data() ), static_cast<const void*>( live_ ),
			sizeof( Engine ) );
	if ( live_->resampling )
	{
		// Preserve the FIR input ring contents; the pointers inside the Engine
		// stay valid because the heap block is never reallocated and every
		// restore targets this same live object.
		fir.resize( fir_total );
		short* base = live_->fir.buffer() - live_->fir.written() - fir_write_off;
		memcpy( fir.data(), base, fir_total * sizeof( short ) );
	}
	else
	{
		fir.clear();
	}
}

void Spc_Preview::restore_site( const std::vector<unsigned char>& blob,
		const std::vector<short>& fir )
{
	memcpy( static_cast<void*>( live_ ), static_cast<const void*>( blob.data() ),
			sizeof( Engine ) );
	if ( live_->resampling )
	{
		short* base = live_->fir.buffer() - live_->fir.written() - fir_write_off;
		memcpy( base, fir.data(), fir_total * sizeof( short ) );
	}
}

blargg_err_t Spc_Preview::begin( const void* data, long size, long rate, unsigned lookahead )
{
	if ( !data || size < Snes_Spc::spc_min_file_size )
		return "Invalid SPC preview data";
	if ( rate != 32000 && rate != 44100 && rate != 48000 )
		return "Unsupported SPC preview sample rate";
	if ( lookahead < min_lookahead || lookahead > max_lookahead )
		return "SPC preview lookahead out of range";

	// Build the replacement site fully before touching live state, so a failed
	// begin() leaves the previous playback completely intact.
	Engine* fresh = BLARGG_NEW Engine;
	if ( !fresh )
		return "Out of memory";

	blargg_err_t err = fresh->apu.init();
	if ( !err )
		err = fresh->apu.load_spc( data, size );
	if ( !err )
		fresh->apu.clear_echo();
	if ( !err )
	{
		fresh->filter.clear();
		fresh->filter.enable( true );
		fresh->filter.set_gain( int( 1.4 * SPC_Filter::gain_unit ) );
		fresh->resampling = ( rate != native_rate );
		if ( fresh->resampling )
		{
			err = fresh->fir.buffer_size( fir_buffer_size );
			if ( !err )
				fresh->fir.time_ratio( double( native_rate ) / rate, 0.9965 );
		}
	}
	if ( err )
	{
		delete fresh;
		return err;
	}

	reset();
	live_ = fresh;
	rate_ = rate;
	lookahead_ = lookahead;
	file_.assign( static_cast<const unsigned char*>( data ),
			static_cast<const unsigned char*>( data ) + size );
	pending_.resize( (lookahead_ + max_read_frames) * 2 );
	work_.resize( max_read_frames * 2 );
	save_site( commit_blob_, commit_fir_ );
	temp_blob_ = commit_blob_;
	temp_fir_ = commit_fir_;
	begun_ = true;
	return 0;
}

blargg_err_t Spc_Preview::schedule_port( uint64_t native_frame, unsigned port, unsigned value )
{
	if ( !begun_ )
		return "SPC preview not begun";
	if ( port >= Snes_Spc::port_count || value > 255 )
		return "Invalid SPC port event";
	if ( native_frame < committed_native_ || native_frame >= max_event_frame )
		return "SPC port event outside accepted frame range";

	// Events stay sorted by (frame, port), which is also the order writes must
	// reach the CPU within one native frame.
	size_t pos = 0;
	while ( pos < events_.size() )
	{
		const Event& e = events_[pos];
		if ( e.frame == native_frame && e.port == port )
		{
			if ( e.value == value )
				return 0; // identical schedule: no action at all
			events_[pos].value = value;
			if ( native_frame < speculative_native_ )
				discard_pending();
			return 0;
		}
		if ( e.frame > native_frame || ( e.frame == native_frame && e.port > port ) )
			break;
		pos++;
	}

	if ( events_.size() >= max_event_keys )
		return "SPC port event queue full";

	Event ev;
	ev.frame = native_frame;
	ev.port  = port;
	ev.value = value;
	events_.insert( events_.begin() + pos, ev );

	if ( native_frame < speculative_native_ )
		discard_pending();
	return 0;
}

void Spc_Preview::discard_pending()
{
	// Restore the entire speculative site (CPU/RAM/timers, DSP BRR/envelope/
	// echo, filter history and FIR fractional phase and input history) to the
	// committed site; the buffered PCM is discarded rather than patched in place.
	restore_site( commit_blob_, commit_fir_ );
	speculative_native_ = committed_native_;
	pending_frames_ = 0;
	rerenders_++;
}

// Runs the live engine for frames native frames starting at native_pos, writing
// the filtered stereo result to out. event_index is advanced past every event
// applied. Event frame boundaries split the batches exactly, matching the
// synchronous reference.
blargg_err_t Spc_Preview::pump( long frames, size_t& event_index,
		uint64_t native_pos, short* out )
{
	Engine& e = *live_;
	long pos = 0;
	while ( pos < frames )
	{
		while ( event_index < events_.size() &&
				events_[event_index].frame == native_pos )
		{
			e.apu.write_port( 0, int( events_[event_index].port ),
					int( events_[event_index].value ) );
			event_index++;
		}

		long chunk = frames - pos;
		if ( event_index < events_.size() &&
				events_[event_index].frame > native_pos )
		{
			uint64_t distance = events_[event_index].frame - native_pos;
			if ( distance < uint64_t( chunk ) )
				chunk = long( distance );
		}
		if ( chunk <= 0 )
			return "Corrupt SPC port schedule";

		blargg_err_t err = e.apu.play( chunk * 2, out + pos * 2 );
		if ( err )
			return err;
		synthesized_ += uint64_t( chunk );
		native_pos += uint64_t( chunk );
		pos += chunk;
	}
	e.filter.run( out, int( frames * 2 ) );
	return 0;
}

blargg_err_t Spc_Preview::render_to( unsigned target_frames )
{
	Engine& e = *live_;
	size_t ei = 0;
	while ( ei < events_.size() && events_[ei].frame < speculative_native_ )
		ei++;

	blargg_err_t err = 0;
	if ( !e.resampling )
	{
		while ( pending_frames_ < target_frames )
		{
			long n = std::min<long>( native_chunk,
					long( target_frames - pending_frames_ ) );
			err = pump( n, ei, speculative_native_,
					pending_.data() + pending_frames_ * 2 );
			if ( err )
				return err;
			speculative_native_ += uint64_t( n );
			pending_frames_ += unsigned( n );
		}
		return 0;
	}

	while ( pending_frames_ < target_frames )
	{
		int got = e.fir.read( pending_.data() + pending_frames_ * 2,
				int( target_frames - pending_frames_ ) * 2 );
		pending_frames_ += unsigned( got / 2 );
		if ( pending_frames_ >= target_frames )
			break;

		int size = e.fir.max_write();
		err = pump( size / 2, ei, speculative_native_, e.fir.buffer() );
		if ( err )
			return err;
		e.fir.write( size );
		speculative_native_ += uint64_t( size / 2 );
	}
	return err;
}

blargg_err_t Spc_Preview::prefetch()
{
	if ( !begun_ )
		return "SPC preview not begun";
	if ( pending_frames_ >= lookahead_ )
		return 0;
	return render_to( lookahead_ );
}

// Advances the committed engine to produce frames output frames, consuming the
// same events the speculative engine used for the delivered prefix.
blargg_err_t Spc_Preview::advance_committed( long frames )
{
	Engine& e = *live_;
	size_t ei = 0; // surviving events all start at or after committed_native_

	if ( !e.resampling )
	{
		blargg_err_t err = pump( frames, ei, committed_native_, work_.data() );
		if ( err )
			return err;
		committed_native_ += uint64_t( frames );
		return 0;
	}

	long drained = 0;
	while ( drained < frames )
	{
		int got = e.fir.read( work_.data(), int( frames - drained ) * 2 );
		drained += got / 2;
		if ( drained >= frames )
			break;
		int size = e.fir.max_write();
		blargg_err_t err = pump( size / 2, ei, committed_native_, e.fir.buffer() );
		if ( err )
			return err;
		e.fir.write( size );
		committed_native_ += uint64_t( size / 2 );
	}
	return 0;
}

void Spc_Preview::age_events()
{
	size_t cut = 0;
	while ( cut < events_.size() && events_[cut].frame < committed_native_ )
		cut++;
	if ( cut )
		events_.erase( events_.begin(), events_.begin() + cut );
}

blargg_err_t Spc_Preview::read( short* out, long frames )
{
	if ( !begun_ )
		return "SPC preview not begun";
	if ( frames < 0 || frames > max_read_frames )
		return "SPC preview read length out of range";
	if ( frames == 0 )
		return 0;
	if ( !out )
		return "SPC preview read requires an output buffer";

	if ( pending_frames_ < unsigned( frames ) )
	{
		unsigned target = std::max( lookahead_, unsigned( frames ) );
		blargg_err_t err = render_to( target );
		if ( err )
			return err;
	}

	// The live object is the speculative engine and also owns the still-valid
	// FIR buffer behind the pending suffix; remember it before replaying the
	// committed engine over the same object.
	save_site( temp_blob_, temp_fir_ );

	memcpy( out, pending_.data(), size_t( frames ) * 2 * sizeof( short ) );
	memmove( pending_.data(), pending_.data() + frames * 2,
			size_t( pending_frames_ - frames ) * 2 * sizeof( short ) );
	pending_frames_ -= unsigned( frames );
	delivered_ += uint64_t( frames );

	if ( pending_frames_ == 0 )
	{
		// No speculative audio remains: the live engine already sits exactly at
		// the boundary, FIR look-ahead included, so it becomes the committed site.
		committed_native_ = speculative_native_;
		save_site( commit_blob_, commit_fir_ );
	}
	else
	{
		// Replay the delivered prefix from the previous committed site to pin the
		// new committed boundary (and its FIR fractional phase), then hand the
		// live object back to the speculative engine that owns the pending suffix.
		restore_site( commit_blob_, commit_fir_ );
		blargg_err_t err = advance_committed( frames );
		if ( err )
		{
			restore_site( temp_blob_, temp_fir_ );
			return err;
		}
		save_site( commit_blob_, commit_fir_ );
		restore_site( temp_blob_, temp_fir_ );
	}

	age_events();
	return 0;
}
