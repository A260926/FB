#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "task_support/check.h"

namespace {

constexpr ALCint Frequency = 48000;

constexpr auto operator""_uz(unsigned long long value) noexcept -> std::size_t
{ return static_cast<std::size_t>(value); }

struct World {
    ALCdevice *device{};
    ALCcontext *context{};
    bool ok{};

    World()
    {
        device = alcLoopbackOpenDeviceSOFT(nullptr);
        if(!device)
            return;
        const ALCint attrs[] = {
            ALC_FREQUENCY, Frequency,
            ALC_FORMAT_CHANNELS_SOFT, ALC_STEREO_SOFT,
            ALC_FORMAT_TYPE_SOFT, ALC_FLOAT_SOFT,
            0};
        context = alcCreateContext(device, attrs);
        if(!context || alcMakeContextCurrent(context) == ALC_FALSE)
            return;
        ok = (alGetError() == AL_NO_ERROR) && (alcGetError(device) == ALC_NO_ERROR);
    }

    ~World()
    {
        if(context)
        {
            alcMakeContextCurrent(context);
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
        }
        if(device)
            alcCloseDevice(device);
    }

    auto render(ALsizei frames) const -> std::vector<float>
    {
        auto samples = std::vector<float>(static_cast<std::size_t>(frames) * 2_uz);
        alcRenderSamplesSOFT(device, samples.data(), frames);
        return samples;
    }
};

struct Rendered {
    bool ok{};
    std::vector<float> samples;
};

struct EchoKit {
    World world;
    ALuint buffer{}, source{}, echo{}, slot{}, filter{};
    bool ok{};

    auto init(unsigned const pulseFrame, float const delay, float const amplitude = 0.25f,
        float const feedback = 0.0f, unsigned const bufferFrames = 4096,
        unsigned const extraPulseFrame = std::numeric_limits<unsigned>::max()) -> bool
    {
        if(!world.ok || pulseFrame >= bufferFrames)
            return false;

        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        alGenEffects(1, &echo);
        alGenAuxiliaryEffectSlots(1, &slot);
        alGenFilters(1, &filter);

        auto input = std::vector<float>(bufferFrames, 0.0f);
        input[pulseFrame] = amplitude;
        if(extraPulseFrame < bufferFrames)
            input[extraPulseFrame] = amplitude;
        alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(),
            static_cast<ALsizei>(input.size() * sizeof(float)), Frequency);

        alEffecti(echo, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
        alEffectf(echo, AL_ECHO_DELAY, delay);
        alEffectf(echo, AL_ECHO_LRDELAY, 0.0f);
        alEffectf(echo, AL_ECHO_DAMPING, 0.0f);
        alEffectf(echo, AL_ECHO_FEEDBACK, feedback);
        alEffectf(echo, AL_ECHO_SPREAD, 0.0f);
        alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, echo);

        alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(filter, AL_LOWPASS_GAIN, 0.0f);
        alSourcei(source, AL_DIRECT_FILTER, filter);
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot, 0, AL_FILTER_NULL);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(source, AL_BUFFER, buffer);
        alSourcePlay(source);

        return alGetError() == AL_NO_ERROR;
    }

    ~EchoKit()
    {
        if(world.context)
            alcMakeContextCurrent(world.context);
        if(source)
        {
            alSourceStop(source);
            alDeleteSources(1, &source);
        }
        if(buffer)
            alDeleteBuffers(1, &buffer);
        if(filter)
            alDeleteFilters(1, &filter);
        if(slot)
            alDeleteAuxiliaryEffectSlots(1, &slot);
        if(echo)
            alDeleteEffects(1, &echo);
    }
};

auto maxAbsDifference(std::vector<float> const &a, std::vector<float> const &b) -> float
{
    if(a.size() != b.size())
        return std::numeric_limits<float>::infinity();
    auto result = 0.0f;
    for(auto i = std::size_t{0}; i < a.size(); ++i)
        result = std::max(result, std::fabs(a[i] - b[i]));
    return result;
}

auto echoZeroReference(unsigned const pulseFrame, float const delay, unsigned const preFrames,
    unsigned const tailFrames) -> Rendered
{
    EchoKit kit;
    if(!kit.init(pulseFrame, delay))
        return {};
    kit.world.render(static_cast<ALsizei>(preFrames));
    alSourceStop(kit.source);
    return {true, kit.world.render(static_cast<ALsizei>(tailFrames))};
}

