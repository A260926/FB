/*
 * tests/envelope_model.c
 *
 * Independent envelope node/clock model for libxmp IT and FastTracker 2 (XM)
 * volume envelopes, cross-checked against the real synthesis chain built from
 * test-dev/simple_module.c and driven only through the public player entry
 * points (xmp_start_player / xmp_play_frame / xmp_play_buffer).
 *
 * The model below is a standalone re-derivation of the tracker envelope rules.
 * It never calls libxmp's static get_envelope()/update_envelope(), and its
 * expectations are not recorded from production output. Engine state (the
 * envelope index in struct channel_data and the final channel volume) is read
 * only after a tick has been synthesized, purely to compare against the model;
 * it is never fed back into the model, whose next position is decided solely
 * from its own node/flag state and the scheduled key-off tick.
 *
 * Rule sources
 * ------------
 *  - Impulse Tracker (IT): ITTECH.TXT/S3MTECH.TXT and the OpenMPT reference
 *    (soundlib IT envelope handling). A held note stays inside the sustain
 *    loop; releasing on the frame that would leave the sustain-loop end runs
 *    one more jump back to the sustain start before the envelope continues.
 *    Ordinary envelope loops wrap when the position is strictly past the loop
 *    end (x > loop_end).
 *  - FastTracker 2 (XM): FT2.DOC and the OpenMPT FT2 reference. A lone sustain
 *    point clamps the position while held; ordinary loops wrap only when the
 *    position is exactly at the loop end (x == loop_end) and are still applied
 *    after the sustain clamp, so a sustain point coincident with a loop end
 *    keeps cycling back to the loop start while the key is held. Releasing on
 *    that coincident point lets the envelope leave the loop.
 *
 * Intentionally not covered (format differences out of scope here)
 * -----------------------------------------------------------------
 *  - FT2's one-tick deferred release when a key-off lands exactly on a lone
 *    sustain point that has no envelope loop (the NOTE_SUSEXIT deferral).
 *  - Instrument fadeout (rls) interaction: every instrument uses fadeout 0,
 *    so the envelope alone determines loudness.
 *  - Pan/pitch envelopes, envelope carry (XMP_ENVELOPE_CARRY), bidirectional
 *    sample loops and NNA virtual-channel behavior.
 *  - The automatic fade at the end of an IT envelope and the generic,
 *    non-FT2/non-IT update_envelope_generic personality.
 *
 * Interpolation rule under test: integer division truncates toward zero and is
 * applied to the full numerator, i.e. y1 + (y2-y1)*(x-x1)/(x2-x1); the slope
 * is never truncated before it is multiplied by the distance.
 *
 * Tolerance split: envelope positions and integer values are compared with
 * zero tolerance in the interpolation/release groups, so a volume error of a
 * single step (one 档) or a prematurely truncated slope cannot be swallowed.
 * Only real PCM amplitudes (audio group) use a documented relative tolerance
 * to absorb fixed-point quantization and the anticlick ramp.
 */

#include "../test-dev/test.h"
#undef TEST
#include "../src/player.h"

#include "../task_support/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Independent envelope model                                          */
/* ------------------------------------------------------------------ */

#define EM_PERSONALITY_IT 0
#define EM_PERSONALITY_FT2 1
#define EM_INVALID_POS (-1)
#define EM_DEFAULT_VOL 64
#define EM_MAX_NODES 8

struct em_node {
	int x;
	int y;
};

struct em_env {
	int on;				/* XMP_ENVELOPE_ON */
	int npt;			/* number of nodes */
	struct em_node node[EM_MAX_NODES];
	int has_loop;			/* ordinary envelope loop */
	int has_sus;			/* sustain point/loop present */
	int has_sloop;			/* sustain is a loop, not a lone point */
	int lps, lpe;			/* ordinary loop node indices */
	int sus, sue;			/* sustain start/end node indices */
};

struct emodel {
	const struct em_env *env;
	int personality;
	int pos;			/* current envelope x, invalid before t1 */
	int release;			/* envelope release latched */
};

/* Evaluate the envelope value at position x with truncation-toward-zero
 * integer interpolation on the complete numerator (no slope pre-truncation). */
