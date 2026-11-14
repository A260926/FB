/*
 * Copyright (C) 2017-2024 Adrien Gesta-Fline
 *
 * This file is part of libaaf
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef __AAFGainLayersInternal_h__
#define __AAFGainLayersInternal_h__

/**
 * @file LibAAF/AAFIface/AAFGainLayersInternal.h
 * @brief Internal definitions of the nested clip gain layer machinery,
 *        shared between AAFIParser.c and AAFIface.c. Not installed as a
 *        public API on its own; user code includes libaaf/AAFGainLayers.h.
 */

#include <libaaf/AAFIface.h>
#include <libaaf/AAFGainLayers.h>


/**
 * Curve kind of a gain layer.
 */
typedef enum aafiGainLayerType_e
{
	/* Layer holds a single ConstantValue::Amplitude multiplier. */
	AAFI_GAIN_LAYER_CONSTANT = 1,

	/*
	 * Layer holds a VaryingValue::Amplitude automation curve, with either
	 * Linear or Constant interpolation between control points.
	 */
	AAFI_GAIN_LAYER_VARYING_LINEAR   = 2,
	AAFI_GAIN_LAYER_VARYING_CONSTANT = 3

} aafiGainLayerType_e;


/**
 * One MonoAudioGain OperationGroup that applies to a leaf clip.
 *
 * Every field is an independent, heap-owned copy made while importing : the
 * layer never references the parser context, aafObject nodes, property value
 * buffers or any other clip's memory. Layers therefore stay valid after the
 * parser exits and after sibling clips have been released.
 *
 * Time mapping (all in the owning track edit rate, which every nested
 * component inherits) :
 *
 *   - group_length is the OperationGroup's Component::Length, i.e. the
 *     denominator of the layer's normalized VaryingValue control point times.
 *   - group_offset is the position of the clip's start inside that
 *     OperationGroup, i.e. the sum of the Component::Length of every sibling
 *     component (leading Filler, sibling clips, nested groups) that precedes
 *     the branch containing the clip.
 *
 * At query time, a clip-local offset maps to the layer coordinate as
 * (group_offset + offset) / group_length. SourceClip::StartTime
 * (aafiAudioClip.essence_offset) does not participate in that mapping.
 */
struct aafiGainLayer
{
	/* One of aafiGainLayerType_e. */
	int                 type;

	/* OperationGroup Component::Length, in track edit units. Always > 0. */
	aafPosition_t       group_length;

	/*
	 * Position of the clip start inside the OperationGroup, in track edit
	 * units. Always satisfies 0 <= group_offset < group_length.
	 */
	aafPosition_t       group_offset;

	/*
	 * Constant layer multiplier (type == AAFI_GAIN_LAYER_CONSTANT).
	 */
	aafRational_t       constant;

	/*
	 * Varying layer control points (type == AAFI_GAIN_LAYER_VARYING_*).
	 *
	 * point_time[] holds the exact, normalized ControlPoint::Time values in
	 * [0,1], strictly increasing. point_value[] holds the non-negative
	 * rational Amplitude multipliers, with strictly positive denominators.
	 * point_cnt is at least one. Points may omit 0 and/or 1; the end point
	 * values are extended outside the first/last point.
	 */
	unsigned int        point_cnt;
	aafRational_t      *point_time;
	aafRational_t      *point_value;

	/* Layers are stored outermost-first on aafiAudioClip.gain_layers. */
	struct aafiGainLayer *next;
};


/**
 * Allocates an empty gain layer.
 *
 * @return Heap-owned layer, or NULL on allocation failure.
 */
aafiGainLayer * aafi_gainLayer_new( void );


/**
 * Validates a VaryingValue control point list before it gets published as a
 * layer : non-empty, positive denominators, times strictly increasing in
 * [0,1] (exact rational comparisons), non-negative amplitude values.
 *
 * @return 0 when usable, -1 otherwise.
 */
int aafi_gainLayer_validatePoints( const aafRational_t *times,
                                   const aafRational_t *values,
                                   unsigned int         cnt );


/**
 * Frees a single layer (including its control point arrays) and returns the
 * layer that followed it, so it can be used as a list pop.
 *
 * @param layer  Layer to free, may be NULL.
 *
 * @return The former layer->next pointer.
 */
aafiGainLayer * aafi_gainLayer_free( aafiGainLayer *layer );


/**
 * Frees a whole layer list and sets *list to NULL.
 *
 * @param list  Address of the layer list head.
 */
void aafi_gainLayer_freeList( aafiGainLayer **list );


/**
 * Appends a layer to a clip's layer list, preserving the outermost-first
 * import order.
 *
 * @param clip   Destination clip.
 * @param layer  Layer to take ownership of.
 *
 * @return 0 on success, -1 on allocation failure (the layer is freed).
 */
int aafi_clip_appendGainLayer( aafiAudioClip *clip, aafiGainLayer *layer );


/**
 * Deep-copies a layer, including its control point arrays. The copy shares
 * no memory with the source and is therefore unaffected by the parser
 * tearing down its templates or sibling clips.
 *
 * @param src  Layer to copy, may be NULL.
 *
 * @return A new independent layer, or NULL on allocation failure or when src
 *         is NULL.
 */
aafiGainLayer * aafi_gainLayer_clone( const aafiGainLayer *src );


#endif /* ! __AAFGainLayersInternal_h__ */
