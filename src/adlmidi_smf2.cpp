/*
 * Layered SMF Format 2 scheduler for libADLMIDI.
 *
 * Each track owns an independent tick/tempo conversion.  All parsed events are
 * placed on one absolute seconds timeline and submitted to the single existing
 * MIDIplay/Opl synth in time, track, and in-track order.
 */

#include "adlmidi_midiplay.hpp"
#include "adlmidi_opl3.hpp"
#include "adlmidi_smf2.hpp"
#include "chips/opl_chip_base.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace
{

static const double POST_SONG_WAIT = 1.0;

struct ByteReader
{
    const unsigned char *data;
    size_t size;
    size_t pos;
    bool ok;

    ByteReader(const void *pData, unsigned long pSize)
        : data(static_cast<const unsigned char *>(pData)),
          size(static_cast<size_t>(pSize)), pos(0), ok(true)
    {}

    unsigned char peek()
    {
        if(pos >= size) { ok = false; return 0; }
        return data[pos];
    }

    unsigned char u8()
    {
        if(pos >= size) { ok = false; return 0; }
        return data[pos++];
    }

    bool be16(unsigned &value)
    {
        if(pos + 2 > size) { ok = false; value = 0; return false; }
        value = (unsigned(data[pos]) << 8) | data[pos + 1];
        pos += 2;
        return true;
    }

    bool be32(unsigned long &value)
    {
        if(pos + 4 > size) { ok = false; value = 0; return false; }
        value = ((unsigned long)data[pos] << 24) |
                ((unsigned long)data[pos + 1] << 16) |
                ((unsigned long)data[pos + 2] << 8) |
                (unsigned long)data[pos + 3];
        pos += 4;
        return true;
    }

    bool vlq(unsigned long &value)
    {
        value = 0;
        unsigned count = 0;
        for(;;)
        {
            if(pos >= size || ++count > 4) { ok = false; return false; }
            unsigned char b = data[pos++];
            value = (value << 7) | (b & 0x7fU);
            if(!(b & 0x80U))
                return true;
        }
    }

    bool bytes(size_t count, std::vector<unsigned char> &out)
    {
        if(pos + count > size) { ok = false; return false; }
        out.assign(data + pos, data + pos + count);
        pos += count;
        return true;
    }

    void skip(size_t count)
    {
        if(pos + count > size) { ok = false; pos = size; return; }
        pos += count;
    }
};

struct Event
{
    unsigned long long atNum; /* time in units of 1 / (ppqn * 1000000) seconds */
    int track;
    int order;
    unsigned char status;
    unsigned char d1;
    unsigned char d2;
    std::vector<unsigned char> raw;
};

struct Song
{
    unsigned ppqn;
    unsigned trackCount;
    unsigned long long denominator;
    unsigned long long endNum;
    std::vector<Event> events;
};

static void fireDebugHook(ADL_MIDIPlayer *device, const Event &e, long long frame);

struct Engine
{
    ADL_MIDIPlayer *device;
    MIDIplay *play;
    Song song;
    size_t nextIndex;
    long long framePos;
    long double musicPos;
    double multiplier;
    long long dueFrame;
    bool haveDue;
    bool finished;
    bool beginPending;
    bool rationalClock;
    size_t portOffset[8];

    void registerPorts()
    {
        for(size_t i = 0; i < 8; ++i)
            portOffset[i] = i * 16;

        /* Create the same named multi-port channels used by an SMF1 reference. */
        for(unsigned i = 0; i < song.trackCount && i < 8; ++i)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "SMF2/%u", i);
            play->realTime_deviceSwitch(i, name, std::strlen(name));
            portOffset[i] = play->realTime_currentDevice(i);
        }
    }

    Engine(ADL_MIDIPlayer *d, MIDIplay *p, const Song &s)
        : device(d), play(p), song(s), nextIndex(0), framePos(0), musicPos(0.0L),
          multiplier(1.0), dueFrame(0), haveDue(false), finished(false),
          beginPending(true), rationalClock(true)
    {
        registerPorts();
    }

    double totalLength() const
    {
        return double((long double)song.endNum / (long double)song.denominator) + POST_SONG_WAIT;
    }

    double tell() const
    {
        return static_cast<double>(musicPos);
    }

    void invalidateDue()
    {
        haveDue = false;
    }

    void updateDue()
    {
        if(haveDue || nextIndex >= song.events.size())
            return;

        const Event &e = song.events[nextIndex];
        long long offset;

        /*
         * At normal multiplier one, use the exact rational frame number.
         * Tempo multipliers changed at run time are floating-point by API
         * contract, so use long double for their segment mapping.
         */
        if(rationalClock && multiplier == 1.0)
        {
            unsigned long long rate = play->m_setup.PCM_RATE;
            unsigned long long numerator = e.atNum * rate;
            unsigned long long frame = numerator / song.denominator;
            if(numerator % song.denominator != 0)
                ++frame;
            long long absolute = static_cast<long long>(frame);
            offset = absolute - framePos;
            if(offset < 0)
                offset = 0;
        }
        else
        {
            long double eventTime = (long double)e.atNum / (long double)song.denominator;
            long double deltaMusic = eventTime - musicPos;
            if(deltaMusic <= 1.0e-15L)
            {
                dueFrame = framePos;
                haveDue = true;
                return;
            }

            long double rate = (long double)play->m_setup.PCM_RATE;
            long double frames = deltaMusic * rate / (long double)multiplier;
            offset = (long long)std::ceil(frames - 1.0e-10L);
            if(offset < 0)
                offset = 0;
        }

        dueFrame = framePos + offset;
        haveDue = true;
    }

    void dispatch(const Event &e, bool seeking)
    {
        if(e.status == 0xF0 || e.status == 0xF7)
        {
            if(!e.raw.empty())
                play->realTime_SysEx(e.raw.data(), e.raw.size());
            return;
        }

        const size_t track = static_cast<size_t>(e.track);
        const unsigned ch = static_cast<unsigned>(portOffset[track]) +
                            static_cast<unsigned>(e.status & 0x0f);
        const unsigned char kind = e.status & 0xf0;

        switch(kind)
        {
        case 0x80:
            play->realTime_NoteOff(static_cast<uint8_t>(ch), e.d1);
            break;
        case 0x90:
            /* Velocity-zero note-on is parsed by the legacy sequencer as note-off. */
            if(e.d2 == 0)
                play->realTime_NoteOff(static_cast<uint8_t>(ch), e.d1);
            /* Seek deliberately ignores historical sounding note-ons. */
            else if(!seeking)
                play->realTime_NoteOn(static_cast<uint8_t>(ch), e.d1, e.d2);
            break;
        case 0xA0:
            play->realTime_NoteAfterTouch(static_cast<uint8_t>(ch), e.d1, e.d2);
            break;
        case 0xB0:
            play->realTime_Controller(static_cast<uint8_t>(ch), e.d1, e.d2);
            break;
        case 0xC0:
            play->realTime_PatchChange(static_cast<uint8_t>(ch), e.d1);
            break;
        case 0xD0:
            play->realTime_ChannelAfterTouch(static_cast<uint8_t>(ch), e.d1);
            break;
        case 0xE0:
            play->realTime_PitchBend(static_cast<uint8_t>(ch),
                                     static_cast<uint16_t>(e.d1 | (unsigned(e.d2) << 7)));
            break;
        default:
            break;
        }
    }

    void processDue()
    {
        if(beginPending)
        {
            play->realTime_ResetState();
            beginPending = false;
        }

        for(;;)
        {
            updateDue();
            if(!haveDue || dueFrame > framePos)
                break;
            haveDue = false;
            fireDebugHook(device, song.events[nextIndex], framePos);
            dispatch(song.events[nextIndex], false);
            ++nextIndex;
        }
    }

    void seek(double seconds)
    {
        if(seconds < 0.0)
            seconds = 0.0;

        /* The legacy seek entry treats positions beyond song length as rewind. */
        if(seconds > totalLength() + 1.0e-9)
        {
            seek(0.0);
            return;
        }

        framePos = 0;
        musicPos = seconds > 0.0 ? static_cast<long double>(seconds) : 0.0L;
        rationalClock = (seconds <= 1.0e-12L);
        nextIndex = 0;
        finished = false;
        haveDue = false;
        beginPending = false;

        /* The legacy seek path rewinds and processes its song-begin hook. */
        play->realTime_ResetState();

        if(seconds > 1.0e-9)
        {
            const long double eps = 1.0e-9L;
            while(nextIndex < song.events.size())
            {
                const Event &e = song.events[nextIndex];
                long double t = (long double)e.atNum / (long double)song.denominator;
                if(t > static_cast<long double>(seconds) + eps)
                    break;
                dispatch(e, true);
                ++nextIndex;
            }
        }
    }

    void rewind()
    {
        seek(0.0);
    }

    int render(int sampleCount, short *out)
    {
        if(sampleCount < 0)
            return 0;
        sampleCount -= sampleCount % 2;
        if(sampleCount <= 0 || !out)
            return 0;

        const int wantedFrames = sampleCount / 2;
        int doneFrames = 0;
        const long double rate = (long double)play->m_setup.PCM_RATE;
        const double length = totalLength();

        while(doneFrames < wantedFrames)
        {
            processDue();

            if(musicPos + 1.0e-12L >= static_cast<long double>(length))
            {
                finished = true;
                break;
            }

            long long step = std::min<long long>(512, wantedFrames - doneFrames);

            updateDue();
            if(haveDue)
            {
                long long toEvent = dueFrame - framePos;
                if(toEvent <= 0)
                    continue; /* processDue should normally have handled this. */
                step = std::min(step, toEvent);
            }

            long double remaining = static_cast<long double>(length) - musicPos;
            long long toEnd = (long long)std::ceil(remaining * rate / (long double)multiplier - 1.0e-10L);
            if(toEnd > 0)
                step = std::min(step, toEnd);
            if(step <= 0)
                break;

            int frames = static_cast<int>(step);
            int32_t *buf = play->m_outBuf;
            std::memset(buf, 0, static_cast<size_t>(frames) * 2 * sizeof(int32_t));

            Synth &synth = *play->m_synth;
            unsigned int chips = synth.m_numChips;
            if(chips == 1)
                synth.m_chips[0]->generate32(buf, static_cast<size_t>(frames));
            else
            {
                for(unsigned int card = 0; card < chips; ++card)
                    synth.m_chips[card]->generateAndMix32(buf, static_cast<size_t>(frames));
            }

            for(int i = 0; i < frames * 2; ++i)
                out[doneFrames * 2 + i] = static_cast<short>(adl_cvtS16(buf[i]));

            /* The old pull loop ages voices by the represented real time. */
            play->TickIterators(static_cast<double>((long double)frames / rate));

            doneFrames += frames;
            framePos += frames;
            musicPos += (long double)frames * (long double)multiplier / rate;
        }

        return doneFrames * 2;
    }
};

