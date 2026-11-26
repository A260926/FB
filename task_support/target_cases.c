/*
 * target_cases.c - gain layer tests.
 *
 * scope    : imports a nested MonoAudioGain graph through the real
 *            aafi_retrieveData() parser, with a leading Sequence Filler
 *            and a trailing sibling clip.
 * curves   : exact rational projection, non-linear layer product, rational
 *            time spellings, Constant interpolation breakpoints,
 *            fractional / large positions and illegal curves.
 * lifetime : evaluation after scope exit, sibling release ordering,
 *            no-gain / legacy field compatibility and the shipped
 *            DR_Audio_Levels.aaf real file.
 *
 * The AAF object graphs are built in memory in C and handed to the
 * existing parser; no AAF file needs to be written.
 */

#include "task_support/check.h"

#include <libaaf/AAFIface.h>
#include <libaaf/AAFIParser.h>
#include <libaaf/AAFDefs/AAFClassDefUIDs.h>
#include <libaaf/AAFDefs/AAFPropertyIDs.h>
#include <libaaf/AAFDefs/AAFTypeDefUIDs.h>
#include <libaaf/AAFDefs/AAFOperationDefs.h>
#include <libaaf/AAFDefs/AAFParameterDefs.h>
#include <libaaf/AAFDefs/AAFInterpolatorDefs.h>
#include <libaaf/AAFDefs/AAFDataDefs.h>
#include <libaaf/AAFDefs/AAFExtEnum.h>

#include "src/AAFCore/AAFClass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>


#define R(n, d) ((aafRational_t){ (int32_t)(n), (int32_t)(d) })

#define KEY_DATA_DEF   10u
#define KEY_OP_GAIN    11u
#define KEY_INTERP_LIN 12u
#define KEY_INTERP_CST 13u
#define KEY_INTERP_POW 14u


/* ------------------------------------------------------------------ */
/* In-memory AAF object graph builder                                  */
/* ------------------------------------------------------------------ */

typedef struct H {
	AAF_Iface  *aafi;
	AAF_Data   *d;
	uint32_t    key;
	aafMobID_t  comp_id;
	aafMobID_t  master_id;
	aafMobID_t  source_id;
} H;


static aafMobID_t h_mobid( uint8_t tag )
{
	aafMobID_t id;
	memset( &id, 0, sizeof(id) );
	id.SMPTELabel[0] = 0x06;
	id.length = 0x60;
	id.material.Data1 = 0x4c414700u + tag;
	id.material.Data4[7] = tag;
	return id;
}


static aafObject *tb_obj( H *h, const aafUID_t *class_id )
{
	aafObject *o = calloc( 1, sizeof(aafObject) );
	if ( !o ) {
		return NULL;
	}

	aafClass *k = aafclass_getClassByID( h->d, class_id );
	if ( !k ) {
		k = aafclass_defineNewClass( h->d, class_id, CONCRETE, NULL );
	}

	o->Class = k;
	o->aafd  = h->d;

	o->nextObj = h->d->Objects;
	h->d->Objects = o;

	return o;
}


static void tb_prop_val( aafObject *o, aafPID_t pid, uint16_t sf,
                         const void *val, size_t len )
{
	aafProperty *p = calloc( 1, sizeof(aafProperty) );
	if ( !p ) {
		return;
	}

	p->pid = pid;
	p->sf  = sf;
	p->len = (uint16_t)len;

	if ( sf == SF_STRONG_OBJECT_REFERENCE ||
	     sf == SF_STRONG_OBJECT_REFERENCE_SET ||
	     sf == SF_STRONG_OBJECT_REFERENCE_VECTOR ) {
		/* Strong reference property values hold the target object pointer. */
		p->val = (void *)val;
	}
	else {
		p->val = malloc( len ? len : 1 );
		if ( p->val ) {
			memcpy( p->val, val, len );
		}
	}

	p->next = o->Properties;
	o->Properties = p;
}


static void tb_i64( aafObject *o, aafPID_t pid, int64_t v )
{
	tb_prop_val( o, pid, SF_DATA, &v, sizeof(v) );
}

static void tb_u32( aafObject *o, aafPID_t pid, uint32_t v )
{
	tb_prop_val( o, pid, SF_DATA, &v, sizeof(v) );
}

static void tb_rat( aafObject *o, aafPID_t pid, aafRational_t v )
{
	tb_prop_val( o, pid, SF_DATA, &v, sizeof(v) );
}

static void tb_uid( aafObject *o, aafPID_t pid, const aafUID_t *u )
{
	tb_prop_val( o, pid, SF_DATA, u, sizeof(aafUID_t) );
}

static void tb_mobid( aafObject *o, aafPID_t pid, const aafMobID_t *m )
{
	tb_prop_val( o, pid, SF_DATA, m, sizeof(aafMobID_t) );
}

static void tb_weak( aafObject *o, aafPID_t pid, uint32_t key )
{
	unsigned char buf[sizeof(aafWeakRef_t) + 2];
	memset( buf, 0, sizeof(buf) );
	aafWeakRef_t *w = (aafWeakRef_t *)buf;
	w->_referencedPropertyIndex = (uint16_t)key;
	tb_prop_val( o, pid, SF_WEAK_OBJECT_REFERENCE, buf, sizeof(buf) );
}


static void tb_indirect_rational( aafObject *o, aafPID_t pid, aafRational_t r )
{
	unsigned char buf[1 + sizeof(aafIndirect_t) + sizeof(aafRational_t)];
	memset( buf, 0, sizeof(buf) );

	buf[0] = AAF_PROPERTIES_BYTEORDER_UNSPECIFIED; /* skipped byte */
	memcpy( buf + 1, &AAFTypeID_Rational, sizeof(aafUID_t) );
	memcpy( buf + 1 + sizeof(aafIndirect_t), &r, sizeof(r) );

	tb_prop_val( o, pid, SF_DATA, buf, sizeof(buf) );
}


