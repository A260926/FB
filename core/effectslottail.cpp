#include "effectslottail.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "core/effects/base.h"
#include "intrusive_ptr.h"

namespace {

void CompactGenerations(EffectSlotTailBank &bank) noexcept
{
    auto write = std::size_t{0};
    for(auto read = std::size_t{0}; read < EffectSlotTailMaxGenerations; ++read)
    {
        if(bank.Generations[read].State)
        {
            if(write != read)
                bank.Generations[write] = bank.Generations[read];
            ++write;
        }
    }
    for(; write < EffectSlotTailMaxGenerations; ++write)
        bank.Generations[write] = EffectSlotTailGeneration{};
}

} // namespace

auto EffectSlotTailBank::handoff(EffectState *const oldState, unsigned const budget,
    EffectState *&evicted) noexcept NONBLOCKING -> bool
{
    evicted = nullptr;

    /* The new state owns live input from this point onward. This marker only
     * applies to the instance being replaced below.
     */
    auto const wasProcessed = std::exchange(CurrentProcessed, false);
    if(!wasProcessed || budget == 0 || !oldState)
        return false;

    CompactGenerations(*this);

    auto count = std::size_t{0};
    while(count < EffectSlotTailMaxGenerations && Generations[count].State)
        ++count;

    if(count == EffectSlotTailMaxGenerations)
    {
        /* The third retiring generation cancels the oldest one. Its owned
         * reference goes back to the mixer's non-realtime release path.
         */
        evicted = Generations[0].State;
        Generations[0] = Generations[1];
        Generations[1] = EffectSlotTailGeneration{oldState, budget, 0, false};
    }
    else
        Generations[count] = EffectSlotTailGeneration{oldState, budget, 0, false};

    return true;
}

void EffectSlotTailBank::render(unsigned const frames,
    std::span<const FloatBufferLine> const zeroInput,
    std::span<FloatBufferLine> const scratch,
    EffectSlotTailReleaseSink const releaseSink) noexcept NONBLOCKING
{
    if(frames == 0) [[unlikely]]
        return;

    for(auto &generation : Generations)
    {
        if(!generation.State)
            continue;

        if(generation.ReleasePending || generation.Position >= generation.Budget)
        {
            if(releaseSink(generation.State))
                generation = EffectSlotTailGeneration{};
            else
                generation.ReleasePending = true;
            continue;
        }

        auto *const state = generation.State;
        auto const outputChannels = std::min(state->mOutTarget.size(), scratch.size());
        auto const blockFrames = std::min(frames, generation.Budget - generation.Position);
        auto done = unsigned{0};
        while(done < blockFrames)
        {
            auto const todo = std::min<unsigned>(blockFrames - done, BufferLineSize);
            auto const output = scratch.first(outputChannels);

            for(auto &line : output)
                std::fill_n(line.begin(), todo, 0.0f);

            /* Retired states receive only silence. Their parameters, gain
             * ramp state, and delay/history were deliberately left untouched
             * at handoff.
             */
            state->process(todo, zeroInput, output);

            for(auto channel = std::size_t{0}; channel < outputChannels; ++channel)
            {
                auto &destination = state->mOutTarget[channel];
                auto const &source = output[channel];
                for(auto i = unsigned{0}; i < todo; ++i)
                {
                    auto const frame = generation.Position + done + i;
                    auto const fade = static_cast<float>(generation.Budget - 1u - frame)
                        / static_cast<float>(generation.Budget);
                    destination[i] += source[i] * fade;
                }
            }

            done += todo;
        }

        generation.Position += done;
        if(generation.Position >= generation.Budget)
        {
            if(releaseSink(state))
                generation = EffectSlotTailGeneration{};
            else
                generation.ReleasePending = true;
        }
    }

    CompactGenerations(*this);
}

void EffectSlotTailBank::clearNow() noexcept
{
    for(auto &generation : Generations)
    {
        if(generation.State)
        {
            /* Control thread teardown may release heap references directly;
             * this is not the audio mixer's realtime path.
             */
            auto owner = al::intrusive_ptr<EffectState>{generation.State};
            generation.State = nullptr;
        }
        generation = EffectSlotTailGeneration{};
    }
    CurrentProcessed = false;
}