static int em_eval(const struct em_env *e, int x)
{
	int i, dy, dx;

	if (!e->on || e->npt <= 0)
		return EM_DEFAULT_VOL;	/* no envelope: default full volume */
	if (x < 0)
		return EM_DEFAULT_VOL;	/* invalid before the first sound */
	if (e->npt == 1 || x >= e->node[e->npt - 1].x)
		return e->node[e->npt - 1].y;	/* hold the last node value */

	for (i = 0; i < e->npt - 1; i++) {
		if (x >= e->node[i].x && x < e->node[i + 1].x)
			break;
	}
	if (i == e->npt - 1)
		return e->node[e->npt - 1].y;

	dy = e->node[i + 1].y - e->node[i].y;
	dx = e->node[i + 1].x - e->node[i].x;
	/* C99 integer division truncates toward zero. */
	return e->node[i].y + dy * (x - e->node[i].x) / dx;
}

/* Advance the model by one synthesis tick. key_off is nonzero only on the
 * frame the key-off is injected (the engine clears KEY_OFF every frame);
 * do_release latches envelope release for this and all following ticks. */
static void em_step(struct emodel *m, int key_off, int do_release)
{
	const struct em_env *e = m->env;

	if (do_release)
		m->release = 1;

	if (m->pos == EM_INVALID_POS)
		m->pos = 0;		/* first synth tick evaluates pos 0 */
	else
		m->pos++;

	if (!e->on || e->npt <= 0)
		return;			/* raw clock keeps running, value=def */

	if (m->personality == EM_PERSONALITY_IT) {
		int sus_x = e->node[e->sus].x;
		int sue_x = e->node[e->sue].x;

		/* Releasing exactly as we leave the sustain loop: perform one
		 * more lap back to the sustain start. */
		if (e->has_sus && key_off && m->pos == sue_x + 1) {
			m->pos = sus_x;
		} else if (e->has_sus && !m->release) {
			/* Held: remain inside the sustain loop / on the point. */
			if (m->pos == sue_x + 1)
				m->pos = sus_x;
		} else if (e->has_loop && m->pos > e->node[e->lpe].x) {
			m->pos = e->node[e->lps].x;	/* strict '>' test */
		}
	} else {
		int sus_x = e->node[e->sus].x;

		/* Lone sustain point clamps while held. */
		if (e->has_sus && !m->release && m->pos >= sus_x)
			m->pos = sus_x;

		/* Ordinary loop is applied after the clamp and only exactly at
		 * the loop end; a release on a coincident sustain point
		 * escapes. */
		if (e->has_loop && m->pos == e->node[e->lpe].x &&
		    !(m->release && e->has_sus &&
		      e->sus == e->lpe && e->sue == e->lpe))
			m->pos = e->node[e->lps].x;
	}
}

static void em_init(struct emodel *m, const struct em_env *e, int personality)
{
	m->env = e;
	m->personality = personality;
	m->pos = EM_INVALID_POS;
	m->release = 0;
}

/* ------------------------------------------------------------------ */
/* Deterministic envelope material (shared geometry)                   */
/* ------------------------------------------------------------------ */

/* Five nodes: x strictly increasing, y in [0,64]. Every segment has a
 * non-integer slope, both positive and negative:
 *   (0,64)->(4,10)  -13.5    (4,10)->(7,48)  +38/3
 *   (7,48)->(10,20) -28/3    (10,20)->(13,64) +44/3.  */
static const struct em_node base_nodes[5] = {
	{ 0, 64 }, { 4, 10 }, { 7, 48 }, { 10, 20 }, { 13, 64 }
};

static struct em_env make_base(int variant)
{
	struct em_env e;
	memset(&e, 0, sizeof(e));
	e.on = 1;
	e.npt = 5;
	memcpy(e.node, base_nodes, sizeof(base_nodes));
	e.sus = e.sue = e.lps = e.lpe = 0;

	switch (variant) {
	case 1:				/* ordinary loop, nodes 1..3 */
		e.has_loop = 1;
		e.lps = 1;
		e.lpe = 3;
		break;
	case 2:				/* lone sustain point at node 1 */
		e.has_sus = 1;
		e.sus = e.sue = 1;
		break;
	case 3:				/* IT multi-point sustain loop 1..2 */
		e.has_sus = 1;
		e.has_sloop = 1;
		e.sus = 1;
		e.sue = 2;
		break;
	case 4:				/* sustain point coincident w/ loop end */
		e.has_loop = 1;
		e.lps = 1;
		e.lpe = 3;
		e.has_sus = 1;
		e.sus = e.sue = 3;
		break;
	case 5:				/* sustain point and loop separated */
		e.has_loop = 1;
		e.lps = 2;
		e.lpe = 3;
		e.has_sus = 1;
		e.sus = e.sue = 1;
		break;
	default:			/* 0: no loop, no sustain */
		break;
	}
	return e;
}

