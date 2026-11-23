
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr ALuint Rate{48000u};
constexpr float PulseAmp{0.2f};
constexpr unsigned PrefixFrames{512u};
constexpr unsigned TailFrames{1024u};
constexpr ALint TailBudget{ALint(TailFrames)};

/* Runs a helper that itself uses TASK_CHECK; propagates its failure. */
#define TASK_RUN(call) TASK_CHECK((call) == 0)

struct Device {
    ALCdevice *mDev{nullptr};
    ALCcontext *mCtx{nullptr};
    ALuint mSlot{0}, mSource{0}, mBuffer{0}, mFilter{0};

    int open()
    {
        mDev = alcLoopbackOpenDeviceSOFT(nullptr);
        TASK_CHECK(mDev != nullptr);
        ALCint const attrs[]{ALC_FREQUENCY, ALCint{Rate},
            ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
            ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT, 0};
        mCtx = alcCreateContext(mDev, attrs);
        TASK_CHECK(mCtx != nullptr);
        TASK_CHECK(alcMakeContextCurrent(mCtx) == ALC_TRUE);

        alGenAuxiliaryEffectSlots(1, &mSlot);
        alGenSources(1, &mSource);
        alGenFilters(1, &mFilter);
        alFilteri(mFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(mFilter, AL_LOWPASS_GAIN, 0.0f);
        alSourcei(mSource, AL_DIRECT_FILTER, mFilter);
        alSource3i(mSource, AL_AUXILIARY_SEND_FILTER, ALint(mSlot), 0, AL_FILTER_NULL);
        alSourcei(mSource, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(mSource, AL_ROLLOFF_FACTOR, 0.0f);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int close()
    {
        alcMakeContextCurrent(nullptr);
        if(mSource) alDeleteSources(1, &mSource);
        if(mBuffer) alDeleteBuffers(1, &mBuffer);
        if(mFilter) alDeleteFilters(1, &mFilter);
        if(mSlot) alDeleteAuxiliaryEffectSlots(1, &mSlot);
        if(mCtx) alcDestroyContext(mCtx);
        if(mDev) TASK_CHECK(alcCloseDevice(mDev) == ALC_TRUE);
        mDev = nullptr; mCtx = nullptr;
        mSlot = mSource = mBuffer = mFilter = 0;
        return 0;
    }

    int loadEcho(float const delay, float const lrDelay, float const feedback)
    {
        ALuint effect{0};
        alGenEffects(1, &effect);
        alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
        alEffectf(effect, AL_ECHO_DELAY, delay);
        alEffectf(effect, AL_ECHO_LRDELAY, lrDelay);
        alEffectf(effect, AL_ECHO_DAMPING, 0.0f);
        alEffectf(effect, AL_ECHO_FEEDBACK, feedback);
        alEffectf(effect, AL_ECHO_SPREAD, 0.0f);
        alAuxiliaryEffectSloti(mSlot, AL_EFFECTSLOT_EFFECT, ALint(effect));
        alDeleteEffects(1, &effect);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int loadNull()
    {
        alAuxiliaryEffectSloti(mSlot, AL_EFFECTSLOT_EFFECT, 0);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    /* A different effect type, hence a distinct EffectState instance. */
    int loadCompressor()
    {
        ALuint effect{0};
        alGenEffects(1, &effect);
        alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_COMPRESSOR);
        alEffecti(effect, AL_COMPRESSOR_ONOFF, AL_TRUE);
        alAuxiliaryEffectSloti(mSlot, AL_EFFECTSLOT_EFFECT, ALint(effect));
        alDeleteEffects(1, &effect);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int setBudget(ALint const budget)
    {
        alAuxiliaryEffectSloti(mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, budget);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int loadInput(std::vector<float> input)
    {
        if(mBuffer)
        {
            alSourcei(mSource, AL_BUFFER, 0);
            alDeleteBuffers(1, &mBuffer);
            mBuffer = 0;
        }
        alGenBuffers(1, &mBuffer);
        alBufferData(mBuffer, AL_FORMAT_MONO_FLOAT32, input.data(),
            ALsizei(input.size()*sizeof(float)), ALsizei(Rate));
        alSourcei(mSource, AL_BUFFER, ALint(mBuffer));
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int play()
    {
        alSourcePlay(mSource);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int stop()
    {
        alSourceStop(mSource);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        return 0;
    }

    int render(std::vector<float> &out, unsigned const frames)
    {
        auto const oldSize = out.size();
        out.resize(oldSize + size_t{frames}*2u);
        alcRenderSamplesSOFT(mDev, out.data()+oldSize, frames);
        TASK_CHECK(alcGetError(mDev) == ALC_NO_ERROR);
        return 0;
    }
};

auto pulseInput(unsigned const frames, std::vector<unsigned> const &pulses,
    float const amp = PulseAmp) -> std::vector<float>
{
    std::vector<float> input(frames, 0.0f);
    for(auto const p : pulses)
        input[p] = amp;
    return input;
}

auto frameEnergy(std::vector<float> const &pcm, unsigned const frame) -> float
{
    auto const l = pcm[size_t{frame}*2u];
    auto const r = pcm[size_t{frame}*2u+1u];
    return l*l + r*r;
}

auto segmentEnergy(std::vector<float> const &pcm, unsigned const begin, unsigned const end) -> double
{
    auto energy = 0.0;
    for(auto frame = begin; frame < end && frame < pcm.size()/2u; ++frame)
        energy += double(frameEnergy(pcm, frame));
    return energy;
}

int checkFinite(std::vector<float> const &pcm)
{
    for(auto const x : pcm)
        TASK_CHECK(std::isfinite(x));
    return 0;
}

auto firstAudibleFrame(std::vector<float> const &pcm, unsigned const start,
    float const threshold = 1e-10f) -> long
{
    for(auto frame = start; frame < pcm.size()/2u; ++frame)
    {
        if(frameEnergy(pcm, frame) > threshold)
            return long(frame);
    }
    return -1;
}

/* Independent reference renderer: a separate loopback device running with the
 * default (zero) tail budget, so only the original effect DSP is exercised.
 */
struct RefRenderer {
    Device mDev;

    int startEcho(std::vector<float> const &input, float const delay, float const feedback)
    {
        TASK_RUN(mDev.open());
        TASK_RUN(mDev.loadEcho(delay, 0.0f, feedback));
        TASK_RUN(mDev.loadInput(input));
        TASK_RUN(mDev.play());
        return 0;
    }

    int render(std::vector<float> &out, unsigned const frames)
    {
        TASK_RUN(mDev.render(out, frames));
        return 0;
    }

    ~RefRenderer() { (void)mDev.close(); }
};

/* Weighted sum of the old effect's zero-input continuation and the new
 * effect's attack, computed independently from two reference renders.
 */
auto buildExpected(std::vector<float> const &oldCont, std::vector<float> const &newAttack,
    std::size_t const budget) -> std::vector<float>
{
    auto const frames = std::max(oldCont.size(), newAttack.size())/2u;
    std::vector<float> expected(frames*2u, 0.0f);
    for(auto frame = 0u; frame < frames; ++frame)
    {
        for(auto ch = 0u; ch < 2u; ++ch)
        {
            auto const idx = size_t{frame}*2u+ch;
            auto value = 0.0f;
            if(idx < newAttack.size())
                value += newAttack[idx];
            if(frame < budget && idx < oldCont.size())
                value += oldCont[idx] * float(budget - 1 - frame)/float(budget);
            expected[idx] = value;
        }
    }
    return expected;
}

auto worstTailDiff(std::vector<float> const &pcm, std::vector<float> const &expected,
    unsigned const prefix) -> float
{
    auto worst = 0.0f;
    for(auto frame = 0u; frame < expected.size()/2u; ++frame)
        for(auto ch = 0u; ch < 2u; ++ch)
            worst = std::max(worst, std::fabs(
                pcm[(size_t{prefix}+frame)*2u+ch] - expected[frame*2u+ch]));
    return worst;
}

constexpr float EchoDelay{480.0f/float(Rate)}; /* 480 frames */
constexpr float EchoDelayFrames{480.0f};

} /* namespace */

/* ============================ handoff group ============================ */

static int handoff_enum_lookup()
{
    Device dev;
    TASK_RUN(dev.open());
    ALenum const value = alGetEnumValue("AL_EFFECTSLOT_TAIL_SAMPLES_SOFT");
    TASK_CHECK(value != 0 && value == AL_EFFECTSLOT_TAIL_SAMPLES_SOFT);
    TASK_CHECK(alIsExtensionPresent("AL_SOFT_effectslot_tail_samples") == AL_TRUE);

    ALint got{-1};
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(alGetError() == AL_NO_ERROR && got == 0);
    TASK_RUN(dev.close());
    return 0;
}

static int handoff_echo_to_null()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));

    /* Independent reference: the original echo, same input, stopped at the
     * switch point so its continuation runs on zero input; the NULL attack is
     * silence.
     */
    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, TailFrames));

    auto const expected = buildExpected(oldCont,
        std::vector<float>(TailFrames*2u, 0.0f), TailBudget);

    /* Prefix output is untouched by the new mode. */
    auto prefixWorst = 0.0f;
    for(auto i = 0u; i < refPrefix.size(); ++i)
        prefixWorst = std::max(prefixWorst, std::fabs(pcm[i] - refPrefix[i]));
    TASK_CHECK(prefixWorst < 1e-6f);

    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) > 1e-4);
    /* The final fade frame carries weight zero. */
    TASK_CHECK(frameEnergy(pcm, PrefixFrames+TailFrames-1u) < 1e-12f);
    TASK_CHECK(worstTailDiff(pcm, expected, PrefixFrames) < 2e-6f);
    return 0;
}

