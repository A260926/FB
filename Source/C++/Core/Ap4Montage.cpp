/*****************************************************************
|
|    AP4 - Track Montage (non re-encoding edit list assembly)
|
|    Copyright 2002-2008 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
|    Bento4|GPL is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with Bento4|GPL; see the file COPYING.  If not, write to
|    the Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4Track.h"
#include "Ap4Types.h"
#include "Ap4Results.h"
#include "Ap4Array.h"
#include "Ap4Atom.h"
#include "Ap4ByteStream.h"
#include "Ap4Sample.h"
#include "Ap4SampleTable.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4SampleDescription.h"
#include "Ap4FragmentSampleTable.h"
#include "Ap4ContainerAtom.h"
#include "Ap4TrakAtom.h"
#include "Ap4TkhdAtom.h"
#include "Ap4MdhdAtom.h"
#include "Ap4ElstAtom.h"

/*----------------------------------------------------------------------
|   internal helpers
+---------------------------------------------------------------------*/
namespace {

const AP4_UI64 MONTAGE_UI64_MAX = (AP4_UI64)(-1);
const AP4_UI64 MONTAGE_SI64_MAX = ((AP4_UI64)1 << 63) - 1;

//
// Convert a duration expressed in movie timescale ticks into media
// timescale ticks. The conversion must be exact: a remainder means the
// cut point would not land on an integer media tick, which is rejected
// rather than silently rounded.
//
AP4_Result
ExactMovieToMedia(AP4_UI64 movie_ticks,
                  AP4_UI32 movie_time_scale,
                  AP4_UI32 media_time_scale,
                  AP4_UI64& media_ticks)
{
    if (movie_time_scale == 0 || media_time_scale == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    if (movie_ticks != 0 && movie_ticks > MONTAGE_UI64_MAX / media_time_scale) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    AP4_UI64 product = movie_ticks * (AP4_UI64)media_time_scale;
    if (product % (AP4_UI64)movie_time_scale != 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    media_ticks = product / (AP4_UI64)movie_time_scale;
    return AP4_SUCCESS;
}

//
// One piece of a requested range after it has been split on the boundary
// between the optional leading empty edit and the media edit.
//
struct SourceSegment {
    AP4_UI64 duration_movie; // segment duration, in movie timescale ticks
    bool     is_empty;       // true: blank edit, no samples
    AP4_UI64 media_begin;    // visible window start, absolute media time (media ticks)
    AP4_UI64 media_end;      // visible window end,   absolute media time (media ticks)
};

//
// The decode window computed for one media segment.
//
struct BlockPlan {
    AP4_Ordinal first_decode; // first source sample index kept (a sync sample)
    AP4_Ordinal last_decode;  // last source sample index kept, inclusive
    AP4_UI64    block_origin; // DTS of first_decode in the source media timeline
    AP4_UI64    rel_begin;    // visible window start relative to block_origin
};

} // namespace

/*----------------------------------------------------------------------
|   AP4_Track::CreateMontage
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::CreateMontage(const AP4_Array<EditRange>& ranges,
                         AP4_Track*&                  output)
{
    output = NULL;

    // only plain, unfragmented video tracks are handled
    if (m_Type != TYPE_VIDEO) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    if (m_SampleTable == NULL || m_TrakAtom == NULL) {
        return AP4_ERROR_INVALID_STATE;
    }
    if (AP4_DYNAMIC_CAST(AP4_FragmentSampleTable, m_SampleTable) != NULL) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_UI32 movie_time_scale = m_MovieTimeScale;
    AP4_UI32 media_time_scale = GetMediaTimeScale();
    if (movie_time_scale == 0 || media_time_scale == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    if (ranges.ItemCount() == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }

    // encrypted tracks are not supported
    AP4_Cardinal description_count = GetSampleDescriptionCount();
    for (AP4_Ordinal i = 0; i < description_count; i++) {
        AP4_SampleDescription* description = GetSampleDescription(i);
        if (description != NULL &&
            description->GetType() == AP4_SampleDescription::TYPE_PROTECTED) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }

    AP4_UI64 movie_duration = GetDuration(); // movie timescale ticks

    // validate the ranges and compute the exact output track duration
    AP4_UI64 total_movie_duration = 0;
    for (AP4_Ordinal i = 0; i < ranges.ItemCount(); i++) {
        if (ranges[i].begin >= ranges[i].end) {
            return AP4_ERROR_INVALID_PARAMETERS;
        }
        if (ranges[i].end > movie_duration) {
            return AP4_ERROR_INVALID_PARAMETERS;
        }
        AP4_UI64 range_duration = ranges[i].end - ranges[i].begin;
        if (range_duration > MONTAGE_UI64_MAX - total_movie_duration) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        total_movie_duration += range_duration;
    }

    // parse and validate the source edit list: nothing, or an optional
    // leading empty edit followed by exactly one rate-1 media edit
    AP4_Array<AP4_ElstEntry> elst_entries;
    AP4_ElstAtom* source_elst = AP4_DYNAMIC_CAST(
        AP4_ElstAtom, m_TrakAtom->FindChild("edts/elst"));
    if (source_elst != NULL) {
        bool seen_media_edit = false;
        AP4_Cardinal entry_count = source_elst->GetEntries().ItemCount();
        for (AP4_Ordinal i = 0; i < entry_count; i++) {
            const AP4_ElstEntry& entry = source_elst->GetEntries()[i];
            if (entry.m_MediaRate != 1) {
                // dwell (rate 0) or variable-rate edits are not supported
                return AP4_ERROR_NOT_SUPPORTED;
            }
            if (entry.m_MediaTime < 0) {
                if (i != 0 || seen_media_edit) {
                    // only one leading empty edit is supported
                    return AP4_ERROR_NOT_SUPPORTED;
                }
            } else {
                if (seen_media_edit) {
                    // multiple media edits are not supported
                    return AP4_ERROR_NOT_SUPPORTED;
                }
                seen_media_edit = true;
            }
            elst_entries.Append(entry);
        }
    }

    AP4_UI64 empty_duration_movie     = 0; // movie ticks
    bool     has_media_edit           = (source_elst == NULL);
    AP4_SI64 media_edit_media_time    = 0; // media ticks
    AP4_UI64 media_edit_duration_movie = 0;
    if (source_elst != NULL) {
        AP4_Ordinal next = 0;
        if (elst_entries.ItemCount() > 0 && elst_entries[0].m_MediaTime < 0) {
            empty_duration_movie = elst_entries[0].m_SegmentDuration;
            next = 1;
        }
        if (next < elst_entries.ItemCount()) {
            has_media_edit = true;
            media_edit_media_time = elst_entries[next].m_MediaTime;
            media_edit_duration_movie = elst_entries[next].m_SegmentDuration;
        }
    } else {
        // no edit list: one media edit starting at media time 0
        media_edit_media_time = 0;
        media_edit_duration_movie = movie_duration;
    }
    if (media_edit_media_time < 0) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    // split every requested range into blank and media segments
    AP4_Array<SourceSegment> segments;
    for (AP4_Ordinal r = 0; r < ranges.ItemCount(); r++) {
        AP4_UI64 begin = ranges[r].begin;
        AP4_UI64 end   = ranges[r].end;

        // part covered by the leading empty edit
        if (begin < empty_duration_movie) {
            AP4_UI64 split = (end < empty_duration_movie) ? end : empty_duration_movie;
            SourceSegment segment;
            segment.duration_movie = split - begin;
            segment.is_empty       = true;
            segment.media_begin    = 0;
            segment.media_end      = 0;
            segments.Append(segment);
            begin = split;
        }

        // part covered by the media edit
        if (begin < end) {
            if (!has_media_edit) {
                return AP4_ERROR_NOT_SUPPORTED;
            }
            AP4_UI64 offset_begin_movie = begin - empty_duration_movie;
            AP4_UI64 offset_end_movie   = end   - empty_duration_movie;
            if (offset_end_movie > media_edit_duration_movie) {
                return AP4_ERROR_INVALID_PARAMETERS;
            }

            AP4_UI64 offset_begin_media = 0;
            AP4_UI64 offset_end_media   = 0;
            AP4_Result result;
            result = ExactMovieToMedia(offset_begin_movie,
                                       movie_time_scale, media_time_scale,
                                       offset_begin_media);
            if (AP4_FAILED(result)) return result;
            result = ExactMovieToMedia(offset_end_movie,
                                       movie_time_scale, media_time_scale,
                                       offset_end_media);
            if (AP4_FAILED(result)) return result;

            if ((AP4_UI64)media_edit_media_time >
                MONTAGE_UI64_MAX - offset_begin_media) {
                return AP4_ERROR_OUT_OF_RANGE;
            }
            AP4_UI64 media_begin =
                (AP4_UI64)media_edit_media_time + offset_begin_media;
            if ((AP4_UI64)media_edit_media_time >
                MONTAGE_UI64_MAX - offset_end_media) {
                return AP4_ERROR_OUT_OF_RANGE;
            }
            AP4_UI64 media_end =
                (AP4_UI64)media_edit_media_time + offset_end_media;
            if (media_end <= media_begin) {
                return AP4_ERROR_INVALID_PARAMETERS;
            }

            SourceSegment segment;
            segment.is_empty       = false;
            segment.media_begin    = media_begin;
            segment.media_end      = media_end;
            // movie duration of the media part only
            segment.duration_movie = offset_end_movie - offset_begin_movie;
            segments.Append(segment);
        }
    }

    // load the source sample metadata (payloads stay referenced, not copied)
    AP4_Cardinal source_sample_count = GetSampleCount();
    AP4_Array<AP4_Sample> source_samples;
    AP4_Array<AP4_UI64>   cts_start;
    for (AP4_Ordinal i = 0; i < source_sample_count; i++) {
        AP4_Sample sample;
        AP4_Result result = GetSample(i, sample);
        if (AP4_FAILED(result)) return result;
        // negative composition offsets are not supported
        if ((AP4_SI32)sample.GetCtsDelta() < 0) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
        if (sample.GetDuration() > MONTAGE_UI64_MAX - sample.GetCts()) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        source_samples.Append(sample);
        cts_start.Append(sample.GetCts());
    }

    // a media segment requires at least one real sync sample in the source
    bool has_media_segment = false;
    for (AP4_Ordinal i = 0; i < segments.ItemCount(); i++) {
        if (!segments[i].is_empty) {
            has_media_segment = true;
            break;
        }
    }
    if (has_media_segment) {
        bool any_sync = false;
        for (AP4_Ordinal i = 0; i < source_sample_count; i++) {
            if (source_samples[i].IsSync()) {
                any_sync = true;
                break;
            }
        }
        if (!any_sync) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }

    // compute one decode block plan per media segment
    AP4_Array<BlockPlan> block_plans; // aligned with media segments in order
    for (AP4_Ordinal s = 0; s < segments.ItemCount(); s++) {
        if (segments[s].is_empty) continue;

        AP4_UI64 media_begin = segments[s].media_begin;
        AP4_UI64 media_end   = segments[s].media_end;

        // display samples whose CTS interval intersects the cut window
        AP4_Ordinal first_display = 0;
        AP4_Ordinal last_display  = 0;
        bool        any_display   = false;
        for (AP4_Ordinal i = 0; i < source_sample_count; i++) {
            AP4_UI64 cts = cts_start[i];
            AP4_UI64 cts_end = cts + (AP4_UI64)source_samples[i].GetDuration();
            if (cts < media_end && cts_end > media_begin) {
                if (!any_display) {
                    first_display = i;
                    any_display   = true;
                }
                last_display = i;
            }
        }
        if (!any_display) {
            // the visible media timeline is supposed to be fully covered
            return AP4_ERROR_INVALID_PARAMETERS;
        }

        // earliest and latest decode items among the display samples,
        // selected by DTS rather than display-order adjacency
        AP4_Ordinal decode_first = first_display;
        AP4_Ordinal decode_last  = first_display;
        for (AP4_Ordinal i = first_display; i <= last_display; i++) {
            if (source_samples[i].GetDts() <
                source_samples[decode_first].GetDts()) {
                decode_first = i;
            }
            if (source_samples[i].GetDts() >
                source_samples[decode_last].GetDts()) {
                decode_last = i;
            }
        }

        // walk back to the nearest real sync sample opening a closed GOP
        AP4_Ordinal sync_index = 0;
        bool        found_sync = false;
        for (AP4_Ordinal k = decode_first + 1; k-- > 0;) {
            if (source_samples[k].IsSync()) {
                sync_index = k;
                found_sync = true;
                break;
            }
        }
        if (!found_sync) {
            return AP4_ERROR_NOT_SUPPORTED;
        }

        AP4_UI64 block_origin = source_samples[sync_index].GetDts();
        if (block_origin > media_begin) {
            return AP4_ERROR_INVALID_STATE;
        }

        BlockPlan plan;
        plan.first_decode = sync_index;
        plan.last_decode  = decode_last;
        plan.block_origin = block_origin;
        plan.rel_begin    = media_begin - block_origin;
        block_plans.Append(plan);
    }

    // build the output sample table, keeping references to source payloads
    AP4_SyntheticSampleTable* output_table = new AP4_SyntheticSampleTable();
    for (AP4_Ordinal i = 0; i < description_count; i++) {
        AP4_SampleDescription* description = GetSampleDescription(i);
        AP4_SampleDescription* clone = description ? description->Clone() : NULL;
        if (clone == NULL) {
            delete output_table;
            return AP4_ERROR_INTERNAL;
        }
        output_table->AddSampleDescription(clone);
    }

    AP4_Array<AP4_ElstEntry> output_entries;
    AP4_UI64 decode_cursor       = 0; // next block start in output media time
    AP4_UI64 media_duration      = 0; // total output media duration
    AP4_Ordinal next_block_plan  = 0;

    for (AP4_Ordinal s = 0; s < segments.ItemCount(); s++) {
        if (segments[s].is_empty) {
            // blank edits keep their exact movie duration and hold no sample
            if (segments[s].duration_movie > MONTAGE_SI64_MAX) {
                delete output_table;
                return AP4_ERROR_OUT_OF_RANGE;
            }
            output_entries.Append(
                AP4_ElstEntry(segments[s].duration_movie, -1, 1));
            continue;
        }

        BlockPlan& plan = block_plans[next_block_plan++];
        AP4_UI64 block_start = decode_cursor;

        // each media segment gets its own private copy of its decode window,
        // including rollback and in-between samples
        for (AP4_Ordinal k = plan.first_decode; k <= plan.last_decode; k++) {
            AP4_Sample copy = source_samples[k];
            AP4_UI64 source_dts = copy.GetDts();
            if (source_dts < plan.block_origin) {
                delete output_table;
                return AP4_ERROR_INTERNAL;
            }
            AP4_UI64 offset = source_dts - plan.block_origin;
            if (offset > MONTAGE_UI64_MAX - decode_cursor) {
                delete output_table;
                return AP4_ERROR_OUT_OF_RANGE;
            }
            AP4_UI64 new_dts = decode_cursor + offset;

            // use the explicit AddSample overload so that the chunk table
            // stays consistent (a description index change opens a new chunk)
            AP4_ByteStream* data_stream = copy.GetDataStream();
            if (data_stream == NULL) {
                delete output_table;
                return AP4_ERROR_INVALID_STATE;
            }
            AP4_Result add_result = output_table->AddSample(
                *data_stream,
                copy.GetOffset(),
                copy.GetSize(),
                copy.GetDuration(),
                copy.GetDescriptionIndex(),
                new_dts,
                copy.GetCtsDelta(),
                copy.IsSync());
            data_stream->Release();
            if (AP4_FAILED(add_result)) {
                delete output_table;
                return add_result;
            }
        }

        AP4_UI64 last_sample_dts =
            decode_cursor +
            (source_samples[plan.last_decode].GetDts() - plan.block_origin);
        AP4_UI32 last_duration = source_samples[plan.last_decode].GetDuration();
        if ((AP4_UI64)last_duration > MONTAGE_UI64_MAX - last_sample_dts) {
            delete output_table;
            return AP4_ERROR_OUT_OF_RANGE;
        }
        decode_cursor = last_sample_dts + (AP4_UI64)last_duration;
        media_duration = decode_cursor;

        // the edit points at the visible window inside this private block
        if (block_start > MONTAGE_SI64_MAX ||
            plan.rel_begin > MONTAGE_SI64_MAX - block_start ||
            segments[s].duration_movie > MONTAGE_SI64_MAX) {
            delete output_table;
            return AP4_ERROR_OUT_OF_RANGE;
        }
        AP4_UI64 edit_media_time = block_start + plan.rel_begin;
        output_entries.Append(
            AP4_ElstEntry(segments[s].duration_movie,
                          (AP4_SI64)edit_media_time, 1));
    }

    // create the output track with all non-temporal properties of the source
    AP4_Track* result_track = new AP4_Track(output_table,
                                            GetId(),
                                            movie_time_scale,
                                            total_movie_duration,
                                            media_time_scale,
                                            media_duration,
                                            this);

    // attach the output edit list (elst v1 is selected automatically when
    // any field does not fit in 32 bits)
    AP4_ContainerAtom* edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
    AP4_ElstAtom* elst = new AP4_ElstAtom();
    for (AP4_Ordinal i = 0; i < output_entries.ItemCount(); i++) {
        elst->AddEntry(output_entries[i]);
    }
    edts->AddChild(elst);
    result_track->UseTrakAtom()->AddChild(edts);

    // make sure tkhd/mdhd keep their full duration when it exceeds 32 bits
    AP4_TkhdAtom* tkhd = result_track->UseTrakAtom()->UseTkhdAtom();
    AP4_MdhdAtom* mdhd = AP4_DYNAMIC_CAST(
        AP4_MdhdAtom, result_track->UseTrakAtom()->FindChild("mdia/mdhd"));
    if (tkhd != NULL && total_movie_duration > 0xFFFFFFFFULL) {
        tkhd->SetVersion(1);
        tkhd->SetSize(AP4_FULL_ATOM_HEADER_SIZE + 80 + 12);
    }
    if (mdhd != NULL && media_duration > 0xFFFFFFFFULL) {
        mdhd->SetVersion(1);
        mdhd->SetSize(AP4_FULL_ATOM_HEADER_SIZE + 20 + 12);
    }

    output = result_track;
    return AP4_SUCCESS;
}
