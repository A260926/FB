// Speculative SPC preview checks: timed port input, rollback equivalence and
// lifecycle/validation. Uses task_support/check.h; the runner supplies main().

#include "Spc_Preview.h"
#include "../task_support/check.h"
#include "../task_support/spc_fixture.h"
#include "../task_support/offline_ports.h"

#include <vector>
#include <algorithm>

using fixture::PortEvent;

namespace {

std::vector<unsigned char> g_file;

const std::vector<unsigned char>& spc_file()
{
	if ( g_file.empty() )
		g_file = fixture::spc( 13 );
	return g_file;
}

unsigned sample_mismatches( const short* a, const short* b, long frames )
{
	unsigned m = 0;
	for ( long i = 0; i < frames * 2; i++ )
		m += ( a[i] != b[i] );
	return m;
}

// Renders the whole stream through the preview in fixed-size read chunks.
int preview_read_all( Spc_Preview& p, long total, long chunk,
		std::vector<short>& out )
{
	out.assign( size_t( total ) * 2, 0 );
	for ( long pos = 0; pos < total; )
	{
		long n = std::min( chunk, total - pos );
		if ( p.read( out.data() + pos * 2, n ) )
			return 1;
		pos += n;
	}
	return 0;
}

// Synchronous reference that knew the complete plan up front.
int offline_render( long rate, std::vector<PortEvent> plan, long total,
		long chunk, std::vector<short>& out )
{
	fixture::OfflinePorts ref;
	if ( ref.begin( spc_file(), rate, plan ) )
		return 1;
	out.assign( size_t( total ) * 2, 0 );
	for ( long pos = 0; pos < total; )
	{
		long n = std::min( chunk, total - pos );
		if ( ref.read( out.data() + pos * 2, n ) )
			return 1;
		pos += n;
	}
	return 0;
}

// Post-rollback prefetch synthesis ceiling: ceil(la*32000/rate) + 3200.
unsigned work_cap( unsigned lookahead, long rate )
{
	return unsigned( ( lookahead * 32000ULL + uint64_t( rate ) - 1 )
			/ uint64_t( rate ) ) + 3200;
}

// Reads and prefetches until the speculative engine runs ahead of the
// committed boundary, opening a native frame range an edit can land in. The
// frames delivered to open the window are appended to out and pos is advanced,
// so no produced audio is ever dropped.
int open_window( Spc_Preview& p, std::vector<short>& out, long& pos )
{
	for ( int k = 0; k < 12; k++ )
	{
		Spc_Preview::Status s = p.status();
		if ( s.speculative_native_frame > s.committed_native_frame )
			return 0;
		long n = std::min( 257L, long( out.size() / 2 ) - pos );
		if ( n <= 0 || p.read( out.data() + pos * 2, n ) || p.prefetch() )
			return 1;
		pos += n;
	}
	return p.status().speculative_native_frame > p.status().committed_native_frame ? 0 : 1;
}

const long rates[3] = { 32000, 44100, 48000 };

} // namespace

// ---- ports group ---------------------------------------------------------

static int ports_match_reference()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		std::vector<PortEvent> plan = {
			{ 500, 0, 37 }, { 500, 1, 91 }, { 1200, 0, 213 },
			{ 4000, 1, 7 }, { 9000, 0, 61 }
		};
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 2048 ) );
		for ( size_t i = 0; i < plan.size(); i++ )
			TASK_CHECK( !p.schedule_port( plan[i].frame, plan[i].port, plan[i].value ) );
		TASK_CHECK( !p.prefetch() );
		TASK_CHECK( p.status().pending_frames == 2048 );
		TASK_CHECK( p.status().synthesized_native_frames > 0 );

		std::vector<short> got, want;
		TASK_CHECK( !preview_read_all( p, 3000, 257, got ) );
		TASK_CHECK( !offline_render( rate, plan, 3000, 257, want ) );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), 3000 ) == 0 );
		TASK_CHECK( p.status().delivered_frames == 3000 );
	}
	return 0;
}