auto weightedNullExpectation(Rendered const &reference, unsigned const n) -> std::vector<float>
{
    auto expected = std::vector<float>(static_cast<std::size_t>(n) * 2_uz, 0.0f);
    if(!reference.ok || reference.samples.size() < expected.size())
        return expected;
    for(auto frame = 0u; frame < n; ++frame)
    {
        auto const weight = static_cast<float>(n - 1u - frame) / static_cast<float>(n);
        for(auto ch = 0; ch < 2; ++ch)
        {
            auto const idx = static_cast<std::size_t>(frame) * 2_uz + ch;
            expected[idx] = reference.samples[idx] * weight;
        }
    }
    return expected;
}

auto runNullTail(unsigned const pulseFrame, float const delay, unsigned const preFrames,
    unsigned const n, unsigned const chunkFrames, int const sourceControl) -> Rendered
{
    EchoKit kit;
    if(!kit.init(pulseFrame, delay))
        return {};

    kit.world.render(static_cast<ALsizei>(preFrames));
    if(alGetError() != AL_NO_ERROR)
        return {};

    alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT,
        static_cast<ALint>(n));
    alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
    if(alGetError() != AL_NO_ERROR)
        return {};

    if(sourceControl == 0)
        alSourceStop(kit.source);
    else if(sourceControl == 1)
        alSourcePause(kit.source);
    else
        alSource3i(kit.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0,
            AL_FILTER_NULL);

    Rendered result{true, {}};
    auto done = 0u;
    while(done < n)
    {
        auto const todo = (chunkFrames == 0) ? (n - done)
            : std::min(n - done, chunkFrames);
        auto part = kit.world.render(static_cast<ALsizei>(todo));
        result.samples.insert(result.samples.end(), part.begin(), part.end());
        done += todo;
    }
    result.ok = result.samples.size() == static_cast<std::size_t>(n) * 2_uz;
    return result;
}

auto peakFrame(std::vector<float> const &samples) -> std::pair<unsigned, float>
{
    auto frame = 0u;
    auto magnitude = 0.0f;
    for(auto i = 0_uz; i + 1_uz < samples.size(); i += 2_uz)
    {
        auto const m = std::max(std::fabs(samples[i]), std::fabs(samples[i + 1]));
        if(m > magnitude)
        {
            magnitude = m;
            frame = static_cast<unsigned>(i / 2_uz);
        }
    }
    return {frame, magnitude};
}

static int handoff_null_reference(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 512u;
    constexpr auto Pulse = 1900u;
    constexpr auto Delay = 0.01f;

    auto reference = echoZeroReference(Pulse, Delay, Pre, N);
    TASK_CHECK(reference.ok);

    auto actual = runNullTail(Pulse, Delay, Pre, N, 0, 0);
    TASK_CHECK(actual.ok);
    TASK_CHECK(actual.samples.size() == static_cast<std::size_t>(N) * 2_uz);

    auto expected = weightedNullExpectation(reference, N);
    auto const diff = maxAbsDifference(actual.samples, expected);
    std::fprintf(stderr, "handoff/null maxdiff=%.9g\n", diff);
    TASK_CHECK(std::isfinite(diff));
    TASK_CHECK(diff < 2.0e-6f);

    auto const peak = peakFrame(actual.samples);
    TASK_CHECK(peak.second > 0.05f);
    TASK_CHECK(peak.first == Pulse + 480u - Pre);
    return 0;
}

static int handoff_unconsumed_replacements(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 512u;
    constexpr auto Pulse = 1900u;

    auto reference = echoZeroReference(Pulse, 0.01f, Pre, N);
    TASK_CHECK(reference.ok);
    auto expected = weightedNullExpectation(reference, N);

    auto actual = Rendered{};
    {
        EchoKit kit;
        TASK_CHECK(kit.init(Pulse, 0.01f));
        kit.world.render(Pre);

        ALuint reverb{}, distortion{};
        alGenEffects(1, &reverb);
        alGenEffects(1, &distortion);
        alEffecti(reverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        alEffecti(distortion, AL_EFFECT_TYPE, AL_EFFECT_DISTORTION);
        TASK_CHECK(alGetError() == AL_NO_ERROR);

        /* No mix occurs between any of these effect handoffs. */
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, N);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, reverb);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, distortion);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
        alDeleteEffects(1, &distortion);
        alDeleteEffects(1, &reverb);

        alSourceStop(kit.source);
        actual.samples = kit.world.render(N);
        actual.ok = true;
    }

    auto const diff = maxAbsDifference(actual.samples, expected);
    std::fprintf(stderr, "handoff/unconsumed maxdiff=%.9g\n", diff);
    TASK_CHECK(diff < 2.0e-6f);
    return 0;
}

