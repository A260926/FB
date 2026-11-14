/*
 * Test-only in-memory AAF object graph builder.
 *
 * It assembles the same aafObject / aafProperty structures that LibCFB and
 * AAFCore would produce from a real .aaf Compound File, but without any file
 * or serialization. The resulting AAF_Data is fed to the real parser through
 * aafi_retrieveData(), so the gain-layer tests exercise the actual import
 * chain rather than hand-filled clip structures.
 *
 * Everything allocated here is reachable from AAF_Data.Objects / Classes and
 * is released by aaf_release().
 */

#ifndef TASK_SUPPORT_MEMGRAPH_H
#define TASK_SUPPORT_MEMGRAPH_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <libaaf/AAFCore.h>
#include <libaaf/AAFDefs/AAFClassDefUIDs.h>
#include <libaaf/AAFDefs/AAFPropertyIDs.h>
#include <libaaf/AAFDefs/AAFTypeDefUIDs.h>
#include <libaaf/AAFDefs/AAFDataDefs.h>
#include <libaaf/AAFDefs/AAFOperationDefs.h>
#include <libaaf/AAFDefs/AAFInterpolatorDefs.h>
#include <libaaf/AAFDefs/AAFParameterDefs.h>
#include <libaaf/AAFDefs/AAFExtEnum.h>
#include <libaaf/AAFIParser.h>
#include <libaaf/AAFIface.h>

/* internal class registry used to build the in-memory dictionary */
#include "../src/AAFCore/AAFClass.h"


typedef struct memgraph
{
	AAF_Data *aafd;
	uint32_t  nextKey;
	uint32_t  nextMob;

	aafObject *dictionary;
	aafObject *content;
	aafObject *mobsSet;
	aafObject *opDefs;
	aafObject *interpDefs;
	aafObject *dataDefs;
	aafObject *paramDefs;

} memgraph;


static inline void *
mg_xmalloc( size_t n )
{
	void *p = calloc( 1, n );

	if ( !p ) {
		exit( 97 );
	}

	return p;
}


static inline aafObject *
mg_object( memgraph *g, const aafUID_t *classID, aafObject *parent )
{
	aafClass *class = aafclass_getClassByID( g->aafd, classID );

	if ( !class ) {
		return NULL;
	}

	aafObject *obj = mg_xmalloc( sizeof(aafObject) );

	obj->Class  = class;
	obj->Parent = parent;
	obj->aafd   = g->aafd;

	/* keep an Objects list exactly like the file loader does */
	obj->nextObj        = g->aafd->Objects;
	g->aafd->Objects    = obj;

	return obj;
}


static inline void
mg_prop_raw( memgraph *g, aafObject *obj, aafPID_t pid,
             const void *val, uint16_t len )
{
	(void)g;

	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_DATA;
	p->def = aafclass_getPropertyDefinitionByID( obj->Class, pid );
	p->len = len;
	p->val = mg_xmalloc( len );
	memcpy( p->val, val, len );

	p->next      = obj->Properties;
	obj->Properties = p;
}


static inline void
mg_prop_ref( aafObject *obj, aafPID_t pid, aafObject *child )
{
	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_STRONG_OBJECT_REFERENCE;
	p->def = aafclass_getPropertyDefinitionByID( obj->Class, pid );
	p->len = sizeof(void *);
	p->val = child;

	if ( child ) {
		child->Parent = obj;
	}

	p->next      = obj->Properties;
	obj->Properties = p;
}


/*
 * Turn an ordered list of objects into a strong reference vector value.
 */
