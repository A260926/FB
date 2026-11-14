/*
 * Nested clip gain layer tests.
 *
 * Three groups :
 *   - scope    : real aafi_retrieveData() imports of in-memory AAF graphs with
 *                nested MonoAudioGain OperationGroups, leading Filler and
 *                sibling clips.
 *   - curves   : exact rational evaluation, linear/constant interpolation,
 *                malformed curves and out-of-range queries.
 *   - lifetime : independent layer storage, sibling release ordering, legacy
 *                field compatibility and the real DR_Audio_Levels.aaf file.
 */

#include <math.h>
#include <string.h>

#include <libaaf.h>

#include "check.h"
#include "memgraph.h"
#include "../src/AAFIface/AAFGainLayersInternal.h"


static int
near_enough( double got, double expected )
{
	double tol = 1e-9 * ( fabs(expected) > 1.0 ? fabs(expected) : 1.0 );

	return fabs( got - expected ) <= tol;
}


static AAF_Iface *
mg_import( memgraph *g )
{
	AAF_Iface *aafi = aafi_alloc( g->aafd );

	if ( !aafi ) {
		return NULL;
	}

	int rc = aafi_retrieveData( aafi );

	if ( rc != 0 ) {
		aafi_release( &aafi );
		return NULL;
	}

	return aafi;
}


static aafiAudioClip *
clip_at_index( AAF_Iface *aafi, unsigned index )
{
	unsigned n = 0;
	aafiAudioTrack *t = NULL;

	AAFI_foreachAudioTrack( aafi, t ) {

		aafiTimelineItem *it = NULL;

		AAFI_foreachTrackItem( t, it ) {

			aafiAudioClip *c = aafi_timelineItemToAudioClip( it );

			if ( c && n++ == index ) {
				return c;
			}
		}
	}

	return NULL;
}


static unsigned
count_clips( AAF_Iface *aafi )
{
	unsigned n = 0;
	aafiAudioTrack *t = NULL;

	AAFI_foreachAudioTrack( aafi, t ) {

		aafiTimelineItem *it = NULL;

		AAFI_foreachTrackItem( t, it ) {
			if ( aafi_timelineItemToAudioClip( it ) ) {
				n++;
			}
		}
	}

	return n;
}


static double
gain( aafiAudioClip *c, int32_t num, int32_t den )
{
	aafRational_t off = { num, den };
	double v = 0.0;

	int rc = aafi_clip_gain_at( c, off, &v );

	TASK_CHECK( rc == 0 );

	return v;
}


/* Build one reusable MasterMob/SourceMob pair, 48 kHz mono PCM. */
static void
build_media( memgraph *g, aafMobID_t *masterID, aafMobID_t *sourceID )
{
	*sourceID = mg_make_mob_id( g );
	*masterID = mg_make_mob_id( g );

	aafObject *srcClip = mg_source_clip( g, NULL, 40, 5, 1, sourceID );
	aafObject *srcMob  = mg_source_mob( g, sourceID, 1, srcClip );

	aafObject *masterClip = mg_source_clip( g, NULL, 40, 5, 1, sourceID );
	aafObject *masterMob  = mg_master_mob( g, masterID, 1, masterClip );

	aafObject *mobs[2] = { srcMob, masterMob };

	mg_set_mobs( g, mobs, 2 );
}


/*
 * Track 1 : the specification example.
 *
 *   outer group L=100, linear 1 -> 3
 *     Sequence: Filler 20, inner group L=40 (linear 2 -> 4), Filler 40
 *       inner Sequence: SourceClip L=40
 */
