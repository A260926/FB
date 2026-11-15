#include "TestPrologue.h"
#include <cal3d/compactutil.h>

#include <cmath>
#include <cstring>
#include <limits>

static float s_getPreviousFloat(float value) {
    // Floats are ordered, so we can get the previous representable value by
    // decrementing its integer bit pattern (except for a few special values
    // like zero, inf, and NaN).
    assert(sizeof(float) == sizeof(int32_t));

    // Apparently memcpy() is one of the few ways to reinterpret floats/ints in
    // C that doesn't involve undefined behavior.
    int32_t valueAsInt32;
    memcpy(&valueAsInt32, &value, sizeof(int32_t));
    int32_t prevValueAsInt32 = valueAsInt32 - 1;
    float prevValueAsFloat;
    memcpy(&prevValueAsFloat, &prevValueAsInt32, sizeof(int32_t));
    return prevValueAsFloat;
}

FIXTURE(CompactUtilFixture) {
    static std::vector<uint16_t> makeVectorWithAllPossibleIndices() {
        size_t numIndices = static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1;
        std::vector<uint16_t> allIndices;
        allIndices.reserve(numIndices);
        for (size_t i = 0; i < numIndices; ++i) {
            allIndices.push_back(static_cast<uint16_t>(i));
        }
        return allIndices;
    }
};

TEST_F(CompactUtilFixture, can_zigzag_encode) {
    CHECK_EQUAL(0U, CalCompactUtil::zigzag( 0));
    CHECK_EQUAL(1U, CalCompactUtil::zigzag(-1));
    CHECK_EQUAL(2U, CalCompactUtil::zigzag( 1));
    CHECK_EQUAL(3U, CalCompactUtil::zigzag(-2));
    CHECK_EQUAL(4U, CalCompactUtil::zigzag( 2));
    CHECK_EQUAL(5U, CalCompactUtil::zigzag(-3));
    CHECK_EQUAL(6U, CalCompactUtil::zigzag( 3));
    CHECK_EQUAL(std::numeric_limits<uint32_t>::max() - 1, CalCompactUtil::zigzag(std::numeric_limits<int32_t>::max()));
    CHECK_EQUAL(std::numeric_limits<uint32_t>::max(), CalCompactUtil::zigzag(std::numeric_limits<int32_t>::min()));
}

TEST_F(CompactUtilFixture, can_zigzag_decode) {
    CHECK_EQUAL( 0, CalCompactUtil::unzigzag(0U));
    CHECK_EQUAL(-1, CalCompactUtil::unzigzag(1U));
    CHECK_EQUAL( 1, CalCompactUtil::unzigzag(2U));
    CHECK_EQUAL(-2, CalCompactUtil::unzigzag(3U));
    CHECK_EQUAL( 2, CalCompactUtil::unzigzag(4U));
    CHECK_EQUAL(-3, CalCompactUtil::unzigzag(5U));
    CHECK_EQUAL( 3, CalCompactUtil::unzigzag(6U));
    CHECK_EQUAL(std::numeric_limits<int32_t>::max(), CalCompactUtil::unzigzag(std::numeric_limits<uint32_t>::max() - 1));
    CHECK_EQUAL(std::numeric_limits<int32_t>::min(), CalCompactUtil::unzigzag(std::numeric_limits<uint32_t>::max()));
}

TEST_F(CompactUtilFixture, can_encode_delta_zigzag) {
    uint32_t dataArr[] = {3, 2, 1, 3};
    auto data = arrayToVector(dataArr);

    uint32_t expectedArr[] = {6, 1, 1, 4};
    auto expected = arrayToVector(expectedArr);

    CHECK_EQUAL(expected, CalCompactUtil::encodeDeltaZigzag(data));
}

TEST_F(CompactUtilFixture, can_decode_delta_zigzag) {
    uint32_t dataArr[] = {6, 1, 1, 4};
    auto data = arrayToVector(dataArr);

    uint32_t expectedArr[] = {3, 2, 1, 3};
    auto expected = arrayToVector(expectedArr);

    CHECK_EQUAL(expected, CalCompactUtil::decodeDeltaZigzag(data));
}

