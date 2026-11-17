#include "cal3d/compactutil.h"

#include <cmath>

uint32_t CalCompactUtil::zigzag(int32_t value) {
    if (value < 0) {
        return (-value) * 2 - 1;
    } else {
        return value * 2;
    }
}

int32_t CalCompactUtil::unzigzag(uint32_t value) {
    int isNegative = value & 0x1;
    int32_t magnitude = (value >> 1) + isNegative;
    return magnitude * (isNegative ? -1 : 1);
}

std::vector<uint32_t> CalCompactUtil::encodeDeltaZigzag(const std::vector<uint32_t>& data) {
    std::vector<uint32_t> outValues;
    outValues.reserve(data.size());

    uint32_t prevValue = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        uint32_t value = *it;

        // NOTE: Here we rely on the compiler wrapping out-of-range values as
        // we expect, even though it is undefined behavior:
        int32_t delta = value - prevValue;

        outValues.push_back(zigzag(delta));
        prevValue = value;
    }
    return outValues;
}

std::vector<uint32_t> CalCompactUtil::decodeDeltaZigzag(const std::vector<uint32_t>& data) {
    std::vector<uint32_t> outValues;
    outValues.reserve(data.size());

    uint32_t prevValue = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        int32_t delta = unzigzag(*it);

        // NOTE: Here we rely on the compiler wrapping out-of-range values as
        // we expect, even though it is undefined behavior:
        uint32_t value = prevValue + delta;

        outValues.push_back(value);
        prevValue = value;
    }
    return outValues;
}

std::vector<uint16_t> CalCompactUtil::encodeHighWaterMark(const std::vector<uint16_t>& data) {
    std::vector<uint16_t> outValues;
    outValues.reserve(data.size());

    uint32_t highValue = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        uint16_t value = *it;
        if (value > highValue) {
            throw std::runtime_error("out-of-range value in high water mark encoding");
        }
        outValues.push_back(static_cast<uint16_t>(highValue - static_cast<uint32_t>(value)));
        if (static_cast<uint32_t>(value) + 1 > highValue) {
            highValue = static_cast<uint32_t>(value) + 1;
        }
    }

    return outValues;
}

std::vector<uint16_t> CalCompactUtil::decodeHighWaterMark(const std::vector<uint16_t>& data) {
    std::vector<uint16_t> outValues;
    outValues.reserve(data.size());

    uint32_t highValue = 0;
    for (auto it = data.begin(); it != data.end(); ++it) {
        uint16_t value = *it;
        if (static_cast<uint32_t>(value) > highValue) {
            throw std::runtime_error("out-of-range value in high water mark decoding");
        }
        uint16_t recoveredValue = static_cast<uint16_t>(highValue - static_cast<uint32_t>(value));
        outValues.push_back(recoveredValue);
        if (static_cast<uint32_t>(recoveredValue) + 1 > highValue) {
            highValue = static_cast<uint32_t>(recoveredValue) + 1;
        }
    }

    return outValues;
}

// NOTE: C++11 has std::minmax_element()
template <typename T>
static void s_getMinMax(
    const std::vector<T>& inData,
    T& outMinimum,
    T& outMaximum
) {
    assert(inData.size() > 0);
    T minimum = inData[0];
    T maximum = inData[0];

    for (auto it = inData.begin(); it != inData.end(); ++it) {
        if (minimum > *it) {
            minimum = *it;
        }
        if (maximum < *it) {
            maximum = *it;
        }
    }

    outMinimum = minimum;
    outMaximum = maximum;
}

// NOTE: C++11 has std::log2().
static double s_log2(double value) {
    return std::log(value) / std::log(2.0);
}

