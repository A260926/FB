/*
 * Independent timing-model regression for libipt packet-level time tracking.
 *
 * The test builds an independent model of the crystal-clock tick domain,
 * TSC anchors, per-CYC quantized fast-counter contributions, covered MTC
 * periods and calibration state.  Sequences are advanced in whole ticks and
 * only then projected onto Intel PT packet fields.  The expected results are
 * derived from that model; they are never computed from pt_time/pt_tcal.
 *
 * Every sequence is encoded with the production encoder (pt_enc_next),
 * consumed by the production packet decoder (pt_pkt_next) and fed into the
 * production pt_time/pt_tcal entry points.  The production results are then
 * compared field by field against the independent model.
 *
 * Only the C standard library is used beyond the libipt production headers.
 */

#include "intel-pt.h"
#include "pt_time.h"
#include "task_support/check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Sequence description                                                */
/* ------------------------------------------------------------------ */

enum gkind {
	GK_PSB,
	GK_PSBEND,
	GK_TSC,		/* a = TSC value                                     */
	GK_TMA,		/* a = CTC payload, b = FC payload                   */
	GK_MTC,		/* a = unbounded MTC period index                    */
	GK_CYC,		/* a = CYC payload                                   */
	GK_CBR		/* a = core:bus ratio                                */
};

struct gstep {
	enum gkind kind;
	uint64_t a, b;
	const char *event;	/* forensic event identifier, or NULL */
};

struct gcfg {
	uint32_t eax, ebx;	/* CPUID 0x15 crystal ratio */
	uint8_t mtc_freq;	/* IA32_RTIT_CTL.MTCFreq */
	uint8_t nom_freq;	/* MSR_PLATFORM_INFO nominal frequency */
};

/* A forensic event record reconstructed from production decoding. */
struct evrec {
	const char *id;
	unsigned pkt_index;
	uint64_t offset;
	int have_tsc;
	uint64_t tsc;
	uint32_t lost_mtc, lost_cyc;
	int determined;		/* independent model: wall time determinable */
	uint64_t anchor;
	unsigned anchor_pkt;
	unsigned cal_src;	/* 0 none, 1 CBR/nominal, 2 MTC, 3 TSC */
};

/* Hard-coded expectations for the hand-computed example events. */
struct handcheck {
	const char *event;
	uint64_t tsc;
	uint32_t lost_mtc, lost_cyc;
	int determined;
};

#define STEP(k_, a_, b_, ev_) ((struct gstep){ (k_), (a_), (b_), (ev_) })
#define PSB()		STEP(GK_PSB, 0, 0, 0)
#define PSBEND()	STEP(GK_PSBEND, 0, 0, 0)
#define TSC(v)		STEP(GK_TSC, (v), 0, 0)
#define TMA(c, f)	STEP(GK_TMA, (c), (f), 0)
#define MTC(p)		STEP(GK_MTC, (p), 0, 0)
#define CYC(v)		STEP(GK_CYC, (v), 0, 0)
#define CBR(r)		STEP(GK_CBR, (r), 0, 0)
#define ETSC(v, e)	STEP(GK_TSC, (v), 0, (e))
#define ECYC(v, e)	STEP(GK_CYC, (v), 0, (e))
#define EMTC(p, e)	STEP(GK_MTC, (p), 0, (e))

/* ------------------------------------------------------------------ */
/* Independent reference model                                         */
/*                                                                     */
/* The model owns an unbounded MTC period counter.  One period holds   */
/* P = 2^mtc_freq crystal ticks.  Packet fields are projections of     */
/* that counter; wraps are handled in the unbounded domain first.      */
/* All fast-counter math uses plain integer division, i.e. the same    */
/* floor rounding the hardware documentation promises, written         */
/* independently from the bit-mask formulas in pt_time.c.             */
/* ------------------------------------------------------------------ */

struct oracle {
	/* pt_time-equivalent state, named after the physical quantities. */
	uint64_t tsc;		/* estimated TSC */
	uint64_t base;		/* TSC accumulated from TSC/MTC anchors */
	uint64_t fc;		/* fast-counter ticks since base */
	uint32_t ctc;		/* last accepted crystal-tick position */
	uint32_t ctc_cyc;	/* crystal position when fc was reset */
	uint32_t lost_mtc, lost_cyc;
	unsigned have_tsc, have_tma, have_mtc, have_cbr;
	uint32_t cbr;

	/* calibration state (pt_tcal equivalent). */
	uint64_t cal_tsc;
	uint64_t cal_cyc_tsc;
	uint64_t cal_cyc_mtc;
	uint32_t cal_ctc;
	unsigned cal_have_mtc;
	uint64_t fcr;		/* fixed-point, 8 fractional bits */
	unsigned have_fcr;
	unsigned cal_src;

	/* bookkeeping not present in production. */
	unsigned in_psb;
	uint64_t anchor;
	unsigned anchor_pkt;
	unsigned pkt_index;
};

static uint32_t o_period_ticks(const struct gcfg *cfg)
{
	return 1u << cfg->mtc_freq;
}

static uint32_t o_ctc_width_mask(const struct gcfg *cfg)
{
	/* W = mtc_freq + 8 bits of CTC are observable. */
	return ((uint32_t) 1 << (cfg->mtc_freq + 8u)) - 1u;
}

/* Distance between two projected crystal positions, correcting at most
 * one observable wrap, exactly as the on-the-wire convention allows.
 */
static uint32_t o_ctc_distance(uint32_t ctc, uint32_t last,
			       const struct gcfg *cfg)
{
	uint32_t span = (uint32_t) 1 << (cfg->mtc_freq + 8u);

	if (ctc < last)
		ctc += span;

	return ctc - last;
}

static void o_init(struct oracle *o)
{
	memset(o, 0, sizeof(*o));
}

static void o_psb(struct oracle *o)
{
	o->in_psb = 1;
}

static void o_psbend(struct oracle *o)
{
	o->in_psb = 0;
}

