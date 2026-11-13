/*****************************************************************
|
|    Task test cases for AP4_Track::CreateMontage
|
|    All expected values below are computed by hand from the explicit
|    sample timetables (never derived from the implementation under test).
|
 ****************************************************************/

#include "Ap4.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4ElstAtom.h"
#include "Ap4FileWriter.h"
#include "Ap4TkhdAtom.h"
#include "Ap4MdhdAtom.h"
#include "check.h"

#include <cstring>

/*----------------------------------------------------------------------
|   hand-written sample timetable
|
|   media timescale 1000, 8 samples, one byte of payload each
|   (marker 100+index), two closed GOPs:
|
|   idx  dts  cts-delta  cts  display interval  sync
|   0      0        200  200  [200,300)          yes
|   1    100        400  500  [500,600)          no
|   2    200        100  300  [300,400)          no
|   3    300        100  400  [400,500)          no
|   4    400        200  600  [600,700)          yes
|   5    500        400  900  [900,1000)         no
|   6    600        100  700  [700,800)          no
|   7    700        100  800  [800,900)          no
+---------------------------------------------------------------------*/
struct SampSpec {
    AP4_UI64    dts;
    AP4_UI32    cts_delta;
    AP4_UI32    duration;
    bool        sync;
    AP4_Ordinal desc_index;
};

static const SampSpec TABLE_T[8] = {
    {  0, 200, 100, true,  0 },
    {100, 400, 100, false, 0 },
    {200, 100, 100, false, 0 },
    {300, 100, 100, false, 0 },
    {400, 200, 100, true,  0 },
    {500, 400, 100, false, 0 },
    {600, 100, 100, false, 0 },
    {700, 100, 100, false, 0 }
};

/*----------------------------------------------------------------------
|   BuildTrack
|
|   Builds a synthetic video track from an explicit timetable.
|   When with_edts is true, the track gets a leading empty edit of
|   empty_dur movie ticks followed by one rate-1 media edit.
+---------------------------------------------------------------------*/
static AP4_Track*
BuildTrack(const SampSpec*  specs,
           unsigned         spec_count,
           AP4_UI32         movie_time_scale,
           AP4_UI32         media_time_scale,
           AP4_UI64        movie_duration,
           AP4_UI64        media_duration,
           unsigned         description_count,
           bool             mark_all_sync,
           bool             with_edts,
           AP4_UI64         empty_dur,
           AP4_SI64         media_edit_time,
           AP4_UI64         media_edit_dur)
{
    AP4_MemoryByteStream* payload = new AP4_MemoryByteStream(spec_count);
    for (unsigned i = 0; i < spec_count; i++) {
        AP4_UI08 marker = (AP4_UI08)(100 + i);
        payload->Write(&marker, 1);
    }

    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    for (unsigned d = 0; d < description_count; d++) {
        AP4_UI32 format = (d == 0)
            ? AP4_ATOM_TYPE('r','a','w',' ')
            : AP4_ATOM_TYPE('r','a','w','2');
        table->AddSampleDescription(
            new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,
                                      format, NULL));
    }
    for (unsigned i = 0; i < spec_count; i++) {
        AP4_Result r = table->AddSample(
            *payload, i, 1,
            specs[i].duration,
            specs[i].desc_index,
            specs[i].dts,
            specs[i].cts_delta,
            mark_all_sync || specs[i].sync);
        if (AP4_FAILED(r)) {
            payload->Release();
            delete table;
            return NULL;
        }
    }
    payload->Release(); // samples keep their own reference

    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_VIDEO,
                                     table, 1,
                                     movie_time_scale,
                                     movie_duration,
                                     media_time_scale,
                                     media_duration,
                                     "und",
                                     320 << 16, 240 << 16);
    if (with_edts) {
        AP4_ContainerAtom* edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
        AP4_ElstAtom* elst = new AP4_ElstAtom();
        elst->AddEntry(AP4_ElstEntry(empty_dur, -1, 1));
        elst->AddEntry(AP4_ElstEntry(media_edit_dur, media_edit_time, 1));
        edts->AddChild(elst);
        const_cast<AP4_TrakAtom*>(track->GetTrakAtom())->AddChild(edts);
    }
    return track;
}

