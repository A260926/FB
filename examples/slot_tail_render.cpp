/* Renders an effect-slot tail handoff through a loopback device and writes the
 * result as raw interleaved 32-bit float stereo PCM, then reads the file back
 * to verify it contains a real, decaying tail. No playback device is opened.
 */

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr ALuint Rate{48000u};
constexpr unsigned PrefixFrames{512u};
constexpr unsigned TailFrames{2048u};
constexpr ALint TailBudget{ALint(TailFrames)};

auto check(ALCdevice *dev, bool const condition, char const *what) -> bool
{
    if(condition) return true;
    std::fprintf(stderr, "slot_tail_render: %s failed (AL error %#x, ALC error %#x)\n",
        what, alGetError(), dev ? alcGetError(dev) : ALC_NO_ERROR);
    return false;
}

} // namespace

auto main(int argc, char **argv) -> int
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s <output-float-pcm-path>\n", argv[0]);
        return 2;
    }
    std::string const outPath{argv[1]};

    ALCdevice *dev = alcLoopbackOpenDeviceSOFT(nullptr);
    if(!dev) return 3;
    ALCint const attrs[]{ALC_FREQUENCY, ALCint{Rate},
        ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
        ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT, 0};
    ALCcontext *ctx = alcCreateContext(dev, attrs);
    if(!ctx || alcMakeContextCurrent(ctx) != ALC_TRUE) return 4;

    ALuint slot{0}, source{0}, buffer{0}, filter{0}, effect{0};
    alGenAuxiliaryEffectSlots(1, &slot);
    alGenSources(1, &source);
    alGenBuffers(1, &buffer);
    alGenFilters(1, &filter);
    alGenEffects(1, &effect);

    /* An echo with feedback gives the retired state a multi-tap decaying tail. */
    alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    alEffectf(effect, AL_ECHO_DELAY, 480.0f/float(Rate));
    alEffectf(effect, AL_ECHO_LRDELAY, 0.0f);
    alEffectf(effect, AL_ECHO_DAMPING, 0.0f);
    alEffectf(effect, AL_ECHO_FEEDBACK, 0.5f);
    alEffectf(effect, AL_ECHO_SPREAD, 0.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, ALint(effect));
    alDeleteEffects(1, &effect);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, TailBudget);
    if(alGetError() != AL_NO_ERROR) return 5;

    alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(filter, AL_LOWPASS_GAIN, 0.0f);
    alSourcei(source, AL_DIRECT_FILTER, filter);
    alSource3i(source, AL_AUXILIARY_SEND_FILTER, ALint(slot), 0, AL_FILTER_NULL);
    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);

    std::vector<float> input(PrefixFrames + TailFrames, 0.0f);
    input[100] = 0.2f;
    alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(),
        ALsizei(input.size()*sizeof(float)), ALsizei(Rate));
    alSourcei(source, AL_BUFFER, ALint(buffer));
    if(alGetError() != AL_NO_ERROR) return 6;

    std::vector<float> pcm;
    pcm.resize(size_t{PrefixFrames}*2u);
    alSourcePlay(source);
    alcRenderSamplesSOFT(dev, pcm.data(), PrefixFrames);

    /* Replace the echo with the NULL effect; the echo's tail keeps fading. */
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    pcm.resize(size_t{PrefixFrames + TailFrames}*2u);
    alcRenderSamplesSOFT(dev, pcm.data() + size_t{PrefixFrames}*2u, TailFrames);
    if(alcGetError(dev) != ALC_NO_ERROR) return 7;

    /* Write the raw interleaved float PCM. */
    std::ofstream out{outPath, std::ios::binary | std::ios::trunc};
    if(!out)
    {
        std::fprintf(stderr, "slot_tail_render: cannot open %s\n", outPath.c_str());
        return 8;
    }
    out.write(reinterpret_cast<char const *>(pcm.data()),
        std::streamsize(pcm.size()*sizeof(float)));
    if(!out.good()) return 9;
    out.close();

    /* Read it back and verify the rendered tail really exists and decays. */
    std::ifstream in{outPath, std::ios::binary | std::ios::ate};
    if(!in) return 10;
    auto const fileBytes = std::size_t(in.tellg());
    if(fileBytes != pcm.size()*sizeof(float)) return 11;
    in.seekg(0);
    std::vector<float> readback(pcm.size());
    in.read(reinterpret_cast<char *>(readback.data()), std::streamsize(fileBytes));
    if(!in.good()) return 12;

    for(std::size_t i{0}; i < readback.size(); ++i)
    {
        if(!std::isfinite(readback[i])) return 13;
        if(std::fabs(readback[i] - pcm[i]) > 0.0f) return 14;
    }

    auto energy = [](std::vector<float> const &data, unsigned begin, unsigned end) {
        double e{0.0};
        for(auto f = begin; f < end; ++f)
            e += double(data[f*2u])*data[f*2u] + double(data[f*2u+1u])*data[f*2u+1u];
        return e;
    };
    if(!check(dev, energy(readback, PrefixFrames, PrefixFrames+TailFrames) > 1e-4,
        "audible tail energy"))
        return 15;
    /* The final fade frame has weight zero, so it must be silent. */
    if(!check(dev, energy(readback, PrefixFrames+TailFrames-1u, PrefixFrames+TailFrames)
        < 1e-12f, "faded-out final frame"))
        return 16;
    /* The tail's later half is quieter than its first half (linear fade). */
    if(!check(dev,
        energy(readback, PrefixFrames+TailFrames/2u, PrefixFrames+TailFrames)
        < energy(readback, PrefixFrames, PrefixFrames+TailFrames/2u),
        "decaying tail envelope"))
        return 17;

    alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    alSourcei(source, AL_BUFFER, 0);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alDeleteFilters(1, &filter);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    if(alGetError() != AL_NO_ERROR) return 18;

    alcMakeContextCurrent(nullptr);
    alcDestroyContext(ctx);
    if(!alcCloseDevice(dev)) return 19;

    std::printf("slot_tail_render: wrote %zu stereo frames to %s (tail energy verified)\n",
        size_t(PrefixFrames+TailFrames), outPath.c_str());
    return 0;
}
