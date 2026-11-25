/*
 * Generate a small legal SMF Format 2 file with independent track tempos,
 * play it through libADLMIDI's layered mode, and write raw stereo signed
 * 16-bit little-endian PCM.  No WAV header is written.
 */

#include <adlmidi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

typedef std::vector<unsigned char> Bytes;

static void putBE(Bytes &b, unsigned long value, unsigned count)
{
    for(unsigned i = count; i > 0; --i)
        b.push_back(static_cast<unsigned char>((value >> (8 * (i - 1))) & 0xff));
}

static void putTag(Bytes &b, const char tag[4])
{
    for(int i = 0; i < 4; ++i)
        b.push_back(static_cast<unsigned char>(tag[i]));
}

static void putVLQ(Bytes &b, unsigned long value)
{
    unsigned char bytes[4];
    unsigned count = 0;
    bytes[count++] = static_cast<unsigned char>(value & 0x7f);
    while((value >>= 7) != 0)
        bytes[count++] = static_cast<unsigned char>((value & 0x7f) | 0x80);
    while(count)
        b.push_back(bytes[--count]);
}

static void addChunk(Bytes &out, const char tag[4], const Bytes &payload)
{
    putTag(out, tag);
    putBE(out, payload.size(), 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

struct Track
{
    Bytes bytes;

    void tempo(unsigned long delta, unsigned long usec)
    {
        putVLQ(bytes, delta);
        bytes.push_back(0xff);
        bytes.push_back(0x51);
        bytes.push_back(3);
        bytes.push_back(static_cast<unsigned char>((usec >> 16) & 0xff));
        bytes.push_back(static_cast<unsigned char>((usec >> 8) & 0xff));
        bytes.push_back(static_cast<unsigned char>(usec & 0xff));
    }

    void program(unsigned long delta, unsigned channel, unsigned program)
    {
        putVLQ(bytes, delta);
        bytes.push_back(static_cast<unsigned char>(0xc0 | (channel & 15)));
        bytes.push_back(static_cast<unsigned char>(program & 127));
    }

    void cc(unsigned long delta, unsigned channel, unsigned controller, unsigned value)
    {
        putVLQ(bytes, delta);
        bytes.push_back(static_cast<unsigned char>(0xb0 | (channel & 15)));
        bytes.push_back(static_cast<unsigned char>(controller & 127));
        bytes.push_back(static_cast<unsigned char>(value & 127));
    }

    void note(unsigned long delta, unsigned channel, unsigned note, unsigned velocity)
    {
        putVLQ(bytes, delta);
        bytes.push_back(static_cast<unsigned char>(0x90 | (channel & 15)));
        bytes.push_back(static_cast<unsigned char>(note & 127));
        bytes.push_back(static_cast<unsigned char>(velocity & 127));
    }

    void end(unsigned long delta)
    {
        putVLQ(bytes, delta);
        bytes.push_back(0xff);
        bytes.push_back(0x2f);
        bytes.push_back(0);
    }
};

static Bytes makeSMF2()
{
    Bytes midi;
    putTag(midi, "MThd");
    putBE(midi, 6, 4);
    putBE(midi, 2, 2); /* SMF Format 2 */
    putBE(midi, 2, 2); /* two independent tracks */
    putBE(midi, 480, 2);

    Track slow;
    slow.tempo(0, 500000);  /* 50 OPL output frames per tick at 48 kHz */
    slow.program(0, 0, 0);
    slow.cc(0, 0, 7, 110);
    slow.cc(0, 0, 10, 28);
    slow.note(0, 0, 60, 110);
    slow.note(480, 0, 60, 0); /* velocity-zero note off */
    slow.note(0, 0, 67, 105);
    slow.note(480, 0, 67, 0);
    slow.end(24);
    addChunk(midi, "MTrk", slow.bytes);

    Track fast;
    fast.tempo(0, 250000);  /* 25 output frames per tick */
    fast.program(0, 0, 25);
    fast.cc(0, 0, 7, 105);
    fast.cc(0, 0, 10, 100);
    fast.note(0, 0, 72, 110);
    fast.note(480, 0, 72, 0);
    fast.note(0, 0, 76, 105);
    fast.note(480, 0, 76, 0);
    fast.end(48);
    addChunk(midi, "MTrk", fast.bytes);

    return midi;
}

int main(int argc, char **argv)
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s output.pcm\n", argv[0] ? argv[0] : "smf2_layered");
        return 2;
    }

    Bytes midi = makeSMF2();
    ADL_MIDIPlayer *player = adl_init(48000);
    if(!player)
        return 1;

    adl_setLoopEnabled(player, 0);
    if(adl_switchEmulator(player, ADLMIDI_EMU_DOSBOX) != 0 ||
       adl_setNumChips(player, 4) < 0 ||
       adl_setBank(player, 0) < 0 ||
       adl_setSMF2Layered(player, 1) != 0 ||
       adl_openData(player, midi.empty() ? 0 : midi.data(),
                    static_cast<unsigned long>(midi.size())) != 0)
    {
        std::fprintf(stderr, "SMF2 layered example setup failed: %s\n", adl_errorInfo(player));
        adl_close(player);
        return 1;
    }

    enum { FRAMES = 8192 };
    std::vector<short> pcm(static_cast<size_t>(FRAMES) * 2, 0);
    int writtenSamples = adl_play(player, FRAMES * 2, pcm.data());
    if(writtenSamples != FRAMES * 2)
    {
        std::fprintf(stderr, "SMF2 layered example rendered %d samples, wanted %d\n",
                     writtenSamples, FRAMES * 2);
        adl_close(player);
        return 1;
    }

    bool nonZero = false;
    for(size_t i = 0; i < pcm.size(); ++i)
        nonZero = nonZero || pcm[i] != 0;
    if(!nonZero)
    {
        std::fprintf(stderr, "SMF2 layered example produced silence\n");
        adl_close(player);
        return 1;
    }

    FILE *out = std::fopen(argv[1], "wb");
    if(!out)
    {
        adl_close(player);
        return 1;
    }

    /* short is little-endian on the target task runners; emit explicit LE. */
    unsigned char raw[2];
    for(size_t i = 0; i < pcm.size(); ++i)
    {
        unsigned short value = static_cast<unsigned short>(pcm[i]);
        raw[0] = static_cast<unsigned char>(value & 0xff);
        raw[1] = static_cast<unsigned char>((value >> 8) & 0xff);
        if(std::fwrite(raw, 1, 2, out) != 2)
        {
            std::fclose(out);
            adl_close(player);
            return 1;
        }
    }

    if(std::fclose(out) != 0)
    {
        adl_close(player);
        return 1;
    }

    adl_close(player);
    return 0;
}