static aafObject *
build_spec_track( memgraph *g, const aafMobID_t *masterID )
{
	aafRational_t innerT[2] = { {0,1}, {1,1} };
	aafRational_t innerV[2] = { {2,1}, {4,1} };
	aafObject *innerParam = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
	                                        innerT, innerV, 2 );

	aafObject *clipA = mg_source_clip( g, NULL, 40, 5, 1, masterID );
	aafObject *innerSeqComps[1] = { clipA };
	aafObject *innerSeq = mg_sequence( g, NULL, 40, innerSeqComps, 1 );
	aafObject *inner   = mg_op_gain( g, NULL, 40, innerSeq, innerParam );

	aafObject *fillerHead = mg_filler( g, NULL, 20 );
	aafObject *fillerTail = mg_filler( g, NULL, 40 );
	aafObject *outerComps[3] = { fillerHead, inner, fillerTail };
	aafObject *outerSeq = mg_sequence( g, NULL, 100, outerComps, 3 );

	aafRational_t outerT[2] = { {0,1}, {1,1} };
	aafRational_t outerV[2] = { {1,1}, {3,1} };
	aafObject *outerParam = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
	                                        outerT, outerV, 2 );
	aafObject *outer = mg_op_gain( g, NULL, 100, outerSeq, outerParam );

	/* slot segment is a Sequence; the gain group is nested inside it */
	aafObject *trackComps[1] = { outer };
	aafObject *trackSeq = mg_sequence( g, NULL, 100, trackComps, 1 );

	aafRational_t rate = { 48000, 1 };

	return mg_timeline_slot( g, NULL, 1, 1, rate, trackSeq );
}


/*
 * Track 2 : sibling scoping.
 *
 *   outer group L=60, constant 3
 *     Sequence: Filler 10, inner group L=20 (constant 5), clip C L=20, Filler 10
 */
static aafObject *
build_sibling_track( memgraph *g, const aafMobID_t *masterID )
{
	aafRational_t five = { 5, 1 };
	aafObject *innerParam = mg_constant_amp( g, NULL, five );

	aafObject *clipB = mg_source_clip( g, NULL, 20, 5, 1, masterID );
	aafObject *innerSeqComps[1] = { clipB };
	aafObject *innerSeq = mg_sequence( g, NULL, 20, innerSeqComps, 1 );
	aafObject *inner   = mg_op_gain( g, NULL, 20, innerSeq, innerParam );

	aafObject *fillerHead = mg_filler( g, NULL, 10 );
	aafObject *clipC      = mg_source_clip( g, NULL, 20, 5, 1, masterID );
	aafObject *fillerTail = mg_filler( g, NULL, 10 );
	aafObject *comps[4]   = { fillerHead, inner, clipC, fillerTail };
	aafObject *outerSeq   = mg_sequence( g, NULL, 60, comps, 4 );

	aafRational_t three = { 3, 1 };
	aafObject *outerParam = mg_constant_amp( g, NULL, three );
	aafObject *outer = mg_op_gain( g, NULL, 60, outerSeq, outerParam );

	aafObject *trackComps[1] = { outer };
	aafObject *trackSeq = mg_sequence( g, NULL, 60, trackComps, 1 );

	aafRational_t rate = { 48000, 1 };

	return mg_timeline_slot( g, NULL, 2, 2, rate, trackSeq );
}


/* Track 3 : plain clip, no gain layer at all. */
static aafObject *
build_plain_track( memgraph *g, const aafMobID_t *masterID )
{
	aafObject *clip = mg_source_clip( g, NULL, 40, 5, 1, masterID );
	aafObject *seqComps[1] = { clip };
	aafObject *seq  = mg_sequence( g, NULL, 40, seqComps, 1 );
	aafRational_t rate = { 48000, 1 };

	return mg_timeline_slot( g, NULL, 3, 3, rate, seq );
}


/*
 * Track 4 : outermost track fader (OperationGroup directly under the
 * TimelineMobSlot). It must stay on track->gain and never become a clip layer.
 */
static aafObject *
build_track_fader_track( memgraph *g, const aafMobID_t *masterID )
{
	aafObject *clip = mg_source_clip( g, NULL, 16, 0, 1, masterID );
	aafObject *seqComps[1] = { clip };
	aafObject *seq = mg_sequence( g, NULL, 16, seqComps, 1 );

	aafRational_t half = { 1, 2 };
	aafObject *param = mg_constant_amp( g, NULL, half );
	aafObject *fader = mg_op_gain( g, NULL, 16, seq, param );

	aafRational_t rate = { 48000, 1 };

	return mg_timeline_slot( g, NULL, 4, 4, rate, fader );
}


