/*****************************************************************
|
|    AP4 - Edited Sample Reader
|
|    Copyright 2002-2026 Axiomatic Systems, LLC
|
|
|    This file is part of Bento4/AP4 (MP4 Atom Processing Library).
|
|    Unless you have obtained Bento4 under a difference license,
|    this version of Bento4 is Bento4|GPL.
|
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
#include <stdlib.h>

#include "Ap4EditedSampleReader.h"
#include "Ap4SampleSpool.h"
#include "Ap4Track.h"
#include "Ap4TrakAtom.h"
#include "Ap4ElstAtom.h"
#include "Ap4Sample.h"
#include "Ap4ByteStream.h"

/*----------------------------------------------------------------------
|   constants
+---------------------------------------------------------------------*/
const AP4_UI64 AP4_EditedReaderUi64Max = ~(AP4_UI64)0;
const AP4_SI64 AP4_EditedReaderSi64Max = (AP4_SI64)(~(AP4_UI64)0 >> 1);
const AP4_SI64 AP4_EditedReaderSi64Min = -AP4_EditedReaderSi64Max-1;

/*----------------------------------------------------------------------
|   checked arithmetic helpers (return false on overflow)
+---------------------------------------------------------------------*/
static bool
AP4_EditedReaderAddUI64(AP4_UI64 a, AP4_UI64 b, AP4_UI64& result)
{
    result = a+b;
    return result >= a;
}

static bool
AP4_EditedReaderMulUI64(AP4_UI64 a, AP4_UI64 b, AP4_UI64& result)
{
    if (a != 0 && b > AP4_EditedReaderUi64Max/a) return false;
    result = a*b;
    return true;
}

static bool
AP4_EditedReaderMulSI64(AP4_SI64 a, AP4_UI64 b, AP4_SI64& result)
{
    if (a >= 0) {
        AP4_UI64 product;
        if (!AP4_EditedReaderMulUI64((AP4_UI64)a, b, product)) return false;
        if (product > (AP4_UI64)AP4_EditedReaderSi64Max) return false;
        result = (AP4_SI64)product;
        return true;
    } else {
        AP4_UI64 magnitude = (AP4_UI64)(-(a+1))+1; // safe for SI64 min
        AP4_UI64 product;
        if (!AP4_EditedReaderMulUI64(magnitude, b, product)) return false;
        if (product > (AP4_UI64)AP4_EditedReaderSi64Max+1) return false;
        result = product == (AP4_UI64)AP4_EditedReaderSi64Max+1
               ? AP4_EditedReaderSi64Min
               : -(AP4_SI64)product;
        return true;
    }
}

static bool
AP4_EditedReaderAddSI64(AP4_SI64 a, AP4_SI64 b, AP4_SI64& result)
{
    if (b > 0 && a > AP4_EditedReaderSi64Max-b) return false;
    if (b < 0 && a < AP4_EditedReaderSi64Min-b) return false;
    result = a+b;
    return true;
}

static AP4_UI64
AP4_EditedReaderGcd(AP4_UI64 a, AP4_UI64 b)
{
    while (b) {
        AP4_UI64 t = a%b;
        a = b;
        b = t;
    }
    return a;
}

/*----------------------------------------------------------------------
|   sorting helpers
+---------------------------------------------------------------------*/
struct AP4_EditedReaderIndexOrder {
    AP4_UI64 m_Key;
    AP4_UI32 m_Index;
};

static int
AP4_EditedReaderCompareUI64(const void* a, const void* b)
{
    const AP4_EditedReaderIndexOrder* ia = (const AP4_EditedReaderIndexOrder*)a;
    const AP4_EditedReaderIndexOrder* ib = (const AP4_EditedReaderIndexOrder*)b;
    if (ia->m_Key < ib->m_Key) return -1;
    if (ia->m_Key > ib->m_Key) return 1;
    return 0;
}

struct AP4_EditedReaderSignedOrder {
    AP4_SI64 m_Key;
    AP4_UI32 m_Index;
};