TEST_F(CompactUtilFixture, can_encode_high_water_mark) {
    uint16_t dataArr[] = {0, 1, 2, 1, 0, 2, 3, 4, 2};
    auto data = arrayToVector(dataArr);

    uint16_t expectedArr[] = {0, 0, 0, 2, 3, 1, 0, 0, 3};
    auto expected = arrayToVector(expectedArr);

    CHECK_EQUAL(expected, CalCompactUtil::encodeHighWaterMark(data));
}

TEST_F(CompactUtilFixture, can_encode_high_water_mark_with_all_indices) {
    std::vector<uint16_t> data = makeVectorWithAllPossibleIndices();

    // Need one additional non-zero element to ensure high watermark check is
    // exercised.
    data.push_back(1);

    auto encoded = CalCompactUtil::encodeHighWaterMark(data);
    auto decoded = CalCompactUtil::decodeHighWaterMark(encoded);

    CHECK_EQUAL(data, decoded);
}

TEST_F(CompactUtilFixture, can_decode_high_water_mark) {
    uint16_t dataArr[] = {0, 0, 0, 2, 3, 1, 0, 0, 3};
    auto data = arrayToVector(dataArr);

    uint16_t expectedArr[] = {0, 1, 2, 1, 0, 2, 3, 4, 2};
    auto expected = arrayToVector(expectedArr);

    CHECK_EQUAL(expected, CalCompactUtil::decodeHighWaterMark(data));
}

TEST_F(CompactUtilFixture, throws_exception_for_out_of_range_high_water_mark_value) {
    uint16_t dataArr[] = {1};
    auto data = arrayToVector(dataArr);
    CHECK_THROW(
        CalCompactUtil::encodeHighWaterMark(data),
        std::runtime_error
    );
}