static int ports_same_frame_dual()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		// Two ports in one frame; within-frame order must be by port number.
		std::vector<PortEvent> plan = { { 700, 1, 123 }, { 700, 0, 45 }, { 3000, 2, 9 } };
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 1024 ) );
		TASK_CHECK( !p.schedule_port( 700, 1, 123 ) );
		TASK_CHECK( !p.schedule_port( 700, 0, 45 ) );
		TASK_CHECK( !p.schedule_port( 3000, 2, 9 ) );
		TASK_CHECK( !p.prefetch() );
		std::vector<short> got, want;
		TASK_CHECK( !preview_read_all( p, 2500, 7, got ) );
		TASK_CHECK( !offline_render( rate, plan, 2500, 7, want ) );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), 2500 ) == 0 );
	}
	return 0;
}

static int ports_replace_and_dedupe()
{
	long rate = 44100;
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 1024 ) );
	TASK_CHECK( !p.schedule_port( 600, 0, 10 ) );
	TASK_CHECK( !p.prefetch() );
	// Replacing an already speculated key rolls back and changes the audio.
	TASK_CHECK( !p.schedule_port( 600, 0, 200 ) );
	TASK_CHECK( p.status().rerenders == 1 );
	TASK_CHECK( p.status().pending_frames == 0 );
	// Re-asserting the same value is a no-op with no extra rerender.
	uint64_t rer = p.status().rerenders;
	TASK_CHECK( !p.schedule_port( 600, 0, 200 ) );
	TASK_CHECK( p.status().rerenders == rer );
	TASK_CHECK( !p.prefetch() );

	std::vector<short> got, want;
	TASK_CHECK( !preview_read_all( p, 2000, 257, got ) );
	TASK_CHECK( !offline_render( rate, { { 600, 0, 200 } }, 2000, 257, want ) );
	TASK_CHECK( sample_mismatches( got.data(), want.data(), 2000 ) == 0 );
	return 0;
}

static int ports_future_append_keeps_cache()
{
	long rate = 48000;
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 1024 ) );
	TASK_CHECK( !p.prefetch() );
	Spc_Preview::Status before = p.status();
	// An event beyond the speculative frontier must not invalidate the cache.
	TASK_CHECK( !p.schedule_port( before.speculative_native_frame + 5000, 0, 77 ) );
	Spc_Preview::Status after = p.status();
	TASK_CHECK( after.rerenders == before.rerenders );
	TASK_CHECK( after.pending_frames == before.pending_frames );
	TASK_CHECK( after.speculative_native_frame == before.speculative_native_frame );
	TASK_CHECK( after.queued_events == 1 );

	std::vector<short> got, want;
	TASK_CHECK( !preview_read_all( p, 2000, 257, got ) );
	TASK_CHECK( !offline_render( rate,
			{ { before.speculative_native_frame + 5000, 0, 77 } }, 2000, 257, want ) );
	TASK_CHECK( sample_mismatches( got.data(), want.data(), 2000 ) == 0 );
	return 0;
}

static int ports_commit_boundary_and_rejects()
{
	// 32 kHz makes the committed native boundary advance one frame per output.
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 32000, 1024 ) );
	TASK_CHECK( !p.schedule_port( 100, 0, 50 ) );
	TASK_CHECK( !p.prefetch() );
	std::vector<short> out( 300 * 2 );
	TASK_CHECK( !p.read( out.data(), 300 ) );
	uint64_t boundary = p.status().committed_native_frame;
	TASK_CHECK( boundary == 300 );

	// Exactly on the boundary is accepted.
	TASK_CHECK( !p.schedule_port( boundary, 0, 60 ) );
	unsigned queued = p.status().queued_events;
	uint64_t rer = p.status().rerenders;
	// One frame too old is rejected and changes nothing.
	TASK_CHECK( p.schedule_port( boundary - 1, 0, 61 ) );
	TASK_CHECK( p.status().queued_events == queued );
	TASK_CHECK( p.status().rerenders == rer );
	// Invalid port/value and the 2^30 ceiling are rejected without side effects.
	TASK_CHECK( p.schedule_port( boundary, 4, 0 ) );
	TASK_CHECK( p.schedule_port( boundary, 0, 256 ) );
	TASK_CHECK( p.schedule_port( uint64_t( 1 ) << 30, 0, 0 ) );
	TASK_CHECK( p.status().queued_events == queued );
	return 0;
}

