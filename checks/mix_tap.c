/* Mix tap check: verifies the read-only per-channel mixer tap.
 *
 * Groups:
 *  1 stereo    - tone.mod stereo: lane reconstruction of the master mix,
 *                PCM identical with tap on/off, aux lane silent.
 *  2 mono      - tone.mod mono: reconstruction with one channel per lane.
 *  3 state     - xmp_set_mix_tap state errors, disabling the tap.
 *  4 nna       - it_note_delay_nna.it: NNA voices and the residual
 *                anticlick tail stay on the originating tracker channel.
 *  5 notecut   - note_cut_ac.it: note cut anticlick decay attribution.
 *  6 smix      - smix instrument sound lands in the auxiliary lane.
 *  7 playbuf   - xmp_play_buffer with non-tick-sized reads: exactly one
 *                callback per mixed tick, identical PCM.
 */

#include "xmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_LANES	(XMP_MAX_CHANNELS + 1)
#define DOWNMIX_SHIFT	12	/* must match src/mixer.c */
#define DEFAULT_AMPLIFY	1	/* must match src/common.h */

static int nfail;

#define FAIL(...) do { \
	printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); nfail++; \
} while (0)

#define REQUIRE(cond, ...) do { \
	if (!(cond)) FAIL(__VA_ARGS__); \
} while (0)

struct tap {
	int calls;
	int bad;
	int lane_count;
	int frames;
	int channels;
	int32_t *cur;		/* copy of the most recent tick, lane-major */
	size_t cap;
	long long lane_energy[MAX_LANES];
	int aux_nonzero_ticks;
	/* reconstructed 16-bit PCM stream (lane sums after downmix) */
	int16_t *stream;
	size_t stream_len;
	size_t stream_cap;
};

static void tap_cb(void *ud, const int32_t *const *lanes, int lane_count,
		   int frames, int channels)
{
	struct tap *t = (struct tap *)ud;
	int l, i, n;
	size_t need;

	t->calls++;
	if (lanes == NULL || lane_count < 1 || lane_count > MAX_LANES ||
	    frames <= 0 || (channels != 1 && channels != 2)) {
		t->bad++;
		return;
	}
	t->lane_count = lane_count;
	t->frames = frames;
	t->channels = channels;

	n = frames * channels;
	need = (size_t)lane_count * n * sizeof(int32_t);
	if (t->cap < need) {
		int32_t *p = (int32_t *)realloc(t->cur, need);
		if (p == NULL) {
			t->bad++;
			return;
		}
		t->cur = p;
		t->cap = need;
	}

	for (l = 0; l < lane_count; l++) {
		const int32_t *lane = lanes[l];
		if (lane == NULL) {
			t->bad++;
			continue;
		}
		memcpy(t->cur + (size_t)l * n, lane, (size_t)n * sizeof(int32_t));
		for (i = 0; i < n; i++) {
			long long v = lane[i];
			t->lane_energy[l] += v < 0 ? -v : v;
		}
		if (l == lane_count - 1) {
			for (i = 0; i < n; i++) {
				if (lane[i] != 0) {
					t->aux_nonzero_ticks++;
					break;
				}
			}
		}
	}

	/* Rebuild the master mix of this tick from the lanes. */
	need = (size_t)n * sizeof(int16_t);
	if (t->stream_cap < t->stream_len + need) {
		size_t cap = (t->stream_cap + need) * 2;
		int16_t *p = (int16_t *)realloc(t->stream, cap);
		if (p == NULL) {
			t->bad++;
			return;
		}
		t->stream = p;
		t->stream_cap = cap;
	}
	for (i = 0; i < n; i++) {
		uint32_t acc = 0;
		int32_t v;
		int smp;
		for (l = 0; l < lane_count; l++) {
			acc += (uint32_t)t->cur[(size_t)l * n + i];
		}
		v = (int32_t)acc;
		smp = v >> (DOWNMIX_SHIFT - DEFAULT_AMPLIFY);
		if (smp > 32767) smp = 32767;
		else if (smp < -32768) smp = -32768;
		t->stream[t->stream_len / 2 + i] = (int16_t)smp;
	}
	t->stream_len += need;
}