static int handoff_budget_zero_keeps_behavior()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));

    /* Default budget of 0 drops the old effect immediately. */
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) < 1e-12f);
    TASK_CHECK(firstAudibleFrame(pcm, PrefixFrames) == -1);
    return 0;
}

static int handoff_echo_to_echo()
{
    constexpr float newDelay{360.0f/float(Rate)};

    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    /* Two pulses: one before the switch feeds the old echo, the second after
     * the switch feeds the new echo's attack.
     */
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u, 700u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    /* Going through NULL creates a fresh EchoState instance in the published
     * snapshot, so the original echo genuinely retires (the never-consumed
     * NULL instance makes no sound).
     */
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.loadEcho(newDelay, 0.0f, 0.25f));
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));

    /* Old continuation: old echo with the first pulse only, stopped at switch. */
    RefRenderer oldRef;
    TASK_RUN(oldRef.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> oldPrefix;
    TASK_RUN(oldRef.render(oldPrefix, PrefixFrames));
    TASK_RUN(oldRef.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(oldRef.render(oldCont, TailFrames));

    /* New attack: a fresh echo fed a pulse at the equivalent relative frame
     * (700 - 512 = 188).
     */
    RefRenderer newRef;
    TASK_RUN(newRef.startEcho(pulseInput(TailFrames, {188u}), newDelay, 0.25f));
    std::vector<float> newAttack;
    TASK_RUN(newRef.render(newAttack, TailFrames));

    auto const expected = buildExpected(oldCont, newAttack, TailBudget);
    TASK_CHECK(worstTailDiff(pcm, expected, PrefixFrames) < 2e-6f);
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) > 1e-4);
    return 0;
}

