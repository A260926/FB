/*
 * clip_gain_layers.h - internal multi-layer clip gain representation.
 *
 * The public query API (aafi_clip_gain_layer_count/aafi_clip_gain_at) and
 * the AAF object graph importer share these definitions. Every bit of
 * curve and coordinate data owned by a layer set is a private copy: it
 * never points into the parser context, the AAF object tree or another
 * clip.
 */

#ifndef AAFI_CLIP_GAIN_LAYERS_H
#define AAFI_CLIP_GAIN_LAYERS_H

#include <libaaf/AAFTypes.h>
#include <libaaf/AAFIface.h>

#ifdef __cplusplus
extern "C" {
#endif


enum aafiGainLayerKind {
	AAFI_GAIN_LAYER_KIND_CONSTANT = 1,
	AAFI_GAIN_LAYER_KIND_VARYING  = 2
};


typedef struct aafiGainLayerPoint {
	aafRational_t time;  /* normalized position on the group [0,1] */
	aafRational_t value; /* non-negative linear amplitude          */
} aafiGainLayerPoint;


typedef struct aafiGainLayer {
	/*
	 * A layer is published in the clip's layer array as soon as its
	 * MonoAudioGain group is known. import_valid tells whether all of
	 * its data was interpreted and validated. An invalid layer makes
	 * aafi_clip_gain_at() fail explicitly instead of evaluating a
	 * broken/partial curve.
	 */
	int                       import_valid;

	enum aafiGainLayerKind   kind;

	/*
	 * AAFI_INTERPOL_LINEAR or AAFI_INTERPOL_CONSTANT, only meaningful
	 * for AAFI_GAIN_LAYER_KIND_VARYING.
	 */
	uint32_t                  interpolation;

	/*
	 * Component::Length of the owning MonoAudioGain OperationGroup, in
	 * track edit units. Always strictly positive for a valid layer.
	 */
	int64_t                   group_length;

	/*
	 * Exact offset from the start of that OperationGroup to the start
	 * of the leaf clip, in track edit units. Takes ancestor group
	 * offsets, Sequence Filler and preceding siblings into account.
	 */
	int64_t                   clip_offset;

	aafRational_t             constant_value;

	unsigned int              point_count;
	aafiGainLayerPoint       *points;
} aafiGainLayer;


struct aafiClipGainLayerSet {
	unsigned int   count;
	aafiGainLayer *layers;

	/*
	 * Set when the graph geometry leading to the clip could not be
	 * interpreted exactly (missing lengths, inconsistent parentage).
	 * aafi_clip_gain_at() then fails for the whole clip.
	 */
	int            geometry_invalid;
};


struct AAF_Iface;
struct aafObject;

/*
 * Walks the AAF object ancestors of the freshly imported leaf
 * SourceClip and builds the independently owned layer set. Never fails
 * the surrounding import: an uninterpretable layer is recorded as an
 * invalid layer instead.
 */
void aafi_clip_gain_layers_import( struct AAF_Iface *aafi,
                                   aafiAudioClip *clip,
                                   struct aafObject *source_clip_obj );

/*
 * Frees a layer set and every owned curve buffer. NULL safe.
 */
void aafi_clip_gain_layer_set_free( struct aafiClipGainLayerSet *set );


#ifdef __cplusplus
}
#endif

#endif /* AAFI_CLIP_GAIN_LAYERS_H */
