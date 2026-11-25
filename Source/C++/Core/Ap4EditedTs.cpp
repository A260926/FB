/*****************************************************************
|
|    AP4 - Edited Movie to MPEG2-TS Writer
|
|    Copyright 2002-2025 Axiomatic Systems, LLC
|
|    This file is part of Bento4/AP4.
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|    Bento4|GPL is free software; you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation; either version 2, or (at your option)
|    any later version.
|
 ****************************************************************/

/*----------------------------------------------------------------------
|   includes
+---------------------------------------------------------------------*/
#include "Ap4.h"
#include "Ap4EditedTs.h"

#include <algorithm>
#include <set>
#include <vector>

#if defined(__SIZEOF_INT128__)
typedef __uint128_t AP4_EditedTsU128;
#define AP4_EDITED_TS_HAVE_U128 1
#endif

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
static const AP4_UI64 AP4_EDITED_TS_CLOCK_OFFSET_90K = 9000;

/*----------------------------------------------------------------------
|   structures
+---------------------------------------------------------------------*/
namespace {
struct SampleInfo {
    AP4_Ordinal index;
    AP4_UI64    dts;
    AP4_UI64    cts;
    AP4_UI32    duration;
    AP4_Ordinal description_index;
    bool        sync;
};

struct PlanItem {
    AP4_Track*   track;
    bool         is_video;
    AP4_Ordinal  sample_index;
    AP4_Ordinal  description_index;
    AP4_UI64     dts90;
    AP4_UI64     pts90;
    AP4_UI32     order;
};
}

/*----------------------------------------------------------------------
|   helpers
+---------------------------------------------------------------------*/
static AP4_UI64 EditedTsUi64Max()
{
    return ~(AP4_UI64)0;
}

static AP4_Result EditedTsScaleFloor90k(AP4_UI64 value,
                                        AP4_UI32 timescale,
                                        AP4_UI64& result)
{
    if (timescale == 0) return AP4_ERROR_INVALID_PARAMETERS;
#ifdef AP4_EDITED_TS_HAVE_U128
    AP4_EditedTsU128 scaled = (AP4_EditedTsU128)value*(AP4_EditedTsU128)90000U;
    AP4_EditedTsU128 quotient = scaled/(AP4_EditedTsU128)timescale;
    if (quotient > (AP4_EditedTsU128)EditedTsUi64Max()) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    result = (AP4_UI64)quotient;
    return AP4_SUCCESS;
#else
    // The build environments for this feature provide a 128-bit type.
    AP4_UI64 quotient = value/timescale;
    AP4_UI64 remainder = value%timescale;
    if (quotient > EditedTsUi64Max()/90000) return AP4_ERROR_OUT_OF_RANGE;
    AP4_UI64 base = quotient*90000;
    AP4_UI64 extra = remainder*90000/timescale;
    if (extra > EditedTsUi64Max()-base) return AP4_ERROR_OUT_OF_RANGE;
    result = base+extra;
    return AP4_SUCCESS;
#endif
}

static AP4_Result EditedTsProject90k(AP4_UI64 movie_start_movie_units,
                                     AP4_UI64 source_time_media_units,
                                     AP4_UI64 media_start_media_units,
                                     AP4_UI32 movie_timescale,
                                     AP4_UI32 media_timescale,
                                     AP4_UI64& result)
{
    if (movie_timescale == 0 || media_timescale == 0) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    if (source_time_media_units < media_start_media_units) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }

#ifdef AP4_EDITED_TS_HAVE_U128
    AP4_EditedTsU128 denominator =
        (AP4_EditedTsU128)movie_timescale*(AP4_EditedTsU128)media_timescale;
    AP4_EditedTsU128 numerator =
        (AP4_EditedTsU128)movie_start_movie_units*(AP4_EditedTsU128)90000U*
            (AP4_EditedTsU128)media_timescale +
        (AP4_EditedTsU128)(source_time_media_units-media_start_media_units)*
            (AP4_EditedTsU128)90000U*(AP4_EditedTsU128)movie_timescale;
    AP4_EditedTsU128 quotient = numerator/denominator;
    if (quotient > (AP4_EditedTsU128)EditedTsUi64Max()) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    result = (AP4_UI64)quotient;
    return AP4_SUCCESS;