static int handoff_unconsumed_replacements()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));

    /* Several replacements before the next mix. The same-type echo updates
     * keep the original instance; the Compressor creates a distinct instance
     * that is replaced again before the mixer ever runs it, so neither it nor
     * the final NULL instance can produce a tail. Only the processed echo can.
     */
    TASK_RUN(dev.loadEcho(0.005f, 0.0f, 0.9f));
    TASK_RUN(dev.loadCompressor());
    TASK_RUN(dev.loadEcho(0.0025f, 0.0f, 0.9f));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));

    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, TailFrames));
    auto const expected = buildExpected(oldCont,
        std::vector<float>(TailFrames*2u, 0.0f), TailBudget);
    TASK_CHECK(worstTailDiff(pcm, expected, PrefixFrames) < 2e-6f);

    /* The first post-switch tap follows the original 480-frame delay cadence
     * (pulse at 100 -> tap at 580), not any intermediate effect's delay.
     */
    auto const first = firstAudibleFrame(pcm, PrefixFrames);
    TASK_CHECK(first > 0);
    TASK_CHECK(std::fabs(float(first) - 580.0f) <= 3.0f);
    return 0;
}

static int handoff_two_generations_coexist()
{
    constexpr ALint budget{2048};
    constexpr unsigned chunk{256u};
    constexpr unsigned renderTail{768u};

    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(budget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+renderTail, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));

    /* First handoff: the echo retires as generation 0; the NULL state runs. */
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, chunk));

    /* Second handoff: the processed NULL state retires as generation 1; a new
     * echo becomes active. Two generations now coexist and the oldest keeps
     * its own fade progress.
     */
    TASK_RUN(dev.loadEcho(600.0f/float(Rate), 0.0f, 0.3f));
    TASK_RUN(dev.render(pcm, renderTail-chunk));

    /* The retired echo's second tap (pulse at 100 plus two 480-frame delays ->
     * frame 1060, retirement k=548) lands here and remains audible.
     */
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames+chunk, PrefixFrames+renderTail) > 1e-6);

    /* Reference continuation of the retired echo and its exact weights, checked
     * at both delay taps (k=68 and k=548) so the fade progress is verified on
     * real signal across the two-generations boundary.
     */
    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+renderTail, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, renderTail));
    for(unsigned const k : {68u, 255u, 256u, 548u, 767u})
    {
        auto const weight = float(budget - 1 - k)/float(budget);
        for(auto ch = 0u; ch < 2u; ++ch)
        {
            auto const got = pcm[(size_t{PrefixFrames}+k)*2u+ch];
            auto const want = oldCont[size_t{k}*2u+ch] * weight;
            TASK_CHECK(std::fabs(got - want) < 2e-6f);
        }
    }
    TASK_RUN(checkFinite(pcm));
    return 0;
}