static inline void
mg_prop_vector( memgraph *g, aafObject *obj, aafPID_t pid,
                aafObject **children, uint32_t n )
{
	(void)g;

	for ( uint32_t i = 0; i < n; i++ ) {

		aafStrongRefSetHeader_t *h = mg_xmalloc( sizeof(aafStrongRefVectorHeader_t) );

		h->_entryCount   = n;
		h->_firstFreeKey = n;
		h->_lastFreeKey  = 0;

		children[i]->Header = h;

		aafStrongRefSetEntry_t *e = mg_xmalloc( sizeof(aafStrongRefSetEntry_t) );

		e->_localKey = i;
		children[i]->Entry = e;
		children[i]->Parent = obj;

		children[i]->prev = ( i > 0 ) ? children[i - 1] : NULL;
		children[i]->next = ( i + 1 < n ) ? children[i + 1] : NULL;
	}

	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_STRONG_OBJECT_REFERENCE_VECTOR;
	p->def = aafclass_getPropertyDefinitionByID( obj->Class, pid );
	p->len = (uint16_t)n;
	p->val = ( n > 0 ) ? children[0] : NULL;

	p->next      = obj->Properties;
	obj->Properties = p;
}


/*
 * Strong reference set whose identification is the 16 raw bytes of a UID.
 * The members must expose PID_DefinitionObject_Identification (definition
 * objects) or PID_Mob_MobID (mobs); use mg_mob_entry_id() for the latter.
 */
static inline void
mg_prop_set_uid( memgraph *g, aafObject *obj, aafPID_t pid,
                 aafObject **members, uint32_t n,
                 aafPID_t identificationPid, uint8_t identificationSize )
{
	(void)g;

	for ( uint32_t i = 0; i < n; i++ ) {

		aafStrongRefSetHeader_t *h = mg_xmalloc( sizeof(aafStrongRefSetHeader_t) );

		h->_entryCount         = n;
		h->_firstFreeKey       = n;
		h->_identificationPid  = identificationPid;
		h->_identificationSize = identificationSize;

		members[i]->Header = h;

		aafStrongRefSetEntry_t *e = mg_xmalloc( sizeof(aafStrongRefSetEntry_t) + identificationSize );

		e->_localKey = i;

		aafProperty *idProp = NULL;
		aafProperty *q = members[i]->Properties;

		while ( q ) {
			if ( q->pid == identificationPid ) {
				idProp = q;
				break;
			}
			q = q->next;
		}

		if ( idProp ) {
			memcpy( e->_identification, idProp->val, identificationSize );
		}

		members[i]->Entry = e;
	}

	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_STRONG_OBJECT_REFERENCE_SET;
	p->def = aafclass_getPropertyDefinitionByID( obj->Class, pid );
	p->len = (uint16_t)n;

	/* sets are walked head-first */
	p->val = ( n > 0 ) ? members[0] : NULL;

	for ( uint32_t i = 0; i < n; i++ ) {
		members[i]->next = ( i + 1 < n ) ? members[i + 1] : NULL;
		members[i]->prev = ( i > 0 ) ? members[i - 1] : NULL;
	}

	p->next      = obj->Properties;
	obj->Properties = p;
}


static inline void
mg_int64( memgraph *g, aafObject *o, aafPID_t pid, int64_t v )
{
	mg_prop_raw( g, o, pid, &v, sizeof(v) );
}


static inline void
mg_uint32( memgraph *g, aafObject *o, aafPID_t pid, uint32_t v )
{
	mg_prop_raw( g, o, pid, &v, sizeof(v) );
}


static inline void
mg_rational( memgraph *g, aafObject *o, aafPID_t pid, aafRational_t v )
{
	mg_prop_raw( g, o, pid, &v, sizeof(v) );
}


static inline void
mg_auid( memgraph *g, aafObject *o, aafPID_t pid, const aafUID_t v )
{
	mg_prop_raw( g, o, pid, &v, sizeof(v) );
}


static inline void
mg_mobid( memgraph *g, aafObject *o, aafPID_t pid, const aafMobID_t v )
{
	mg_prop_raw( g, o, pid, &v, sizeof(v) );
}


/* Indirect value wrapping one Rational, with the leading byte-order mark. */
static inline void
mg_indirect_rational( memgraph *g, aafObject *o, aafPID_t pid,
                      aafRational_t v )
{
	(void)g;

	size_t len = 1u + sizeof(aafIndirect_t) + sizeof(aafRational_t);
	unsigned char *buf = mg_xmalloc( len );

	buf[0] = AAF_PROPERTIES_BYTEORDER_LE;
	memcpy( buf + 1, &AAFTypeID_Rational, sizeof(aafUID_t) );
	memcpy( buf + 1 + sizeof(aafUID_t), &v, sizeof(aafRational_t) );

	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_DATA;
	p->def = aafclass_getPropertyDefinitionByID( o->Class, pid );
	p->len = (uint16_t)len;
	p->val = buf;

	p->next      = o->Properties;
	o->Properties = p;
}