static int ports_queue_full()
{
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 32000, 1024 ) );
	for ( unsigned i = 0; i < 256; i++ )
		TASK_CHECK( !p.schedule_port( 1000 + i, i % 4, i & 255 ) );
	TASK_CHECK( p.status().queued_events == 256 );
	// The 257th distinct key is rejected; the 256 accepted entries stay intact.
	TASK_CHECK( p.schedule_port( 5000, 0, 1 ) );
	TASK_CHECK( p.status().queued_events == 256 );
	return 0;
}

static int ports_read_chunk_sizes()
{
	const long chunks[3] = { 1, 7, 257 };
	for ( unsigned c = 0; c < 3; c++ )
	{
		long rate = 44100;
		std::vector<PortEvent> plan = { { 300, 0, 88 }, { 2500, 1, 33 } };
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 2048 ) );
		TASK_CHECK( !p.schedule_port( 300, 0, 88 ) );
		TASK_CHECK( !p.schedule_port( 2500, 1, 33 ) );
		TASK_CHECK( !p.prefetch() );
		std::vector<short> got, want;
		TASK_CHECK( !preview_read_all( p, 2600, chunks[c], got ) );
		TASK_CHECK( !offline_render( rate, plan, 2600, chunks[c], want ) );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), 2600 ) == 0 );
		TASK_CHECK( p.status().delivered_frames == 2600 );
	}
	return 0;
}

static int ports_no_event_legacy_path()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 2048 ) );
		TASK_CHECK( !p.prefetch() );
		std::vector<short> got, want;
		TASK_CHECK( !preview_read_all( p, 3000, 257, got ) );
		TASK_CHECK( !offline_render( rate, {}, 3000, 1024, want ) );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), 3000 ) == 0 );
	}

	// At 32 kHz the no-event preview must also match the legacy Spc_Emu path.
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 32000, 2048 ) );
	std::vector<short> got, legacy;
	TASK_CHECK( !preview_read_all( p, 3000, 257, got ) );
	Spc_Emu emu;
	legacy.assign( 3000 * 2, 0 );
	TASK_CHECK( !fixture::setup( emu, spc_file(), 32000 ) );
	TASK_CHECK( !fixture::render( emu, legacy.data(), 3000, 1024 ) );
	TASK_CHECK( sample_mismatches( got.data(), legacy.data(), 3000 ) == 0 );
	return 0;
}

// ---- rollback group ------------------------------------------------------

static int rollback_edit_before_delivery()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 1024 ) );
		TASK_CHECK( !p.schedule_port( 400, 0, 10 ) );
		TASK_CHECK( !p.schedule_port( 2000, 1, 20 ) );
		TASK_CHECK( !p.prefetch() );

		// Insert a brand-new key, then replace another, both inside a region
		// the speculative engine has already crossed. A re-prefetch reopens the
		// speculated range so the second edit also forces a rollback.
		TASK_CHECK( !p.schedule_port( 400, 1, 55 ) );
		TASK_CHECK( p.status().rerenders == 1 );
		TASK_CHECK( !p.prefetch() );
		TASK_CHECK( p.status().speculative_native_frame > 400 );
		TASK_CHECK( !p.schedule_port( 400, 0, 222 ) );
		TASK_CHECK( p.status().rerenders == 2 );
		Spc_Preview::Status s = p.status();
		TASK_CHECK( s.pending_frames == 0 );
		TASK_CHECK( s.speculative_native_frame == s.committed_native_frame );
		TASK_CHECK( !p.prefetch() );

		std::vector<short> got, want;
		std::vector<PortEvent> plan = { { 400, 0, 222 }, { 400, 1, 55 }, { 2000, 1, 20 } };
		TASK_CHECK( !preview_read_all( p, 3000, 257, got ) );
		TASK_CHECK( !offline_render( rate, plan, 3000, 257, want ) );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), 3000 ) == 0 );
	}
	return 0;
}