static int handoff_third_generation_evicts_oldest()
{
    constexpr ALint budget{4096};
    constexpr unsigned chunk{256u};
    constexpr unsigned total{PrefixFrames + 4u*chunk};

    /* Continuous pulse train so every active state receives real wet input. */
    std::vector<unsigned> pulses;
    for(auto p = 32u; p < total; p += 64u) pulses.push_back(p);

    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(budget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));          /* state A */
    TASK_RUN(dev.loadInput(pulseInput(total, pulses, 0.05f)));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));

    TASK_RUN(dev.loadNull());                                 /* A retires (gen 0) */
    TASK_RUN(dev.render(pcm, chunk));

    TASK_RUN(dev.loadEcho(240.0f/float(Rate), 0.0f, 0.3f));  /* NULL retires (gen 1); C active */
    TASK_RUN(dev.render(pcm, chunk));

    /* The oldest generation (A) is still contributing before the eviction. */
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames+chunk, PrefixFrames+2u*chunk) > 1e-6);

    TASK_RUN(dev.loadNull());                                 /* C retires; A is evicted */
    TASK_RUN(dev.render(pcm, chunk));
    TASK_RUN(dev.render(pcm, chunk));
    TASK_RUN(checkFinite(pcm));

    /* Only the surviving younger generations remain. C saw the pulse train at
     * frames 800..992, so its 240-frame delay taps keep arriving after the
     * third switch at frame 1024 (pulse at 800 -> first post-switch tap near
     * frame 1040), proving a non-oldest generation survived with its own
     * history.
     */
    auto const cTap = firstAudibleFrame(pcm, PrefixFrames+2u*chunk);
    TASK_CHECK(cTap > 0);
    TASK_CHECK(std::fabs(float(cTap) - 1040.0f) <= 4.0f);
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames+2u*chunk,
        PrefixFrames+4u*chunk) > 1e-7);

    /* The evicted oldest generation (A) used a 480-frame delay. Its unique
     * second-feedback tap from the pulse at frame 96 would land on frame 1056,
     * a frame where C's 240-frame cadence has no tap. With A evicted it must
     * be silent, while C's own neighboring taps remain audible.
     */
    TASK_CHECK(frameEnergy(pcm, 1056u) < 1e-12f);
    TASK_CHECK(frameEnergy(pcm, 1040u) > 1e-8f);
    TASK_CHECK(frameEnergy(pcm, 1104u) > 1e-8f);
    return 0;
}