/* Weak reference into a UID-identified strong reference set. */
static inline aafWeakRef_t *
mg_weak_uid( aafObject *setHead, const aafUID_t *id )
{
	aafWeakRef_t *w = mg_xmalloc( sizeof(aafWeakRef_t) + 16u );

	w->_identificationPid  = PID_DefinitionObject_Identification;
	w->_identificationSize = 16;
	memcpy( w->_identification, id, 16 );

	(void)setHead;

	return w;
}


/* Weak reference into a MobID-identified strong reference set (32 bytes). */
static inline aafWeakRef_t *
mg_weak_mob( const aafMobID_t *id )
{
	aafWeakRef_t *w = mg_xmalloc( sizeof(aafWeakRef_t) + sizeof(aafMobID_t) );

	w->_identificationPid  = PID_Mob_MobID;
	w->_identificationSize = (uint8_t)sizeof(aafMobID_t);
	memcpy( w->_identification, id, sizeof(aafMobID_t) );

	return w;
}


static inline void
mg_prop_weak( memgraph *g, aafObject *o, aafPID_t pid, aafWeakRef_t *w )
{
	uint16_t len = (uint16_t)( sizeof(aafWeakRef_t) + w->_identificationSize );
	unsigned char *buf = mg_xmalloc( len );

	memcpy( buf, w, len );
	free( w );

	aafProperty *p = mg_xmalloc( sizeof(aafProperty) );

	p->pid = pid;
	p->sf  = SF_WEAK_OBJECT_REFERENCE;
	p->def = aafclass_getPropertyDefinitionByID( o->Class, pid );
	p->len = len;
	p->val = buf;

	p->next      = o->Properties;
	o->Properties = p;

	(void)g;
}


/* ASCII-only UTF-16LE string, double-NUL terminated. */
static inline void
mg_string( memgraph *g, aafObject *o, aafPID_t pid, const char *s )
{
	size_t n = strlen( s );
	uint16_t len = (uint16_t)(( n + 1u ) * 2u);
	uint16_t *buf = mg_xmalloc( len );

	for ( size_t i = 0; i < n; i++ ) {
		buf[i] = (uint16_t)(unsigned char)s[i];
	}

	buf[n] = 0;
	mg_prop_raw( g, o, pid, buf, len );
	free( buf );
}


static inline aafObject *
mg_definition( memgraph *g, const aafUID_t *classID, const aafUID_t *id,
               const char *name )
{
	aafObject *o = mg_object( g, classID, g->dictionary );

	mg_auid( g, o, PID_DefinitionObject_Identification, *id );
	mg_string( g, o, PID_DefinitionObject_Name, name );

	return o;
}


static inline aafObject *
mg_filler( memgraph *g, aafObject *parent, int64_t length )
{
	aafObject *o = mg_object( g, &AAFClassID_Filler, parent );

	aafWeakRef_t *w = mg_weak_uid( g->dataDefs, &AAFDataDef_Sound );
	mg_prop_weak( g, o, PID_Component_DataDefinition, w );
	mg_int64( g, o, PID_Component_Length, length );

	return o;
}


static inline aafObject *
mg_sequence( memgraph *g, aafObject *parent, int64_t length,
             aafObject **components, uint32_t n )
{
	aafObject *o = mg_object( g, &AAFClassID_Sequence, parent );

	aafWeakRef_t *w = mg_weak_uid( g->dataDefs, &AAFDataDef_Sound );
	mg_prop_weak( g, o, PID_Component_DataDefinition, w );
	mg_int64( g, o, PID_Component_Length, length );
	mg_prop_vector( g, o, PID_Sequence_Components, components, n );

	return o;
}