/*
 * Gives one collection member its own vector/set index header and entry,
 * exactly like parsed objects have. aaf_release() frees both once.
 */
static void tb_member_index( H *h, aafObject *item, uint32_t count )
{
	aafStrongRefSetHeader_t *hdr = calloc( 1, sizeof(*hdr) );
	aafStrongRefSetEntry_t  *ent = calloc( 1, sizeof(*ent) );

	if ( hdr ) {
		hdr->_entryCount = count;
		hdr->_identificationSize = 0; /* local key reference table */
		item->Header = hdr;
	}

	if ( ent ) {
		ent->_localKey = ++h->key;
		item->Entry = ent;
	}
}


static void tb_vec( H *h, aafObject *owner, aafPID_t pid,
                    aafObject **items, unsigned int count )
{
	aafObject *head = NULL;

	for ( unsigned int i = 0; i < count; i++ ) {
		aafObject *it = items[i];
		if ( !it ) {
			continue;
		}

		tb_member_index( h, it, count );
		it->Parent = owner;
		it->prev = NULL;
		it->next = NULL;

		if ( !head ) {
			head = it;
		}
		else {
			aafObject *last = head;
			while ( last->next ) {
				last = last->next;
			}
			last->next = it;
			it->prev = last;
		}
	}

	tb_prop_val( owner, pid, SF_STRONG_OBJECT_REFERENCE_VECTOR, head, 0 );
}


static void tb_strong( H *h, aafObject *owner, aafPID_t pid, aafObject *child )
{
	(void)h;
	if ( child ) {
		child->Parent = owner;
	}
	tb_prop_val( owner, pid, SF_STRONG_OBJECT_REFERENCE, child, 0 );
}


static aafObject *tb_def( H *h, const aafUID_t *ident, uint32_t key )
{
	aafObject *o = tb_obj( h, &AAFClassID_OperationDefinition );
	tb_uid( o, PID_DefinitionObject_Identification, ident );
	tb_member_index( h, o, 1 );
	if ( o->Entry ) {
		o->Entry->_localKey = key;
	}
	return o;
}


/* --------------------------- graph pieces -------------------------- */

static aafObject *make_filler( H *h, int64_t len )
{
	aafObject *o = tb_obj( h, &AAFClassID_Filler );
	tb_weak( o, PID_Component_DataDefinition, KEY_DATA_DEF );
	tb_i64( o, PID_Component_Length, len );
	return o;
}


static aafObject *make_source_clip( H *h, int64_t len, int64_t start_time,
                                   const aafMobID_t *target_mob )
{
	aafObject *o = tb_obj( h, &AAFClassID_SourceClip );
	tb_weak( o, PID_Component_DataDefinition, KEY_DATA_DEF );
	tb_i64( o, PID_Component_Length, len );
	tb_i64( o, PID_SourceClip_StartTime, start_time );
	tb_u32( o, PID_SourceReference_SourceMobSlotID, 1 );
	tb_mobid( o, PID_SourceReference_SourceID, target_mob );
	return o;
}


static aafObject *make_sequence( H *h, aafObject **items, unsigned count )
{
	aafObject *o = tb_obj( h, &AAFClassID_Sequence );
	tb_weak( o, PID_Component_DataDefinition, KEY_DATA_DEF );
	tb_vec( h, o, PID_Sequence_Components, items, count );
	return o;
}


static aafObject *make_cp( H *h, aafRational_t t, aafRational_t v )
{
	aafObject *o = tb_obj( h, &AAFClassID_ControlPoint );
	tb_rat( o, PID_ControlPoint_Time, t );
	tb_indirect_rational( o, PID_ControlPoint_Value, v );
	return o;
}


typedef struct LayerCfg {
	int             constant;      /* ConstantValue instead of VaryingValue */
	aafRational_t   cval;

	int             interp;        /* 0 linear, 1 constant, 2 power, 3 none */

	int             zero_length;
	int             omit_length;

	unsigned int    point_count;
	aafRational_t   times[12];
	aafRational_t   values[12];

	int             omit_point_list;
	int             omit_interpolation;
} LayerCfg;


static aafObject *make_gain_og( H *h, const LayerCfg *cfg, int64_t length )
{
	aafObject *og = tb_obj( h, &AAFClassID_OperationGroup );

	tb_weak( og, PID_Component_DataDefinition, KEY_DATA_DEF );
	tb_weak( og, PID_OperationGroup_Operation, KEY_OP_GAIN );

	if ( cfg->omit_length ) {
		/* no length property */
	}
	else if ( cfg->zero_length ) {
		tb_i64( og, PID_Component_Length, 0 );
	}
	else {
		tb_i64( og, PID_Component_Length, length );
	}

	aafObject *param = NULL;

	if ( cfg->constant ) {
		param = tb_obj( h, &AAFClassID_ConstantValue );
		tb_uid( param, PID_Parameter_Definition, &AAFParameterDef_Amplitude );
		tb_indirect_rational( param, PID_ConstantValue_Value, cfg->cval );
	}
	else {
		aafObject *vv = tb_obj( h, &AAFClassID_VaryingValue );
		param = vv;
		tb_uid( vv, PID_Parameter_Definition, &AAFParameterDef_Amplitude );

		if ( !cfg->omit_interpolation ) {
			uint32_t key = KEY_INTERP_LIN;
			if ( cfg->interp == 1 ) { key = KEY_INTERP_CST; }
			if ( cfg->interp == 2 ) { key = KEY_INTERP_POW; }
			tb_weak( vv, PID_VaryingValue_Interpolation, key );
		}

		if ( !cfg->omit_point_list ) {
			aafObject *cps[12];
			unsigned int n = cfg->point_count;
			if ( n > 12 ) { n = 12; }

			for ( unsigned int i = 0; i < n; i++ ) {
				cps[i] = make_cp( h, cfg->times[i], cfg->values[i] );
			}

			tb_vec( h, vv, PID_VaryingValue_PointList, cps, n );
		}
	}

	aafObject *params[] = { param };
	tb_vec( h, og, PID_OperationGroup_Parameters, params, 1 );

	return og;
}


