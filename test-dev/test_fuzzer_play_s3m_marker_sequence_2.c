#include "test.h"

/* A side effect of rescanning modules when the player mode changes is that
 * switching between a mode that does/doesn't support markers to one that
 * doesn't/does can change the number of sequences. If the current sequence
 * happened to be one that no longer exists after the rescan, out-of-bounds
 * reads could occur, so now libxmp_scan_sequences fixes p->sequence.
 */

TEST(test_fuzzer_play_s3m_marker_sequence_2)
{
	static const struct playback_sequence sequence[] =
	{
		{ PLAY_FRAMES,		1, 0 },
		{ PLAY_SET_POSITION,	2, 2 }, /* sequence 1 */
		{ PLAY_FRAMES,		1, 0 },
		{ PLAY_SET_POSITION,	4, 4 }, /* sequence 2 */
		{ PLAY_FRAMES,		1, 0 },
		{ PLAY_SET_POSITION,	6, 6 }, /* sequence 3 */
		{ PLAY_FRAMES,		1, 0 },
		{ PLAY_SET_PLAYER_MODE,	XMP_MODE_MOD, 0 }, /* rescan */
		{ PLAY_FRAMES,		4, 0 }, /* should be sequence 0 now */
		{ PLAY_END,		0, 0 }
	};
	compare_playback("data/f/play_s3m_marker_sequence_2.s3m", sequence, 4000, 0, 0);
}
END_TEST
