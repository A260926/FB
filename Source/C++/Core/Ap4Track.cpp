/*****************************************************************
|
|    AP4 - Track Objects
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
|    along with Bento4|GPL; see the file COPYING.  If not, write to the
|    Free Software Foundation, 59 Temple Place - Suite 330, Boston, MA
|    02111-1307, USA.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4ByteStream.h"
#include "Ap4HdlrAtom.h"
#include "Ap4MvhdAtom.h"
#include "Ap4Track.h"
#include "Ap4Utils.h"
#include "Ap4Sample.h"
#include "Ap4DataBuffer.h"
#include "Ap4TrakAtom.h"
#include "Ap4TkhdAtom.h"
#include "Ap4MoovAtom.h"
#include "Ap4AtomSampleTable.h"
#include "Ap4SdpAtom.h"
#include "Ap4MdhdAtom.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4ContainerAtom.h"
#include "Ap4ElstAtom.h"
#include "Ap4SampleDescription.h"

/*----------------------------------------------------------------------
|   AP4_Track::AP4_Track
+---------------------------------------------------------------------*/
AP4_Track::AP4_Track(Type             type,
                     AP4_SampleTable* sample_table,
                     AP4_UI32         track_id, 
                     AP4_UI32         movie_time_scale,
                     AP4_UI64         track_duration,
                     AP4_UI32         media_time_scale,
                     AP4_UI64         media_duration,
                     const char*      language,
                     AP4_UI32         width,
                     AP4_UI32         height,
                     AP4_UI64         creation_time,
                     AP4_UI64         modification_time) :
    m_TrakAtomIsOwned(true),
    m_Type(type),
    m_SampleTable(sample_table),
    m_SampleTableIsOwned(true),
    m_MovieTimeScale(movie_time_scale ? 
                     movie_time_scale : 
                     AP4_TRACK_DEFAULT_MOVIE_TIMESCALE)
{
    // compute the default volume value
    unsigned int volume = 0;
    if (type == TYPE_AUDIO) volume = 0x100;

    // compute the handler type and name
    AP4_Atom::Type hdlr_type;
    const char* hdlr_name;
    switch (type) {
        case TYPE_AUDIO:
            hdlr_type = AP4_HANDLER_TYPE_SOUN;
            hdlr_name = "Bento4 Sound Handler";
            break;

        case TYPE_VIDEO:
            hdlr_type = AP4_HANDLER_TYPE_VIDE;
            hdlr_name = "Bento4 Video Handler";
            break;

        case TYPE_HINT:
            hdlr_type = AP4_HANDLER_TYPE_HINT;
            hdlr_name = "Bento4 Hint Handler";
            break;

        case TYPE_TEXT:
            hdlr_type = AP4_HANDLER_TYPE_TEXT;
            hdlr_name = "Bento4 Text Handler";
            break;

        case TYPE_SUBTITLES:
            hdlr_type = AP4_HANDLER_TYPE_SUBT;
            hdlr_name = "Bento4 Subtitle Handler";
            break;

        default:
            hdlr_type = 0;
            hdlr_name = NULL;
            break;
    }

    // create a trak atom
    m_TrakAtom = new AP4_TrakAtom(sample_table,
                                  hdlr_type, 
                                  hdlr_name,
                                  track_id,
                                  creation_time,
                                  modification_time,
                                  track_duration,
                                  media_time_scale,
                                  media_duration,
                                  (AP4_UI16)volume, 
                                  language,
                                  width, 
                                  height);
}