static AP4_Track*
BuildStandardTrack(AP4_UI32 movie_time_scale,
                   AP4_UI32 media_time_scale,
                   AP4_UI64 empty_dur_movie,
                   AP4_UI64 media_edit_dur_movie)
{
    // movie duration = empty edit + media edit
    AP4_UI64 movie_duration = empty_dur_movie + media_edit_dur_movie;
    return BuildTrack(TABLE_T, 8,
                      movie_time_scale, media_time_scale,
                      movie_duration, 1000,
                      1, false,
                      empty_dur_movie != 0 || media_edit_dur_movie != 0,
                      empty_dur_movie, 200, media_edit_dur_movie);
}

/*----------------------------------------------------------------------
|   helpers
+---------------------------------------------------------------------*/
static AP4_ElstAtom*
TrackElst(AP4_Track* track)
{
    return AP4_DYNAMIC_CAST(AP4_ElstAtom,
        const_cast<AP4_TrakAtom*>(track->GetTrakAtom())
            ->FindChild("edts/elst"));
}

static AP4_UI08
SampleByte(AP4_Track* track, AP4_Ordinal index)
{
    AP4_Sample sample;
    AP4_DataBuffer data;
    if (AP4_FAILED(track->ReadSample(index, sample, data)) ||
        data.GetDataSize() != 1) {
        return 0;
    }
    return data.GetData()[0];
}

struct WrittenFile {
    AP4_MemoryByteStream* stream;
    AP4_File*             file;
    AP4_Track*            track;
};

static WrittenFile
WriteAndReopen(AP4_Track* track, AP4_UI32 movie_time_scale,
               AP4_UI64 initial_movie_duration = 0)
{
    WrittenFile result;
    result.stream = NULL;
    result.file   = NULL;
    result.track  = NULL;

    AP4_Movie* movie = new AP4_Movie(movie_time_scale,
                                     initial_movie_duration);
    movie->AddTrack(track);
    AP4_File file(movie);

    AP4_MemoryByteStream* encoded = new AP4_MemoryByteStream();
    TASK_CHECK(AP4_SUCCEEDED(AP4_FileWriter::Write(file, *encoded)));
    TASK_CHECK(AP4_SUCCEEDED(encoded->Seek(0)));

    AP4_File* reread = new AP4_File(*encoded);
    TASK_CHECK(reread->GetMovie() != NULL);
    AP4_Track* parsed = reread->GetMovie()->GetTrack(track->GetId());
    TASK_CHECK(parsed != NULL);

    result.stream = encoded;
    result.file   = reread;
    result.track  = parsed;
    return result;
}