/* ============================== clock group ============================= */

static int clock_chunking_matches_whole()
{
    constexpr unsigned tailLen{265u};
    auto const run = [&](std::array<unsigned,3> const chunks,
        std::vector<float> &out) -> int {
        Device dev;
        TASK_RUN(dev.open());
        TASK_RUN(dev.setBudget(TailBudget));
        TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
        TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+tailLen, {100u})));
        TASK_RUN(dev.play());
        TASK_RUN(dev.render(out, PrefixFrames));
        TASK_RUN(dev.loadNull());
        for(auto const n : chunks)
        {
            if(n) TASK_RUN(dev.render(out, n));
        }
        return 0;
    };

    std::vector<float> whole;
    TASK_RUN(run({tailLen, 0u, 0u}, whole));
    std::vector<float> chunked;
    TASK_RUN(run({1u, 7u, 257u}, chunked));
    TASK_CHECK(whole.size() == chunked.size());

    auto worst = 0.0f;
    for(auto i = 0u; i < whole.size(); ++i)
        worst = std::max(worst, std::fabs(whole[i] - chunked[i]));
    TASK_CHECK(worst < 1e-7f);

    /* Independent weighted reference, checked at frames spanning all chunks. */
    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+tailLen, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, tailLen));
    for(unsigned const k : {0u, 1u, 7u, 8u, 264u})
    {
        auto const weight = float(TailBudget - 1 - k)/float(TailBudget);
        for(auto ch = 0u; ch < 2u; ++ch)
        {
            auto const got = chunked[(size_t{PrefixFrames}+k)*2u+ch];
            auto const want = oldCont[size_t{k}*2u+ch] * weight;
            TASK_CHECK(std::fabs(got - want) < 2e-6f);
        }
    }
    return 0;
}