/* ------------------------------------------------------------------ */
/* Engine harness: realize the same material in a real libxmp context   */
/* ------------------------------------------------------------------ */

struct eng {
	xmp_context ctx;
};

static int eng_build(struct eng *g, const struct em_env *e, int personality,
		     int with_sample)
{
	struct context_data *ctx;
	struct xmp_envelope *aei;
	int i;

	memset(g, 0, sizeof(*g));
	g->ctx = xmp_create_context();
	if (!g->ctx)
		return -1;
	ctx = (struct context_data *)g->ctx;

	create_simple_module(ctx, 1, 4);

	aei = &ctx->m.mod.xxi[0].aei;
	memset(aei->data, 0, sizeof(aei->data));
	aei->flg = 0;
	aei->npt = e->npt;
	for (i = 0; i < e->npt; i++) {
		aei->data[i * 2] = (short)e->node[i].x;
		aei->data[i * 2 + 1] = (short)e->node[i].y;
	}
	if (e->on)
		aei->flg |= XMP_ENVELOPE_ON;
	if (e->has_loop) {
		aei->flg |= XMP_ENVELOPE_LOOP;
		aei->lps = e->lps;
		aei->lpe = e->lpe;
	}
	if (e->has_sus) {
		aei->flg |= XMP_ENVELOPE_SUS;
		aei->sus = e->sus;
		aei->sue = e->sue;
		if (e->has_sloop)
			aei->flg |= XMP_ENVELOPE_SLOOP;
	}

	set_instrument_fadeout(ctx, 0, 0);	/* fadeout 0: envelope only */
	new_event(ctx, 0, 0, 0, 60, 1, 0, 0, 0, 0, 0);

	if (personality == EM_PERSONALITY_IT)
		set_quirk(ctx, QUIRKS_IT, READ_EVENT_IT);
	else
		set_quirk(ctx, QUIRKS_FT2 | QUIRK_FT2ENV, READ_EVENT_FT2);

	if (with_sample) {
		/* Non-zero, looping 8-bit sample with amplitude headroom. */
		struct xmp_sample *s = &ctx->m.mod.xxs[0];
		int len = 64, k;
		signed char *buf;
		free(s->data - 4);
		buf = (signed char *)calloc(1, (size_t)len + 4);
		if (!buf)
			return -1;
		s->data = (unsigned char *)(buf + 4);
		s->len = len;
		s->lps = 0;
		s->lpe = len;
		s->flg = XMP_SAMPLE_LOOP;
		for (k = 0; k < len; k++)
			buf[4 + k] = (k & 1) ? 40 : -40;	/* +/-40 of 127 */
	}

	if (xmp_start_player(g->ctx, 48000, XMP_FORMAT_MONO) != 0)
		return -1;
	xmp_set_player(g->ctx, XMP_PLAYER_INTERP, XMP_INTERP_NEAREST);
	xmp_set_player(g->ctx, XMP_PLAYER_DSP, 0);	/* no lowpass filter */
	return 0;
}

static void eng_keyoff(struct eng *g)
{
	struct xmp_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.note = XMP_KEY_OFF;
	xmp_inject_event(g->ctx, 0, &ev);
}

static void eng_destroy(struct eng *g)
{
	if (!g->ctx)
		return;
	xmp_end_player(g->ctx);
	xmp_release_module(g->ctx);
	xmp_free_context(g->ctx);
	g->ctx = NULL;
}

struct obs {
	int pos;
	int vol;
};

static void eng_observe(struct eng *g, struct obs *o)
{
	struct context_data *ctx = (struct context_data *)g->ctx;
	struct xmp_frame_info fi;
	xmp_get_frame_info(g->ctx, &fi);
	o->pos = ctx->p.xc_data[0].v_idx;
	o->vol = fi.channel_info[0].volume;
}

/* ------------------------------------------------------------------ */
/* Per-scene statistics and CONTENT_MISMATCH reporting                  */
/* ------------------------------------------------------------------ */

struct scene_stats {
	unsigned ticks;		/* engine-vs-model ticks actually compared */
	unsigned releases;	/* key-off events injected */
	unsigned pcm;		/* PCM values/buffers actually compared */
};

static struct scene_stats stats;