/*----------------------------------------------------------------------
|   projection group: edit list projection and parameter validation
+---------------------------------------------------------------------*/
static int
RunProjectionTests()
{
    // P1: a range spanning the leading empty edit and the media edit
    {
        AP4_Track* src = BuildStandardTrack(1000, 1000, 200, 800);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 100; r.end = 400;
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 300);
        TASK_CHECK(out->GetSampleCount() == 3);
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == -1);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 200);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 200);
        TASK_CHECK(SampleByte(out, 0) == 100);
        TASK_CHECK(SampleByte(out, 1) == 101);
        TASK_CHECK(SampleByte(out, 2) == 102);
        delete out;
        delete src;
    }

    // P2: distinct movie/media timescales with an exact conversion
    {
        AP4_Track* src = BuildStandardTrack(30000, 1000, 6000, 24000);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 9000; r.end = 18000; // media window [300,600)
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 9000);
        // display samples 2 ([300,400)) and 3 ([400,500)); decode chain 0..3
        TASK_CHECK(out->GetSampleCount() == 4);
        TASK_CHECK(SampleByte(out, 0) == 100);
        TASK_CHECK(SampleByte(out, 3) == 103);
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 9000);
        delete out;
        delete src;
    }

    // P3: cut point that does not land on an integer media tick
    {
        AP4_Track* src = BuildStandardTrack(30000, 1000, 6000, 24000);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 9001; r.end = 15000; // 3001 media ticks would be needed
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) ==
                   AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);
        delete src;
    }

    // P4: reversed, reordered requests on a track without an edit list
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r1; r1.begin = 600; r1.end = 800;
        AP4_Track::EditRange r2; r2.begin = 200; r2.end = 500;
        ranges.Append(r1);
        ranges.Append(r2);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 500);
        TASK_CHECK(out->GetSampleCount() == 7);
        // block A = source 4,5,6 then block B = source 0,1,2,3
        const AP4_UI08 expected[7] = {104, 105, 106, 100, 101, 102, 103};
        for (unsigned i = 0; i < 7; i++) {
            TASK_CHECK(SampleByte(out, i) == expected[i]);
        }
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 2);
        // first block (samples 4,5,6) starts at output media time 0
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 200);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);
        // second block (samples 0..3) starts at output media time 300
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 500);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 300);
        delete out;
        delete src;
    }

    // P5: invalid inputs leave the source track untouched
    {
        AP4_Track* src = BuildStandardTrack(1000, 1000, 200, 800);
        TASK_CHECK(src != NULL);
        AP4_Cardinal before_count = src->GetSampleCount();
        AP4_ElstAtom* before_elst = TrackElst(src);
        TASK_CHECK(before_elst != NULL &&
                   before_elst->GetEntries().ItemCount() == 2);

        AP4_Track* out = NULL;
        AP4_Array<AP4_Track::EditRange> empty;
        TASK_CHECK(src->CreateMontage(empty, out) ==
                   AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        AP4_Array<AP4_Track::EditRange> bad;
        AP4_Track::EditRange r;
        r.begin = 500; r.end = 500; // begin == end
        bad.Append(r);
        TASK_CHECK(src->CreateMontage(bad, out) ==
                   AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        bad[0].begin = 900; bad[0].end = 1001; // end > track duration
        TASK_CHECK(src->CreateMontage(bad, out) ==
                   AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        TASK_CHECK(src->GetSampleCount() == before_count);
        TASK_CHECK(TrackElst(src)->GetEntries().ItemCount() == 2);
        TASK_CHECK(SampleByte(src, 7) == 107);
        delete src;
    }

    // P6: edit list shapes that are explicitly not supported
    {
        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0; r.end = 100;
        ranges.Append(r);

        // dwell (media rate 0)
        {
            AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                        1000, 1000, 1, false,
                                        false, 0, 0, 0);
            AP4_ContainerAtom* edts =
                new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
            AP4_ElstAtom* elst = new AP4_ElstAtom();
            elst->AddEntry(AP4_ElstEntry(1000, 0, 0)); // rate 0 = dwell
            edts->AddChild(elst);
            const_cast<AP4_TrakAtom*>(src->GetTrakAtom())->AddChild(edts);

            AP4_Track* out = NULL;
            TASK_CHECK(src->CreateMontage(ranges, out) ==
                       AP4_ERROR_NOT_SUPPORTED);
            TASK_CHECK(out == NULL);
            delete src;
        }

        // two media edits
        {
            AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                        1000, 1000, 1, false,
                                        false, 0, 0, 0);
            AP4_ContainerAtom* edts =
                new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
            AP4_ElstAtom* elst = new AP4_ElstAtom();
            elst->AddEntry(AP4_ElstEntry(500, 0, 1));
            elst->AddEntry(AP4_ElstEntry(500, 500, 1));
            edts->AddChild(elst);
            const_cast<AP4_TrakAtom*>(src->GetTrakAtom())->AddChild(edts);

            AP4_Track* out = NULL;
            TASK_CHECK(src->CreateMontage(ranges, out) ==
                       AP4_ERROR_NOT_SUPPORTED);
            TASK_CHECK(out == NULL);
            delete src;
        }

        // non-unit media rate
        {
            AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                        1000, 1000, 1, false,
                                        false, 0, 0, 0);
            AP4_ContainerAtom* edts =
                new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
            AP4_ElstAtom* elst = new AP4_ElstAtom();
            elst->AddEntry(AP4_ElstEntry(1000, 0, 2));
            edts->AddChild(elst);
            const_cast<AP4_TrakAtom*>(src->GetTrakAtom())->AddChild(edts);

            AP4_Track* out = NULL;
            TASK_CHECK(src->CreateMontage(ranges, out) ==
                       AP4_ERROR_NOT_SUPPORTED);
            TASK_CHECK(out == NULL);
            delete src;
        }
    }

    // P7: negative composition offsets are not supported
    {
        SampSpec specs[3] = {
            {  0, 0x80000000U, 100, true,  0 }, // interpreted as negative
            {100, 0,          100, false, 0 },
            {200, 0,          100, false, 0 }
        };
        AP4_Track* src = BuildTrack(specs, 3, 1000, 1000,
                                    300, 300, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0; r.end = 100;
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) ==
                   AP4_ERROR_NOT_SUPPORTED);
        TASK_CHECK(out == NULL);
        delete src;
    }

    // P8: non-video tracks and trailing empty edits are not supported
    {
        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0; r.end = 100;
        ranges.Append(r);

        // non-video track
        {
            AP4_MemoryByteStream* payload = new AP4_MemoryByteStream(2);
            AP4_UI08 bytes[2] = {1, 2};
            payload->Write(bytes, 2);
            AP4_SyntheticSampleTable* table =
                new AP4_SyntheticSampleTable();
            table->AddSampleDescription(new AP4_SampleDescription(
                AP4_SampleDescription::TYPE_UNKNOWN,
                AP4_ATOM_TYPE('r','a','w',' '), NULL));
            table->AddSample(*payload, 0, 1, 100, 0, 0,   0, true);
            table->AddSample(*payload, 1, 1, 100, 0, 100, 0, true);
            payload->Release();
            AP4_Track* src = new AP4_Track(AP4_Track::TYPE_AUDIO, table,
                                           1, 1000, 200, 1000, 200,
                                           "und", 0, 0);
            AP4_Track* out = NULL;
            TASK_CHECK(src->CreateMontage(ranges, out) ==
                       AP4_ERROR_NOT_SUPPORTED);
            TASK_CHECK(out == NULL);
            delete src;
        }

        // empty edit that is not the leading edit
        {
            AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                        1000, 1000, 1, false,
                                        false, 0, 0, 0);
            AP4_ContainerAtom* edts =
                new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
            AP4_ElstAtom* elst = new AP4_ElstAtom();
            elst->AddEntry(AP4_ElstEntry(800, 0, 1));
            elst->AddEntry(AP4_ElstEntry(200, -1, 1)); // trailing blank
            edts->AddChild(elst);
            const_cast<AP4_TrakAtom*>(src->GetTrakAtom())->AddChild(edts);

            AP4_Track* out = NULL;
            TASK_CHECK(src->CreateMontage(ranges, out) ==
                       AP4_ERROR_NOT_SUPPORTED);
            TASK_CHECK(out == NULL);
            delete src;
        }
    }

    // P9: overlapping ranges each keep a private block, duration is summed
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r1; r1.begin = 200; r1.end = 500;
        AP4_Track::EditRange r2; r2.begin = 400; r2.end = 700;
        ranges.Append(r1);
        ranges.Append(r2);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 600);
        TASK_CHECK(out->GetSampleCount() == 9); // block of 4 then block of 5
        const AP4_UI08 expected[9] = {
            100, 101, 102, 103,
            100, 101, 102, 103, 104
        };
        for (unsigned i = 0; i < 9; i++) {
            TASK_CHECK(SampleByte(out, i) == expected[i]);
        }
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 200);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 300);
        // second block starts at output media time 400, window 400 inside it
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 800);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 300);
        delete out;
        delete src;
    }

    return 0;
}

