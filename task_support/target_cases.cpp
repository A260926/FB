/*----------------------------------------------------------------------
|
|   Target tests for AP4_Track::CreateMontage
|
|   All expected timelines below are hand-written and hand-computed; the
|   montage implementation is never used to generate the expected values.
|
+---------------------------------------------------------------------*/
#include "Ap4.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4ElstAtom.h"
#include "Ap4FileWriter.h"
#include "Ap4MdhdAtom.h"
#include "Ap4TkhdAtom.h"
#include "check.h"
#include <string.h>

/*----------------------------------------------------------------------
|   hand-written sample specification (decode order = table order)
+---------------------------------------------------------------------*/
struct MontageSampleSpec {
    AP4_UI64       dts;
    AP4_UI32       cts_delta;
    AP4_UI32       duration;
    bool           sync;
    unsigned       description_index;
    unsigned char  payload;
};

/*----------------------------------------------------------------------
|   MontageMakeTrack
+---------------------------------------------------------------------*/
static AP4_Track*
MontageMakeTrack(AP4_UI32                    movie_time_scale,
                 AP4_UI32                    media_time_scale,
                 AP4_UI64                    track_duration,
                 AP4_UI64                    media_duration,
                 const MontageSampleSpec*    specs,
                 AP4_Cardinal                sample_count,
                 AP4_Cardinal                description_count,
                 const AP4_ElstEntry*        elst_entries,
                 AP4_Cardinal                elst_count)
{
    AP4_MemoryByteStream* input = new AP4_MemoryByteStream(sample_count);
    for (AP4_Cardinal i=0; i<sample_count; i++) {
        input->Write(&specs[i].payload, 1);
    }

    AP4_SyntheticSampleTable* table = new AP4_SyntheticSampleTable();
    for (AP4_Cardinal d=0; d<description_count; d++) {
        table->AddSampleDescription(new AP4_SampleDescription(
            AP4_SampleDescription::TYPE_UNKNOWN,
            AP4_ATOM_TYPE('r','a','w',' '), NULL));
    }
    for (AP4_Cardinal i=0; i<sample_count; i++) {
        AP4_Result r = table->AddSample(*input,
                                        i, 1,
                                        specs[i].duration,
                                        specs[i].description_index,
                                        specs[i].dts,
                                        specs[i].cts_delta,
                                        specs[i].sync);
        if (AP4_FAILED(r)) { delete table; input->Release(); return NULL; }
    }
    input->Release(); // the samples keep their own reference

    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_VIDEO,
                                     table, 1,
                                     movie_time_scale, track_duration,
                                     media_time_scale, media_duration,
                                     "und", 0x01000000, 0x00800000);
    if (elst_count) {
        AP4_ContainerAtom* edts = new AP4_ContainerAtom(AP4_ATOM_TYPE_EDTS);
        AP4_ElstAtom* elst = new AP4_ElstAtom();
        for (AP4_Cardinal e=0; e<elst_count; e++) elst->AddEntry(elst_entries[e]);
        edts->AddChild(elst);
        track->UseTrakAtom()->AddChild(edts);
    }
    return track;
}

/*----------------------------------------------------------------------
|   MontageGetElst
+---------------------------------------------------------------------*/
static AP4_ElstAtom*
MontageGetElst(AP4_Track* track)
{
    return AP4_DYNAMIC_CAST(AP4_ElstAtom,
                            track->UseTrakAtom()->FindChild("edts/elst"));
}

/*----------------------------------------------------------------------
|   MontagePayloads: return the payload bytes of every output sample
+---------------------------------------------------------------------*/
static AP4_Result
MontagePayloads(AP4_Track* track, AP4_Array<unsigned char>& payloads)
{
    payloads.SetItemCount(0);
    for (AP4_Cardinal i=0; i<track->GetSampleCount(); i++) {
        AP4_Sample sample;
        AP4_DataBuffer data;
        AP4_Result r = track->ReadSample(i, sample, data);
        if (AP4_FAILED(r)) return r;
        if (data.GetDataSize() != 1) return AP4_ERROR_INVALID_FORMAT;
        payloads.Append(data.GetData()[0]);
    }
    return AP4_SUCCESS;
}