static aafObject *make_tms( H *h, aafObject *segment )
{
	aafObject *tms = tb_obj( h, &AAFClassID_TimelineMobSlot );
	tb_u32( tms, PID_MobSlot_SlotID, 1 );
	tb_rat( tms, PID_TimelineMobSlot_EditRate, R(1, 1) );
	tb_strong( h, tms, PID_MobSlot_Segment, segment );
	return tms;
}


/*
 * Finish the graph with CompositionMob/TimelineMobSlot whose Segment is
 * @p segment (no extra Sequence wrapper).
 */
static void h_finish_segment( H *h, aafObject *segment )
{
	/* MasterMob -> TimelineMobSlot -> SourceClip -> SourceMob. */
	aafObject *master_clip = make_source_clip( h, 8, 0, &h->source_id );
	aafObject *master_tms  = make_tms( h, master_clip );

	aafObject *master = tb_obj( h, &AAFClassID_MasterMob );
	tb_mobid( master, PID_Mob_MobID, &h->master_id );
	aafObject *master_slots[] = { master_tms };
	tb_vec( h, master, PID_Mob_Slots, master_slots, 1 );

	aafObject *source = tb_obj( h, &AAFClassID_SourceMob );
	tb_mobid( source, PID_Mob_MobID, &h->source_id );

	/* SourceMob needs Mob::Slots + a matching SlotID for reference resolution. */
	aafObject *source_tms = tb_obj( h, &AAFClassID_TimelineMobSlot );
	tb_u32( source_tms, PID_MobSlot_SlotID, 1 );
	tb_rat( source_tms, PID_TimelineMobSlot_EditRate, R( 48000, 1 ) );
	aafObject *source_slots[] = { source_tms };
	tb_vec( h, source, PID_Mob_Slots, source_slots, 1 );

	/* CompositionMob with the caller supplied TimelineMobSlot Segment. */
	aafObject *tms = make_tms( h, segment );

	aafObject *comp = tb_obj( h, &AAFClassID_CompositionMob );
	tb_mobid( comp, PID_Mob_MobID, &h->comp_id );
	tb_uid( comp, PID_Mob_UsageCode, &AAFUsage_TopLevel );
	aafObject *slots[] = { tms };
	tb_vec( h, comp, PID_Mob_Slots, slots, 1 );

	/*
	 * The Mob objects are members of the ContentStorage Mobs set, not items
	 * of any tb_vec, so give each its own set index header. The retrieve
	 * trace macro reads the head Header->_entryCount.
	 */
	tb_member_index( h, comp, 3 );
	tb_member_index( h, master, 3 );
	tb_member_index( h, source, 3 );

	h->d->Mobs = comp;
	comp->next = master;
	master->next = source;
}


/*
 * Builds: CompositionMob/TimelineMobSlot/Sequence[ root_item ].
 * Also builds the shared MasterMob/SourceMob chain so SourceClip
 * reference resolution and essence pointer creation succeed.
 */
static void h_finish_mobs( H *h, aafObject *root_item )
{
	aafObject *main_seq = make_sequence( h, (aafObject *[]){ root_item }, 1 );
	h_finish_segment( h, main_seq );
}


static void h_init( H *h )
{
	memset( h, 0, sizeof(*h) );

	h->aafi = aafi_alloc( NULL );
	TASK_CHECK( h->aafi != NULL );

	h->d = h->aafi->aafd;
	aafclass_setDefaultClasses( h->d );

	if ( getenv( "TC_VERBOSE" ) ) {
		aafi_set_debug( h->aafi, VERB_ERROR, 0, stderr, NULL, NULL );
	}

	h->comp_id   = h_mobid( 3 );
	h->master_id = h_mobid( 1 );
	h->source_id = h_mobid( 2 );

	/* Weak reference definition tables. */
	h->d->DataDefinition = tb_def( h, &AAFDataDef_Sound, KEY_DATA_DEF );
	h->d->OperationDefinition = tb_def( h, &AAFOperationDef_MonoAudioGain, KEY_OP_GAIN );

	aafObject *lin = tb_def( h, &AAFInterpolationDef_Linear, KEY_INTERP_LIN );
	aafObject *cst = tb_def( h, &AAFInterpolationDef_Constant, KEY_INTERP_CST );
	aafObject *pow = tb_def( h, &AAFInterpolationDef_Power, KEY_INTERP_POW );
	lin->next = cst;
	cst->next = pow;
	h->d->InterpolationDefinition = lin;
}


static void h_destroy( H *h )
{
	aafi_release( &h->aafi );
	h->aafi = NULL;
	h->d = NULL;
}


/*
 * Builds a nested stack of gain groups around one leaf clip.
 * cfgs[0] is the outermost layer. fillers[i] optionally inserts a
 * Filler before layer i's child, and group_lens[i] sets that group's
 * Component::Length.
 */
static aafiAudioClip *build_layer_stack_f( H *h,
                                          const LayerCfg *cfgs,
                                          const int64_t *fillers,
                                          unsigned int n,
                                          const int64_t *group_lens,
                                          int64_t clip_len )
{
	aafObject *leaf = make_source_clip( h, clip_len, 777, &h->master_id );
	aafObject *child = leaf;

	for ( int i = (int)n - 1; i >= 0; i-- ) {

		int64_t glen = group_lens ? group_lens[i] : 40;
		aafObject *og = make_gain_og( h, &cfgs[i], glen );

		if ( fillers && fillers[i] > 0 ) {
			aafObject *f = make_filler( h, fillers[i] );
			aafObject *seq = make_sequence( h, (aafObject *[]){ f, child }, 2 );
			aafObject *inputs[] = { seq };
			tb_vec( h, og, PID_OperationGroup_InputSegments, inputs, 1 );
		}
		else {
			aafObject *inputs[] = { child };
			tb_vec( h, og, PID_OperationGroup_InputSegments, inputs, 1 );
		}

		child = og;
	}

	h_finish_mobs( h, child );

	int rc = aafi_retrieveData( h->aafi );
	TASK_CHECK( rc == 0 );

	aafiAudioTrack *t = h->aafi->Audio->Tracks;
	TASK_CHECK( t != NULL );
	aafiTimelineItem *it = t->timelineItems;
	TASK_CHECK( it != NULL );
	aafiAudioClip *clip = aafi_timelineItemToAudioClip( it );
	TASK_CHECK( clip != NULL );

	return clip;
}