static int handoff_old_tail_plus_new_attack(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 512u;
    constexpr auto Post = 256u;

    auto oldReference = echoZeroReference(1900, 0.01f, Pre, Post);
    TASK_CHECK(oldReference.ok);

    auto newReference = Rendered{};
    {
        EchoKit kit;
        TASK_CHECK(kit.init(1900, 0.01f, 0.25f, 0.0f, 4096, 2100));
        ALuint reverb{};
        alGenEffects(1, &reverb);
        alEffecti(reverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        kit.world.render(Pre);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, reverb);
        newReference.samples = kit.world.render(Post);
        newReference.ok = true;
        alDeleteEffects(1, &reverb);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
    }

    auto actual = Rendered{};
    {
        EchoKit kit;
        TASK_CHECK(kit.init(1900, 0.01f, 0.25f, 0.0f, 4096, 2100));
        ALuint reverb{};
        alGenEffects(1, &reverb);
        alEffecti(reverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
        kit.world.render(Pre);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, N);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, reverb);
        actual.samples = kit.world.render(Post);
        actual.ok = true;
        alDeleteEffects(1, &reverb);
        TASK_CHECK(alGetError() == AL_NO_ERROR);
    }

    TASK_CHECK(newReference.ok && actual.ok);
    auto expected = std::vector<float>(static_cast<std::size_t>(Post) * 2_uz);
    for(auto frame = 0u; frame < Post; ++frame)
    {
        auto const weight = static_cast<float>(N - 1u - frame) / static_cast<float>(N);
        for(auto ch = 0_uz; ch < 2_uz; ++ch)
        {
            auto const idx = static_cast<std::size_t>(frame) * 2_uz + ch;
            expected[idx] = newReference.samples[idx]
                + oldReference.samples[idx] * weight;
        }
    }

    auto const diff = maxAbsDifference(actual.samples, expected);
    std::fprintf(stderr, "handoff/old+new maxdiff=%.9g\n", diff);
    TASK_CHECK(diff < 5.0e-5f);
    return 0;
}

struct GenerationKit {
    World world;
    ALuint buffer{}, source{}, slot{}, filter{};
    ALuint echoA{}, reverbB{}, echoC{};
    bool ok{};

    static constexpr unsigned Frames = 8192;
    static constexpr unsigned Pre = 2048;
    static constexpr unsigned L = 128;
    static constexpr unsigned M = 384;
    static constexpr unsigned N = 1024;

    auto init() -> bool
    {
        if(!world.ok)
            return false;
        alGenBuffers(1, &buffer);
        alGenSources(1, &source);
        alGenAuxiliaryEffectSlots(1, &slot);
        alGenFilters(1, &filter);
        alGenEffects(1, &echoA);
        alGenEffects(1, &reverbB);
        alGenEffects(1, &echoC);

        auto input = std::vector<float>(Frames, 0.0f);
        input[1900] = 0.25f; /* A's pulse: delayed tap lands after t2. */
        input[2100] = 0.22f; /* Excites B during its short live interval. */
        input[2208] = 0.25f; /* C's pulse: delayed tap after t2. */
        alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(),
            static_cast<ALsizei>(input.size() * sizeof(float)), Frequency);