static void scene_begin(void)
{
	memset(&stats, 0, sizeof(stats));
}

static void scene_report(const char *group, const char *name)
{
	printf("SCENE %s/%s ticks=%u releases=%u pcm=%u\n", group, name,
	       stats.ticks, stats.releases, stats.pcm);
}

static int cmp_int(const char *what, int tick, int expected, int actual)
{
	if (expected == actual)
		return 1;
	printf("CONTENT_MISMATCH %s tick=%d expected=%d actual=%d\n",
	       what, tick, expected, actual);
	return 0;
}

/* Run the engine and the independent model in lockstep, comparing position
 * and integer value at every synthesized tick. keyoff_tick <= 0 = held. */
static int run_lockstep(const struct em_env *e, int personality, int ticks,
			int keyoff_tick)
{
	struct eng g;
	struct emodel m;
	int t;

	if (eng_build(&g, e, personality, 0) != 0) {
		printf("SETUP_ERROR engine build failed\n");
		return 1;
	}
	em_init(&m, e, personality);

	for (t = 1; t <= ticks; t++) {
		int key_off = (t == keyoff_tick);
		struct obs o;

		if (key_off) {
			eng_keyoff(&g);
			stats.releases++;
		}
		if (xmp_play_frame(g.ctx) != 0) {
			printf("SETUP_ERROR play_frame failed tick=%d\n", t);
			eng_destroy(&g);
			return 1;
		}
		em_step(&m, key_off, key_off);
		eng_observe(&g, &o);
		stats.ticks++;

		TASK_CHECK(cmp_int("envelope position", t, m.pos, o.pos));
		TASK_CHECK(cmp_int("envelope value", t,
				   em_eval(e, m.pos), o.vol));
	}

	eng_destroy(&g);
	return 0;
}

/* ------------------------------------------------------------------ */
/* interpolation group                                                  */
/* ------------------------------------------------------------------ */

static int test_interp_noloop(void)
{
	struct em_env e = make_base(0);
	struct emodel m;
	scene_begin();

	/* Before the first sound the position is invalid; the first synth tick
	 * evaluates position 0, and positions 1..13 then hold the last node. */
	em_init(&m, &e, EM_PERSONALITY_IT);
	TASK_CHECK(m.pos == EM_INVALID_POS);
	em_step(&m, 0, 0);
	TASK_CHECK(cmp_int("first tick evaluates position 0", 1, 0, m.pos));
	TASK_CHECK(em_eval(&e, m.pos) == 64);

	/* Positions 0..13, then the last node value is held. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 0) == 0);
	/* Interior, non-node positions exercise truncated interpolation and
	 * would differ if the slope were truncated first. */
	TASK_CHECK(em_eval(&e, 1) == 51);	/* 64 + (-54)*1/4 */
	TASK_CHECK(em_eval(&e, 3) == 24);	/* 64 + (-54)*3/4, not -13*3 */
	TASK_CHECK(em_eval(&e, 6) == 35);	/* 10 + 38*2/3 */
	TASK_CHECK(em_eval(&e, 12) == 49);	/* 20 + 44*2/3 */
	scene_report("interpolation", "noloop");
	return 0;
}

static int test_interp_normal_loop(void)
{
	struct em_env e = make_base(1);
	scene_begin();
	/* IT wraps strictly past the loop end; FT2 exactly on it: the
	 * personalities diverge by one tick and both are modelled. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 0) == 0);
	scene_report("interpolation", "normal_loop");
	return 0;
}

static int test_interp_single_sustain(void)
{
	struct em_env e = make_base(2);
	scene_begin();
	/* Both personalities clamp on the lone sustain point (node 1, x=4). */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 14, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 14, 0) == 0);
	scene_report("interpolation", "single_sustain");
	return 0;
}