static unsigned collect_clips( AAF_Iface *aafi, aafiAudioClip ***out )
{
	unsigned n = 0;

	for ( aafiAudioTrack *t = aafi->Audio->Tracks; t; t = t->next ) {
		for ( aafiTimelineItem *it = t->timelineItems; it; it = it->next ) {
			if ( aafi_timelineItemToAudioClip( it ) ) {
				n++;
			}
		}
	}

	aafiAudioClip **arr = calloc( n ? n : 1, sizeof(*arr) );
	n = 0;

	for ( aafiAudioTrack *t = aafi->Audio->Tracks; t; t = t->next ) {
		for ( aafiTimelineItem *it = t->timelineItems; it; it = it->next ) {
			aafiAudioClip *c = aafi_timelineItemToAudioClip( it );
			if ( c ) {
				arr[n++] = c;
			}
		}
	}

	*out = arr;
	return n;
}


static int gain_rc( const aafiAudioClip *c, int32_t num, int32_t den, double *m )
{
	aafRational_t off = R( num, den );
	return aafi_clip_gain_at( c, off, m );
}


static int near_mul( double got, double want )
{
	double tol = 1e-9 * fmax( 1.0, fabs(want) );
	return fabs( got - want ) <= tol + 1e-12;
}


/* ------------------------------------------------------------------ */
/* scope                                                               */
/* ------------------------------------------------------------------ */

/*
 * Builds the task's numeric graph:
 *
 * outer MonoAudioGain OG, length 100, linear 1 -> 3 over t=0..1
 *   input Sequence:
 *     Filler length 20
 *     inner MonoAudioGain OG, length 40, linear 2 -> 4
 *       SourceClip A length 40
 *     SourceClip B length 40   (outer layer only)
 */
static void run_scope( void )
{
	H h;
	h_init( &h );

	aafObject *clip_a = make_source_clip( &h, 40, 321, &h.master_id );

	/* inner group */
	LayerCfg inner;
	memset( &inner, 0, sizeof(inner) );
	inner.point_count = 2;
	inner.times[0] = R(0,1);  inner.values[0] = R(2,1);
	inner.times[1] = R(1,1);  inner.values[1] = R(4,1);

	aafObject *inner_g = make_gain_og( &h, &inner, 40 );
	aafObject *inner_inputs[] = { clip_a };
	tb_vec( &h, inner_g, PID_OperationGroup_InputSegments, inner_inputs, 1 );

	/* trailing sibling clip */
	aafObject *clip_b = make_source_clip( &h, 40, 0, &h.master_id );

	/* outer input sequence: Filler(20), inner group, sibling clip */
	aafObject *filler = make_filler( &h, 20 );
	aafObject *outer_seq = make_sequence(
		&h, (aafObject *[]){ filler, inner_g, clip_b }, 3 );

	/* outer group */
	LayerCfg outer;
	memset( &outer, 0, sizeof(outer) );
	outer.point_count = 2;
	outer.times[0] = R(0,1); outer.values[0] = R(1,1);
	outer.times[1] = R(1,1); outer.values[1] = R(3,1);

	aafObject *outer_g = make_gain_og( &h, &outer, 100 );
	aafObject *outer_inputs[] = { outer_seq };
	tb_vec( &h, outer_g, PID_OperationGroup_InputSegments, outer_inputs, 1 );

	h_finish_mobs( &h, outer_g );

	TASK_CHECK( aafi_retrieveData( h.aafi ) == 0 );

	aafiAudioClip **clips = NULL;
	unsigned nclips = collect_clips( h.aafi, &clips );
	TASK_CHECK( nclips == 2 );

	aafiAudioClip *a = clips[0];
	aafiAudioClip *b = clips[1];

	/* nested clip keeps BOTH layers, independent of parser ctx. */
	TASK_CHECK( aafi_clip_gain_layer_count( a ) == 2 );
	TASK_CHECK( aafi_clip_gain_layer_count( b ) == 1 );

	double m = -1.0;

	TASK_CHECK( gain_rc( a, 10, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 4.0 ) );            /* 1.6 * 2.5 */

	TASK_CHECK( gain_rc( a, 20, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 5.4 ) );            /* 1.8 * 3.0 */

	/* offset 0: outer at 20/100 = 1.4, inner at 0 = 2 -> 2.8 */
	TASK_CHECK( gain_rc( a, 0, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.8 ) );

	/* sibling inherits outer only: at its start outer pos 60/100 = 2.2 */
	TASK_CHECK( gain_rc( b, 0, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.2 ) );

	/* and it must not inherit the inner curve: offset 10 -> 70/100 = 2.4 */
	TASK_CHECK( gain_rc( b, 10, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.4 ) );

	/* offset 20 -> 80/100 = 2.6 */
	TASK_CHECK( gain_rc( b, 20, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.6 ) );

	/* out of range offset is rejected */
	TASK_CHECK( gain_rc( a, 40, 1, &m ) != 0 );

	free( clips );
	h_destroy( &h );
}


/* ------------------------------------------------------------------ */
/* curves                                                              */
/* ------------------------------------------------------------------ */

