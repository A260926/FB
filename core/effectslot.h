#ifndef CORE_EFFECTSLOT_H
#define CORE_EFFECTSLOT_H

#include <atomic>
#include <memory>

#include "effects/base.h"
#include "effectslottail.h"
#include "flexarray.h"
#include "intrusive_ptr.h"
#include "mixparams.hpp"
#include "vector.h"

struct EffectSlotBase;
struct WetBuffer;

using EffectSlotArray = al::FlexArray<EffectSlotBase*>;


enum class EffectSlotType : unsigned char {
    None,
    Reverb,
    Chorus,
    Autowah,
    Compressor,
    Convolution,
    Dedicated,
    Distortion,
    Echo,
    Equalizer,
    Flanger,
    FrequencyShifter,
    PitchShifter,
    RingModulator,
    VocalMorpher,
};

struct EffectSlotProps {
    float Gain;
    bool  AuxSendAuto;
    EffectSlotBase *Target;

    EffectSlotType Type;
    EffectProps Props;

    al::intrusive_ptr<EffectState> State;

    /* Number of device output frames a replaced EffectState keeps fading out
     * for. 0 keeps the original replacement behavior.
     */
    std::size_t TailBudget;

    std::atomic<EffectSlotProps*> next;
};


struct EffectSlotBase {
    bool InUse{false};

    std::atomic<EffectSlotProps*> Update{nullptr};

    /* Wet buffer configuration is ACN channel order with N3D scaling.
     * Consequently, effects that only want to work with mono input can use
     * channel 0 by itself. Effects that want multichannel can process the
     * ambisonics signal and make a B-Format source pan.
     */
    MixParams Wet;

    float Gain{1.0f};
    bool  AuxSendAuto{true};
    EffectSlotBase *Target{nullptr};

    EffectSlotType EffectType{EffectSlotType::None};
    EffectProps mEffectProps;
    al::intrusive_ptr<EffectState> mEffectState;

    /* Device output frames a replaced effect state keeps fading out for, as
     * published by the last property update.
     */
    std::size_t TailBudget{0};

    /* Whether the current EffectState instance has actually run on the mixer.
     * A state replaced before it ever processed audio makes no sound and must
     * not be retained as a tail generation. Only touched by the mixer thread.
     */
    bool mStateProcessed{false};

    /* Retired effect state generations still rendering their tails. */
    EffectSlotTail mTail;

    float RoomRolloff{0.0f}; /* Added to the source's room rolloff, not multiplied. */
    float DecayTime{0.0f};
    float DecayLFRatio{0.0f};
    float DecayHFRatio{0.0f};
    bool DecayHFLimit{false};
    float AirAbsorptionGainHF{1.0f};

    /* Mixing buffer used by the Wet mix. */
    al::vector<FloatBufferLine,16> mWetBuffer;


    static std::unique_ptr<EffectSlotArray> CreatePtrArray(size_t count);
};

#endif /* CORE_EFFECTSLOT_H */