/* Rebuild the master mix from the tapped lanes of the most recent tick
 * and compare it against the 16-bit PCM produced by the same tick. */
static int recon_error(const struct tap *t, const void *buffer, int buffer_size)
{
	const int16_t *pcm = (const int16_t *)buffer;
	int n = buffer_size / 2;
	int l, i;
	int maxerr = 0;

	if (t->cur == NULL || t->frames * t->channels != n)
		return 1 << 30;

	for (i = 0; i < n; i++) {
		uint32_t acc = 0;
		int32_t v;
		int smp, err;
		for (l = 0; l < t->lane_count; l++)
			acc += (uint32_t)t->cur[(size_t)l * n + i];
		v = (int32_t)acc;
		smp = v >> (DOWNMIX_SHIFT - DEFAULT_AMPLIFY);
		if (smp > 32767) smp = 32767;
		else if (smp < -32768) smp = -32768;
		err = smp - pcm[i];
		if (err < 0) err = -err;
		if (err > maxerr) maxerr = err;
	}
	return maxerr;
}

static long long lane_cur_energy(const struct tap *t, int lane)
{
	int n = t->frames * t->channels;
	int i;
	long long e = 0;

	if (t->cur == NULL || lane >= t->lane_count)
		return 0;
	for (i = 0; i < n; i++) {
		long long v = t->cur[(size_t)lane * n + i];
		e += v < 0 ? -v : v;
	}
	return e;
}

static long long pcm_energy(const void *buffer, int buffer_size)
{
	const int16_t *pcm = (const int16_t *)buffer;
	int n = buffer_size / 2;
	int i;
	long long e = 0;

	for (i = 0; i < n; i++) {
		int v = pcm[i];
		e += v < 0 ? -v : v;
	}
	return e;
}

/* Render reference PCM without a tap. */
static unsigned char *render_ref(const char *path, int rate, int format,
				 int frames, size_t *out_len)
{
	xmp_context c = xmp_create_context();
	unsigned char *buf = NULL;
	size_t len = 0;
	int i;

	if (c == NULL || xmp_load_module(c, path) != 0 ||
	    xmp_start_player(c, rate, format) != 0) {
		FAIL("reference render setup failed for %s", path);
		goto end;
	}
	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;
		if (xmp_play_frame(c) != 0)
			break;
		xmp_get_frame_info(c, &fi);
		p = (unsigned char *)realloc(buf, len + fi.buffer_size);
		if (p == NULL) {
			FAIL("out of memory");
			break;
		}
		buf = p;
		memcpy(buf + len, fi.buffer, fi.buffer_size);
		len += fi.buffer_size;
	}
    end:
	if (c != NULL) {
		xmp_end_player(c);
		xmp_release_module(c);
		xmp_free_context(c);
	}
	*out_len = len;
	return buf;
}

static xmp_context start_tapped(const char *path, int rate, int format,
				struct tap *t)
{
	xmp_context c = xmp_create_context();

	if (c == NULL || xmp_load_module(c, path) != 0 ||
	    xmp_set_mix_tap(c, tap_cb, t) != 0 ||
	    xmp_start_player(c, rate, format) != 0) {
		FAIL("tapped player setup failed for %s", path);
		return NULL;
	}
	return c;
}

static void end_context(xmp_context c)
{
	xmp_end_player(c);
	xmp_release_module(c);
	xmp_free_context(c);
}

