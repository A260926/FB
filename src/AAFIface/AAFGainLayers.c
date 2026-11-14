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

/**
 * @file LibAAF/AAFIface/AAFGainLayers.c
 * @brief Nested clip gain layers storage, validation and exact-rational
 *        compound gain evaluation.
 * @ingroup AAFIface
 */

#include <stdlib.h>
#include <math.h>
#include <limits.h>

#include <libaaf/AAFGainLayers.h>
#include "AAFGainLayersInternal.h"


/* ****************************************************************************
 *                      E x a c t   r a t i o n a l   m a t h
 * ****************************************************************************
 *
 * All ordering decisions (offset range checks, control point ordering,
 * coordinate comparisons) are performed on the integer rational values,
 * never on doubles, so two differently written but equal fractions compare
 * equal and a tiny fraction added to a large time position is not swallowed.
 * The aafRational_t storage is int32, hence the widened internal frac64.
 */

typedef struct frac64
{
	int64_t num;
	int64_t den; /* always strictly positive */

} frac64;


static int
mul_overflow( int64_t a, int64_t b )
{
	if ( a == 0 || b == 0 ) {
		return 0;
	}

	if ( a > 0 ) {
		if ( b > 0 ) {
			return ( a > INT64_MAX / b );
		} else {
			return ( b < INT64_MIN / a );
		}
	} else {
		if ( b > 0 ) {
			return ( a < INT64_MIN / b );
		} else {
			return ( b < INT64_MAX / a );
		}
	}
}


static int64_t
frac_gcd( int64_t a, int64_t b )
{
	if ( a < 0 ) {
		a = -a;
	}

	if ( b < 0 ) {
		b = -b;
	}

	while ( b != 0 ) {
		int64_t r = a % b;
		a = b;
		b = r;
	}

	return a;
}


static frac64
frac_reduce( frac64 f )
{
	if ( f.num == 0 ) {
		f.den = 1;
		return f;
	}

	int64_t g = frac_gcd( f.num, f.den );

	if ( g > 1 ) {
		f.num /= g;
		f.den /= g;
	}

	return f;
}


/*
 * Compare two positive-denominator fractions exactly.
 *
 * @return -1, 0 or 1, respectively when a < b, a == b or a > b.
 */
static int
frac_cmp( frac64 a, frac64 b )
{
	if ( !mul_overflow( a.num, b.den ) && !mul_overflow( b.num, a.den ) ) {

		int64_t lhs = a.num * b.den;
		int64_t rhs = b.num * a.den;

		if ( lhs < rhs ) {
			return -1;
		}

		if ( lhs > rhs ) {
			return 1;
		}

		return 0;
	}

	/*
	 * Only reachable with unusually large coordinates : reduce both
	 * fractions before the cross product. Equality is still decided on
	 * integers, never on doubles.
	 */
	a = frac_reduce( a );
	b = frac_reduce( b );

	if ( mul_overflow( a.num, b.den ) || mul_overflow( b.num, a.den ) ) {
		/* cannot compare without losing exactness */
		return -2;
	}

	int64_t lhs = a.num * b.den;
	int64_t rhs = b.num * a.den;

	if ( lhs < rhs ) {
		return -1;
	}

	if ( lhs > rhs ) {
		return 1;
	}

	return 0;
}


static frac64
frac_from_rat( aafRational_t r )
{
	frac64 f = { (int64_t)r.numerator, (int64_t)r.denominator };

	return f;
}


/*
 * Validate a VaryingValue control point list for import.
 *
 * The times must lie in [0,1], use positive denominators and be strictly
 * increasing (compared exactly, not as doubles). The amplitude values must
 * be non-negative rationals with positive denominators. The list must not be
 * empty.
 *
 * @return 0 when the curve is usable, -1 otherwise.
 */
int
aafi_gainLayer_validatePoints( const aafRational_t *times,
                               const aafRational_t *values,
                               unsigned int         cnt )
{
	if ( cnt == 0 || times == NULL || values == NULL ) {
		return -1;
	}

	const frac64 zero = { 0, 1 };
	const frac64 one  = { 1, 1 };

	for ( unsigned int i = 0; i < cnt; i++ ) {

		if ( times[i].denominator <= 0 || values[i].denominator <= 0 ) {
			return -1;
		}

		frac64 t = frac_from_rat( times[i] );

		int c_lo = frac_cmp( t, zero );
		int c_hi = frac_cmp( t, one );

		if ( c_lo == -2 || c_hi == -2 ) {
			return -1;
		}

		/* time in [0,1] */
		if ( c_lo < 0 || c_hi > 0 ) {
			return -1;
		}

		/* non-negative amplitude */
		if ( values[i].numerator < 0 ) {
			return -1;
		}

		if ( i > 0 ) {
			int c = frac_cmp( frac_from_rat( times[i - 1] ), t );

			if ( c == -2 || c >= 0 ) {
				/* duplicate or out-of-order time, or uncomparable */
				return -1;
			}
		}
	}

	return 0;
}


