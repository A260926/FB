#pragma once

#include "cal3d/global.h"

// Make old version of clang happy.
#if defined(__clang__) && __clang_major__ <= 3 && __clang_minor__ <= 1
  #include <stdint.h>
#else
  #include <cstdint>
#endif

#include <string>
#include <vector>

class CAL3D_API CalQuantizedData {
public:
    float minimum;
    float maximum;
    uint32_t maxQuantizedValue;
    std::vector<uint32_t> values;
};

class CAL3D_API CalCompactUtil {
public:
    static uint32_t zigzag(int32_t);
    static int32_t unzigzag(uint32_t);

    static std::vector<uint32_t> encodeDeltaZigzag(const std::vector<uint32_t>&);
    static std::vector<uint32_t> decodeDeltaZigzag(const std::vector<uint32_t>&);
    static std::vector<uint16_t> encodeHighWaterMark(const std::vector<uint16_t>&);
    static std::vector<uint16_t> decodeHighWaterMark(const std::vector<uint16_t>&);

    // Theoretically, a robust decoder could handle an arbitrary number of
    // quantized bits, but if we are quantizing for compression, it makes sense
    // to limit ourselves to some upper bound.  We default to 31 max bits to
    // leave room for delta encoding, which potentially doubles the value
    // range.
    static CalQuantizedData quantizeToPrecision(const std::vector<float>&, float epsilon, unsigned int maxBits=31);
    static CalQuantizedData quantizeToBits(const std::vector<float>&, unsigned int bitCount);
    static CalQuantizedData quantizeConsistent(const std::vector<float>&, float epsilon, unsigned int maxBits=31);
    static std::vector<float> unquantize(const CalQuantizedData&);

    static std::vector<uint32_t> decodeMidi(const std::string&);
    static std::vector<uint16_t> decodeMidiToShorts(const std::string&);

    template<typename T>
    static std::string encodeMidi(const std::vector<T>& data) {
        std::string result;
        uint8_t septets[5];
        for (auto it = data.begin(); it != data.end(); ++it) {
            T value = *it;
            unsigned int septetCount = 0;
            do {
                assert(septetCount < 5);
                septets[septetCount++] = value & 0x7f;
                value >>= 7;
            } while (value);

            while (septetCount > 1) {
                --septetCount;
                result += static_cast<char>(septets[septetCount] | 0x80);
            }
            result += static_cast<char>(septets[0]);
        }
        return result;
    }

    static std::string encodeLatin1AsUtf8(const std::string& s);
};