static int
run_scope( void )
{
	memgraph *g = mg_new();

	TASK_CHECK( g != NULL );

	aafMobID_t masterID, sourceID;
	build_media( g, &masterID, &sourceID );

	aafObject *slots[4] = {
		build_spec_track( g, &masterID ),
		build_sibling_track( g, &masterID ),
		build_plain_track( g, &masterID ),
		build_track_fader_track( g, &masterID )
	};

	aafMobID_t compID = mg_make_mob_id( g );
	aafObject *comp = mg_composition( g, &compID, slots, 4 );

	/*
	 * mg_set_mobs() already registered the two media mobs; rebuild the set
	 * with the composition included while preserving those entries.
	 */
	aafObject *allMobs[3];
	allMobs[0] = comp;

	/* recover media mobs through the current set */
	unsigned k = 1;
	for ( aafObject *m = g->aafd->Mobs; m != NULL && k < 3; m = m->next ) {
		allMobs[k++] = m;
	}

	TASK_CHECK( k == 3 );
	mg_set_mobs( g, allMobs, 3 );

	AAF_Iface *aafi = mg_import( g );

	TASK_CHECK( aafi != NULL );
	TASK_CHECK( count_clips( aafi ) == 5 );

	/* clip A : two layers, specification numeric example */
	aafiAudioClip *clipA = clip_at_index( aafi, 0 );

	TASK_CHECK( clipA != NULL );
	TASK_CHECK( aafi_clip_gain_layer_count( clipA ) == 2 );
	TASK_CHECK( near_enough( gain( clipA, 10, 1 ), 4.0 ) );
	TASK_CHECK( near_enough( gain( clipA, 20, 1 ), 5.4 ) );

	/* clip B (nested) then clip C (outer-only sibling) */
	aafiAudioClip *clipB = clip_at_index( aafi, 1 );
	aafiAudioClip *clipC = clip_at_index( aafi, 2 );

	TASK_CHECK( clipB != NULL && clipC != NULL );
	TASK_CHECK( aafi_clip_gain_layer_count( clipB ) == 2 );
	TASK_CHECK( aafi_clip_gain_layer_count( clipC ) == 1 );

	/* B : outer 3 * inner 5 = 15 ; C : outer 3 only, at its own offset */
	TASK_CHECK( near_enough( gain( clipB, 0, 1 ), 15.0 ) );
	TASK_CHECK( near_enough( gain( clipC, 0, 1 ), 3.0 ) );
	TASK_CHECK( near_enough( gain( clipC, 19, 1 ), 3.0 ) );

	/* plain clip : no layer, unity gain */
	aafiAudioClip *plain = clip_at_index( aafi, 3 );

	TASK_CHECK( plain != NULL );
	TASK_CHECK( aafi_clip_gain_layer_count( plain ) == 0 );
	TASK_CHECK( near_enough( gain( plain, 0, 1 ), 1.0 ) );

	/* track fader clip : no clip layer, fader landed on the track */
	aafiAudioClip *faderClip = clip_at_index( aafi, 4 );
	aafiAudioTrack *track4 = NULL;

	AAFI_foreachAudioTrack( aafi, track4 ) {
		if ( track4->number == 4 ) {
			break;
		}
	}

	TASK_CHECK( faderClip != NULL );
	TASK_CHECK( aafi_clip_gain_layer_count( faderClip ) == 0 );
	TASK_CHECK( track4 != NULL && track4->gain != NULL );
	TASK_CHECK( near_enough( gain( faderClip, 0, 1 ), 1.0 ) );

	aafi_release( &aafi );
	mg_free( g );

	return 0;
}


/*
 * Build a single-track composition whose only clip wraps one MonoAudioGain
 * group with the supplied parameter. Used by curve and malformed-curve tests.
 */