static int rollback_after_partial_delivery()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		const unsigned la = 2048;
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, la ) );
		TASK_CHECK( !p.schedule_port( 800, 0, 10 ) );
		TASK_CHECK( !p.schedule_port( 5000, 1, 20 ) );
		TASK_CHECK( !p.prefetch() );

		const long total = 4000;
		std::vector<short> got( size_t( total ) * 2, 0 );
		long pos = 0;
		const long first = 300;
		TASK_CHECK( !p.read( got.data(), first ) );
		pos = first;
		TASK_CHECK( !open_window( p, got, pos ) );
		Spc_Preview::Status s = p.status();
		uint64_t target = s.committed_native_frame + 50;
		TASK_CHECK( target < s.speculative_native_frame );

		uint64_t synth_before = p.status().synthesized_native_frames;
		TASK_CHECK( !p.schedule_port( target, 2, 123 ) );
		TASK_CHECK( p.status().rerenders >= 1 );
		TASK_CHECK( p.status().pending_frames == 0 );
		TASK_CHECK( p.status().speculative_native_frame == p.status().committed_native_frame );
		TASK_CHECK( !p.prefetch() );
		// Re-render work is bounded even though the prefix was already delivered.
		uint64_t added = p.status().synthesized_native_frames - synth_before;
		TASK_CHECK( added <= work_cap( la, rate ) );
		// Synthesis accounting never moves backwards on a rollback.
		TASK_CHECK( p.status().synthesized_native_frames >= synth_before );

		for ( ; pos < total; )
		{
			long n = std::min( 7L, total - pos );
			TASK_CHECK( !p.read( got.data() + pos * 2, n ) );
			pos += n;
		}
		std::vector<PortEvent> plan = {
			{ 800, 0, 10 }, { 5000, 1, 20 }, { target, 2, 123 }
		};
		std::vector<short> want;
		TASK_CHECK( !offline_render( rate, plan, total, 257, want ) );
		// The already delivered prefix is unchanged, and the suffix matches too.
		TASK_CHECK( sample_mismatches( got.data(), want.data(), first ) == 0 );
		long suffix = total - 2000;
		TASK_CHECK( sample_mismatches( got.data() + 2000 * 2, want.data() + 2000 * 2,
				suffix ) == 0 );
		TASK_CHECK( sample_mismatches( got.data(), want.data(), total ) == 0 );
	}
	return 0;
}

static int rollback_resampled_long_suffix()
{
	// Thousands of frames at each non-native rate, edited mid-stream.
	for ( unsigned r = 1; r < 3; r++ )
	{
		long rate = rates[r];
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, 2048 ) );
		TASK_CHECK( !p.schedule_port( 1000, 0, 40 ) );
		TASK_CHECK( !p.prefetch() );
		const long total = 5000;
		std::vector<short> got( size_t( total ) * 2, 0 );
		long pos = 0;
		TASK_CHECK( !p.read( got.data(), 1000 ) );
		pos = 1000;
		TASK_CHECK( !open_window( p, got, pos ) );
		uint64_t target = p.status().committed_native_frame + 80;
		TASK_CHECK( target < p.status().speculative_native_frame );
		TASK_CHECK( !p.schedule_port( target, 3, 210 ) );
		TASK_CHECK( !p.prefetch() );
		for ( ; pos < total; )
		{
			long n = std::min( 257L, total - pos );
			TASK_CHECK( !p.read( got.data() + pos * 2, n ) );
			pos += n;
		}
		std::vector<PortEvent> plan = { { 1000, 0, 40 }, { target, 3, 210 } };
		std::vector<short> want;
		TASK_CHECK( !offline_render( rate, plan, total, 257, want ) );
		TASK_CHECK( sample_mismatches( got.data() + 2000 * 2, want.data() + 2000 * 2,
				total - 2000 ) == 0 );
	}
	return 0;
}