static int clock_echo_arrival_from_delay()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.0f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, TailFrames));

    /* Delay-derived arrival: pulse at 100 plus the 480-frame echo delay. */
    auto const first = firstAudibleFrame(pcm, 0);
    TASK_CHECK(first > 0);
    TASK_CHECK(std::fabs(float(first) - (100.0f + EchoDelayFrames)) <= 3.0f);

    /* Independent zero-budget reference; the render is split at the same
     * switch frame so its startup gain smoothing timeline matches the tail
     * device's settled retired state.
     */
    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.0f));
    std::vector<float> refPcm;
    TASK_RUN(ref.render(refPcm, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> refTail;
    TASK_RUN(ref.render(refTail, TailFrames));
    refPcm.insert(refPcm.end(), refTail.begin(), refTail.end());
    auto const refFirst = firstAudibleFrame(refPcm, 0);
    TASK_CHECK(refFirst > 0);
    TASK_CHECK(first == refFirst);

    /* The tap level equals the reference tap times the retirement weight. */
    auto const k = unsigned(first) - PrefixFrames;
    auto const weight = float(TailBudget - 1 - k)/float(TailBudget);
    for(auto ch = 0u; ch < 2u; ++ch)
    {
        auto const got = pcm[size_t(unsigned(first))*2u+ch];
        auto const want = refPcm[size_t(unsigned(first))*2u+ch] * weight;
        TASK_CHECK(std::fabs(got - want) < 2e-6f);
    }
    return 0;
}

static int clock_budget_change_does_not_recompute()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, 128u));
    auto const before = pcm;

    /* A budget change while the tail is retiring must not restart or rescale
     * the in-flight generation.
     */
    TASK_RUN(dev.setBudget(256));
    TASK_RUN(dev.render(pcm, TailFrames-128u));
    TASK_RUN(checkFinite(pcm));
    for(auto i = 0u; i < before.size(); ++i)
        TASK_CHECK(std::fabs(pcm[i] - before[i]) < 1e-12f);

    /* The original 1024-frame budget still governs: the second feedback tap
     * lands at k=548 (100 plus two 480-frame delays) and remains audible there
     * with the old budget's weight; under the new 256-frame budget it would
     * already have been cut.
     */
    constexpr unsigned secondTap{548u};
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames+secondTap, PrefixFrames+secondTap+2u) > 1e-8);

    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, TailFrames));
    auto const oldWeight = float(TailBudget - 1 - secondTap)/float(TailBudget);
    for(auto ch = 0u; ch < 2u; ++ch)
    {
        auto const got = pcm[(size_t{PrefixFrames}+secondTap)*2u+ch];
        auto const want = oldCont[size_t{secondTap}*2u+ch] * oldWeight;
        TASK_CHECK(std::fabs(got - want) < 2e-6f);
    }

    /* A later handoff uses the new, smaller budget: its tail is silent after
     * 256 frames even though the device keeps rendering.
     */
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, 256u));
    TASK_RUN(checkFinite(pcm));
    TASK_CHECK(segmentEnergy(pcm, unsigned(pcm.size()/2u)-8u,
        unsigned(pcm.size()/2u)) < 1e-12f);
    return 0;
}

/* ============================ lifetime group ============================ */

static int lifetime_source_stop_keeps_tail()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.stop());
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) > 1e-4);

    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, TailFrames));
    auto const expected = buildExpected(oldCont,
        std::vector<float>(TailFrames*2u, 0.0f), TailBudget);
    TASK_CHECK(worstTailDiff(pcm, expected, PrefixFrames) < 2e-6f);
    return 0;
}

static int lifetime_send_removed_keeps_tail()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    alSource3i(dev.mSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0,
        AL_FILTER_NULL);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) > 1e-4);
    return 0;
}

static int lifetime_source_paused_keeps_tail()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    alSourcePause(dev.mSource);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_RUN(checkFinite(pcm));
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+TailFrames) > 1e-4);

    /* Pausing the source does not freeze the retired state: its output still
     * matches the independent zero-input reference exactly.
     */
    RefRenderer ref;
    TASK_RUN(ref.startEcho(pulseInput(PrefixFrames+TailFrames, {100u}), EchoDelay, 0.5f));
    std::vector<float> refPrefix;
    TASK_RUN(ref.render(refPrefix, PrefixFrames));
    TASK_RUN(ref.mDev.stop());
    std::vector<float> oldCont;
    TASK_RUN(ref.render(oldCont, TailFrames));
    auto const expected = buildExpected(oldCont,
        std::vector<float>(TailFrames*2u, 0.0f), TailBudget);
    TASK_CHECK(worstTailDiff(pcm, expected, PrefixFrames) < 2e-6f);
    return 0;
}