#else
    (void)movie_start_movie_units;
    return AP4_ERROR_NOT_SUPPORTED;
#endif
}

static AP4_Result EditedTsValidateAtomTree(AP4_Atom* atom)
{
    if (atom == NULL) return AP4_SUCCESS;

    // Protected sample entries contain an 'sinf' atom.
    if (atom->GetType() == AP4_ATOM_TYPE_SINF) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    // A self-contained URL reference has full-atom flag bit 0 set.
    AP4_UrlAtom* url = AP4_DYNAMIC_CAST(AP4_UrlAtom, atom);
    if (url && (url->GetFlags() & 0x1) == 0) {
        return AP4_ERROR_NOT_SUPPORTED;
    }

    AP4_ContainerAtom* container = AP4_DYNAMIC_CAST(AP4_ContainerAtom, atom);
    if (container) {
        for (AP4_List<AP4_Atom>::Item* item = container->GetChildren().FirstItem();
             item;
             item = item->GetNext()) {
            AP4_Result result = EditedTsValidateAtomTree(item->GetData());
            if (AP4_FAILED(result)) return result;
        }
    }
    return AP4_SUCCESS;
}

static AP4_Result EditedTsGatherSamples(AP4_Track*               track,
                                        std::vector<SampleInfo>& samples,
                                        std::set<AP4_UI64>&      dts_boundaries)
{
    AP4_Cardinal count = track->GetSampleCount();
    samples.reserve(count);

    bool have_previous = false;
    AP4_UI64 previous_dts = 0;
    for (AP4_Ordinal i=0; i<count; ++i) {
        AP4_Sample sample;
        AP4_Result result = track->GetSample(i, sample);
        if (AP4_FAILED(result)) return result;

        AP4_UI64 dts = sample.GetDts();
        AP4_UI64 cts = sample.GetCts();
        if (have_previous && dts < previous_dts) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        if (cts < dts) {
            // This writer's contract is limited to non-negative composition
            // offsets.  B-frame reordering may still make CTS non-monotonic.
            return AP4_ERROR_NOT_SUPPORTED;
        }

        AP4_Ordinal description_index = sample.GetDescriptionIndex();
        if (description_index >= track->GetSampleDescriptionCount() ||
            track->GetSampleDescription(description_index) == NULL) {
            return AP4_ERROR_INVALID_FORMAT;
        }

        SampleInfo info;
        info.index = i;
        info.dts = dts;
        info.cts = cts;
        info.duration = sample.GetDuration();
        info.description_index = description_index;
        info.sync = sample.IsSync();
        samples.push_back(info);

        dts_boundaries.insert(dts);
        AP4_UI64 ui64_max = EditedTsUi64Max();
        if (info.duration > ui64_max-dts) return AP4_ERROR_OUT_OF_RANGE;
        dts_boundaries.insert(dts+(AP4_UI64)info.duration);

        previous_dts = dts;
        have_previous = true;
    }

    return AP4_SUCCESS;
}

static AP4_Result EditedTsValidateDescription(AP4_Track* track,
                                             bool       is_video,
                                             AP4_Ordinal description_index)
{
    AP4_SampleDescription* description = track->GetSampleDescription(description_index);
    if (description == NULL) return AP4_ERROR_INVALID_FORMAT;

    if (is_video) {
        if (description->GetType() != AP4_SampleDescription::TYPE_AVC) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
        AP4_AvcSampleDescription* avc =
            AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, description);
        if (avc == NULL) return AP4_ERROR_NOT_SUPPORTED;
    } else {
        if (description->GetFormat() != AP4_SAMPLE_FORMAT_MP4A) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
        AP4_MpegAudioSampleDescription* aac =
            AP4_DYNAMIC_CAST(AP4_MpegAudioSampleDescription, description);
        if (aac == NULL ||
            aac->GetMpeg4AudioObjectType() != AP4_MPEG4_AUDIO_OBJECT_TYPE_AAC_LC) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
    }
    return AP4_SUCCESS;
}