static void o_tsc(struct oracle *o, uint64_t tsc)
{
	if (o->in_psb) {
		/* Header TSC: it may calibrate against the previously stored
		 * ordinary TSC, unless MTC calibration already took over.
		 */
		uint64_t last = o->cal_tsc;
		uint64_t cyc = o->cal_cyc_tsc;

		o->cal_tsc = tsc;
		o->cal_cyc_tsc = 0;

		if (last && cyc && !o->cal_have_mtc) {
			uint64_t hi = tsc;
			uint64_t delta;

			/* Correct a single 56-bit TSC wrap, independently
			 * expressed via the documented payload width.
			 */
			if (hi < last)
				hi += 1ull << 56;
			delta = hi - last;
			o->fcr = (delta << 8) / cyc;
			o->have_fcr = 1;
			o->cal_src = 3;
		}
	} else {
		/* Ordinary TSC is stored for later header calibration only. */
		o->cal_tsc = tsc;
		o->cal_cyc_tsc = 0;
	}

	o->have_tsc = 1;
	o->have_tma = 0;
	o->have_mtc = 0;
	o->tsc = o->base = o->anchor = tsc;
	o->ctc = 0;
	o->fc = 0;
	o->lost_mtc = 0;
	o->lost_cyc = 0;
	o->anchor_pkt = o->pkt_index;
}

static void o_cal_reset(struct oracle *o)
{
	o->fcr = 0;
	o->have_fcr = 0;
	o->cal_src = 0;
	o->cal_tsc = 0;
	o->cal_cyc_tsc = 0;
	o->cal_cyc_mtc = 0;
	o->cal_ctc = 0;
	o->cal_have_mtc = 0;
}

static void o_cbr(struct oracle *o, const struct gcfg *cfg, uint32_t ratio)
{
	o->have_cbr = 1;
	o->cbr = ratio;

	/* A CBR outside PSB+ signals a frequency change: calibration is
	 * invalidated before the new nominal ratio is installed.
	 */
	if (!o->in_psb)
		o_cal_reset(o);

	if (cfg->nom_freq && ratio) {
		o->fcr = ((uint64_t) cfg->nom_freq << 8) / ratio;
		o->have_fcr = 1;
		if (o->cal_src == 0)
			o->cal_src = 1;
	}
}

static void o_tma(struct oracle *o, const struct gcfg *cfg,
		  uint32_t ctc, uint32_t fc)
{
	uint32_t mask = o_ctc_width_mask(cfg);

	o->have_tma = 1;
	o->base -= fc;
	o->fc += fc;
	if (cfg->mtc_freq + 8u <= 16u)
		o->have_mtc = 1;
	o->ctc = o->ctc_cyc = ctc & mask;
}

static void o_mtc(struct oracle *o, const struct gcfg *cfg, uint64_t period)
{
	uint32_t ticks = o_period_ticks(cfg);
	uint32_t mask = o_ctc_width_mask(cfg);
	uint32_t ctc = (uint32_t) ((period * ticks) & mask);
	uint32_t fc_ticks;

	/* Calibration sees every MTC, even ones the time model ignores. */
	if (!o->cal_have_mtc) {
		o->cal_cyc_mtc = 0;
		o->cal_ctc = ctc;
		o->cal_have_mtc = 1;
	} else if (o->cal_cyc_mtc) {
		/* Distance against the previous calibration CTC first. */
		uint32_t cal_delta = o_ctc_distance(ctc, o->cal_ctc, cfg);
		uint64_t old_cyc = o->cal_cyc_mtc;

		o->cal_cyc_mtc = 0;
		o->cal_ctc = ctc;
		if (cfg->eax && cfg->ebx) {
			fc_ticks =
				((uint64_t) cal_delta * cfg->ebx) / cfg->eax;
			o->fcr = (fc_ticks << 8) / old_cyc;
			o->have_fcr = 1;
			o->cal_src = 2;	/* MTC wins over TSC */
		}
	}
	/* With no cycles since the previous MTC, production keeps the
	 * stored calibration CTC and retries at the next MTC; so do we.
	 */

	/* MTCs between a TSC and its TMA are ignored by the time model. */
	if (o->have_tsc && !o->have_tma)
		return;

	/* Remember the crystal position when the pending FC was cleared. */
	if (o->fc || o->lost_cyc || !o->have_mtc)
		o->ctc_cyc = ctc;

	{
		unsigned prev_have_mtc = o->have_mtc;
		/* Distance against the previous time-model CTC first. */
		uint32_t time_delta =
			o_ctc_distance(ctc, o->ctc, cfg);

		o->have_mtc = 1;
		o->fc = 0;
		o->ctc = ctc;
		o->lost_cyc = 0;	/* MTC recovers CYC losses */

		/* The first MTC without a TMA cannot be placed in time. */
		if (!prev_have_mtc && !o->have_tma)
			return;

		if (!cfg->eax || !cfg->ebx)
			return;

		fc_ticks = ((uint64_t) time_delta * cfg->ebx) / cfg->eax;
		o->base += fc_ticks;
		o->tsc = o->base;
	}
}

static void o_cyc(struct oracle *o, const struct gcfg *cfg, uint64_t cyc)
{
	uint64_t adjusted;

	o->cal_cyc_mtc += cyc;
	o->cal_cyc_tsc += cyc;

	if (!o->have_fcr) {
		/* No ratio: the packet cannot update time. */
		o->lost_cyc += 1;
		return;
	}

	adjusted = cyc;
	if (o->fc == 0 && o->ctc != o->ctc_cyc) {
		uint32_t delta = o_ctc_distance(o->ctc, o->ctc_cyc, cfg);
		uint64_t fc_ticks, old_cyc;

		if (cfg->eax && cfg->ebx) {
			fc_ticks = ((uint64_t) delta * cfg->ebx) / cfg->eax;
			old_cyc = (fc_ticks << 8) / o->fcr;

			/* Saturation: never subtract more than the payload. */
			if (adjusted < old_cyc)
				adjusted = old_cyc;
			adjusted -= old_cyc;
		}
	}

	o->fc += (adjusted * o->fcr) >> 8;
	o->tsc = o->base + o->fc;
}

/* Wall-clock evidence is determinable only while a TSC anchor exists and
 * no packet loss is pending.  Estimated CYC interpolation is never exact
 * wall time, so forensic records additionally require the anchor to be
 * fresh enough (checked by the individual groups via lost counters).
 */
static int o_determined(const struct oracle *o)
{
	return o->have_tsc && !o->lost_mtc && !o->lost_cyc;
}

/* ------------------------------------------------------------------ */
/* Production chain replay                                             */
/* ------------------------------------------------------------------ */

#define TRACE_BUF 1024u