static inline aafObject *
mg_source_clip( memgraph *g, aafObject *parent, int64_t length,
                int64_t startTime, uint32_t slotID,
                const aafMobID_t *sourceID )
{
	aafObject *o = mg_object( g, &AAFClassID_SourceClip, parent );

	aafWeakRef_t *w = mg_weak_uid( g->dataDefs, &AAFDataDef_Sound );
	mg_prop_weak( g, o, PID_Component_DataDefinition, w );
	mg_int64( g, o, PID_Component_Length, length );
	mg_int64( g, o, PID_SourceClip_StartTime, startTime );
	mg_uint32( g, o, PID_SourceReference_SourceMobSlotID, slotID );

	if ( sourceID ) {
		mg_mobid( g, o, PID_SourceReference_SourceID, *sourceID );
	}

	return o;
}


static inline aafObject *
mg_constant_amp( memgraph *g, aafObject *parent, aafRational_t value )
{
	aafObject *o = mg_object( g, &AAFClassID_ConstantValue, parent );

	/* Parameter::Definition is stored as a raw AUID value (SF_DATA). */
	mg_auid( g, o, PID_Parameter_Definition, AAFParameterDef_Amplitude );
	mg_indirect_rational( g, o, PID_ConstantValue_Value, value );

	return o;
}


static inline aafObject *
mg_control_point( memgraph *g, aafObject *parent,
                  aafRational_t time, aafRational_t value )
{
	aafObject *o = mg_object( g, &AAFClassID_ControlPoint, parent );

	mg_rational( g, o, PID_ControlPoint_Time, time );
	mg_indirect_rational( g, o, PID_ControlPoint_Value, value );

	return o;
}


static inline aafObject *
mg_varying_amp( memgraph *g, aafObject *parent, const aafUID_t *interpID,
                const aafRational_t *times, const aafRational_t *values,
                uint32_t n )
{
	aafObject *o = mg_object( g, &AAFClassID_VaryingValue, parent );

	/* Parameter::Definition is stored as a raw AUID value (SF_DATA). */
	mg_auid( g, o, PID_Parameter_Definition, AAFParameterDef_Amplitude );

	aafWeakRef_t *idef = mg_weak_uid( g->interpDefs, interpID );
	mg_prop_weak( g, o, PID_VaryingValue_Interpolation, idef );

	aafObject **points = mg_xmalloc( n * sizeof(aafObject *) );

	for ( uint32_t i = 0; i < n; i++ ) {
		points[i] = mg_control_point( g, o, times[i], values[i] );
	}

	mg_prop_vector( g, o, PID_VaryingValue_PointList, points, n );
	free( points );

	return o;
}


static inline aafObject *
mg_op_gain( memgraph *g, aafObject *parent, int64_t length,
            aafObject *input, aafObject *parameter )
{
	aafObject *o = mg_object( g, &AAFClassID_OperationGroup, parent );

	aafWeakRef_t *ddef = mg_weak_uid( g->dataDefs, &AAFDataDef_Sound );
	mg_prop_weak( g, o, PID_Component_DataDefinition, ddef );
	mg_int64( g, o, PID_Component_Length, length );

	aafWeakRef_t *odef = mg_weak_uid( g->opDefs, &AAFOperationDef_MonoAudioGain );
	mg_prop_weak( g, o, PID_OperationGroup_Operation, odef );

	aafObject *inputs[1] = { input };
	mg_prop_vector( g, o, PID_OperationGroup_InputSegments, inputs, 1 );

	aafObject *params[1] = { parameter };
	mg_prop_vector( g, o, PID_OperationGroup_Parameters, params, 1 );

	return o;
}


static inline aafMobID_t
mg_make_mob_id( memgraph *g )
{
	aafMobID_t id;

	memset( &id, 0, sizeof(id) );
	id.material.Data1 = 0x47410000u + g->nextMob++;

	return id;
}


static inline aafObject *
mg_pcm_descriptor( memgraph *g, aafObject *parent )
{
	aafObject *o = mg_object( g, &AAFClassID_PCMDescriptor, parent );

	aafRational_t rate = { 48000, 1 };

	mg_rational( g, o, PID_FileDescriptor_SampleRate, rate );
	mg_int64( g, o, PID_FileDescriptor_Length, 48000 );
	mg_uint32( g, o, PID_SoundDescriptor_Channels, 1 );
	mg_uint32( g, o, PID_SoundDescriptor_QuantizationBits, 16 );

	return o;
}