static int test_interp_it_sustain_loop(void)
{
	struct em_env e = make_base(3);
	scene_begin();
	/* IT cycles the multi-point sustain loop (nodes 1..2, x 4..7). */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	/* FT2 has no sustain-loop concept and clamps on the sustain start. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 14, 0) == 0);
	scene_report("interpolation", "it_sustain_loop");
	return 0;
}

static int test_interp_sustain_loop_coincide(void)
{
	struct em_env e = make_base(4);
	struct emodel m;
	scene_begin();
	/* IT holds the sustain point coincident with the loop end (x=10). */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	/* FT2 keeps applying the ordinary loop back to its start (x=4). */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 0) == 0);

	/* Dedicated model-level expectation of the personality split, driven
	 * for a fixed number of ticks (FT2 never holds the loop-end position). */
	em_init(&m, &e, EM_PERSONALITY_IT);
	{
		int k;
		for (k = 0; k < 12; k++)
			em_step(&m, 0, 0);
	}
	TASK_CHECK(cmp_int("IT held coincide stays at sustain end", 0, 10,
			   m.pos));

	em_init(&m, &e, EM_PERSONALITY_FT2);
	{
		int k;
		for (k = 0; k < 11; k++)
			em_step(&m, 0, 0);
	}
	/* Tick 11 reaches the loop end (x=10) and immediately returns to the
	 * ordinary loop start (x=4). */
	TASK_CHECK(cmp_int("FT2 held coincide returns to loop start", 0, 4,
			   m.pos));
	scene_report("interpolation", "sustain_loop_coincide");
	return 0;
}

static int test_interp_sustain_loop_separated(void)
{
	struct em_env e = make_base(5);
	scene_begin();
	/* Sustain point (x=4) precedes the ordinary loop (x 7..10); a held note
	 * is clamped at the sustain point and never reaches the loop. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 0) == 0);
	scene_report("interpolation", "sustain_loop_separated");
	return 0;
}

static int test_interp_degenerate_envelopes(void)
{
	struct em_env e;
	struct emodel m;
	scene_begin();

	/* Disabled envelope: the clock runs, every value is the default 64. */
	e = make_base(0);
	e.on = 0;
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 8, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 8, 0) == 0);

	/* Zero nodes: treated as "no envelope" without any bad table read. */
	memset(&e, 0, sizeof(e));
	e.on = 1;
	e.npt = 0;
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 8, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 8, 0) == 0);
	TASK_CHECK(em_eval(&e, 5) == EM_DEFAULT_VOL);

	/* Single node: constant value, no loop. */
	memset(&e, 0, sizeof(e));
	e.on = 1;
	e.npt = 1;
	e.node[0].x = 0;
	e.node[0].y = 40;
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 8, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 8, 0) == 0);
	TASK_CHECK(em_eval(&e, 7) == 40);

	/* Single-node sustain loop holds position 0. */
	e.has_sus = 1;
	e.has_sloop = 1;
	e.sus = e.sue = 0;
	em_init(&m, &e, EM_PERSONALITY_IT);
	em_step(&m, 0, 0);
	em_step(&m, 0, 0);
	TASK_CHECK(cmp_int("single node sustain holds position", 0, 0, m.pos));
	scene_report("interpolation", "degenerate_envelopes");
	return 0;
}

/* ------------------------------------------------------------------ */
/* release group                                                        */
/* ------------------------------------------------------------------ */

/* IT multi-point sustain loop with key-off at the sustain start, inside the
 * loop, at the end boundary (one extra wrap) and past the end. */
static int test_release_it_sustain_loop(void)
{
	struct em_env e = make_base(3);		/* sustain loop nodes 1..2 */
	struct eng g;
	struct emodel m;
	int t, boundary_wrap_seen = 0, pos_at_13 = -1;

	scene_begin();

	/* Boundary release: key-off on tick 9, the frame that would leave the
	 * sustain-loop end (x=7 -> x=8). IT must wrap back to x=4 once more. */
	TASK_CHECK(eng_build(&g, &e, EM_PERSONALITY_IT, 0) == 0);
	em_init(&m, &e, EM_PERSONALITY_IT);
	for (t = 1; t <= 14; t++) {
		int key_off = (t == 9);
		struct obs o;

		if (key_off) {
			eng_keyoff(&g);
			stats.releases++;
		}
		TASK_CHECK(xmp_play_frame(g.ctx) == 0);
		em_step(&m, key_off, key_off);
		eng_observe(&g, &o);
		stats.ticks++;

		TASK_CHECK(cmp_int("IT boundary release position", t,
				   m.pos, o.pos));
		TASK_CHECK(cmp_int("IT boundary release value", t,
				   em_eval(&e, m.pos), o.vol));
		if (t == 9 && m.pos == 4)
			boundary_wrap_seen = 1;
		if (t == 13)
			pos_at_13 = m.pos;
	}
	TASK_CHECK(boundary_wrap_seen);		/* the extra wrap happened */
	/* After the wrap the envelope ran on through the successor nodes. */
	TASK_CHECK(cmp_int("IT release reaches successor node", 13, 8,
			   pos_at_13));
	TASK_CHECK(cmp_int("IT release continues to next node", 14, 9,
			   m.pos));
	eng_destroy(&g);

	/* Release at sustain start (tick 5), inside (tick 6) and past the end
	 * boundary (tick 10, already on a new held lap). */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 14, 5) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 14, 6) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 14, 10) == 0);
	scene_report("release", "it_sustain_loop_boundary");
	return 0;
}

