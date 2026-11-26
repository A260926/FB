#ifndef CORE_EFFECTSLOTTAIL_H
#define CORE_EFFECTSLOTTAIL_H

#include <array>
#include <cstddef>
#include <span>

#include "ambidefs.h"
#include "bufferline.h"
#include "opthelper_macros.hpp"

struct EffectState;

inline constexpr std::size_t EffectSlotTailMaxGenerations{2};

/* Transfers one owned EffectState reference to the non-realtime release path.
 * Returns false when the release queue is full; the caller retains ownership in
 * that case.
 */
struct EffectSlotTailReleaseSink {
    void *Context;
    bool (*Release)(void *context, EffectState *state) noexcept NONBLOCKING;

    auto operator()(EffectState *state) const noexcept NONBLOCKING -> bool
    { return Release(Context, state); }
};

struct EffectSlotTailGeneration {
    /* Owns one reference transferred out of EffectSlotBase::mEffectState. */
    EffectState *State{};
    unsigned Budget{};
    unsigned Position{};
    bool ReleasePending{};
};

/* Retired effect states belonging to one output-to-device effect slot. The
 * storage is fixed-size so the mixer never allocates while handing off states.
 */
struct EffectSlotTailBank {
    std::array<EffectSlotTailGeneration, EffectSlotTailMaxGenerations> Generations{};
    bool CurrentProcessed{};

    /* Hands oldState off as a retiring generation when appropriate. Returns
     * true when the bank owns oldState. On a full bank, evicted receives the
     * owned reference of the oldest generation and the caller must release it
     * through the existing non-realtime event/free-list path.
     */
    [[nodiscard]]
    auto handoff(EffectState *oldState, unsigned budget, EffectState *&evicted) noexcept
        NONBLOCKING -> bool;

    /* Renders all retiring generations with silent input. States which finish
     * are offered to releaseSink; a failed offer keeps the non-outputting
     * generation resident until a later block.
     */
    void render(unsigned frames, std::span<const FloatBufferLine> zeroInput,
        std::span<FloatBufferLine> scratch, EffectSlotTailReleaseSink releaseSink) noexcept
        NONBLOCKING;

    /* Control-thread teardown after the mixer has been waited out. */
    void clearNow() noexcept;

    void noteCurrentProcessed() noexcept { CurrentProcessed = true; }
};

/* Reusable mixer scratch storage. Keeping this on ContextBase avoids both
 * audio-thread allocation and a large stack allocation on every mix.
 */
struct EffectSlotTailWorkspace {
    alignas(16) std::array<FloatBufferLine, MaxAmbiChannels> ZeroInput{};
    alignas(16) std::array<FloatBufferLine, MaxAmbiChannels> Scratch{};
};

#endif /* CORE_EFFECTSLOTTAIL_H */