        alEffecti(echoA, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
        alEffectf(echoA, AL_ECHO_DELAY, 0.01f);
        alEffectf(echoA, AL_ECHO_LRDELAY, 0.0f);
        alEffectf(echoA, AL_ECHO_DAMPING, 0.0f);
        alEffectf(echoA, AL_ECHO_FEEDBACK, 0.0f);
        alEffectf(echoA, AL_ECHO_SPREAD, 0.0f);

        alEffecti(reverbB, AL_EFFECT_TYPE, AL_EFFECT_REVERB);

        alEffecti(echoC, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
        alEffectf(echoC, AL_ECHO_DELAY, 0.003f);
        alEffectf(echoC, AL_ECHO_LRDELAY, 0.0f);
        alEffectf(echoC, AL_ECHO_DAMPING, 0.0f);
        alEffectf(echoC, AL_ECHO_FEEDBACK, 0.0f);
        alEffectf(echoC, AL_ECHO_SPREAD, 0.0f);

        alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        alFilterf(filter, AL_LOWPASS_GAIN, 0.0f);
        alSourcei(source, AL_DIRECT_FILTER, filter);
        alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot, 0, AL_FILTER_NULL);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(source, AL_BUFFER, buffer);
        alSourcePlay(source);

        ok = alGetError() == AL_NO_ERROR;
        return ok;
    }

    ~GenerationKit()
    {
        if(world.context)
            alcMakeContextCurrent(world.context);
        if(source)
        {
            alSourceStop(source);
            alDeleteSources(1, &source);
        }
        if(buffer) alDeleteBuffers(1, &buffer);
        if(filter) alDeleteFilters(1, &filter);
        if(slot) alDeleteAuxiliaryEffectSlots(1, &slot);
        if(echoA) alDeleteEffects(1, &echoA);
        if(reverbB) alDeleteEffects(1, &reverbB);
        if(echoC) alDeleteEffects(1, &echoC);
    }
};

static int handoff_two_generations_and_eviction(void)
{
    auto actual = Rendered{};
    {
        GenerationKit kit;
        TASK_CHECK(kit.init());
        kit.world.render(GenerationKit::Pre);

        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT,
            GenerationKit::N);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, kit.reverbB);
        kit.world.render(GenerationKit::L);

        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, kit.echoC);
        kit.world.render(GenerationKit::L);

        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
        actual.samples = kit.world.render(GenerationKit::M);
        actual.ok = true;
        TASK_CHECK(alGetError() == AL_NO_ERROR);
    }

    /* B reference: continue Reverb with zero input beginning at t1. */
    auto bReference = Rendered{};
    {
        GenerationKit kit;
        TASK_CHECK(kit.init());
        kit.world.render(GenerationKit::Pre);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, kit.reverbB);
        kit.world.render(GenerationKit::L);
        alSourceStop(kit.source);
        bReference.samples = kit.world.render(GenerationKit::L + GenerationKit::M);
        bReference.ok = true;
    }

    /* C reference: continue the new Echo with zero input beginning at t2. */
    auto cReference = Rendered{};
    {
        GenerationKit kit;
        TASK_CHECK(kit.init());
        kit.world.render(GenerationKit::Pre);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, kit.reverbB);
        kit.world.render(GenerationKit::L);
        alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, kit.echoC);
        kit.world.render(GenerationKit::L);
        alSourceStop(kit.source);
        cReference.samples = kit.world.render(GenerationKit::M);
        cReference.ok = true;
    }

    auto expectedBC = std::vector<float>(static_cast<std::size_t>(GenerationKit::M) * 2_uz);
    auto expectedABC = expectedBC;
    auto aReference = echoZeroReference(1900, 0.01f, GenerationKit::Pre,
        2u * GenerationKit::L + GenerationKit::M);
    TASK_CHECK(aReference.ok);

    for(auto j = 0u; j < GenerationKit::M; ++j)
    {
        auto const weightB = static_cast<float>(GenerationKit::N - 1u
            - GenerationKit::L - j) / static_cast<float>(GenerationKit::N);
        auto const weightC = static_cast<float>(GenerationKit::N - 1u - j)
            / static_cast<float>(GenerationKit::N);
        auto const weightA = static_cast<float>(GenerationKit::N - 1u
            - 2u * GenerationKit::L - j) / static_cast<float>(GenerationKit::N);
        for(auto ch = 0_uz; ch < 2_uz; ++ch)
        {
            auto const outIdx = static_cast<std::size_t>(j) * 2_uz + ch;
            auto const bIdx = static_cast<std::size_t>(GenerationKit::L + j) * 2_uz + ch;
            expectedBC[outIdx] = bReference.samples[bIdx] * weightB
                + cReference.samples[outIdx] * weightC;
            auto const aIdx = static_cast<std::size_t>(2u * GenerationKit::L + j) * 2_uz + ch;
            expectedABC[outIdx] = expectedBC[outIdx]
                + aReference.samples[aIdx] * weightA;
        }
    }

    auto const diffBC = maxAbsDifference(actual.samples, expectedBC);
    auto const diffABC = maxAbsDifference(actual.samples, expectedABC);
    std::fprintf(stderr, "handoff/generations BC=%.9g ABC=%.9g\n", diffBC, diffABC);
    TASK_CHECK(diffBC < 1.0e-4f);
    TASK_CHECK(diffABC > 0.01f);
    return 0;
}