static AAF_Iface *
import_single_layer( memgraph *g, aafObject *parameter, int64_t groupLen,
                     aafMobID_t *compID, const aafMobID_t *masterID )
{
	/*
	 * The clip and the wrapping Sequence keep a positive length so the
	 * SourceClip is always created. A zero or negative groupLen only
	 * invalidates the OperationGroup, exercising its own validation.
	 */
	int64_t clipLen = ( groupLen > 0 ) ? groupLen : 10;

	aafObject *clip = mg_source_clip( g, NULL, clipLen, 7, 1, masterID );
	aafObject *seqComps[1] = { clip };
	aafObject *seq = mg_sequence( g, NULL, clipLen, seqComps, 1 );
	aafObject *op  = mg_op_gain( g, NULL, groupLen, seq, parameter );

	/* slot segment is a Sequence; the gain group is nested inside it */
	aafObject *trackComps[1] = { op };
	aafObject *trackSeq = mg_sequence( g, NULL, clipLen, trackComps, 1 );

	aafRational_t rate = { 48000, 1 };
	aafObject *slot = mg_timeline_slot( g, NULL, 1, 1, rate, trackSeq );

	aafObject *slots[1] = { slot };
	*compID = mg_make_mob_id( g );
	aafObject *comp = mg_composition( g, compID, slots, 1 );

	aafObject *allMobs[3];
	allMobs[0] = comp;

	unsigned k = 1;
	for ( aafObject *m = g->aafd->Mobs; m != NULL && k < 3; m = m->next ) {
		allMobs[k++] = m;
	}

	if ( k != 3 ) {
		return NULL;
	}

	mg_set_mobs( g, allMobs, 3 );

	return mg_import( g );
}


static memgraph *
new_media_graph( aafMobID_t *masterID )
{
	memgraph *g = mg_new();

	if ( !g ) {
		return NULL;
	}

	aafMobID_t sourceID;
	build_media( g, masterID, &sourceID );

	return g;
}