/* FT2 ordinary loop whose end coincides with the sustain point: releasing on
 * that point is allowed to leave the loop (the opposite of IT's extra wrap). */
static int test_release_ft2_coincident_escape(void)
{
	struct em_env e = make_base(4);		/* loop 1..3, sus node 3 */
	struct eng g;
	struct emodel m;
	int t;

	scene_begin();
	TASK_CHECK(eng_build(&g, &e, EM_PERSONALITY_FT2, 0) == 0);
	em_init(&m, &e, EM_PERSONALITY_FT2);
	/* Key off on tick 11, the frame reaching the coincident end (x=10). */
	for (t = 1; t <= 16; t++) {
		int key_off = (t == 11);
		struct obs o;

		if (key_off) {
			eng_keyoff(&g);
			stats.releases++;
		}
		TASK_CHECK(xmp_play_frame(g.ctx) == 0);
		em_step(&m, key_off, key_off);
		eng_observe(&g, &o);
		stats.ticks++;

		TASK_CHECK(cmp_int("FT2 coincident release position", t,
				   m.pos, o.pos));
		TASK_CHECK(cmp_int("FT2 coincident release value", t,
				   em_eval(&e, m.pos), o.vol));
	}
	/* After release the envelope leaves the loop and runs past its end. */
	TASK_CHECK(m.pos > e.node[e.lpe].x);
	eng_destroy(&g);
	scene_report("release", "ft2_coincident_escape");
	return 0;
}

/* Dedicated held-note expectation separating the two personalities. */
static int test_release_held_personality_split(void)
{
	struct em_env e = make_base(4);
	struct emodel it, ft2;
	int t;

	scene_begin();
	/* Engine-backed held trajectories for both personalities. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 0) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 0) == 0);

	em_init(&it, &e, EM_PERSONALITY_IT);
	em_init(&ft2, &e, EM_PERSONALITY_FT2);
	for (t = 0; t < 14; t++) {
		em_step(&it, 0, 0);
		em_step(&ft2, 0, 0);
	}
	/* IT prefers the sustain interval; FT2 cycles the ordinary loop. */
	TASK_CHECK(cmp_int("IT held stays at sustain end", 0, 10, it.pos));
	TASK_CHECK(ft2.pos >= e.node[e.lps].x && ft2.pos <= e.node[e.lpe].x);
	scene_report("release", "held_personality_split");
	return 0;
}

/* Two complete ordinary-loop laps while held, then key-off; plus the separated
 * sustain-point/loop material, verifying the post-release trajectory. */
static int test_release_two_laps_then_off(void)
{
	struct em_env e = make_base(1);		/* ordinary loop nodes 1..3 */
	struct emodel m;
	int t, wraps = 0;

	scene_begin();
	em_init(&m, &e, EM_PERSONALITY_FT2);
	for (t = 1; t <= 16; t++) {
		em_step(&m, (t == 12), (t == 12));
		if (m.pos == e.node[e.lps].x)
			wraps++;
	}
	TASK_CHECK(wraps >= 2);			/* at least two loop returns */
	/* Ordinary loops keep cycling after release in both personalities. */
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 16, 12) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 16, 12) == 0);

	/* Separated sustain point + loop: after release the ordinary loop
	 * takes over once the envelope leaves the sustain clamp. */
	e = make_base(5);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_IT, 18, 6) == 0);
	TASK_CHECK(run_lockstep(&e, EM_PERSONALITY_FT2, 18, 6) == 0);
	scene_report("release", "two_laps_then_off");
	return 0;
}

/* ------------------------------------------------------------------ */
/* audio group                                                          */
/* ------------------------------------------------------------------ */

#define AUDIO_TICKS 14
#define AUDIO_WIN_LO 300	/* analysis window skips the anticlick ramp */
#define AUDIO_WIN_HI 700	/* and any frame-edge transition */
#define AUDIO_RATIO_TOL 0.06	/* 6% relative; measured noise was < 0.1% */

