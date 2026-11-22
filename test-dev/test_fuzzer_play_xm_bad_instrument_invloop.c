#include "test.h"

/* This test relies on several fringe behaviors that might not be permanent:
 * - Loading XMs does not filter out unsupported extended (Exx) effects, in
 *   this case, EFx (invert loop).
 * - Loading XMs does not filter bad loop parameters when there is no sample
 *   data, as these samples will never be mixed.
 * - When the player is set to Protracker 2 mode, EFx effects are interpreted
 *   as invert loop and can be used on junk samples attached to valid
 *   instruments via Protracker 2 instrument changes.
 *
 * The invert loop handler correctly filtered NULL samples, but did not
 * avoid signed integer overflow from bad loop parameters.
 */

TEST(test_fuzzer_play_xm_bad_instrument_invloop)
{
	static const struct playback_sequence sequence[] =
	{
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_PROTRACKER, 0 },
		{ PLAY_FRAMES,		4, 0 },
		{ PLAY_END,		0, 0 }
	};
	compare_playback("data/f/play_xm_bad_instrument_invloop.xm", sequence, 4000, 0, 0);
}
END_TEST