CalQuantizedData CalCompactUtil::quantizeToPrecision(
    const std::vector<float>& data,
    float epsilon,
    unsigned int maxBits
) {
    // NOTE: Theoretically, we could encode a constant series with zero epsilon...
    assert(epsilon > 0);

    if (data.size() == 0) {
        CalQuantizedData outData;
        outData.minimum = 0.0f;
        outData.maximum = 0.0f;
        outData.maxQuantizedValue = 0;
        return outData;
    }

    float minimum;
    float maximum;
    s_getMinMax(data, minimum, maximum);
    float range = maximum - minimum;

    // TODO: We can actually do better than the following calculation if we do a
    // little more work and/or relax the restriction of filling the entire
    // range.
    //
    // For example, we can represent a list of values consisting of integers in
    // [1..5] exactly with three bits per entry, while the following calculation
    // will use many more bits in an attempt to fit the values to a power-of-two
    // number of increments in the 1..5 range.
    //
    // Likewise, the values [-0.5, 0] can be exactly expressed with one bit, but
    // the following code can allocate many more, depending on tolerance.
    //
    // Pre-quantized keyframe times and two-frame pose animations might
    // particularly benefit from better quantization.
    unsigned int bitCount = std::ceil(s_log2(1.0 + range / epsilon));

    if (bitCount > maxBits) {
        throw std::runtime_error("max bits exceeded in quantization");
    }

    CalQuantizedData outData;
    outData.minimum = minimum;
    outData.maximum = maximum;
    outData.maxQuantizedValue = (1 << bitCount) - 1;

    float scale = outData.maxQuantizedValue / range;
    for (auto it = data.begin(); it != data.end(); ++it) {
        outData.values.push_back((*it - minimum) * scale + 0.5f);
    }

    return outData;
}

CalQuantizedData CalCompactUtil::quantizeToBits(const std::vector<float>& data, unsigned int bitCount) {
    assert(bitCount <= 32);

    if (data.size() == 0) {
        CalQuantizedData outData;
        outData.minimum = 0.0f;
        outData.maximum = 0.0f;
        outData.maxQuantizedValue = 0;
        return outData;
    }

    float minimum;
    float maximum;
    s_getMinMax(data, minimum, maximum);
    float range = maximum - minimum;

    CalQuantizedData outData;
    outData.minimum = minimum;
    outData.maximum = maximum;
    outData.maxQuantizedValue = (static_cast<uint64_t>(1) << bitCount) - 1;

    float scale = outData.maxQuantizedValue / range;
    for (auto it = data.begin(); it != data.end(); ++it) {
        outData.values.push_back((*it - minimum) * scale + 0.5f);
    }

    return outData;
}

static int64_t s_divideAndRound(float numerator, float denominator) {
    // NOTE: There is potential for overflow in the cast from float to int64_t.
    return std::floor(numerator / denominator + 0.5f);
}

static float s_clamp(float value, float min, float max) {
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }

    return value;
}