static int
run_curves( void )
{
	aafMobID_t compID, masterID;

	/* 1. exact rational equality : point time 1/6 written 2/12 */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t t[3] = { {0,1}, {2,12}, {1,1} };
		aafRational_t v[3] = { {1,1}, {5,1}, {5,1} };
		aafObject *param = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
		                                   t, v, 3 );
		AAF_Iface *aafi = import_single_layer( g, param, 6, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );

		TASK_CHECK( c != NULL && aafi_clip_gain_layer_count( c ) == 1 );

		/*
		 * The 2/12 == 1/6 control point sits at group position
		 * (1/6)*groupLen == 1 edit unit, so offset 1/1 must hit it exactly
		 * even though the point time is written as 2/12. Offset 1/12 (half
		 * an edit unit) is a true fraction between point 0 and that point.
		 */
		{
			double exactPoint = gain( c, 1, 1 );
			double midPoint   = gain( c, 1, 12 );

			/* linear at t=(1/12)/6=1/72 between (0,1) and (1/6,5) */
			double expectedMid = 1.0 + 4.0 * ( ( 1.0 / 72.0 ) / ( 1.0 / 6.0 ) );

			TASK_CHECK( near_enough( exactPoint, 5.0 ) );
			TASK_CHECK( near_enough( midPoint, expectedMid ) );
		}

		aafi_release( &aafi );
		mg_free( g );
	}

	/* 2. constant interpolation breakpoints */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t t[2] = { {1,4}, {3,4} };
		aafRational_t v[2] = { {2,1}, {7,1} };
		aafObject *param = mg_varying_amp( g, NULL, &AAFInterpolationDef_Constant,
		                                   t, v, 2 );
		AAF_Iface *aafi = import_single_layer( g, param, 4, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );

		/*
		 * groupLen = 4, so control points t=1/4 and t=3/4 sit at group
		 * positions 1 and 3 edit units.
		 */

		/* before first point : first value extended */
		TASK_CHECK( near_enough( gain( c, 0, 1 ), 2.0 ) );
		/* strictly inside (offset 1/2 -> t=1/8) : left value held */
		TASK_CHECK( near_enough( gain( c, 1, 2 ), 2.0 ) );
		/* exactly on the left point (offset 1 -> t=1/4) */
		TASK_CHECK( near_enough( gain( c, 1, 1 ), 2.0 ) );
		/* exactly on the right point (offset 3 -> t=3/4) : right value */
		TASK_CHECK( near_enough( gain( c, 3, 1 ), 7.0 ) );
		/* after last point (offset 7/2 -> t=7/8) : last value extended */
		TASK_CHECK( near_enough( gain( c, 7, 2 ), 7.0 ) );

		aafi_release( &aafi );
		mg_free( g );
	}

	/* 3. single control point omitting 0 and 1 : end values extended */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t t[1] = { {1,2} };
		aafRational_t v[1] = { {3,1} };
		aafObject *param = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
		                                   t, v, 1 );
		AAF_Iface *aafi = import_single_layer( g, param, 8, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );

		TASK_CHECK( near_enough( gain( c, 0, 1 ), 3.0 ) );
		TASK_CHECK( near_enough( gain( c, 7, 1 ), 3.0 ) );

		aafi_release( &aafi );
		mg_free( g );
	}

	/* 4. large group length and tiny fractional offset must not be swallowed */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t t[2] = { {0,1}, {1,1} };
		aafRational_t v[2] = { {0,1}, {2000000,1} };
		aafObject *param = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
		                                   t, v, 2 );
		AAF_Iface *aafi = import_single_layer( g, param, 1000000, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );

		double atZero = gain( c, 0, 1 );
		double atThird = gain( c, 1, 3 );

		TASK_CHECK( near_enough( atZero, 0.0 ) );
		TASK_CHECK( near_enough( atThird, 2.0 / 3.0 ) );
		TASK_CHECK( atThird != atZero );

		aafi_release( &aafi );
		mg_free( g );
	}

	/* 5. out-of-range / invalid queries fail and leave the output untouched */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t cv = { 2, 1 };
		aafObject *param = mg_constant_amp( g, NULL, cv );
		AAF_Iface *aafi = import_single_layer( g, param, 10, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );
		aafRational_t badLen   = { 10, 1 };
		aafRational_t negative = { -1, 1 };
		aafRational_t badDen   = { 1, 0 };
		double sentinel = -123.0;
		double out = sentinel;

		TASK_CHECK( aafi_clip_gain_at( c, badLen, &out ) != 0 );
		TASK_CHECK( out == sentinel );
		TASK_CHECK( aafi_clip_gain_at( c, negative, &out ) != 0 );
		TASK_CHECK( out == sentinel );
		TASK_CHECK( aafi_clip_gain_at( c, badDen, &out ) != 0 );
		TASK_CHECK( out == sentinel );
		TASK_CHECK( aafi_clip_gain_at( NULL, badLen, &out ) != 0 );
		TASK_CHECK( out == sentinel );

		aafi_release( &aafi );
		mg_free( g );
	}

	/* 6. out-of-order, repeated queries are stateless and consistent */
	{
		memgraph *g = new_media_graph( &masterID );
		TASK_CHECK( g != NULL );

		aafRational_t t[2] = { {0,1}, {1,1} };
		aafRational_t v[2] = { {1,1}, {9,1} };
		aafObject *param = mg_varying_amp( g, NULL, &AAFInterpolationDef_Linear,
		                                   t, v, 2 );
		AAF_Iface *aafi = import_single_layer( g, param, 10, &compID, &masterID );

		TASK_CHECK( aafi != NULL );

		aafiAudioClip *c = clip_at_index( aafi, 0 );
		double first = gain( c, 5, 1 );

		for ( int i = 0; i < 8; i++ ) {
			TASK_CHECK( near_enough( gain( c, 9, 1 ), 8.2 ) );
			TASK_CHECK( near_enough( gain( c, 1, 3 ), 1.0 + 8.0 / 30.0 ) );
			TASK_CHECK( near_enough( gain( c, 5, 1 ), first ) );
		}

		aafi_release( &aafi );
		mg_free( g );
	}

	/*
	 * 7. malformed curves never get published as gain layers and the strict
	 *    validator rejects every required malformed shape. A structurally
	 *    invalid zero-length OperationGroup still fails the whole import.
	 */

	/* 7a. the strict point-list validator, exercised directly */
	{
		aafRational_t goodT[2] = { {0,1}, {1,1} };
		aafRational_t goodV[2] = { {1,1}, {2,1} };

		TASK_CHECK( aafi_gainLayer_validatePoints( goodT, goodV, 2 ) == 0 );

		/* empty list */
		TASK_CHECK( aafi_gainLayer_validatePoints( NULL, NULL, 0 ) != 0 );

		/* duplicated point times (2/4 == 1/2) */
		aafRational_t dupT[2] = { {1,2}, {2,4} };
		TASK_CHECK( aafi_gainLayer_validatePoints( dupT, goodV, 2 ) != 0 );

		/* out-of-order times */
		aafRational_t orderT[2] = { {1,1}, {1,2} };
		TASK_CHECK( aafi_gainLayer_validatePoints( orderT, goodV, 2 ) != 0 );

		/* time below 0 and above 1 */
		aafRational_t negT[2] = { { -1, 2 }, { 1, 1 } };
		aafRational_t bigT[2] = { { 0, 1 }, { 3, 2 } };
		TASK_CHECK( aafi_gainLayer_validatePoints( negT, goodV, 2 ) != 0 );
		TASK_CHECK( aafi_gainLayer_validatePoints( bigT, goodV, 2 ) != 0 );

		/* zero denominators */
		aafRational_t zeroDenT[2] = { { 0, 1 }, { 1, 0 } };
		aafRational_t zeroDenV[2] = { { 1, 1 }, { 2, 0 } };
		TASK_CHECK( aafi_gainLayer_validatePoints( zeroDenT, goodV, 2 ) != 0 );
		TASK_CHECK( aafi_gainLayer_validatePoints( goodT, zeroDenV, 2 ) != 0 );

		/* negative amplitude */
		aafRational_t negV[2] = { { -1, 1 }, { 2, 1 } };
		TASK_CHECK( aafi_gainLayer_validatePoints( goodT, negV, 2 ) != 0 );
	}

	/*
	 * 7b. through the real import chain, a malformed curve must not produce
	 *     any gain layer, while the clip itself still imports successfully.
	 */
	{
		/* empty control point list */
		memgraph *g1 = new_media_graph( &masterID );
		TASK_CHECK( g1 != NULL );
		aafObject *p1 = mg_varying_amp( g1, NULL, &AAFInterpolationDef_Linear,
		                                NULL, NULL, 0 );
		AAF_Iface *a1 = import_single_layer( g1, p1, 10, &compID, &masterID );
		TASK_CHECK( a1 != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( clip_at_index( a1, 0 ) ) == 0 );
		aafi_release( &a1 );
		mg_free( g1 );

		/* duplicated point times (2/4 == 1/2) */
		memgraph *g2 = new_media_graph( &masterID );
		TASK_CHECK( g2 != NULL );
		aafRational_t dt[2] = { {1,2}, {2,4} };
		aafRational_t dv[2] = { {1,1}, {2,1} };
		aafObject *p2 = mg_varying_amp( g2, NULL, &AAFInterpolationDef_Linear,
		                                dt, dv, 2 );
		AAF_Iface *a2 = import_single_layer( g2, p2, 10, &compID, &masterID );
		TASK_CHECK( a2 != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( clip_at_index( a2, 0 ) ) == 0 );
		aafi_release( &a2 );
		mg_free( g2 );

		/* zero denominator */
		memgraph *g3 = new_media_graph( &masterID );
		TASK_CHECK( g3 != NULL );
		aafRational_t zt[2] = { {0,1}, {1,1} };
		aafRational_t zv[2] = { {1,1}, {2,0} };
		aafObject *p3 = mg_varying_amp( g3, NULL, &AAFInterpolationDef_Linear,
		                                zt, zv, 2 );
		AAF_Iface *a3 = import_single_layer( g3, p3, 10, &compID, &masterID );
		TASK_CHECK( a3 != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( clip_at_index( a3, 0 ) ) == 0 );
		aafi_release( &a3 );
		mg_free( g3 );

		/* unsupported interpolation (Log) */
		memgraph *g4 = new_media_graph( &masterID );
		TASK_CHECK( g4 != NULL );
		aafRational_t lt[2] = { {0,1}, {1,1} };
		aafRational_t lv[2] = { {1,1}, {2,1} };
		aafObject *p4 = mg_varying_amp( g4, NULL, &AAFInterpolationDef_Log,
		                                lt, lv, 2 );
		AAF_Iface *a4 = import_single_layer( g4, p4, 10, &compID, &masterID );
		TASK_CHECK( a4 != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( clip_at_index( a4, 0 ) ) == 0 );
		aafi_release( &a4 );
		mg_free( g4 );

		/* negative amplitude constant */
		memgraph *g5 = new_media_graph( &masterID );
		TASK_CHECK( g5 != NULL );
		aafRational_t neg = { -1, 1 };
		aafObject *p5 = mg_constant_amp( g5, NULL, neg );
		AAF_Iface *a5 = import_single_layer( g5, p5, 10, &compID, &masterID );
		TASK_CHECK( a5 != NULL );
		TASK_CHECK( aafi_clip_gain_layer_count( clip_at_index( a5, 0 ) ) == 0 );
		aafi_release( &a5 );
		mg_free( g5 );

		/* zero-length OperationGroup is structurally invalid : import fails */
		memgraph *g0 = new_media_graph( &masterID );
		TASK_CHECK( g0 != NULL );
		aafRational_t cv = { 1, 1 };
		aafObject *p0 = mg_constant_amp( g0, NULL, cv );
		AAF_Iface *a0 = import_single_layer( g0, p0, 0, &compID, &masterID );
		TASK_CHECK( a0 == NULL );
		mg_free( g0 );
	}

	return 0;
}