static int clock_chunk_invariance(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 1028u; /* 4 * 257, and not a multiple of 7. */
    constexpr auto Pulse = 2010u;
    constexpr auto Delay = 0.002f;
    constexpr auto DelayFrames = 96u;

    auto reference = echoZeroReference(Pulse, Delay, Pre, N);
    TASK_CHECK(reference.ok);
    auto expected = weightedNullExpectation(reference, N);

    for(auto const chunk : {0u, 1u, 7u, 257u})
    {
        auto actual = runNullTail(Pulse, Delay, Pre, N, chunk, 0);
        TASK_CHECK(actual.ok);
        auto const diff = maxAbsDifference(actual.samples, expected);
        std::fprintf(stderr, "clock/chunks chunk=%u maxdiff=%.9g\n", chunk, diff);
        TASK_CHECK(diff < 5.0e-5f);

        if(chunk == 0)
        {
            auto const peak = peakFrame(actual.samples);
            auto const expectedArrival = Pulse + DelayFrames - Pre;
            std::fprintf(stderr, "clock/delay peak=%u expected=%u mag=%g\n",
                peak.first, expectedArrival, peak.second);
            TASK_CHECK(peak.second > 0.02f);
            TASK_CHECK(peak.first == expectedArrival);
        }
    }
    return 0;
}

static int clock_budget_change_does_not_recompute_exit(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 512u;
    constexpr auto First = 64u;

    auto reference = echoZeroReference(1900, 0.01f, Pre, N);
    TASK_CHECK(reference.ok);
    auto expected = weightedNullExpectation(reference, N);

    EchoKit kit;
    TASK_CHECK(kit.init(1900, 0.01f));
    kit.world.render(Pre);
    alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, N);
    alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
    alSourceStop(kit.source);

    auto actual = kit.world.render(First);
    /* This publication reaches the same live Null instance. The already
     * retiring Echo generation must retain its original N and progress.
     */
    alAuxiliaryEffectSloti(kit.slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 1);
    auto rest = kit.world.render(N - First);
    actual.insert(actual.end(), rest.begin(), rest.end());
    TASK_CHECK(actual.size() == expected.size());

    auto const diff = maxAbsDifference(actual, expected);
    TASK_CHECK(diff < 2.0e-6f);
    return 0;
}

static int lifetime_source_controls(void)
{
    constexpr auto Pre = 2048u;
    constexpr auto N = 512u;
    constexpr auto Pulse = 1900u;

    auto reference = echoZeroReference(Pulse, 0.01f, Pre, N);
    TASK_CHECK(reference.ok);
    auto expected = weightedNullExpectation(reference, N);

    for(auto control = 0; control < 3; ++control)
    {
        auto actual = runNullTail(Pulse, 0.01f, Pre, N, 37u, control);
        TASK_CHECK(actual.ok);
        auto const diff = maxAbsDifference(actual.samples, expected);
        std::fprintf(stderr, "lifetime/source-control %d maxdiff=%.9g\n", control, diff);
        TASK_CHECK(diff < 2.0e-5f);
    }
    return 0;
}