static inline aafObject *
mg_source_mob( memgraph *g, const aafMobID_t *mobID, uint32_t slotID,
               aafObject *slotSegment )
{
	aafObject *mob = mg_object( g, &AAFClassID_SourceMob, NULL );

	mg_mobid( g, mob, PID_Mob_MobID, *mobID );

	aafTimeStamp_t ts;
	memset( &ts, 0, sizeof(ts) );
	mg_prop_raw( g, mob, PID_Mob_CreationTime, &ts, sizeof(ts) );

	aafObject *slot = mg_object( g, &AAFClassID_TimelineMobSlot, mob );

	mg_uint32( g, slot, PID_MobSlot_SlotID, slotID );
	mg_int64( g, slot, PID_TimelineMobSlot_Origin, 0 );
	mg_rational( g, slot, PID_TimelineMobSlot_EditRate, (aafRational_t){ 48000, 1 } );
	mg_prop_ref( slot, PID_MobSlot_Segment, slotSegment );

	aafObject *slots[1] = { slot };
	mg_prop_vector( g, mob, PID_Mob_Slots, slots, 1 );

	aafObject *desc = mg_pcm_descriptor( g, mob );
	mg_prop_ref( mob, PID_SourceMob_EssenceDescription, desc );

	return mob;
}


static inline aafObject *
mg_master_mob( memgraph *g, const aafMobID_t *mobID, uint32_t slotID,
               aafObject *slotSegment )
{
	aafObject *mob = mg_object( g, &AAFClassID_MasterMob, NULL );

	mg_mobid( g, mob, PID_Mob_MobID, *mobID );

	aafTimeStamp_t ts;
	memset( &ts, 0, sizeof(ts) );
	mg_prop_raw( g, mob, PID_Mob_CreationTime, &ts, sizeof(ts) );

	aafObject *slot = mg_object( g, &AAFClassID_TimelineMobSlot, mob );

	mg_uint32( g, slot, PID_MobSlot_SlotID, slotID );
	mg_rational( g, slot, PID_TimelineMobSlot_EditRate, (aafRational_t){ 48000, 1 } );
	mg_prop_ref( slot, PID_MobSlot_Segment, slotSegment );

	aafObject *slots[1] = { slot };
	mg_prop_vector( g, mob, PID_Mob_Slots, slots, 1 );

	return mob;
}


static inline aafObject *
mg_timeline_slot( memgraph *g, aafObject *parent, uint32_t slotID,
                  uint32_t trackNum, aafRational_t editRate,
                  aafObject *segment )
{
	aafObject *slot = mg_object( g, &AAFClassID_TimelineMobSlot, parent );

	mg_uint32( g, slot, PID_MobSlot_SlotID, slotID );
	mg_uint32( g, slot, PID_MobSlot_PhysicalTrackNumber, trackNum );
	mg_rational( g, slot, PID_TimelineMobSlot_EditRate, editRate );
	mg_prop_ref( slot, PID_MobSlot_Segment, segment );

	return slot;
}


static inline aafObject *
mg_composition( memgraph *g, const aafMobID_t *mobID,
                aafObject **slots, uint32_t n )
{
	aafObject *mob = mg_object( g, &AAFClassID_CompositionMob, NULL );

	mg_mobid( g, mob, PID_Mob_MobID, *mobID );
	mg_auid( g, mob, PID_Mob_UsageCode, AAFUsage_TopLevel );

	aafTimeStamp_t ts;
	memset( &ts, 0, sizeof(ts) );
	mg_prop_raw( g, mob, PID_Mob_CreationTime, &ts, sizeof(ts) );

	mg_prop_vector( g, mob, PID_Mob_Slots, slots, n );

	return mob;
}