static void timing_mismatch(const char *group, unsigned seqno,
			    unsigned pkt_index, const char *field,
			    uint64_t actual, uint64_t expected,
			    const char *event)
{
	fprintf(stderr,
		"TIMING_MISMATCH group=%s seq=%u pkt=%u field=%s "
		"actual=0x%llx expect=0x%llx event=%s\n",
		group, seqno, pkt_index, field,
		(unsigned long long) actual,
		(unsigned long long) expected,
		event ? event : "-");
	exit(1);
}

static void check_uint(const char *group, unsigned seqno, unsigned pkt,
		       const char *field, uint64_t actual, uint64_t expected,
		       const char *event)
{
	if (actual != expected)
		timing_mismatch(group, seqno, pkt, field, actual, expected,
				event);
}

/* Feed one decoded production packet into both production entry points,
 * mirroring the dispatch order used by pt_event_decoder.c.
 */
static void produce(struct pt_time *time, struct pt_time_cal *tcal,
		    const struct pt_config *cfg, const struct pt_packet *pkt,
		    unsigned in_psb, int *errcode_out)
{
	int errcode = 0;
	uint64_t fcr;
	int fcr_status;

	switch (pkt->type) {
	case ppt_psb:
		errcode = pt_tcal_update_psb(tcal, cfg);
		break;

	case ppt_psbend:
		break;

	case ppt_tsc:
		if (in_psb)
			errcode = pt_tcal_header_tsc(tcal, &pkt->payload.tsc,
						     cfg);
		else
			errcode = pt_tcal_update_tsc(tcal, &pkt->payload.tsc,
						     cfg);
		if (errcode == 0)
			errcode = pt_time_update_tsc(time, &pkt->payload.tsc,
						     cfg);
		break;

	case ppt_cbr:
		if (in_psb)
			errcode = pt_tcal_header_cbr(tcal, &pkt->payload.cbr,
						     cfg);
		else
			errcode = pt_tcal_update_cbr(tcal, &pkt->payload.cbr,
						     cfg);
		if (errcode == 0)
			errcode = pt_time_update_cbr(time, &pkt->payload.cbr,
						     cfg);
		break;

	case ppt_tma:
		errcode = pt_tcal_update_tma(tcal, &pkt->payload.tma, cfg);
		if (errcode == 0)
			errcode = pt_time_update_tma(time, &pkt->payload.tma,
						     cfg);
		break;

	case ppt_mtc:
		errcode = pt_tcal_update_mtc(tcal, &pkt->payload.mtc, cfg);
		if (errcode == 0)
			errcode = pt_time_update_mtc(time, &pkt->payload.mtc,
						     cfg);
		break;

	case ppt_cyc: {
		errcode = pt_tcal_update_cyc(tcal, &pkt->payload.cyc, cfg);
		if (errcode == 0) {
			fcr_status = pt_tcal_fcr(&fcr, tcal);
			if (fcr_status < 0) {
				TASK_CHECK(fcr_status == -pte_no_time);
				fcr = 0;
			}
			errcode = pt_time_update_cyc(time, &pkt->payload.cyc,
						     cfg, fcr);
		}
		break;
	}

	default:
		break;
	}

	*errcode_out = errcode;
}

static void make_config(struct pt_config *cfg, const struct gcfg *g,
			uint8_t *buf, size_t len)
{
	pt_config_init(cfg);
	cfg->begin = buf;
	cfg->end = buf + len;
	cfg->cpuid_0x15_eax = g->eax;
	cfg->cpuid_0x15_ebx = g->ebx;
	cfg->mtc_freq = g->mtc_freq;
	cfg->nom_freq = g->nom_freq;
}

/*
 * Encode, decode and verify one deterministic sequence.
 * Hand checks, if present, assert literal hand-computed event values.
 */