static int lifetime_delete_cancels_and_handle_reuse()
{
    Device dev;
    TASK_RUN(dev.open());
    TASK_RUN(dev.setBudget(TailBudget));
    TASK_RUN(dev.loadEcho(EchoDelay, 0.0f, 0.5f));
    TASK_RUN(dev.loadInput(pulseInput(PrefixFrames+TailFrames, {100u})));
    TASK_RUN(dev.play());

    std::vector<float> pcm;
    TASK_RUN(dev.render(pcm, PrefixFrames));
    TASK_RUN(dev.loadNull());
    TASK_RUN(dev.render(pcm, 128u));
    /* The first echo tap (retirement k=68) has already established the tail. */
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames, PrefixFrames+128u) > 1e-8);

    /* Remove the source's reference, then delete the slot: every retained tail
     * is cancelled immediately.
     */
    alSource3i(dev.mSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0,
        AL_FILTER_NULL);
    alDeleteAuxiliaryEffectSlots(1, &dev.mSlot);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    dev.mSlot = 0;

    TASK_RUN(dev.render(pcm, TailFrames));
    TASK_CHECK(segmentEnergy(pcm, PrefixFrames+128u, unsigned(pcm.size()/2u))
        < 1e-12f);

    /* A reused slot handle starts silent; the cancelled tail cannot return. */
    ALuint reused{0};
    alGenAuxiliaryEffectSlots(1, &reused);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    ALint got{-1};
    alGetAuxiliaryEffectSloti(reused, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(got == 0);
    alAuxiliaryEffectSloti(reused, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, TailBudget);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    std::vector<float> reusePcm;
    TASK_RUN(dev.render(reusePcm, TailFrames));
    TASK_RUN(checkFinite(reusePcm));
    TASK_CHECK(segmentEnergy(reusePcm, 0, TailFrames) < 1e-12f);

    alDeleteAuxiliaryEffectSlots(1, &reused);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    return 0;
}

static int lifetime_invalid_budget_preserves_state()
{
    Device dev;
    TASK_RUN(dev.open());

    ALint got{-1};
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, -1);
    TASK_CHECK(alGetError() == AL_INVALID_VALUE);
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(got == 0);

    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT,
        ALint(10u*Rate + 1u));
    TASK_CHECK(alGetError() == AL_INVALID_VALUE);
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(got == 0);

    /* The upper boundary is legal. */
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT,
        ALint(10u*Rate));
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(got == ALint(10u*Rate));

    /* A nonzero budget cannot be combined with an effect slot target. */
    ALuint target{0};
    alGenAuxiliaryEffectSlots(1, &target);
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TARGET_SOFT, ALint(target));
    TASK_CHECK(alGetError() == AL_INVALID_OPERATION);
    ALint targetValue{-1};
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TARGET_SOFT, &targetValue);
    TASK_CHECK(targetValue == 0);

    /* A chained slot cannot enable a nonzero budget. */
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 0);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TARGET_SOFT, ALint(target));
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 128);
    TASK_CHECK(alGetError() == AL_INVALID_OPERATION);
    alGetAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &got);
    TASK_CHECK(got == 0);

    alAuxiliaryEffectSloti(dev.mSlot, AL_EFFECTSLOT_TARGET_SOFT, 0);
    alDeleteAuxiliaryEffectSlots(1, &target);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_RUN(dev.close());
    return 0;
}

extern const struct task_case task_cases[] = {
    {"handoff", "enum_lookup", handoff_enum_lookup},
    {"handoff", "echo_to_null", handoff_echo_to_null},
    {"handoff", "budget_zero_keeps_behavior", handoff_budget_zero_keeps_behavior},
    {"handoff", "echo_to_echo", handoff_echo_to_echo},
    {"handoff", "unconsumed_replacements", handoff_unconsumed_replacements},
    {"handoff", "two_generations_coexist", handoff_two_generations_coexist},
    {"handoff", "third_generation_evicts_oldest", handoff_third_generation_evicts_oldest},

    {"clock", "chunking_matches_whole", clock_chunking_matches_whole},
    {"clock", "echo_arrival_from_delay", clock_echo_arrival_from_delay},
    {"clock", "budget_change_does_not_recompute", clock_budget_change_does_not_recompute},

    {"lifetime", "source_stop_keeps_tail", lifetime_source_stop_keeps_tail},
    {"lifetime", "source_paused_keeps_tail", lifetime_source_paused_keeps_tail},
    {"lifetime", "send_removed_keeps_tail", lifetime_send_removed_keeps_tail},
    {"lifetime", "delete_cancels_and_handle_reuse", lifetime_delete_cancels_and_handle_reuse},
    {"lifetime", "invalid_budget_preserves_state", lifetime_invalid_budget_preserves_state},
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