static void run_curves_product( void )
{
	H h;
	h_init( &h );

	LayerCfg cfg[2];
	memset( cfg, 0, sizeof(cfg) );

	cfg[0].point_count = 2;                 /* outer */
	cfg[0].times[0] = R(0,1);  cfg[0].values[0] = R(1,1);
	cfg[0].times[1] = R(1,1);  cfg[0].values[1] = R(3,1);

	cfg[1].point_count = 2;                 /* inner */
	cfg[1].times[0] = R(0,1);  cfg[1].values[0] = R(2,1);
	cfg[1].times[1] = R(1,1);  cfg[1].values[1] = R(4,1);

	int64_t fillers[2] = { 20, 0 };
	int64_t lens[2]    = { 100, 40 };

	aafiAudioClip *c = build_layer_stack_f( &h, cfg, fillers, 2, lens, 40 );
	TASK_CHECK( aafi_clip_gain_layer_count( c ) == 2 );

	double m = 0.0;
	TASK_CHECK( gain_rc( c, 10, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 4.0 ) );

	TASK_CHECK( gain_rc( c, 20, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 5.4 ) );

	/* offset 30: outer 50/100 -> 2.0, inner 30/40=.75 -> 3.5 => 7.0 */
	TASK_CHECK( gain_rc( c, 30, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 7.0 ) );

	/* repeat / out-of-order calls are stateless */
	double m1 = 0, m2 = 0;
	TASK_CHECK( gain_rc( c, 37, 1, &m1 ) == 0 );
	TASK_CHECK( gain_rc( c, 3, 1, &m2 ) == 0 );
	TASK_CHECK( gain_rc( c, 37, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, m1 ) );
	TASK_CHECK( m2 != m1 );

	h_destroy( &h );
}


static void run_curves_rational_spellings( void )
{
	H h;
	h_init( &h );

	LayerCfg cfg[1];
	memset( cfg, 0, sizeof(cfg) );
	cfg[0].point_count = 3;
	cfg[0].times[0] = R(0,1);  cfg[0].values[0] = R(1,1);
	cfg[0].times[1] = R(2,4);  cfg[0].values[1] = R(3,1);  /* 1/2, differently written */
	cfg[0].times[2] = R(1,1);  cfg[0].values[2] = R(5,1);

	aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
	                                         (int64_t[]){ 40 }, 40 );

	double m = 0;

	/* T = 10/40 = 1/4: halfway 1 -> 3 => 2 */
	TASK_CHECK( gain_rc( c, 10, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.0 ) );

	/* T = 20/40 = 1/2 exactly hits the differently written point => 3 */
	TASK_CHECK( gain_rc( c, 20, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 3.0 ) );

	/* same offset written as an equivalent rational must give same answer */
	double m_eq = -99;
	TASK_CHECK( gain_rc( c, 40, 2, &m_eq ) == 0 ); /* 40/2 = 20 */
	TASK_CHECK( near_mul( m_eq, 3.0 ) );

	h_destroy( &h );
}


static void run_curves_constant_interp( void )
{
	H h;
	h_init( &h );

	LayerCfg cfg[1];
	memset( cfg, 0, sizeof(cfg) );
	cfg[0].interp = 1; /* Constant */
	cfg[0].point_count = 3;
	cfg[0].times[0] = R(0,1); cfg[0].values[0] = R(1,1);
	cfg[0].times[1] = R(1,2); cfg[0].values[1] = R(4,1);
	cfg[0].times[2] = R(1,1); cfg[0].values[2] = R(2,1);

	aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
	                                         (int64_t[]){ 40 }, 40 );
	double m = 0;

	TASK_CHECK( gain_rc( c, 10, 1, &m ) == 0 );  /* T=.25 left of .5 */
	TASK_CHECK( near_mul( m, 1.0 ) );

	TASK_CHECK( gain_rc( c, 20, 1, &m ) == 0 );  /* exact breakpoint -> right */
	TASK_CHECK( near_mul( m, 4.0 ) );

	TASK_CHECK( gain_rc( c, 30, 1, &m ) == 0 );  /* holds right value */
	TASK_CHECK( near_mul( m, 4.0 ) );

	h_destroy( &h );

	/* endpoint extension with points not containing 0 or 1 */
	H h2;
	h_init( &h2 );

	LayerCfg cfg2[1];
	memset( cfg2, 0, sizeof(cfg2) );
	cfg2[0].interp = 1;
	cfg2[0].point_count = 2;
	cfg2[0].times[0] = R(1,4); cfg2[0].values[0] = R(7,1);
	cfg2[0].times[1] = R(3,4); cfg2[0].values[1] = R(9,1);

	aafiAudioClip *c2 = build_layer_stack_f( &h2, cfg2, NULL, 1,
	                                          (int64_t[]){ 40 }, 40 );

	TASK_CHECK( gain_rc( c2, 0, 1, &m ) == 0 );   /* before first: extend 7 */
	TASK_CHECK( near_mul( m, 7.0 ) );
	TASK_CHECK( gain_rc( c2, 10, 1, &m ) == 0 );  /* exact 1/4 -> 7 */
	TASK_CHECK( near_mul( m, 7.0 ) );
	TASK_CHECK( gain_rc( c2, 20, 1, &m ) == 0 );  /* between: keep left 7 */
	TASK_CHECK( near_mul( m, 7.0 ) );
	TASK_CHECK( gain_rc( c2, 30, 1, &m ) == 0 );  /* exact 3/4 -> 9 */
	TASK_CHECK( near_mul( m, 9.0 ) );
	TASK_CHECK( gain_rc( c2, 39, 1, &m ) == 0 );  /* after: extend 9 */
	TASK_CHECK( near_mul( m, 9.0 ) );

	h_destroy( &h2 );
}