/*----------------------------------------------------------------------
|   MontageRoundTrip: write the track and parse the result back.
|   Ownership of 'track' is transferred to the AP4_File (the movie owns
|   the track and the file owns the movie), so the caller must not
|   delete 'track' afterwards.
+---------------------------------------------------------------------*/
static AP4_File*
MontageRoundTrip(AP4_Track* track, AP4_MemoryByteStream*& encoded)
{
    AP4_Movie* movie = new AP4_Movie(track->GetMovieTimeScale());
    movie->AddTrack(track);
    AP4_File* file = new AP4_File(movie);

    encoded = new AP4_MemoryByteStream();
    AP4_Result r = AP4_FileWriter::Write(*file, *encoded);
    if (AP4_FAILED(r)) {
        delete file;
        encoded->Release();
        encoded = NULL;
        return NULL;
    }
    encoded->Seek(0);

    AP4_File* reread = new AP4_File(*encoded);
    delete file; // deletes the written movie and the track it owns
    return reread;
}

/*----------------------------------------------------------------------
|   hand-written 8-sample reordered timeline (two closed GOPs)
|
|   decode  dts   cts   display interval   sync
|   s0        0  1200   [1200,1300)         yes
|   s1      100  1000   [1000,1100)
|   s2      200  1100   [1100,1200)
|   s3      300  1300   [1300,1400)
|   s4      400  1600   [1600,1700)         yes
|   s5      500  1400   [1400,1500)
|   s6      600  1500   [1500,1600)
|   s7      700  1700   [1700,1800)
|
|   display order: s1 s2 s0 s3 | s5 s6 s4 s7
|   elst media edit starts at media time 1000, movie duration 800
+---------------------------------------------------------------------*/
static const AP4_UI32 MONTAGE_TICK = 100;
static AP4_Track*
MontageMakeReorderedTrack(bool with_sync,
                          AP4_UI32 movie_time_scale = 1000,
                          AP4_UI32 media_time_scale = 1000,
                          AP4_UI64 leading_empty     = 0)
{
    static const MontageSampleSpec specs[8] = {
        {   0, 1200, MONTAGE_TICK, true,  0, 100},
        { 100,  900, MONTAGE_TICK, false, 0, 101},
        { 200,  900, MONTAGE_TICK, false, 0, 102},
        { 300, 1000, MONTAGE_TICK, false, 0, 103},
        { 400, 1200, MONTAGE_TICK, true,  0, 104},
        { 500,  900, MONTAGE_TICK, false, 0, 105},
        { 600,  900, MONTAGE_TICK, false, 0, 106},
        { 700, 1000, MONTAGE_TICK, false, 0, 107},
    };
    MontageSampleSpec local[8];
    for (unsigned i=0; i<8; i++) {
        local[i] = specs[i];
        if (!with_sync) local[i].sync = false;
    }

    AP4_ElstEntry entries[2];
    AP4_Cardinal entry_count = 0;
    AP4_UI64 track_duration = 800;
    if (leading_empty) {
        entries[entry_count++] = AP4_ElstEntry(leading_empty, -1, 1);
        track_duration += leading_empty;
    }
    entries[entry_count++] = AP4_ElstEntry(800, 1000, 1);

    return MontageMakeTrack(movie_time_scale, media_time_scale,
                            track_duration, 1800,
                            local, 8, 1, entries, entry_count);
}