static int lifetime_delete_and_reuse(void)
{
    World world;
    TASK_CHECK(world.ok);

    ALuint buffer{}, source{}, echo{}, slot{}, filter{};
    alGenBuffers(1, &buffer);
    alGenSources(1, &source);
    alGenEffects(1, &echo);
    alGenAuxiliaryEffectSlots(1, &slot);
    alGenFilters(1, &filter);

    auto input = std::vector<float>(4096, 0.0f);
    input[1900] = 0.3f;
    alBufferData(buffer, AL_FORMAT_MONO_FLOAT32, input.data(), 4096 * sizeof(float),
        Frequency);
    alEffecti(echo, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    alEffectf(echo, AL_ECHO_DELAY, 0.01f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, echo);
    alFilteri(filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(filter, AL_LOWPASS_GAIN, 0.0f);
    alSourcei(source, AL_DIRECT_FILTER, filter);
    alSource3i(source, AL_AUXILIARY_SEND_FILTER, slot, 0, AL_FILTER_NULL);
    alSourcei(source, AL_BUFFER, buffer);
    alSourcePlay(source);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    world.render(2048);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 512);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
    auto dying = world.render(384);
    TASK_CHECK(peakFrame(dying).second > 0.02f);

    /* Dereference the slot before deleting it, as required by the API. */
    alSource3i(source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    alSourceStop(source);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    ALuint reused{};
    alGenAuxiliaryEffectSlots(1, &reused);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_CHECK(reused == slot);

    auto silent = world.render(256);
    auto silencePeak = peakFrame(silent).second;
    TASK_CHECK(silencePeak <= 1.0e-8f);

    /* Exercising the reused bank must not resurrect the deleted generation. */
    alAuxiliaryEffectSloti(reused, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 128);
    alAuxiliaryEffectSloti(reused, AL_EFFECTSLOT_EFFECT, echo);
    auto stillSilent = world.render(128);
    TASK_CHECK(peakFrame(stillSilent).second <= 1.0e-8f);

    alDeleteAuxiliaryEffectSlots(1, &reused);
    alDeleteEffects(1, &echo);
    alDeleteFilters(1, &filter);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    return 0;
}

static int lifetime_invalid_budget(void)
{
    TASK_CHECK(alGetEnumValue("AL_EFFECTSLOT_TAIL_SAMPLES_SOFT")
        == AL_EFFECTSLOT_TAIL_SAMPLES_SOFT);

    World world;
    TASK_CHECK(world.ok);

    ALuint slot{}, target{};
    alGenAuxiliaryEffectSlots(1, &slot);
    alGenAuxiliaryEffectSlots(1, &target);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    ALint value = -123;
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    TASK_CHECK(value == 0);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 12345);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(value == 12345);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, -1);
    TASK_CHECK(alGetError() == AL_INVALID_VALUE);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(value == 12345);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT,
        10 * Frequency + 1);
    TASK_CHECK(alGetError() == AL_INVALID_VALUE);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(value == 12345);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 10 * Frequency);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 12345);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    /* The mandated validation is enabling a non-zero budget while a target is
     * current. Target assignment itself keeps its original behavior.
     */
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TARGET_SOFT, static_cast<ALint>(target));
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TARGET_SOFT, AL_EFFECTSLOT_NULL);
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 0);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TARGET_SOFT, static_cast<ALint>(target));
    TASK_CHECK(alGetError() == AL_NO_ERROR);

    /* Targeted slots cannot enable a non-zero budget either. */
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, 17);
    TASK_CHECK(alGetError() == AL_INVALID_OPERATION);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(value == 0);

    ALint const vectorValue[1] = {99};
    alAuxiliaryEffectSlotiv(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, vectorValue);
    TASK_CHECK(alGetError() == AL_INVALID_OPERATION);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TAIL_SAMPLES_SOFT, &value);
    TASK_CHECK(value == 0);

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_TARGET_SOFT, AL_EFFECTSLOT_NULL);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteAuxiliaryEffectSlots(1, &target);
    TASK_CHECK(alGetError() == AL_NO_ERROR);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"handoff", "null_reference", handoff_null_reference},
    {"handoff", "old_tail_plus_new_attack", handoff_old_tail_plus_new_attack},
    {"handoff", "unconsumed_replacements", handoff_unconsumed_replacements},
    {"handoff", "two_generations_eviction", handoff_two_generations_and_eviction},
    {"clock", "chunks_1_7_257", clock_chunk_invariance},
    {"clock", "budget_change_keeps_exit", clock_budget_change_does_not_recompute_exit},
    {"lifetime", "source_pause_stop_detach", lifetime_source_controls},
    {"lifetime", "delete_and_reuse", lifetime_delete_and_reuse},
    {"lifetime", "invalid_budget_preserves_state", lifetime_invalid_budget},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