static void run_curves_fractional_and_linear_extend( void )
{
	H h;
	h_init( &h );

	/* Linear curve .25 -> 2, .75 -> 4, endpoints extended. */
	LayerCfg cfg[1];
	memset( cfg, 0, sizeof(cfg) );
	cfg[0].point_count = 2;
	cfg[0].times[0] = R(1,4); cfg[0].values[0] = R(2,1);
	cfg[0].times[1] = R(3,4); cfg[0].values[1] = R(4,1);

	aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
	                                         (int64_t[]){ 40 }, 40 );
	double m = 0;

	TASK_CHECK( gain_rc( c, 0, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 2.0 ) );             /* extend first */

	TASK_CHECK( gain_rc( c, 20, 1, &m ) == 0 );  /* T=.5 midpoint => 3 */
	TASK_CHECK( near_mul( m, 3.0 ) );

	TASK_CHECK( gain_rc( c, 39, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 4.0 ) );             /* extend last */

	/* fractional offset 1/2 at clip position 19.5 => T=.4875:
	 * between .25 and .75, f=(.4875-.25)/.5=.475 => 2+.475*2=2.95 */
	TASK_CHECK( gain_rc( c, 39, 2, &m ) == 0 );   /* 39/2 = 19.5 */
	TASK_CHECK( near_mul( m, 2.95 ) );

	h_destroy( &h );
}


static void run_curves_large_position( void )
{
	H h;
	h_init( &h );

	/*
	 * Large group: length 3,000,001, breakpoint at t=1/3.
	 * Group coordinate at the breakpoint is 1,000,000 + 1/3 exactly.
	 * A 1,000,000 integer offset plus a 1/3 fraction must compare equal
	 * to the breakpoint using exact rational arithmetic.
	 */
	LayerCfg cfg[1];
	memset( cfg, 0, sizeof(cfg) );
	cfg[0].interp = 1; /* Constant: discrete right-value at breakpoint */
	cfg[0].point_count = 2;
	cfg[0].times[0] = R(0,1); cfg[0].values[0] = R(1,1);
	cfg[0].times[1] = R(1,3); cfg[0].values[1] = R(8,1);

	int64_t lens[1] = { 3000001 };
	int64_t fillers[1] = { 1000000 };

	aafiAudioClip *c = build_layer_stack_f( &h, cfg, fillers, 1, lens, 2000001 );

	double m = 0;

	/* offset 1/3: exact breakpoint -> right value 8 */
	TASK_CHECK( gain_rc( c, 1, 3, &m ) == 0 );
	TASK_CHECK( near_mul( m, 8.0 ) );

	/* just before, offset 0 -> left value 1 */
	TASK_CHECK( gain_rc( c, 0, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 1.0 ) );

	h_destroy( &h );

	/*
	 * Large integer position plus a tiny fraction. The 1/2 breakpoint of a
	 * length 2,000,000,000 group sits at coordinate 1,000,000,000. With a
	 * 999,999,999 offset inside the group, clip offset 1 reaches it exactly
	 * while 999999999/1000000000 sits 1e-9 before it - below double ULP at
	 * 1e9, so a (double)d + fraction projection would eat it. Exact rational
	 * comparison must still select the left Constant-interpolation value.
	 */
	H h2;
	h_init( &h2 );

	LayerCfg cfg2;
	memset( &cfg2, 0, sizeof(cfg2) );
	cfg2.interp = 1; /* Constant: discrete flip at the breakpoint */
	cfg2.point_count = 2;
	cfg2.times[0] = R(0,1); cfg2.values[0] = R(1,1);
	cfg2.times[1] = R(1,2); cfg2.values[1] = R(8,1);

	int64_t lens2[1]    = { 2000000000 };
	int64_t fillers2[1] = { 999999999 };

	aafiAudioClip *c2 = build_layer_stack_f( &h2, &cfg2, fillers2, 1, lens2, 1000000001 );

	/* tiny step just before the breakpoint -> left value */
	TASK_CHECK( gain_rc( c2, 999999999, 1000000000, &m ) == 0 );
	TASK_CHECK( near_mul( m, 1.0 ) );

	/* exact breakpoint -> right value */
	TASK_CHECK( gain_rc( c2, 1, 1, &m ) == 0 );
	TASK_CHECK( near_mul( m, 8.0 ) );

	h_destroy( &h2 );
}


static void run_curves_illegal( void )
{
	/* Each broken graph must publish a counted but invalid layer, and
	 * aafi_clip_gain_at must fail without touching multiplier. */

	/* 1: zero group length */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].constant = 1; cfg[0].cval = R(2,1);
		cfg[0].zero_length = 1;
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		TASK_CHECK( aafi_clip_gain_layer_count( c ) == 1 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 2: varying with no control points */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].omit_point_list = 1;
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		TASK_CHECK( aafi_clip_gain_layer_count( c ) == 1 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 3: duplicate times, written differently (1/2 and 2/4) */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].point_count = 2;
		cfg[0].times[0] = R(1,2); cfg[0].values[0] = R(1,1);
		cfg[0].times[1] = R(2,4); cfg[0].values[1] = R(2,1);
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 4: reversed times */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].point_count = 2;
		cfg[0].times[0] = R(1,1); cfg[0].values[0] = R(1,1);
		cfg[0].times[1] = R(0,1); cfg[0].values[1] = R(2,1);
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 5: illegal denominator (time denominator 0) */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].point_count = 2;
		cfg[0].times[0] = R(0,1); cfg[0].values[0] = R(1,1);
		cfg[0].times[1] = R(1,0); cfg[0].values[1] = R(2,1);
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 6: negative value */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].constant = 1;
		cfg[0].cval = R( -3, 1 );
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}

	/* 7: unsupported (Power) interpolation */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].interp = 2;
		cfg[0].point_count = 2;
		cfg[0].times[0] = R(0,1); cfg[0].values[0] = R(1,1);
		cfg[0].times[1] = R(1,1); cfg[0].values[1] = R(2,1);
		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 40 }, 40 );
		double sentinel = -77.0;
		TASK_CHECK( gain_rc( c, 1, 1, &sentinel ) != 0 );
		TASK_CHECK( sentinel == -77.0 );
		h_destroy( &h );
	}
}