/* Group 1: stereo reconstruction and PCM identity with the tap off. */
static unsigned char *group_stereo(const unsigned char *ref, size_t ref_len,
				   int frames)
{
	xmp_context c;
	struct tap t;
	unsigned char *pcm = NULL;
	size_t pcm_len = 0;
	long long mod_energy = 0;
	int maxerr = 0;
	int i, l;

	memset(&t, 0, sizeof(t));
	c = start_tapped("checks/tone.mod", 44100, 0, &t);
	if (c == NULL)
		return NULL;

	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;
		int err;

		if (xmp_play_frame(c) != 0) {
			FAIL("stereo: frame %d ended early", i);
			break;
		}
		xmp_get_frame_info(c, &fi);
		REQUIRE(t.lane_count == 5, "stereo: lane_count %d != 5",
			t.lane_count);
		REQUIRE(t.channels == 2, "stereo: channels %d != 2", t.channels);
		err = recon_error(&t, fi.buffer, fi.buffer_size);
		if (err > maxerr)
			maxerr = err;
		p = (unsigned char *)realloc(pcm, pcm_len + fi.buffer_size);
		if (p == NULL) {
			FAIL("stereo: out of memory");
			break;
		}
		pcm = p;
		memcpy(pcm + pcm_len, fi.buffer, fi.buffer_size);
		pcm_len += fi.buffer_size;
	}
	end_context(c);

	for (l = 0; l < 4; l++)
		mod_energy += t.lane_energy[l];

	REQUIRE(t.bad == 0, "stereo: %d callback contract violations", t.bad);
	REQUIRE(t.calls == frames, "stereo: %d callbacks != %d frames",
		t.calls, frames);
	REQUIRE(maxerr == 0, "stereo: reconstruction error %d", maxerr);
	REQUIRE(pcm_len == ref_len && memcmp(pcm, ref, ref_len) == 0,
		"stereo: PCM differs between tap on/off");
	REQUIRE(mod_energy > 0, "stereo: module lanes are silent");
	REQUIRE(t.lane_energy[4] == 0 && t.aux_nonzero_ticks == 0,
		"stereo: unexpected sound in auxiliary lane (%lld)",
		t.lane_energy[4]);

	printf("MIX_TAP_GROUP stereo recon_err=%d calls=%d pcm_identical=%d aux_energy=%lld\n",
		maxerr, t.calls, pcm_len == ref_len && memcmp(pcm, ref, ref_len) == 0,
		t.lane_energy[4]);

	free(t.cur);
	free(t.stream);
	return pcm;
}

/* Group 2: mono reconstruction. */
static void group_mono(int frames)
{
	xmp_context c;
	struct tap t;
	int maxerr = 0;
	int i;

	memset(&t, 0, sizeof(t));
	c = start_tapped("checks/tone.mod", 44100, XMP_FORMAT_MONO, &t);
	if (c == NULL)
		return;

	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		int err;

		if (xmp_play_frame(c) != 0) {
			FAIL("mono: frame %d ended early", i);
			break;
		}
		xmp_get_frame_info(c, &fi);
		REQUIRE(t.channels == 1, "mono: channels %d != 1", t.channels);
		err = recon_error(&t, fi.buffer, fi.buffer_size);
		if (err > maxerr)
			maxerr = err;
	}
	end_context(c);

	REQUIRE(t.bad == 0, "mono: %d callback contract violations", t.bad);
	REQUIRE(t.calls == frames, "mono: %d callbacks != %d frames",
		t.calls, frames);
	REQUIRE(maxerr == 0, "mono: reconstruction error %d", maxerr);
	REQUIRE(t.lane_energy[4] == 0, "mono: aux lane not silent");

	printf("MIX_TAP_GROUP mono recon_err=%d calls=%d\n", maxerr, t.calls);

	free(t.cur);
	free(t.stream);
}