/* ****************************************************************************
 *                          L i f e c y c l e
 * ****************************************************************************/

aafiGainLayer *
aafi_gainLayer_new( void )
{
	aafiGainLayer *layer = calloc( 1, sizeof(aafiGainLayer) );

	return layer;
}


aafiGainLayer *
aafi_gainLayer_free( aafiGainLayer *layer )
{
	if ( !layer ) {
		return NULL;
	}

	aafiGainLayer *next = layer->next;

	free( layer->point_time );
	free( layer->point_value );
	free( layer );

	return next;
}


void
aafi_gainLayer_freeList( aafiGainLayer **list )
{
	if ( !list ) {
		return;
	}

	aafiGainLayer *layer = *list;

	while ( layer ) {
		layer = aafi_gainLayer_free( layer );
	}

	*list = NULL;
}


int
aafi_clip_appendGainLayer( aafiAudioClip *clip, aafiGainLayer *layer )
{
	if ( !clip || !layer ) {
		return -1;
	}

	layer->next = NULL;

	if ( !clip->gain_layers ) {
		clip->gain_layers = layer;
		return 0;
	}

	aafiGainLayer *last = clip->gain_layers;

	while ( last->next ) {
		last = last->next;
	}

	last->next = layer;

	return 0;
}


aafiGainLayer *
aafi_gainLayer_clone( const aafiGainLayer *src )
{
	if ( !src ) {
		return NULL;
	}

	aafiGainLayer *dst = aafi_gainLayer_new();

	if ( !dst ) {
		return NULL;
	}

	dst->type         = src->type;
	dst->group_length = src->group_length;
	dst->group_offset = src->group_offset;
	dst->constant     = src->constant;
	dst->point_cnt    = src->point_cnt;

	if ( src->point_cnt > 0 ) {

		dst->point_time = calloc( src->point_cnt, sizeof(aafRational_t) );
		dst->point_value = calloc( src->point_cnt, sizeof(aafRational_t) );

		if ( !dst->point_time || !dst->point_value ) {
			aafi_gainLayer_free( dst );
			return NULL;
		}

		for ( unsigned int i = 0; i < src->point_cnt; i++ ) {
			dst->point_time[i]  = src->point_time[i];
			dst->point_value[i] = src->point_value[i];
		}
	}

	return dst;
}


/* ****************************************************************************
 *                      C u r v e   e v a l u a t i o n
 * ****************************************************************************/

static double
rat_to_double( aafRational_t r )
{
	return (double)r.numerator / (double)r.denominator;
}


/*
 * Evaluate one layer at normalized position t (exact rational in [0,1]).
 *
 * @return 0 on success and *out receives the linear multiplier,
 *         -1 if the layer cannot be interpreted or the result is not finite.
 */