static long long frame_mean_abs(struct eng *g)
{
	struct xmp_frame_info fi;
	short *p;
	int i, n;
	long long acc = 0;

	xmp_get_frame_info(g->ctx, &fi);
	p = (short *)fi.buffer;
	n = fi.buffer_size / (int)sizeof(short);
	if (AUDIO_WIN_HI > n)
		return -1;
	for (i = AUDIO_WIN_LO; i < AUDIO_WIN_HI; i++)
		acc += abs(p[i]);
	stats.pcm++;
	return acc / (AUDIO_WIN_HI - AUDIO_WIN_LO);
}

static struct em_env make_audio_constant(void)
{
	struct em_env e;
	memset(&e, 0, sizeof(e));
	e.on = 1;
	e.npt = 1;
	e.node[0].x = 0;
	e.node[0].y = 64;		/* independent constant-64 envelope */
	return e;
}

/* Varying envelope (ordinary loop) vs an independently built constant-64
 * envelope: compare envelope volume and real PCM amplitude ratio per tick. */
static int test_audio_pcm_ratio(void)
{
	struct em_env vary = make_base(1);
	struct em_env cnst = make_audio_constant();
	struct eng gv, gc;
	struct emodel m;
	int t;
	long long prev_vary = -1;
	int prev_env = -1;

	scene_begin();
	TASK_CHECK(eng_build(&gv, &vary, EM_PERSONALITY_IT, 1) == 0);
	TASK_CHECK(eng_build(&gc, &cnst, EM_PERSONALITY_IT, 1) == 0);
	em_init(&m, &vary, EM_PERSONALITY_IT);

	for (t = 1; t <= AUDIO_TICKS; t++) {
		long long va, vc;
		int env_val;
		double ratio, expect, rel;

		TASK_CHECK(xmp_play_frame(gv.ctx) == 0);
		TASK_CHECK(xmp_play_frame(gc.ctx) == 0);
		em_step(&m, 0, 0);
		env_val = em_eval(&vary, m.pos);
		stats.ticks += 2;

		va = frame_mean_abs(&gv);
		vc = frame_mean_abs(&gc);
		TASK_CHECK(va > 0 && vc > 0);

		ratio = (double)va / (double)vc;
		expect = (double)env_val / 64.0;
		rel = (ratio - expect) / expect;
		if (rel < 0)
			rel = -rel;
		TASK_CHECK(rel <= AUDIO_RATIO_TOL);

		/* A one-档 envelope change must visibly move the real PCM; the
		 * 6% band must not mask it (smallest step here is > 15%). */
		if (prev_vary > 0 && env_val != prev_env) {
			double change = (double)llabs(va - prev_vary) /
					(double)prev_vary;
			TASK_CHECK(change > 0.05);
		}
		prev_vary = va;
		prev_env = env_val;
	}
	eng_destroy(&gv);
	eng_destroy(&gc);
	scene_report("audio", "pcm_amplitude_ratio");
	return 0;
}

/* Real sound across a sustain-loop wrap and across key-off: the PCM amplitude
 * follows the envelope before and after release and after the loop jumps. */
static int test_audio_loop_and_release_sound(void)
{
	struct em_env vary = make_base(3);	/* IT sustain loop nodes 1..2 */
	struct em_env cnst = make_audio_constant();
	struct eng gv, gc;
	struct emodel m;
	int t, keyoff = 9;

	scene_begin();
	TASK_CHECK(eng_build(&gv, &vary, EM_PERSONALITY_IT, 1) == 0);
	TASK_CHECK(eng_build(&gc, &cnst, EM_PERSONALITY_IT, 1) == 0);
	em_init(&m, &vary, EM_PERSONALITY_IT);

	for (t = 1; t <= AUDIO_TICKS; t++) {
		int key = (t == keyoff);
		long long va, vc;
		double ratio, expect, rel;

		if (key) {
			eng_keyoff(&gv);
			stats.releases++;
		}
		TASK_CHECK(xmp_play_frame(gv.ctx) == 0);
		TASK_CHECK(xmp_play_frame(gc.ctx) == 0);
		em_step(&m, key, key);
		stats.ticks += 2;

		va = frame_mean_abs(&gv);
		vc = frame_mean_abs(&gc);
		TASK_CHECK(va > 0);		/* keeps sounding (fadeout 0) */
		ratio = (double)va / (double)vc;
		expect = (double)em_eval(&vary, m.pos) / 64.0;
		rel = (ratio - expect) / expect;
		if (rel < 0)
			rel = -rel;
		TASK_CHECK(rel <= AUDIO_RATIO_TOL);
	}
	/* The boundary key-off performed the extra sustain wrap and then let
	 * the envelope run beyond the sustain-loop end. */
	TASK_CHECK(m.release);
	TASK_CHECK(m.pos > vary.node[vary.sue].x);
	eng_destroy(&gv);
	eng_destroy(&gc);
	scene_report("audio", "loop_and_release_sound");
	return 0;
}