TEST_F(CompactUtilFixture, can_quantize) {
    float dataArr[] = {1.234f, -2.345f, 3.456f};
    auto data = arrayToVector(dataArr);
    auto quantizedResult = CalCompactUtil::quantizeToPrecision(data, 0.1f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_EQUAL(data.size(), unquantizedResult.size());
    CHECK_ARRAY_CLOSE(data, unquantizedResult, data.size(), 0.1f);
}

TEST_F(CompactUtilFixture, can_quantize_to_bits) {
    float dataArr[] = {0.0f, 1.0f, 2.0f, 3.0f};
    auto data = arrayToVector(dataArr);

    auto oneBitQuantizedResult = CalCompactUtil::quantizeToBits(data, 1);
    CHECK_EQUAL(1U, oneBitQuantizedResult.maxQuantizedValue);

    auto oneBitUnquantizedResult = CalCompactUtil::unquantize(oneBitQuantizedResult);
    CHECK_EQUAL(data.size(), oneBitUnquantizedResult.size());
    CHECK_ARRAY_CLOSE(data, oneBitUnquantizedResult, data.size(), 2.0f);

    auto twoBitQuantizedResult = CalCompactUtil::quantizeToBits(data, 2);
    CHECK_EQUAL(3U, twoBitQuantizedResult.maxQuantizedValue);

    auto twoBitUnquantizedResult = CalCompactUtil::unquantize(twoBitQuantizedResult);
    CHECK_EQUAL(data.size(), twoBitUnquantizedResult.size());
    CHECK_ARRAY_CLOSE(data, twoBitUnquantizedResult, data.size(), 1.0f);
}

TEST_F(CompactUtilFixture, can_quantize_to_32_bits) {
    float dataArr[] = {0.0f, 1.0f};
    auto data = arrayToVector(dataArr);
    auto quantizedResult = CalCompactUtil::quantizeToBits(data, 32);

    CHECK_EQUAL(std::numeric_limits<uint32_t>::max(), quantizedResult.maxQuantizedValue);
}

TEST_F(CompactUtilFixture, test_quantize_and_unquantize_can_handle_empty_series) {
    std::vector<float> data;
    auto quantizedResult = CalCompactUtil::quantizeToPrecision(data, 0.1f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_EQUAL(data, unquantizedResult);
}

TEST_F(CompactUtilFixture, test_quantize_and_unquantize_can_handle_constant_series) {
    float dataArr[] = {10.0f, 10.0f, 10.0f};
    auto data = arrayToVector(dataArr);
    auto quantizedResult = CalCompactUtil::quantizeToPrecision(data, 0.1f);
    CHECK_EQUAL(0U, quantizedResult.maxQuantizedValue);

    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_ARRAY_CLOSE(data, unquantizedResult, data.size(), 0.1f);
}

TEST_F(CompactUtilFixture, test_quantize_throws_error_for_too_many_bits) {
    float dataArr[] = {0.0f, 1.0f, 2.0f, 3.0f};
    auto data = arrayToVector(dataArr);

    auto quantizedResult = CalCompactUtil::quantizeToPrecision(data, 1.0f, 2);
    CHECK_EQUAL(3U, quantizedResult.maxQuantizedValue);

    CHECK_THROW(
        CalCompactUtil::quantizeToPrecision(data, 1.0f, 1),
        std::runtime_error
    );
}

TEST_F(CompactUtilFixture, test_can_quantize_consistent) {
    float dataArr[] = {1.234f, -2.345f, 3.456f};
    auto data = arrayToVector(dataArr);
    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 0.5f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_ARRAY_CLOSE(data, unquantizedResult, data.size(), 0.5f);
}

TEST_F(CompactUtilFixture, test_consistent_quantization_rounds_identical_values_to_the_same_value) {
    float data1Arr[] = {1.1f, 0.0f, 9.0f};
    auto data1 = arrayToVector(data1Arr);
    auto unquantizedResult1 = CalCompactUtil::unquantize(CalCompactUtil::quantizeConsistent(data1, 1.0f));

    float data2Arr[] = {1.1f, 1.0f, 2.0f};
    auto data2 = arrayToVector(data2Arr);
    auto unquantizedResult2 = CalCompactUtil::unquantize(CalCompactUtil::quantizeConsistent(data2, 1.0f));

    CHECK_EQUAL(unquantizedResult1[0], unquantizedResult2[0]);
}

TEST_F(CompactUtilFixture, test_quantize_consistent_rounds_negative_values_like_positive_ones) {
    float dataArr[] = {-0.6f, 0.6f};
    auto data = arrayToVector(dataArr);

    float expectedArr[] = {-1.0f, 1.0f};
    auto expected = arrayToVector(expectedArr);

    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 1.0f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_ARRAY_CLOSE(expected, unquantizedResult, expected.size(), 0.001);
}

TEST_F(CompactUtilFixture, test_can_quantize_consistent_values_that_fit_in_int) {
    // floats have a 24-bit mantissa, so they can't represent maxint exactly.
    // Casting from int to float at maxint rounds up, at least on MSVC.
    float maxintAsFloat = static_cast<float>(std::numeric_limits<uint32_t>::max());
    float floatBeforeMaxint = s_getPreviousFloat(maxintAsFloat);
    float floatErrorAtMaxint = std::ceil(maxintAsFloat) - std::floor(floatBeforeMaxint);

    std::vector<float> data;
    data.push_back(0);
    data.push_back(floatBeforeMaxint);

    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 1.0f, 32);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    // Turns out that unquantizing happens to be exact in this case, but we'd
    // tolerate at least the gap between floats.
    CHECK_CLOSE(data[1], unquantizedResult[1], std::max(1.0f, floatErrorAtMaxint));
}

TEST_F(CompactUtilFixture, test_throws_error_when_quantizing_consistent_values_that_dont_fit_in_int) {
    float maxintAsFloat = static_cast<float>(std::numeric_limits<int32_t>::max());
    float minintAsFloat = static_cast<float>(std::numeric_limits<int32_t>::min());

    std::vector<float> data;
    data.push_back(0);
    data.push_back(maxintAsFloat);

    // IMVU - changed behavior
    // instead of throwing an exception if data range is too large for the bit size,
    // we'll instead clamp the values to the maximum allowable values that can still be quantized.

    // CHECK_THROW(
    //     CalCompactUtil::quantizeConsistent(data, 1.0f, 32),
    //     std::runtime_error
    // );

    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 1.0f, 32);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);

    CHECK_CLOSE(unquantizedResult[0], 0, .001);
    CHECK_CLOSE(unquantizedResult[1], maxintAsFloat, .001);

    data.clear();
    data.push_back(0);
    data.push_back(maxintAsFloat * 2);

    quantizedResult = CalCompactUtil::quantizeConsistent(data, 1.0f, 32);
    unquantizedResult = CalCompactUtil::unquantize(quantizedResult);

    CHECK_CLOSE(unquantizedResult[0], 0, .001);
    CHECK_CLOSE(unquantizedResult[1], maxintAsFloat / 2.0, .001);

    data.clear();
    data.push_back(minintAsFloat);
    data.push_back(maxintAsFloat);

    quantizedResult = CalCompactUtil::quantizeConsistent(data, 1.0f, 32);
    unquantizedResult = CalCompactUtil::unquantize(quantizedResult);

    CHECK_CLOSE(unquantizedResult[0], minintAsFloat / 2.0, .001);
    CHECK_CLOSE(unquantizedResult[1], maxintAsFloat / 2.0, .001);
}