/* ------------------------------------------------------------------ */
/* lifetime                                                            */
/* ------------------------------------------------------------------ */

static aafiAudioClip *build_plain_clip( H *h )
{
	aafObject *clip = make_source_clip( h, 16, 0, &h->master_id );
	aafObject *seq = make_sequence( h, (aafObject *[]){ clip }, 1 );
	h_finish_mobs( h, seq );
	TASK_CHECK( aafi_retrieveData( h->aafi ) == 0 );
	aafiAudioTrack *t = h->aafi->Audio->Tracks;
	TASK_CHECK( t != NULL );
	return aafi_timelineItemToAudioClip( t->timelineItems );
}


static void run_lifetime_sibling_release( int remove_first )
{
	H h;
	h_init( &h );

	/* same numeric nested graph as scope */
	LayerCfg inner;
	memset( &inner, 0, sizeof(inner) );
	inner.point_count = 2;
	inner.times[0] = R(0,1); inner.values[0] = R(2,1);
	inner.times[1] = R(1,1); inner.values[1] = R(4,1);

	aafObject *clip_a = make_source_clip( &h, 40, 0, &h.master_id );
	aafObject *inner_g = make_gain_og( &h, &inner, 40 );
	aafObject *ii[] = { clip_a };
	tb_vec( &h, inner_g, PID_OperationGroup_InputSegments, ii, 1 );

	LayerCfg outer;
	memset( &outer, 0, sizeof(outer) );
	outer.point_count = 2;
	outer.times[0] = R(0,1); outer.values[0] = R(1,1);
	outer.times[1] = R(1,1); outer.values[1] = R(3,1);

	aafObject *clip_b = make_source_clip( &h, 40, 0, &h.master_id );
	aafObject *seq = make_sequence(
		&h, (aafObject *[]){ make_filler( &h, 20 ), inner_g, clip_b }, 3 );

	aafObject *outer_g = make_gain_og( &h, &outer, 100 );
	aafObject *oi[] = { seq };
	tb_vec( &h, outer_g, PID_OperationGroup_InputSegments, oi, 1 );

	h_finish_mobs( &h, outer_g );
	TASK_CHECK( aafi_retrieveData( h.aafi ) == 0 );

	aafiAudioClip **clips = NULL;
	unsigned n = collect_clips( h.aafi, &clips );
	TASK_CHECK( n == 2 );

	aafiAudioClip *a = clips[0];
	aafiAudioClip *b = clips[1];
	free( clips );

	double m = 0;
	TASK_CHECK( gain_rc( a, 10, 1, &m ) == 0 && near_mul( m, 4.0 ) );
	TASK_CHECK( gain_rc( b, 0, 1, &m ) == 0 && near_mul( m, 2.2 ) );

	aafiAudioClip *victim   = remove_first ? a : b;
	aafiAudioClip *survivor = remove_first ? b : a;

	TASK_CHECK( aafi_removeTimelineItem( h.aafi, victim->timelineItem ) == 0 );

	/* survivor still evaluates exactly as before */
	if ( remove_first ) {
		TASK_CHECK( gain_rc( survivor, 0, 1, &m ) == 0 );
		TASK_CHECK( near_mul( m, 2.2 ) );
	}
	else {
		TASK_CHECK( gain_rc( survivor, 10, 1, &m ) == 0 );
		TASK_CHECK( near_mul( m, 4.0 ) );
		TASK_CHECK( gain_rc( survivor, 20, 1, &m ) == 0 );
		TASK_CHECK( near_mul( m, 5.4 ) );
	}

	/* repeated evaluation after scope exit / removal stays stable */
	double again = 0;
	TASK_CHECK( aafi_clip_gain_at( survivor, R(12,1), &m ) == 0 );
	TASK_CHECK( aafi_clip_gain_at( survivor, R(12,1), &again ) == 0 );
	TASK_CHECK( m == again );

	h_destroy( &h );
}


static void run_lifetime_no_gain_and_compat( void )
{
	/* no gain layers -> count 0, unity gain, strict arguments */
	{
		H h; h_init( &h );
		aafiAudioClip *c = build_plain_clip( &h );
		TASK_CHECK( c != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( c ) == 0 );

		double m = 999.0;
		TASK_CHECK( aafi_clip_gain_at( c, R(0,1), &m ) == 0 );
		TASK_CHECK( m == 1.0 );

		TASK_CHECK( aafi_clip_gain_at( NULL, R(0,1), &m ) != 0 );
		TASK_CHECK( aafi_clip_gain_at( c, R(0,1), NULL ) != 0 );

		double sentinel = 5.5;
		TASK_CHECK( aafi_clip_gain_at( c, R(0,0), &sentinel ) != 0 );
		TASK_CHECK( sentinel == 5.5 );

		TASK_CHECK( aafi_clip_gain_at( c, R(-1,1), &sentinel ) != 0 );
		TASK_CHECK( sentinel == 5.5 );

		TASK_CHECK( aafi_clip_gain_at( c, R(16,1), &sentinel ) != 0 );
		TASK_CHECK( sentinel == 5.5 );

		h_destroy( &h );
	}

	/* legacy single constant gain field stays usable */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].constant = 1;
		cfg[0].cval = R( 3, 2 );

		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 16 }, 16 );
		TASK_CHECK( c->gain != NULL );
		TASK_CHECK( c->gain->pts_cnt == 1 );
		TASK_CHECK( c->gain->value != NULL );
		TASK_CHECK( c->gain->value[0].numerator == 3 );
		TASK_CHECK( c->gain->value[0].denominator == 2 );

		double m = 0;
		TASK_CHECK( gain_rc( c, 5, 1, &m ) == 0 );
		TASK_CHECK( near_mul( m, 1.5 ) );

		h_destroy( &h );
	}

	/* legacy single automation field stays usable */
	{
		H h; h_init( &h );
		LayerCfg cfg[1]; memset( cfg, 0, sizeof(cfg) );
		cfg[0].point_count = 2;
		cfg[0].times[0] = R(0,1); cfg[0].values[0] = R(1,1);
		cfg[0].times[1] = R(1,1); cfg[0].values[1] = R(3,1);

		aafiAudioClip *c = build_layer_stack_f( &h, cfg, NULL, 1,
		                                         (int64_t[]){ 16 }, 16 );
		TASK_CHECK( c->automation != NULL );
		TASK_CHECK( c->automation->pts_cnt >= 2 );
		TASK_CHECK( c->automation->time != NULL );
		TASK_CHECK( c->automation->value != NULL );

		h_destroy( &h );
	}
}