/* Group 3: state handling and disabling the tap. */
static void group_state(const unsigned char *ref, size_t ref_len, int frames)
{
	xmp_context c, c2;
	struct tap t;
	unsigned char *pcm = NULL;
	size_t pcm_len = 0;
	int r1, r2, r3, r4, pcm_ok;
	int i;

	memset(&t, 0, sizeof(t));

	/* Setting (and clearing) the tap is allowed while stopped. */
	c2 = xmp_create_context();
	r1 = xmp_set_mix_tap(c2, tap_cb, &t);
	r2 = xmp_set_mix_tap(c2, NULL, NULL);
	xmp_free_context(c2);
	REQUIRE(r1 == 0 && r2 == 0, "state: cannot set tap while stopped");

	c = xmp_create_context();
	xmp_load_module(c, "checks/tone.mod");
	REQUIRE(xmp_set_mix_tap(c, tap_cb, &t) == 0,
		"state: cannot set tap while loaded");
	xmp_start_player(c, 44100, 0);

	r1 = xmp_set_mix_tap(c, tap_cb, NULL);
	r2 = xmp_set_mix_tap(c, NULL, NULL);
	REQUIRE(r1 == -XMP_ERROR_STATE && r2 == -XMP_ERROR_STATE,
		"state: tap change while playing returned %d/%d", r1, r2);

	for (i = 0; i < 4; i++)
		xmp_play_frame(c);
	REQUIRE(t.calls == 4, "state: %d callbacks != 4", t.calls);
	xmp_end_player(c);

	r3 = xmp_set_mix_tap(c, NULL, NULL);
	REQUIRE(r3 == 0, "state: cannot clear tap while stopped");

	/* After disabling, playback must be silent on the tap and the
	 * PCM must match the reference rendered before the tap existed. */
	xmp_start_player(c, 44100, 0);
	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;

		if (xmp_play_frame(c) != 0)
			break;
		xmp_get_frame_info(c, &fi);
		p = (unsigned char *)realloc(pcm, pcm_len + fi.buffer_size);
		if (p == NULL)
			break;
		pcm = p;
		memcpy(pcm + pcm_len, fi.buffer, fi.buffer_size);
		pcm_len += fi.buffer_size;
	}
	r4 = t.calls;
	end_context(c);

	pcm_ok = (pcm_len == ref_len && memcmp(pcm, ref, ref_len) == 0);
	REQUIRE(r4 == 4, "state: tap fired %d times after disable", r4 - 4);
	REQUIRE(pcm_ok, "state: PCM changed after disabling the tap");

	printf("MIX_TAP_GROUP state playing_ret=%d/%d calls_after_disable=%d pcm_identical=%d\n",
		r1, r2, r4 - 4, pcm_ok);

	free(pcm);
	free(t.cur);
	free(t.stream);
}

/* Groups 4/5 helper: attribution check for IT NNA / note cut tails. */
static void group_it(const char *name, const char *path, int format,
		     int frames)
{
	xmp_context c;
	struct tap t;
	struct xmp_module_info mi;
	unsigned char *ref;
	size_t ref_len = 0, pcm_len = 0;
	unsigned char *pcm = NULL;
	int expect_lanes;
	int maxerr = 0;
	int tail_ticks = 0;
	int pcm_ok;
	int i;

	ref = render_ref(path, 8000, format, frames, &ref_len);

	memset(&t, 0, sizeof(t));
	c = xmp_create_context();
	if (c == NULL || xmp_load_module(c, path) != 0) {
		FAIL("%s: load failed", name);
		free(ref);
		return;
	}
	xmp_get_module_info(c, &mi);
	expect_lanes = mi.mod->chn + 1;

	if (xmp_set_mix_tap(c, tap_cb, &t) != 0 ||
	    xmp_start_player(c, 8000, format) != 0) {
		FAIL("%s: start failed", name);
		free(ref);
		return;
	}

	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;
		int err;

		if (xmp_play_frame(c) != 0) {
			FAIL("%s: frame %d ended early", name, i);
			break;
		}
		xmp_get_frame_info(c, &fi);
		REQUIRE(t.lane_count == expect_lanes,
			"%s: lane_count %d != %d", name, t.lane_count,
			expect_lanes);
		err = recon_error(&t, fi.buffer, fi.buffer_size);
		if (err > maxerr)
			maxerr = err;

		/* Ticks with sound but no active voices are residual
		 * anticlick tails: they must be attributed to the
		 * originating tracker channel (lane 0 here), never to
		 * the auxiliary lane. */
		if (fi.virt_used == 0 && pcm_energy(fi.buffer, fi.buffer_size) > 0) {
			tail_ticks++;
			REQUIRE(lane_cur_energy(&t, 0) > 0,
				"%s: tail tick %d lost from channel lane", name, i);
			REQUIRE(lane_cur_energy(&t, expect_lanes - 1) == 0,
				"%s: tail tick %d leaked into aux lane", name, i);
		}

		p = (unsigned char *)realloc(pcm, pcm_len + fi.buffer_size);
		if (p == NULL) {
			FAIL("%s: out of memory", name);
			break;
		}
		pcm = p;
		memcpy(pcm + pcm_len, fi.buffer, fi.buffer_size);
		pcm_len += fi.buffer_size;
	}
	end_context(c);

	pcm_ok = (pcm_len == ref_len && memcmp(pcm, ref, ref_len) == 0);

	REQUIRE(t.bad == 0, "%s: %d callback contract violations", name, t.bad);
	REQUIRE(t.calls == frames, "%s: %d callbacks != %d frames",
		name, t.calls, frames);
	REQUIRE(maxerr == 0, "%s: reconstruction error %d", name, maxerr);
	REQUIRE(pcm_ok, "%s: PCM differs between tap on/off", name);
	REQUIRE(t.lane_energy[expect_lanes - 1] == 0 &&
		t.aux_nonzero_ticks == 0,
		"%s: NNA/cut sound leaked into aux lane (%lld)",
		name, t.lane_energy[expect_lanes - 1]);
	REQUIRE(t.lane_energy[0] > 0, "%s: channel lane is silent", name);
	REQUIRE(tail_ticks > 0, "%s: no residual tail ticks observed", name);

	printf("MIX_TAP_GROUP %s recon_err=%d calls=%d tail_ticks=%d aux_energy=%lld pcm_identical=%d\n",
		name, maxerr, t.calls, tail_ticks,
		t.lane_energy[expect_lanes - 1], pcm_ok);

	free(ref);
	free(pcm);
	free(t.cur);
	free(t.stream);
}