TEST_F(CompactUtilFixture, test_quantize_consistent_can_handle_empty_series) {
    std::vector<float> data;
    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 0.1f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_EQUAL(data, unquantizedResult);
}

TEST_F(CompactUtilFixture, test_can_quantize_consistent_for_constant_series) {
    float dataArr[] = {10.0f, 10.0f, 10.0f};
    auto data = arrayToVector(dataArr);

    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 0.1f);
    CHECK_EQUAL(0U, quantizedResult.maxQuantizedValue);

    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_ARRAY_CLOSE(data, unquantizedResult, data.size(), 0.1f);
}

TEST_F(CompactUtilFixture, can_quantize_tight_ranges_consistently) {
    // Range of z-coordinate values seen in submesh 0 of PID 17597888 ("YSL Polo")
    float dataArr[] = { 1.8691740036010742, 1.8691861629486084 };
    auto data = arrayToVector(dataArr);

    auto quantizedResult = CalCompactUtil::quantizeConsistent(data, 0.000040f);
    auto unquantizedResult = CalCompactUtil::unquantize(quantizedResult);
    CHECK_ARRAY_CLOSE(data, unquantizedResult, data.size(), 0.000020f);
}

TEST_F(CompactUtilFixture, test_can_midi_encode_and_decode) {
    uint32_t dataArr[] = {0, 1, 0x7f, 0x80};
    auto data = arrayToVector(dataArr);

    auto encodedResult = CalCompactUtil::encodeMidi(data);
    auto decodedResult = CalCompactUtil::decodeMidi(encodedResult);

    CHECK_EQUAL(data, decodedResult);
}

TEST_F(CompactUtilFixture, test_can_midi_encode_and_decode_empty_list) {
    std::vector<uint32_t> data;
    auto encodedResult = CalCompactUtil::encodeMidi(data);
    auto decodedResult = CalCompactUtil::decodeMidi(encodedResult);

    CHECK_EQUAL(0U, decodedResult.size());
}

TEST_F(CompactUtilFixture, test_throws_exception_when_midi_decoding_truncated_data) {
    uint8_t dataArr[] = {0x81, 0, 0x81};
    std::string data;
    for (auto it = arrayBegin(dataArr); it != arrayEnd(dataArr); ++it) {
        data += static_cast<char>(*it);
    }

    CHECK_THROW(
        CalCompactUtil::decodeMidi(data),
        std::runtime_error
    );
}

TEST_F(CompactUtilFixture, test_can_encode_maxint) {
    uint8_t dataArr[] = {0x8f, 0xff, 0xff, 0xff, 0x7f};
    std::string data;
    for (auto it = arrayBegin(dataArr); it != arrayEnd(dataArr); ++it) {
        data += static_cast<char>(*it);
    }

    auto decodedResult = CalCompactUtil::decodeMidi(data);
    CHECK_EQUAL(std::numeric_limits<uint32_t>::max(), decodedResult[0]);
}

TEST_F(CompactUtilFixture, test_throws_exception_when_midi_decoding_overflow_value) {
    uint8_t dataArr[] = {0xff, 0xff, 0xff, 0xff, 0};
    std::string data;
    for (auto it = arrayBegin(dataArr); it != arrayEnd(dataArr); ++it) {
        data += static_cast<char>(*it);
    }

    CHECK_THROW(
        CalCompactUtil::decodeMidi(data),
        std::runtime_error
    );
}