static int rollback_long_delivery_work_cap()
{
	for ( unsigned r = 0; r < 3; r++ )
	{
		long rate = rates[r];
		const unsigned la = 2048;
		Spc_Preview p;
		TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), rate, la ) );
		TASK_CHECK( !p.schedule_port( 2000, 0, 30 ) );
		TASK_CHECK( !p.schedule_port( 7000, 1, 40 ) );
		TASK_CHECK( !p.prefetch() );
		std::vector<short> sink( 9000 * 2, 0 );
		long sink_pos = 0;
		// A long delivery run before the edit.
		for ( int k = 0; k < 8; k++ )
			TASK_CHECK( !p.read( sink.data() + sink_pos * 2, 512 ) );
		sink_pos = 8 * 512;
		TASK_CHECK( !open_window( p, sink, sink_pos ) );
		uint64_t target = p.status().committed_native_frame + 100;
		TASK_CHECK( target < p.status().speculative_native_frame );
		uint64_t before = p.status().synthesized_native_frames;
		TASK_CHECK( !p.schedule_port( target, 3, 123 ) );
		TASK_CHECK( !p.prefetch() );
		uint64_t added = p.status().synthesized_native_frames - before;
		TASK_CHECK( added <= work_cap( la, rate ) );
	}
	return 0;
}

// ---- lifecycle group -----------------------------------------------------

static int life_unbegun_and_read_validation()
{
	Spc_Preview p;
	TASK_CHECK( p.prefetch() );
	TASK_CHECK( p.schedule_port( 0, 0, 0 ) );
	std::vector<short> out( 4096 * 2 );
	TASK_CHECK( p.read( out.data(), 1 ) );
	Spc_Preview::Status s = p.status();
	TASK_CHECK( s.delivered_frames == 0 && s.committed_native_frame == 0 );

	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 44100, 1024 ) );
	// Zero frames (even with null out) is a complete no-op.
	TASK_CHECK( !p.read( nullptr, 0 ) );
	TASK_CHECK( p.status().delivered_frames == 0 );
	// Positive frames need a buffer; bad sizes never advance.
	TASK_CHECK( p.read( nullptr, 4 ) );
	TASK_CHECK( p.read( out.data(), 4097 ) );
	TASK_CHECK( p.read( out.data(), -1 ) );
	TASK_CHECK( p.status().delivered_frames == 0 );
	TASK_CHECK( !p.read( out.data(), 4096 ) );
	TASK_CHECK( p.status().delivered_frames == 4096 );
	return 0;
}

static int life_begin_validation_and_preservation()
{
	Spc_Preview p;
	TASK_CHECK( p.begin( spc_file().data(), long( spc_file().size() ), 31999, 1024 ) );
	TASK_CHECK( p.begin( spc_file().data(), long( spc_file().size() ), 44100, 511 ) );
	TASK_CHECK( p.begin( spc_file().data(), long( spc_file().size() ), 44100, 4097 ) );
	TASK_CHECK( p.begin( nullptr, 10, 44100, 1024 ) );

	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 48000, 2048 ) );
	TASK_CHECK( !p.prefetch() );
	Spc_Preview::Status good = p.status();
	// A failing re-begin must leave the existing playback untouched.
	TASK_CHECK( p.begin( spc_file().data(), long( spc_file().size() ), 999, 2048 ) );
	Spc_Preview::Status after = p.status();
	TASK_CHECK( after.pending_frames == good.pending_frames );
	TASK_CHECK( after.synthesized_native_frames == good.synthesized_native_frames );
	return 0;
}

static int life_prefetch_idempotent()
{
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 44100, 2048 ) );
	TASK_CHECK( !p.schedule_port( 0, 0, 43 ) );
	TASK_CHECK( !p.prefetch() );
	Spc_Preview::Status s = p.status();
	TASK_CHECK( s.pending_frames == 2048 );
	TASK_CHECK( !p.prefetch() );
	TASK_CHECK( p.status().synthesized_native_frames == s.synthesized_native_frames );
	TASK_CHECK( p.status().pending_frames == 2048 );
	return 0;
}

