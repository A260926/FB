/* Content tests for optional layered SMF Format 2 playback. */

#include "task_support/check.h"
#include "task_support/midi_fixture.hpp"

#include <adlmidi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

typedef task_fixture::Bytes Bytes;

void be16(Bytes &b, unsigned v)
{
    b.push_back(static_cast<unsigned char>((v >> 8) & 255));
    b.push_back(static_cast<unsigned char>(v & 255));
}

void be32(Bytes &b, unsigned long v)
{
    task_fixture::be(b, v, 4);
}

void vlq(Bytes &b, unsigned long v)
{
    task_fixture::vlq(b, static_cast<unsigned>(v));
}

Bytes chunk(const char *name, const Bytes &payload)
{
    Bytes out;
    task_fixture::tag(out, name);
    be32(out, static_cast<unsigned long>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes header(int format, unsigned tracks, unsigned division)
{
    Bytes h;
    task_fixture::tag(h, "MThd");
    be32(h, 6);
    be16(h, format);
    be16(h, tracks);
    be16(h, division);
    return h;
}

struct TrackBuilder
{
    Bytes data;
    unsigned long runningDelta;

    TrackBuilder() : runningDelta(0) {}

    void raw(unsigned long delta, const unsigned char *bytes, size_t count)
    {
        vlq(data, delta);
        for(size_t i = 0; i < count; ++i)
            data.push_back(bytes[i]);
    }

    void meta(unsigned long delta, unsigned char type, const Bytes &value)
    {
        vlq(data, delta);
        data.push_back(0xFF);
        data.push_back(type);
        vlq(data, static_cast<unsigned long>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
    }

    void tempo(unsigned long delta, unsigned usec)
    {
        Bytes v;
        v.push_back(static_cast<unsigned char>((usec >> 16) & 255));
        v.push_back(static_cast<unsigned char>((usec >> 8) & 255));
        v.push_back(static_cast<unsigned char>(usec & 255));
        meta(delta, 0x51, v);
    }

    void textMeta(unsigned long delta, unsigned char type, const std::string &text)
    {
        Bytes v(text.begin(), text.end());
        meta(delta, type, v);
    }

    void deviceName(unsigned long delta, unsigned index)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "SMF2/%u", index);
        textMeta(delta, 0x09, name);
    }

    void channel(unsigned long delta, unsigned char status, unsigned d1, int d2 = -1)
    {
        vlq(data, delta);
        data.push_back(status);
        data.push_back(static_cast<unsigned char>(d1));
        if(d2 >= 0)
            data.push_back(static_cast<unsigned char>(d2));
    }

    void program(unsigned long delta, unsigned ch, unsigned p)
    {
        channel(delta, static_cast<unsigned char>(0xC0 | ch), p);
    }

    void cc(unsigned long delta, unsigned ch, unsigned c, unsigned v)
    {
        channel(delta, static_cast<unsigned char>(0xB0 | ch), c, v);
    }

    void pan(unsigned long delta, unsigned ch, unsigned v) { cc(delta, ch, 10, v); }
    void pedal(unsigned long delta, unsigned ch, unsigned v) { cc(delta, ch, 64, v); }

    void noteOn(unsigned long delta, unsigned ch, unsigned note, unsigned vel = 100)
    {
        channel(delta, static_cast<unsigned char>(0x90 | ch), note, vel);
    }

    void noteOff(unsigned long delta, unsigned ch, unsigned note, unsigned vel = 64)
    {
        channel(delta, static_cast<unsigned char>(0x80 | ch), note, vel);
    }

    void pitchBend(unsigned long delta, unsigned ch, unsigned value)
    {
        channel(delta, static_cast<unsigned char>(0xE0 | ch), value & 127, (value >> 7) & 127);
    }

    void end(unsigned long delta = 0)
    {
        vlq(data, delta);
        data.push_back(0xFF);
        data.push_back(0x2F);
        data.push_back(0);
    }

    Bytes mtrk() const { return chunk("MTrk", data); }
};

Bytes assemble(const Bytes &head, const std::vector<TrackBuilder> &tracks)
{
    Bytes out = head;
    for(size_t i = 0; i < tracks.size(); ++i)
    {
        Bytes t = tracks[i].mtrk();
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

ADL_MIDIPlayer *layeredPlayer(const Bytes &midi)
{
    ADL_MIDIPlayer *d = task_fixture::player();
    if(!d)
        return 0;
    if(adl_setSMF2Layered(d, 1) != 0 ||
       adl_openData(d, midi.data(), static_cast<unsigned long>(midi.size())) != 0)
    {
        adl_close(d);
        return 0;
    }
    return d;
}

bool renderFrames(ADL_MIDIPlayer *d, int frames, std::vector<short> &pcm)
{
    pcm.assign(static_cast<size_t>(frames) * 2, 0);
    int got = adl_play(d, frames * 2, pcm.data());
    return got == frames * 2;
}

bool appendRender(ADL_MIDIPlayer *d, int frames, std::vector<short> &pcm)
{
    size_t old = pcm.size();
    pcm.resize(old + static_cast<size_t>(frames) * 2);
    int got = adl_play(d, frames * 2, pcm.data() + old);
    return got == frames * 2;
}

bool renderFragmented(ADL_MIDIPlayer *d, int totalFrames, std::vector<short> &pcm)
{
    static const int pattern[] = {1, 7, 113};
    int made = 0;
    unsigned calls = 0;
    pcm.clear();
    while(made < totalFrames)
    {
        int ask = pattern[calls % 3];
        if(ask > totalFrames - made)
            ask = totalFrames - made;
        if(!appendRender(d, ask, pcm))
            return false;
        made += ask;
        ++calls;
    }
    return true;
}

bool renderWithTempoChange(ADL_MIDIPlayer *d, int totalFrames, int changeFrame,
                           double tempo, bool fragmented, std::vector<short> &pcm)
{
    pcm.clear();
    if(fragmented)
    {
        if(!renderFragmented(d, changeFrame, pcm))
            return false;
    }
    else if(!appendRender(d, changeFrame, pcm))
        return false;

    adl_setTempo(d, tempo);

    if(fragmented)
    {
        std::vector<short> tail;
        if(!renderFragmented(d, totalFrames - changeFrame, tail))
            return false;
        pcm.insert(pcm.end(), tail.begin(), tail.end());
    }
    else if(!appendRender(d, totalFrames - changeFrame, pcm))
        return false;
    return true;
}

struct HookEvent
{
    ADL_MIDIPlayer *device;
    int track, order, status, d1, d2;
    long long frame;
};

std::vector<HookEvent> hookEvents;

void hookCallback(ADL_MIDIPlayer *device, int track, int order, int status,
                   int d1, int d2, long long frame, void *userData)
{
    std::vector<HookEvent> *events = static_cast<std::vector<HookEvent> *>(userData);
    HookEvent e = {device, track, order, status, d1, d2, frame};
    if(events)
        events->push_back(e);
}

const HookEvent *findNoteOn(int track, int note)
{
    for(size_t i = 0; i < hookEvents.size(); ++i)
    {
        const HookEvent &e = hookEvents[i];
        if(e.track == track && (e.status & 0xF0) == 0x90 && e.d1 == note && e.d2 > 0)
            return &hookEvents[i];
    }
    return 0;
}

bool samePcm(const std::vector<short> &a, const std::vector<short> &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

int clocks_integral_and_fragments()
{
    std::vector<TrackBuilder> tracks(3);

    /* Track 0: 50 frames/tick, then 25, then two same-tick tempos; last wins. */
    tracks[0].tempo(0, 500000);
    tracks[0].program(0, 0, 0);
    tracks[0].noteOn(0, 0, 60, 110);
    tracks[0].noteOff(2, 0, 60);                 /* tick 2: frame 100 */
    tracks[0].tempo(118, 250000);               /* tick 120: frame 6000 */
    tracks[0].noteOn(2, 0, 62, 110);            /* tick 122: 6000 + 50 = 6050 */
    tracks[0].noteOff(2, 0, 62);
    tracks[0].tempo(116, 500000);               /* tick 240 */
    tracks[0].tempo(0, 1000000);                /* same tick, last => 100 frames/tick */
    tracks[0].noteOn(0, 0, 64, 110);            /* frame 9000 */
    tracks[0].noteOff(1, 0, 64);                /* frame 9100 */
    tracks[0].noteOn(0, 0, 65, 110);            /* same tick/order after off */
    tracks[0].noteOff(1, 0, 65);
    tracks[0].end(48);

    /* Track 1: 25 frames/tick initially, then 100 frames/tick. */
    tracks[1].tempo(0, 250000);
    tracks[1].program(0, 0, 20);
    tracks[1].noteOn(0, 0, 72, 105);
    tracks[1].noteOff(4, 0, 72);                 /* frame 100 */
    tracks[1].tempo(116, 1000000);              /* tick 120: frame 3000 */
    tracks[1].noteOn(120, 0, 74, 105);          /* tick 240: 3000 + 12000 = 15000 */
    tracks[1].noteOff(1, 0, 74);
    tracks[1].end(24);

    /* Track 2 gives another independent integration and an earlier end. */
    tracks[2].tempo(0, 500000);
    tracks[2].program(0, 0, 40);
    tracks[2].pan(0, 0, 96);
    tracks[2].noteOn(0, 0, 79, 100);
    tracks[2].noteOff(6, 0, 79);                 /* tick 6: frame 300 */
    tracks[2].noteOn(54, 0, 81, 100);            /* tick 60: frame 3000 */
    tracks[2].noteOff(1, 0, 81);
    tracks[2].end(12);

    Bytes midi = assemble(header(2, 3, 480), tracks);

    adl_smf2::setDebugEventHook(&hookCallback, &hookEvents);
    hookEvents.clear();
    ADL_MIDIPlayer *big = layeredPlayer(midi);
    TASK_CHECK(big != 0);
    std::vector<short> bigPcm;
    TASK_CHECK(renderFrames(big, 16384, bigPcm));
    adl_smf2::setDebugEventHook(0, 0);

    TASK_CHECK(findNoteOn(0, 60) && findNoteOn(0, 60)->frame == 0);
    TASK_CHECK(findNoteOn(0, 62) && findNoteOn(0, 62)->frame == 6050);
    TASK_CHECK(findNoteOn(0, 64) && findNoteOn(0, 64)->frame == 9000);
    TASK_CHECK(findNoteOn(0, 65) && findNoteOn(0, 65)->frame == 9100);
    TASK_CHECK(findNoteOn(1, 72) && findNoteOn(1, 72)->frame == 0);
    TASK_CHECK(findNoteOn(1, 74) && findNoteOn(1, 74)->frame == 15000);
    TASK_CHECK(findNoteOn(2, 81) && findNoteOn(2, 81)->frame == 3000);
    TASK_CHECK(adl_totalTimeLength(big) > 16384.0 / 48000.0);
    adl_close(big);

    ADL_MIDIPlayer *frag = layeredPlayer(midi);
    TASK_CHECK(frag != 0);
    std::vector<short> fragPcm;
    TASK_CHECK(renderFragmented(frag, 16384, fragPcm));
    TASK_CHECK(samePcm(bigPcm, fragPcm));
    adl_close(frag);

    /* Mid-playback multiplier. Prefix ends at an exact delivered frame. */
    const int total = 12000, change = 4096;
    ADL_MIDIPlayer *block2 = layeredPlayer(midi);
    ADL_MIDIPlayer *frag2 = layeredPlayer(midi);
    TASK_CHECK(block2 && frag2);
    std::vector<short> block2Pcm, frag2Pcm;
    TASK_CHECK(renderWithTempoChange(block2, total, change, 2.0, false, block2Pcm));

    hookEvents.clear();
    adl_smf2::setDebugEventHook(&hookCallback, &hookEvents);
    TASK_CHECK(renderWithTempoChange(frag2, total, change, 2.0, true, frag2Pcm));
    adl_smf2::setDebugEventHook(0, 0);

    TASK_CHECK(samePcm(block2Pcm, frag2Pcm));
    const HookEvent *fast64 = findNoteOn(0, 64);
    TASK_CHECK(fast64 && fast64->frame == 6548);
    const HookEvent *fast65 = findNoteOn(0, 65);
    TASK_CHECK(fast65 && fast65->frame == 6598);
    const HookEvent *fast74 = findNoteOn(1, 74);
    TASK_CHECK(fast74 && fast74->frame == 9548);
    adl_close(block2);
    adl_close(frag2);
    return 0;
}

int clocks_ppqn_extremes()
{
    const unsigned ppqns[2] = {1, 9600};
    for(size_t i = 0; i < 2; ++i)
    {
        std::vector<TrackBuilder> tracks(1);
        tracks[0].tempo(0, 500000);
        tracks[0].program(0, 0, 3);
        tracks[0].noteOn(0, 0, 64, 100);
        tracks[0].noteOff(ppqns[i], 0, 64);
        tracks[0].end(0);
        Bytes midi = assemble(header(2, 1, ppqns[i]), tracks);
        ADL_MIDIPlayer *p = layeredPlayer(midi);
        TASK_CHECK(p != 0);
        std::vector<short> pcm;
        TASK_CHECK(renderFrames(p, 128, pcm));
        TASK_CHECK(task_fixture::nonzero(pcm));
        adl_close(p);
    }
    return 0;
}

Bytes ports_smf2()
{
    std::vector<TrackBuilder> t(2);
    t[0].tempo(0, 500000);
    t[0].program(0, 0, 0);
    t[0].pan(0, 0, 16);
    t[0].pedal(0, 0, 0);
    t[0].noteOn(0, 0, 60, 115);
    t[0].program(4, 0, 31);
    t[0].pan(4, 0, 28);
    t[0].pedal(4, 0, 127);
    t[0].noteOff(12, 0, 60);
    t[0].end(8);

    t[1].tempo(0, 500000);
    t[1].program(0, 0, 44);
    t[1].pan(0, 0, 112);
    t[1].pedal(0, 0, 127);
    t[1].noteOn(0, 0, 60, 115);
    t[1].program(4, 0, 12);
    t[1].pan(4, 0, 96);
    t[1].pedal(4, 0, 0);
    t[1].pitchBend(4, 0, 10000);
    t[1].noteOff(12, 0, 60);
    t[1].end(8);
    return assemble(header(2, 2, 480), t);
}

Bytes ports_reference_smf1()
{
    std::vector<TrackBuilder> t(2);
    /* Device meta must precede the channel events at time zero. */
    t[0].deviceName(0, 0);
    t[0].tempo(0, 500000);
    t[0].program(0, 0, 0);
    t[0].pan(0, 0, 16);
    t[0].pedal(0, 0, 0);
    t[0].noteOn(0, 0, 60, 115);
    t[0].program(4, 0, 31);
    t[0].pan(4, 0, 28);
    t[0].pedal(4, 0, 127);
    t[0].noteOff(12, 0, 60);
    t[0].end(8);

    t[1].deviceName(0, 1);
    t[1].tempo(0, 500000);
    t[1].program(0, 0, 44);
    t[1].pan(0, 0, 112);
    t[1].pedal(0, 0, 127);
    t[1].noteOn(0, 0, 60, 115);
    t[1].program(4, 0, 12);
    t[1].pan(4, 0, 96);
    t[1].pedal(4, 0, 0);
    t[1].pitchBend(4, 0, 10000);
    t[1].noteOff(12, 0, 60);
    t[1].end(8);
    return assemble(header(1, 2, 480), t);
}

int ports_named_independent_channels()
{
    Bytes smf2 = ports_smf2();
    Bytes ref1 = ports_reference_smf1();

    ADL_MIDIPlayer *a = layeredPlayer(smf2);
    ADL_MIDIPlayer *b = task_fixture::player();
    TASK_CHECK(a && b);
    TASK_CHECK(adl_openData(b, ref1.data(), (unsigned long)ref1.size()) == 0);

    std::vector<short> pa, pb;
    TASK_CHECK(renderFrames(a, 3072, pa));
    TASK_CHECK(renderFrames(b, 3072, pb));
    TASK_CHECK(task_fixture::nonzero(pa));
    TASK_CHECK(task_fixture::nonzero(pb));

    if(!samePcm(pa, pb))
    {
        size_t first = 0;
        while(first < pa.size() && first < pb.size() && pa[first] == pb[first]) ++first;
        std::fprintf(stderr, "ports PCM mismatch at sample %zu\n", first);
        TASK_CHECK(0);
    }

    /* Fragmentation must not change the layered result either. */
    ADL_MIDIPlayer *c = layeredPlayer(smf2);
    std::vector<short> pc;
    TASK_CHECK(c && renderFragmented(c, 3072, pc));
    TASK_CHECK(samePcm(pa, pc));

    adl_close(a);
    adl_close(b);
    adl_close(c);
    return 0;
}

Bytes transport_song()
{
    std::vector<TrackBuilder> t(2);
    t[0].tempo(0, 500000);
    t[0].program(0, 0, 7);
    t[0].pan(0, 0, 32);
    t[0].pedal(0, 0, 127);
    t[0].noteOn(0, 0, 67, 110);
    t[0].tempo(240, 250000);              /* frame 12000 */
    t[0].noteOff(240, 0, 67);             /* tick480: 12000 + 6000 = 18000 */
    t[0].noteOn(0, 0, 72, 110);
    t[0].noteOff(240, 0, 72);
    t[0].end(24);

    t[1].tempo(0, 1000000);
    t[1].program(0, 0, 50);
    t[1].pan(0, 0, 100);
    t[1].noteOn(0, 0, 60, 105);
    t[1].tempo(120, 500000);              /* tick120 at 100f/t = 12000 */
    t[1].noteOff(120, 0, 60);             /* tick240: 12000 + 6000 = 18000 */
    t[1].noteOn(0, 0, 76, 105);
    t[1].noteOff(240, 0, 76);
    t[1].end(24);
    return assemble(header(2, 2, 480), t);
}

int transport_config_legacy_reject()
{
    TASK_CHECK(adl_setSMF2Layered(0, 1) == -1);

    ADL_MIDIPlayer *d = task_fixture::player();
    TASK_CHECK(d != 0);
    TASK_CHECK(adl_setSMF2Layered(d, 2) == -1);
    TASK_CHECK(adl_setSMF2Layered(d, -1) == -1);
    TASK_CHECK(adl_setSMF2Layered(d, 1) == 0);

    Bytes normal = task_fixture::smf(1);
    TASK_CHECK(adl_openData(d, normal.data(), (unsigned long)normal.size()) == 0);
    TASK_CHECK(adl_setSMF2Layered(d, 0) == -1);
    TASK_CHECK(adl_setSMF2Layered(d, 1) == -1);
    std::vector<short> pcm;
    TASK_CHECK(task_fixture::render(d, 2048, pcm) && task_fixture::nonzero(pcm));
    adl_close(d);

    /* Default-off path remains available and successful loads lock configuration. */
    ADL_MIDIPlayer *legacy = task_fixture::player();
    TASK_CHECK(legacy != 0);
    TASK_CHECK(adl_setSMF2Layered(legacy, 0) == 0);
    Bytes smf2 = task_fixture::smf(2);
    TASK_CHECK(adl_openData(legacy, smf2.data(), (unsigned long)smf2.size()) == 0);
    TASK_CHECK(adl_setSMF2Layered(legacy, 1) == -1);
    TASK_CHECK(task_fixture::render(legacy, 4096, pcm) && task_fixture::nonzero(pcm));
    adl_close(legacy);
    return 0;
}

int transport_seek_rewind_independent_instances()
{
    Bytes midi = transport_song();

    ADL_MIDIPlayer *d = layeredPlayer(midi);
    TASK_CHECK(d != 0);
    double length = adl_totalTimeLength(d);
    TASK_CHECK(length > 1.0);

    std::vector<short> first;
    TASK_CHECK(renderFragmented(d, 917, first)); /* 1/7/113 fragmentation prefix */
    TASK_CHECK(adl_positionTell(d) > 0.015);

    adl_positionSeek(d, 0.30); /* after both tempo maps' 12000-frame point */
    TASK_CHECK(std::fabs(adl_positionTell(d) - 0.30) < 1.0e-9);
    std::vector<short> afterSeek;
    TASK_CHECK(renderFragmented(d, 5000, afterSeek));
    TASK_CHECK(task_fixture::nonzero(afterSeek));

    /*
     * Seek follows the old panic rule, so a second instance must have the same
     * pre-seek pull/aging history before its continuation is compared.
     */
    ADL_MIDIPlayer *ref = layeredPlayer(midi);
    std::vector<short> refBefore;
    TASK_CHECK(ref != 0 && renderFragmented(ref, 917, refBefore));
    adl_positionSeek(ref, 0.30);
    std::vector<short> refSeek;
    TASK_CHECK(renderFragmented(ref, 5000, refSeek));
    TASK_CHECK(samePcm(afterSeek, refSeek));
    adl_close(ref);

    /* Rewind must discard position and stale pull rounding. */
    adl_positionRewind(d);
    TASK_CHECK(adl_positionTell(d) == 0.0);
    std::vector<short> rewound;
    TASK_CHECK(renderFragmented(d, 1337, rewound));

    ADL_MIDIPlayer *rewindTwin = layeredPlayer(midi);
    std::vector<short> twinBefore, twinSeek;
    TASK_CHECK(rewindTwin && renderFragmented(rewindTwin, 917, twinBefore));
    adl_positionSeek(rewindTwin, 0.30);
    TASK_CHECK(renderFragmented(rewindTwin, 5000, twinSeek));
    adl_positionRewind(rewindTwin);
    std::vector<short> freshStart;
    TASK_CHECK(renderFragmented(rewindTwin, 1337, freshStart));
    if(!samePcm(rewound, freshStart))
    {
        size_t k = 0;
        while(k < rewound.size() && k < freshStart.size() && rewound[k] == freshStart[k]) ++k;
        std::fprintf(stderr, "rewind twin mismatch sample=%zu rew=%d twin=%d\n",
                     k, k < rewound.size() ? rewound[k] : 0,
                     k < freshStart.size() ? freshStart[k] : 0);
    }
    TASK_CHECK(samePcm(rewound, freshStart));
    adl_close(rewindTwin);

    adl_positionSeek(d, adl_totalTimeLength(d) + 10.0);
    TASK_CHECK(adl_positionTell(d) == 0.0);
    std::vector<short> beyond;
    TASK_CHECK(renderFrames(d, 128, beyond) && task_fixture::nonzero(beyond));
    adl_close(d);

    /* Interleave two instances and prove their streams stay independent. */
    ADL_MIDIPlayer *a = layeredPlayer(transport_song());
    ADL_MIDIPlayer *b = layeredPlayer(ports_smf2());
    TASK_CHECK(a && b);
    std::vector<short> ai, bi, asolo, bsolo;
    for(int i = 0; i < 20; ++i)
    {
        TASK_CHECK(appendRender(a, 37, ai));
        TASK_CHECK(appendRender(b, 53, bi));
    }
    ADL_MIDIPlayer *a2 = layeredPlayer(transport_song());
    ADL_MIDIPlayer *b2 = layeredPlayer(ports_smf2());
    TASK_CHECK(a2 && b2);
    TASK_CHECK(renderFrames(a2, 740, asolo));
    TASK_CHECK(renderFrames(b2, 1060, bsolo));
    TASK_CHECK(samePcm(ai, asolo));
    TASK_CHECK(samePcm(bi, bsolo));
    adl_close(a); adl_close(b); adl_close(a2); adl_close(b2);

    /* Reload on one configured instance replaces the clock and ports. */
    ADL_MIDIPlayer *reload = task_fixture::player();
    TASK_CHECK(reload && adl_setSMF2Layered(reload, 1) == 0);
    TASK_CHECK(adl_openData(reload, midi.data(), (unsigned long)midi.size()) == 0);
    TASK_CHECK(renderFrames(reload, 512, asolo));
    TASK_CHECK(adl_openData(reload, ports_smf2().data(),
                            (unsigned long)ports_smf2().size()) == 0);
    TASK_CHECK(adl_totalTimeLength(reload) > 1.0);
    TASK_CHECK(renderFrames(reload, 512, bsolo) && task_fixture::nonzero(bsolo));
    adl_reset(reload);
    TASK_CHECK(renderFrames(reload, 512, bsolo) && task_fixture::nonzero(bsolo));
    TASK_CHECK(adl_setSMF2Layered(reload, 0) == -1);
    adl_close(reload);
    return 0;
}

int transport_equivalent_smf1_after_seek()
{
    /*
     * The independent SMF2 tempo maps place both tracks' next changes at
     * 18000 frames.  Encode that already-integrated time as ordinary SMF1
     * ticks under a fixed 500000 us/quarter map: 50 output frames/tick.
     */
    std::vector<TrackBuilder> t(2);
    t[0].deviceName(0, 0);
    t[0].tempo(0, 500000);
    t[0].program(0, 0, 7);
    t[0].pan(0, 0, 32);
    t[0].pedal(0, 0, 127);
    t[0].noteOn(0, 0, 67, 110);
    t[0].noteOff(360, 0, 67);   /* tick 360 = frame 18000 */
    t[0].noteOn(0, 0, 72, 110);
    t[0].noteOff(120, 0, 72);
    t[0].end(12);

    t[1].deviceName(0, 1);
    t[1].tempo(0, 500000);
    t[1].program(0, 0, 50);
    t[1].pan(0, 0, 100);
    t[1].noteOn(0, 0, 60, 105);
    t[1].noteOff(360, 0, 60);
    t[1].noteOn(0, 0, 76, 105);
    t[1].noteOff(240, 0, 76);
    t[1].end(24);
    Bytes equivalent = assemble(header(1, 2, 480), t);

    ADL_MIDIPlayer *layered = layeredPlayer(transport_song());
    ADL_MIDIPlayer *reference = task_fixture::player();
    TASK_CHECK(layered && reference);
    TASK_CHECK(adl_openData(reference, equivalent.data(),
                            (unsigned long)equivalent.size()) == 0);

    adl_positionSeek(layered, 0.30);
    adl_positionSeek(reference, 0.30);

    hookEvents.clear();
    adl_smf2::setDebugEventHook(&hookCallback, &hookEvents);
    std::vector<short> layeredPcm, referencePcm;
    TASK_CHECK(renderFrames(layered, 6000, layeredPcm));
    adl_smf2::setDebugEventHook(0, 0);
    TASK_CHECK(renderFrames(reference, 6000, referencePcm));
    TASK_CHECK(findNoteOn(0, 72) && findNoteOn(0, 72)->frame == 3600);
    TASK_CHECK(findNoteOn(1, 76) && findNoteOn(1, 76)->frame == 3600);
    TASK_CHECK(task_fixture::nonzero(layeredPcm));
    TASK_CHECK(task_fixture::nonzero(referencePcm));
    if(!samePcm(layeredPcm, referencePcm))
    {
        size_t k = 0;
        while(k < layeredPcm.size() && k < referencePcm.size() &&
              layeredPcm[k] == referencePcm[k]) ++k;
        std::fprintf(stderr, "seek SMF1 PCM mismatch at sample %zu\n", k);
        TASK_CHECK(0);
    }

    adl_close(layered);
    adl_close(reference);
    return 0;
}

int transport_rejected_subsets()
{
    /* SMPTE division. */
    Bytes smpte = header(2, 1, 0x8000);
    TrackBuilder t;
    t.noteOn(0, 0, 60);
    t.noteOff(10, 0, 60);
    t.end(0);
    Bytes tr = t.mtrk();
    smpte.insert(smpte.end(), tr.begin(), tr.end());

    ADL_MIDIPlayer *d = task_fixture::player();
    TASK_CHECK(d && adl_setSMF2Layered(d, 1) == 0);
    TASK_CHECK(adl_openData(d, smpte.data(), (unsigned long)smpte.size()) == -1);
    adl_close(d);

    /* Device/port meta and loop markers reject while mode stays enabled. */
    const unsigned char badMetas[][2] = {{0x09, 0}, {0x21, 0}, {0x06, 1}};
    for(unsigned i = 0; i < sizeof(badMetas) / sizeof(badMetas[0]); ++i)
    {
        ADL_MIDIPlayer *p = task_fixture::player();
        TASK_CHECK(p && adl_setSMF2Layered(p, 1) == 0);
        std::vector<TrackBuilder> tracks(1);
        if(badMetas[i][0] == 0x09)
            tracks[0].deviceName(0, 0);
        else if(badMetas[i][0] == 0x21)
        {
            Bytes v; v.push_back(3);
            tracks[0].meta(0, 0x21, v);
        }
        else
            tracks[0].textMeta(0, 0x06, "loopstart");
        tracks[0].noteOn(0, 0, 60);
        tracks[0].noteOff(10, 0, 60);
        tracks[0].end(0);
        Bytes midi = assemble(header(2, 1, 480), tracks);
        TASK_CHECK(adl_openData(p, midi.data(), (unsigned long)midi.size()) == -1);
        TASK_CHECK(adl_setSMF2Layered(p, 1) == 0);
        adl_close(p);
    }
    return 0;
}

} // namespace

const struct task_case task_cases[] =
{
    {"clocks", "independent_tempo_maps_and_fragmentation", clocks_integral_and_fragments},
    {"clocks", "ppqn_range_extremes", clocks_ppqn_extremes},
    {"ports", "named_ports_match_equivalent_smf1", ports_named_independent_channels},
    {"transport", "configuration_and_legacy_path", transport_config_legacy_reject},
    {"transport", "seek_rewind_interleave_reload", transport_seek_rewind_independent_instances},
    {"transport", "seek_matches_equivalent_smf1", transport_equivalent_smf1_after_seek},
    {"transport", "reject_unsupported_smf2_subset", transport_rejected_subsets}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