static AP4_Result EditedTsAppendSegment(AP4_Track*                     track,
                                        bool                           is_video,
                                        const std::vector<SampleInfo>& samples,
                                        const std::set<AP4_UI64>&      dts_boundaries,
                                        AP4_UI64                       media_start,
                                        AP4_UI64                       media_end,
                                        AP4_UI64                       movie_start,
                                        AP4_UI32                       movie_timescale,
                                        AP4_UI32                       media_timescale,
                                        std::vector<PlanItem>&         plan)
{
    if (media_end <= media_start) return AP4_ERROR_INVALID_FORMAT;
    if (dts_boundaries.find(media_start) == dts_boundaries.end() ||
        dts_boundaries.find(media_end) == dts_boundaries.end()) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    bool found_first = false;
    AP4_Ordinal segment_description = 0;
    bool have_segment_description = false;
    AP4_UI64 min_cts = EditedTsUi64Max();
    AP4_UI64 max_cts = 0;

    for (AP4_Ordinal i=0; i<samples.size(); ++i) {
        const SampleInfo& sample = samples[i];
        if (sample.dts < media_start || sample.dts >= media_end) continue;

        if (!found_first) {
            if (is_video && !sample.sync) return AP4_ERROR_INVALID_FORMAT;
            segment_description = sample.description_index;
            have_segment_description = true;
            found_first = true;
        } else if (sample.description_index != segment_description) {
            // One edit/segment may not switch sample descriptions.
            return AP4_ERROR_INVALID_FORMAT;
        }

        AP4_Result result = EditedTsValidateDescription(track, is_video,
                                                        sample.description_index);
        if (AP4_FAILED(result)) return result;

        min_cts = std::min(min_cts, sample.cts);
        max_cts = std::max(max_cts, sample.cts);

        AP4_UI64 dts90;
        AP4_UI64 pts90;
        result = EditedTsProject90k(movie_start, sample.dts, media_start,
                                     movie_timescale, media_timescale, dts90);
        if (AP4_FAILED(result)) return result;
        result = EditedTsProject90k(movie_start, sample.cts, media_start,
                                     movie_timescale, media_timescale, pts90);
        if (AP4_FAILED(result)) return result;

        PlanItem item;
        item.track = track;
        item.is_video = is_video;
        item.sample_index = sample.index;
        item.description_index = sample.description_index;
        item.dts90 = dts90;
        item.pts90 = pts90;
        item.order = (AP4_UI32)plan.size();
        plan.push_back(item);
    }

    if (!found_first) return AP4_ERROR_INVALID_FORMAT;
    if (!have_segment_description) return AP4_ERROR_INVALID_FORMAT;

    // The complete composition-time range of the selected frames must be in
    // the source window.  No preroll or tail trimming is attempted.
    if (min_cts < media_start || max_cts > media_end) {
        return AP4_ERROR_INVALID_FORMAT;
    }

    return AP4_SUCCESS;
}