static void run_sequence(const char *group, unsigned seqno,
			 const struct gcfg *g,
			 const struct gstep *steps, unsigned nsteps,
			 const struct handcheck *hands, unsigned nhands)
{
	uint8_t buf[TRACE_BUF];
	struct pt_config enc_cfg, dec_cfg;
	struct pt_encoder *enc;
	struct pt_packet_decoder *dec;
	struct pt_packet pkt;
	struct pt_time time;
	struct pt_time_cal tcal;
	struct oracle o;
	uint64_t len;
	unsigned i;
	unsigned in_psb = 0;
	unsigned seen_hands = 0;
	struct evrec events[32];
	unsigned nevents = 0;

	TASK_CHECK(nsteps <= 64);
	TASK_CHECK(nhands <= 8);

	make_config(&enc_cfg, g, buf, sizeof(buf));
	enc = pt_alloc_encoder(&enc_cfg);
	TASK_CHECK(enc != NULL);

	/* Encode the whole sequence with the production encoder. */
	for (i = 0; i < nsteps; ++i) {
		struct pt_packet p;
		int size;

		memset(&p, 0, sizeof(p));
		switch (steps[i].kind) {
		case GK_PSB:
			p.type = ppt_psb;
			break;
		case GK_PSBEND:
			p.type = ppt_psbend;
			break;
		case GK_TSC:
			p.type = ppt_tsc;
			p.payload.tsc.tsc = steps[i].a;
			break;
		case GK_TMA:
			p.type = ppt_tma;
			p.payload.tma.ctc = (uint16_t) steps[i].a;
			p.payload.tma.fc = (uint16_t) steps[i].b;
			break;
		case GK_MTC:
			p.type = ppt_mtc;
			p.payload.mtc.ctc =
				(uint8_t) (steps[i].a & 0xffu);
			break;
		case GK_CYC:
			p.type = ppt_cyc;
			p.payload.cyc.value = steps[i].a;
			break;
		case GK_CBR:
			p.type = ppt_cbr;
			p.payload.cbr.ratio = (uint8_t) steps[i].a;
			break;
		}

		size = pt_enc_next(enc, &p);
		TASK_CHECK(size > 0);
	}

	{
		int off_status = pt_enc_get_offset(enc, &len);
		TASK_CHECK(off_status == 0);
	}
	pt_free_encoder(enc);
	TASK_CHECK(len > 0 && len < sizeof(buf));

	/* Decode and replay through the production time/calibration chain. */
	make_config(&dec_cfg, g, buf, (size_t) len);
	dec = pt_pkt_alloc_decoder(&dec_cfg);
	TASK_CHECK(dec != NULL);
	TASK_CHECK(pt_pkt_sync_forward(dec) == 0);

	pt_time_init(&time);
	pt_tcal_init(&tcal);
	o_init(&o);

	i = 0;
	for (;;) {
		uint64_t offset = 0;
		int n, errcode;
		uint64_t got_tsc = 0;
		uint32_t got_lm = 0, got_lc = 0;
		int query_status, fcr_status;
		uint64_t got_fcr = 0;

		TASK_CHECK(pt_pkt_get_offset(dec, &offset) == 0);
		n = pt_pkt_next(dec, &pkt, sizeof(pkt));
		if (n == -pte_eos)
			break;
		TASK_CHECK(n > 0);
		TASK_CHECK(i < nsteps);

		o.pkt_index = i;

		/* Verify the packet decoder round-trips the projected
		 * payload that the independent model encoded.
		 */
		switch (pkt.type) {
		case ppt_tsc:
			TASK_CHECK(pkt.payload.tsc.tsc == steps[i].a);
			break;
		case ppt_tma:
			TASK_CHECK(pkt.payload.tma.ctc ==
				   (uint16_t) steps[i].a);
			TASK_CHECK(pkt.payload.tma.fc ==
				   (uint16_t) steps[i].b);
			break;
		case ppt_mtc:
			TASK_CHECK(pkt.payload.mtc.ctc ==
				   (uint8_t) (steps[i].a & 0xffu));
			break;
		case ppt_cyc:
			TASK_CHECK(pkt.payload.cyc.value == steps[i].a);
			break;
		case ppt_cbr:
			TASK_CHECK(pkt.payload.cbr.ratio ==
				   (uint8_t) steps[i].a);
			break;
		default:
			break;
		}

		if (pkt.type == ppt_psb) {
			in_psb = 1;
			o_psb(&o);
		}
		if (pkt.type == ppt_psbend) {
			in_psb = 0;
			o_psbend(&o);
		}

		produce(&time, &tcal, &dec_cfg, &pkt, in_psb, &errcode);

		/* Drive the independent model with the same logical step. */
		switch (steps[i].kind) {
		case GK_TSC:
			o_tsc(&o, steps[i].a);
			break;
		case GK_CBR:
			o_cbr(&o, g, (uint32_t) steps[i].a);
			break;
		case GK_TMA:
			o_tma(&o, g, (uint32_t) steps[i].a,
			      (uint32_t) steps[i].b);
			break;
		case GK_MTC:
			o_mtc(&o, g, steps[i].a);
			break;
		case GK_CYC:
			o_cyc(&o, g, steps[i].a);
			break;
		default:
			break;
		}

		/* Valid sequences must keep the whole production chain
		 * healthy.  A negative status is itself a timing-chain
		 * discrepancy (for example a dropped wrap correction turns
		 * into -pte_bad_packet), so report it through the same
		 * TIMING_MISMATCH channel rather than a generic failure.
		 */
		if (errcode != 0)
			timing_mismatch(group, seqno, i, "update_errcode",
					(uint64_t)(unsigned) errcode, 0,
					steps[i].event);
		TASK_CHECK(errcode == 0);

		query_status = pt_time_query_tsc(&got_tsc, &got_lm, &got_lc,
						 &time);
		if (o.have_tsc) {
			TASK_CHECK(query_status == 0);
			check_uint(group, seqno, i, "tsc", got_tsc, o.tsc,
				   steps[i].event);
		} else {
			TASK_CHECK(query_status == -pte_no_time);
		}
		check_uint(group, seqno, i, "lost_mtc", got_lm, o.lost_mtc,
			   steps[i].event);
		check_uint(group, seqno, i, "lost_cyc", got_lc, o.lost_cyc,
			   steps[i].event);
		check_uint(group, seqno, i, "have_tsc",
			   (uint64_t)(query_status == 0),
			   (uint64_t) o.have_tsc, steps[i].event);

		fcr_status = pt_tcal_fcr(&got_fcr, &tcal);
		if (o.have_fcr) {
			TASK_CHECK(fcr_status == 0);
			check_uint(group, seqno, i, "fcr", got_fcr, o.fcr,
				   steps[i].event);
		} else {
			TASK_CHECK(fcr_status == -pte_no_time);
		}

		/* Collect forensic event records at marked positions. */
		if (steps[i].event) {
			TASK_CHECK(nevents <
				   sizeof(events) / sizeof(events[0]));
			events[nevents].id = steps[i].event;
			events[nevents].pkt_index = i;
			events[nevents].offset = offset;
			events[nevents].have_tsc = o.have_tsc;
			events[nevents].tsc = got_tsc;
			events[nevents].lost_mtc = got_lm;
			events[nevents].lost_cyc = got_lc;
			events[nevents].determined = o_determined(&o);
			events[nevents].anchor = o.anchor;
			events[nevents].anchor_pkt = o.anchor_pkt;
			events[nevents].cal_src = o.cal_src;
			++nevents;
		}

		++i;
	}

	TASK_CHECK(i == nsteps);
	pt_pkt_free_decoder(dec);

	/* Verify the literal hand-computed examples. */
	for (unsigned h = 0; h < nhands; ++h) {
		const struct evrec *r = NULL;

		for (unsigned k = 0; k < nevents; ++k) {
			if (!strcmp(events[k].id, hands[h].event)) {
				r = &events[k];
				break;
			}
		}

		TASK_CHECK(r != NULL);
		TASK_CHECK(r->have_tsc == 1);
		check_uint(group, seqno, r->pkt_index, "hand.tsc", r->tsc,
			   hands[h].tsc, r->id);
		check_uint(group, seqno, r->pkt_index, "hand.lost_mtc",
			   r->lost_mtc, hands[h].lost_mtc, r->id);
		check_uint(group, seqno, r->pkt_index, "hand.lost_cyc",
			   r->lost_cyc, hands[h].lost_cyc, r->id);
		check_uint(group, seqno, r->pkt_index, "hand.determined",
			   r->determined, hands[h].determined, r->id);
		++seen_hands;
	}
	TASK_CHECK(seen_hands == nhands);
}

/* Two evidence streams can only be ordered globally when both carry
 * determinable wall time.  An estimated or undetermined record must never
 * be used to assert cross-stream ordering.
 */
static int evidence_orderable(const struct evrec *a, const struct evrec *b)
{
	return a->determined && b->determined;
}

/* ------------------------------------------------------------------ */
/* Group 1: anchors - TSC/TMA anchoring and ignored MTC                */
/* ------------------------------------------------------------------ */