/*----------------------------------------------------------------------
|   decode group: exact retained decode samples, rollback and cuts
+---------------------------------------------------------------------*/
static int
RunDecodeTests()
{
    // D1: mid-GOP cut keeps the sync sample and the whole decode chain
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 300; r.end = 500; // display samples 2 and 3
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 4);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(out, i) == (AP4_UI08)(100 + i));
        }
        AP4_Sample s;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(0, s)));
        TASK_CHECK(s.GetDts() == 0 && s.IsSync());
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(1, s)));
        TASK_CHECK(s.GetDts() == 100 && s.GetCtsDelta() == 400 &&
                   !s.IsSync());
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(2, s)));
        TASK_CHECK(s.GetDts() == 200 && s.GetCtsDelta() == 100);
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(3, s)));
        TASK_CHECK(s.GetDts() == 300 && s.GetCtsDelta() == 100);
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);
        delete out;
        delete src;
    }

    // D2: cut across display pictures keeps hidden in-between samples
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 500; r.end = 700; // display samples 1 and 4
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 5);
        // decode chain 0..4, including pictures 2 and 3 whose display
        // intervals lie outside the requested window
        for (unsigned i = 0; i < 5; i++) {
            TASK_CHECK(SampleByte(out, i) == (AP4_UI08)(100 + i));
        }
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 500);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);
        delete out;
        delete src;
    }

    // D3: a cut in the second GOP rolls back to its own sync, not index 0
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 700; r.end = 900; // display samples 6 and 7
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 4);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(out, i) == (AP4_UI08)(104 + i));
        }
        AP4_Sample s;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(0, s)));
        TASK_CHECK(s.IsSync() && s.GetDts() == 0);
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);
        delete out;
        delete src;
    }

    // D4: a repeated request produces two private, independent blocks
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 300; r.end = 500;
        ranges.Append(r);
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 8);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(out, i)     == (AP4_UI08)(100 + i));
            TASK_CHECK(SampleByte(out, i + 4) == (AP4_UI08)(100 + i));
        }
        AP4_Sample s;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(4, s)));
        TASK_CHECK(s.GetDts() == 400); // second block restarts at the tail
        AP4_ElstAtom* elst = TrackElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 700);
        delete out;

        // destroying the montage must not invalidate the source track
        TASK_CHECK(src->GetSampleCount() == 8);
        TASK_CHECK(SampleByte(src, 7) == 107);
        AP4_Sample live;
        TASK_CHECK(AP4_SUCCEEDED(src->GetSample(7, live)));
        TASK_CHECK(live.GetDts() == 700 && live.GetCtsDelta() == 100);
        delete src;
    }

    // D5: no real sync sample anywhere: index 0 must not be assumed a sync
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, true /*all sync flag
                                    is overridden below*/, false, 0, 0, 0);
        TASK_CHECK(src != NULL);
        // force every sample to be a non-sync sample
        AP4_SyntheticSampleTable* table =
            AP4_DYNAMIC_CAST(AP4_SyntheticSampleTable, src->GetSampleTable());
        TASK_CHECK(table != NULL);
        for (unsigned i = 0; i < table->GetSampleCount(); i++) {
            table->UseSample(i).SetSync(false);
        }

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0; r.end = 500;
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) ==
                   AP4_ERROR_NOT_SUPPORTED);
        TASK_CHECK(out == NULL);
        delete src;
    }

    return 0;
}