static inline memgraph *
mg_new( void )
{
	memgraph *g = mg_xmalloc( sizeof(memgraph) );

	g->aafd = aaf_alloc( NULL );

	if ( !g->aafd || aafclass_setDefaultClasses( g->aafd ) < 0 ) {
		return NULL;
	}

	/* empty Header, only the operational pattern is consulted */
	aafObject *header = mg_object( g, &AAFClassID_Header, NULL );

	g->aafd->Header.obj                = header;
	g->aafd->Header.OperationalPattern = &AAFUID_NULL;

	g->dictionary = mg_object( g, &AAFClassID_Dictionary, NULL );
	g->content    = mg_object( g, &AAFClassID_ContentStorage, NULL );

	aafObject *opDefs[1]    = { mg_definition( g, &AAFClassID_OperationDefinition,
	                                           &AAFOperationDef_MonoAudioGain,
	                                           "Mono Audio Gain" ) };
	aafObject *interpDefs[3] = {
		mg_definition( g, &AAFClassID_InterpolationDefinition,
		               &AAFInterpolationDef_Linear, "Linear" ),
		mg_definition( g, &AAFClassID_InterpolationDefinition,
		               &AAFInterpolationDef_Constant, "Constant" ),
		mg_definition( g, &AAFClassID_InterpolationDefinition,
		               &AAFInterpolationDef_Log, "Log" )
	};
	aafObject *dataDefs[1]  = { mg_definition( g, &AAFClassID_DataDefinition,
	                                           &AAFDataDef_Sound, "Sound" ) };
	aafObject *paramDefs[1] = { mg_definition( g, &AAFClassID_ParameterDefinition,
	                                           &AAFParameterDef_Amplitude,
	                                           "Amplitude" ) };

	mg_prop_set_uid( g, g->dictionary, PID_Dictionary_OperationDefinitions, opDefs, 1,
	                 PID_DefinitionObject_Identification, 16 );
	mg_prop_set_uid( g, g->dictionary, PID_Dictionary_InterpolationDefinitions, interpDefs, 3,
	                 PID_DefinitionObject_Identification, 16 );
	mg_prop_set_uid( g, g->dictionary, PID_Dictionary_DataDefinitions, dataDefs, 1,
	                 PID_DefinitionObject_Identification, 16 );
	mg_prop_set_uid( g, g->dictionary, PID_Dictionary_ParameterDefinitions, paramDefs, 1,
	                 PID_DefinitionObject_Identification, 16 );

	g->opDefs    = opDefs[0];
	g->interpDefs = interpDefs[0];
	g->dataDefs  = dataDefs[0];
	g->paramDefs = paramDefs[0];

	g->aafd->Dictionary              = g->dictionary;
	g->aafd->OperationDefinition     = g->opDefs;
	g->aafd->InterpolationDefinition = g->interpDefs;
	g->aafd->DataDefinition          = g->dataDefs;
	g->aafd->ParameterDefinition     = g->paramDefs;

	/*
	 * ContentStorage::Mobs is attached later by mg_set_mobs(), once every
	 * Composition/Source/Master mob exists.
	 */
	g->aafd->Content = g->content;
	g->aafd->Mobs    = NULL;

	return g;
}


/*
 * Register the composition/master/source mobs as the ContentStorage Mobs set.
 * Called once all mobs exist.
 */
static inline void
mg_set_mobs( memgraph *g, aafObject **mobs, uint32_t n )
{
	mg_prop_set_uid( g, g->content, PID_ContentStorage_Mobs, mobs, n,
	                 PID_Mob_MobID, (uint8_t)sizeof(aafMobID_t) );

	/* the property just created is head of the Properties list */
	aafProperty *p = g->content->Properties;

	g->aafd->Mobs = p->val;
}


static inline void
mg_free( memgraph *g )
{
	if ( !g ) {
		return;
	}

	/*
	 * The AAF_Data ownership is transferred to the AAF_Iface built by
	 * mg_import() (aafi_alloc(aafd) does not copy it), so aaf_release()
	 * already frees every object and the underlying CFB_Data. Only the
	 * small memgraph wrapper is owned here.
	 */
	free( g );
}


#endif /* TASK_SUPPORT_MEMGRAPH_H */