/* Group 6: smix instrument sound goes to the auxiliary lane. */
static void group_smix(int frames)
{
	xmp_context c;
	struct tap t;
	unsigned char *pcm = NULL, *ref = NULL;
	size_t pcm_len = 0, ref_len = 0;
	int maxerr = 0;
	int pcm_ok;
	int i, j;

	/* Reference: same smix setup without a tap. */
	c = xmp_create_context();
	if (c == NULL || xmp_load_module(c, "checks/tone.mod") != 0 ||
	    xmp_start_smix(c, 1, 0) != 0 ||
	    xmp_start_player(c, 44100, 0) != 0 ||
	    xmp_smix_play_instrument(c, 0, 60, 64, 0) != 0) {
		FAIL("smix: reference setup failed");
		return;
	}
	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;

		if (xmp_play_frame(c) != 0)
			break;
		xmp_get_frame_info(c, &fi);
		p = (unsigned char *)realloc(ref, ref_len + fi.buffer_size);
		if (p == NULL)
			break;
		ref = p;
		memcpy(ref + ref_len, fi.buffer, fi.buffer_size);
		ref_len += fi.buffer_size;
	}
	xmp_end_smix(c);
	end_context(c);

	memset(&t, 0, sizeof(t));
	c = xmp_create_context();
	if (c == NULL || xmp_load_module(c, "checks/tone.mod") != 0 ||
	    xmp_start_smix(c, 1, 0) != 0 ||
	    xmp_set_mix_tap(c, tap_cb, &t) != 0 ||
	    xmp_start_player(c, 44100, 0) != 0 ||
	    xmp_smix_play_instrument(c, 0, 60, 64, 0) != 0) {
		FAIL("smix: setup failed");
		free(ref);
		return;
	}

	for (i = 0; i < frames; i++) {
		struct xmp_frame_info fi;
		unsigned char *p;
		int err;

		if (xmp_play_frame(c) != 0) {
			FAIL("smix: frame %d ended early", i);
			break;
		}
		xmp_get_frame_info(c, &fi);
		REQUIRE(t.lane_count == 5, "smix: lane_count %d != 5",
			t.lane_count);
		err = recon_error(&t, fi.buffer, fi.buffer_size);
		if (err > maxerr)
			maxerr = err;
		p = (unsigned char *)realloc(pcm, pcm_len + fi.buffer_size);
		if (p == NULL) {
			FAIL("smix: out of memory");
			break;
		}
		pcm = p;
		memcpy(pcm + pcm_len, fi.buffer, fi.buffer_size);
		pcm_len += fi.buffer_size;
	}
	xmp_end_smix(c);
	end_context(c);

	pcm_ok = (pcm_len == ref_len && memcmp(pcm, ref, ref_len) == 0);

	REQUIRE(t.bad == 0, "smix: %d callback contract violations", t.bad);
	REQUIRE(t.calls == frames, "smix: %d callbacks != %d frames",
		t.calls, frames);
	REQUIRE(maxerr == 0, "smix: reconstruction error %d", maxerr);
	REQUIRE(pcm_ok, "smix: PCM differs between tap on/off");
	REQUIRE(t.lane_energy[4] > 0,
		"smix: auxiliary lane did not receive smix sound");
	for (j = 0; j < 4; j++) {
		/* Module lanes must still carry the module's own sound. */
		if (t.lane_energy[j] > 0)
			break;
	}
	REQUIRE(j < 4, "smix: module lanes are silent");

	printf("MIX_TAP_GROUP smix recon_err=%d calls=%d aux_energy=%lld pcm_identical=%d\n",
		maxerr, t.calls, t.lane_energy[4], pcm_ok);

	free(ref);
	free(pcm);
	free(t.cur);
	free(t.stream);
}

