#ifndef CORE_EFFECTSLOTTAIL_H
#define CORE_EFFECTSLOTTAIL_H

#include <array>
#include <cstddef>
#include <span>

#include "bufferline.h"
#include "effects/base.h"
#include "intrusive_ptr.h"
#include "opthelper_macros.hpp"
#include "vector.h"

struct ContextBase;
struct EffectSlotBase;


/* A single retired EffectState generation. After the effect slot switches to
 * a different EffectState instance, the old instance keeps rendering its own
 * history with zero input for a fixed number of device output frames, its
 * output fading out linearly.
 */
struct EffectTailGeneration {
    /* The retired effect state, keeping its history, parameters and gains
     * frozen at the time of retirement.
     */
    al::intrusive_ptr<EffectState> mState;

    /* Private scratch buffer the retired state writes into. Its layout matches
     * the state's output target at the time of retirement.
     */
    al::vector<FloatBufferLine,16> mOutput;

    /* The output target the state had when it was retired, into which the
     * faded tail is added.
     */
    std::span<FloatBufferLine> mDestination;

    /* The tail budget captured from the property snapshot that retired the
     * state, in device output frames.
     */
    std::size_t mBudget{0};

    /* Number of device output frames already rendered since retirement. */
    std::size_t mElapsed{0};
};


/* Manages the retired effect state generations for one EffectSlotBase.
 *
 * The control thread owns the storage lifecycle: scratch buffers are
 * pre-allocated with ensureBuffers before any retirement can happen, and
 * generations are only cancelled/destroyed from control threads once the mixer
 * is quiesced. The mixer thread uses only allocation-free, non-blocking
 * methods.
 */
class EffectSlotTail {
public:
    /* At most two retired generations may be audible at the same time. */
    static constexpr std::size_t MaxGenerations = 2;

    EffectSlotTail() = default;
    EffectSlotTail(const EffectSlotTail&) = delete;
    auto operator=(const EffectSlotTail&) -> EffectSlotTail& = delete;
    ~EffectSlotTail();

    /* Control-thread allocation of the shared zero-input buffer (wetChannels
     * lines) and each generation's private output scratch (outChannels lines).
     * Growing an already-sized manager only happens while no generation is
     * active, so the mixer never observes a reallocation.
     */
    void ensureBuffers(std::size_t wetChannels, std::size_t outChannels);

    /* Cancels every retained generation, releasing the states on the calling
     * (control) thread. Only valid while the mixer is not accessing the slot.
     */
    void cancelAll() noexcept;

    /* Mixer-side retirement of the slot's previous state, of which the caller
     * transfers one reference. Returns true if the state was retained as a
     * tail generation; on false the caller retains ownership and must handle
     * the state's release itself. If a third generation arrives, the oldest
     * generation is evicted through the context's asynchronous release path.
     */
    [[nodiscard]] auto retire(ContextBase &context, EffectState *oldstate,
        std::size_t budget) noexcept NONBLOCKING -> bool;

    /* Mixer-side rendering of one device update: every retained generation is
     * fed zero input, its output is multiplied by the linear retirement fade
     * and added to the generation's captured output destination.
     */
    void process(std::size_t samplesToDo, ContextBase &context) noexcept NONBLOCKING;

    [[nodiscard]] auto empty() const noexcept -> bool { return mCount == 0; }

private:
    /* Sends one state reference to the context's asynchronous release event
     * queue without freeing it on the mixer thread.
     */
    [[nodiscard]] static auto queueRelease(ContextBase &context,
        EffectState *state) noexcept -> bool;

    std::array<EffectTailGeneration,MaxGenerations> mGenerations{};

    /* Read-only zero input shared by every generation. */
    al::vector<FloatBufferLine,16> mZeroInput;

    std::size_t mWetChannels{0};
    std::size_t mOutChannels{0};
    std::size_t mCount{0};
};

#endif /* CORE_EFFECTSLOTTAIL_H */
