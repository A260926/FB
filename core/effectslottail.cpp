
#include "config.h"

#include "effectslottail.h"

#include <algorithm>
#include <ranges>
#include <span>
#include <utility>

#include "async_event.h"
#include "context.h"
#include "effects/base.h"
#include "opthelper_macros.hpp"


EffectSlotTail::~EffectSlotTail()
{
    /* Context destruction guarantees the mixer is no longer accessing the slot
     * before the EffectSlotBase (and this manager) goes away, but make sure no
     * state reference leaks regardless.
     */
    cancelAll();
}

void EffectSlotTail::ensureBuffers(std::size_t const wetChannels, std::size_t const outChannels)
{
    if(wetChannels == 0 || outChannels == 0)
        return;

    if(mZeroInput.size() != wetChannels)
    {
        mZeroInput.resize(wetChannels);
        std::ranges::for_each(mZeroInput, [](FloatBufferLine &line) noexcept { line.fill(0.0f); });
    }

    for(EffectTailGeneration &gen : mGenerations)
    {
        if(gen.mOutput.size() != outChannels)
            gen.mOutput.resize(outChannels);
    }

    mWetChannels = wetChannels;
    mOutChannels = outChannels;
}

void EffectSlotTail::cancelAll() noexcept
{
    for(std::size_t i{0};i < mCount;++i)
    {
        if(EffectState *state = mGenerations[i].mState.release())
            state->dec_ref();
        mGenerations[i].mDestination = {};
        mGenerations[i].mBudget = 0;
        mGenerations[i].mElapsed = 0;
    }
    mCount = 0;
}

auto EffectSlotTail::queueRelease(ContextBase &context, EffectState *state) noexcept
    NONBLOCKING -> bool
{
    auto *const ring = context.mAsyncEvents.get();
    if(auto const evt_vec = ring->getWriteVector(); !evt_vec[0].empty())
    {
        auto &evt = InitAsyncEvent<AsyncEffectReleaseEvent>(evt_vec[0].front());
        evt.mEffectState = state;
        ring->writeAdvance(1);
        return true;
    }
    return false;
}

auto EffectSlotTail::retire(ContextBase &context, EffectState *oldstate,
    std::size_t const budget) noexcept NONBLOCKING -> bool
{
    if(budget == 0 || mWetChannels == 0 || mOutChannels == 0)
        return false;

    std::size_t slot{mCount};
    if(slot >= MaxGenerations)
    {
        /* A third generation arrives: cancel the oldest one. The other
         * generation keeps its progress when the slots are swapped.
         */
        EffectTailGeneration &oldest = mGenerations[0];
        EffectState *evicted = oldest.mState.release();
        if(!evicted->releaseIfNoDelete() && !queueRelease(context, evicted))
        {
            /* The release queue is full; keep the old generation and leave the
             * caller to handle the would-be retired state normally.
             */
            IGNORE_FUNCTION_EFFECTS(oldest.mState.reset(evicted));
            return false;
        }
        oldest.mDestination = {};
        oldest.mBudget = 0;
        oldest.mElapsed = 0;
        IGNORE_FUNCTION_EFFECTS(std::swap(mGenerations[0], mGenerations[1]));
        slot = MaxGenerations - 1;
    }
    else
        ++mCount;

    EffectTailGeneration &gen = mGenerations[slot];
    /* Takes over the caller's reference without changing the refcount. */
    IGNORE_FUNCTION_EFFECTS(gen.mState.reset(oldstate));
    gen.mBudget = budget;
    gen.mElapsed = 0;
    gen.mDestination = oldstate->mOutTarget;
    return true;
}

void EffectSlotTail::process(std::size_t const samplesToDo, ContextBase &context) noexcept
    NONBLOCKING
{
    if(mCount == 0)
        return;

    auto const zero_input = std::span<const FloatBufferLine>{mZeroInput}.first(mWetChannels);

    std::size_t active{0};
    while(active < mCount)
    {
        EffectTailGeneration &gen = mGenerations[active];

        auto const outchans = gen.mDestination.size();
        auto const outspan = std::span<FloatBufferLine>{gen.mOutput}.first(outchans);

        /* The retired state only ever receives zero input from now on. */
        std::ranges::for_each(outspan, [samplesToDo](FloatBufferLine &line) noexcept
        { std::ranges::fill(std::span{line}.first(samplesToDo), 0.0f); });
        gen.mState->process(samplesToDo, zero_input, outspan);

        auto const budget = gen.mBudget;
        auto const elapsed = gen.mElapsed;
        auto const todo = std::min(samplesToDo, budget - elapsed);
        auto const inv_budget = 1.0f / static_cast<float>(budget);

        /* Retirement frame k (from 0) is weighted by (N-1-k)/N and added to
         * the state's captured output destination.
         */
        for(std::size_t ch{0};ch < outchans;++ch)
        {
            FloatBufferLine const &srcline = gen.mOutput[ch];
            FloatBufferLine &dstline = gen.mDestination[ch];
            for(std::size_t j{0};j < todo;++j)
            {
                auto const k = elapsed + j;
                auto const weight = static_cast<float>(budget - 1 - k) * inv_budget;
                dstline[j] += srcline[j] * weight;
            }
        }
        gen.mElapsed = elapsed + todo;

        if(gen.mElapsed < budget)
        {
            ++active;
            continue;
        }

        /* Budget exhausted: hand the state to the non-real-time release
         * mechanism. Nothing is freed on the mixer thread; the scratch output
         * buffer stays allocated for a future retirement.
         */
        EffectState *state = gen.mState.release();
        if(!state->releaseIfNoDelete() && !queueRelease(context, state))
        {
            /* The release queue is full; keep the state and retry next update. */
            IGNORE_FUNCTION_EFFECTS(gen.mState.reset(state));
            ++active;
            continue;
        }
        gen.mDestination = {};
        gen.mBudget = 0;
        gen.mElapsed = 0;
        --mCount;
        if(active < mCount)
            IGNORE_FUNCTION_EFFECTS(std::swap(gen, mGenerations[mCount]));
    }
}
