// SlotTailRender: render an Echo handoff tail with ALC_SOFT_loopback and write
// interleaved 32-bit floating-point stereo PCM. The output path is the only
// command-line argument. The file is read back before this program succeeds.

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace {

constexpr ALCint SampleRate = 48000;
constexpr unsigned PreFrames = 2048;
constexpr unsigned TailFrames = 4096;
constexpr unsigned PulseFrame = 1800;

auto fail(std::string const &message) -> int
{
    std::fprintf(stderr, "slot_tail_render: %s\n", message.c_str());
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    if(argc != 2)
        return fail("usage: slot_tail_render <output-f32-stereo.pcm>");

    ALCdevice *device = alcLoopbackOpenDeviceSOFT(nullptr);
    if(!device)
        return fail("alcLoopbackOpenDeviceSOFT returned null");

    const ALCint attrs[] = {
        ALC_FREQUENCY, SampleRate,
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
        0};
    ALCcontext *context = alcCreateContext(device, attrs);
    if(!context || alcMakeContextCurrent(context) == ALC_FALSE)
    {
        if(context)
            alcDestroyContext(context);
        alcCloseDevice(device);
        return fail("failed to create loopback context");
    }

    ALuint buffer{}, source{}, echo{}, slot{}, directFilter{};
    alGenBuffers(1, &buffer);
    alGenSources(1, &source);
    alGenEffects(1, &echo);
    alGenAuxiliaryEffectSlots(1, &slot);
    alGenFilters(1, &directFilter);

    /* One generated impulse. The direct path is muted so the PCM demonstrates
     * the effect handoff tail rather than the source's direct signal.
     */
    auto input = std::vector<float>(4096, 0.0f);
    input[PulseFrame] = 0.45f;
    alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(),
        static_cast<ALsizei>(input.size() * sizeof(float)), SampleRate);

    alEffecti(echo, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    alEffectf(echo, AL_ECHO_DELAY, 0.02f);
    alEffectf(echo, AL_ECHO_LRDELAY, 0.0f);
    alEffectf(echo, AL_ECHO_DAMPING, 0.0f);
    alEffectf(echo, AL_ECHO_FEEDBACK, 0.35f);
    alEffectf(echo, AL_ECHO_SPREAD, 0.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, echo);

    alFilteri(directFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(directFilter, AL_LOWPASS_GAIN, 0.0f);
    alSourcei(source, AL_DIRECT_FILTER, directFilter);
    alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot, 0, AL_FILTER_NULL);
    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
    alSourcei(source, AL_BUFFER, buffer);
    alSourcePlay(source);

    if(alGetError() != AL_NO_ERROR)
        return fail("OpenAL setup failed");

    auto pre = std::vector<float>(static_cast<std::size_t>(PreFrames) * 2u);
    alcRenderSamplesSOFT(device, pre.data(), PreFrames);

    /* Give the old Echo state a finite retirement budget, then replace it by
     * AL_EFFECT_NULL. The retiring Echo remains audible.
     */
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, TailFrames);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
    alSourceStop(source);
    if(alGetError() != AL_NO_ERROR)
        return fail("effect handoff failed");

    auto tail = std::vector<float>(static_cast<std::size_t>(TailFrames) * 2u);
    alcRenderSamplesSOFT(device, tail.data(), TailFrames);
    if(alcGetError(device) != ALC_NO_ERROR || alGetError() != AL_NO_ERROR)
        return fail("loopback rendering failed");

    auto pcm = pre;
    pcm.insert(pcm.end(), tail.begin(), tail.end());

    {
        std::ofstream output{argv[1], std::ios::binary | std::ios::trunc};
        if(!output)
            return fail("cannot open output file");
        output.write(reinterpret_cast<char const *>(pcm.data()),
            static_cast<std::streamsize>(pcm.size() * sizeof(float)));
        if(!output)
            return fail("failed writing PCM");
    }

    /* Read the file back and verify that the retired segment contains real,
     * finite sound rather than merely creating an empty artifact.
     */
    std::ifstream inputFile{argv[1], std::ios::binary};
    if(!inputFile)
        return fail("cannot reopen PCM file");

    inputFile.seekg(0, std::ios::end);
    auto const byteCount = static_cast<std::streamoff>(inputFile.tellg());
    auto const expectedBytes = static_cast<std::streamoff>(pcm.size() * sizeof(float));
    if(byteCount != expectedBytes)
        return fail("PCM file size mismatch");
    inputFile.seekg(0, std::ios::beg);

    auto readback = std::vector<float>(pcm.size());
    inputFile.read(reinterpret_cast<char *>(readback.data()), expectedBytes);
    if(!inputFile)
        return fail("failed reading PCM back");

    auto tailPeak = 0.0f;
    auto tailEnergy = 0.0;
    auto tailNonFinite = false;
    for(auto frame = 0u; frame < TailFrames; ++frame)
    {
        for(auto ch = 0; ch < 2; ++ch)
        {
            auto const sample = readback[(PreFrames + frame) * 2u + ch];
            if(!std::isfinite(sample))
                tailNonFinite = true;
            tailPeak = std::max(tailPeak, std::fabs(sample));
            tailEnergy += static_cast<double>(sample) * sample;
        }
    }

    if(tailNonFinite || tailPeak < 0.05f || tailEnergy < 0.01)
    {
        std::fprintf(stderr, "tail check failed: peak=%g energy=%.9g\n",
            tailPeak, tailEnergy);
        return fail("PCM tail was not present");
    }

    std::printf("slot_tail_render wrote %lld bytes; tail_peak=%.6f tail_energy=%.6g\n",
        static_cast<long long>(byteCount), tailPeak, tailEnergy);

    alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alDeleteFilters(1, &directFilter);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &echo);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(context);
    alcCloseDevice(device);
    return 0;
}