/*======================================================================
|   projection group
+=====================================================================*/
static int
run_projection_tests()
{
    /* -- cross-empty projection -------------------------------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true, 1000, 1000, 100);
        TASK_CHECK(src != NULL);
        TASK_CHECK(src->GetDuration() == 900);

        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 200)); // 50 empty + 100 media
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 150);

        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == -1);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 50);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 1000);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 100);

        // media cut [1000,1100) displays s1 only; roll-in pulls s0 too
        TASK_CHECK(out->GetSampleCount() == 2);
        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        TASK_CHECK(payloads.ItemCount() == 2 &&
                   payloads[0] == 100 && payloads[1] == 101);

        // non-temporal properties are inherited
        TASK_CHECK(out->GetId() == 1);
        TASK_CHECK(out->GetWidth() == 0x01000000);
        TASK_CHECK(strcmp(out->GetTrackLanguage(), "und") == 0);
        TASK_CHECK(out->GetSampleDescriptionCount() == 1);

        delete out;
        delete src;
    }

    /* -- different time scales: exact conversion required ------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true, 300, 1000);
        TASK_CHECK(src != NULL);

        AP4_Array<AP4_Track::EditRange> good;
        good.Append(AP4_Track::EditRange(0, 3)); // 3 movie ticks = 10 media
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(good, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL && out->GetDuration() == 3);
        TASK_CHECK(MontageGetElst(out)->GetEntries()[0].m_SegmentDuration == 3);
        delete out;

        AP4_Array<AP4_Track::EditRange> bad;
        bad.Append(AP4_Track::EditRange(0, 1)); // 1000/300 is not integral
        out = NULL;
        TASK_CHECK(src->CreateMontage(bad, out) == AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        delete src;
    }

    /* -- out-of-order, overlapping and repeated requests --------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        TASK_CHECK(src != NULL);

        // hand computation:
        //  [250,350) -> media [1250,1350): displays s0,s3 -> block s0..s3 (4)
        //  [50,150)  -> media [1050,1150): displays s1,s2 -> block s0..s2 (3)
        //  [250,350) repeated -> another independent 4-sample block
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(250, 350));
        ranges.Append(AP4_Track::EditRange(50, 150));
        ranges.Append(AP4_Track::EditRange(250, 350));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 300);
        TASK_CHECK(out->GetSampleCount() == 11);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        static const unsigned char expected[11] = {
            100,101,102,103, 100,101,102, 100,101,102,103
        };
        TASK_CHECK(payloads.ItemCount() == 11);
        for (unsigned i=0; i<11; i++) TASK_CHECK(payloads[i] == expected[i]);

        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 3);
        // block spans are 400, 300, 400 media ticks; cut positions are
        // 1250 and 1050 relative to their own block origins
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 1250);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 400+1050);
        TASK_CHECK(elst->GetEntries()[2].m_MediaTime == 700+1250);
        // the repeated GOP points at the second copy, not the first one
        TASK_CHECK(elst->GetEntries()[2].m_MediaTime !=
                   elst->GetEntries()[0].m_MediaTime);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 100);
        TASK_CHECK(elst->GetEntries()[2].m_SegmentDuration == 100);

        delete out;
        delete src;
    }

    /* -- failures leave the source track and the output untouched ------ */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        AP4_Cardinal before_count = src->GetSampleCount();
        AP4_UI64 before_duration = src->GetDuration();
        AP4_Cardinal before_elst = MontageGetElst(src)->GetEntries().ItemCount();

        AP4_Array<AP4_Track::EditRange> empty_list;
        AP4_Track* out = (AP4_Track*)0x1;
        TASK_CHECK(src->CreateMontage(empty_list, out) == AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        AP4_Array<AP4_Track::EditRange> bad_range;
        bad_range.Append(AP4_Track::EditRange(100, 100)); // begin == end
        TASK_CHECK(src->CreateMontage(bad_range, out) == AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        AP4_Array<AP4_Track::EditRange> too_far;
        too_far.Append(AP4_Track::EditRange(0, 801)); // end > track duration
        TASK_CHECK(src->CreateMontage(too_far, out) == AP4_ERROR_INVALID_PARAMETERS);
        TASK_CHECK(out == NULL);

        TASK_CHECK(src->GetSampleCount() == before_count);
        TASK_CHECK(src->GetDuration() == before_duration);
        TASK_CHECK(MontageGetElst(src)->GetEntries().ItemCount() == before_elst);

        delete src;
    }

    return 0;
}

/*======================================================================
|   decode group
+=====================================================================*/
static int
run_decode_tests()
{
    /* -- cut inside a GOP keeps the closed-GOP roll-in ----------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        TASK_CHECK(src != NULL);

        // [50,150) -> media [1050,1150): display samples s1,s2 (decode
        // indexes 1,2); the roll-in must start at the real sync s0.
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 150));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 3);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        TASK_CHECK(payloads[0] == 100 && payloads[1] == 101 && payloads[2] == 102);

        AP4_Sample s0, s1, s2;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(0, s0)));
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(1, s1)));
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(2, s2)));
        TASK_CHECK(s0.IsSync() && !s1.IsSync() && !s2.IsSync());
        // decode order and timestamps are preserved (block origin dts 0)
        TASK_CHECK(s0.GetDts() == 0 && s1.GetDts() == 100 && s2.GetDts() == 200);
        TASK_CHECK(s0.GetCts() == 1200 && s1.GetCts() == 1000 && s2.GetCts() == 1100);
        TASK_CHECK(s0.GetDescriptionIndex() == 0);

        // cut is not snapped to a sample/sync boundary
        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 1050);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);

        delete out;
        delete src;
    }

    /* -- cut crossing a GOP boundary keeps one continuous decode run --- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);

        // [250,550) -> media [1250,1550): displays s0,s3,s5,s6, i.e.
        // decode indexes {0,3,5,6}; one run s0..s6 (7 samples) is kept,
        // the range is not merged into anything larger
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(250, 550));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out->GetSampleCount() == 7);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        for (unsigned i=0; i<7; i++) TASK_CHECK(payloads[i] == (unsigned char)(100+i));

        AP4_Sample sample;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(4, sample)));
        TASK_CHECK(sample.IsSync()); // s4 keeps its sync flag
        TASK_CHECK(sample.GetCts() == 1600);

        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 1250);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 300);
        // media timeline is longer than the displayed cut
        TASK_CHECK(out->GetMediaDuration() >= 700);

        delete out;
        delete src;
    }

    /* -- repeated requests carry independent copies of the decode run -- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 150));
        ranges.Append(AP4_Track::EditRange(50, 150));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out->GetSampleCount() == 6);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        TASK_CHECK(payloads[0] == 100 && payloads[3] == 100);
        TASK_CHECK(payloads[1] == 101 && payloads[4] == 101);
        TASK_CHECK(payloads[2] == 102 && payloads[5] == 102);

        // DTS restarts relative to each block origin and blocks are
        // concatenated at the decode tail (block span = 300)
        AP4_Sample first, second;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(0, first)));
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(3, second)));
        TASK_CHECK(first.GetDts() == 0 && second.GetDts() == 300);
        TASK_CHECK(first.GetCts() == 1200 && second.GetCts() == 1500);

        delete out;
        delete src;
    }

    /* -- no real sync before the cut: error, index 0 is not assumed ---- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(false);
        TASK_CHECK(src != NULL);
        AP4_Cardinal before = src->GetSampleCount();

        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 150));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_ERROR_NOT_SUPPORTED);
        TASK_CHECK(out == NULL);
        TASK_CHECK(src->GetSampleCount() == before);

        delete src;
    }

    /* -- no edts: media time 0 at movie time 0, cut inside a GOP ------- */
    {
        // hand-written timeline (no edit list, media time 0 at movie 0):
        //   s0 dts/cts 0   sync, s1 100, s2 200, s3 300 sync, durations 100
        static const MontageSampleSpec specs[4] = {
            {  0, 0, 100, true,  0, 210},
            {100, 0, 100, false, 0, 211},
            {200, 0, 100, false, 0, 212},
            {300, 0, 100, true,  0, 213},
        };
        AP4_Track* src = MontageMakeTrack(1000, 1000, 400, 400,
                                          specs, 4, 1, NULL, 0);
        TASK_CHECK(src != NULL);

        // [150,250) displays s1,s2; roll-in starts at the real sync s0.
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(150, 250));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out->GetSampleCount() == 3);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        TASK_CHECK(payloads[0] == 210 && payloads[1] == 211 && payloads[2] == 212);

        AP4_Sample s0, s1, s2;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(0, s0)));
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(1, s1)));
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(2, s2)));
        // the block starts at media time 0; DTS values are preserved
        TASK_CHECK(s0.GetDts() == 0 && s1.GetDts() == 100 && s2.GetDts() == 200);
        TASK_CHECK(s0.IsSync() && !s1.IsSync());

        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 1);
        // the edit hides the roll-in and the pictures outside [150,250)
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 150);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);
        // media timeline (300 ticks through s2) is longer than the cut
        TASK_CHECK(out->GetMediaDuration() == 300);

        delete out;
        delete src;
    }

    /* -- destroying the source track must not invalidate the montage --- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 150));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);

        delete src; // the montage keeps its own stream references

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(out, payloads)));
        TASK_CHECK(payloads.ItemCount() == 3 &&
                   payloads[0] == 100 && payloads[1] == 101 && payloads[2] == 102);
        AP4_Sample sample;
        TASK_CHECK(AP4_SUCCEEDED(out->GetSample(1, sample)));
        TASK_CHECK(sample.GetDts() == 100 && sample.IsSync() == false);

        delete out;
    }

    return 0;
}

/*======================================================================
|   roundtrip group
+=====================================================================*/
static int
run_roundtrip_tests()
{
    /* -- repeated blocks survive a real file write/read --------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 150));
        ranges.Append(AP4_Track::EditRange(50, 150));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);

        // MontageRoundTrip takes ownership of 'out'
        AP4_MemoryByteStream* encoded = NULL;
        AP4_File* file = MontageRoundTrip(out, encoded);
        TASK_CHECK(file != NULL && encoded != NULL);
        AP4_Track* reread = file->GetMovie()->GetTrack(1);
        TASK_CHECK(reread != NULL);
        TASK_CHECK(reread->GetDuration() == 200);
        TASK_CHECK(reread->GetSampleCount() == 6);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(reread, payloads)));
        static const unsigned char expected[6] = {100,101,102,100,101,102};
        for (unsigned i=0; i<6; i++) TASK_CHECK(payloads[i] == expected[i]);

        AP4_Sample a, b;
        TASK_CHECK(AP4_SUCCEEDED(reread->GetSample(0, a)));
        TASK_CHECK(AP4_SUCCEEDED(reread->GetSample(3, b)));
        TASK_CHECK(a.GetDts() == 0 && b.GetDts() == 300);
        TASK_CHECK(a.GetCts() == 1200 && b.GetCts() == 1500);

        AP4_ElstAtom* elst = MontageGetElst(reread);
        TASK_CHECK(elst != NULL && elst->GetEntries().ItemCount() == 2);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == 1050);
        TASK_CHECK(elst->GetEntries()[1].m_MediaTime == 1350);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 100);
        TASK_CHECK(elst->GetEntries()[1].m_SegmentDuration == 100);

        delete file;
        encoded->Release();
        delete src;
    }

    /* -- description index switches are preserved ---------------------- */
    {
        static const MontageSampleSpec specs[4] = {
            {  0, 0, 100, true, 0, 200},
            {100, 0, 100, true, 1, 201},
            {200, 0, 100, true, 0, 202},
            {300, 0, 100, true, 1, 203},
        };
        AP4_Track* src = MontageMakeTrack(1000, 1000, 400, 400,
                                          specs, 4, 2, NULL, 0);
        TASK_CHECK(src != NULL);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(50, 350));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out->GetSampleDescriptionCount() == 2);

        AP4_MemoryByteStream* encoded = NULL;
        AP4_File* file = MontageRoundTrip(out, encoded);
        TASK_CHECK(file != NULL);
        AP4_Track* reread = file->GetMovie()->GetTrack(1);
        TASK_CHECK(reread != NULL);
        TASK_CHECK(reread->GetSampleDescriptionCount() == 2);
        TASK_CHECK(reread->GetSampleCount() == 4);
        for (unsigned i=0; i<4; i++) {
            AP4_Sample sample;
            TASK_CHECK(AP4_SUCCEEDED(reread->GetSample(i, sample)));
            TASK_CHECK(sample.GetDescriptionIndex() == i%2);
        }
        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(reread, payloads)));
        TASK_CHECK(payloads[0] == 200 && payloads[3] == 203);

        delete file;
        encoded->Release();
        delete src;
    }

    /* -- a fully empty result still writes and reopens ----------------- */
    {
        AP4_Track* src = MontageMakeReorderedTrack(true, 1000, 1000, 100);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(0, 100)); // inside the empty edit
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetSampleCount() == 0);
        TASK_CHECK(out->GetDuration() == 100);
        AP4_ElstAtom* elst = MontageGetElst(out);
        TASK_CHECK(elst->GetEntries().ItemCount() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_MediaTime == -1);

        AP4_MemoryByteStream* encoded = NULL;
        AP4_File* file = MontageRoundTrip(out, encoded);
        TASK_CHECK(file != NULL);
        AP4_Track* reread = file->GetMovie()->GetTrack(1);
        TASK_CHECK(reread != NULL);
        TASK_CHECK(reread->GetSampleCount() == 0);
        TASK_CHECK(reread->GetDuration() == 100);
        AP4_ElstAtom* reread_elst = MontageGetElst(reread);
        TASK_CHECK(reread_elst != NULL &&
                   reread_elst->GetEntries().ItemCount() == 1 &&
                   reread_elst->GetEntries()[0].m_MediaTime == -1 &&
                   reread_elst->GetEntries()[0].m_SegmentDuration == 100);

        delete file;
        encoded->Release();
        delete src;
    }

    /* -- 64-bit durations force elst/tkhd/mdhd version 1 --------------- */
    {
        const AP4_UI32 BIG = 0xFFFFFFFF;
        static const MontageSampleSpec specs[2] = {
            {         0, 0, BIG, true, 0, 30},
            {BIG,          0, BIG, true, 0, 31},
        };
        AP4_Track* src = MontageMakeTrack(1000, 1000,
                                          2ULL*BIG, 2ULL*BIG,
                                          specs, 2, 1, NULL, 0);
        TASK_CHECK(src != NULL);
        AP4_Array<AP4_Track::EditRange> ranges;
        ranges.Append(AP4_Track::EditRange(0, 2ULL*BIG));
        AP4_Track* out = NULL;
        TASK_CHECK(src->CreateMontage(ranges, out) == AP4_SUCCESS);
        TASK_CHECK(out != NULL);
        TASK_CHECK(out->GetDuration() == 2ULL*BIG);

        AP4_MemoryByteStream* encoded = NULL;
        AP4_File* file = MontageRoundTrip(out, encoded);
        TASK_CHECK(file != NULL);
        AP4_Track* reread = file->GetMovie()->GetTrack(1);
        TASK_CHECK(reread != NULL);
        TASK_CHECK(reread->GetDuration() == 2ULL*BIG);
        TASK_CHECK(reread->GetMediaDuration() == 2ULL*BIG);

        AP4_ElstAtom* elst = MontageGetElst(reread);
        TASK_CHECK(elst != NULL);
        TASK_CHECK(elst->GetVersion() == 1);
        TASK_CHECK(elst->GetEntries()[0].m_SegmentDuration == 2ULL*BIG);

        TASK_CHECK(reread->UseTrakAtom()->UseTkhdAtom()->GetVersion() == 1);
        AP4_MdhdAtom* mdhd = AP4_DYNAMIC_CAST(
            AP4_MdhdAtom, reread->UseTrakAtom()->FindChild("mdia/mdhd"));
        TASK_CHECK(mdhd != NULL && mdhd->GetVersion() == 1);
        TASK_CHECK(mdhd->GetDuration() == 2ULL*BIG);

        AP4_Array<unsigned char> payloads;
        TASK_CHECK(AP4_SUCCEEDED(MontagePayloads(reread, payloads)));
        TASK_CHECK(payloads.ItemCount() == 2 &&
                   payloads[0] == 30 && payloads[1] == 31);

        delete file;
        encoded->Release();
        delete src;
    }

    return 0;
}

/*----------------------------------------------------------------------
|   run_montage_tests
+---------------------------------------------------------------------*/
int
run_montage_tests(const char* group)
{
    if (strcmp(group, "projection") == 0) return run_projection_tests();
    if (strcmp(group, "decode") == 0)     return run_decode_tests();
    if (strcmp(group, "roundtrip") == 0)  return run_roundtrip_tests();
    return 1;
}