/*----------------------------------------------------------------------
|   AP4_Track::AP4_Track
+---------------------------------------------------------------------*/
AP4_Track::AP4_Track(AP4_SampleTable* sample_table,
                     AP4_UI32         track_id, 
                     AP4_UI32         movie_time_scale, // 0 = use default
                     AP4_UI64         track_duration,   // in the movie timescale
                     AP4_UI32         media_time_scale,
                     AP4_UI64         media_duration,   // in the media timescale
                     const AP4_Track* track_prototype) :
    m_TrakAtomIsOwned(true),
    m_Type(track_prototype->m_Type),
    m_SampleTable(sample_table),
    m_SampleTableIsOwned(true),
    m_MovieTimeScale(movie_time_scale ? 
                     movie_time_scale : 
                     AP4_TRACK_DEFAULT_MOVIE_TIMESCALE)
{
    // compute the handler type and name
    AP4_Atom::Type hdlr_type;
    const char* hdlr_name;
    switch (track_prototype->GetType()) {
        case TYPE_AUDIO:
            hdlr_type = AP4_HANDLER_TYPE_SOUN;
            hdlr_name = "Bento4 Sound Handler";
            break;

        case TYPE_VIDEO:
            hdlr_type = AP4_HANDLER_TYPE_VIDE;
            hdlr_name = "Bento4 Video Handler";
            break;

        case TYPE_HINT:
            hdlr_type = AP4_HANDLER_TYPE_HINT;
            hdlr_name = "Bento4 Hint Handler";
            break;

        case TYPE_TEXT:
            hdlr_type = AP4_HANDLER_TYPE_TEXT;
            hdlr_name = "Bento4 Text Handler";
            break;

        case TYPE_SUBTITLES:
            hdlr_type = AP4_HANDLER_TYPE_SUBT;
            hdlr_name = "Bento4 Subtitle Handler";
            break;

        default:
            hdlr_type = track_prototype->GetHandlerType();
            hdlr_name = track_prototype->GetTrackLanguage();
            break;
    }

    // create a trak atom
    const AP4_TkhdAtom* tkhd = NULL;
    if (track_prototype->GetTrakAtom()) {
        tkhd = track_prototype->GetTrakAtom()->GetTkhdAtom();
    }
    m_TrakAtom = new AP4_TrakAtom(sample_table,
                                  hdlr_type, 
                                  hdlr_name,
                                  track_id, 
                                  tkhd?tkhd->GetCreationTime():0,
                                  tkhd?tkhd->GetModificationTime():0, 
                                  track_duration,
                                  media_time_scale,
                                  media_duration,
                                  tkhd?tkhd->GetVolume():(track_prototype->GetType() == TYPE_AUDIO?0x100:0),
                                  track_prototype->GetTrackLanguage(),
                                  track_prototype->GetWidth(),
                                  track_prototype->GetHeight(),
                                  tkhd?tkhd->GetLayer():0,
                                  tkhd?tkhd->GetAlternateGroup():0,
                                  tkhd?tkhd->GetMatrix():NULL);
}

/*----------------------------------------------------------------------
|   AP4_Track::AP4_Track
+---------------------------------------------------------------------*/
AP4_Track::AP4_Track(AP4_TrakAtom&   atom, 
                     AP4_ByteStream& sample_stream, 
                     AP4_UI32        movie_time_scale) :
    m_TrakAtom(&atom),
    m_TrakAtomIsOwned(false),
    m_Type(TYPE_UNKNOWN),
    m_SampleTable(NULL),
    m_SampleTableIsOwned(true),
    m_MovieTimeScale(movie_time_scale)
{
    // find the handler type
    AP4_Atom* sub = atom.FindChild("mdia/hdlr");
    if (sub) {
        AP4_HdlrAtom* hdlr = AP4_DYNAMIC_CAST(AP4_HdlrAtom, sub);
        if (hdlr) {
            AP4_UI32 type = hdlr->GetHandlerType();
            if (type == AP4_HANDLER_TYPE_SOUN) {
                m_Type = TYPE_AUDIO;
            } else if (type == AP4_HANDLER_TYPE_VIDE) {
                m_Type = TYPE_VIDEO;
            } else if (type == AP4_HANDLER_TYPE_HINT) {
                m_Type = TYPE_HINT;
            } else if (type == AP4_HANDLER_TYPE_ODSM ||
                       type == AP4_HANDLER_TYPE_SDSM) {
                m_Type = TYPE_SYSTEM;
            } else if (type == AP4_HANDLER_TYPE_TEXT ||
                       type == AP4_HANDLER_TYPE_TX3G) {
                m_Type = TYPE_TEXT;
            } else if (type == AP4_HANDLER_TYPE_JPEG) {
                m_Type = TYPE_JPEG;
            } else if (type == AP4_HANDLER_TYPE_SUBT ||
                       type == AP4_HANDLER_TYPE_SBTL) {
                m_Type = TYPE_SUBTITLES;
            }
        }
    }

    // create a facade for the stbl atom
    AP4_ContainerAtom* stbl = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom.FindChild("mdia/minf/stbl"));
    if (stbl) {
        m_SampleTable = new AP4_AtomSampleTable(stbl, sample_stream);
    }
}