static int life_reset()
{
	Spc_Preview p;
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 44100, 1024 ) );
	TASK_CHECK( !p.schedule_port( 10, 0, 5 ) );
	TASK_CHECK( !p.prefetch() );
	std::vector<short> out( 16 * 2 );
	TASK_CHECK( !p.read( out.data(), 16 ) );
	p.reset();
	Spc_Preview::Status z = p.status();
	TASK_CHECK( z.delivered_frames == 0 && z.committed_native_frame == 0 );
	TASK_CHECK( z.speculative_native_frame == 0 && z.synthesized_native_frames == 0 );
	TASK_CHECK( z.pending_frames == 0 && z.queued_events == 0 && z.rerenders == 0 );
	p.reset(); // repeatable
	TASK_CHECK( p.read( out.data(), 1 ) ); // not begun after reset
	// Reusable after reset.
	TASK_CHECK( !p.begin( spc_file().data(), long( spc_file().size() ), 32000, 512 ) );
	TASK_CHECK( !p.prefetch() );
	TASK_CHECK( p.status().pending_frames == 512 );
	return 0;
}

static int life_two_instances_interleave()
{
	Spc_Preview a, b;
	TASK_CHECK( !a.begin( spc_file().data(), long( spc_file().size() ), 44100, 1024 ) );
	TASK_CHECK( !b.begin( spc_file().data(), long( spc_file().size() ), 32000, 1024 ) );
	TASK_CHECK( !a.schedule_port( 50, 1, 99 ) );
	TASK_CHECK( !b.schedule_port( 60, 0, 7 ) );
	TASK_CHECK( !a.prefetch() );
	TASK_CHECK( !b.prefetch() );
	std::vector<short> ao( 64 * 2 ), bo( 32 * 2 );
	TASK_CHECK( !a.read( ao.data(), 64 ) );
	TASK_CHECK( a.status().delivered_frames == 64 );
	TASK_CHECK( b.status().delivered_frames == 0 );
	TASK_CHECK( !b.read( bo.data(), 32 ) );
	TASK_CHECK( b.status().delivered_frames == 32 );
	TASK_CHECK( a.status().delivered_frames == 64 );

	// Each instance matches its own independent synchronous reference.
	std::vector<short> want_a, want_b;
	TASK_CHECK( !offline_render( 44100, { { 50, 1, 99 } }, 64, 64, want_a ) );
	TASK_CHECK( !offline_render( 32000, { { 60, 0, 7 } }, 32, 32, want_b ) );
	TASK_CHECK( sample_mismatches( ao.data(), want_a.data(), 64 ) == 0 );
	TASK_CHECK( sample_mismatches( bo.data(), want_b.data(), 32 ) == 0 );
	return 0;
}

extern const struct task_case task_cases[] = {
	{ "ports", "match_reference",       ports_match_reference },
	{ "ports", "same_frame_dual",       ports_same_frame_dual },
	{ "ports", "replace_and_dedupe",    ports_replace_and_dedupe },
	{ "ports", "future_append",         ports_future_append_keeps_cache },
	{ "ports", "boundary_and_rejects",  ports_commit_boundary_and_rejects },
	{ "ports", "queue_full",            ports_queue_full },
	{ "ports", "read_chunk_sizes",      ports_read_chunk_sizes },
	{ "ports", "no_event_legacy_path",  ports_no_event_legacy_path },

	{ "rollback", "edit_before_delivery",   rollback_edit_before_delivery },
	{ "rollback", "after_partial_delivery", rollback_after_partial_delivery },
	{ "rollback", "resampled_long_suffix",  rollback_resampled_long_suffix },
	{ "rollback", "long_delivery_work_cap", rollback_long_delivery_work_cap },

	{ "lifecycle", "unbegun_and_read_validation", life_unbegun_and_read_validation },
	{ "lifecycle", "begin_validation_preserve",   life_begin_validation_and_preservation },
	{ "lifecycle", "prefetch_idempotent",         life_prefetch_idempotent },
	{ "lifecycle", "reset",                       life_reset },
	{ "lifecycle", "two_instances_interleave",    life_two_instances_interleave },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
