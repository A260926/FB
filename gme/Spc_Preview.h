// Speculative SPC preview with editable timed CPU input ports.

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#ifndef SPC_PREVIEW_H
#define SPC_PREVIEW_H

#include "blargg_common.h"

#include <stdint.h>
#include <vector>

struct Snes_Spc;
struct SPC_Filter;
template<int> class Fir_Resampler;

// Renders a short, editable preview of an SPC file. Port writes scheduled with
// schedule_port() reach the emulated SPC700 through Snes_Spc::write_port() at
// 32 kHz native frame boundaries; they are never mixed into the PCM or DSP
// registers directly.
//
// Already delivered audio is immutable. A change that falls inside the region
// the speculative engine has already rendered discards the pending PCM and
// rebuilds it from the last committed engine site; changes entirely in the
// future leave the rendered prefix alone. Output is sample-identical to a
// synchronous render that knew every accepted event ahead of time.
//
// All calls must be serialized; there is no internal locking.
class Spc_Preview {
public:
	struct Status {
		// Stereo frames handed to read()
		uint64_t delivered_frames;
		// Native 32 kHz frame the committed engine has actually advanced to,
		// including FIR batch look-ahead already consumed. It is not derived
		// from delivered_frames and never moves during speculative synthesis.
		uint64_t committed_native_frame;
		// Native 32 kHz frame the speculative engine has advanced to
		uint64_t speculative_native_frame;
		// Native frames ever fed to Snes_Spc::play() (positive counts only),
		// re-renders and replays included; never decreases
		uint64_t synthesized_native_frames;
		// Number of times pending audio was discarded and rebuilt
		uint64_t rerenders;
		// Undelivered stereo frames currently held in the pending buffer
		unsigned pending_frames;
		// Accepted port events not yet consumed by the committed engine
		unsigned queued_events;
	};

	Spc_Preview();
	~Spc_Preview();

	// Loads a complete SPC image (copied internally): track 0, tempo 1.0,
	// default gain, accurate filtering, silence checking disabled, no track
	// length fade. rate must be 32000, 44100 or 48000; lookahead is the number
	// of output frames prefetch() keeps ready and must be 512..4096.
	// On failure the previous playback state is left untouched.
	blargg_err_t begin( const void* data, long size, long rate, unsigned lookahead );

	// Queues a write of value (0..255) to CPU input port 0..3, performed just
	// before native 32 kHz frame native_frame is generated. Only the last value
	// per (frame, port) is kept; re-scheduling the same value is a no-op.
	// Frames must be at or beyond the committed boundary and below 2^30. At most
	// 256 uncommitted keys are held. Invalid arguments, stale frames and a full
	// queue fail without changing any state.
	blargg_err_t schedule_port( uint64_t native_frame, unsigned port, unsigned value );

	// Fills the pending buffer up to the lookahead size using the speculative
	// engine. Never changes delivered counts or the committed boundary; a second
	// call with the buffer already full and no new events synthesizes nothing.
	blargg_err_t prefetch();

	// Delivers frames stereo frames. 1..4096 frames are allowed; frames == 0
	// does nothing (out may be null). Negative or oversized requests and null
	// out with positive frames fail without advancing anything.
	blargg_err_t read( short* out, long frames );

	Status status() const;

	// Clears every piece of state; safe to call repeatedly and before begin().
	void reset();

	Spc_Preview( const Spc_Preview& ) = delete;
	Spc_Preview& operator=( const Spc_Preview& ) = delete;

private:
	struct Event;
	struct Engine;

	void save_site( std::vector<unsigned char>& blob, std::vector<short>& fir ) const;
	void restore_site( const std::vector<unsigned char>& blob,
			const std::vector<short>& fir );
	void age_events();
	void discard_pending();
	blargg_err_t render_to( unsigned target_frames );
	blargg_err_t pump( long frames, size_t& event_index, uint64_t native_pos,
			short* out );
	blargg_err_t advance_committed( long frames );

	std::vector<unsigned char> file_;
	std::vector<unsigned char> commit_blob_;   // committed engine site
	std::vector<unsigned char> temp_blob_;     // speculative/backup scratch site
	std::vector<short> commit_fir_;            // committed FIR input buffer contents
	std::vector<short> temp_fir_;              // speculative FIR input buffer contents
	std::vector<Event> events_;
	std::vector<short> pending_;               // undelivered output PCM
	std::vector<short> work_;                  // discarded replay output

	long rate_;
	unsigned lookahead_;
	uint64_t delivered_;
	uint64_t committed_native_;
	uint64_t speculative_native_;
	uint64_t synthesized_;
	uint64_t rerenders_;
	unsigned pending_frames_;
	bool begun_;

	// The live engine has a stable address and owns long-lived allocations
	// (DSP RAM lives inside it; the FIR input buffer is heap backed). Snapshots
	// are always restored back into this same object, so every pointer stored in
	// the byte image remains valid after restore.
	Engine* live_;
};

#endif