static AP4_Result EditedTsBuildTrackPlan(AP4_Track*             track,
                                         bool                   is_video,
                                         AP4_UI32               movie_timescale,
                                         AP4_UI64               movie_duration,
                                         std::vector<PlanItem>& plan)
{
    AP4_UI32 media_timescale = track->GetMediaTimeScale();
    if (media_timescale == 0) return AP4_ERROR_INVALID_PARAMETERS;

    std::vector<SampleInfo> samples;
    std::set<AP4_UI64> dts_boundaries;
    AP4_Result result = EditedTsGatherSamples(track, samples, dts_boundaries);
    if (AP4_FAILED(result)) return result;

    AP4_Atom* elst_atom = NULL;
    if (track->UseTrakAtom()) {
        elst_atom = track->UseTrakAtom()->FindChild("edts/elst");
    }
    AP4_ElstAtom* elst = AP4_DYNAMIC_CAST(AP4_ElstAtom, elst_atom);

    if (elst == NULL) {
        // No edit list: play the whole track from media time zero.
        if (samples.empty()) return AP4_SUCCESS;
        if (dts_boundaries.find(0) == dts_boundaries.end()) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        AP4_UI64 media_end = track->GetMediaDuration();
        if (media_end == 0 ||
            dts_boundaries.find(media_end) == dts_boundaries.end() ||
            *dts_boundaries.rbegin() != media_end) {
            // The implicit whole-track segment must end on the final DTS
            // boundary; inconsistent duration tables are rejected.
            return AP4_ERROR_INVALID_FORMAT;
        }
        return EditedTsAppendSegment(track, is_video, samples, dts_boundaries,
                                     0, media_end, 0,
                                     movie_timescale, media_timescale, plan);
    }

    AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
    if (entries.ItemCount() == 0) return AP4_ERROR_INVALID_FORMAT;

    AP4_UI64 movie_cursor = 0;
    bool have_previous_media_end = false;
    AP4_UI64 previous_media_end = 0;

    for (AP4_Ordinal i=0; i<entries.ItemCount(); ++i) {
        AP4_ElstEntry& entry = entries[i];

        // Only rate=1.0 is supported.  The fractional half is normally zero.
        if (entry.m_MediaRate != 1 || entry.m_MediaRateFraction != 0) {
            return AP4_ERROR_NOT_SUPPORTED;
        }
        if (entry.m_SegmentDuration == 0) {
            return AP4_ERROR_INVALID_FORMAT;
        }

        AP4_UI64 movie_start = movie_cursor;
#ifdef AP4_EDITED_TS_HAVE_U128
        AP4_EditedTsU128 cursor_end =
            (AP4_EditedTsU128)movie_cursor + entry.m_SegmentDuration;
        if (cursor_end > (AP4_EditedTsU128)EditedTsUi64Max() ||
            cursor_end > (AP4_EditedTsU128)movie_duration) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        movie_cursor = (AP4_UI64)cursor_end;
#else
        if (entry.m_SegmentDuration > EditedTsUi64Max()-movie_cursor ||
            movie_cursor+entry.m_SegmentDuration > movie_duration) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        movie_cursor += entry.m_SegmentDuration;
#endif

        if (entry.m_MediaTime < 0) {
            // Empty edit: movie time passes, but no media interval is consumed.
            continue;
        }

        AP4_UI64 media_start = (AP4_UI64)entry.m_MediaTime;
        AP4_UI64 media_end;
#ifdef AP4_EDITED_TS_HAVE_U128
        AP4_EditedTsU128 product =
            (AP4_EditedTsU128)entry.m_SegmentDuration*(AP4_EditedTsU128)media_timescale;
        if (product % (AP4_EditedTsU128)movie_timescale != 0) {
            return AP4_ERROR_INVALID_FORMAT;
        }
        AP4_EditedTsU128 source_duration =
            product/(AP4_EditedTsU128)movie_timescale;
        AP4_EditedTsU128 source_end =
            (AP4_EditedTsU128)media_start+source_duration;
        if (source_end > (AP4_EditedTsU128)EditedTsUi64Max()) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        media_end = (AP4_UI64)source_end;
#else
        return AP4_ERROR_NOT_SUPPORTED;
#endif

        if (have_previous_media_end && media_start < previous_media_end) {
            // Source media intervals must be forward and non-overlapping.
            return AP4_ERROR_INVALID_FORMAT;
        }
        previous_media_end = media_end;
        have_previous_media_end = true;

        result = EditedTsAppendSegment(track, is_video, samples, dts_boundaries,
                                       media_start, media_end, movie_start,
                                       movie_timescale, media_timescale, plan);
        if (AP4_FAILED(result)) return result;
    }

    return AP4_SUCCESS;
}

