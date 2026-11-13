////////////////////////////////////////////////////////////////////////////////
///
/// time_anchor.cpp : example for the anchored time-stretch API.
///
/// Generates a short stereo signal that contains two distinct drum-like
/// attacks, stretches it with SoundTouch::setTimeAnchors() so that both
/// attacks (and the head/tail) are protected verbatim while the gaps between
/// them are WSOLA-stretched, and writes the result as a stereo 16-bit PCM
/// WAV file to .build/time-anchor-example.wav.
///
////////////////////////////////////////////////////////////////////////////////

#include "SoundTouch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using namespace soundtouch;

namespace
{

const double PI = 3.14159265358979323846;
const int SAMPLE_RATE = 48000;
const int CHANNELS = 2;
const uint PROTECT = 128;

// Finite input N frames and desired output M frames, with internal anchors
// marking the two attacks. Every neighbouring protected zone is separated by
// at least 8192 frames on both axes and every gap ratio is within [0.5, 2].
const std::vector<TimeAnchor> ANCHORS = {
    {0, 0},
    {24000, 32000},     // first attack, expanded gap before it
    {48000, 48000},     // second attack, nominal gap
    {96000, 104000}     // end of the finite run, mildly expanded tail gap
};

static void writeLE(std::ofstream &out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out.put((char)((value >> (8 * i)) & 0xFFu));
    }
}

// A short, decaying percussive attack with a distinct pitch per hit.
static float drum(float phase, float decay)
{
    float env = std::exp(-decay);
    return 0.9f * env * (float)std::sin(phase);
}

static std::vector<float> generateInput()
{
    const int N = (int)ANCHORS.back().inputFrame;
    std::vector<float> pcm((size_t)N * CHANNELS);

    for (int i = 0; i < N; ++i)
    {
        // gentle stereo bed tone
        float bed = 0.12f * (float)std::sin(2.0 * PI * 110.0 * i / SAMPLE_RATE);

        float hit = 0.0f;
        // two different attacks at the internal anchor frames
        const int attacks[2] = {24000, 48000};
        const float freqs[2] = {180.0f, 92.0f};
        for (int k = 0; k < 2; ++k)
        {
            int d = i - attacks[k];
            if (d >= 0 && d < 1200)
            {
                float phase = 2.0 * PI * freqs[k] * d / SAMPLE_RATE;
                hit += drum(phase, 6.0f * d / 1200.0f);
            }
        }

        float left = bed + hit;
        float right = -bed - 0.7f * hit;     // fixed per-channel scale relation
        pcm[(size_t)i * CHANNELS + 0] = left;
        pcm[(size_t)i * CHANNELS + 1] = right;
    }
    return pcm;
}

static void writeWav(const std::vector<float> &pcm, const char *path)
{
    std::ofstream out(path, std::ios::binary);
    const uint32_t dataBytes = (uint32_t)(pcm.size() * sizeof(int16_t));

    out.write("RIFF", 4);
    writeLE(out, 36u + dataBytes, 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLE(out, 16u, 4);                  // PCM fmt chunk size
    writeLE(out, 1u, 2);                   // PCM format
    writeLE(out, (uint32_t)CHANNELS, 2);
    writeLE(out, (uint32_t)SAMPLE_RATE, 4);
    writeLE(out, (uint32_t)SAMPLE_RATE * CHANNELS * 2u, 4);   // byte rate
    writeLE(out, (uint32_t)CHANNELS * 2u, 2);                 // block align
    writeLE(out, 16u, 2);                 // bits per sample
    out.write("data", 4);
    writeLE(out, dataBytes, 4);

    for (float v : pcm)
    {
        float clamped = v;
        if (clamped > 1.0f) clamped = 1.0f;
        if (clamped < -1.0f) clamped = -1.0f;
        int16_t s = (int16_t)std::lround(clamped * 32767.0f);
        writeLE(out, (uint32_t)(uint16_t)s, 2);
    }
}

} // namespace

int main()
{
    std::vector<float> input = generateInput();

    SoundTouch st;
    st.setSampleRate(SAMPLE_RATE);
    st.setChannels(CHANNELS);
    // rate, pitch and tempo nominal before setting the anchors
    st.setRate(1.0f);
    st.setPitch(1.0f);
    st.setTempo(1.0f);
    st.setSetting(SETTING_USE_QUICKSEEK, 0);
    st.setSetting(SETTING_SEQUENCE_MS, 40);
    st.setSetting(SETTING_SEEKWINDOW_MS, 15);
    st.setSetting(SETTING_OVERLAP_MS, 8);
    st.setTimeAnchors(ANCHORS, PROTECT);

    const uint64_t N = ANCHORS.back().inputFrame;
    std::vector<float> output;
    std::vector<float> readBuffer(4096 * CHANNELS);

    uint64_t fed = 0;
    while (fed < N)
    {
        uint chunk = (uint)std::min(4096ull, N - fed);
        st.putSamples(input.data() + fed * CHANNELS, chunk);
        fed += chunk;

        uint got;
        while ((got = st.receiveSamples(readBuffer.data(), 4096)))
        {
            output.insert(output.end(), readBuffer.begin(),
                          readBuffer.begin() + (size_t)got * CHANNELS);
        }
    }

    st.flush();
    uint got;
    while ((got = st.receiveSamples(readBuffer.data(), 4096)))
    {
        output.insert(output.end(), readBuffer.begin(),
                      readBuffer.begin() + (size_t)got * CHANNELS);
    }

    const uint64_t expectedFrames = ANCHORS.back().outputFrame;
    if (output.size() != (size_t)expectedFrames * CHANNELS)
    {
        std::fprintf(stderr, "unexpected output length %zu (want %llu frames)\n",
                     output.size() / CHANNELS,
                     (unsigned long long)expectedFrames);
        return 1;
    }

    const char *path = ".build/time-anchor-example.wav";
    writeWav(output, path);
    std::printf("wrote %s: %llu input frames -> %llu output frames, stereo PCM16\n",
                path, (unsigned long long)N, (unsigned long long)expectedFrames);
    return 0;
}