static int
AP4_EditedReaderCompareSI64(const void* a, const void* b)
{
    const AP4_EditedReaderSignedOrder* ia = (const AP4_EditedReaderSignedOrder*)a;
    const AP4_EditedReaderSignedOrder* ib = (const AP4_EditedReaderSignedOrder*)b;
    if (ia->m_Key < ib->m_Key) return -1;
    if (ia->m_Key > ib->m_Key) return 1;
    return 0;
}

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader::AP4_EditedSampleReader
+---------------------------------------------------------------------*/
AP4_EditedSampleReader::AP4_EditedSampleReader(AP4_ByteStream& source,
                                               AP4_SampleSpool* spool,
                                               AP4_UI64         time_scale,
                                               AP4_UI64         required) :
    m_Source(source),
    m_Spool(spool),
    m_TimeScale(time_scale),
    m_Required(required),
    m_NextSpool(0),
    m_Cursor(0),
    m_Frontier(0),
    m_TerminalError(AP4_SUCCESS)
{
}

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader::~AP4_EditedSampleReader
+---------------------------------------------------------------------*/
AP4_EditedSampleReader::~AP4_EditedSampleReader()
{
    delete m_Spool;
}

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader::Create
+---------------------------------------------------------------------*/
AP4_Result
AP4_EditedSampleReader::Create(AP4_Track&               track,
                               AP4_ByteStream&          source,
                               AP4_ByteStream&          scratch,
                               AP4_UI64                 capacity,
                               AP4_EditedSampleReader*& reader)
{
    reader = NULL;

    // check the timescales and compute the common timescale C
    AP4_UI64 movie_time_scale = track.GetMovieTimeScale();
    AP4_UI64 media_time_scale = track.GetMediaTimeScale();
    if (movie_time_scale == 0 || media_time_scale == 0) {
        return AP4_ERROR_INVALID_FORMAT;
    }
    AP4_UI64 gcd = AP4_EditedReaderGcd(movie_time_scale, media_time_scale);
    AP4_UI64 common_time_scale;
    if (!AP4_EditedReaderMulUI64(movie_time_scale/gcd, media_time_scale, common_time_scale)) {
        return AP4_ERROR_OUT_OF_RANGE;
    }
    AP4_UI64 movie_units_per_c = common_time_scale/movie_time_scale;
    AP4_UI64 media_units_per_c = common_time_scale/media_time_scale;

    // locate the media data: the source stream is positioned at the
    // first byte of the mdat payload, which extends to the end of
    // the stream
    AP4_Position    origin = 0;
    AP4_LargeSize   source_size = 0;
    AP4_Result      result = source.Tell(origin);
    if (AP4_FAILED(result)) return result;
    result = source.GetSize(source_size);
    if (AP4_FAILED(result)) return result;
    if (source_size < origin) return AP4_ERROR_INVALID_FORMAT;

    // collect the sample table metadata (no media bytes are read)
    AP4_Cardinal sample_count = track.GetSampleCount();
    AP4_Array<SampleInfo> samples;
    result = samples.SetItemCount(sample_count);
    if (AP4_FAILED(result)) return result;
    AP4_UI64 media_total = 0;
    for (AP4_Ordinal i=0; i<sample_count; i++) {
        AP4_Sample sample;
        result = track.GetSample(i, sample);
        if (AP4_FAILED(result)) return result;

        SampleInfo& info = samples[i];
        AP4_UI64    offset = sample.GetOffset();
        AP4_UI64    size   = sample.GetSize();
        AP4_UI64    end;
        if (!AP4_EditedReaderAddUI64(offset, size, end)) return AP4_ERROR_OUT_OF_RANGE;
        if (offset < origin || end > source_size) return AP4_ERROR_INVALID_FORMAT;
        info.offset           = offset-origin;
        info.size             = (AP4_UI32)size;
        info.dts              = sample.GetDts();
        info.duration         = sample.GetDuration();
        info.description_index = sample.GetDescriptionIndex();
        info.sync             = sample.IsSync();
        if (info.dts > (AP4_UI64)AP4_EditedReaderSi64Max) return AP4_ERROR_OUT_OF_RANGE;
        AP4_SI64 cts;
        if (!AP4_EditedReaderAddSI64((AP4_SI64)info.dts, (AP4_SI32)sample.GetCtsDelta(), cts)) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
        info.cts = cts;
        if (!AP4_EditedReaderAddUI64(media_total, info.duration, media_total)) {
            return AP4_ERROR_OUT_OF_RANGE;
        }
    }

    // check that the physical sample ranges do not overlap
    AP4_EditedReaderIndexOrder* physical = NULL;
    if (sample_count) {
        physical = new AP4_EditedReaderIndexOrder[sample_count];
        if (physical == NULL) return AP4_ERROR_OUT_OF_MEMORY;
        for (AP4_Ordinal i=0; i<sample_count; i++) {
            physical[i].m_Key   = samples[i].offset;
            physical[i].m_Index = i;
        }
        qsort(physical, sample_count, sizeof(physical[0]), AP4_EditedReaderCompareUI64);
        for (AP4_Ordinal i=1; i<sample_count; i++) {
            const SampleInfo& previous = samples[physical[i-1].m_Index];
            if (physical[i].m_Key < previous.offset+previous.size) {
                delete[] physical;
                return AP4_ERROR_INVALID_FORMAT;
            }
        }
    }

    // build the edit list
    struct EditInfo {
        bool     gap;
        AP4_UI64 media_begin_c; // media timeline, in C units
        AP4_UI64 span_c;        // edit duration, in C units
    };
    AP4_Array<EditInfo> edits;
    AP4_ElstAtom*       elst = NULL;
    AP4_TrakAtom*       trak = track.UseTrakAtom();
    if (trak) {
        elst = AP4_DYNAMIC_CAST(AP4_ElstAtom, trak->FindChild("edts/elst"));
    }
    if (elst && elst->GetEntries().ItemCount()) {
        AP4_Array<AP4_ElstEntry>& entries = elst->GetEntries();
        for (AP4_Ordinal i=0; i<entries.ItemCount(); i++) {
            const AP4_ElstEntry& entry = entries[i];
            if (entry.m_SegmentDuration == 0) {
                delete[] physical;
                return AP4_ERROR_INVALID_FORMAT;
            }
            EditInfo edit;
            if (!AP4_EditedReaderMulUI64(entry.m_SegmentDuration, movie_units_per_c, edit.span_c)) {
                delete[] physical;
                return AP4_ERROR_OUT_OF_RANGE;
            }
            if (entry.m_MediaTime == -1) {
                edit.gap          = true;
                edit.media_begin_c = 0;
            } else {
                if (entry.m_MediaTime < 0) {
                    delete[] physical;
                    return AP4_ERROR_INVALID_FORMAT;
                }
                // only plain rate 1 is supported: the integer and the
                // fraction parts of the rate must be exactly 1 and 0
                if (entry.m_MediaRate != 1 || entry.m_MediaRateFraction != 0) {
                    delete[] physical;
                    return AP4_ERROR_INVALID_FORMAT;
                }
                edit.gap = false;
                if (!AP4_EditedReaderMulUI64((AP4_UI64)entry.m_MediaTime, media_units_per_c, edit.media_begin_c)) {
                    delete[] physical;
                    return AP4_ERROR_OUT_OF_RANGE;
                }
                if (edit.media_begin_c > (AP4_UI64)AP4_EditedReaderSi64Max) {
                    delete[] physical;
                    return AP4_ERROR_OUT_OF_RANGE;
                }
            }
            result = edits.Append(edit);
            if (AP4_FAILED(result)) {
                delete[] physical;
                return result;
            }
        }
    } else if (sample_count) {
        // no edit list: play the entire track
        EditInfo edit;
        edit.gap           = false;
        edit.media_begin_c = 0;
        if (!AP4_EditedReaderMulUI64(media_total, media_units_per_c, edit.span_c)) {
            delete[] physical;
            return AP4_ERROR_OUT_OF_RANGE;
        }
        result = edits.Append(edit);
        if (AP4_FAILED(result)) {
            delete[] physical;
            return result;
        }
    }

    // compute the frame intervals in C units and the delivery plan
    AP4_Array<PlanEntry> plan;
    AP4_UI64             movie_cursor_c = 0;
    {
        AP4_SI64* frame_begin = NULL;
        AP4_SI64* frame_end   = NULL;
        bool*     visible     = NULL;
        AP4_SI64* win_begin   = NULL;
        AP4_SI64* win_end     = NULL;
        if (sample_count) {
            frame_begin = new AP4_SI64[sample_count];
            frame_end   = new AP4_SI64[sample_count];
            visible     = new bool[sample_count];
            win_begin   = new AP4_SI64[sample_count];
            win_end     = new AP4_SI64[sample_count];
            if (!frame_begin || !frame_end || !visible || !win_begin || !win_end) {
                result = AP4_ERROR_OUT_OF_MEMORY;
                goto cleanup;
            }
            for (AP4_Ordinal i=0; i<sample_count; i++) {
                AP4_SI64 begin_c;
                AP4_UI64 duration_c;
                if (!AP4_EditedReaderMulSI64(samples[i].cts, media_units_per_c, begin_c) ||
                    !AP4_EditedReaderMulUI64(samples[i].duration, media_units_per_c, duration_c) ||
                    duration_c > (AP4_UI64)AP4_EditedReaderSi64Max ||
                    !AP4_EditedReaderAddSI64(begin_c, (AP4_SI64)duration_c, frame_end[i])) {
                    result = AP4_ERROR_OUT_OF_RANGE;
                    goto cleanup;
                }
                frame_begin[i] = begin_c;
            }
        }

        for (AP4_Ordinal edit_index=0; edit_index<edits.ItemCount(); edit_index++) {
            const EditInfo& edit = edits[edit_index];
            if (edit.span_c > (AP4_UI64)AP4_EditedReaderSi64Max) {
                result = AP4_ERROR_OUT_OF_RANGE;
                goto cleanup;
            }
            if (edit.gap) {
                PlanEntry entry;
                entry.edit_index     = edit_index;
                entry.gap            = true;
                entry.movie_begin    = movie_cursor_c;
                entry.movie_duration = edit.span_c;
                result = plan.Append(entry);
                if (AP4_FAILED(result)) goto cleanup;
                if (!AP4_EditedReaderAddUI64(movie_cursor_c, edit.span_c, movie_cursor_c)) {
                    result = AP4_ERROR_OUT_OF_RANGE;
                    goto cleanup;
                }
                continue;
            }

            // select the visible frames: CTS interval intersects the
            // edit interval, which is left-closed and right-open
            AP4_SI64 edit_begin = (AP4_SI64)edit.media_begin_c;
            AP4_SI64 edit_end;
            if (!AP4_EditedReaderAddSI64(edit_begin, (AP4_SI64)edit.span_c, edit_end)) {
                result = AP4_ERROR_OUT_OF_RANGE;
                goto cleanup;
            }
            AP4_Ordinal visible_count = 0;
            for (AP4_Ordinal i=0; i<sample_count; i++) {
                visible[i] = frame_begin[i] < edit_end && frame_end[i] > edit_begin;
                if (visible[i]) {
                    win_begin[i] = frame_begin[i] > edit_begin ? frame_begin[i] : edit_begin;
                    win_end[i]   = frame_end[i]   < edit_end   ? frame_end[i]   : edit_end;
                    ++visible_count;
                }
            }
            if (visible_count == 0) {
                // a media segment must be fully covered by frames
                result = AP4_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            // the visible frames must tile the edit interval exactly:
            // no overlap and no gap
            {
                AP4_EditedReaderSignedOrder* presentation = new AP4_EditedReaderSignedOrder[visible_count];
                if (presentation == NULL) {
                    result = AP4_ERROR_OUT_OF_MEMORY;
                    goto cleanup;
                }
                AP4_Ordinal count = 0;
                for (AP4_Ordinal i=0; i<sample_count; i++) {
                    if (visible[i]) {
                        presentation[count].m_Key   = samples[i].cts;
                        presentation[count].m_Index = i;
                        ++count;
                    }
                }
                qsort(presentation, count, sizeof(presentation[0]), AP4_EditedReaderCompareSI64);
                AP4_SI64 cursor = edit_begin;
                bool     tiled  = true;
                for (AP4_Ordinal i=0; i<count; i++) {
                    AP4_Ordinal frame = presentation[i].m_Index;
                    if (win_begin[frame] != cursor) {
                        tiled = false;
                        break;
                    }
                    cursor = win_end[frame];
                }
                delete[] presentation;
                if (!tiled || cursor != edit_end) {
                    result = AP4_ERROR_INVALID_FORMAT;
                    goto cleanup;
                }
            }

            // find the delivery range: from the nearest sync sample at
            // or before the first visible frame to the last visible
            // frame, in decode order
            AP4_Ordinal first_visible = sample_count;
            AP4_Ordinal last_visible  = 0;
            for (AP4_Ordinal i=0; i<sample_count; i++) {
                if (visible[i]) {
                    if (i < first_visible) first_visible = i;
                    last_visible = i;
                }
            }
            AP4_Ordinal sync = first_visible;
            while (sync != 0 && !samples[sync].sync) --sync;
            if (!samples[sync].sync) {
                result = AP4_ERROR_INVALID_FORMAT;
                goto cleanup;
            }

            // emit the packets for this edit; every edit restarts an
            // independently decodable chain at a sync sample
            for (AP4_Ordinal i=sync; i<=last_visible; i++) {
                PlanEntry entry;
                entry.edit_index = edit_index;
                entry.sample     = i;
                entry.gap        = false;
                if (visible[i]) {
                    AP4_UI64 offset_in_edit = (AP4_UI64)(win_begin[i]-edit_begin);
                    if (!AP4_EditedReaderAddUI64(movie_cursor_c, offset_in_edit, entry.movie_begin)) {
                        result = AP4_ERROR_OUT_OF_RANGE;
                        goto cleanup;
                    }
                    entry.movie_duration = (AP4_UI64)(win_end[i]-win_begin[i]);
                }
                result = plan.Append(entry);
                if (AP4_FAILED(result)) goto cleanup;
                if (samples[i].refs == 0xFFFFFFFF) {
                    result = AP4_ERROR_OUT_OF_RANGE;
                    goto cleanup;
                }
                ++samples[i].refs;
            }
            if (!AP4_EditedReaderAddUI64(movie_cursor_c, edit.span_c, movie_cursor_c)) {
                result = AP4_ERROR_OUT_OF_RANGE;
                goto cleanup;
            }
        }

        result = AP4_SUCCESS;
cleanup:
        delete[] frame_begin;
        delete[] frame_end;
        delete[] visible;
        delete[] win_begin;
        delete[] win_end;
        if (AP4_FAILED(result)) {
            delete[] physical;
            return result;
        }
    }

    // simulate the spool traffic in delivery order to compute the
    // exact peak of live spooled bytes: when a sample is delivered,
    // the read frontier first advances to the end of that sample and
    // every sample passed that still has pending deliveries is fully
    // spooled; a sample is released right after its last delivery
    AP4_Array<AP4_UI32> spool_order;
    for (AP4_Ordinal i=0; i<sample_count; i++) {
        AP4_Ordinal index = physical[i].m_Index;
        if (samples[index].refs && samples[index].size) {
            result = spool_order.Append(index);
            if (AP4_FAILED(result)) {
                delete[] physical;
                return result;
            }
        }
    }
    AP4_UI64 required   = 0;
    {
        AP4_UI64 frontier = 0;
        AP4_UI64 live     = 0;
        AP4_Ordinal next  = 0;
        AP4_Array<AP4_UI32> sim_refs;
        result = sim_refs.SetItemCount(sample_count);
        if (AP4_FAILED(result)) {
            delete[] physical;
            return result;
        }
        for (AP4_Ordinal i=0; i<sample_count; i++) sim_refs[i] = samples[i].refs;
        for (AP4_Ordinal i=0; i<plan.ItemCount(); i++) {
            PlanEntry& entry = plan[i];
            if (entry.gap) continue;
            SampleInfo& info = samples[entry.sample];
            if (info.size) {
                AP4_UI64 target = info.offset+info.size;
                if (target > frontier) {
                    while (next < spool_order.ItemCount() &&
                           samples[spool_order[next]].offset < target) {
                        live += samples[spool_order[next]].size;
                        ++next;
                    }
                    frontier = target;
                    if (live > required) required = live;
                }
            }
            if (sim_refs[entry.sample]) --sim_refs[entry.sample];
            if (sim_refs[entry.sample] == 0 && info.size) live -= info.size;
        }
    }
    delete[] physical;
    physical = NULL;

    // the capacity must cover the planned peak
    if (capacity < required) {
        return AP4_ERROR_OUT_OF_RANGE;
    }

    // create the spool and the reader
    AP4_SampleSpool* spool = NULL;
    result = AP4_SampleSpool::Create(scratch, capacity, spool);
    if (AP4_FAILED(result)) return result;

    AP4_EditedSampleReader* instance = new AP4_EditedSampleReader(source,
                                                                  spool,
                                                                  common_time_scale,
                                                                  required);
    instance->m_Samples    = samples;
    instance->m_SpoolOrder = spool_order;
    instance->m_Plan       = plan;

    reader = instance;
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader::AdvanceFrontier
+---------------------------------------------------------------------*/
AP4_Result
AP4_EditedSampleReader::AdvanceFrontier(AP4_UI64 target)
{
    while (m_Frontier < target) {
        // spool every referenced sample whose bytes are passed
        if (m_NextSpool < m_SpoolOrder.ItemCount()) {
            AP4_UI32          index = m_SpoolOrder[m_NextSpool];
            const SampleInfo& info  = m_Samples[index];
            if (info.offset < target) {
                if (info.offset < m_Frontier) return AP4_ERROR_INTERNAL;
                if (info.offset > m_Frontier) {
                    AP4_Result result = m_Spool->Discard(m_Source, info.offset-m_Frontier);
                    if (AP4_FAILED(result)) return result;
                    m_Frontier = info.offset;
                }
                AP4_Result result = m_Spool->Open(index, info.size);
                if (AP4_FAILED(result)) return result;
                result = m_Spool->Store(index, 0, m_Source, info.size);
                if (AP4_FAILED(result)) return result;
                m_Frontier += info.size;
                ++m_NextSpool;
                continue;
            }
        }
        // skip bytes that carry no pending reference
        AP4_Result result = m_Spool->Discard(m_Source, target-m_Frontier);
        if (AP4_FAILED(result)) return result;
        m_Frontier = target;
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   AP4_EditedSampleReader::ReadNext
+---------------------------------------------------------------------*/
AP4_Result
AP4_EditedSampleReader::ReadNext(Packet& packet)
{
    if (m_TerminalError != AP4_SUCCESS) return m_TerminalError;
    if (m_Cursor >= m_Plan.ItemCount()) return AP4_ERROR_EOS;

    const PlanEntry& entry = m_Plan[m_Cursor];

    // reset the packet
    packet.kind                = entry.gap ? Packet::GAP : Packet::MEDIA;
    packet.edit_index          = entry.edit_index;
    packet.source_sample_index = (AP4_Ordinal)-1;
    packet.dts                 = 0;
    packet.cts                 = 0;
    packet.duration            = 0;
    packet.description_index   = 0;
    packet.sync                = false;
    packet.movie_begin         = entry.movie_begin;
    packet.movie_duration      = entry.movie_duration;
    packet.payload.SetDataSize(0);

    if (entry.gap) {
        ++m_Cursor;
        return AP4_SUCCESS;
    }

    AP4_UI32          index = entry.sample;
    const SampleInfo& info  = m_Samples[index];
    packet.source_sample_index = index;
    packet.dts                 = info.dts;
    packet.cts                 = info.cts;
    packet.duration            = info.duration;
    packet.description_index   = info.description_index;
    packet.sync                = info.sync;

    if (info.size) {
        // advance the physical read frontier past the end of this
        // sample, spooling every referenced sample on the way
        AP4_UI64 target = info.offset+info.size;
        if (target > m_Frontier) {
            AP4_Result result = AdvanceFrontier(target);
            if (AP4_FAILED(result)) {
                m_TerminalError = result;
                return result;
            }
        }

        // the payload is always read back from the spool
        AP4_Result result = packet.payload.SetDataSize(info.size);
        if (AP4_FAILED(result)) return result;
        result = m_Spool->Load(index, 0, packet.payload.UseData(), info.size);
        if (AP4_FAILED(result)) {
            m_TerminalError = result;
            return result;
        }
    }

    // account for this delivery and release the spool slot after the
    // last use of the sample
    if (m_Samples[index].refs) --m_Samples[index].refs;
    if (m_Samples[index].refs == 0 && info.size) {
        m_Spool->Close(index);
    }

    ++m_Cursor;
    return AP4_SUCCESS;
}