static void run_lifetime_track_fader( void )
{
	H h;
	h_init( &h );

	/* TMS -> track MonoAudioGain(0.5) -> Sequence -> one clip. */
	aafObject *clip = make_source_clip( &h, 16, 0, &h.master_id );
	aafObject *seq  = make_sequence( &h, (aafObject *[]){ clip }, 1 );

	LayerCfg cfg;
	memset( &cfg, 0, sizeof(cfg) );
	cfg.constant = 1;
	cfg.cval = R( 1, 2 );

	aafObject *fader = make_gain_og( &h, &cfg, 16 );
	aafObject *inputs[] = { seq };
	tb_vec( &h, fader, PID_OperationGroup_InputSegments, inputs, 1 );

	h_finish_segment( &h, fader );

	TASK_CHECK( aafi_retrieveData( h.aafi ) == 0 );

	aafiAudioTrack *t = h.aafi->Audio->Tracks;
	TASK_CHECK( t != NULL );
	TASK_CHECK( t->gain != NULL ); /* legacy track fader path */

	aafiAudioClip *c = aafi_timelineItemToAudioClip( t->timelineItems );
	TASK_CHECK( c != NULL );

	/* track fader is not a clip layer and is not multiplied in */
	TASK_CHECK( aafi_clip_gain_layer_count( c ) == 0 );
	double m = 77.0;
	TASK_CHECK( gain_rc( c, 4, 1, &m ) == 0 );
	TASK_CHECK( m == 1.0 );

	h_destroy( &h );
}


static int file_readable( const char *path )
{
	FILE *f = fopen( path, "rb" );
	if ( f ) { fclose( f ); return 1; }
	return 0;
}


static void run_lifetime_real_file( void )
{
	const char *candidates[] = {
		"test/aaf/DR_Audio_Levels.aaf",
		"../test/aaf/DR_Audio_Levels.aaf",
		NULL
	};

	const char *path = NULL;
	for ( int i = 0; candidates[i]; i++ ) {
		if ( file_readable( candidates[i] ) ) { path = candidates[i]; break; }
	}

	/* The shipped file must be reachable from the task runner cwd. */
	TASK_CHECK( path != NULL );
	if ( !path ) {
		return;
	}

	AAF_Iface *aafi = aafi_alloc( NULL );
	TASK_CHECK( aafi != NULL );
	TASK_CHECK( aafi_load_file( aafi, path ) == 0 );

	unsigned tracks = 0, clips = 0, gains = 0, autos = 0, layered = 0;

	for ( aafiAudioTrack *t = aafi->Audio->Tracks; t; t = t->next ) {
		tracks++;
		for ( aafiTimelineItem *it = t->timelineItems; it; it = it->next ) {
			aafiAudioClip *c = aafi_timelineItemToAudioClip( it );
			if ( !c ) { continue; }
			clips++;

			if ( c->gain ) {
				gains++;
				TASK_CHECK( c->gain->pts_cnt > 0 );
				TASK_CHECK( c->gain->value != NULL );
			}
			if ( c->automation ) {
				autos++;
				TASK_CHECK( c->automation->pts_cnt > 0 );
				TASK_CHECK( c->automation->time != NULL );
				TASK_CHECK( c->automation->value != NULL );
			}

			size_t lc = aafi_clip_gain_layer_count( c );
			if ( lc > 0 ) { layered++; }

			/* API must evaluate everywhere, repeatedly and finitely. */
			for ( int64_t o = 0; o < c->len; o += ( c->len > 8 ? c->len / 8 : 1 ) ) {
				double m = -1.0;
				int rc = aafi_clip_gain_at( c, R( (int32_t)o, 1 ), &m );
				TASK_CHECK( rc == 0 );
				TASK_CHECK( isfinite( m ) );
				TASK_CHECK( m >= 0.0 );

				double m2 = -2.0;
				TASK_CHECK( aafi_clip_gain_at( c, R( (int32_t)o, 1 ), &m2 ) == 0 );
				TASK_CHECK( m == m2 );
			}
		}
	}

	TASK_CHECK( tracks == 2 );
	TASK_CHECK( clips == 8 );
	TASK_CHECK( gains == 6 );
	TASK_CHECK( autos == 4 );
	TASK_CHECK( layered == 8 );

	aafi_release( &aafi );
	TASK_CHECK( aafi == NULL );
}


int run_gain_layers_tests( const char *group )
{
	if ( strcmp( group, "scope" ) == 0 ) {
		run_scope();
		return 0;
	}

	if ( strcmp( group, "curves" ) == 0 ) {
		run_curves_product();
		run_curves_rational_spellings();
		run_curves_constant_interp();
		run_curves_fractional_and_linear_extend();
		run_curves_large_position();
		run_curves_illegal();
		return 0;
	}

	if ( strcmp( group, "lifetime" ) == 0 ) {
		run_lifetime_sibling_release( 1 );
		run_lifetime_sibling_release( 0 );
		run_lifetime_no_gain_and_compat();
		run_lifetime_track_fader();
		run_lifetime_real_file();
		return 0;
	}

	return 1;
}