struct State
{
    int enabled;
    bool loaded;
    double tempoMultiplier;
    Engine *engine;

    State() : enabled(0), loaded(false), tempoMultiplier(1.0), engine(0) {}
    ~State() { delete engine; }

private:
    State(const State &);
    State &operator=(const State &);
};

static std::map<ADL_MIDIPlayer *, State> &states()
{
    static std::map<ADL_MIDIPlayer *, State> s;
    return s;
}

static adl_smf2::DebugEventHook g_debugHook = 0;
static void *g_debugUserData = 0;

static void fireDebugHook(ADL_MIDIPlayer *device, const Event &e, long long frame)
{
    if(g_debugHook)
        g_debugHook(device, e.track, e.order, e.status, e.d1, e.d2, frame, g_debugUserData);
}

static State *findState(ADL_MIDIPlayer *device)
{
    std::map<ADL_MIDIPlayer *, State>::iterator it = states().find(device);
    return it == states().end() ? 0 : &it->second;
}

static MIDIplay *midiPlayer(ADL_MIDIPlayer *device)
{
    return static_cast<MIDIplay *>(device->adl_midiPlayer);
}

static bool isLoopMarkerText(const std::vector<unsigned char> &data)
{
    std::string s;
    for(size_t i = 0; i < data.size(); ++i)
    {
        char c = static_cast<char>(data[i]);
        if(c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if(c >= 32 && c < 127)
            s.push_back(c);
    }
    while(!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\0'))
        s.erase(s.size() - 1);

    return s.compare(0, 9, "loopstart") == 0 ||
           s.compare(0, 7, "loopend") == 0;
}

struct EventLess
{
    bool operator()(const Event &a, const Event &b) const
    {
        if(a.atNum != b.atNum) return a.atNum < b.atNum;
        if(a.track != b.track) return a.track < b.track;
        return a.order < b.order;
    }
};

/*
 * Returns 0 for a non-layered/non-SMF2 input, 1 for a valid layered song, and
 * -1 for an SMF2 input that violates the supported subset.
 */
static int parseSMF2(const void *pData, unsigned long size, Song &song, std::string &error)
{
    ByteReader whole(pData, size);
    unsigned long headerLen = 0;
    unsigned format = 0, trackCount = 0, division = 0;

    if(size < 14 || std::memcmp(whole.data, "MThd", 4) != 0)
        return 0;
    whole.pos = 4;
    if(!whole.be32(headerLen) || headerLen < 6 || whole.pos + headerLen > whole.size)
        return -1;
    if(!whole.be16(format) || !whole.be16(trackCount) || !whole.be16(division))
        return -1;
    if(format != 2)
        return 0;
    if(trackCount < 1 || trackCount > 8)
    {
        error = "Layered SMF2 supports 1..8 tracks";
        return -1;
    }
    if(division & 0x8000U)
    {
        error = "SMPTE divisions are not supported in layered SMF2";
        return -1;
    }

    song.ppqn = division;
    song.trackCount = trackCount;
    if(song.ppqn < 1 || song.ppqn > 9600)
    {
        error = "Layered SMF2 supports PPQN 1..9600";
        return -1;
    }
    song.denominator = static_cast<unsigned long long>(song.ppqn) * 1000000ULL;

    whole.pos = 8 + headerLen;
    unsigned parsedTracks = 0;
    unsigned long long maxEnd = 0;

    while(whole.pos < whole.size)
    {
        if(whole.pos + 8 > whole.size)
        {
            error = "Truncated SMF2 chunk header";
            return -1;
        }

        unsigned char tag[4];
        for(int i = 0; i < 4; ++i) tag[i] = whole.u8();
        unsigned long chunkLen = 0;
        if(!whole.be32(chunkLen) || whole.pos + chunkLen > whole.size)
        {
            error = "Invalid SMF2 chunk length";
            return -1;
        }

        if(std::memcmp(tag, "MTrk", 4) != 0)
        {
            whole.skip(chunkLen);
            continue;
        }

        if(parsedTracks >= trackCount)
        {
            error = "Too many SMF2 tracks";
            return -1;
        }

        const size_t trackIndex = parsedTracks++;
        ByteReader tr(whole.data + whole.pos, chunkLen);
        whole.pos += chunkLen;

        unsigned long delta = 0;
        if(!tr.vlq(delta))
        {
            error = "Bad SMF2 track delta";
            return -1;
        }

        unsigned long long absTick = 0;
        unsigned long long lastTempoTick = 0;
        unsigned long long timeNum = 0;
        unsigned long tempo = 500000;
        unsigned char running = 0;
        int order = 0;
        bool gotEnd = false;

        while(!gotEnd)
        {
            absTick += delta;
            unsigned long long tickDelta = absTick - lastTempoTick;
            unsigned long long add = tickDelta * static_cast<unsigned long long>(tempo);
            if(tickDelta != 0 && add / tickDelta != static_cast<unsigned long long>(tempo))
            {
                error = "SMF2 timing overflow";
                return -1;
            }
            if(std::numeric_limits<unsigned long long>::max() - timeNum < add)
            {
                error = "SMF2 timing overflow";
                return -1;
            }
            timeNum += add;
            lastTempoTick = absTick;

            unsigned char first = tr.u8();
            if(!tr.ok)
            {
                error = "Truncated SMF2 event";
                return -1;
            }

            unsigned char status;
            if(first < 0x80)
            {
                if(!running)
                {
                    error = "SMF2 running status without status byte";
                    return -1;
                }
                status = running;
            }
            else
            {
                status = first;
                if(status < 0xF0)
                    running = status;
                else
                    running = 0;
            }

            Event e;
            e.atNum = timeNum;
            e.track = static_cast<int>(trackIndex);
            e.order = order++;
            e.status = status;
            e.d1 = 0;
            e.d2 = 0;

            if(status == 0xFF)
            {
                unsigned char metaType = tr.u8();
                unsigned long metaLen = 0;
                if(!tr.vlq(metaLen))
                {
                    error = "Bad SMF2 meta length";
                    return -1;
                }
                std::vector<unsigned char> meta;
                if(!tr.bytes(metaLen, meta))
                {
                    error = "Truncated SMF2 meta event";
                    return -1;
                }

                if(metaType == 0x2F)
                {
                    gotEnd = true;
                    if(timeNum > maxEnd)
                        maxEnd = timeNum;
                    break;
                }

                if(metaType == 0x09 || metaType == 0x21)
                {
                    error = "Device/port switching meta events are not allowed in layered SMF2";
                    return -1;
                }

                if(metaType == 0x51)
                {
                    if(meta.size() != 3)
                    {
                        error = "Invalid SMF2 tempo event";
                        return -1;
                    }
                    tempo = ((unsigned long)meta[0] << 16) |
                            ((unsigned long)meta[1] << 8) |
                            (unsigned long)meta[2];
                    if(tempo == 0)
                    {
                        error = "Invalid zero SMF2 tempo";
                        return -1;
                    }
                }
                else if(metaType == 0x06 && isLoopMarkerText(meta))
                {
                    error = "Loop markers are not allowed in layered SMF2";
                    return -1;
                }
                /* Other meta events are harmless and do not affect OPL playback. */
            }
            else if(status == 0xF0 || status == 0xF7)
            {
                unsigned long sysexLen = 0;
                if(!tr.vlq(sysexLen))
                {
                    error = "Bad SMF2 SysEx length";
                    return -1;
                }
                std::vector<unsigned char> payload;
                if(!tr.bytes(sysexLen, payload))
                {
                    error = "Truncated SMF2 SysEx";
                    return -1;
                }

                if(status == 0xF0)
                {
                    e.raw.reserve(payload.size() + 1);
                    e.raw.push_back(0xF0);
                    e.raw.insert(e.raw.end(), payload.begin(), payload.end());
                    if(!e.raw.empty() && e.raw.back() != 0xF7)
                        e.raw.push_back(0xF7);
                    song.events.push_back(e);
                }
                else if(!payload.empty())
                {
                    e.raw.reserve(payload.size() + 1);
                    e.raw.push_back(0xF0);
                    e.raw.insert(e.raw.end(), payload.begin(), payload.end());
                    if(e.raw.back() != 0xF7)
                        e.raw.push_back(0xF7);
                    song.events.push_back(e);
                }
            }
            else if(status >= 0x80 && status < 0xF0)
            {
                const unsigned char kind = status & 0xF0;
                if(first < 0x80)
                    e.d1 = first;
                else
                    e.d1 = tr.u8();

                if(kind != 0xC0 && kind != 0xD0)
                    e.d2 = tr.u8();

                if(!tr.ok || e.d1 >= 0x80 || e.d2 >= 0x80)
                {
                    error = "Invalid SMF2 channel event data";
                    return -1;
                }

                /* Reject the two RPG Maker loop controller markers explicitly. */
                if(kind == 0xB0 && (e.d1 == 110 || e.d1 == 111))
                {
                    error = "Loop controller markers are not allowed in layered SMF2";
                    return -1;
                }

                song.events.push_back(e);
            }
            else if(status >= 0xF1 && status <= 0xFE)
            {
                /* System common/realtime events are accepted but do not make sound. */
                unsigned dataCount = 0;
                if(status == 0xF1 || status == 0xF3)
                    dataCount = 1;
                else if(status == 0xF2)
                    dataCount = 2;
                for(unsigned k = 0; k < dataCount; ++k)
                    tr.u8();
                if(!tr.ok)
                {
                    error = "Truncated SMF2 system common event";
                    return -1;
                }
            }
            else
            {
                error = "Unsupported SMF2 system event";
                return -1;
            }

            if(gotEnd)
                break;
            if(!tr.vlq(delta))
            {
                error = "Missing SMF2 next delta";
                return -1;
            }
        }

        if(!gotEnd || !tr.ok)
        {
            error = "SMF2 track has no valid end event";
            return -1;
        }
    }

    if(parsedTracks != trackCount || parsedTracks < 1)
    {
        error = "SMF2 track count does not match MThd";
        return -1;
    }

    song.endNum = maxEnd;
    std::stable_sort(song.events.begin(), song.events.end(), EventLess());

    return 1;
}

} // anonymous namespace

extern "C" ADLMIDI_EXPORT int adl_setSMF2Layered(ADL_MIDIPlayer *device, int enabled)
{
    if(!device || !device->adl_midiPlayer)
        return -1;
    if(enabled != 0 && enabled != 1)
        return -1;

    State *st = findState(device);
    if(st && st->loaded)
        return -1;

    if(!st)
        st = &states()[device];
    if(st->loaded)
        return -1;

    st->enabled = enabled;
    return 0;
}

namespace adl_smf2
{

int setLayered(ADL_MIDIPlayer *device, int enabled)
{
    return adl_setSMF2Layered(device, enabled);
}

void forget(ADL_MIDIPlayer *device)
{
    if(!device)
        return;
    std::map<ADL_MIDIPlayer *, State>::iterator it = states().find(device);
    if(it != states().end())
        states().erase(it);
}

void noteLoadSuccess(ADL_MIDIPlayer *device)
{
    if(!device)
        return;
    State *st = findState(device);
    if(!st)
        st = &states()[device];
    st->loaded = true;
}

int openData(ADL_MIDIPlayer *device, const void *data, unsigned long size)
{
    if(!device || !device->adl_midiPlayer || !data)
        return -1;

    State *st = findState(device);
    if(!st || !st->enabled)
        return 0;

    Song song;
    std::string error;
    int recognized = parseSMF2(data, size, song, error);
    if(recognized == 0)
        return 0;
    if(recognized < 0)
    {
        MIDIplay *play = midiPlayer(device);
        if(!error.empty())
            play->setErrorString(error);
        return -1;
    }

    MIDIplay *play = midiPlayer(device);

    /*
     * Reuse the production loader for all synth/bank initialization.  The
     * layered scheduler below never asks its global tempo map for timing.
     */
    if(!play->LoadMIDI(data, static_cast<size_t>(size)))
        return -1;

    Engine *engine = new(std::nothrow) Engine(device, play, song);
    if(!engine)
        return -1;

    engine->multiplier = st->tempoMultiplier;
    engine->rationalClock = (engine->multiplier == 1.0);

    delete st->engine;
    st->engine = engine;
    st->loaded = true;
    play->m_setup.delay = 0.0;
    play->m_setup.carry = 0.0;
    play->m_setup.tick_skip_samples_delay = 0;
    return 1;
}

int openFile(ADL_MIDIPlayer *device, const char *path)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->enabled)
        return 0;

    std::ifstream f(path, std::ios::binary);
    if(!f)
        return -1;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if(bytes.empty())
        return -1;
    return openData(device, bytes.data(), static_cast<unsigned long>(bytes.size()));
}