static bool EditedTsPlanCompare(const PlanItem& a, const PlanItem& b)
{
    if (a.dts90 != b.dts90) return a.dts90 < b.dts90;
    if (a.is_video != b.is_video) return a.is_video; // video first on ties
    return a.order < b.order;
}

static AP4_Result EditedTsWriteItem(const PlanItem&                        item,
                                    AP4_Mpeg2TsWriter::SampleStream*       video,
                                    AP4_Mpeg2TsWriter::SampleStream*       audio,
                                    AP4_UI64                               origin90k,
                                    AP4_ByteStream&                        output)
{
    AP4_Mpeg2TsWriter::SampleStream* stream = item.is_video ? video : audio;
    if (stream == NULL) return AP4_ERROR_INVALID_STATE;

    const AP4_UI64 ui64_max = EditedTsUi64Max();
    if (origin90k > ui64_max-AP4_EDITED_TS_CLOCK_OFFSET_90K) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    AP4_UI64 offset = origin90k+AP4_EDITED_TS_CLOCK_OFFSET_90K;
    if (item.dts90 > ui64_max-offset || item.pts90 > ui64_max-offset) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    stream->SetTimestampOverride(offset+item.dts90, offset+item.pts90);

    AP4_Sample sample;
    AP4_DataBuffer sample_data;
    AP4_Result result = item.track->ReadSample(item.sample_index, sample, sample_data);
    if (AP4_FAILED(result)) return result;

    AP4_SampleDescription* description =
        item.track->GetSampleDescription(item.description_index);
    if (description == NULL) return AP4_ERROR_INVALID_FORMAT;

    return stream->WriteSample(sample, sample_data, description, false, output);
}