/* Group 7: xmp_play_buffer with non-tick-sized reads. */
static void group_playbuf(const unsigned char *ref, size_t ref_len)
{
	xmp_context c;
	struct tap t;
	unsigned char *out = NULL;
	size_t out_len = 0;
	int pcm_ok, stream_ok;
	int iter;

	memset(&t, 0, sizeof(t));
	c = start_tapped("checks/tone.mod", 44100, 0, &t);
	if (c == NULL)
		return;

	/* 10000 bytes is not a multiple of the tick buffer size. */
	out = (unsigned char *)malloc(ref_len);
	if (out == NULL) {
		FAIL("playbuf: out of memory");
		end_context(c);
		return;
	}
	for (iter = 0; out_len < ref_len && iter < 1000; iter++) {
		unsigned char chunk[10000];
		size_t n = ref_len - out_len;
		int r;

		if (n > sizeof(chunk))
			n = sizeof(chunk);
		r = xmp_play_buffer(c, chunk, (int)n, 0);
		if (r != 0) {
			FAIL("playbuf: xmp_play_buffer returned %d", r);
			break;
		}
		memcpy(out + out_len, chunk, n);
		out_len += n;
	}
	end_context(c);

	pcm_ok = (out_len == ref_len && memcmp(out, ref, ref_len) == 0);
	stream_ok = (t.stream_len == ref_len &&
		     memcmp(t.stream, ref, ref_len) == 0);

	REQUIRE(t.bad == 0, "playbuf: %d callback contract violations", t.bad);
	REQUIRE(out_len == ref_len, "playbuf: short read %lu != %lu",
		(unsigned long)out_len, (unsigned long)ref_len);
	REQUIRE(pcm_ok, "playbuf: PCM differs from play_frame reference");
	REQUIRE(t.calls == 48, "playbuf: %d callbacks != 48 ticks", t.calls);
	REQUIRE(stream_ok, "playbuf: lane reconstruction mismatch");

	printf("MIX_TAP_GROUP playbuf calls=%d pcm_identical=%d recon_identical=%d\n",
		t.calls, pcm_ok, stream_ok);

	free(out);
	free(t.cur);
	free(t.stream);
}

int main(void)
{
	unsigned char *ref;
	size_t ref_len = 0;

	/* Reference: 48 stereo frames of tone.mod rendered without a tap. */
	ref = render_ref("checks/tone.mod", 44100, 0, 48, &ref_len);
	if (ref_len == 0) {
		FAIL("cannot render reference PCM");
	} else {
		group_stereo(ref, ref_len, 48);
		group_state(ref, ref_len, 48);
		group_playbuf(ref, ref_len);
	}
	group_mono(24);
	group_it("nna", "test-dev/data/it_note_delay_nna.it", 0, 32);
	group_it("notecut", "test-dev/data/note_cut_ac.it", XMP_FORMAT_MONO, 17);
	group_smix(24);

	free(ref);

	if (nfail != 0) {
		printf("MIX_TAP_FAILED failures=%d\n", nfail);
		return 1;
	}
	printf("MIX_TAP_OK\n");
	return 0;
}