static int
run_lifetime( void )
{
	/* 1. nested graph, then sibling release order and legacy fields */
	memgraph *g = mg_new();

	TASK_CHECK( g != NULL );

	aafMobID_t masterID, sourceID;
	build_media( g, &masterID, &sourceID );

	aafObject *slots[2] = {
		build_spec_track( g, &masterID ),
		build_sibling_track( g, &masterID )
	};

	aafMobID_t compID = mg_make_mob_id( g );
	aafObject *comp = mg_composition( g, &compID, slots, 2 );

	aafObject *allMobs[3];
	allMobs[0] = comp;

	unsigned k = 1;
	for ( aafObject *m = g->aafd->Mobs; m != NULL && k < 3; m = m->next ) {
		allMobs[k++] = m;
	}

	TASK_CHECK( k == 3 );
	mg_set_mobs( g, allMobs, 3 );

	AAF_Iface *aafi = mg_import( g );

	TASK_CHECK( aafi != NULL );

	aafiAudioClip *clipA = clip_at_index( aafi, 0 );
	aafiAudioClip *clipB = clip_at_index( aafi, 1 );
	aafiAudioClip *clipC = clip_at_index( aafi, 2 );

	TASK_CHECK( clipA && clipB && clipC );

	double savedA = gain( clipA, 10, 1 );
	double savedB = gain( clipB, 0, 1 );

	TASK_CHECK( near_enough( savedA, 4.0 ) );
	TASK_CHECK( near_enough( savedB, 15.0 ) );

	/* the outermost constant/automation still feeds the legacy fields */
	TASK_CHECK( clipB->gain != NULL || clipB->automation != NULL );

	/* release the outer-only sibling first; nested clips must stay valid */
	aafiTimelineItem *itemC = clipC->timelineItem;

	TASK_CHECK( aafi_removeTimelineItem( aafi, itemC ) == 0 );

	TASK_CHECK( near_enough( gain( clipA, 20, 1 ), 5.4 ) );
	TASK_CHECK( near_enough( gain( clipB, 0, 1 ), savedB ) );
	TASK_CHECK( aafi_clip_gain_layer_count( clipA ) == 2 );
	TASK_CHECK( aafi_clip_gain_layer_count( clipB ) == 2 );

	aafi_release( &aafi );
	mg_free( g );

	/*
	 * 2. two independent imports : tearing down a second graph must not
	 * affect the first one's independently owned layers.
	 */
	aafMobID_t m1, m2;

	memgraph *g1 = new_media_graph( &m1 );
	TASK_CHECK( g1 != NULL );
	aafObject *slot1[1] = { build_spec_track( g1, &m1 ) };
	aafMobID_t c1id = mg_make_mob_id( g1 );
	aafObject *comp1 = mg_composition( g1, &c1id, slot1, 1 );
	aafObject *mobs1[3] = { comp1, g1->aafd->Mobs, g1->aafd->Mobs->next };
	mg_set_mobs( g1, mobs1, 3 );

	AAF_Iface *aafi1 = mg_import( g1 );

	TASK_CHECK( aafi1 != NULL );

	aafiAudioClip *keep = clip_at_index( aafi1, 0 );
	double keepVal = gain( keep, 10, 1 );

	memgraph *g2 = new_media_graph( &m2 );
	TASK_CHECK( g2 != NULL );
	aafObject *slot2[1] = { build_plain_track( g2, &m2 ) };
	aafMobID_t c2id = mg_make_mob_id( g2 );
	aafObject *comp2 = mg_composition( g2, &c2id, slot2, 1 );
	aafObject *mobs2[3] = { comp2, g2->aafd->Mobs, g2->aafd->Mobs->next };
	mg_set_mobs( g2, mobs2, 3 );

	AAF_Iface *aafi2 = mg_import( g2 );

	TASK_CHECK( aafi2 != NULL );
	TASK_CHECK( near_enough( gain( keep, 10, 1 ), keepVal ) );
	TASK_CHECK( near_enough( gain( keep, 20, 1 ), 5.4 ) );

	aafi_release( &aafi2 );
	mg_free( g2 );

	TASK_CHECK( near_enough( gain( keep, 10, 1 ), keepVal ) );

	aafi_release( &aafi1 );
	mg_free( g1 );

	/* 3. real Resolve file : nested layers, legacy fields and full release */
	AAF_Iface *dr = aafi_alloc( NULL );

	TASK_CHECK( dr != NULL );
	TASK_CHECK( aafi_load_file( dr, "test/aaf/DR_Audio_Levels.aaf" ) == 0 );

	unsigned drClips = 0;
	unsigned drLayerClips = 0;
	unsigned legacyGains = 0;
	unsigned legacyAutomation = 0;
	aafiAudioTrack *t = NULL;

	AAFI_foreachAudioTrack( dr, t ) {

		aafiTimelineItem *it = NULL;

		AAFI_foreachTrackItem( t, it ) {

			aafiAudioClip *c = aafi_timelineItemToAudioClip( it );

			if ( !c ) {
				continue;
			}

			drClips++;

			if ( c->gain ) {
				legacyGains++;
			}

			if ( c->automation ) {
				legacyAutomation++;
			}

			if ( aafi_clip_gain_layer_count( c ) > 0 ) {
				drLayerClips++;
			}

			double v = 0.0;
			aafRational_t off = { 0, 1 };

			TASK_CHECK( aafi_clip_gain_at( c, off, &v ) == 0 );
			TASK_CHECK( isfinite( v ) );
		}
	}

	TASK_CHECK( drClips == 8 );
	TASK_CHECK( legacyGains == 6 );
	TASK_CHECK( legacyAutomation == 4 );
	TASK_CHECK( drLayerClips > 0 );

	aafi_release( &dr );
	TASK_CHECK( dr == NULL );

	return 0;
}


int
run_gain_layers_tests( const char *group )
{
	if ( strcmp( group, "scope" ) == 0 ) {
		return run_scope();
	}

	if ( strcmp( group, "curves" ) == 0 ) {
		return run_curves();
	}

	if ( strcmp( group, "lifetime" ) == 0 ) {
		return run_lifetime();
	}

	return -1;
}