int playS16(ADL_MIDIPlayer *device, int sampleCount, short *out)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return -1;
    return st->engine->render(sampleCount, out);
}

double totalTimeLength(ADL_MIDIPlayer *device)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return -1.0;
    return st->engine->totalLength();
}

double positionTell(ADL_MIDIPlayer *device)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return -1.0;
    return st->engine->tell();
}

void positionSeek(ADL_MIDIPlayer *device, double seconds)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return;
    st->engine->seek(seconds);
}

void positionRewind(ADL_MIDIPlayer *device)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return;
    st->engine->rewind();
}

void handleReset(ADL_MIDIPlayer *device)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return;
    /* adl_reset() has just cleared the MIDI port/channel maps. */
    st->engine->registerPorts();
    st->engine->seek(0.0);
}

void setTempo(ADL_MIDIPlayer *device, double multiplier)
{
    if(!device)
        return;
    State *st = findState(device);
    if(!st)
        st = &states()[device];
    st->tempoMultiplier = multiplier;
    if(!st->engine)
        return;
    st->engine->multiplier = multiplier;
    st->engine->rationalClock = false;
    st->engine->invalidateDue();
}

int atEnd(ADL_MIDIPlayer *device)
{
    State *st = device ? findState(device) : 0;
    if(!st || !st->engine)
        return -1;
    return st->engine->finished ? 1 : 0;
}

void setDebugEventHook(DebugEventHook hook, void *userData)
{
    g_debugHook = hook;
    g_debugUserData = userData;
}

} // namespace adl_smf2
