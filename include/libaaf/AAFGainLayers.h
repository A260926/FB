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

#ifndef __AAFGainLayers_h__
#define __AAFGainLayers_h__

/**
 * @file LibAAF/AAFIface/AAFGainLayers.h
 * @brief Nested clip gain layers (MonoAudioGain OperationGroups) and the
 *        compound gain query API.
 * @ingroup AAFIface
 * @{
 */

#include <stddef.h>

#include <libaaf/AAFTypes.h>


/**
 * A single clip gain layer, backed by one MonoAudioGain OperationGroup that
 * wraps the clip in the CompositionMob audio track.
 *
 * The structure is opaque on purpose : user code shall not read its fields,
 * but use aafi_clip_gain_layer_count() and aafi_clip_gain_at().
 */
typedef struct aafiGainLayer aafiGainLayer;


/*
 * AAFIface.h defines the full aafiAudioClip structure. This identical typedef
 * (valid in C11) only lets the declarations below stand on their own.
 */
typedef struct aafiAudioClip aafiAudioClip;


/**
 * Returns the number of clip gain layers attached to an audio clip.
 *
 * Each layer corresponds to one MonoAudioGain OperationGroup in the
 * CompositionMob audio track, whose InputSegment chain contains the clip,
 * either directly or through nested OperationGroups / Sequences.
 *
 * The outermost track volume fader is not a clip layer and is never counted
 * here; it stays available through aafiAudioTrack.gain / .automation.
 *
 * A clip without any clip gain layer returns 0.
 *
 * @param clip  The audio clip, as returned by aafi_timelineItemToAudioClip().
 *
 * @return      The number of gain layers, or 0 if clip is NULL or has none.
 */
size_t aafi_clip_gain_layer_count( const aafiAudioClip *clip );


/**
 * Evaluates the compound clip gain at a given clip-local position.
 *
 * offset is a rational position, relative to the beginning of the clip and
 * expressed in the edit units of the track owning the clip
 * (aafiAudioTrack.edit_rate). It shall satisfy 0 <= offset < clip->len. The
 * rational form is kept so callers can pass fractional edit units exactly.
 *
 * For each gain layer, the offset is projected (using exact rational
 * arithmetic) onto that layer's own OperationGroup coordinate space, the
 * layer curve is evaluated at that normalized position, and all the layer
 * multipliers are multiplied together. Control point times are normalized
 * over each OperationGroup's own Component::Length, never over the clip.
 *
 * Only the clip layers are taken into account. The track volume fader
 * (aafiAudioTrack.gain / .automation) is not multiplied in.
 *
 * On success, 0 is returned and the linear amplitude multiplier is written to
 * *multiplier. On error, a non-zero value is returned and *multiplier is left
 * untouched. A clip without any gain layer always succeeds and yields 1.0.
 *
 * The function is stateless and reentrant : it may be called with offsets in
 * any order, several times, from several threads on the same immutable clip.
 *
 * @param clip        The audio clip.
 * @param offset      Clip-local position, in track edit units.
 * @param multiplier  Receives the compound linear amplitude multiplier.
 *
 * @return            0 on success, non-zero on error.
 */
int aafi_clip_gain_at( const aafiAudioClip *clip,
                       aafRational_t        offset,
                       double              *multiplier );


/**
 * @}
 */

#endif /* ! __AAFGainLayers_h__ */
