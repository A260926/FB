#include "test.h"

/* MDL modules without samples but with instruments can exist and are
 * valid. Modules like this have found bugs in the FT2 and MOD player
 * routines, so try playing a few frames with different players.
 */

TEST(test_fuzzer_play_mdl_zero_samples)
{
	static const struct playback_sequence sequence[] =
	{
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_FT2, 0 },
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_PROTRACKER, 0 },
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_ST3, 0 },
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_IT, 0 },
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_ITSMP, 0 },
		{ PLAY_FRAMES,		8, 0 },
		{ PLAY_END,		0, 0 }
	};
	compare_playback("data/f/play_mdl_zero_samples.mdl", sequence, 4000, 0, 0);
}
END_TEST