static void case_anchors(void)
{
	/* mf=4: 16 ticks/MTC period, crystal ratio 1/2, nominal CBR cal. */
	const struct gcfg a = { 2, 1, 4, 8 };
	/* mf=3: 8 ticks/period, fractional ratio 3/4. */
	const struct gcfg b = { 4, 3, 3, 0 };
	/* mf=5: 32 ticks/period, integer ratio 2. */
	const struct gcfg c = { 1, 2, 5, 16 };
	/* mf=0: lowest MTC frequency, TMA carries all 8 CTC bits. */
	const struct gcfg lo = { 2, 1, 0, 8 };
	/* mf=8: highest MTC frequency, TMA CTC spans all 16 bits. */
	const struct gcfg hi = { 1, 1, 8, 8 };

	/* 1. TSC anchor; time unavailable before the anchor. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), MTC(3), ECYC(10, "ANCHOR-NOTIME"),
			ETSC(10000, "ANCHOR-TSC"), ECYC(10, "ANCHOR-CYC"),
			PSBEND()
		};
		run_sequence("anchors", 1, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 2. Hand example inside a single MTC period: after TMA the CTC
	 * positions coincide, so no cross-period subtraction applies and
	 * each CYC is quantized independently at 4 fast-counter ticks per
	 * cycle (nominal 8 / CBR 2, fixed-point fcr = 1024).
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2),
			ETSC(10000, 0), TMA(0, 0),
			ECYC(36, "AUDIT-1"), ECYC(10, "AUDIT-2"),
			MTC(1), PSBEND()
		};
		struct handcheck h[] = {
			{ "AUDIT-1", 10144, 0, 0, 1 },
			{ "AUDIT-2", 10184, 0, 0, 1 }
		};
		run_sequence("anchors", 2, &a, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 2b. An MTC emitted between TSC and TMA is ignored by the time
	 * model (it cannot be placed until the TMA arrives); the later TMA
	 * re-establishes the crystal position.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2),
			ETSC(10000, 0), MTC(7), TMA(16, 0),
			ECYC(8, "IGNORED-MTC"), MTC(2), PSBEND()
		};
		run_sequence("anchors", 10, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 3. Low-frequency TMA: full CTC visible in the TMA payload. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(2000, 0), TMA(200, 40),
			MTC(201), ECYC(8, "LO-TMA"), PSBEND()
		};
		run_sequence("anchors", 3, &lo, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 4. High-frequency TMA: 16-bit CTC coverage. */
	{
		struct gstep s[] = {
			PSB(), CBR(4), ETSC(0x100000ull, 0), TMA(0xff00, 64),
			MTC(256), ECYC(16, "HI-TMA"), PSBEND()
		};
		run_sequence("anchors", 4, &hi, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 5. Fractional crystal ratio 3/4 with floor rounding per MTC. */
	{
		struct gstep s[] = {
			PSB(), ETSC(4096, 0), TMA(0, 0),
			MTC(1), MTC(2), MTC(3), EMTC(4, "FRAC-MTC"), PSBEND()
		};
		run_sequence("anchors", 5, &b, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 6. Integer ratio 2 with CYC contributions. */
	{
		struct gstep s[] = {
			PSB(), CBR(8), ETSC(8000, 0), TMA(0, 0),
			MTC(1), ECYC(20, "INT-RATIO"), MTC(2), PSBEND()
		};
		run_sequence("anchors", 6, &c, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 7. Ordinary TSC only stores calibration data; the header TSC of
	 * the next PSB+ is what actually calibrates.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(1000, 0), PSBEND(),
			CYC(25), ETSC(1200, 0),
			PSB(), ETSC(1200, "HDR-VS-TSC"), PSBEND()
		};
		run_sequence("anchors", 7, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 8. MTC calibration takes precedence over TSC calibration. */
	{
		struct gstep s[] = {
			PSB(), ETSC(3000, 0), PSBEND(),
			CYC(40), MTC(1), CYC(40), EMTC(2, "MTC-PRIMARY"),
			PSB(), ETSC(9000, 0), PSBEND()
		};
		run_sequence("anchors", 8, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 9. Second TSC re-anchors and clears any pending losses. */
	{
		struct gstep s[] = {
			PSB(), ETSC(500, 0), TMA(0, 0), MTC(1),
			ETSC(7777, "REANCHOR"), ECYC(12, "POST-ANCHOR"),
			PSBEND()
		};
		run_sequence("anchors", 9, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}
}

/* ------------------------------------------------------------------ */
/* Group 2: wrap - 8-bit MTC payload wrap-around                       */
/* ------------------------------------------------------------------ */

static void case_wrap(void)
{
	const struct gcfg a = { 2, 1, 4, 8 };	/* P=16, W=12 */
	const struct gcfg b = { 4, 3, 3, 8 };	/* fractional, P=8 */
	const struct gcfg z = { 1, 1, 0, 8 };	/* P=1, W=8 */

	/* 1. Hand example across the 8-bit MTC wrap 255 -> 0.  No CYC is
	 * emitted between the two wrap-adjacent MTCs, so the nominal CBR
	 * ratio stays in force.  The CYC after the wrap spans the boundary:
	 * the two crystal ticks since the TMA position estimate 2 old
	 * cycles, which are removed from the payload.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2),
			ETSC(50000, 0), TMA(4080, 0),
			EMTC(255, "WRAP-PRE"),
			EMTC(0, "WRAP-BOUNDARY"),
			ECYC(20, "WRAP-POST"), PSBEND()
		};
		struct handcheck h[] = {
			{ "WRAP-PRE", 50000, 0, 0, 1 },
			{ "WRAP-POST", 50080, 0, 0, 1 }
		};
		run_sequence("wrap", 1, &a, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 2. Wrap with mtc_freq=0: every tick is an MTC period. */
	{
		struct gstep s[] = {
			PSB(), CBR(1), ETSC(9000, 0), TMA(254, 0),
			MTC(255), EMTC(256, "ZERO-MF-WRAP"), CYC(4), PSBEND()
		};
		run_sequence("wrap", 2, &z, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 3. Wrap under the fractional 3/4 ratio. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(6000, 0), TMA(4088, 0),
			MTC(255), EMTC(256, "FRAC-WRAP"), MTC(257), PSBEND()
		};
		run_sequence("wrap", 3, &b, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 4. Pending CYC crosses the wrap; the old-period portion is
	 * subtracted through the wrapped CTC distance.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(7000, 0), TMA(4064, 0),
			MTC(254), MTC(255), CYC(60),
			EMTC(256, "WRAP-ADJUST"), PSBEND()
		};
		run_sequence("wrap", 4, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 5. A long forward MTC jump is placed with a single distance
	 * correction; the following TSC re-anchor confirms the time line.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(8000, 0), TMA(0, 0),
			MTC(1), MTC(255), EMTC(256, "NEAR-WRAP"),
			ETSC(8500, "NEAR-WRAP-REC"), PSBEND()
		};
		run_sequence("wrap", 5, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 6. Calibration across a wrap: MTC-derived FCR survives 255->0. */
	{
		struct gstep s[] = {
			PSB(), ETSC(1000, 0), PSBEND(),
			CYC(32), MTC(254), CYC(32),
			EMTC(255, 0), CYC(32), EMTC(256, "WRAP-CAL"), PSBEND()
		};
		run_sequence("wrap", 6, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 7. A TMA after a wrap re-establishes the CTC position. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(2000, 0), TMA(0, 0),
			MTC(255), EMTC(256, 0),
			ETSC(2400, 0), TMA(16, 8),
			ECYC(10, "WRAP-TMA"), PSBEND()
		};
		run_sequence("wrap", 7, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 8. Two consecutive single wraps in one long stream. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(3000, 0), TMA(0, 0),
			MTC(255), EMTC(256, 0), MTC(511),
			EMTC(512, "TWO-WRAPS"), PSBEND()
		};
		run_sequence("wrap", 8, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 9. A lost CYC (no calibration yet) is cleared by the first MTC;
	 * the following MTC crosses the 8-bit wrap.  The recovery MTC is
	 * named so a broken lost_cyc reset points at a concrete event.
	 */
	{
		struct gstep s[] = {
			PSB(), ETSC(4000, 0), TMA(4080, 0),
			ECYC(10, "WRAP9-LOST"),
			EMTC(255, "WRAP-RECOVER"), MTC(256), PSBEND()
		};
		struct handcheck h[] = {
			{ "WRAP9-LOST", 4000, 0, 1, 0 }
		};
		run_sequence("wrap", 9, &a, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}
}

/* ------------------------------------------------------------------ */
/* Group 3: overlap - CYC packets spanning MTC periods                 */
/* ------------------------------------------------------------------ */

static void case_overlap(void)
{
	const struct gcfg a = { 2, 1, 4, 8 };	/* P=16 */
	const struct gcfg b = { 4, 3, 3, 8 };	/* P=8, ratio 3/4 */
	const struct gcfg c = { 1, 2, 5, 8 };	/* P=32, ratio 2 */

	/* The cross-period subtraction only fires when a CYC arrives while
	 * the pending fast counter is zero and the crystal position has
	 * advanced past ctc_cyc, so the spanning CYC is emitted after the
	 * boundary MTC(s), never before them.
	 */

	/* 1. One CYC spanning two MTC periods: the estimated prior-period
	 * cycles (4) are removed, 76 cycles charge the current estimate.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(10000, 0), TMA(0, 0),
			MTC(1), MTC(2), ECYC(80, "SPAN-TWO"), PSBEND()
		};
		run_sequence("overlap", 1, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 2. One CYC spanning three MTC periods. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(20000, 0), TMA(0, 0),
			MTC(1), MTC(2), MTC(3),
			ECYC(120, "SPAN-THREE"), PSBEND()
		};
		run_sequence("overlap", 2, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 3. Saturation: the modeled old portion (6 cycles) exceeds the
	 * payload (2), so the payload is clamped and the current period
	 * receives no fast-counter contribution at all.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(30000, 0), TMA(0, 0),
			MTC(1), MTC(2), MTC(3),
			ECYC(2, "SATURATE"), PSBEND()
		};
		run_sequence("overlap", 3, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 4. Subtraction rounding under the fractional 3/4 ratio. */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(40000, 0), TMA(0, 0),
			MTC(1), MTC(2), ECYC(60, "FRAC-SPAN"), PSBEND()
		};
		run_sequence("overlap", 4, &b, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 5. CYC fully inside one period: a preceding CYC+MTC primes
	 * ctc_cyc so no subtraction applies to the measured packet.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(50000, 0), TMA(0, 0),
			MTC(1), CYC(8), MTC(2),
			ECYC(30, "IN-PERIOD"), PSBEND()
		};
		run_sequence("overlap", 5, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 6. Several CYCs between two MTCs: documented overlap imprecision
	 * is reproduced exactly, not treated as clock accuracy.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(60000, 0), TMA(0, 0),
			MTC(1), CYC(4), MTC(2),
			CYC(10), CYC(15), CYC(12),
			EMTC(3, "MULTI-CYC"), PSBEND()
		};
		run_sequence("overlap", 6, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 7. First CYC after a TMA carrying a fast-counter offset consumes
	 * that offset; after the following MTC, ctc and ctc_cyc coincide,
	 * so no period adjustment happens.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(70000, 0), TMA(0, 96),
			MTC(1), ECYC(40, "TMA-FC-PATH"), PSBEND()
		};
		run_sequence("overlap", 7, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 8. Long 32-tick periods at mtc_freq=5 with a spanning CYC. */
	{
		struct gstep s[] = {
			PSB(), CBR(8), ETSC(80000, 0), TMA(0, 0),
			MTC(1), MTC(2), ECYC(200, "LONG-PERIOD"), PSBEND()
		};
		run_sequence("overlap", 8, &c, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 9. A spanning CYC followed by another CYC (pending FC now
	 * non-zero, so no second subtraction), then an MTC clears it.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(2), ETSC(90000, 0), TMA(0, 0),
			MTC(1), MTC(2), CYC(50), CYC(20),
			EMTC(3, "OVERLAP-CLEAR"), PSBEND()
		};
		run_sequence("overlap", 9, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}
}

/* ------------------------------------------------------------------ */
/* Group 4: recalibrate - calibration loss and recovery                */
/* ------------------------------------------------------------------ */

static void case_recal(void)
{
	const struct gcfg a = { 2, 1, 4, 8 };
	const struct gcfg noxtal = { 0, 0, 4, 12 };

	/* 1. Nominal CBR calibration without any crystal ratio: the hand
	 * example is independent of CPUID 0x15.  100 cycles * 3 FC/cycle.
	 */
	{
		struct gstep s[] = {
			PSB(), CBR(4),
			ETSC(7000, 0), ECYC(100, "CAL-HAND"), PSBEND()
		};
		struct handcheck h[] = {
			{ "CAL-HAND", 7300, 0, 0, 1 }
		};
		run_sequence("recal", 1, &noxtal, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 2. Header TSC calibrates from CYCs accumulated after an ordinary
	 * TSC.  The ordinary TSC only stores its value; the PSB+ header TSC
	 * turns the stored pair into a ratio (80 FC over 40 cycles).
	 */
	{
		const struct gcfg nonom = { 2, 1, 4, 0 };
		struct gstep s[] = {
			PSB(), ETSC(1000, 0), PSBEND(),
			CYC(40),
			PSB(), ETSC(1080, "HDR-CAL"), CYC(20), PSBEND()
		};
		struct handcheck h[] = {
			{ "HDR-CAL", 1080, 0, 0, 1 }
		};
		run_sequence("recal", 2, &nonom, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 3. MTC calibration wins over a later TSC calibration. */
	{
		struct gstep s[] = {
			PSB(), ETSC(5000, 0), PSBEND(),
			CYC(16), MTC(1), CYC(16),
			EMTC(2, "MTC-BEATS-TSC"),
			PSB(), ETSC(9999, 0), PSBEND()
		};
		run_sequence("recal", 3, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 4. CYC before any calibration is loss; the next MTC clears
	 * lost_cyc.  This is the loss_reset mutation target.
	 */
	{
		struct gstep s[] = {
			PSB(), ETSC(6000, 0), TMA(0, 0),
			CYC(12), EMTC(1, "LOSS-RESET"),
			CYC(12), EMTC(2, "AFTER-RESET"), PSBEND()
		};
		struct handcheck h[] = {
			{ "LOSS-RESET", 6008, 0, 0, 1 }
		};
		run_sequence("recal", 4, &a, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 5. Two unusable CYCs accumulate lost_cyc; evidence is
	 * undetermined in that window.
	 */
	{
		struct gstep s[] = {
			PSB(), ETSC(6100, 0), TMA(0, 0),
			ECYC(7, "RECAL-LOST"), CYC(9),
			ETSC(6400, "RECAL-TSC-REC"), PSBEND()
		};
		struct handcheck h[] = {
			{ "RECAL-LOST", 6100, 0, 1, 0 },
			{ "RECAL-TSC-REC", 6400, 0, 0, 1 }
		};
		run_sequence("recal", 5, &a, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 6. With no nominal frequency, a CBR change outside PSB+ invalidates
	 * the old MTC-derived ratio; the following CYCs are lost (lost_cyc
	 * rises, evidence undetermined).  The next MTC clears lost_cyc even
	 * though tcal only re-arms there; a further CYC+MTC restores a
	 * usable ratio.
	 */
	{
		const struct gcfg nonom = { 2, 1, 4, 0 };
		struct gstep s[] = {
			PSB(), ETSC(7000, 0), TMA(0, 0), PSBEND(),
			MTC(1), CYC(16), MTC(2),
			CBR(4), ECYC(20, "CBR-INVALID"), CYC(16),
			EMTC(3, "CBR-MTC-REC"),
			CYC(8), MTC(4)
		};
		struct handcheck h[] = {
			{ "CBR-INVALID", 7016, 0, 1, 0 },
			{ "CBR-MTC-REC", 7024, 0, 0, 1 }
		};
		run_sequence("recal", 6, &nonom, s,
			     sizeof(s) / sizeof(s[0]), h,
			     sizeof(h) / sizeof(h[0]));
	}

	/* 7. Full timeline: loss window, MTC calibration, determined time. */
	{
		struct gstep s[] = {
			PSB(), ETSC(8000, 0), TMA(0, 0),
			ECYC(24, "RECAL-LOST2"),
			MTC(1), CYC(24),
			EMTC(2, "RECAL-REC"), PSBEND()
		};
		run_sequence("recal", 7, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 8. CBR inside a fresh PSB+ installs a nominal ratio directly. */
	{
		struct gstep s[] = {
			PSB(), CBR(8), ETSC(9000, 0),
			ECYC(40, "PSB-CBR"), PSBEND()
		};
		run_sequence("recal", 8, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* 9. A TSC anchor recovers every pending loss at once. */
	{
		struct gstep s[] = {
			PSB(), ETSC(10000, 0), TMA(0, 0),
			CYC(5), CYC(6),
			ETSC(10333, "TSC-CLEARS-ALL"), PSBEND()
		};
		run_sequence("recal", 9, &a, s,
			     sizeof(s) / sizeof(s[0]), 0, 0);
	}

	/* Cross-stream ordering must be refused while one side is
	 * undetermined.  The literals mirror recal-5 and wrap-1.
	 */
	{
		struct evrec undetermined = {
			"RECAL-LOST", 3, 0, 1, 6100, 0, 1, 0, 6100, 1, 0
		};
		struct evrec determined = {
			"WRAP-POST", 6, 0, 1, 50080, 0, 0, 1, 50000, 1, 1
		};

		TASK_CHECK(evidence_orderable(&undetermined, &determined) == 0);
		TASK_CHECK(evidence_orderable(&determined, &determined) == 1);
	}
}

/* ------------------------------------------------------------------ */
/* Invalid arguments and truncated packets                             */
/* ------------------------------------------------------------------ */

static void case_invalid_and_truncated(void)
{
	struct pt_config cfg;
	struct pt_time time;
	struct pt_time_cal tcal;
	struct pt_packet p;
	uint8_t buf[TRACE_BUF];
	uint64_t tsc, fcr;
	uint32_t lm, lc;
	int st;

	pt_config_init(&cfg);
	cfg.begin = buf;
	cfg.end = buf + sizeof(buf);
	cfg.cpuid_0x15_eax = 2;
	cfg.cpuid_0x15_ebx = 1;
	cfg.mtc_freq = 4;
	cfg.nom_freq = 8;

	pt_time_init(&time);
	pt_tcal_init(&tcal);

	/* NULL arguments. */
	memset(&p, 0, sizeof(p));
	p.payload.tsc.tsc = 1;
	TASK_CHECK(pt_time_update_tsc(NULL, &p.payload.tsc, &cfg)
		   == -pte_internal);
	TASK_CHECK(pt_time_update_tsc(&time, NULL, &cfg) == -pte_internal);
	TASK_CHECK(pt_time_update_tma(&time, NULL, &cfg) == -pte_internal);
	TASK_CHECK(pt_time_update_mtc(&time, NULL, &cfg) == -pte_internal);
	TASK_CHECK(pt_time_update_cyc(&time, NULL, &cfg, 1) == -pte_internal);
	TASK_CHECK(pt_tcal_update_mtc(NULL, &p.payload.mtc, &cfg)
		   == -pte_internal);

	/* No time / no ratio before any anchor. */
	TASK_CHECK(pt_time_query_tsc(&tsc, &lm, &lc, &time)
		   == -pte_no_time);
	TASK_CHECK(pt_tcal_fcr(&fcr, &tcal) == -pte_no_time);

	/* TMA requires a TSC context. */
	p.payload.tma.ctc = 0;
	p.payload.tma.fc = 0;
	TASK_CHECK(pt_time_update_tma(&time, &p.payload.tma, &cfg)
		   == -pte_bad_context);

	/* CBR ratio zero is rejected. */
	p.payload.cbr.ratio = 0;
	TASK_CHECK(pt_time_update_cbr(&time, &p.payload.cbr, &cfg)
		   == -pte_bad_packet);
	TASK_CHECK(pt_tcal_header_cbr(&tcal, &p.payload.cbr, &cfg)
		   == -pte_bad_packet);

	/* Bad crystal configuration surfaces from MTC processing. */
	{
		struct pt_config bad = cfg;
		struct pt_packet_tsc tp = { 4096 };
		struct pt_packet_tma ap = { 0, 0 };
		struct pt_packet_mtc mp = { 1 };

		bad.cpuid_0x15_eax = 0;
		TASK_CHECK(pt_time_update_tsc(&time, &tp, &bad) == 0);
		TASK_CHECK(pt_time_update_tma(&time, &ap, &bad) == 0);
		st = pt_time_update_mtc(&time, &mp, &bad);
		TASK_CHECK(st == -pte_bad_config);
	}

	/* Truncated packets: encode valid streams and cut the buffer end. */
	{
		struct pt_encoder *enc;
		struct pt_config ec;
		struct pt_packet_decoder *dec;
		uint64_t full;

		pt_config_init(&ec);
		ec.begin = buf;
		ec.end = buf + sizeof(buf);
		ec.cpuid_0x15_eax = 2;
		ec.cpuid_0x15_ebx = 1;
		ec.mtc_freq = 4;

		enc = pt_alloc_encoder(&ec);
		TASK_CHECK(enc != NULL);
		memset(&p, 0, sizeof(p));
		p.type = ppt_psb;
		TASK_CHECK(pt_enc_next(enc, &p) > 0);
		p.type = ppt_tsc;
		p.payload.tsc.tsc = 0x11223344556677ull;
		TASK_CHECK(pt_enc_next(enc, &p) > 0);
		TASK_CHECK(pt_enc_get_offset(enc, &full) == 0);
		pt_free_encoder(enc);

		/* Cut one, three and seven bytes off the TSC payload. */
		for (unsigned cut = 1; cut <= 7; cut += 2) {
			struct pt_config dc = ec;
			dc.end = buf + full - cut;
			dec = pt_pkt_alloc_decoder(&dc);
			TASK_CHECK(dec != NULL);
			TASK_CHECK(pt_pkt_sync_forward(dec) == 0);
			st = pt_pkt_next(dec, &p, sizeof(p));
			TASK_CHECK(st > 0);
			st = pt_pkt_next(dec, &p, sizeof(p));
			TASK_CHECK(st == -pte_eos);
			pt_pkt_free_decoder(dec);
		}

		/* Truncated extended CYC. */
		{
			struct pt_config ec2 = ec;
			uint8_t buf2[64];
			uint64_t len2;
			ec2.begin = buf2;
			ec2.end = buf2 + sizeof(buf2);
			enc = pt_alloc_encoder(&ec2);
			TASK_CHECK(enc != NULL);
			memset(&p, 0, sizeof(p));
			p.type = ppt_psb;
			TASK_CHECK(pt_enc_next(enc, &p) > 0);
			p.type = ppt_cyc;
			p.payload.cyc.value = 5000;	/* multi-byte CYC */
			TASK_CHECK(pt_enc_next(enc, &p) > 0);
			TASK_CHECK(pt_enc_get_offset(enc, &len2) == 0);
			pt_free_encoder(enc);

			{
				struct pt_config dc = ec2;
				dc.end = buf2 + len2 - 1;
				dec = pt_pkt_alloc_decoder(&dc);
				TASK_CHECK(dec != NULL);
				TASK_CHECK(pt_pkt_sync_forward(dec) == 0);
				TASK_CHECK(pt_pkt_next(dec, &p, sizeof(p)) > 0);
				TASK_CHECK(pt_pkt_next(dec, &p, sizeof(p))
					   == -pte_eos);
				pt_pkt_free_decoder(dec);
			}
		}

		/* Truncated TMA. */
		{
			struct pt_config ec3 = ec;
			uint8_t buf3[64];
			uint64_t len3;
			ec3.begin = buf3;
			ec3.end = buf3 + sizeof(buf3);
			enc = pt_alloc_encoder(&ec3);
			TASK_CHECK(enc != NULL);
			memset(&p, 0, sizeof(p));
			p.type = ppt_psb;
			TASK_CHECK(pt_enc_next(enc, &p) > 0);
			p.type = ppt_tma;
			p.payload.tma.ctc = 12;
			p.payload.tma.fc = 34;
			TASK_CHECK(pt_enc_next(enc, &p) > 0);
			TASK_CHECK(pt_enc_get_offset(enc, &len3) == 0);
			pt_free_encoder(enc);

			{
				struct pt_config dc = ec3;
				dc.end = buf3 + len3 - 1;
				dec = pt_pkt_alloc_decoder(&dc);
				TASK_CHECK(dec != NULL);
				TASK_CHECK(pt_pkt_sync_forward(dec) == 0);
				TASK_CHECK(pt_pkt_next(dec, &p, sizeof(p)) > 0);
				TASK_CHECK(pt_pkt_next(dec, &p, sizeof(p))
					   == -pte_eos);
				pt_pkt_free_decoder(dec);
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static void group_anchors(void)
{
	case_anchors();
	case_invalid_and_truncated();
}

const struct task_case task_cases[] = {
	{ "anchors", group_anchors },
	{ "wrap", case_wrap },
	{ "overlap", case_overlap },
	{ "recalibrate", case_recal }
};

const unsigned task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