/*----------------------------------------------------------------------
|   roundtrip group: real FileWriter write and independent re-parse
+---------------------------------------------------------------------*/
static int
RunRoundtripTests()
{
    // R1: repeated blocks survive a real write/reparse cycle
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 300; r.end = 500; // one 4-sample decode block per request
        ranges.Append(r);
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);

        WrittenFile written = WriteAndReopen(out, 1000);
        AP4_Track* t = written.track;
        TASK_CHECK(t->GetSampleCount() == 8);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(t, i)     == (AP4_UI08)(100 + i));
            TASK_CHECK(SampleByte(t, i + 4) == (AP4_UI08)(100 + i));
        }
        AP4_Sample s;
        TASK_CHECK(AP4_SUCCEEDED(t->GetSample(0, s)));
        TASK_CHECK(s.GetDts() == 0 && s.GetCtsDelta() == 200);
        TASK_CHECK(AP4_SUCCEEDED(t->GetSample(4, s)));
        TASK_CHECK(s.GetDts() == 400 && s.GetCtsDelta() == 200);
        AP4_ElstAtom* elst = TrackElst(t);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 700);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 200);
        delete written.file;
        written.stream->Release();
        delete src;
    }

    // R2: description index switching is preserved through the roundtrip
    {
        SampSpec specs[4] = {
            {  0, 0, 100, true,  0 },
            {100, 0, 100, true,  0 },
            {200, 0, 100, true,  1 },
            {300, 0, 100, true,  1 }
        };
        AP4_Track* src = BuildTrack(specs, 4, 1000, 1000,
                                    400, 400, 2, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0; r.end = 400;
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);

        WrittenFile written = WriteAndReopen(out, 1000);
        AP4_Track* t = written.track;
        TASK_CHECK(t->GetSampleDescriptionCount() == 2);
        TASK_CHECK(t->GetSampleCount() == 4);
        AP4_Sample s;
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(AP4_SUCCEEDED(t->GetSample(i, s)));
            TASK_CHECK(s.GetDescriptionIndex() == (i < 2 ? 0 : 1));
            TASK_CHECK(SampleByte(t, i) == (AP4_UI08)(100 + i));
        }
        delete written.file;
        written.stream->Release();
        delete src;
    }

    // R3: a result that is blank end to end writes and re-parses with
    //     no samples at all, and non-temporal properties survive
    {
        AP4_Track* src = BuildStandardTrack(1000, 1000, 200, 800);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 50; r.end = 150; // fully inside the leading empty edit
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 0);
        TASK_CHECK(out->GetDuration() == 100);
        // track id, language and geometry come from the source track
        TASK_CHECK(out->GetId() == src->GetId());
        TASK_CHECK(std::strcmp(out->GetTrackLanguage(), "und") == 0);
        TASK_CHECK(out->GetWidth() == src->GetWidth());
        TASK_CHECK(out->GetHeight() == src->GetHeight());
        TASK_CHECK(out->GetType() == AP4_Track::TYPE_VIDEO);
        TASK_CHECK(out->GetMovieTimeScale() == src->GetMovieTimeScale());
        TASK_CHECK(out->GetMediaTimeScale() == src->GetMediaTimeScale());

        WrittenFile written = WriteAndReopen(out, 1000);
        AP4_Track* t = written.track;
        TASK_CHECK(t->GetSampleCount() == 0);
        TASK_CHECK(t->GetDuration() == 100);
        TASK_CHECK(t->GetId() == src->GetId());
        TASK_CHECK(std::strcmp(t->GetTrackLanguage(), "und") == 0);
        TASK_CHECK(t->GetWidth() == src->GetWidth());
        TASK_CHECK(t->GetHeight() == src->GetHeight());
        AP4_ElstAtom* elst = TrackElst(t);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == -1);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);
        delete written.file;
        written.stream->Release();
        delete src;
    }

    // R4: fields that do not fit in 32 bits force version-1 atoms
    {
        SampSpec specs[2] = {
            {          0, 0, 0xFFFFFFFFULL, true, 0 },
            { 0xFFFFFFFFULL, 0, 0xFFFFFFFFULL, true, 0 }
        };
        AP4_UI64 media_duration = 0x1FFFFFFFEULL;
        AP4_Track* src = BuildTrack(specs, 2, 1000, 1000,
                                    media_duration, media_duration,
                                    1, false, false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> ranges;
        AP4_Track::EditRange r;
        r.begin = 0;
        r.end   = 0x100000000ULL; // larger than 2^32 movie ticks
        ranges.Append(r);

        AP4_Track* out = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(ranges, out)));
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 2);
        TASK_CHECK(out->GetDuration() == 0x100000000ULL);
        TASK_CHECK(out->GetMediaDuration() == 0x1FFFFFFFEULL);

        WrittenFile written = WriteAndReopen(out, 1000, 0x100000000ULL);
        AP4_Track* t = written.track;
        TASK_CHECK(t->GetDuration() == 0x100000000ULL);
        TASK_CHECK(t->GetMediaDuration() == 0x1FFFFFFFEULL);
        TASK_CHECK(written.file->GetMovie()->GetMvhdAtom() != NULL);
        TASK_CHECK(written.file->GetMovie()->GetMvhdAtom()->GetVersion() == 1);
        TASK_CHECK(written.file->GetMovie()->GetMvhdAtom()->GetDuration() ==
                   0x100000000ULL);

        AP4_ElstAtom* elst = TrackElst(t);
        TASK_CHECK(elst != NULL);
        TASK_CHECK(elst->GetVersion() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration ==
                   0x100000000ULL);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 0);

        AP4_TkhdAtom* tkhd =
            const_cast<AP4_TrakAtom*>(t->GetTrakAtom())->UseTkhdAtom();
        TASK_CHECK(tkhd != NULL && tkhd->GetVersion() == 1);
        AP4_MdhdAtom* mdhd = AP4_DYNAMIC_CAST(AP4_MdhdAtom,
            const_cast<AP4_TrakAtom*>(t->GetTrakAtom())
                ->FindChild("mdia/mdhd"));
        TASK_CHECK(mdhd != NULL && mdhd->GetVersion() == 1);

        AP4_Sample s;
        TASK_CHECK(AP4_SUCCEEDED(t->GetSample(0, s)));
        TASK_CHECK(s.GetDts() == 0);
        TASK_CHECK(AP4_SUCCEEDED(t->GetSample(1, s)));
        TASK_CHECK(s.GetDts() == 0xFFFFFFFFULL);
        delete written.file;
        written.stream->Release();
        delete src;
    }

    // R5: a montage re-opened from disk can itself be re-montaged
    {
        AP4_Track* src = BuildTrack(TABLE_T, 8, 1000, 1000,
                                    1000, 1000, 1, false,
                                    false, 0, 0, 0);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> first_ranges;
        AP4_Track::EditRange r1; r1.begin = 300; r1.end = 500;
        first_ranges.Append(r1);
        AP4_Track* first = NULL;
        TASK_CHECK(AP4_SUCCEEDED(src->CreateMontage(first_ranges, first)));
        TASK_CHECK(first != NULL);

        WrittenFile written = WriteAndReopen(first, 1000);
        AP4_Track* reparsed = written.track;
        TASK_CHECK(reparsed->GetSampleCount() == 4);

        // cut the reparsed montage again, over its whole visible window
        AP4_Array<AP4_Track::EditRange> second_ranges;
        AP4_Track::EditRange r2; r2.begin = 0; r2.end = 200;
        second_ranges.Append(r2);
        AP4_Track* second = NULL;
        TASK_CHECK(AP4_SUCCEEDED(reparsed->CreateMontage(second_ranges,
                                                         second)));
        TASK_CHECK(second != NULL);
        TASK_CHECK(second->GetDuration() == 200);
        TASK_CHECK(second->GetSampleCount() == 4);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(second, i) == (AP4_UI08)(100 + i));
        }
        AP4_ElstAtom* elst = TrackElst(second);
        TASK_CHECK(elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 300);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 200);

        // the second montage also round-trips
        WrittenFile rewritten = WriteAndReopen(second, 1000);
        AP4_Track* t2 = rewritten.track;
        TASK_CHECK(t2->GetSampleCount() == 4);
        TASK_CHECK(t2->GetDuration() == 200);
        for (unsigned i = 0; i < 4; i++) {
            TASK_CHECK(SampleByte(t2, i) == (AP4_UI08)(100 + i));
        }
        delete rewritten.file;
        rewritten.stream->Release();
        delete written.file;
        written.stream->Release();
        delete src;
    }

    return 0;
}

/*----------------------------------------------------------------------
|   entry point used by task_support/driver.cpp
+---------------------------------------------------------------------*/
int
run_montage_tests(const char* group)
{
    if (std::strcmp(group, "projection") == 0) {
        return RunProjectionTests();
    } else if (std::strcmp(group, "decode") == 0) {
        return RunDecodeTests();
    } else if (std::strcmp(group, "roundtrip") == 0) {
        return RunRoundtripTests();
    }
    return 1;
}