// The intent of this function is to ensure that two different sets of values
// quantized to the same precision will quantize identical source values to
// identical destination values, so that e.g. quantization does not introduce
// seams between meshes that share vertex positions.
//
// Ideally, pick a precision value that does not induce unnecessary
// rounding errors. Dividing by two, for example, perhaps doesn't have
// as much error as dividing by 10.
CalQuantizedData CalCompactUtil::quantizeConsistent(
    const std::vector<float>& data,
    float epsilon,
    unsigned int maxBits
) {
    assert(epsilon > 0);
    assert(maxBits <= 32);

    std::vector<int64_t> roundedValues;
    roundedValues.reserve(data.size());
    for (auto it = data.begin(); it != data.end(); ++it) {
        roundedValues.push_back(s_divideAndRound(*it, epsilon));
    }

    if (data.size() == 0) {
        CalQuantizedData outData;
        outData.minimum = 0.0f;
        outData.maximum = 0.0f;
        outData.maxQuantizedValue = 0;
        return outData;
    }

    // TODO: perf - could compute min/max as part of previous pass
    int64_t roundedMinimum;
    int64_t roundedMaximum;
    s_getMinMax(roundedValues, roundedMinimum, roundedMaximum);
    uint64_t qRange = roundedMaximum - roundedMinimum;
    unsigned int bitCount = std::ceil(s_log2(qRange + 1));

    if (bitCount > maxBits) {
        // if the range is to big to be compressed, then clamp the original values
        int64_t maxValue = 1;
        maxValue = maxValue << (maxBits - 2); 
        float max = static_cast<double>(epsilon) * maxValue;
        float min = -max;
        for(size_t i=0; i<data.size(); ++i) {
            float v = data[i];
            v = s_clamp(v, min, max);
            roundedValues[i] = s_divideAndRound(v, epsilon);
        }

        s_getMinMax(roundedValues, roundedMinimum, roundedMaximum);
        qRange = roundedMaximum - roundedMinimum;
        bitCount = std::ceil(s_log2(qRange + 1));
    }
    
    if (bitCount > maxBits) {
        throw std::runtime_error("max bits exceeded in consistent quantization");
    }

    CalQuantizedData outData;
    // TODO: can probably compute this stuff with less passes/copying and with
    // more careful range checking.
    outData.values.reserve(data.size());
    for (auto it = roundedValues.begin(); it != roundedValues.end(); ++it) {
        // Probably no overflow here: if bitCount < 32, static cast should be
        // OK, and subtracting the minimum should be OK.
        outData.values.push_back(static_cast<uint32_t>(*it - roundedMinimum));
    }
    outData.maxQuantizedValue = static_cast<uint32_t>((static_cast<uint64_t>(1) << bitCount) - 1);
    outData.minimum = roundedMinimum * epsilon;
    outData.maximum = roundedMinimum * epsilon + outData.maxQuantizedValue * epsilon;

    return outData;
}

std::vector<float> CalCompactUtil::unquantize(const CalQuantizedData& data) {
    float minimum = data.minimum;
    float maximum = data.maximum;

    if (data.maxQuantizedValue == 0) {
        return std::vector<float>(data.values.size(), minimum);
    }

    std::vector<float> outData;
    outData.reserve(data.values.size());

    float maxQuantizedValue = data.maxQuantizedValue;
    for (auto it = data.values.begin(); it != data.values.end(); ++it) {
        uint32_t value = *it;
        float w = static_cast<float>(value) / maxQuantizedValue;
        outData.push_back(minimum * (1.0f - w) + maximum * w);
    }

    return outData;
}

template <typename OutValue, uint32_t OverflowMask>
static std::vector<OutValue> s_decodeMidi(const std::string& data) {
    std::vector<OutValue> result;

    // NOTE: Haven't put any work into figuring out if this is a good idea:
    result.reserve(data.size() / 2);

    auto it = data.begin();
    while (it != data.end()) {
        uint32_t value = 0U;
        while (true) {
            unsigned char ch = static_cast<unsigned char>(*(it++));
            if (value & OverflowMask) {
                throw std::runtime_error("midi value too big");
            }
            value = (value << 7) | (ch & 0x7f);
            if (!(ch & 0x80)) {
                break;
            }
            if (it == data.end()) {
                throw std::runtime_error("truncated midi data");
            }
        }
        result.push_back(value);
    }
    return result;
}

std::vector<uint32_t> CalCompactUtil::decodeMidi(const std::string& data) {
    return s_decodeMidi<uint32_t, 0xfe000000U>(data);
}

std::vector<uint16_t> CalCompactUtil::decodeMidiToShorts(const std::string& data) {
    return s_decodeMidi<uint16_t, 0xfe00U>(data);
}

std::string CalCompactUtil::encodeLatin1AsUtf8(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (auto it = s.begin(); it != s.end(); ++it) {
        uint8_t ch = static_cast<uint8_t>(*it);
        if (ch <= 0x7f) {
            result += static_cast<char>(ch);
        } else if (ch <= 0xbf) {
            result += '\xc2';
            result += static_cast<char>(ch);
        } else {
            result += '\xc3';
            result += static_cast<char>(ch & 0xbf);
        }
    }
    return result;
}