/*----------------------------------------------------------------------
|   AP4_Track::~AP4_Track
+---------------------------------------------------------------------*/
AP4_Track::~AP4_Track()
{
    if (m_TrakAtomIsOwned) delete m_TrakAtom;
    if (m_SampleTableIsOwned) delete m_SampleTable;
}

/*----------------------------------------------------------------------
|   AP4_Track::Clone
+---------------------------------------------------------------------*/
AP4_Track* 
AP4_Track::Clone(AP4_Result* result)
{
    AP4_SyntheticSampleTable* sample_table = new AP4_SyntheticSampleTable();
    
    // default return value
    if (result) *result = AP4_SUCCESS;
    
    // add clones of the sample descriptions to the new sample table
    for (unsigned int i=0; ;i++) {
        AP4_SampleDescription* sample_description = GetSampleDescription(i);
        if (sample_description == NULL) break;
        sample_table->AddSampleDescription(sample_description->Clone());
    }

    AP4_Sample  sample;
    AP4_Ordinal index = 0;
    while (AP4_SUCCEEDED(GetSample(index, sample))) {
        AP4_ByteStream* data_stream;
        data_stream = sample.GetDataStream();
        sample_table->AddSample(*data_stream,
                                sample.GetOffset(),
                                sample.GetSize(),
                                sample.GetDuration(),
                                sample.GetDescriptionIndex(),
                                sample.GetDts(),
                                sample.GetCtsDelta(),
                                sample.IsSync());
        AP4_RELEASE(data_stream); // release our ref, the table has kept its own ref.
        index++;
    }    
    
    // create the cloned track
    AP4_Track* clone = new AP4_Track(sample_table,
                                     GetId(),
                                     GetMovieTimeScale(),
                                     GetDuration(),
                                     GetMediaTimeScale(),
                                     GetMediaDuration(),
                                     this);

    return clone;
}