/*----------------------------------------------------------------------
|   AP4_EditedTsWriter::Write
+---------------------------------------------------------------------*/
AP4_Result
AP4_EditedTsWriter::Write(AP4_Movie&      movie,
                          AP4_ByteStream& output,
                          AP4_UI64        origin90k,
                          AP4_UI32        pcrInterval90k)
{
    if (pcrInterval90k < 1 || pcrInterval90k > 9000) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    if (movie.HasFragments()) return AP4_ERROR_NOT_SUPPORTED;

    AP4_UI32 movie_timescale = movie.GetTimeScale();
    if (movie_timescale == 0) return AP4_ERROR_INVALID_PARAMETERS;
    AP4_UI64 movie_duration = movie.GetDuration();

    AP4_Track* video_track = NULL;
    AP4_Track* audio_track = NULL;
    unsigned int video_track_count = 0;
    unsigned int audio_track_count = 0;
    for (AP4_List<AP4_Track>::Item* item = movie.GetTracks().FirstItem();
         item;
         item = item->GetNext()) {
        AP4_Track* track = item->GetData();
        if (track->GetType() == AP4_Track::TYPE_VIDEO) {
            ++video_track_count;
            video_track = track;
        } else if (track->GetType() == AP4_Track::TYPE_AUDIO) {
            ++audio_track_count;
            audio_track = track;
        }
    }
    if (video_track_count != 1 || audio_track_count > 1) {
        return AP4_ERROR_INVALID_PARAMETERS;
    }
    if (video_track == NULL) return AP4_ERROR_INVALID_PARAMETERS;

    AP4_Result result = EditedTsValidateAtomTree(movie.GetMoovAtom());
    if (AP4_FAILED(result)) return result;

    AP4_UI64 end90k;
    result = EditedTsScaleFloor90k(movie_duration, movie_timescale, end90k);
    if (AP4_FAILED(result)) return result;

    std::vector<PlanItem> plan;
    result = EditedTsBuildTrackPlan(video_track, true,
                                    movie_timescale, movie_duration, plan);
    if (AP4_FAILED(result)) return result;
    if (audio_track) {
        result = EditedTsBuildTrackPlan(audio_track, false,
                                        movie_timescale, movie_duration, plan);
        if (AP4_FAILED(result)) return result;
    }

    // Validate all full-width clock arithmetic before the first output byte.
    const AP4_UI64 ui64_max = EditedTsUi64Max();
    if (origin90k > ui64_max-end90k) return AP4_ERROR_OUT_OF_RANGE;
    AP4_UI64 end_clock90k = origin90k+end90k;
    if (end_clock90k > ui64_max/300) return AP4_ERROR_OUT_OF_RANGE;
    if (origin90k > ui64_max-AP4_EDITED_TS_CLOCK_OFFSET_90K) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    AP4_UI64 pes_offset90k = origin90k+AP4_EDITED_TS_CLOCK_OFFSET_90K;
    for (std::size_t i=0; i<plan.size(); ++i) {
        if (plan[i].dts90 > end90k || plan[i].pts90 > end90k) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        if (plan[i].dts90 > ui64_max-pes_offset90k ||
            plan[i].pts90 > ui64_max-pes_offset90k) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
    }

    std::sort(plan.begin(), plan.end(), EditedTsPlanCompare);

    AP4_Mpeg2TsWriter ts_writer;
    AP4_Mpeg2TsWriter::SampleStream* video_stream = NULL;
    AP4_Mpeg2TsWriter::SampleStream* audio_stream = NULL;

    result = ts_writer.SetVideoStream(90000,
                                      AP4_MPEG2_STREAM_TYPE_AVC,
                                      AP4_MPEG2_TS_DEFAULT_STREAM_ID_VIDEO,
                                      video_stream,
                                      AP4_MPEG2_TS_DEFAULT_PID_VIDEO,
                                      NULL, 0, 0);
    if (AP4_FAILED(result)) return result;
    if (audio_track) {
        result = ts_writer.SetAudioStream(90000,
                                          AP4_MPEG2_STREAM_TYPE_ISO_IEC_13818_7,
                                          AP4_MPEG2_TS_DEFAULT_STREAM_ID_AUDIO,
                                          audio_stream,
                                          AP4_MPEG2_TS_DEFAULT_PID_AUDIO,
                                          NULL, 0, 0);
        if (AP4_FAILED(result)) return result;
    }

    result = ts_writer.WritePAT(output);
    if (AP4_FAILED(result)) return result;
    result = ts_writer.WritePMT(output);
    if (AP4_FAILED(result)) return result;

    // PCR grid points, with the movie end included exactly once.
    std::vector<AP4_UI64> pcr_points;
    for (AP4_UI64 grid=0;;) {
        pcr_points.push_back(grid);
        if (end90k-grid < pcrInterval90k) break;
        grid += pcrInterval90k;
    }
    if (pcr_points.back() != end90k) pcr_points.push_back(end90k);

    std::size_t next_item = 0;
    for (std::size_t p=0; p<pcr_points.size(); ++p) {
        AP4_UI64 pcr_time90k = pcr_points[p];

        while (next_item < plan.size() && plan[next_item].dts90 < pcr_time90k) {
            result = EditedTsWriteItem(plan[next_item], video_stream, audio_stream,
                                       origin90k, output);
            if (AP4_FAILED(result)) return result;
            ++next_item;
        }

        AP4_UI64 pcr27m = (origin90k+pcr_time90k)*300;
        result = video_stream->WritePcrPacket(pcr27m, output);
        if (AP4_FAILED(result)) return result;

        while (next_item < plan.size() && plan[next_item].dts90 == pcr_time90k) {
            result = EditedTsWriteItem(plan[next_item], video_stream, audio_stream,
                                       origin90k, output);
            if (AP4_FAILED(result)) return result;
            ++next_item;
        }
    }

    // Validated plans have no item after the movie-end PCR point.
    while (next_item < plan.size()) {
        result = EditedTsWriteItem(plan[next_item], video_stream, audio_stream,
                                   origin90k, output);
        if (AP4_FAILED(result)) return result;
        ++next_item;
    }

    return AP4_SUCCESS;
}