static int
eval_layer( const aafiGainLayer *layer, frac64 t, double *out )
{
	if ( layer->type == AAFI_GAIN_LAYER_CONSTANT ) {

		if ( layer->constant.denominator <= 0 ||
		     layer->constant.numerator < 0 )
		{
			return -1;
		}

		double v = rat_to_double( layer->constant );

		if ( !isfinite( v ) ) {
			return -1;
		}

		*out = v;
		return 0;
	}

	if ( layer->type != AAFI_GAIN_LAYER_VARYING_LINEAR &&
	     layer->type != AAFI_GAIN_LAYER_VARYING_CONSTANT )
	{
		return -1;
	}

	if ( layer->point_cnt == 0 || !layer->point_time || !layer->point_value ) {
		return -1;
	}

	/* clamp outside the first/last control point, extending end values */

	int c_first = frac_cmp( t, frac_from_rat( layer->point_time[0] ) );

	if ( c_first == -2 ) {
		return -1;
	}

	if ( c_first <= 0 ) {
		double v = rat_to_double( layer->point_value[0] );

		if ( !isfinite( v ) ) {
			return -1;
		}

		*out = v;
		return 0;
	}

	unsigned int last = layer->point_cnt - 1;

	int c_last = frac_cmp( t, frac_from_rat( layer->point_time[last] ) );

	if ( c_last == -2 ) {
		return -1;
	}

	if ( c_last >= 0 ) {
		double v = rat_to_double( layer->point_value[last] );

		if ( !isfinite( v ) ) {
			return -1;
		}

		*out = v;
		return 0;
	}

	/* t is strictly inside the point range, locate the surrounding pair */

	unsigned int i = 0;

	while ( i < last ) {
		int c = frac_cmp( frac_from_rat( layer->point_time[i + 1] ), t );

		if ( c == -2 ) {
			return -1;
		}

		if ( c >= 0 ) {
			break;
		}

		i++;
	}

	/*
	 * After the loop, point_time[i] < t <= point_time[i+1]. Equality on the
	 * right point must take the right point's value for both interpolations.
	 */
	int c_eq = frac_cmp( t, frac_from_rat( layer->point_time[i + 1] ) );

	if ( c_eq == -2 ) {
		return -1;
	}

	if ( c_eq == 0 ) {
		double v = rat_to_double( layer->point_value[i + 1] );

		if ( !isfinite( v ) ) {
			return -1;
		}

		*out = v;
		return 0;
	}

	double v1 = rat_to_double( layer->point_value[i] );

	if ( !isfinite( v1 ) ) {
		return -1;
	}

	if ( layer->type == AAFI_GAIN_LAYER_VARYING_CONSTANT ) {
		/* hold the left value until, but excluding, the right point */
		*out = v1;
		return 0;
	}

	/* linear interpolation, fractions converted to double only now */

	double v2 = rat_to_double( layer->point_value[i + 1] );
	double t0 = rat_to_double( layer->point_time[i] );
	double t1 = rat_to_double( layer->point_time[i + 1] );
	double tt = (double)t.num / (double)t.den;

	if ( !isfinite( v2 ) || !isfinite( t0 ) || !isfinite( t1 ) ||
	     !isfinite( tt ) || t1 == t0 )
	{
		return -1;
	}

	double v = v1 + ( v2 - v1 ) * ( ( tt - t0 ) / ( t1 - t0 ) );

	if ( !isfinite( v ) ) {
		return -1;
	}

	*out = v;
	return 0;
}


/* ****************************************************************************
 *                          P u b l i c   A P I
 * ****************************************************************************/

size_t
aafi_clip_gain_layer_count( const aafiAudioClip *clip )
{
	if ( !clip ) {
		return 0;
	}

	size_t cnt = 0;

	for ( const aafiGainLayer *layer = clip->gain_layers;
	      layer != NULL;
	      layer = layer->next )
	{
		cnt++;
	}

	return cnt;
}


int
aafi_clip_gain_at( const aafiAudioClip *clip,
                   aafRational_t        offset,
                   double              *multiplier )
{
	if ( !clip || !multiplier ) {
		return -1;
	}

	if ( offset.denominator <= 0 ) {
		return -1;
	}

	/* 0 <= offset < clip->len, compared exactly as rationals */
	const frac64 off = { (int64_t)offset.numerator,
	                     (int64_t)offset.denominator };
	const frac64 zero = { 0, 1 };
	const frac64 len  = { clip->len, 1 };

	int c_off_lo = frac_cmp( off, zero );
	int c_off_hi = frac_cmp( off, len );

	if ( c_off_lo == -2 || c_off_hi == -2 ||
	     c_off_lo < 0 || c_off_hi >= 0 )
	{
		return -1;
	}

	if ( !clip->gain_layers ) {
		*multiplier = 1.0;
		return 0;
	}

	double compound = 1.0;

	for ( const aafiGainLayer *layer = clip->gain_layers;
	      layer != NULL;
	      layer = layer->next )
	{
		if ( layer->group_length <= 0 ||
		     layer->group_offset < 0 ||
		     layer->group_offset >= layer->group_length )
		{
			return -1;
		}

		/*
		 * Project the clip-local offset onto this layer's OperationGroup
		 * coordinate space, keeping the fraction exact :
		 *
		 *   t = (group_offset + offset) / group_length
		 *
		 * group_offset, offset and group_length all share the owning track
		 * edit rate. SourceClip::StartTime deliberately does not appear.
		 */
		int64_t add = (int64_t)layer->group_offset * off.den;

		if ( add < 0 || add > INT64_MAX - off.num ) {
			return -1;
		}

		int64_t t_num = off.num + add;

		if ( t_num < 0 || mul_overflow( off.den, layer->group_length ) ) {
			return -1;
		}

		int64_t t_den = off.den * layer->group_length;

		/* the clip must be fully contained in the OperationGroup */
		if ( t_num < 0 || t_num > t_den ) {
			return -1;
		}

		frac64 t = { t_num, t_den };

		double m = 1.0;

		if ( eval_layer( layer, t, &m ) != 0 ) {
			return -1;
		}

		compound *= m;

		if ( !isfinite( compound ) ) {
			return -1;
		}
	}

	*multiplier = compound;

	return 0;
}