/*----------------------------------------------------------------------
|   AP4_Track::CreateMontage - helpers
+---------------------------------------------------------------------*/
namespace {

/* Convert a duration in movie ticks to media ticks, requiring an exact
   (integer) conversion, with overflow detection. */
static AP4_Result
Montage_ToMediaTicks(AP4_UI64  movie_ticks,
                     AP4_UI32  movie_time_scale,
                     AP4_UI32  media_time_scale,
                     AP4_UI64& media_ticks)
{
    const AP4_UI64 max_ui64 = (AP4_UI64)(-1);
    if (movie_ticks != 0 && movie_ticks > max_ui64/media_time_scale) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    AP4_UI64 product = movie_ticks*media_time_scale;
    if (product % movie_time_scale != 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    media_ticks = product/movie_time_scale;
    return AP4_SUCCESS;
}

/* Add two unsigned 64-bit values, detecting overflow. */
static AP4_Result
Montage_AddChecked(AP4_UI64 a, AP4_UI64 b, AP4_UI64& result)
{
    const AP4_UI64 max_ui64 = (AP4_UI64)(-1);
    if (b > max_ui64-a) return AP4_ERROR_INVALID_PARAMETERS;
    result = a+b;
    return AP4_SUCCESS;
}

} /* anonymous namespace */

/*----------------------------------------------------------------------
|   AP4_Track::CreateMontage
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::CreateMontage(const AP4_Array<EditRange>& ranges,
                         AP4_Track*&                 output)
{
    output = NULL;

    // only plain, non-fragmented video tracks are supported
    if (m_Type != TYPE_VIDEO) {
        return AP4_ERROR_NOT_SUPPORTED;
    }
    if (m_SampleTable == NULL || m_TrakAtom == NULL) {
        return AP4_ERROR_INVALID_STATE;
    }

    AP4_UI32 movie_time_scale = m_MovieTimeScale;
    AP4_UI32 media_time_scale = GetMediaTimeScale();
    if (movie_time_scale == 0 || media_time_scale == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }

    // validate the request itself
    AP4_Cardinal range_count = ranges.ItemCount();
    if (range_count == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    AP4_UI64 track_duration = GetDuration();
    const AP4_UI64 max_ui64 = (AP4_UI64)(-1);
    for (AP4_Ordinal r=0; r<range_count; r++) {
        if (ranges[r].begin >= ranges[r].end ||
            ranges[r].end > track_duration) {
            return AP4_ERROR_INVALID_PARAMETERS;
        }
    }

    // fragmented tracks are not supported: a plain, non-fragmented track
    // has its sample locations in an stco or co64 atom inside the stbl
    AP4_ContainerAtom* stbl_container = AP4_DYNAMIC_CAST(
        AP4_ContainerAtom, m_TrakAtom->FindChild("mdia/minf/stbl"));
    if (stbl_container == NULL ||
        (stbl_container->GetChild(AP4_ATOM_TYPE_STCO) == NULL &&
         stbl_container->GetChild(AP4_ATOM_TYPE_CO64) == NULL)) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    // encrypted tracks are not supported: reject the common CENC atoms
    if (stbl_container) {
        if (stbl_container->GetChild(AP4_ATOM_TYPE_SENC) != NULL ||
            stbl_container->GetChild(AP4_ATOM_TYPE_SAIO) != NULL ||
            stbl_container->GetChild(AP4_ATOM_TYPE_SAIZ) != NULL) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }
    for (AP4_Ordinal d=0; d<GetSampleDescriptionCount(); d++) {
        AP4_SampleDescription* desc = GetSampleDescription(d);
        if (desc && desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }

    // parse the source edit list: either no edts, or one optional leading
    // empty edit followed by a single rate=1 media edit
    AP4_UI64 empty_duration = 0; // movie ticks of the leading empty edit
    AP4_SI64 media_origin   = 0; // media time at which the media edit starts
    {
        AP4_Atom* edts_atom = m_TrakAtom->FindChild("edts");
        if (edts_atom) {
            AP4_ContainerAtom* edts = AP4_DYNAMIC_CAST(AP4_ContainerAtom, edts_atom);
            AP4_ElstAtom* elst = edts ?
                AP4_DYNAMIC_CAST(AP4_ElstAtom, edts->GetChild(AP4_ATOM_TYPE_ELST)) : NULL;
            if (elst == NULL) return AP4_ERROR_NOT_SUPPORTED;

            AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
            if (entries.ItemCount() == 0) return AP4_ERROR_NOT_SUPPORTED;
            AP4_Cardinal media_edit_count = 0;
            for (AP4_Ordinal e=0; e<entries.ItemCount(); e++) {
                const AP4_ElstEntry& entry = entries[e];
                bool is_empty = (entry.m_MediaTime == -1);
                // dwell and variable-rate edits are not supported
                if (entry.m_MediaRate != 1) return AP4_ERROR_NOT_SUPPORTED;
                if (is_empty) {
                    // empty edits are only allowed as a single leading edit
                    if (media_edit_count != 0 || e != 0) {
                        return AP4_ERROR_NOT_SUPPORTED;
                    }
                    AP4_UI64 added = 0;
                    AP4_Result check = Montage_AddChecked(empty_duration,
                                                          entry.m_SegmentDuration,
                                                          added);
                    if (AP4_FAILED(check)) return check;
                    empty_duration = added;
                } else {
                    if (entry.m_MediaTime < 0) return AP4_ERROR_NOT_SUPPORTED;
                    // only one media edit is supported
                    if (media_edit_count != 0) return AP4_ERROR_NOT_SUPPORTED;
                    media_edit_count++;
                    media_origin = entry.m_MediaTime;
                }
            }
            if (media_edit_count != 1) return AP4_ERROR_NOT_SUPPORTED;
        }
    }

    // snapshot the source sample timeline (decode order is table order)
    AP4_Cardinal sample_count = GetSampleCount();
    AP4_Array<AP4_UI64> sample_dts;
    AP4_Array<AP4_UI64> sample_cts;
    AP4_Array<AP4_UI32> sample_duration;
    AP4_Array<bool>     sample_sync;
    if (sample_count != 0) {
        sample_dts.SetItemCount(sample_count);
        sample_cts.SetItemCount(sample_count);
        sample_duration.SetItemCount(sample_count);
        sample_sync.SetItemCount(sample_count);
        for (AP4_Ordinal i=0; i<sample_count; i++) {
            AP4_Sample sample;
            AP4_Result result = GetSample(i, sample);
            if (AP4_FAILED(result)) return result;
            // negative composition offsets are not supported
            if ((AP4_SI32)sample.GetCtsDelta() < 0) {
                return AP4_ERROR_NOT_SUPPORTED;
            }
            sample_dts[i]      = sample.GetDts();
            sample_cts[i]      = sample.GetCts();
            sample_duration[i] = sample.GetDuration();
            sample_sync[i]     = sample.IsSync();
        }
    }

    // build the output sample table, keeping the source sample descriptions
    AP4_Result result = AP4_SUCCESS;
    AP4_SyntheticSampleTable* out_table = new AP4_SyntheticSampleTable();
    for (AP4_Ordinal i=0; ; i++) {
        AP4_SampleDescription* sample_description = GetSampleDescription(i);
        if (sample_description == NULL) break;
        AP4_SampleDescription* clone = sample_description->Clone(&result);
        if (clone == NULL || AP4_FAILED(result)) {
            delete clone;
            delete out_table;
            return result != AP4_SUCCESS ? result : AP4_FAILURE;
        }
        result = out_table->AddSampleDescription(clone);
        if (AP4_FAILED(result)) { delete clone; delete out_table; return result; }
    }

    // the edit list is built standalone: mutating an elst that already
    // has a parent would not propagate its new size up the atom tree
    AP4_ElstAtom* out_elst = new AP4_ElstAtom();
    AP4_ContainerAtom* out_edts = NULL;
    AP4_Track* montage = NULL;
    AP4_UI64 out_movie_duration = 0; // movie ticks
    AP4_UI64 out_media_duration = 0; // media ticks (decode tail)
    AP4_UI64 out_media_tail = 0;     // media ticks (last edit end)
    AP4_UI64 mdhd_duration = 0;      // media ticks (mdhd duration)
    AP4_Cardinal out_sample_count = 0;
    for (AP4_Ordinal r=0; r<range_count; r++) {
        AP4_UI64 range_begin = ranges[r].begin;
        AP4_UI64 range_end   = ranges[r].end;

        // part of the range that falls inside the leading empty edit
        AP4_UI64 empty_part = 0;
        AP4_UI64 media_begin_movie = range_begin;
        AP4_UI64 media_end_movie   = range_end;
        if (range_begin < empty_duration) {
            if (range_end <= empty_duration) {
                // whole range is empty time
                empty_part = range_end-range_begin;
                media_begin_movie = empty_duration;
                media_end_movie   = empty_duration;
            } else {
                empty_part = empty_duration-range_begin;
                media_begin_movie = empty_duration;
            }
        }

        if (empty_part != 0) {
            AP4_UI64 new_duration = 0;
            result = Montage_AddChecked(out_movie_duration, empty_part, new_duration);
            if (AP4_FAILED(result)) goto end;
            out_movie_duration = new_duration;
            // empty edits keep the same length of blank time, no samples attached
            result = out_elst->AddEntry(AP4_ElstEntry(empty_part, -1, 1));
            if (AP4_FAILED(result)) goto end;
        }

        if (media_end_movie <= media_begin_movie) continue;

        // convert the media cut [media_begin_movie, media_end_movie) to
        // exact media ticks
        AP4_UI64 begin_offset_media, end_offset_media;
        result = Montage_ToMediaTicks(media_begin_movie-empty_duration,
                                      movie_time_scale,
                                      media_time_scale,
                                      begin_offset_media);
        if (AP4_FAILED(result)) goto end;
        result = Montage_ToMediaTicks(media_end_movie-empty_duration,
                                      movie_time_scale,
                                      media_time_scale,
                                      end_offset_media);
        if (AP4_FAILED(result)) goto end;
        if ((AP4_UI64)media_origin > max_ui64-begin_offset_media ||
            (AP4_UI64)media_origin > max_ui64-end_offset_media) {
            result = AP4_ERROR_INVALID_PARAMETERS;
            goto end;
        }
        AP4_UI64 cut_begin_media = (AP4_UI64)media_origin+begin_offset_media;
        AP4_UI64 cut_end_media   = (AP4_UI64)media_origin+end_offset_media;

        // display samples intersecting the cut, decided by CTS intervals
        // (never by decode-order adjacency)
        bool found = false;
        AP4_Ordinal first_display = 0;
        AP4_Ordinal last_display  = 0;
        for (AP4_Ordinal i=0; i<sample_count; i++) {
            AP4_UI64 cts = sample_cts[i];
            AP4_UI64 cts_end = 0;
            if (sample_duration[i] > max_ui64-cts) {
                result = AP4_ERROR_INVALID_PARAMETERS;
                goto end;
            }
            cts_end = cts+sample_duration[i];
            if (cts < cut_end_media && cts_end > cut_begin_media) {
                if (!found) {
                    first_display = i;
                    last_display  = i;
                    found = true;
                } else {
                    if (i < first_display) first_display = i;
                    if (i > last_display)  last_display  = i;
                }
            }
        }
        if (!found) {
            // a media cut with no covering sample cannot be assembled
            result = AP4_ERROR_INVALID_FORMAT;
            goto end;
        }

        // roll-in: walk back from the earliest decode item to a real sync
        AP4_Ordinal block_start = first_display;
        bool have_sync = false;
        while (true) {
            if (sample_sync[block_start]) { have_sync = true; break; }
            if (block_start == 0) break;
            --block_start;
        }
        if (!have_sync) {
            // do not silently treat sample 0 as a sync point
            result = AP4_ERROR_NOT_SUPPORTED;
            goto end;
        }
        AP4_Ordinal block_end = last_display;

        // The cut can start before the DTS of the block's first sample
        // (the media timeline there is simply empty). Keep that gap so
        // the edit's media_time never has to be negative.
        AP4_UI64 block_dts_origin = sample_dts[block_start];
        AP4_UI64 lead_gap = 0;
        if (block_dts_origin > cut_begin_media) {
            lead_gap = block_dts_origin-cut_begin_media;
        }
        AP4_UI64 block_base = 0; // new DTS of the block's first sample
        result = Montage_AddChecked(out_media_duration, lead_gap, block_base);
        if (AP4_FAILED(result)) goto end;

        // copy the closed decode run [block_start, block_end], each cut
        // carrying its own independent copy of the samples
        for (AP4_Ordinal i=block_start; i<=block_end; i++) {
            AP4_Sample sample;
            result = GetSample(i, sample);
            if (AP4_FAILED(result)) goto end;
            AP4_ByteStream* data_stream = sample.GetDataStream();
            if (data_stream == NULL) {
                result = AP4_ERROR_INVALID_STATE;
                goto end;
            }
            AP4_UI64 relative_dts = sample_dts[i]-block_dts_origin;
            if (relative_dts > max_ui64-block_base) {
                data_stream->Release();
                result = AP4_ERROR_INVALID_PARAMETERS;
                goto end;
            }
            AP4_UI64 new_dts = block_base+relative_dts;
            result = out_table->AddSample(*data_stream,
                                          sample.GetOffset(),
                                          sample.GetSize(),
                                          sample.GetDuration(),
                                          sample.GetDescriptionIndex(),
                                          new_dts,
                                          sample.GetCtsDelta(),
                                          sample.IsSync());
            data_stream->Release();
            if (AP4_FAILED(result)) goto end;
            if (out_sample_count == 0xFFFFFFFF) {
                result = AP4_ERROR_INVALID_PARAMETERS;
                goto end;
            }
            ++out_sample_count;
        }

        // span of this block on the output media timeline
        AP4_UI64 dts_span = sample_dts[block_end]-block_dts_origin;
        if (sample_duration[block_end] > max_ui64-dts_span) {
            result = AP4_ERROR_INVALID_PARAMETERS;
            goto end;
        }
        AP4_UI64 block_span = dts_span+sample_duration[block_end];
        AP4_UI64 new_media_duration = 0;
        result = Montage_AddChecked(block_base, block_span, new_media_duration);
        if (AP4_FAILED(result)) goto end;

        // the output edit points at the new copy of the samples and hides
        // the roll-in and any tail pictures outside the requested cut.
        // the block's media timeline is shifted by a constant, so the cut
        // position maps directly (this also holds when display and decode
        // orders differ).
        AP4_UI64 cut_offset = 0;
        if (cut_begin_media >= block_dts_origin) {
            cut_offset = cut_begin_media-block_dts_origin;
        }
        if (cut_offset > max_ui64-block_base) {
            result = AP4_ERROR_INVALID_PARAMETERS;
            goto end;
        }
        AP4_UI64 edit_media_time = block_base+cut_offset;
        if (edit_media_time > (AP4_UI64)0x7FFFFFFFFFFFFFFFULL) {
            // an elst media time is a signed 64-bit field
            result = AP4_ERROR_INVALID_PARAMETERS;
            goto end;
        }
        AP4_UI64 edit_duration_movie = media_end_movie-media_begin_movie;

        AP4_UI64 new_movie_duration = 0;
        result = Montage_AddChecked(out_movie_duration,
                                    edit_duration_movie,
                                    new_movie_duration);
        if (AP4_FAILED(result)) goto end;

        result = out_elst->AddEntry(AP4_ElstEntry(edit_duration_movie,
                                                  (AP4_SI64)edit_media_time,
                                                  1));
        if (AP4_FAILED(result)) {
            out_media_duration = new_media_duration;
            goto end;
        }
        // the mdhd duration must also cover the end of the last displayed
        // picture, which can extend past the decode tail due to CTS deltas
        AP4_UI64 edit_media_end = 0;
        result = Montage_AddChecked(edit_media_time,
                                    cut_end_media-cut_begin_media,
                                    edit_media_end);
        if (AP4_FAILED(result)) goto end;
        if (edit_media_end > out_media_tail) out_media_tail = edit_media_end;
        out_media_duration = new_media_duration;
        out_movie_duration = new_movie_duration;
    }

    // create the output track: non-temporal properties come from the source
    mdhd_duration =
        out_media_duration > out_media_tail ? out_media_duration : out_media_tail;
    montage = new AP4_Track(out_table,
                            GetId(),
                            movie_time_scale,
                            out_movie_duration,
                            media_time_scale,
                            mdhd_duration,
                            this);
    // attach the edit list (tkhd, edts, mdia order). The edts is created
    // only now so that the elst's final size is observed when added.
    out_edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
    out_edts->AddChild(out_elst);
    montage->UseTrakAtom()->AddChild(out_edts, 1);

    output = montage;
    return AP4_SUCCESS;

end:
    delete out_elst;
    delete out_table;
    return result;
}

/*----------------------------------------------------------------------
|   AP4_Track::Attach
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::Attach(AP4_MoovAtom* moov)
{
    if (!m_TrakAtomIsOwned) return AP4_ERROR_INTERNAL;
    moov->AddChild(m_TrakAtom);
    m_TrakAtomIsOwned = false;

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetFlags
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetFlags() const
{
    if (m_TrakAtom) {
        AP4_TkhdAtom* tkhd = AP4_DYNAMIC_CAST(AP4_TkhdAtom, m_TrakAtom->FindChild("tkhd"));
        if (tkhd) {
            return tkhd->GetFlags();
        }
    }
    return 0;
}

/*----------------------------------------------------------------------
|   AP4_Track::SetFlags
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::SetFlags(AP4_UI32 flags)
{
    if (m_TrakAtom) {
        AP4_TkhdAtom* tkhd = AP4_DYNAMIC_CAST(AP4_TkhdAtom, m_TrakAtom->FindChild("tkhd"));
        if (tkhd) {
            tkhd->SetFlags(flags);
            return AP4_SUCCESS;
        }
    }
    return AP4_ERROR_INVALID_STATE;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetHandlerType
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetHandlerType() const
{
    if (m_TrakAtom) {
        AP4_HdlrAtom* hdlr = AP4_DYNAMIC_CAST(AP4_HdlrAtom, m_TrakAtom->FindChild("mdia/hdlr"));
        if (hdlr) {
            return hdlr->GetHandlerType();
        }
    }
    return 0;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetId
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetId() const
{
    return m_TrakAtom->GetId();
}

/*----------------------------------------------------------------------
|   AP4_Track::GetWidth
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetWidth() const
{
    return m_TrakAtom->GetWidth();
}

/*----------------------------------------------------------------------
|   AP4_Track::GetHeight
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetHeight() const
{
    return m_TrakAtom->GetHeight();
}

/*----------------------------------------------------------------------
|   AP4_Track::SetId
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::SetId(AP4_UI32 id)
{
    m_TrakAtom->SetId(id);

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetDuration
+---------------------------------------------------------------------*/
AP4_UI64
AP4_Track::GetDuration() const
{
    return m_TrakAtom->GetDuration();
}

/*----------------------------------------------------------------------
|   AP4_Track::GetDurationMs
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetDurationMs() const
{
    AP4_UI64 duration = m_TrakAtom->GetDuration();
    return AP4_DurationMsFromUnits(duration, m_MovieTimeScale);
}

/*----------------------------------------------------------------------
|   AP4_Track::GetSampleCount
+---------------------------------------------------------------------*/
AP4_Cardinal
AP4_Track::GetSampleCount() const
{
    // delegate to the sample table
    return m_SampleTable ? m_SampleTable->GetSampleCount() : 0;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetSample
+---------------------------------------------------------------------*/
AP4_Result 
AP4_Track::GetSample(AP4_Ordinal index, AP4_Sample& sample)
{
    // delegate to the sample table
    return m_SampleTable ? m_SampleTable->GetSample(index, sample) : AP4_FAILURE;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetSampleDescription
+---------------------------------------------------------------------*/
AP4_SampleDescription*
AP4_Track::GetSampleDescription(AP4_Ordinal index)
{
    // delegate to the sample table
    return m_SampleTable ? m_SampleTable->GetSampleDescription(index) : NULL;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetSampleDescriptionCount
+---------------------------------------------------------------------*/
AP4_Cardinal
AP4_Track::GetSampleDescriptionCount()
{
    // delegate to the sample table
    return m_SampleTable ? m_SampleTable->GetSampleDescriptionCount() : 0;
}

/*----------------------------------------------------------------------
|   AP4_Track::ReadSample
+---------------------------------------------------------------------*/
AP4_Result   
AP4_Track::ReadSample(AP4_Ordinal     index, 
                      AP4_Sample&     sample,
                      AP4_DataBuffer& data)
{
    AP4_Result result;

    // get the sample
    result = GetSample(index, sample);
    if (AP4_FAILED(result)) return result;

    // read the data
    return sample.ReadData(data);
}

/*----------------------------------------------------------------------
|   AP4_Track::GetSampleIndexForTimeStampMs
+---------------------------------------------------------------------*/
AP4_Result  
AP4_Track::GetSampleIndexForTimeStampMs(AP4_UI32 ts_ms, AP4_Ordinal& index)
{
    if (m_SampleTable == NULL) return AP4_ERROR_INVALID_STATE;

    // convert the ts in the timescale of the track's media
    AP4_UI64 ts = AP4_ConvertTime(ts_ms, 1000, GetMediaTimeScale());

    return m_SampleTable->GetSampleIndexForTimeStamp(ts, index);
}

/*----------------------------------------------------------------------
|   AP4_Track::GetNearestSyncSampleIndex
+---------------------------------------------------------------------*/
AP4_Ordinal  
AP4_Track::GetNearestSyncSampleIndex(AP4_Ordinal index, bool before /* = true */)
{
    if (m_SampleTable == NULL) return index;
    return m_SampleTable->GetNearestSyncSampleIndex(index, before);
}

/*----------------------------------------------------------------------
|   AP4_Track::SetMovieTimeScale
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::SetMovieTimeScale(AP4_UI32 time_scale)
{
    // check that we can convert
    if (m_MovieTimeScale == 0) return AP4_FAILURE;

    // convert from one time scale to the other
    m_TrakAtom->SetDuration(AP4_ConvertTime(m_TrakAtom->GetDuration(), 
                                            m_MovieTimeScale,
                                            time_scale));
    
    // keep the new movie timescale
    m_MovieTimeScale = time_scale;

    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetMediaTimeScale
+---------------------------------------------------------------------*/
AP4_UI32
AP4_Track::GetMediaTimeScale() const
{
    return m_TrakAtom?m_TrakAtom->GetMediaTimeScale():0;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetMediaDuration
+---------------------------------------------------------------------*/
AP4_UI64
AP4_Track::GetMediaDuration() const
{
    return m_TrakAtom?m_TrakAtom->GetMediaDuration():0;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetTrackName
+---------------------------------------------------------------------*/
const char*
AP4_Track::GetTrackName() const
{
    if (AP4_HdlrAtom* hdlr = AP4_DYNAMIC_CAST(AP4_HdlrAtom, m_TrakAtom->FindChild("mdia/hdlr"))) {
        return hdlr->GetHandlerName().GetChars();
    }
    return NULL;
}

/*----------------------------------------------------------------------
|   AP4_Track::GetTrackLanguage
+---------------------------------------------------------------------*/
const char*
AP4_Track::GetTrackLanguage() const
{
    if (AP4_MdhdAtom* mdhd = AP4_DYNAMIC_CAST(AP4_MdhdAtom, m_TrakAtom->FindChild("mdia/mdhd"))) {
        return mdhd->GetLanguage().GetChars();
    }
    return NULL;
}

/*----------------------------------------------------------------------
|   AP4_Track::SetTrackLanguage
+---------------------------------------------------------------------*/
AP4_Result
AP4_Track::SetTrackLanguage(const char* language)
{
    if (strlen(language) != 3) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }

    if (AP4_MdhdAtom* mdhd = AP4_DYNAMIC_CAST(AP4_MdhdAtom, m_TrakAtom->FindChild("mdia/mdhd"))) {
        return mdhd->SetLanguage(language);
    }
    return AP4_ERROR_INVALID_STATE;
}