/* The envelope advances per synthesis tick, not per API call: frame reads and
 * buffer reads using 1/17/257-sample-frame requests (plus a 1-byte sub-frame
 * request) produce a byte-identical prefix. */
static int test_audio_frame_buffer_prefix(void)
{
	struct em_env vary = make_base(1);
	struct eng gf, gb;
	struct xmp_frame_info fi;
	const int nbytes = 8 * 1920;		/* eight mono 16-bit frames */
	/* 1, 17, 257 sample frames (2 bytes each, mono 16-bit), and a
	 * deliberately sub-frame 1-byte read. */
	const int chunk_frames[4] = { 0, 1, 17, 257 };
	const int chunk_bytes[4] = { 1, 1 * 2, 17 * 2, 257 * 2 };
	unsigned char *via_frame, *via_buffer;
	int copied = 0, k;

	scene_begin();
	via_frame = (unsigned char *)malloc((size_t)nbytes);
	via_buffer = (unsigned char *)malloc((size_t)nbytes);
	TASK_CHECK(via_frame && via_buffer);

	TASK_CHECK(eng_build(&gf, &vary, EM_PERSONALITY_IT, 1) == 0);
	while (copied < nbytes) {
		int n;
		TASK_CHECK(xmp_play_frame(gf.ctx) == 0);
		xmp_get_frame_info(gf.ctx, &fi);
		n = fi.buffer_size;
		if (n > nbytes - copied)
			n = nbytes - copied;
		memcpy(via_frame + copied, fi.buffer, (size_t)n);
		copied += n;
	}
	TASK_CHECK(copied == nbytes);

	for (k = 0; k < 4; k++) {
		int got = 0;
		TASK_CHECK(eng_build(&gb, &vary, EM_PERSONALITY_IT, 1) == 0);
		xmp_play_buffer(gb.ctx, NULL, 0, 0);	/* align buffer start */
		while (got < nbytes) {
			int req = chunk_bytes[k];
			int rc;
			if (req > nbytes - got)
				req = nbytes - got;
			rc = xmp_play_buffer(gb.ctx, via_buffer + got, req, 0);
			got += req;
			stats.pcm++;
			if (rc < 0)
				break;
		}
		TASK_CHECK(got == nbytes);
		if (memcmp(via_frame, via_buffer, (size_t)nbytes) != 0) {
			printf("CONTENT_MISMATCH frame/buffer prefix "
			       "frames=%d bytes=%d sample=0 "
			       "expected=frame actual=buffer\n",
			       chunk_frames[k], chunk_bytes[k]);
			TASK_CHECK(0);
		}
		eng_destroy(&gb);
	}

	free(via_frame);
	free(via_buffer);
	eng_destroy(&gf);
	scene_report("audio", "frame_buffer_prefix");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	{ "interpolation", "noloop",                test_interp_noloop },
	{ "interpolation", "normal_loop",           test_interp_normal_loop },
	{ "interpolation", "single_sustain",        test_interp_single_sustain },
	{ "interpolation", "it_sustain_loop",       test_interp_it_sustain_loop },
	{ "interpolation", "sustain_loop_coincide", test_interp_sustain_loop_coincide },
	{ "interpolation", "sustain_loop_separated", test_interp_sustain_loop_separated },
	{ "interpolation", "degenerate_envelopes",  test_interp_degenerate_envelopes },

	{ "release", "it_sustain_loop_boundary", test_release_it_sustain_loop },
	{ "release", "ft2_coincident_escape",    test_release_ft2_coincident_escape },
	{ "release", "held_personality_split",   test_release_held_personality_split },
	{ "release", "two_laps_then_off",        test_release_two_laps_then_off },

	{ "audio", "pcm_amplitude_ratio",      test_audio_pcm_ratio },
	{ "audio", "loop_and_release_sound",   test_audio_loop_and_release_sound },
	{ "audio", "frame_buffer_prefix",      test_audio_frame_buffer_prefix },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
